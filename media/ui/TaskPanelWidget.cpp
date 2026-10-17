#include "TaskPanelWidget.h"
#include "Icons.h"
#include "ThemeManager.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QProgressBar>
#include <QTimer>
#include <QFrame>
#include <QDateTime>
#include <QEvent>
#include <QMouseEvent>
#include <QTransform>
#include <QPixmap>
#include <QDebug>

namespace media {

namespace {

/*! 面板高度上限。任务很多时不能让它把内容区挤没。 */
constexpr int kMaxBodyHeight = 180;

/*! 耗时文字的刷新间隔。 */
constexpr int kElapsedRefreshMs = 1000;

} // namespace

TaskPanelWidget::TaskPanelWidget(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("TaskPanel"));
    buildUi();

    m_elapsedTimer = new QTimer(this);
    m_elapsedTimer->setInterval(kElapsedRefreshMs);
    connect(m_elapsedTimer, &QTimer::timeout, this, [this]() {
        // 每秒刷新一次耗时文字。
        // 这里会重建明细行——因为耗时是文本的一部分。
        // 重建代价很小（通常只有 1~2 行），换来的是实现简单、不会出现
        // "标签更新了但布局没跟上"的问题。
        if (isVisible())
            refresh();
    });

    // 初始无任务：整个面板隐藏，不占位置
    setVisible(false);
}

TaskPanelWidget::~TaskPanelWidget() = default;

void TaskPanelWidget::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ---------- 头部（折叠时也可见） ----------
    m_header = new QWidget(this);
    m_header->setObjectName(QStringLiteral("TopBar"));
    m_header->setCursor(Qt::PointingHandCursor);
    m_header->setToolTip(QStringLiteral("点击展开或收起任务明细"));

    auto *headerLayout = new QHBoxLayout(m_header);
    headerLayout->setContentsMargins(12, 6, 12, 6);
    headerLayout->setSpacing(8);

    m_chevron = new QLabel(m_header);
    m_chevron->setPixmap(Icons::pixmap(Icons::ChevronRight,
                                       ThemeManager::textSecondary(), 14));
    m_chevron->setFixedSize(16, 16);
    // 箭头只是个指示器，不该拦截点击——否则点在箭头上无法折叠
    m_chevron->setAttribute(Qt::WA_TransparentForMouseEvents);
    headerLayout->addWidget(m_chevron);

    m_summary = new QLabel(m_header);
    m_summary->setStyleSheet(
        QStringLiteral("color: %1; font-size: 12px;")
            .arg(ThemeManager::textSecondary().name()));
    m_summary->setAttribute(Qt::WA_TransparentForMouseEvents);
    headerLayout->addWidget(m_summary, 1);

    m_cancelAllBtn = new QPushButton(
        Icons::get(Icons::Close, ThemeManager::dangerColor(), 14),
        QStringLiteral("取消全部"), m_header);
    m_cancelAllBtn->setProperty("danger", true);
    m_cancelAllBtn->setCursor(Qt::PointingHandCursor);
    m_cancelAllBtn->setToolTip(QStringLiteral("取消所有正在进行的后台任务"));
    connect(m_cancelAllBtn, &QPushButton::clicked,
            this, &TaskPanelWidget::cancelAllRequested);
    headerLayout->addWidget(m_cancelAllBtn);

    root->addWidget(m_header);

    // ---------- 主体（可折叠） ----------
    m_body = new QWidget(this);
    m_body->setObjectName(QStringLiteral("TaskPanelBody"));
    m_body->setMaximumHeight(kMaxBodyHeight);

    m_bodyLayout = new QVBoxLayout(m_body);
    m_bodyLayout->setContentsMargins(14, 8, 14, 10);
    m_bodyLayout->setSpacing(6);

    root->addWidget(m_body);

    // 头部整体可点，用于折叠/展开
    m_header->installEventFilter(this);
}

