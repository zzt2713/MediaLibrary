#include "ThumbnailCache.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QBuffer>
#include <QDateTime>
#include <QMutexLocker>
#include <QDebug>

namespace media {

namespace {

/*!
 * 估算 QImage 实际占用。
 *
 * 为什么不用 width*height*4：QImage 的每行数据是「字节宽度」对齐的
 * （32 位平台上通常按 4 字节对齐），并且不同格式每像素字节数不同
 * （单色 1bpp、索引 8bpp、RGB32 4bpp 等）。QImage::sizeInBytes() 正是
 * 按 sizeInBytes() 的公式（bytesPerLine * height）算出来的，所以直接用它，
 * 既准确又不需要我们跟着 Qt 的内部对齐规则走。
 */
qint64 imageBytes(const QImage &image)
{
    if (image.isNull())
        return 0;
    // sizeInBytes() 返回 qsizetype，转成 qint64 参与上限比较
    return static_cast<qint64>(image.sizeInBytes());
}

/*! 磁盘缓存统一用 PNG：无损、Qt 内置编解码，不引第三方。 */
const char *kDiskFormat = "PNG";

} // namespace

ThumbnailCache::ThumbnailCache(QObject *parent)
    : QObject(parent)
{
    m_cacheDir = QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation))
                     .filePath(QStringLiteral("thumbnails"));
    QDir().mkpath(m_cacheDir);
    m_stats.cacheDir = m_cacheDir;
}

ThumbnailCache::~ThumbnailCache()
{
    // 这里不再落盘：put() 已经是同步写盘，析构时没有待写数据。
    // 只清内存，QImage 自身的析构会释放像素缓冲区。
    clearMemory();
}

void ThumbnailCache::setCacheDirectory(const QString &dir)
{
    if (dir.isEmpty())
        return;

    QMutexLocker locker(&m_mutex);
    m_cacheDir = dir;
    m_stats.cacheDir = dir;
    locker.unlock();

    QDir().mkpath(dir);
}

void ThumbnailCache::setMemoryLimit(qint64 bytes)
{
    if (bytes <= 0)
        return;

    {
        QMutexLocker locker(&m_mutex);
        m_memoryLimit = bytes;
    }
    evictMemoryIfNeeded();
    emit statsChanged();
}

void ThumbnailCache::setDiskLimit(qint64 bytes)
{
    if (bytes <= 0)
        return;

    {
        QMutexLocker locker(&m_mutex);
        m_diskLimit = bytes;
    }

    // 与 setMemoryLimit 的语义对齐：改了上限就立刻按新上限回收一次，
    // 否则用户调小上限后要等到下一次写入才生效。
    enforceDiskLimit();
    emit statsChanged();
}

QString ThumbnailCache::diskPathFor(const ThumbKey &key) const
{
    // 用类型前缀分目录，避免单目录文件数过多拖慢文件系统枚举。
    // 键的 MD5 已含路径+mtime+size+尺寸，天然唯一。
    const QString keyHex = key.toCacheKey();
    const QString sub = keyHex.left(2);

    return QDir(m_cacheDir)
        .filePath(QStringLiteral("%1/%2/%3.png")
                      .arg(mediaTypePrefix(media::typeFromPath(key.path)), sub, keyHex));
}

QImage ThumbnailCache::get(const ThumbKey &key, bool *fromDisk)
{
    if (fromDisk)
        *fromDisk = false;

    const QString cacheKey = key.toCacheKey();

    // 第一级：内存
    {
        QMutexLocker locker(&m_mutex);
        auto it = m_memory.find(cacheKey);
        if (it != m_memory.end() && !it.value().image.isNull()) {
            touchMemory(cacheKey, it.value());
            ++m_stats.memoryHits;
            const QImage image = it.value().image;
            locker.unlock();
            // 命中是常态路径，不每帧发信号，避免淹没有意义的重绘
            return image;
        }
        ++m_stats.misses;
    }

    // 第二级：磁盘。QImage::load 是纯 CPU 侧解码，可以在工作线程里调。
    QImage image;
    const QString path = diskPathFor(key);
    if (QFile::exists(path)) {
        if (image.load(path, kDiskFormat) && !image.isNull()) {
            /*!
             * 标记为最近访问，供 enforceDiskLimit() 区分冷热。
             *
             * setFileTime 是 QFileDevice 的【非静态成员函数】，必须用一个
             * 已打开的 QFile 实例调用 —— 它没有 QFile::setFileTime(path, ...)
             * 这种静态形式（Qt 6.8 的 qfiledevice.h:111 只有成员版本）。
             * 之前的写法按静态函数调用，所以报 C2660。
             *
             * 需要以 ReadWrite 打开：只读句柄上改时间会失败。
             * 打开失败就跳过——缩略图本身已经拿到了，时间戳只是淘汰策略的
             * 参考信息，不该因为它而让整个缓存命中路径失败。
             */
            QFile timeFile(path);
            if (timeFile.open(QIODevice::ReadWrite)) {
                const QDateTime now = QDateTime::currentDateTime();
                timeFile.setFileTime(now, QFileDevice::FileAccessTime);
                timeFile.setFileTime(now, QFileDevice::FileModificationTime);
                timeFile.close();
            }

            // 回填内存，下次直接命中第一级
            QMutexLocker locker(&m_mutex);
            MemEntry entry;
            entry.image = image;
            entry.bytes = imageBytes(image);
            entry.stamp = ++m_accessCounter;
            m_memory.insert(cacheKey, entry);
            m_memoryBytes += entry.bytes;
            ++m_stats.diskHits;
            --m_stats.misses;   // 磁盘命中不算 miss
            locker.unlock();

            if (fromDisk)
                *fromDisk = true;

            evictMemoryIfNeeded();
            return image;
        }

        // 文件损坏（写盘中断、磁盘错误）：删掉它，让上层重新生成
        QFile::remove(path);
    }

    return QImage();
}

