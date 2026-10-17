#ifndef TASKPANELWIDGET_H
#define TASKPANELWIDGET_H

#include "../tasks/TaskManager.h"

#include <QWidget>
#include <QList>

class QVBoxLayout;
class QLabel;
class QPushButton;
class QTimer;
class QEvent;
class QMouseEvent;

namespace media {

/*!
 * \brief 后台任务面板。
 *
 * 任务书要求"长时间运行的任务应有进度展示与取消入口"。
 *
 * 设计成一个可折叠的下拉面板而不是独立窗口：
 *  - 独立窗口需要用户主动打开，等于把任务状态藏起来，失去意义；
 *  - 状态栏一行文字放不下"多个任务 + 进度条 + 取消按钮"；
 *  - 内嵌在状态栏上方、默认收起，需要时展开，不干扰浏览。
 *
 * 关键行为：没有任务时自动隐藏整个面板（不留一块空白），
 * 有任务时自动展开并显示。用户手动收起后，新任务到来会再次展开——
 * 这样既有提示作用，又不会违背用户当下的意愿太久。
 */
class TaskPanelWidget : public QWidget
{
    Q_OBJECT

public:
    explicit TaskPanelWidget(QWidget *parent = nullptr);
    ~TaskPanelWidget() override;

    void setTaskManager(TaskManager *manager);

    /*! 用最新的任务快照重建面板内容。 */
    void refresh();

    /*! 是否处于展开状态。 */
    bool isExpanded() const { return m_expanded; }
    void setExpanded(bool expanded);

signals:
    /*! 用户请求取消全部任务。 */
    void cancelAllRequested();
    /*! 用户请求取消缩略图生成。 */
    void cancelThumbnailsRequested();
    /*! 展开状态变化，供主窗口保存设置。 */
    void expandedChanged(bool expanded);

protected:
    /*!
     * 拦截头部的鼠标释放事件来实现"点击折叠/展开"。
     *
     * 为什么用事件过滤器而不是把整个 header 做成按钮：
     * header 里已经有"取消全部"这个真按钮，把它再套一层按钮
     * 会导致点击事件归属混乱（点按钮也会触发折叠）。
     * 用过滤器可以精确判断点击位置，跳开按钮区域。
     */
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void onToggleClicked();

private:
    void buildUi();
    QString humanizeDuration(const QDateTime &startedAt) const;

    TaskManager *m_taskManager = nullptr;

    QWidget     *m_header = nullptr;
    QWidget     *m_body = nullptr;
    QLabel      *m_summary = nullptr;
    QLabel      *m_chevron = nullptr;
    QVBoxLayout *m_bodyLayout = nullptr;
    QPushButton *m_cancelAllBtn = nullptr;

    /*!
     * 耗时显示需要每秒刷新一次（"已运行 12 秒"）。
     * 只在面板可见且有任务时启动，避免平时空转。
     */
    QTimer *m_elapsedTimer = nullptr;

    bool m_expanded = false;
    bool m_userCollapsed = false;
    int  m_lastTaskCount = 0;
};

} // namespace media

#endif // TASKPANELWIDGET_H