bool TaskPanelWidget::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_header && event->type() == QEvent::MouseButtonRelease) {
        auto *me = static_cast<QMouseEvent *>(event);

        /*!
         * 点在"取消全部"按钮上的情况要放行给按钮处理。
         *
         * 用几何位置判断而不是比较 target 对象：事件过滤器在
         * 子控件之前收到事件，此时无法从 watched 里区分是按钮还是空白。
         * 用 mapTo 把坐标换到按钮的坐标系再判断最直接。
         */
        if (m_cancelAllBtn) {
            const QPoint btnPos = m_cancelAllBtn->mapFrom(m_header, me->pos());
            if (m_cancelAllBtn->rect().contains(btnPos))
                return false;
        }

        if (me->button() != Qt::LeftButton)
            return false;

        onToggleClicked();
        return true;
    }

    return QWidget::eventFilter(watched, event);
}

void TaskPanelWidget::setTaskManager(TaskManager *manager)
{
    if (m_taskManager == manager)
        return;

    if (m_taskManager)
        disconnect(m_taskManager, nullptr, this, nullptr);

    m_taskManager = manager;

    if (m_taskManager) {
        connect(m_taskManager, &TaskManager::tasksChanged,
                this, &TaskPanelWidget::refresh);
    }

    refresh();
}

void TaskPanelWidget::setExpanded(bool expanded)
{
    if (m_expanded == expanded)
        return;

    m_expanded = expanded;

    m_body->setVisible(expanded);

    /*!
     * 旋转 90° 得到"向下"的箭头。
     *
     * 之所以不单独加一个 ChevronDown 图标：为了一个小箭头
     * 多维护一份绘制代码不划算，而 QPixmap::transformed 在
     * 14px 的尺寸下质量完全够用。
     */
    QPixmap arrow = Icons::pixmap(Icons::ChevronRight, ThemeManager::textSecondary(), 14);
    if (expanded) {
        QTransform rotate;
        rotate.rotate(90);
        arrow = arrow.transformed(rotate, Qt::SmoothTransformation);
    }
    m_chevron->setPixmap(arrow);

    emit expandedChanged(expanded);
}

void TaskPanelWidget::onToggleClicked()
{
    setExpanded(!m_expanded);

    // 用户主动收起后，这批任务期间不再自动展开，避免反复打断他
    if (!m_expanded)
        m_userCollapsed = true;
}

QString TaskPanelWidget::humanizeDuration(const QDateTime &startedAt) const
{
    if (!startedAt.isValid())
        return QString();

    const qint64 secs = startedAt.secsTo(QDateTime::currentDateTime());

    // 负数说明系统时钟被回拨了，或者时间戳异常。显示"已运行 -3 秒"很怪，
    // 直接不显示耗时。
    if (secs < 0)
        return QString();

    if (secs < 60)
        return QStringLiteral("%1 秒").arg(secs);

    return QStringLiteral("%1 分 %2 秒").arg(secs / 60).arg(secs % 60);
}

