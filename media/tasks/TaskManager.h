#ifndef TASKMANAGER_H
#define TASKMANAGER_H

#include "../core/MediaTypes.h"
#include "TaskQueue.h"

#include <QObject>
#include <QString>
#include <QList>
#include <QHash>
#include <QDateTime>
#include <QPointer>
#include <QImage>
#include <QStringList>

namespace media {

class MediaDatabase;
class ThumbnailCache;
class TaskNotifier;

/*!
 * \brief 后台任务的统一入口。
 *
 * 任务书要求把"后台任务提交、状态跟踪、取消和进度管理"收在一个模块里。
 * 在加这一层之前，MainWindow 直接持有 TaskQueue 并自己 new ScanTask/ThumbnailTask，
 * 结果是：界面类知道太多任务细节（用什么并发度、任务怎么构造、结果怎么路由）。
 *
 * 这一层的职责：
 *  - 对外只暴露"发起一次扫描 / 请求一批缩略图 / 取消什么"这类业务语义
 *  - 对内管住 TaskQueue、任务对象的生命周期、结果回投到主线程
 *  - 维护一份"当前有哪些任务在跑"的状态，供状态栏与任务面板显示
 *
 * 它同时也是唯一直接接触 TaskNotifier 的地方，让 MainWindow 不必关心
 * 跨线程信号怎么排队。
 */
class TaskManager : public QObject
{
    Q_OBJECT

public:
    explicit TaskManager(QObject *parent = nullptr);
    ~TaskManager() override;

    /*!
     * \brief 注入依赖。这三个对象由外部（通常是 Application/MainWindow）拥有，
     * TaskManager 只借用指针，不接管所有权。
     */
    void initialize(MediaDatabase *database, ThumbnailCache *cache);

    // ---------- 扫描 ----------

    /*!
     * \brief 发起一次目录扫描。
     * \param path        根目录
     * \param markMissing 扫描后是否把本次未见的记录标为失效
     * \return 是否成功提交（同一目录已在扫描时返回 false）
     */
    bool startScan(const QString &path, bool markMissing = true);

    /*! 该目录当前是否正在扫描。 */
    bool isScanning(const QString &path) const;
    /*! 是否有任何扫描在进行。 */
    bool isScanningAny() const { return !m_activeScans.isEmpty(); }
    /*! 全部已登记的扫描根目录。 */
    QList<QString> activeScans() const { return m_activeScans; }

    // ---------- 缩略图 ----------

    /*!
     * \brief 请求一张缩略图。命中缓存会立即发 thumbnailReady。
     *
     * 与旧的直接 new ThumbnailTask 相比，这里额外做了两件事：
     *  - 同 id 的重复请求会被合并（正在生成时不再排第二个）
     *  - 登记任务指针，使 cancelThumbnails() 能精确取消
     */
    void requestThumbnail(const MediaItem &item, int edge);

    /*! 该 id 的缩略图是否正在生成。 */
    bool isThumbnailPending(qint64 itemId) const;

    /*! 取消全部未完成的缩略图任务（切目录/改筛选/关窗时用）。 */
    void cancelThumbnails();

    /*!
     * \brief 非阻塞地取消一切后台任务（扫描 + 缩略图），供任务面板"取消全部"使用。
     *
     * 取消全部时不能用队列级取消标志：那个标志置位后要等下一次
     * startScan() 里的 resetCancel() 才会清除，期间新提交的缩略图任务
     * 会全部静默失败（网格再也不出图）。所以这里改用任务级令牌：
     * 每个扫描任务在 startScan() 时登记 token，这里逐个置位，
     * 缩略图走 cancelThumbnails()。
     *
     * 已在运行的扫描会在下一个批次检查点（≤200ms）退出并照常回调 finished；
     * 还没开始跑的扫描在 run() 入口直接返回，不会回调 —— 因此这里
     * 主动清空登记表并补发 scanFinished，避免"幽灵扫描"永远挂在任务列表里。
     * 运行中的任务稍后重复回调 scanFinished 是无害的（UI 只是多刷新一次）。
     */
    void cancelAllNonBlocking();

    /*! 当前未完成的缩略图任务数。 */
    int pendingThumbnailCount() const { return m_pendingThumbs.size(); }

