#ifndef TASKQUEUE_H
#define TASKQUEUE_H

#include <QObject>
#include <QThreadPool>
#include <QSemaphore>
#include <QRunnable>
#include <QAtomicInt>
#include <QHash>
#include <QMutex>
#include <QPointer>
// CancelToken 用 shared_ptr<QAtomicInt>
#include <memory>

#include "../core/MediaTypes.h"

namespace media {

/*!
 * \brief 带并发上限与取消能力的任务队列。
 *
 * 为什么不直接用 QThreadPool：
 * QThreadPool 限制的是"占用的线程数"，但缩略图任务几乎全部时间在等磁盘 IO，
 * 线程数等于 CPU 核数会让 IO 排队，反而变慢；而全部放开又会因为
 * 同时解码几百张图把内存打满。所以这里用 QSemaphore 单独限制"同时在跑的任务数"，
 * 与线程池大小解耦——这个值可以按 IO 密集程度单独调。
 *
 * 每个任务在 run() 开始时 acquire、结束时 release，
 * 于是超出上限的任务会阻塞在 acquire 上排队，而不是一起挤进内存。
 */
class TaskQueue : public QObject
{
    Q_OBJECT

public:
    explicit TaskQueue(QObject *parent = nullptr);
    ~TaskQueue() override;

    /*!
     * \brief 设置最大并发任务数。
     * \param concurrency <= 0 时自动取 idealThreadCount()，并夹在 [4, 8]
     *
     * 默认值取核数（上限 8）而不是固定 4：
     * 缩略图任务是「读文件 + 解码」混合型。解码是纯 CPU、单张图不并行，
     * 所以固定 4 在 8 核以上的机器上白白空着一半核心——这正是大目录里
     * 缩略图一张张慢慢冒出来的主要原因。IO 侧因为读的是顺序小文件，
     * 并发到 8 仍在普通磁盘的能力范围内；再高（机械盘、网络盘）会因
     * 寻道竞争反而变慢，所以上限压在 8。
     *
     * 用户可在设置里手动指定 1-32，覆盖这里的默认。
     */
    void setConcurrency(int concurrency);
    int concurrency() const { return m_concurrency; }

    /*! 提交任务。队列内部会接管其所有权（QRunnable 默认 autoDelete）。 */
    void submit(QRunnable *task);

    /*! 当前排队的任务数（含正在运行的）。 */
    int pendingCount() const;

    /*!
     * \brief 取消所有已提交任务并等待线程结束。
     *
     * 窗口关闭时必须调用：否则工作线程可能还在写数据库，
     * 而 QSqlDatabase 已经被析构，会崩在析构阶段。
     * \param timeoutMs 等待上限，超时则放弃等待（记录警告）
     */
    void cancelAllAndWait(int timeoutMs = 5000);

    /*! 请求取消，但不阻塞等待。适合"切换目录"这类场景。 */
    void requestCancelAll();

    /*! 清掉取消标记，开始新一轮任务前调用。 */
    void resetCancel();

    bool isCancelled() const { return m_cancelFlag.loadRelaxed() != 0; }

    /*! 供任务实现读取的取消标志指针。 */
    const QAtomicInt *cancelFlag() const { return &m_cancelFlag; }

    /*!
     * \brief 供 QueuedTask 上报运行状态。
     *
     * 信号只能由对象自己的成员函数发出，外部不能对 m_queue-> 直接 emit，
     * 所以这里开一个明确的内部接口。
     */
    void notifyRunningDelta(int delta);

signals:
    /*! 排队任务数变化，UI 据此显示"正在加载 N 项"。 */
    void pendingChanged(int pending);
    /*! 队列空闲。 */
    void allTasksDone();

private:
    friend class QueuedTask;   // 需要访问 semaphore 做限流

    QThreadPool *m_pool = nullptr;
    QSemaphore  *m_semaphore = nullptr;
    int          m_concurrency = 0;
    QAtomicInt   m_cancelFlag{ 0 };
    QAtomicInt   m_running{ 0 };

