#include "EmptyStateWidget.h"
#include "Metrics.h"
#include "ThemeManager.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace media {

EmptyStateWidget::EmptyStateWidget(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setAlignment(Qt::AlignCenter);
    layout->setSpacing(12);

    m_title = new QLabel(QStringLiteral("资源库还是空的"), this);
    m_title->setAlignment(Qt::AlignCenter);
    // Font::Title：与 HomePage 的空状态标题统一（原来是 18px，两处不一致）
    m_title->setStyleSheet(
        QStringLiteral("color: %1; font-size: %2px; font-weight: 600;")
            .arg(ThemeManager::textPrimary().name())
            .arg(Metrics::Font::Title));
    layout->addWidget(m_title);

    m_hint = new QLabel(this);
    m_hint->setAlignment(Qt::AlignCenter);
    // 注意：Qt 的样式表不支持 line-height，行距靠文本里的 \n 控制
    m_hint->setStyleSheet(
        QStringLiteral("color: %1; font-size: 13px;")
            .arg(ThemeManager::textSecondary().name()));
    layout->addWidget(m_hint);

    auto *addButton = new QPushButton(QStringLiteral("添加目录"), this);
    addButton->setCursor(Qt::PointingHandCursor);
    addButton->setFixedWidth(120);
    connect(addButton, &QPushButton::clicked,
            this, &EmptyStateWidget::addFolderRequested);

    auto *buttonRow = new QHBoxLayout();
    buttonRow->addStretch();
    buttonRow->addWidget(addButton);
    buttonRow->addStretch();
    layout->addLayout(buttonRow);

    // 构造时先按初始文案填好，避免显示出一块空白
    setReason(NoFolder);
}

void EmptyStateWidget::setReason(Reason reason)
{
    switch (reason) {
    case NoFolder:
        m_title->setText(QStringLiteral("资源库还是空的"));
        m_hint->setText(QStringLiteral(
            "点击左上角的 + 添加一个目录，或者直接把文件夹拖进窗口\n"
            "支持图片、视频、音频，会自动递归扫描并建立索引"));
        break;

    case Indexing:
        m_title->setText(QStringLiteral("正在建立索引…"));
        m_hint->setText(QStringLiteral("首次扫描需要一些时间，完成后会自动显示"));
        break;

    case FilteredEmpty:
        m_title->setText(QStringLiteral("当前条件下没有资源"));
        m_hint->setText(QStringLiteral(
            "试试清空搜索框、切换到「全部资源」分组，\n"
            "或者点击刷新重新扫描目录"));
        break;
    }
}

} // namespace media