    // ---------- 取消与收尾 ----------

    /*!
     * \brief 取消一切并等待线程退出。
     *
     * 必须在关闭窗口/析构数据库之前调用。这是整个程序里
     * 唯一的"任务收尾"入口，避免各处各自 cancel 造成时序混乱。
     */
    void shutdown(int timeoutMs = 8000);

    /*! 队列当前的并发上限，供设置界面显示与调整。 */
    int concurrency() const;
    /*! 调整并发上限。 */
    void setConcurrency(int value);

    /*! 扫描配置。由设置界面写入，下一次扫描生效。 */
    void setScanOptions(bool recursive, bool readExif, bool skipHidden);
    /*! 附加的跳过目录名。 */
    void setExtraSkipDirs(const QStringList &dirs);

    // ---------- 状态查询 ----------

    struct TaskStatus
    {
        QString   label;        // 人类可读的描述，如"扫描 D:/Photos"
        QString   detail;       // 附加信息，如"已发现 1234 个文件"
        int       progress = 0; // 0-100，-1 表示不确定
        bool      cancellable = true;
        QDateTime startedAt;
    };

    /*! 当前任务的快照，供任务面板渲染。 */
    QList<TaskStatus> currentTasks() const;
    /*! 任务数量变化（开始/结束）时通知 UI 刷新面板。 */
    bool hasActiveTasks() const;

signals:
    /*! 扫描进度。 */
    void scanProgress(const media::ScanProgress &progress, const QString &rootPath);
    /*! 扫描结束。affected=新增/更新条数，discovered=发现总数。 */
    void scanFinished(const QString &rootPath, int affected, int discovered);
    /*! 扫描过程中的非致命警告。 */
    void scanWarning(const QString &path, const QString &message);

    /*! 缩略图就绪。在主线程发出。 */
    void thumbnailReady(qint64 itemId, const media::ThumbKey &key,
                        const QImage &image, bool ok);
    /*! 缩略图生成失败（坏图、无权限）。 */
    void thumbnailFailed(qint64 itemId, const QString &reason);

    /*! 任务集合发生变化，UI 应刷新任务面板与状态栏。 */
    void tasksChanged();

private:
    void wireNotifier();
    void onScanProgress(const ScanProgress &progress);
    void onScanWarning(const QString &path, const QString &message);

    MediaDatabase *m_database = nullptr;
    ThumbnailCache *m_cache = nullptr;

    TaskQueue *m_queue = nullptr;
    TaskNotifier *m_notifier = nullptr;

    /*! 正在扫描的根目录。用 QList 而非 QSet：数量极少，且顺序对显示有用。 */
    QList<QString> m_activeScans;

    /*!
     * 每个进行中扫描的任务取消令牌（根路径 → token）。
     * 与缩略图的 m_pendingThumbs 同理：存令牌不存任务指针，
     * 任务被线程池销毁后写入依然安全。用于 cancelAllNonBlocking()。
     */
    QHash<QString, CancelToken> m_scanTokens;

    /*! 扫描开始时间，用于任务面板显示已耗时。 */
    QHash<QString, QDateTime> m_scanStartedAt;
    /*! 最近一次进度，供面板显示"已发现 N"。 */
    ScanProgress m_lastScanProgress;

    /*!
     * 未完成的缩略图任务。存【取消令牌】而不是任务指针。
     *
     * 原因：QueuedTask 继承 QRunnable，不是 QObject，QPointer 对它是编译错误；
     * 而裸指针会悬垂（任务跑完就被 QThreadPool delete）。
     * CancelToken 是 shared_ptr<QAtomicInt>，任务销毁后依然可以安全写入，
     * 任务会在下一个检查点读到并退出。
     */
    QHash<qint64, CancelToken> m_pendingThumbs;
    /*! 待请求/生成中的 id 集合，用于合并重复请求。 */
    QHash<qint64, int> m_thumbEdgeByItem;

    /*! 扫描配置，由 setScanOptions 写入。 */
    bool m_recursive = true;
    bool m_readExif = true;
    bool m_skipHidden = true;
    QStringList m_extraSkipDirs;
};

} // namespace media

#endif // TASKMANAGER_H
