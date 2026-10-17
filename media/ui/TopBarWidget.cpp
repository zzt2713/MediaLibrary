#include "TopBarWidget.h"
#include "Metrics.h"
#include "Icons.h"
#include "ThemeManager.h"

#include <QButtonGroup>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>

namespace media {

namespace {

/*! 尺寸滑块的初始值。28 对应缩略图边长约 160px，也就是中等尺寸。 */
constexpr int kDefaultThumbnailSlider = 28;

} // namespace

TopBarWidget::TopBarWidget(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("TopBar"));

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(10, 8, 12, 8);
    layout->setSpacing(8);

    // ---------- 品牌标识 ----------
    auto *brand = new QLabel(QStringLiteral("PICTURE"), this);
    brand->setObjectName(QStringLiteral("AppTitle"));
    layout->addWidget(brand);

    auto *workspace = new QLabel(QStringLiteral("本地媒体资源库"), this);
    workspace->setStyleSheet(
        QStringLiteral("color: %1; font-size: 12px; padding-left: 2px;")
            .arg(ThemeManager::textSecondary().name()));
    layout->addWidget(workspace);

    layout->addSpacing(10);

    // ---------- 添加目录 ----------
    auto *addButton = new QPushButton(this);
    addButton->setProperty("iconButton", true);
    addButton->setIcon(Icons::get(Icons::Add, ThemeManager::textPrimary(), 18));
    addButton->setToolTip(QStringLiteral("添加目录到资源库"));
    addButton->setCursor(Qt::PointingHandCursor);
    connect(addButton, &QPushButton::clicked,
            this, &TopBarWidget::addFolderRequested);
    layout->addWidget(addButton);

    // ---------- 刷新 ----------
    auto *refreshButton = new QPushButton(this);
    refreshButton->setProperty("iconButton", true);
    refreshButton->setIcon(Icons::get(Icons::Refresh, ThemeManager::textPrimary(), 17));
    refreshButton->setToolTip(QStringLiteral("重新扫描当前目录"));
    refreshButton->setCursor(Qt::PointingHandCursor);
    connect(refreshButton, &QPushButton::clicked,
            this, &TopBarWidget::refreshRequested);
    layout->addWidget(refreshButton);

    layout->addSpacing(4);

    // ---------- 搜索 ----------
    m_searchBox = new QLineEdit(this);
    m_searchBox->setObjectName(QStringLiteral("SearchBox"));
    m_searchBox->setPlaceholderText(QStringLiteral("搜索文件名…"));
    m_searchBox->setClearButtonEnabled(true);
    m_searchBox->setFixedWidth(260);
    m_searchBox->setMinimumHeight(Metrics::Control::Button);
    connect(m_searchBox, &QLineEdit::textChanged,
            this, &TopBarWidget::searchTextChanged);
    // 回车提交：触发覆盖全库的查询（逐字输入只筛已加载页，见 MainWindow）
    connect(m_searchBox, &QLineEdit::returnPressed, this, [this]() {
        emit searchSubmitted(m_searchBox->text());
    });
    layout->addWidget(m_searchBox);

    layout->addStretch();

    // ---------- 排序 ----------
    auto *sortLabel = new QLabel(QStringLiteral("排序"), this);
    sortLabel->setStyleSheet(QStringLiteral("color: %1;")
                                 .arg(ThemeManager::textSecondary().name()));
    layout->addWidget(sortLabel);

