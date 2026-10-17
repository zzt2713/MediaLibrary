#ifndef SCANTASK_H
#define SCANTASK_H

#include "TaskQueue.h"
#include "../core/MediaTypes.h"
#include "../core/MediaScanner.h"
#include "../core/MediaDatabase.h"

#include <QPointer>
#include <QString>
// 直接使用 std::function，显式包含而不是依赖 MediaScanner.h 的传递引入——
// 传递依赖一旦上游改了头文件就会突然断掉，排查起来很费时间。
#include <functional>

namespace media {

/*!
 * \brief 目录扫描任务。
 *
 * 关键点：扫描结果不经过信号槽传回 UI，而是在工作线程里直接写库，
 * 只把进度与统计发回。理由：
 *  - 上万条 MediaItem 跨线程拷贝开销可观；
 *  - 数据库写入本身耗时，放主线程做等于把扫描又变回同步；
 *  - MediaDatabase 按线程分配连接，工作线程直接调它是安全的。
 *
 * 信号由外部传入的 TaskNotifier 转发，因此本类不继承 QObject。
 */
class ScanTask : public QueuedTask
{
public:
    ScanTask(TaskQueue *queue,
             MediaDatabase *database,
             TaskNotifier *notifier,
             const QString &rootPath,
             int recursionDepth = 0);

    ~ScanTask() override;

    /*! 扫描完后是否把本次未见的记录标为失效。 */
    void setMarkMissing(bool enable) { m_markMissing = enable; }
    void setScannerOptions(const MediaScanner::Options &options) { m_scannerOptions = options; }

    /*!
     * 完成回调。在工作线程内、runTask 结束前被调用。
     * 参数：根路径、新增/更新条数、发现总数。
     */
    using FinishedCallback = std::function<void(const QString &, int, int)>;
    void setFinishedCallback(FinishedCallback cb) { m_finished = std::move(cb); }

protected:
    void runTask() override;

private:
    QPointer<MediaDatabase> m_database;
    QPointer<TaskNotifier>  m_notifier;
    QString m_rootPath;
    int     m_recursionDepth = 0;
    bool    m_markMissing = true;
    MediaScanner::Options m_scannerOptions;
    FinishedCallback m_finished;
};

} // namespace media

#endif // SCANTASK_H
