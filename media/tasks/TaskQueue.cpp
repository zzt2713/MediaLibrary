#include "TaskQueue.h"

#include <QThread>
#include <QMutexLocker>
#include <QDebug>

namespace media {

namespace {

/*!
 * 自动并发数：按核数，夹在 [4, 8]。
 *
 * 下限 4：双核机器上也让磁盘 IO 与图像解码有机会重叠。
 * 上限 8：再多在机械盘/网络盘上会因寻道竞争反而变慢，收益趋于平坦。
 * 构造函数与 setConcurrency(<=0) 共用这一处，避免两套值漂移。
 */
int autoConcurrencyValue()
{
    return qBound(4, QThread::idealThreadCount(), 8);
}

} // namespace

TaskQueue::TaskQueue(QObject *parent)
    : QObject(parent)
{
    m_pool = new QThreadPool(this);

    // 缩略图任务是混合负载：解码吃 CPU，读文件/写缓存吃 IO，两者重叠得很好，
    // 所以按核数给足并发比固定一个保守值更能缩短整屏加载时间（见 autoConcurrencyValue）。
    // 用户可在设置里手动调（1-32），这里的自动值只是默认起点。
    const int autoConcurrency = autoConcurrencyValue();

    m_concurrency = autoConcurrency;
    m_semaphore = new QSemaphore(m_concurrency);

    // 线程池上限留出余量：真正限流靠 semaphore，
    // 线程池只需要保证有足够线程来跑"已获准"的任务即可。
    m_pool->setMaxThreadCount(qMax(8, m_concurrency * 2));

    // 让线程池在空闲一段时间后回收线程，避免长期占着几十个线程不放
    m_pool->setExpiryTimeout(30000);
}

TaskQueue::~TaskQueue()
{
    // 析构前必须等干净：否则工作线程持有 this 的指针继续跑会崩
    cancelAllAndWait();
}

void TaskQueue::setConcurrency(int concurrency)
{
    // <=0 表示"回到自动值"。与构造函数共用 autoConcurrencyValue()：
    // 两处各写一套会漂移，参数传入 0 时得到的行为和默认值不同会很意外。
    if (concurrency <= 0)
        concurrency = autoConcurrencyValue();

    QMutexLocker locker(&m_mutex);
    const int delta = concurrency - m_concurrency;
    m_concurrency = concurrency;

    // 扩容：直接放行更多（release 正数）
    // 缩容：先消耗掉多余许可（acquire 负数不可能的，改用 tryAcquire 循环）
    if (delta > 0) {
        m_semaphore->release(delta);
    } else if (delta < 0) {
        // 缩容时不能保证立刻生效：正在跑的任务数可能已经超过新上限。
        // 这里只把信号量可用数降到目标，后续任务自然按新上限排队。
        for (int i = 0; i < -delta; ++i) {
            if (!m_semaphore->tryAcquire(1))
                break;   // 没得扣了，说明已有任务在跑，等它们结束即可
        }
    }

    qDebug() << "[TaskQueue] concurrency =" << concurrency;
}

void TaskQueue::submit(QRunnable *task)
{
    if (!task)
        return;

    m_pool->start(task);
    emit pendingChanged(pendingCount());
}

int TaskQueue::pendingCount() const
{
    if (!m_pool)
        return 0;

    // activeThreadCount 是正在跑的，semaphore 可用数反映排队情况。
    // 这里用「线程池活跃数」作为近似——对 UI 显示"正在加载"已经足够。
    return m_pool->activeThreadCount();
}

void TaskQueue::requestCancelAll()
{
    m_cancelFlag.storeRelaxed(1);
}

void TaskQueue::resetCancel()
{
    m_cancelFlag.storeRelaxed(0);
}

void TaskQueue::cancelAllAndWait(int timeoutMs)
{
    requestCancelAll();

    if (m_pool) {
        m_pool->clear();          // 丢掉还没开始跑的排队任务
        if (!m_pool->waitForDone(timeoutMs)) {
            // 超时不代表任务会继续跑下去：任务内部会看到 cancelFlag 并尽快返回。
            // 这里只警告，不强制终止线程（Qt 不支持安全地强杀线程）。
            qWarning() << "[TaskQueue] waitForDone timed out after" << timeoutMs << "ms";
        }
    }

    // 所有任务都已退出，把许可补回满额，以便后续 resetCancel 后能正常使用
    {
        QMutexLocker locker(&m_mutex);
        if (m_semaphore) {
            const int available = m_semaphore->available();
            if (available < m_concurrency)
                m_semaphore->release(m_concurrency - available);
        }
    }

    emit pendingChanged(0);
    emit allTasksDone();
}

void TaskQueue::notifyRunningDelta(int delta)
{
    Q_UNUSED(delta)
    // 信号必须在对象自己的成员函数里发出。
    // 这里从工作线程 emit，Qt 会自动排队到接收者所在线程（Qt::AutoConnection）。
    emit pendingChanged(pendingCount());
}

// ---------------------------------------------------------------- TaskNotifier

TaskNotifier::TaskNotifier(QObject *parent)
    : QObject(parent)
{
    // 刻意什么都不做：这个类只是"信号转发桥"，没有状态。
    // 构造函数必须有定义，否则 TaskManager 里 new TaskNotifier 会链接失败。
}

// ---------------------------------------------------------------- QueuedTask

QueuedTask::QueuedTask(TaskQueue *queue)
    : m_queue(queue)
    // 取消标志在这里创建。外部通过 cancelToken() 拿到同一个对象，
    // 于是即使本任务已被线程池 delete，外部手里的令牌依然有效。
    , m_ownCancelled(std::make_shared<QAtomicInt>(0))
{
    // 由 QThreadPool 在 run() 结束后 delete 自己
    setAutoDelete(true);
}

QueuedTask::~QueuedTask() = default;

void QueuedTask::run()
{
    if (!m_queue) {
        // 队列已析构，任务不应该再跑
        return;
    }

    // 已经取消就不必占并发名额了，直接返回
    if (shouldCancel())
        return;

    // 限流点：超出并发上限的任务会在这里阻塞排队。
    // 之所以不丢进队列自己管，是因为 QSemaphore 的阻塞等待是内核级的，
    // 不占 CPU，也不需要轮询。
    if (m_queue->m_semaphore) {
        m_queue->m_semaphore->acquire();
        m_acquired = true;
    }

    // acquire 可能等了很久，拿到许可后要再看一次取消标志
    if (shouldCancel()) {
        if (m_acquired) {
            m_queue->m_semaphore->release();
            m_acquired = false;
        }
        return;
    }

    if (m_queue) {
        m_queue->m_running.ref();
        m_queue->notifyRunningDelta(1);
    }

    runTask();

    if (m_queue) {
        m_queue->m_running.deref();
        m_queue->notifyRunningDelta(-1);
    }

    if (m_acquired && m_queue && m_queue->m_semaphore) {
        m_queue->m_semaphore->release();
        m_acquired = false;
    }
}

bool QueuedTask::shouldCancel() const
{
    // 队列级取消（切目录 / 关窗口）优先，其次看本任务是否被单独取消
    if (isOwnCancelled())
        return true;

    return m_queue && m_queue->isCancelled();
}

} // namespace media