    mutable QMutex m_mutex;
};

/*!
 * \brief 任务的取消令牌。
 *
 * 为什么需要它：QRunnable【不是 QObject】，所以不能用 QPointer 做悬垂保护。
 * 而调用方（TaskManager）需要在任务可能已被线程池 delete 之后
 * 仍然能安全地"请求取消"——直接持有裸指针会 use-after-free。
 *
 * 解法是把取消标志做成共享对象：任务持有一份，容器也持有一份。
 * 即使任务对象已销毁，容器手里的 shared_ptr 依然有效，
 * 对它的写入不会碰到已释放的内存；任务在下次检查点读到标志即自行退出。
 */
using CancelToken = std::shared_ptr<QAtomicInt>;

/*!
 * \brief 任务基类。封装了 acquire/release 与取消检查，
 * 子类只需要实现 runTask()。
 *
 * 注意：QRunnable 不是 QObject，不能带信号。
 * 需要向 UI 汇报的任务请走 TaskNotifier（见下），
 * 它是独立的对象，由任务在工作线程里调用，信号自动排队到主线程。
 *
 * 取消有两级：
 *  - 队列级：TaskQueue::requestCancelAll() 会把所有任务一起取消（切目录、关窗口）
 *  - 任务级：通过 CancelToken 只停某一个任务（例如某张图已滚出可视区）
 */
class QueuedTask : public QRunnable
{
public:
    explicit QueuedTask(TaskQueue *queue);
    ~QueuedTask() override;

    void run() final;

    /*!
     * \brief 只取消这一个任务。
     *
     * 与队列级取消的区别：队列级是"全都别做了"（用户切了目录），
     * 任务级是"这一个不用做了"（某个缩略图已经滚出可视区）。
     * 后者更精细，能避免为一个已经看不见的项白跑一次磁盘读 + 解码。
     *
     * 线程安全：可从任意线程调用（UI 线程取消、工作线程读取）。
     */
    void ownCancel() { m_ownCancelled->storeRelaxed(1); }
    bool isOwnCancelled() const { return m_ownCancelled->loadRelaxed() != 0; }

    /*!
     * \brief 取出本任务的取消令牌。
     *
     * 调用方应保存这个 shared_ptr 而不是任务指针：
     * 任务被线程池 delete 之后，令牌依然可以安全使用。
     */
    CancelToken cancelToken() const { return m_ownCancelled; }

protected:
    /*! 子类在这里做实际工作。返回前不需要自己 release。 */
    virtual void runTask() = 0;

    /*!
     * \brief 在长循环中调用，检查是否需要提前退出。
     * \return true 表示已取消（队列级或任务级），子类应立即返回
     */
    bool shouldCancel() const;

    TaskQueue *queue() const { return m_queue; }

private:
    /*!
     * TaskQueue 是 QObject，可以用 QPointer 保护它的存活。
     * 注意这里指向队列而不是任务自身 —— 任务不是 QObject，不能用 QPointer。
     */
    QPointer<TaskQueue> m_queue;
    bool m_acquired = false;

    /*!
     * 取消标志。用 shared_ptr 而不是直接成员，是为了让外部
     * （TaskManager）能在任务销毁后仍然安全地请求取消。
     */
    CancelToken m_ownCancelled;
};

/*!
 * \brief 任务信号桥。工作线程里调用 emitXxx()，UI 线程接收信号。
 *
 * 之所以单独抽一个 QObject 而不是让任务类继承 QObject 再走 moveToThread：
 * QRunnable 的生命周期由 QThreadPool 管理（autoDelete），
 * 把它同时做成 QObject 会让「谁负责 delete」变得含糊。
 * 拆开之后职责清晰：Notifier 归 UI 管，Runnable 归线程池管。
 */
class TaskNotifier : public QObject
{
    Q_OBJECT

public:
    explicit TaskNotifier(QObject *parent = nullptr);

    /*!
     * 注意参数名不能叫 progress —— 会遮蔽同名信号，
     * 导致 `emit progress(progress)` 里前一个 progress 解析成参数对象，
     * 编译器报 C2064 "term does not evaluate to a function"。
     * 命名遮蔽在这类"转发信号"的小函数里特别容易踩到。
     */
    void emitProgress(const ScanProgress &info) { emit progress(info); }

    /*!
     * 这里同理：参数叫 path / message 也要留意，
     * 虽然 warning(path, message) 恰好没有遮蔽（参数名与信号名不同），
     * 但为保持一致性与可读性，转发时用显式不同的名字。
     */
    void emitWarning(const QString &path, const QString &message) { emit warning(path, message); }

signals:
    void progress(const media::ScanProgress &progress);
    void warning(const QString &path, const QString &message);
};

} // namespace media

#endif // TASKQUEUE_H