bool ThumbnailCache::containsInMemory(const ThumbKey &key) const
{
    QMutexLocker locker(&m_mutex);
    auto it = m_memory.constFind(key.toCacheKey());
    return it != m_memory.constEnd() && !it.value().image.isNull();
}

void ThumbnailCache::put(const ThumbKey &key, const QImage &image)
{
    if (image.isNull() || key.path.isEmpty())
        return;

    const QString cacheKey = key.toCacheKey();
    const qint64 bytes = imageBytes(image);

    // 单张超过内存上限的 1/4 就不进内存了，否则一次就能把缓存挤空
    {
        QMutexLocker locker(&m_mutex);
        if (bytes > m_memoryLimit / 4)
        {
            // 仍然要落盘，只是跳过内存层
        }
        else
        {
            MemEntry entry;
            entry.image = image;
            entry.bytes = bytes;
            entry.stamp = ++m_accessCounter;

            // 同键覆盖时先扣掉旧占用，否则统计会一直虚高
            auto old = m_memory.find(cacheKey);
            if (old != m_memory.end())
                m_memoryBytes -= old.value().bytes;

            m_memory.insert(cacheKey, entry);
            m_memoryBytes += bytes;
        }
    }

    // 写磁盘。放在工作线程调用，不阻塞 UI。
    // QImage::save 走 CPU 编码，不涉及平台绘图后端，线程安全。
    const QString path = diskPathFor(key);
    QDir().mkpath(QFileInfo(path).absolutePath());

    // 先写临时文件再 rename：避免程序中途退出留下半截 PNG 被当成有效缓存
    const QString tmpPath = path + QStringLiteral(".tmp");

    QByteArray encoded;
    QBuffer buffer(&encoded);
    if (buffer.open(QIODevice::WriteOnly)) {
        if (image.save(&buffer, kDiskFormat, 90)) {
            QFile tmp(tmpPath);
            if (tmp.open(QIODevice::WriteOnly)) {
                tmp.write(encoded);
                tmp.close();

                QFile::remove(path);
                if (QFile::rename(tmpPath, path)) {
                    QMutexLocker locker(&m_mutex);
                    ++m_stats.diskWrites;
                } else {
                    QFile::remove(tmpPath);
                }
            }
        }
    }

    evictMemoryIfNeeded();

    /*!
     * 磁盘回收的触发点。
     *
     * enforceDiskLimit() 要遍历整个缓存目录并排序，比写一张图贵得多，
     * 所以不能每写一张就调用一次 —— 那样会把扫描速度拖垮。
     * 折中做法是攒够 kDiskCheckInterval 次写入才检查一次；
     * 检查本身很轻（先累加总大小，没超上限直接返回）。
     */
    bool shouldCheck = false;
    {
        QMutexLocker locker(&m_mutex);
        shouldCheck = (++m_diskWritesSinceCheck >= kDiskCheckInterval);
        if (shouldCheck)
            m_diskWritesSinceCheck = 0;
    }

    if (shouldCheck)
        enforceDiskLimit();
}

void ThumbnailCache::touchMemory(const QString &cacheKey, MemEntry &entry)
{
    // 调用方必须已持有 m_mutex
    Q_UNUSED(cacheKey)
    entry.stamp = ++m_accessCounter;
}

