#include "TaskManager.h"
#include "ScanTask.h"
#include "ThumbnailTask.h"
#include "../core/MediaDatabase.h"
#include "../core/ThumbnailCache.h"
#include "../core/Logger.h"

#include <QDir>
#include <QFileInfo>
#include <QMetaObject>

namespace media {

TaskManager::TaskManager(QObject *parent)
    : QObject(parent)
{
    m_queue = new TaskQueue(this);
    m_notifier = new TaskNotifier(this);

    wireNotifier();

    // 队列的排队数变化直接转成 tasksChanged，
    // UI 只需要关心"任务集合变了"，不必理解 TaskQueue 的内部计数。
    connect(m_queue, &TaskQueue::pendingChanged,
            this, &TaskManager::tasksChanged);
}

TaskManager::~TaskManager()
{
    /*!
     * 析构时必须确保线程停干净。
     *
     * 注意这里不能依赖 QObject 的父子树顺序——m_queue 是 this 的子对象，
     * 会随 this 一起析构，但工作线程可能仍在跑任务、
     * 而任务持有的 MediaDatabase 指针在更早的别处被析构。
     * 所以在析构函数体里显式等一次。
     */
    if (m_queue)
        m_queue->cancelAllAndWait(8000);
}

void TaskManager::wireNotifier()
{
    /*!
     * TaskNotifier 的信号在主线程被接收（notifier 属于主线程）。
     *
     * ScanTask 端用 DirectConnection 把信号"推"进 notifier，
     * 由 notifier 在这里用普通连接投递到主线程 ——
     * 于是扫描线程不必等 UI，而 UI 也能实时收到进度。
     * 这条链路的理由在 ScanTask::runTask() 里有详细注释。
     */
    connect(m_notifier, &TaskNotifier::progress,
            this, &TaskManager::onScanProgress);
    connect(m_notifier, &TaskNotifier::warning,
            this, &TaskManager::onScanWarning);
}

void TaskManager::initialize(MediaDatabase *database, ThumbnailCache *cache)
{
    m_database = database;
    m_cache = cache;
}

int TaskManager::concurrency() const
{
    return m_queue ? m_queue->concurrency() : 0;
}

void TaskManager::setConcurrency(int value)
{
    if (m_queue)
        m_queue->setConcurrency(value);
}

void TaskManager::setScanOptions(bool recursive, bool readExif, bool skipHidden)
{
    m_recursive = recursive;
    m_readExif = readExif;
    m_skipHidden = skipHidden;
}

void TaskManager::setExtraSkipDirs(const QStringList &dirs)
{
    m_extraSkipDirs = dirs;
}

// ---------------------------------------------------------------- 扫描

bool TaskManager::startScan(const QString &path, bool markMissing)
{
    if (path.isEmpty() || !m_database || !m_database->isOpen())
        return false;

    const QString norm = QDir::cleanPath(QDir::fromNativeSeparators(path));

    // 防重复提交：同一目录已在扫描就直接返回。
    // 用户连点刷新、或文件监控的多次事件很容易触发重复提交，
    // 重复扫描会白白吃掉大量磁盘 IO。
    if (m_activeScans.contains(norm)) {
        LOG_DEBUG("Task") << "scan already running for " << norm << ", skipped";
        return false;
    }

    QFileInfo info(norm);
    if (!info.exists() || !info.isDir()) {
        emit scanWarning(norm, QStringLiteral("目录不存在或不是目录"));
        return false;
    }

    m_activeScans.append(norm);
    m_scanStartedAt.insert(norm, QDateTime::currentDateTime());
    // 扫描期间挂起取消标志的重置：新一轮任务开始，清掉上一轮的取消状态
    m_queue->resetCancel();

    auto *task = new ScanTask(m_queue, m_database, m_notifier, norm);

    MediaScanner::Options options;
    options.recursive = m_recursive;
    options.readExif = m_readExif;
    // 预统计：为了让进度条有真实分母。大目录上多走一次遍历，值得。
    options.countFirst = true;
    options.followSymlinks = false;   // 符号链接可能成环，导致无限遍历

    if (m_skipHidden) {
        // MediaScanner 内部默认跳过隐藏目录（name 以 '.' 开头），
        // 这里把用户自定义的附加跳过名单合并进去。
        options.skipDirNames.append(m_extraSkipDirs);
    } else {
        // 用户明确要求不跳过隐藏目录时，把内置名单也清掉，
        // 避免"设置里关了但行为没变"的困惑。
        options.skipDirNames = m_extraSkipDirs;
    }

    task->setScannerOptions(options);
    task->setMarkMissing(markMissing);

    // 登记取消令牌，供 cancelAllNonBlocking() 精确取消这一个扫描
    m_scanTokens.insert(norm, task->cancelToken());

    task->setFinishedCallback([this](const QString &rootPath, int affected, int discovered) {
        /*!
         * 这个回调在 **工作线程** 里执行，所以这里绝不能碰 UI 或模型。
         * 用 QMetaObject::invokeMethod 把处理排到主线程。
         * 这是整个项目里跨线程最容易写错的一处。
         */
        QMetaObject::invokeMethod(this, [this, rootPath, affected, discovered]() {
            m_activeScans.removeAll(rootPath);
            m_scanStartedAt.remove(rootPath);
            m_scanTokens.remove(rootPath);

            LOG_INFO("Task") << "scan finished: " << rootPath
                             << " affected=" << affected
                             << " discovered=" << discovered;

            emit scanFinished(rootPath, affected, discovered);
            emit tasksChanged();
        }, Qt::QueuedConnection);
    });

    m_queue->submit(task);

    LOG_INFO("Task") << "scan submitted: " << norm;
    emit tasksChanged();

    return true;
}

bool TaskManager::isScanning(const QString &path) const
{
    const QString norm = QDir::cleanPath(QDir::fromNativeSeparators(path));
    return m_activeScans.contains(norm);
}

void TaskManager::onScanProgress(const ScanProgress &progress)
{
    // 已在主线程（notifier 属于主线程，信号经队列投递过来）
    m_lastScanProgress = progress;
    emit scanProgress(progress, progress.currentPath);
    emit tasksChanged();
}

void TaskManager::onScanWarning(const QString &path, const QString &message)
{
    LOG_WARN("Task") << path << " : " << message;
    emit scanWarning(path, message);
}

// ---------------------------------------------------------------- 缩略图

bool TaskManager::isThumbnailPending(qint64 itemId) const
{
    return m_thumbEdgeByItem.contains(itemId);
}

void TaskManager::requestThumbnail(const MediaItem &item, int edge)
{
    if (!item.isValid() || !m_cache)
        return;

    // 合并重复请求。
    // 网格滚动时会反复请求同一批可见项，若不合并，
    // 同一张图会被排队生成多次（第一次的结果还没回来，第二次又进来了）。
    if (m_thumbEdgeByItem.contains(item.id)) {
        const int requestedEdge = m_thumbEdgeByItem.value(item.id);
        if (requestedEdge >= edge) {
            // 已有一个尺寸不小于当前请求的任务在跑，它的结果足够用
            return;
        }
        // 新请求要求更大尺寸：作废旧的，重新排一个。
        // 这种情况出现在用户调大缩略图滑块时。
        // 这里只对令牌写取消标志 —— 任务本身可能已经被销毁，
        // 但令牌是共享的，写入永远安全。
        if (auto token = m_pendingThumbs.value(item.id))
            token->storeRelaxed(1);
        m_pendingThumbs.remove(item.id);
        m_thumbEdgeByItem.remove(item.id);
    }

    // 先查两级缓存。命中就不必建任务 —— 否则缓存命中也会占用并发名额，
    // 在大目录里会把队列堵满。
    ThumbKey key;
    key.path = item.path;
    key.mtimeMs = item.mtimeMs;
    key.size = item.size;
    key.edge = edge;

    bool fromDisk = false;
    const QImage cached = m_cache->get(key, &fromDisk);
    if (!cached.isNull()) {
        // 即便同步就拿到了，也走一次队列投递。
        // 理由：调用方（网格）可能在绘制循环里发请求，
        // 在那里直接同步发信号会重入视图代码，行为不可预期。
        QMetaObject::invokeMethod(this, [this, id = item.id, key, cached]() {
            emit thumbnailReady(id, key, cached, true);
        }, Qt::QueuedConnection);
        return;
    }

    auto *task = new ThumbnailTask(m_queue, m_cache, m_database, item, edge);

    /*!
     * 结果回调在工作线程执行，只做一件事：把结果投回主线程。
     *
     * 跨线程携带 QImage 而不是 QPixmap —— QPixmap 绑定平台后端，
     * 只能在 GUI 线程构造使用；QImage 是纯 CPU 像素缓冲，可以安全跨线程。
     * lambda 值捕获 QImage 走隐式共享，不会真的复制像素。
     */
    task->setResultCallback([this](qint64 itemId, const ThumbKey &k,
                                   const QImage &image, bool ok) {
        QMetaObject::invokeMethod(this, [this, itemId, k, image, ok]() {
            m_pendingThumbs.remove(itemId);
            m_thumbEdgeByItem.remove(itemId);
            emit thumbnailReady(itemId, k, image, ok);
        }, Qt::QueuedConnection);
    });

    task->setErrorCallback([this](qint64 itemId, const QString &reason) {
        QMetaObject::invokeMethod(this, [this, itemId, reason]() {
            LOG_DEBUG("Task") << "thumbnail failed for item " << itemId << " : " << reason;
            emit thumbnailFailed(itemId, reason);
        }, Qt::QueuedConnection);
    });

    m_pendingThumbs.insert(item.id, task->cancelToken());
    m_thumbEdgeByItem.insert(item.id, edge);

    m_queue->submit(task);
}

void TaskManager::cancelThumbnails()
{
    /*!
     * 只做"标记取消 + 清登记表"，不等待、不删除。
     *
     * 任务会在下一个 shouldCancel() 检查点自行退出，
     * release 与对象销毁由 TaskQueue 与线程池负责。
     * 用 QPointer 判空是因为任务可能已经跑完并被线程池 delete。
     */
    int cancelled = 0;

    for (auto it = m_pendingThumbs.begin(); it != m_pendingThumbs.end(); ++it) {
        // 令牌一定非空（插入时即已创建），但仍判一下以防将来改动引入空值
        if (auto token = it.value()) {
            token->storeRelaxed(1);
            ++cancelled;
        }
    }

    m_pendingThumbs.clear();
    m_thumbEdgeByItem.clear();

    if (cancelled > 0) {
        LOG_DEBUG("Task") << "cancelled " << cancelled << " pending thumbnail tasks";
        emit tasksChanged();
    }
}

// ---------------------------------------------------------------- 收尾

void TaskManager::cancelAllNonBlocking()
{
    int cancelledScans = 0;

    for (auto it = m_scanTokens.begin(); it != m_scanTokens.end(); ++it) {
        if (auto token = it.value()) {
            token->storeRelaxed(1);
            ++cancelledScans;
        }
    }

    const QList<QString> cancelledRoots = m_activeScans;

    m_activeScans.clear();
    m_scanStartedAt.clear();
    m_scanTokens.clear();
    m_lastScanProgress = ScanProgress();

    cancelThumbnails();

    /*!
     * 为每个被取消的根目录补发 scanFinished。
     *
     * 还没开始跑的扫描任务在 run() 入口看到取消标志就直接返回，
     * 它的 finished 回调永远不会执行；若不补发，主窗口的
     * m_activeScans 与文件监控的挂起状态会永远停在"扫描中"。
     * 运行中的任务稍后还会再回调一次 —— 主窗口那边的处理是幂等的，
     * 多调一次只是多刷新一次界面，无害。
     */
    for (const QString &root : cancelledRoots)
        emit scanFinished(root, 0, 0);

    if (cancelledScans > 0) {
        LOG_INFO("Task") << "cancelled " << cancelledScans << " scan tasks (non-blocking)";
    }

    emit tasksChanged();
}

void TaskManager::shutdown(int timeoutMs)
{
    LOG_INFO("Task") << "shutting down task manager, timeout=" << timeoutMs;

    if (m_queue) {
        // 队列级取消 + 等待。会覆盖所有残留的扫描与缩略图任务。
        m_queue->cancelAllAndWait(timeoutMs);
    }

    m_activeScans.clear();
    m_scanStartedAt.clear();
    m_scanTokens.clear();
    m_pendingThumbs.clear();
    m_thumbEdgeByItem.clear();

    emit tasksChanged();
}

// ---------------------------------------------------------------- 状态

bool TaskManager::hasActiveTasks() const
{
    if (!m_activeScans.isEmpty())
        return true;

    return !m_pendingThumbs.isEmpty();
}

QList<TaskManager::TaskStatus> TaskManager::currentTasks() const
{
    QList<TaskStatus> tasks;

    // 扫描任务：每个被扫描的根目录是一条
    for (const QString &root : m_activeScans) {
        TaskStatus status;
        status.label = QStringLiteral("扫描 %1").arg(QFileInfo(root).fileName());
        status.detail = root;

        // 有预统计总数时给出百分比，否则是不确定进度
        if (m_lastScanProgress.total > 0) {
            const int pct = qBound(0, m_lastScanProgress.discovered * 100
                                          / m_lastScanProgress.total, 100);
            status.progress = pct;
            status.detail = QStringLiteral("%1 / %2 个文件")
                                .arg(m_lastScanProgress.discovered)
                                .arg(m_lastScanProgress.total);
        } else {
            status.progress = -1;
            if (m_lastScanProgress.discovered > 0) {
                status.detail = QStringLiteral("已发现 %1 个文件")
                                    .arg(m_lastScanProgress.discovered);
            }
        }

        status.cancellable = true;
        status.startedAt = m_scanStartedAt.value(root);
        tasks.append(status);
    }

    // 缩略图任务汇总成一条。
    // 不逐个列出来：滚动时可能有几十个同时在跑，
    // 逐个显示既无意义也刷不过来。
    if (!m_pendingThumbs.isEmpty()) {
        TaskStatus status;
        status.label = QStringLiteral("生成缩略图");
        status.detail = QStringLiteral("剩余 %1 项").arg(m_pendingThumbs.size());
        status.progress = -1;   // 无法预知总量
        status.cancellable = true;
        tasks.append(status);
    }

    return tasks;
}

} // namespace media