    m_sortCombo = new QComboBox(this);
    m_sortCombo->addItem(QStringLiteral("修改时间 ↓"), QStringLiteral("mtime_desc"));
    m_sortCombo->addItem(QStringLiteral("修改时间 ↑"), QStringLiteral("mtime_asc"));
    m_sortCombo->addItem(QStringLiteral("名称 A→Z"), QStringLiteral("name_asc"));
    m_sortCombo->addItem(QStringLiteral("名称 Z→A"), QStringLiteral("name_desc"));
    m_sortCombo->addItem(QStringLiteral("大小 ↓"), QStringLiteral("size_desc"));
    m_sortCombo->addItem(QStringLiteral("大小 ↑"), QStringLiteral("size_asc"));
    m_sortCombo->addItem(QStringLiteral("类型"), QStringLiteral("type_asc"));
    m_sortCombo->setFixedWidth(120);
    connect(m_sortCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int index) {
                if (index < 0)
                    return;
                const QString key = m_sortCombo->itemData(index).toString();
                if (!key.isEmpty())
                    emit sortKeyChanged(key);
            });
    layout->addWidget(m_sortCombo);

    layout->addSpacing(6);

    // ---------- 视图模式 ----------
    m_gridButton = new QPushButton(this);
    m_gridButton->setProperty("iconButton", true);
    m_gridButton->setCheckable(true);
    m_gridButton->setChecked(true);
    m_gridButton->setToolTip(QStringLiteral("网格视图"));
    m_gridButton->setCursor(Qt::PointingHandCursor);
    layout->addWidget(m_gridButton);

    m_listButton = new QPushButton(this);
    m_listButton->setProperty("iconButton", true);
    m_listButton->setCheckable(true);
    m_listButton->setToolTip(QStringLiteral("列表视图"));
    m_listButton->setCursor(Qt::PointingHandCursor);
    layout->addWidget(m_listButton);

    // 用 QButtonGroup 保证两者互斥；
    // 直接互连 clicked 会在程序化改状态时产生递归。
    auto *viewGroup = new QButtonGroup(this);
    viewGroup->setExclusive(true);
    viewGroup->addButton(m_gridButton);
    viewGroup->addButton(m_listButton);
    connect(viewGroup, &QButtonGroup::buttonToggled, this,
            [this](QAbstractButton *button, bool checked) {
                if (!checked)
                    return;

                m_viewMode = (button == m_gridButton)
                                 ? ThumbnailGrid::GridMode
                                 : ThumbnailGrid::ListMode;
                updateViewModeIcons();
                emit viewModeChanged(m_viewMode == ThumbnailGrid::GridMode);
            });

    updateViewModeIcons();

    layout->addSpacing(6);

    // ---------- 缩略图尺寸 ----------
    m_sizeSlider = new QSlider(Qt::Horizontal, this);
    m_sizeSlider->setRange(0, 100);
    m_sizeSlider->setValue(kDefaultThumbnailSlider);
    m_sizeSlider->setFixedWidth(90);
    m_sizeSlider->setToolTip(QStringLiteral("缩略图大小"));
    connect(m_sizeSlider, &QSlider::valueChanged, this, [this](int value) {
        updateThumbnailSizeLabel();
        emit thumbnailSizeChanged(value);
    });
    layout->addWidget(m_sizeSlider);

    m_sizeLabel = new QLabel(this);
    m_sizeLabel->setStyleSheet(QStringLiteral("color: %1;")
                                   .arg(ThemeManager::textSecondary().name()));
    m_sizeLabel->setFixedWidth(30);
    layout->addWidget(m_sizeLabel);

    updateThumbnailSizeLabel();

    layout->addSpacing(6);

    // ---------- 设置 ----------
    auto *settingsButton = new QPushButton(this);
    settingsButton->setProperty("iconButton", true);
    settingsButton->setIcon(Icons::get(Icons::Settings, ThemeManager::textSecondary(), 17));
    settingsButton->setToolTip(QStringLiteral("设置"));
    settingsButton->setCursor(Qt::PointingHandCursor);
    connect(settingsButton, &QPushButton::clicked,
            this, &TopBarWidget::settingsRequested);
    layout->addWidget(settingsButton);
}

void TopBarWidget::setSearchText(const QString &text)
{
    if (m_searchBox->text() == text)
        return;

    // 屏蔽信号：这是"把界面状态同步到目标值"，不是用户输入。
    // 不屏蔽会发出 searchTextChanged，让 MainWindow 的槽再跑一遍，
    // 在 applySmartGroup 清空搜索框的场景下形成无谓往返。
    const QSignalBlocker blocker(m_searchBox);
    m_searchBox->setText(text);
}

QString TopBarWidget::searchText() const
{
    return m_searchBox->text();
}

void TopBarWidget::setSortKey(const QString &key)
{
    for (int i = 0; i < m_sortCombo->count(); ++i) {
        if (m_sortCombo->itemData(i).toString() != key)
            continue;

        // 与当前一致时什么都不做：setCurrentIndex 到同一个位置不发信号，
        // 但显式判断能让"为什么这里要判断"这件事在代码里看得见。
        if (m_sortCombo->currentIndex() != i)
            m_sortCombo->setCurrentIndex(i);   // 触发 sortKeyChanged
        return;
    }
}

void TopBarWidget::setViewMode(ThumbnailGrid::ViewMode mode)
{
    if (m_viewMode == mode)
        return;

    m_viewMode = mode;

    // 屏蔽信号：这里是"把界面状态同步到目标值"，
    // 不屏蔽的话 setChecked 会触发 buttonToggled，进而再次走一遍这里。
    {
        const QSignalBlocker gridBlocker(m_gridButton);
        const QSignalBlocker listBlocker(m_listButton);
        m_gridButton->setChecked(mode == ThumbnailGrid::GridMode);
        m_listButton->setChecked(mode == ThumbnailGrid::ListMode);
    }

    updateViewModeIcons();
    emit viewModeChanged(mode == ThumbnailGrid::GridMode);
}

int TopBarWidget::thumbnailSliderValue() const
{
    return m_sizeSlider->value();
}

void TopBarWidget::setThumbnailSliderValue(int value)
{
    // setValue 到同一个值不发信号，这里不需要额外判断
    m_sizeSlider->setValue(value);
}

void TopBarWidget::updateViewModeIcons()
{
    const bool gridMode = (m_viewMode == ThumbnailGrid::GridMode);

    // 图标颜色跟随选中态，让当前模式一眼可辨
    m_gridButton->setIcon(Icons::get(Icons::GridView,
        gridMode ? ThemeManager::textPrimary() : ThemeManager::textSecondary(), 17));
    m_listButton->setIcon(Icons::get(Icons::ListView,
        gridMode ? ThemeManager::textSecondary() : ThemeManager::textPrimary(), 17));
}

void TopBarWidget::updateThumbnailSizeLabel()
{
    // 像素值向 ThumbnailGrid 要，不在这里再写一遍映射公式：
    // 两处各存一份的话，一旦调了映射范围就会显示错误数字。
    m_sizeLabel->setText(
        QString::number(ThumbnailGrid::edgeFromSlider(m_sizeSlider->value())));
}

} // namespace media
