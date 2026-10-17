#include "ScanTask.h"

#include <QDir>
#include <QFileInfo>
#include <QDebug>
#include <QSet>
#include <chrono>

namespace media {

ScanTask::ScanTask(TaskQueue *queue,
                   MediaDatabase *database,
                   TaskNotifier *notifier,
                   const QString &rootPath,
                   int recursionDepth)
    : QueuedTask(queue)
    , m_database(database)
    , m_notifier(notifier)
    , m_rootPath(QDir::cleanPath(QDir::fromNativeSeparators(rootPath)))
    , m_recursionDepth(recursionDepth)
{
}

ScanTask::~ScanTask() = default;

void ScanTask::runTask()
{
    if (!m_database || m_rootPath.isEmpty())
        return;

    if (!m_database->isOpen()) {
        if (m_notifier)
            m_notifier->emitWarning(m_rootPath, QStringLiteral("数据库未打开，扫描已跳过"));
        if (m_finished)
            m_finished(m_rootPath, 0, 0);
        return;
    }

    // 登记根目录（若尚未登记）。recursed 子任务不重复登记。
    if (m_recursionDepth == 0)
        m_database->addFolder(m_rootPath);

    MediaScanner scanner;
    scanner.setOptions(m_scannerOptions);

    /*!
     * 取消联动。
     *
     * ScanTask 没法在扫描进行中直接调 scanner.cancel()——runTask 是同步阻塞的，
     * 控制权一直在 scanDirectory 的循环里。所以在批次回调里做**时间心跳**：
     * 每处理完一批文件就看一下距离上次检查是否已过 200ms，到了就检查取消标志，
     * 需要时调 scanner.cancel()，后续循环就会退出。
     *
     * 用"时间"而不是"文件计数"作为节奏，是因为按计数在文件数少的目录里
     * 可能整趟扫描都触发不了一次；而扫描恰恰容易在"目录多、每个目录文件少"
     * 的结构上耗时最久。
     *
     * 最坏情况下取消延迟不超过 200ms 加一个批次（64 个文件）的处理时间，
     * 用户按下取消后的感知是即时的。
     */
    bool cancelRelayed = false;
    auto lastCancelCheck = std::chrono::steady_clock::now();
    constexpr auto kCancelCheckInterval = std::chrono::milliseconds(200);

    // 本次扫描见过的路径，用于结束后标记失效记录
    QStringList seenPaths;
    seenPaths.reserve(1024);

    int affectedTotal = 0;

    /*!
     * 关于这里的 Qt::DirectConnection —— 这是本项目里最容易被问倒的一处，说明清楚：
     *
     * TaskNotifier 对象归属主线程。如果这里用默认的 AutoConnection，
     * 由于"发送者"(scanner, 工作线程)与"接收者"(notifier, 主线程)线程不同，
     * 信号会被排队投递到主线程的事件循环。
     *
     * 但问题在于：扫描是在工作线程里**同步阻塞**执行的。排队投递意味着
     * 进度信号要等扫描返回、工作线程回到事件循环后才会被处理——
     * 那时扫描早就结束了，进度条会一直是 0% 然后突然跳到 100%。
     *
     * 所以这里刻意用 DirectConnection：让 emitProgress() 立即在**工作线程内**执行。
     * 它内部只是 `emit progress(...)`，而 MainWindow 连接这个信号时用的是
     * 普通连接 —— 主线程的槽会在通知时被排队，于是：
     *
     *   工作线程 scanner 循环
     *     → (DirectConnection) notifier->emitProgress()
     *       → (AutoConnection, 跨线程) 排队到主线程
     *         → MainWindow::onScanProgress() 更新进度条
     *
     * 也就是说：信号"发出"是同步的（保证实时），"处理"是异步的（不阻塞扫描）。
     * DirectConnection 只跨越了中间那一层转发，没有让任何 UI 代码在工作线程执行。
     */
    /*!
     * 进度与警告通过 notifier 转发。notifier 为空时跳过连接——
     * QObject::connect 传空指针只会返回 false 并打印警告，
     * 但显式判断更清楚，也避免日志噪音。
     */
    if (m_notifier) {
        QObject::connect(&scanner, &MediaScanner::progress,
                         m_notifier.data(), &TaskNotifier::emitProgress,
                         Qt::DirectConnection);

        QObject::connect(&scanner, &MediaScanner::warning,
                         m_notifier.data(), &TaskNotifier::emitWarning,
                         Qt::DirectConnection);
    }

    const int discovered = scanner.scanDirectory(
        m_rootPath,
        [this, &seenPaths, &affectedTotal, &cancelRelayed, &scanner, &lastCancelCheck](
            const QList<MediaItem> &batch) {
            if (cancelRelayed)
                return;

            // 时间心跳：不管本批有多少文件都检查一次
            const auto now = std::chrono::steady_clock::now();
            if (now - lastCancelCheck >= kCancelCheckInterval) {
                lastCancelCheck = now;
                if (shouldCancel()) {
                    scanner.cancel();
                    cancelRelayed = true;
                    return;
                }
            }

            for (const MediaItem &item : batch)
                seenPaths.append(item.path);

            // 直接写库：MediaDatabase 为当前线程取独立连接，跨线程安全
            affectedTotal += m_database->upsertItems(batch);
        });

    // 被取消时不做失效标记——否则会把"还没扫到"误判成"文件已删除"
    if (!shouldCancel() && m_markMissing && !seenPaths.isEmpty()) {
        const int marked = m_database->markMissing(m_rootPath, seenPaths);
        if (marked > 0)
            qDebug() << "[ScanTask] marked missing:" << marked << "under" << m_rootPath;
    }

    if (m_finished)
        m_finished(m_rootPath, affectedTotal, discovered);
}

} // namespace media
