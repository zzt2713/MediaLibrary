#ifndef THUMBNAILCACHE_H
#define THUMBNAILCACHE_H

#include "MediaTypes.h"

#include <QObject>
#include <QImage>
#include <QHash>
#include <QList>
#include <QMutex>
#include <QString>

namespace media {

/*!
 * \brief 两级缩略图缓存：内存 LRU + 磁盘文件缓存。
 *
 * 设计要点：
 *  - 键里带 mtime 与 size，文件一改键就变，旧缓存自然查不到。
 *    这比"监听变更再删缓存"可靠：不会漏删，也不需要跨进程协调。
 *  - 内存上限按字节数而非条数控制。同样 1000 条，缩略图大小可能差十倍，
 *    按条数会导致大缩略图时内存爆掉。
 *  - 磁盘缓存文件名 = 键的 MD5，因此不会有文件名非法字符问题，
 *    也不会因为路径太长超出文件系统限制。
 *  - 缓存里存的是 QImage 而不是 QPixmap。QPixmap 绑定平台后端存储，
 *    只允许在 GUI 线程构造与使用；而这个缓存的读写主要发生在工作线程
 *    （ThumbnailTask 里），用 QPixmap 就是未定义行为，Windows 上会直接崩。
 *    QImage 是纯 CPU 侧的数据容器，跨线程读写安全，天然适合做线程边界的载体。
 */
class ThumbnailCache : public QObject
{
    Q_OBJECT

public:
    explicit ThumbnailCache(QObject *parent = nullptr);
    ~ThumbnailCache() override;

    /*!
     * \brief 设置磁盘缓存目录。为空则用 QStandardPaths::CacheLocation/thumbnails。
     */
    void setCacheDirectory(const QString &dir);
    QString cacheDirectory() const { return m_cacheDir; }

    /*! 内存缓存上限（字节）。默认 128MB。 */
    void setMemoryLimit(qint64 bytes);
    qint64 memoryLimit() const { return m_memoryLimit; }

    /*! 磁盘缓存上限（字节）。默认 1GB，超出后按最后访问时间淘汰。 */
    void setDiskLimit(qint64 bytes);
    qint64 diskLimit() const { return m_diskLimit; }

    // ---------- 查询 ----------

    /*!
     * \brief 取缩略图。先查内存，再查磁盘。
     * \param fromDisk 输出参数，指示结果是否来自磁盘（用于统计命中率）
     * \return 未命中时返回空 QImage
     */
    QImage get(const ThumbKey &key, bool *fromDisk = nullptr);

    /*! 只探内存，不做磁盘 IO。供绘制路径调用，避免滚动时卡在磁盘读上。 */
    bool containsInMemory(const ThumbKey &key) const;

    // ---------- 写入 ----------

    /*!
     * \brief 存入两级缓存。
     *
     * 磁盘写入是同步的，但只在工作线程调用（缩略图任务里），
     * 主线程不调这个方法，所以不会阻塞 UI。
     */
    void put(const ThumbKey &key, const QImage &image);

    // ---------- 管理 ----------

    /*! 清空内存缓存。 */
    void clearMemory();
    /*! 清空磁盘缓存（用户显式操作）。 */
    void clearDisk();
    /*! 两级一起清。 */
    void clearAll();

    /*! 当前内存占用字节数。 */
    qint64 memoryUsage() const;

    /*! 统计信息，供状态栏与性能面板展示。 */
    struct Stats
    {
        int     memoryHits = 0;
        int     diskHits = 0;
        int     misses = 0;
        int     diskWrites = 0;
        qint64  memoryBytes = 0;
        int     memoryEntries = 0;
        QString cacheDir;
    };
    Stats stats() const;
    void resetStats();

    /*!
     * \brief 清理磁盘缓存中超限的旧文件。
     *
     * 按最后访问时间倒序保留到 limit 以内。会在后台线程调用，
     * 不要在 UI 线程直接调（首次可能遍历上万文件）。
     */
    void enforceDiskLimit();

    /*! 删除某条资源对应的所有尺寸缓存，文件被删除时调用。 */
    void invalidate(const QString &path);

signals:
    void statsChanged();

private:
    struct MemEntry
    {
        QImage  image;
        qint64  bytes = 0;
        quint64 stamp = 0;   // 访问序号，越大越新
    };

    QString diskPathFor(const ThumbKey &key) const;
    void touchMemory(const QString &cacheKey, MemEntry &entry);
    void evictMemoryIfNeeded();

    /*! 每写多少次磁盘缓存才做一次上限检查，避免每张图都遍历缓存目录。 */
    static constexpr int kDiskCheckInterval = 32;

    QString m_cacheDir;
    qint64  m_memoryLimit = 128LL * 1024 * 1024;
    qint64  m_diskLimit = 1024LL * 1024 * 1024;
    int     m_diskWritesSinceCheck = 0;

    mutable QMutex m_mutex;
    QHash<QString, MemEntry> m_memory;
    qint64  m_memoryBytes = 0;
    quint64 m_accessCounter = 0;

    mutable Stats m_stats;
};

} // namespace media

#endif // THUMBNAILCACHE_H