void TaskPanelWidget::refresh()
{
    if (!m_taskManager) {
        setVisible(false);
        return;
    }

    const QList<TaskManager::TaskStatus> tasks = m_taskManager->currentTasks();

    /*!
     * 只有缩略图任务（没有扫描）时完全不显示面板。
     *
     * 缩略图是浏览时随手触发的短任务——点一下图片、滚动一下网格就会排一批。
     * 这种任务也弹面板的话，每次点击都会把底部顶出来一截，反而打断浏览。
     * currentTasks() 只有扫描与缩略图两类，所以"没有扫描"即"纯缩略图"。
     * 扫描（长时间、用户主动发起）仍然正常显示面板与进度。
     */
    if (tasks.isEmpty() || !m_taskManager->isScanningAny()) {
        setVisible(false);
        m_elapsedTimer->stop();
        m_lastTaskCount = 0;
        m_userCollapsed = false;   // 下一批任务到来时重新自动展开
        return;
    }

    setVisible(true);

    if (!m_elapsedTimer->isActive())
        m_elapsedTimer->start();

    // 任务从无到有：自动展开，让用户知道后台在做什么。
    // 但如果用户刚刚手动收起过，就尊重他的选择。
    const bool newlyStarted = (m_lastTaskCount == 0);
    if (newlyStarted && !m_userCollapsed)
        setExpanded(true);

    m_lastTaskCount = tasks.size();

    // ---------- 摘要行 ----------
    QString summary;
    if (tasks.size() == 1) {
        summary = tasks.first().label;
        if (!tasks.first().detail.isEmpty())
            summary += QStringLiteral(" · ") + tasks.first().detail;
    } else {
        summary = QStringLiteral("%1 个后台任务进行中").arg(tasks.size());
    }
    m_summary->setText(summary);

    // ---------- 明细 ----------
    // 清空旧的明细行。cancelAllBtn 在 header 里，不在此布局中，不受影响。
    while (QLayoutItem *item = m_bodyLayout->takeAt(0)) {
        if (QWidget *w = item->widget())
            w->deleteLater();
        delete item;
    }

    for (const TaskManager::TaskStatus &task : tasks) {
        auto *row = new QWidget(m_body);
        auto *rowLayout = new QVBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(3);

        auto *titleRow = new QHBoxLayout();
        titleRow->setSpacing(8);

        auto *title = new QLabel(task.label, row);
        title->setStyleSheet(
            QStringLiteral("color: %1; font-size: 12px;")
                .arg(ThemeManager::textPrimary().name()));
        titleRow->addWidget(title, 1);

        const QString elapsed = humanizeDuration(task.startedAt);
        QString detailText = task.detail;
        if (!elapsed.isEmpty()) {
            detailText = detailText.isEmpty()
                             ? QStringLiteral("已运行 %1").arg(elapsed)
                             : QStringLiteral("%1 · 已运行 %2").arg(detailText, elapsed);
        }

        auto *detail = new QLabel(detailText, row);
        detail->setStyleSheet(
            QStringLiteral("color: %1; font-size: 11px;")
                .arg(ThemeManager::textDisabled().name()));
        titleRow->addWidget(detail);

        rowLayout->addLayout(titleRow);

        // 进度条：progress >= 0 显示确定进度，否则用忙碌态
        auto *bar = new QProgressBar(row);
        bar->setTextVisible(false);
        bar->setFixedHeight(5);
        if (task.progress >= 0) {
            bar->setRange(0, 100);
            bar->setValue(task.progress);
        } else {
            // Qt 里 range(0, 0) 表示"不确定进度"的忙碌动画
            bar->setRange(0, 0);
        }
        rowLayout->addWidget(bar);

        m_bodyLayout->addWidget(row);
    }

    /*!
     * 缩略图与扫描的取消语义不同，所以给出单独的入口。
     *
     * "取消全部"在头部，用于"我要停下来"；
     * 而"仅取消缩略图生成"是常见诉求——用户想继续扫描，
     * 只是不想让后台一直忙着生成缩略图（比如在省电、或在做别的事）。
     */
    const bool hasThumbTasks = m_taskManager->pendingThumbnailCount() > 0;
    const bool hasScanTasks = m_taskManager->isScanningAny();

    if (hasThumbTasks && hasScanTasks) {
        auto *thumbBtn = new QPushButton(
            Icons::get(Icons::Close, ThemeManager::textSecondary(), 13),
            QStringLiteral("仅取消缩略图生成"), m_body);
        thumbBtn->setFlat(true);
        thumbBtn->setCursor(Qt::PointingHandCursor);
        thumbBtn->setToolTip(QStringLiteral("扫描会继续进行，只停掉缩略图生成"));
        connect(thumbBtn, &QPushButton::clicked,
                this, &TaskPanelWidget::cancelThumbnailsRequested);
        m_bodyLayout->addWidget(thumbBtn, 0, Qt::AlignLeft);
    }

    m_bodyLayout->addStretch();
}

} // namespace media