void ThumbnailCache::evictMemoryIfNeeded()
{
    QMutexLocker locker(&m_mutex);

    if (m_memoryBytes <= m_memoryLimit)
        return;

    // 按访问序号排序，淘汰最旧的一批。
    // 只在超限时排序，正常路径下零开销。
    QList<QPair<quint64, QString>> order;
    order.reserve(m_memory.size());
    for (auto it = m_memory.constBegin(); it != m_memory.constEnd(); ++it)
        order.append({ it.value().stamp, it.key() });

    std::sort(order.begin(), order.end(),
              [](const QPair<quint64, QString> &a, const QPair<quint64, QString> &b) {
                  return a.first < b.first;
              });

    // 淘汰到目标的 80%，避免刚好卡在上限反复触发排序
    const qint64 target = static_cast<qint64>(m_memoryLimit * 0.8);
    for (const auto &pair : order) {
        if (m_memoryBytes <= target)
            break;

        auto it = m_memory.find(pair.second);
        if (it == m_memory.end())
            continue;

        m_memoryBytes -= it.value().bytes;
        m_memory.remove(pair.second);
    }
}

void ThumbnailCache::clearMemory()
{
    {
        QMutexLocker locker(&m_mutex);
        m_memory.clear();
        m_memoryBytes = 0;
        m_accessCounter = 0;
    }
    emit statsChanged();
}

void ThumbnailCache::clearDisk()
{
    QString dir;
    {
        QMutexLocker locker(&m_mutex);
        dir = m_cacheDir;
    }

    if (dir.isEmpty())
        return;

    // 只删我们自己生成的 .png / .tmp，不动目录下其他文件
    QDir root(dir);
    const QStringList subDirs = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &sub : subDirs) {
        QDir subDir(root.filePath(sub));
        const QStringList files = subDir.entryList(
            QStringList() << QStringLiteral("*.png") << QStringLiteral("*.tmp"),
            QDir::Files);
        for (const QString &f : files)
            QFile::remove(subDir.filePath(f));

        // 只删我们自己建的空子目录
        subDir.rmdir(subDir.absolutePath());
    }
    root.rmdir(dir);

    QDir().mkpath(dir);

    {
        QMutexLocker locker(&m_mutex);
        m_stats.diskWrites = 0;
    }
    emit statsChanged();
}

void ThumbnailCache::clearAll()
{
    clearMemory();
    clearDisk();
}

qint64 ThumbnailCache::memoryUsage() const
{
    QMutexLocker locker(&m_mutex);
    return m_memoryBytes;
}

ThumbnailCache::Stats ThumbnailCache::stats() const
{
    QMutexLocker locker(&m_mutex);
    Stats s = m_stats;
    s.memoryBytes = m_memoryBytes;
    s.memoryEntries = m_memory.size();
    return s;
}

void ThumbnailCache::resetStats()
{
    {
        QMutexLocker locker(&m_mutex);
        const QString dir = m_stats.cacheDir;
        m_stats = Stats();
        m_stats.cacheDir = dir;
    }
    emit statsChanged();
}

void ThumbnailCache::enforceDiskLimit()
{
    QString dir;
    qint64 limit = 0;
    {
        QMutexLocker locker(&m_mutex);
        dir = m_cacheDir;
        limit = m_diskLimit;
    }

    if (dir.isEmpty() || limit <= 0)
        return;

    QDir root(dir);
    if (!root.exists())
        return;

    // 收集所有缓存文件及最后访问时间
    struct Entry { QString path; qint64 size; QDateTime access; };
    QList<Entry> entries;
    qint64 total = 0;

    const QStringList subs = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &sub : subs) {
        QDir subDir(root.filePath(sub));
        const QFileInfoList files = subDir.entryInfoList(
            QStringList() << QStringLiteral("*.png"), QDir::Files);
        for (const QFileInfo &fi : files) {
            Entry e;
            e.path = fi.absoluteFilePath();
            e.size = fi.size();
            e.access = fi.lastRead();
            entries.append(e);
            total += e.size;
        }
    }

    if (total <= limit)
        return;

    // 最久未访问的排前面，从头部开始删
    std::sort(entries.begin(), entries.end(),
              [](const Entry &a, const Entry &b) { return a.access < b.access; });

    const qint64 target = static_cast<qint64>(limit * 0.9);
    for (const Entry &e : entries) {
        if (total <= target)
            break;
        if (QFile::remove(e.path))
            total -= e.size;
    }
}

void ThumbnailCache::invalidate(const QString &path)
{
    /*!
     * 缓存键是「路径 + mtime + size + 尺寸」的 MD5，是不可逆摘要，
     * 所以无法由路径反推出要删哪些缓存文件。
     * 这里不做主动删除，理由：
     *  - 文件被删除后不会再被请求，其缓存自然变成冷数据；
     *  - 文件被修改后 mtime 变化，旧键再也命中不了，等价于已失效；
     *  - 兜底由 enforceDiskLimit() 按最后访问时间回收。
     * 只有清空全部缓存（clearDisk）才是显式删除，语义清晰。
     */
    Q_UNUSED(path)
    emit statsChanged();
}

} // namespace media
