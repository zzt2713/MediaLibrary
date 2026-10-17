#ifndef MEDIADATABASE_H
#define MEDIADATABASE_H

#include "MediaTypes.h"

#include <QObject>
#include <QSqlDatabase>
#include <QString>
#include <QList>
#include <QHash>
#include <QMutex>

namespace media {

/*!
 * \brief 资源库的数据访问层，封装 SQLite。
 *
 * 线程模型（这是本项目最需要讲清楚的一点）：
 * QSqlDatabase 不能跨线程共享，所以这里为每个线程创建独立连接，
 * 连接名 = 基础连接名 + 线程指针地址。所有公开方法都通过 connectionForCurrentThread()
 * 取当前线程的连接，因此可以被主线程和工作线程同时调用而不冲突。
 * 关闭连接时收集本进程创建过的所有连接名统一 close + removeDatabase。
 *
 * 写入策略：批量写入走事务。扫描一万张图若逐条 commit，
 * SQLite 会因为每次 fsync 慢上两个数量级。
 */
class MediaDatabase : public QObject
{
    Q_OBJECT

public:
    explicit MediaDatabase(QObject *parent = nullptr);
    ~MediaDatabase() override;

    /*!
     * \brief 打开数据库文件并建表。
     * \param dbPath 数据库文件路径；留空则用 QStandardPaths 的 AppDataLocation 下的 library.db
     */
    bool open(const QString &dbPath = QString());
    void close();
    bool isOpen() const;
    QString databasePath() const { return m_dbPath; }
    QString lastError() const { return m_lastError; }

    // ---------- 目录 ----------

    /*! 登记一个被索引的根目录。已存在则更新 lastScan 并返回其 id。 */
    qint64 addFolder(const QString &path, const QString &displayName = QString());
    void removeFolder(const QString &path);
    QList<QString> allFolderPaths() const;
    bool folderExists(const QString &path) const;

    // ---------- 资源写入 ----------

    /*!
     * \brief 增量同步的核心：批量写入扫描结果。
     *
     * 对每个条目按 path 查已有记录：
     *  - 不存在：插入
     *  - 存在但 mtime/size 变了：更新元数据，并清空 hash 字段（内容可能已变）
     *  - 存在且未变：跳过，保留 favorite / tags 等用户数据
     * 全过程在一个事务里完成。
     * \return 实际发生插入或更新的条数
     */
    int upsertItems(const QList<MediaItem> &items, qint64 folderId = 0);

    /*! 更新单个资源的用户态字段（收藏、标签等）。 */
    bool updateItem(const MediaItem &item);
    bool setFavorite(qint64 itemId, bool favorite);

    /*!
     * \brief 标记失效记录。
     *
     * 扫描完某个目录后，把该目录下不在 seenPaths 里的记录标 missing=true，
     * 而不是直接删除——用户可能只是临时把移动硬盘拔了，物理删除会连带丢掉收藏信息。
     * \return 被标记的条数
     */
    int markMissing(const QString &folderPath, const QStringList &seenPaths);

    /*! 清理确实不存在的记录（用户显式触发的"清理失效记录"）。 */
    int purgeMissing();

    /*! 文件被删/改名后按路径定位并标记。 */
    bool markMissingByPath(const QString &path);

    /*! 文件移动/重命名：把旧路径的记录迁到新路径，保留收藏与标签。 */
    bool relocatePath(const QString &oldPath, const QString &newPath);

    /*!
     * \brief 编辑索引里的显示名（F2 / 右键重命名）。
     *
     * 与 relocatePath 的区别只在失败语义：这里明确区分
     * 「新路径已有记录」和「其他 SQL 错误」，让调用方能给出准确提示。
     * favorite / tags / play_history 都不受影响（它们挂在 id 上）。
     */
    bool renameItem(const QString &oldPath, const QString &newPath);

    // ---------- 资源查询 ----------

    MediaItem itemById(qint64 id) const;
    MediaItem itemByPath(const QString &path) const;

    /*!
     * \brief 分页查询，供大型资源库的懒加载使用。
     * \param folderPath 空串表示不限目录
     * \param type       Unknown 表示不限类型
     * \param onlyFavorites 仅收藏
     * \param offset/limit  分页参数；limit <= 0 表示不分页
     * \param searchText 文件名子串匹配（空串不过滤），通配符会被转义
     * \param includeMissing true 表示只查失效记录（missing = 1），
     *                       false 表示只查有效记录。两者互斥，
     *                       与 countItems 必须传同一个值。
     */
    QList<MediaItem> queryItems(const QString &folderPath = QString(),
                                MediaType type = MediaType::Unknown,
                                bool onlyFavorites = false,
                                int offset = 0,
                                int limit = -1,
                                const QString &orderBy = QStringLiteral("mtime DESC"),
                                const QString &searchText = QString(),
                                bool includeMissing = false) const;

