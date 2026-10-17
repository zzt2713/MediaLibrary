#include "MediaScanner.h"
#include "ExifReader.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QFileInfoList>
#include <QDateTime>
#include <QDebug>

namespace media {

namespace {

/*! 一批攒够多少条就交给调用方写库。太小会让事务次数暴涨，太大占内存。 */
constexpr int kBatchSize = 64;

/*!
 * 判断某个文件是否位于「应当跳过」的目录内。
 *
 * 为什么需要它：QDirIterator 只能按文件属性过滤，不能按目录名过滤，
 * 所以 skipDirNames 只能在拿到文件路径后自己判断。做法是逐段检查
 * 相对路径的父目录部分（最后一段是文件名，不参与判断）：
 *   - 命中 skipNames（不区分大小写）
 *   - 以 '.' 开头
 * 顺带把深度上限也在这里执行：父目录层数超过 maxDepth 就跳过。
 * maxDepth <= 0 表示不限制。
 */
bool isInSkippedDir(const QString &root, const QString &filePath,
                    const QStringList &skipNames, int maxDepth)
{
    const QString relative = QDir(root).relativeFilePath(filePath);
    const QStringList parts = relative.split(QLatin1Char('/'), Qt::SkipEmptyParts);

    const int depth = parts.size() - 1;   // 最后一段是文件名
    if (depth <= 0)
        return false;                     // 直接位于根目录下，无需判断

    if (maxDepth > 0 && depth > maxDepth)
        return true;

    for (int i = 0; i < depth; ++i) {
        const QString &name = parts.at(i);
        if (name.startsWith(QLatin1Char('.')))
            return true;
        if (skipNames.contains(name, Qt::CaseInsensitive))
            return true;
    }

    return false;
}

} // namespace

MediaScanner::MediaScanner(QObject *parent)
    : QObject(parent)
{
}

bool MediaScanner::isReadable(const QString &path)
{
    QFileInfo info(path);
    if (!info.exists() || !info.isDir())
        return false;

    // exists + isDir 还不足以说明可读：Windows 上 ACL 拒绝时
    // QDir 能构造但 entryList 返回空。这里真读一次，拿到空且不可读就判否。
    QDir dir(path);
    const QFileInfoList entries = dir.entryInfoList(
        QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden);

    if (!entries.isEmpty())
        return true;

    // 空目录 vs 不可读目录：空目录是合法的，不可读才是问题。
    // 用 readable 权限位做二次判断。
    return info.isReadable();
}

QList<QString> MediaScanner::listSubDirectories(const QString &rootPath,
                                                const QStringList &skipNames)
{
    QList<QString> result;

    QDir dir(rootPath);
    if (!dir.exists())
        return result;

    const QFileInfoList entries = dir.entryInfoList(
        QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks,
        QDir::Name | QDir::IgnoreCase);

    for (const QFileInfo &info : entries) {
        // 跳过系统/工具目录，它们不该进资源库
        if (skipNames.contains(info.fileName(), Qt::CaseInsensitive))
            continue;
        if (info.fileName().startsWith(QLatin1Char('.')))
            continue;   // 隐藏目录默认跳过

        result.append(QDir::cleanPath(info.absoluteFilePath()));
    }

    return result;
}

MediaItem MediaScanner::buildItem(const QFileInfo &info) const
{
    MediaItem item;

    item.path = QDir::cleanPath(QDir::fromNativeSeparators(info.absoluteFilePath()));
    item.fileName = info.fileName();
    item.folderPath = QDir::cleanPath(QDir::fromNativeSeparators(info.absolutePath()));
    item.type = typeFromPath(item.path);
    item.size = info.size();
    item.modified = info.lastModified();
    item.mtimeMs = item.modified.toMSecsSinceEpoch();

    // 图片才读 EXIF：视频/音频读它没意义，而且 QImageReader 打不开这些格式
    if (m_options.readExif && item.type == MediaType::Image) {
        const ImageMetadata meta = ExifReader::read(item.path);
        if (meta.width > 0 && meta.height > 0) {
            item.width = meta.width;
            item.height = meta.height;

            if (meta.hasExif) {
                item.cameraMake = meta.cameraMake;
                item.cameraModel = meta.cameraModel;
                if (meta.takenAt.isValid())
                    item.takenAt = meta.takenAt;
            }
        } else {
            // 读不出尺寸：文件损坏或格式不被支持。
            // 仍然收进库并标记尺寸为 0，让用户能在界面上看到"这张图坏了"，
            // 而不是静默丢弃——静默丢弃会让用户以为程序漏扫了。
            item.width = 0;
            item.height = 0;
        }
    }

    if (!item.takenAt.isValid())
        item.takenAt = item.modified;

    return item;
}

int MediaScanner::scanDirectory(const QString &rootPath,
                                const std::function<void(const QList<MediaItem> &)> &onBatch)
{
    if (rootPath.isEmpty())
        return 0;

    const QString root = QDir::cleanPath(QDir::fromNativeSeparators(rootPath));
    QFileInfo rootInfo(root);

    if (!rootInfo.exists() || !rootInfo.isDir()) {
        emit warning(root, QStringLiteral("目录不存在或不是目录"));
        return 0;
    }

    if (!isReadable(root)) {
        emit warning(root, QStringLiteral("目录无访问权限，已跳过"));
        return 0;
    }

    // ---------- 阶段一：预统计总数 ----------
    // 目的是让进度条有真实分母。多走一次遍历，换来的是进度可预期。
    int total = -1;
    if (m_options.countFirst) {
        total = 0;
        const QDir::Filters fileFilters = QDir::Files | QDir::NoSymLinks;
        const auto flags = m_options.recursive
                               ? (QDirIterator::Subdirectories)
                               : (QDirIterator::NoIteratorFlags);

        QDirIterator counter(root, fileFilters, flags);
        while (counter.hasNext()) {
            if (isCancelled())
                break;
            const QString candidate = counter.next();
            if (typeFromPath(candidate) == MediaType::Unknown)
                continue;
            // 与主扫描用同一套过滤条件，否则进度条的分母会比实际处理的多
            if (isInSkippedDir(root, candidate, m_options.skipDirNames,
                               m_options.maxDepth))
                continue;
            ++total;
        }

        // 预统计期间如果被取消，就不再进入主扫描
        if (isCancelled())
            return 0;
    }

    // ---------- 阶段二：主扫描 ----------
    const QDir::Filters filters = QDir::Files | QDir::NoSymLinks;
    const auto flags = m_options.recursive
                           ? (QDirIterator::Subdirectories)
                           : (QDirIterator::NoIteratorFlags);

    QDirIterator it(root, filters, flags);

    QList<MediaItem> batch;
    batch.reserve(kBatchSize);

    int discovered = 0;
    QString lastFolder;
    int foldersSeen = 0;

    /*!
     * 注意命名：这里的局部变量叫 progressInfo 而不是 progress。
     *
     * 因为本类有一个同名信号 void progress(const ScanProgress &)，
     * 若局部变量也叫 progress，`emit progress(progressInfo)` 里的
     * progress 会被局部变量遮蔽（shadow），编译器认为在"调用一个
     * ScanProgress 对象"，报 C2064 "term does not evaluate to a function"。
     * 这类遮蔽错误在阅读代码时极难发现，改掉变量名最省事。
     */
    ScanProgress progressInfo;
    progressInfo.total = total;

    while (it.hasNext()) {
        // 取消检查放在循环顶部：即使目录里有十万个文件，
        // 也能在一个文件的处理时间内响应用户的取消。
        if (isCancelled())
            break;

        const QString filePath = it.next();
        const MediaType type = typeFromPath(filePath);
        if (type == MediaType::Unknown)
            continue;   // 非媒体文件直接跳过，连 QFileInfo 都不构造，省一次 stat

        // 跳过名单与深度上限。QDirIterator 无法按目录名过滤，只能在这里排除，
        // 否则 .git / node_modules 里的文件会被全量收进资源库。
        if (isInSkippedDir(root, filePath, m_options.skipDirNames, m_options.maxDepth))
            continue;

        QFileInfo info(filePath);
        if (!info.exists() || !info.isFile())
            continue;   // 迭代期间文件被删（竞态），跳过

        MediaItem item = buildItem(info);
        if (!item.isValid())
            continue;

        batch.append(item);
        ++discovered;

        if (batch.size() >= kBatchSize) {
            if (onBatch)
                onBatch(batch);
            batch.clear();

            // 每批上报一次进度。逐文件上报会把事件循环淹掉。
            progressInfo.discovered = discovered;
            progressInfo.currentPath = item.folderPath;
            if (progressInfo.currentPath != lastFolder) {
                lastFolder = progressInfo.currentPath;
                ++foldersSeen;
                progressInfo.foldersDone = foldersSeen;
            }
            emit progress(progressInfo);
        }
    }

    // 收尾残留
    if (!batch.isEmpty() && onBatch)
        onBatch(batch);

    progressInfo.discovered = discovered;
    progressInfo.foldersDone = progressInfo.foldersTotal;
    emit progress(progressInfo);

    return discovered;
}

} // namespace media