    /*! 与 queryItems 条件一一对应，用于分页总数。 */
    int countItems(const QString &folderPath = QString(),
                   MediaType type = MediaType::Unknown,
                   bool onlyFavorites = false,
                   const QString &searchText = QString(),
                   bool includeMissing = false) const;

    /*! 每个目录下的资源数，用于左侧树显示计数徽标。一次查询搞定，避免 N+1。 */
    QHash<QString, int> folderCounts() const;

    /*! 各类型总数，用于侧边栏"图片/视频/音频"分组计数。 */
    QHash<int, int> typeCounts() const;

    /*! 全部已索引资源的轻量快照（不含 hash），供重复检测与统计使用。 */
    QList<MediaItem> allItems(bool includeMissing = false) const;

    /*! 还没有内容哈希的记录，供后台补算。 */
    QList<MediaItem> itemsWithoutHash(int limit = 200) const;
    bool setContentHash(qint64 itemId, const QString &hash);
    bool setVisualHash(qint64 itemId, const QString &hash);
    bool setDimensions(qint64 itemId, int width, int height, double durationMs);

    /*! 按内容哈希找同组条目。 */
    QList<MediaItem> itemsByContentHash(const QString &hash) const;
    /*! 所有出现次数 > 1 的内容哈希组，直接在 SQL 里筛掉独一份的。 */
    QHash<QString, QList<MediaItem>> duplicateGroups() const;

    // ---------- 标签 ----------

    QStringList allTags() const;
    QStringList tagsForItem(qint64 itemId) const;
    bool addTag(qint64 itemId, const QString &tag);
    bool removeTag(qint64 itemId, const QString &tag);
    /*! 全局删除标签：解绑所有条目并从标签表移除。侧边栏右键删除用。 */
    bool deleteTag(const QString &tag);
    /*! 一次取回多个条目的标签，避免网格里逐项查库。 */
    QHash<qint64, QStringList> tagsForItems(const QList<qint64> &itemIds) const;

    // ---------- 播放记录 ----------

    /*! 记录一次播放/预览。同一资源重复播放只更新时间与次数，不产生堆积行。 */
    bool recordPlayback(qint64 itemId, double positionMs = 0);
    /*! 最近播放列表，按时间倒序。 */
    QList<MediaItem> recentPlayed(int limit = 50) const;
    /*! 播放记录条数。侧边栏只显示计数时用它，比 recentPlayed().size() 便宜得多。 */
    int playHistoryCount() const;
    /*! 某资源的累计播放次数。 */
    int playCount(qint64 itemId) const;
    /*! 继续播放的位置，没有记录时返回 0。 */
    double lastPosition(qint64 itemId) const;
    void clearPlaybackHistory();

    // ---------- 统计 ----------

    struct Stats
    {
        int total = 0;
        int images = 0;
        int videos = 0;
        int audios = 0;
        int favorites = 0;
        int missing = 0;
        qint64 totalBytes = 0;
    };
    Stats stats() const;

signals:
    /*! 资源写入后发出，UI 据此刷新模型而不是全量重查。 */
    void libraryChanged();
    void errorOccurred(const QString &message);

private:
    bool ensureSchema();

    /*!
     * \brief 建全表索引（不含表结构）。
     *
     * 与建表分开的原因：旧版本数据库的表已经存在，只有索引是新增的。
     * 若把新索引塞进 ensureSchema() 的主流程，每次打开都会跑一遍
     * 十来个 CREATE INDEX IF NOT EXISTS —— 20 万条的库上虽然单个是 no-op，
     * 但 SQLite 仍要为每条语句做一次目录检查，白耗启动时间。
     * 因此用 schema_meta 里的版本号控制：只在版本号变化时建一次。
     */
    bool ensureIndexes();

    /*! 读取 schema_meta 里的版本号；表不存在或没有记录时返回 0。 */
    int storedSchemaVersion(QSqlDatabase &db) const;

    /*!
     * \brief 为某个连接设置 PRAGMA（WAL、synchronous 等）。
     *
     * 声明为 const 是必要的：它由 connectionForCurrentThread() 调用，
     * 而后者是 const（因为 isOpen() 等查询方法也要用它拿连接）。
     * 本方法只对传进来的 db 参数做设置，不修改 MediaDatabase 自身的状态，
     * 所以语义上确实应该是 const —— 之前的写法是 const 正确性疏漏。
     */
    void applyPragmas(QSqlDatabase &db) const;

    QString connectionForCurrentThread() const;

    QString m_dbPath;
    QString m_baseConnectionName;
    mutable QString m_lastError;

    /*!
     * 已创建的连接名。close() 时需要逐个 removeDatabase，
     * 否则 Qt 会警告 "connection is still in use"。
     * 用 mutable + 加锁，因为工作线程也会创建连接。
     */
    mutable QList<QString> m_connectionNames;
    mutable QMutex m_connMutex;
};

} // namespace media

#endif // MEDIADATABASE_H
