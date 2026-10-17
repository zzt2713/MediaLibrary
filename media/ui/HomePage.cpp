#include "HomePage.h"
#include "Metrics.h"
#include "Icons.h"
#include "ThemeManager.h"
#include "../core/MediaDatabase.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QPushButton>
#include <QFrame>
#include <QTimer>
#include <QDir>
#include <QFileInfo>
#include <QDateTime>
#include <QLocale>
#include <QDebug>
#include <QSizePolicy>

namespace media {

namespace {

/*! 首页各区块最多展示多少条，避免首页变成第二个资源列表。 */
constexpr int kMaxRecentFolders = 5;
constexpr int kMaxRecentItems = 6;
constexpr int kMaxFavorites = 6;

/*! 把时间戳转成"3 分钟前"这类相对描述。 */
QString humanizeTime(const QDateTime &dt)
{
    if (!dt.isValid())
        return QStringLiteral("--");

    const QDateTime now = QDateTime::currentDateTime();
    const qint64 secs = dt.secsTo(now);

    // 未来时间（时钟回拨、文件时间戳异常）直接显示绝对时间，
    // 显示成"负几分钟前"会很怪。
    if (secs < 0)
        return dt.toString(QStringLiteral("yyyy-MM-dd HH:mm"));

    if (secs < 60)
        return QStringLiteral("刚刚");
    if (secs < 3600)
        return QStringLiteral("%1 分钟前").arg(secs / 60);
    if (secs < 86400)
        return QStringLiteral("%1 小时前").arg(secs / 3600);
    if (secs < 86400 * 7)
        return QStringLiteral("%1 天前").arg(secs / 86400);

    return dt.toString(QStringLiteral("yyyy-MM-dd"));
}

/*! 根据扩展名给出类型图标名。 */
QString iconForType(MediaType type)
{
    switch (type) {
    case MediaType::Image: return Icons::Image;
    case MediaType::Video: return Icons::Video;
    case MediaType::Audio: return Icons::Audio;
    default:               return Icons::Info;
    }
}

/*!
 * 让一个卡片的高度只由内容决定，不被父布局纵向拉伸。
 *
 * 首页滚动区设了 setWidgetResizable(true)，m_content 的高度至少等于视口。
 * 纵向布局默认把富余高度按各控件的垂直伸展因子分配，结果就是这几块
 * "最近访问 / 最近浏览 / 收藏"卡片被拉高，内容却仍贴在卡片顶部，
 * 下面留一大片空白（截图里红箭头指的三处）。
 *
 * 把垂直策略设成 Fixed / Maximum 后，卡片只占自身内容的高度，
 * 富余空间交给 m_content 布局末尾的 addStretch() 统一吸收。
 */
void keepContentHeight(QWidget *card)
{
    if (!card)
        return;
    QSizePolicy sp = card->sizePolicy();
    sp.setVerticalPolicy(QSizePolicy::Fixed);
    sp.setRetainSizeWhenHidden(true);
    card->setSizePolicy(sp);
}

} // namespace

HomePage::HomePage(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("HomePage"));
    buildUi();

    // 先按"无数据库"渲染一次空状态，避免初始显示一堆空卡片。
    // setDatabase() 接上数据后会再 refresh() 一次。
    refresh();
}

HomePage::~HomePage() = default;

void HomePage::clearLayout(QLayout *layout)
{
    if (!layout)
        return;

    while (QLayoutItem *item = layout->takeAt(0)) {
        // 嵌套布局要递归清理，否则里面的 widget 不会被销毁
        if (QLayout *childLayout = item->layout())
            clearLayout(childLayout);

        if (QWidget *widget = item->widget())
            widget->deleteLater();

        delete item;
    }
}

void HomePage::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    m_scroll = new QScrollArea(this);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    m_content = new QWidget();
    auto *contentLayout = new QVBoxLayout(m_content);
    contentLayout->setContentsMargins(24, 20, 24, 24);
    contentLayout->setSpacing(18);

    // ---------- 欢迎语 ----------
    auto *welcome = new QLabel(QStringLiteral("资源库概览"), m_content);
    welcome->setStyleSheet(
        QStringLiteral("color: %1; font-size: 20px; font-weight: 600;")
            .arg(ThemeManager::textPrimary().name()));
    contentLayout->addWidget(welcome);

    // ---------- 统计卡片行 ----------
    m_statsRow = new QWidget(m_content);
    m_statsLayout = new QHBoxLayout(m_statsRow);
    m_statsLayout->setContentsMargins(0, 0, 0, 0);
    m_statsLayout->setSpacing(12);
    contentLayout->addWidget(m_statsRow);
    keepContentHeight(m_statsRow);

    // ---------- 最近目录 ----------
    m_recentFoldersCard = new QWidget(m_content);
    auto *folderCardLayout = new QVBoxLayout(m_recentFoldersCard);
    folderCardLayout->setContentsMargins(0, 0, 0, 0);
    folderCardLayout->setSpacing(8);
    folderCardLayout->addWidget(makeSectionHeader(QStringLiteral("最近访问的目录")));

    auto *folderInner = new QFrame(m_recentFoldersCard);
    folderInner->setProperty("card", true);
    m_recentFoldersLayout = new QVBoxLayout(folderInner);
    m_recentFoldersLayout->setContentsMargins(0, 0, 0, 0);
    m_recentFoldersLayout->setSpacing(2);
    folderCardLayout->addWidget(folderInner);
    contentLayout->addWidget(m_recentFoldersCard);
    keepContentHeight(m_recentFoldersCard);

    // ---------- 最近浏览 ----------
    m_recentItemsCard = new QWidget(m_content);
    auto *itemCardLayout = new QVBoxLayout(m_recentItemsCard);
    itemCardLayout->setContentsMargins(0, 0, 0, 0);
    itemCardLayout->setSpacing(8);

    auto *itemHeader = makeSectionHeader(QStringLiteral("最近浏览"), QStringLiteral("查看全部"));
    if (auto *btn = itemHeader->findChild<QPushButton *>()) {
        connect(btn, &QPushButton::clicked, this, &HomePage::recentPlaybackRequested);
    }
    itemCardLayout->addWidget(itemHeader);

    auto *itemInner = new QFrame(m_recentItemsCard);
    itemInner->setProperty("card", true);
    m_recentItemsLayout = new QVBoxLayout(itemInner);
    m_recentItemsLayout->setContentsMargins(0, 0, 0, 0);
    m_recentItemsLayout->setSpacing(2);
    itemCardLayout->addWidget(itemInner);
    contentLayout->addWidget(m_recentItemsCard);
    keepContentHeight(m_recentItemsCard);

    // ---------- 收藏 ----------
    m_favoritesCard = new QWidget(m_content);
    auto *favCardLayout = new QVBoxLayout(m_favoritesCard);
    favCardLayout->setContentsMargins(0, 0, 0, 0);
    favCardLayout->setSpacing(8);

    auto *favHeader = makeSectionHeader(QStringLiteral("收藏"), QStringLiteral("查看全部"));
    if (auto *btn = favHeader->findChild<QPushButton *>()) {
        connect(btn, &QPushButton::clicked, this, [this]() {
            emit smartGroupRequested(QStringLiteral("favorites"));
        });
    }
    favCardLayout->addWidget(favHeader);

    auto *favInner = new QFrame(m_favoritesCard);
    favInner->setProperty("card", true);
    m_favoritesLayout = new QVBoxLayout(favInner);
    m_favoritesLayout->setContentsMargins(0, 0, 0, 0);
    m_favoritesLayout->setSpacing(2);
    favCardLayout->addWidget(favInner);
    contentLayout->addWidget(m_favoritesCard);
    keepContentHeight(m_favoritesCard);

    // ---------- 空状态 ----------
    m_emptyPage = new QWidget(m_content);
    auto *emptyLayout = new QVBoxLayout(m_emptyPage);
    emptyLayout->setContentsMargins(0, 30, 0, 30);
    emptyLayout->setSpacing(14);
    emptyLayout->setAlignment(Qt::AlignCenter);

    auto *emptyIcon = new QLabel(m_emptyPage);
    emptyIcon->setPixmap(Icons::pixmap(Icons::Image, ThemeManager::textDisabled(), 56));
    emptyIcon->setAlignment(Qt::AlignCenter);
    emptyLayout->addWidget(emptyIcon);

    auto *emptyTitle = new QLabel(QStringLiteral("资源库还是空的"), m_emptyPage);
    emptyTitle->setAlignment(Qt::AlignCenter);
    // Font::Title：与 EmptyStateWidget 的空状态标题统一（原来是 17px，两处不一致）
    emptyTitle->setStyleSheet(
        QStringLiteral("color: %1; font-size: %2px; font-weight: 600;")
            .arg(ThemeManager::textPrimary().name())
            .arg(Metrics::Font::Title));
    emptyLayout->addWidget(emptyTitle);

    m_emptyHint = new QLabel(
        QStringLiteral("添加一个目录，把本地的图片、视频和音频收进来。\n"
                       "扫描会在后台进行，不会影响你操作界面。"),
        m_emptyPage);
    m_emptyHint->setAlignment(Qt::AlignCenter);
    m_emptyHint->setStyleSheet(
        QStringLiteral("color: %1; font-size: 13px;")
            .arg(ThemeManager::textSecondary().name()));
    emptyLayout->addWidget(m_emptyHint);

    auto *emptyBtnRow = new QHBoxLayout();
    emptyBtnRow->addStretch();

    auto *addBtn = new QPushButton(
        Icons::get(Icons::Add, ThemeManager::textPrimary(), 17),
        QStringLiteral("添加目录"), m_emptyPage);
    addBtn->setCursor(Qt::PointingHandCursor);
    addBtn->setMinimumHeight(34);
    addBtn->setMinimumWidth(130);
    connect(addBtn, &QPushButton::clicked, this, &HomePage::addFolderRequested);
    emptyBtnRow->addWidget(addBtn);

    emptyBtnRow->addStretch();
    emptyLayout->addLayout(emptyBtnRow);

    contentLayout->addWidget(m_emptyPage);
    contentLayout->addStretch();

    m_scroll->setWidget(m_content);
    root->addWidget(m_scroll, 1);
}

QWidget *HomePage::makeSectionHeader(const QString &title, const QString &actionText)
{
    auto *header = new QWidget(this);
    auto *layout = new QHBoxLayout(header);
    layout->setContentsMargins(2, 0, 2, 0);
    layout->setSpacing(8);

    auto *label = new QLabel(title, header);
    label->setStyleSheet(
        QStringLiteral("color: %1; font-size: 14px; font-weight: 600;")
            .arg(ThemeManager::textPrimary().name()));
    layout->addWidget(label);
    layout->addStretch();

    if (!actionText.isEmpty()) {
        auto *action = new QPushButton(actionText, header);
        // 用扁平样式：它是次要入口，不该和主按钮抢注意力
        // background 各状态都要显式写：否则 :hover / :pressed / :checked
        // 会从全局 QPushButton 规则里继承 %SURFACE_HOVER% /
        // %SURFACE_PRESSED% / %ACCENT_SUBTLE%，按钮上出现色块
        action->setFlat(true);
        action->setCursor(Qt::PointingHandCursor);
        action->setStyleSheet(
            QStringLiteral("QPushButton { color: %1; border: none;"
                           " background: transparent; padding: 2px 6px; }"
                           "QPushButton:hover { color: %2; background: transparent; }"
                           "QPushButton:pressed { background: transparent; }"
                           "QPushButton:checked { background: transparent; }")
                .arg(ThemeManager::textSecondary().name(),
                     ThemeManager::accent().name()));
        layout->addWidget(action);
    }

    return header;
}

QWidget *HomePage::makeStatCard(const QString &title, const QString &value,
                                const QString &caption, const QString &accentColorHex)
{
    auto *card = new QFrame(this);
    card->setProperty("stat", true);
    /*!
     * 统计卡不再用整块底色 + 描边。
     *
     * 之前是 panelBackground 底 + 1px 边 + 10px 圆角的一整块，四个并排
     * 就是四个灰框，和上面的区块卡片叠在一起让首页显得很"重"。
     * 现在只保留一条 3px 的左侧色条，颜色即该项的类型强调色，
     * 数字本身已经够大，不需要再用底色把它圈起来。
     * 左边距给足，避免色条贴上文字。
     */
    card->setStyleSheet(
        QStringLiteral("QFrame[stat=\"true\"] {"
                       "  background: transparent;"
                       "  border: none;"
                       "  border-left: 3px solid %1;"
                       "  border-top-left-radius: 0px;"
                       "  border-bottom-left-radius: 0px;"
                       "  padding-left: 10px;"
                       "}").arg(accentColorHex));
    card->setMinimumHeight(86);
    // 同时给一个最大高度：横向布局在多张卡片平均分配高度时，会把矮的卡片
    // 拉到和最高的一致，标题/数值/说明被推到卡片顶部、下方留白。
    // 统计卡片本身内容固定（三行文字），不需要随窗口变高，锁死上限。
    card->setMaximumHeight(86);

    auto *layout = new QVBoxLayout(card);
    // 左侧 2px：色条与文字的间距已由样式表的 padding-left: 10px 提供
    layout->setContentsMargins(2, 12, 10, 12);
    layout->setSpacing(2);

    auto *titleLabel = new QLabel(title, card);
    titleLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 12px;")
            .arg(ThemeManager::textSecondary().name()));
    layout->addWidget(titleLabel);

    auto *valueLabel = new QLabel(value, card);
    valueLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 24px; font-weight: 600;")
            .arg(accentColorHex));
    layout->addWidget(valueLabel);

    auto *captionLabel = new QLabel(caption, card);
    captionLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 11px;")
            .arg(ThemeManager::textDisabled().name()));
    layout->addWidget(captionLabel);

    /*!
     * 不再加 addStretch()。
     *
     * 之前底部有一条弹簧把三行文字顶到卡片上半部分，卡片一旦被横向布局
     * 按最高那张拉高，下半部分就是空白（截图里"收藏"卡片最明显）。
     * 现在高度锁死为 86，内容自然填满，不需要弹簧撑。
     */
    return card;
}

QPushButton *HomePage::makeListRow(const QString &iconName, const QString &title,
                               const QString &subtitle, const QString &tooltip)
{
    auto *row = new QPushButton(this);
    row->setFlat(true);
    row->setCursor(Qt::PointingHandCursor);
    row->setToolTip(tooltip);
    /*!
     * 内联样式必须把 :hover / :pressed / :disabled / :checked 都覆盖一遍。
     *
     * 全局 style.qss 里 `QPushButton:hover { border: 1px solid ... }` 的
     * 选择器权重高于按钮自己内联的裸 `QPushButton { border: none; }`。
     * 于是鼠标悬停到哪一行、哪一行就冒出一个深色边框框住文字——
     * 列表行本是无边框的整行点击区，出现描边会很难看。
     * 所以这里逐个伪状态显式关掉 border，压过全局规则。
     *
     * background 同理，而且必须每个状态都写：
     * QSS 是"按状态匹配、逐条合并"的，正常态写了 transparent 只压制
     * 全局的 `QPushButton { background-color: %SURFACE% }`，
     * 点下去时状态变成 :pressed / :checked，全局里对应状态
     * 的 `background-color: %SURFACE_PRESSED% }` / `%ACCENT_SUBTLE% }`
     * 会因为本行没有覆盖而生效——结果就是点一下冒出一块灰/蓝色底，
     * 把文字和图标都盖在底色上。
     * 所以这里四个状态全部显式声明 background-color。
     */
    row->setStyleSheet(
        QStringLiteral(
            "QPushButton { text-align: left; border: none;"
            " background-color: transparent;"
            " border-radius: 7px; padding: 6px 8px; }"
            "QPushButton:hover { background-color: %1; border: none; }"
            "QPushButton:pressed { border: none; background-color: %1; }"
            "QPushButton:checked { border: none; background-color: transparent; }"
            "QPushButton:disabled { border: none; background-color: transparent; }")
            .arg(ThemeManager::surfaceHover().name()));

    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(4, 2, 4, 2);
    layout->setSpacing(10);

    auto *icon = new QLabel(row);
    icon->setPixmap(Icons::pixmap(iconName, ThemeManager::textSecondary(), 17));
    icon->setFixedSize(20, 20);
    // 图标本身不该吃掉点击，否则点在图标的像素上按钮收不到事件
    icon->setAttribute(Qt::WA_TransparentForMouseEvents);
    layout->addWidget(icon);

    auto *textWrap = new QWidget(row);
    textWrap->setAttribute(Qt::WA_TransparentForMouseEvents);
    auto *textLayout = new QVBoxLayout(textWrap);
    textLayout->setContentsMargins(0, 0, 0, 0);
    textLayout->setSpacing(0);

    auto *titleLabel = new QLabel(title, textWrap);
    titleLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 13px;")
            .arg(ThemeManager::textPrimary().name()));
    // 文件名过长时省略，完整名称靠 tooltip
    titleLabel->setTextFormat(Qt::PlainText);
    textLayout->addWidget(titleLabel);

    if (!subtitle.isEmpty()) {
        auto *subLabel = new QLabel(subtitle, textWrap);
        subLabel->setStyleSheet(
            QStringLiteral("color: %1; font-size: 11px;")
                .arg(ThemeManager::textDisabled().name()));
        textLayout->addWidget(subLabel);
    }

    layout->addWidget(textWrap, 1);

    return row;
}

void HomePage::setDatabase(MediaDatabase *database)
{
    if (m_database == database)
        return;

    if (m_database)
        disconnect(m_database, nullptr, this, nullptr);

    m_database = database;

    if (m_database) {
        /*!
         * 库变化时不立刻重建首页。
         *
         * 扫描时 libraryChanged 每 64 个文件触发一次，
         * 而重建首页要跑好几个聚合查询 + 创建一批控件。
         * 若每次都重建，扫描期间首页会持续闪烁且明显变慢。
         *
         * 这里的策略是：标脏，等用户切回首页时再刷新。
         * 首页不是高频交互的页面，"切回来时是最新的"已经足够。
         */
        connect(m_database, &MediaDatabase::libraryChanged, this, [this]() {
            m_needsRefresh = true;
        });
    }

    refresh();
}

void HomePage::refreshIfNeeded()
{
    if (m_needsRefresh)
        refresh();
}

void HomePage::refresh()
{
    m_needsRefresh = false;

    if (!m_database || !m_database->isOpen()) {
        m_statsRow->setVisible(false);
        m_recentFoldersCard->setVisible(false);
        m_recentItemsCard->setVisible(false);
        m_favoritesCard->setVisible(false);
        m_emptyPage->setVisible(true);
        m_emptyHint->setText(QStringLiteral("资源库数据库不可用。"));
        return;
    }

    const MediaDatabase::Stats stats = m_database->stats();
    const bool hasContent = stats.total > 0;

    // 空库：只显示引导，不显示一堆 "0" 的统计卡片
    m_statsRow->setVisible(hasContent);
    m_recentFoldersCard->setVisible(hasContent);
    m_recentItemsCard->setVisible(hasContent);
    m_favoritesCard->setVisible(hasContent);
    m_emptyPage->setVisible(!hasContent);

    if (!hasContent) {
        const QList<QString> folders = m_database->allFolderPaths();
        if (folders.isEmpty()) {
            m_emptyHint->setText(QStringLiteral(
                "添加一个目录，把本地的图片、视频和音频收进来。\n"
                "扫描会在后台进行，不会影响你操作界面。"));
        } else {
            // 有登记目录但没有资源：说明还没扫过，或目录里确实没有媒体文件
            m_emptyHint->setText(QStringLiteral(
                "已登记 %1 个目录，但还没有索引到任何媒体文件。\n"
                "点右上角的刷新按钮开始扫描，或者换一个目录。")
                    .arg(folders.size()));
        }
        return;
    }

    rebuildStats();
    rebuildRecentFolders();
    rebuildRecentItems();
    rebuildFavorites();
}

void HomePage::rebuildStats()
{
    clearLayout(m_statsLayout);

    const MediaDatabase::Stats s = m_database->stats();

    m_statsLayout->addWidget(makeStatCard(
        QStringLiteral("全部资源"),
        QString::number(s.total),
        QStringLiteral("合计 %1").arg(formatSize(s.totalBytes)),
        ThemeManager::textPrimary().name()));

    m_statsLayout->addWidget(makeStatCard(
        QStringLiteral("图片"),
        QString::number(s.images),
        QStringLiteral("张"),
        ThemeManager::typeColorImage().name()));

    m_statsLayout->addWidget(makeStatCard(
        QStringLiteral("视频"),
        QString::number(s.videos),
        QStringLiteral("个"),
        ThemeManager::typeColorVideo().name()));

    m_statsLayout->addWidget(makeStatCard(
        QStringLiteral("音频"),
        QString::number(s.audios),
        QStringLiteral("个"),
        ThemeManager::typeColorAudio().name()));

    m_statsLayout->addWidget(makeStatCard(
        QStringLiteral("收藏"),
        QString::number(s.favorites),
        QStringLiteral("项"),
        ThemeManager::favoriteColor().name()));

    // 失效记录只在确实存在时占一个卡片位置，避免常驻一个恒为 0 的格子
    if (s.missing > 0) {
        m_statsLayout->addWidget(makeStatCard(
            QStringLiteral("失效记录"),
            QString::number(s.missing),
            QStringLiteral("文件已不存在"),
            ThemeManager::dangerColor().name()));
    }
}

void HomePage::rebuildRecentFolders()
{
    clearLayout(m_recentFoldersLayout);

    /*!
     * "最近访问的目录"的数据来源：内嵌资源里文件夹的修改时间排序。
     *
     * 这里没有单独的"目录访问历史表"——那需要额外维护一张表并处理
     * 目录被删除的情况。用已有数据推导：按 folder_path 分组取最近修改时间，
     * 能得到"最近有内容变动的目录"，对用户来说是相近的语义，
     * 且不需要引入新的持久化状态。
     */
    const QList<MediaItem> recent = m_database->queryItems(
        QString(), MediaType::Unknown, false, 0, 200, QStringLiteral("mtime DESC"));

    QList<QPair<QString, QDateTime>> folders;
    QSet<QString> seen;

    for (const MediaItem &item : recent) {
        if (seen.contains(item.folderPath))
            continue;
        seen.insert(item.folderPath);
        folders.append({ item.folderPath, item.modified });
        if (folders.size() >= kMaxRecentFolders)
            break;
    }

    if (folders.isEmpty()) {
        auto *label = new QLabel(QStringLiteral("还没有可用的目录。"), m_recentFoldersCard);
        label->setStyleSheet(QStringLiteral("color: %1; font-size: 12px; padding: 8px;")
                                 .arg(ThemeManager::textDisabled().name()));
        m_recentFoldersLayout->addWidget(label);
        return;
    }

    for (const auto &pair : folders) {
        const QString path = pair.first;
        const QFileInfo info(path);

        // 目录可能已被删除（移动硬盘拔出、手动删掉）。这里如实标注，
        // 而不是假装它还在——用户需要知道点进去会是空的。
        const bool exists = info.exists() && info.isDir();

        const int count = m_database->countItems(path);
        const QString subtitle = exists
                                     ? QStringLiteral("%1 个资源 · %2")
                                           .arg(count)
                                           .arg(humanizeTime(pair.second))
                                     : QStringLiteral("目录已不存在");

        auto *row = makeListRow(exists ? Icons::Folder : Icons::Trash,
                                info.fileName().isEmpty() ? path : info.fileName(),
                                subtitle,
                                path);

        // 目录不存在时视觉上弱化，并且点击不跳转（跳过去也没内容）
        if (!exists) {
            row->setEnabled(false);
            row->setToolTip(QStringLiteral("该目录已不存在：%1").arg(path));
        } else {
            connect(row, &QPushButton::clicked, this, [this, path]() {
                emit folderRequested(path);
            });
        }

        m_recentFoldersLayout->addWidget(row);
    }
}

void HomePage::rebuildRecentItems()
{
    clearLayout(m_recentItemsLayout);

    const QList<MediaItem> recent = m_database->recentPlayed(kMaxRecentItems);

    if (recent.isEmpty()) {
        auto *label = new QLabel(
            QStringLiteral("还没有浏览记录。打开任意资源后，这里会显示最近访问的内容。"),
            m_recentItemsCard);
        label->setWordWrap(true);
        label->setStyleSheet(QStringLiteral("color: %1; font-size: 12px; padding: 8px;")
                                 .arg(ThemeManager::textDisabled().name()));
        m_recentItemsLayout->addWidget(label);
        return;
    }

    for (const MediaItem &item : recent) {
        QStringList parts;
        parts << mediaTypeName(item.type);
        parts << formatSize(item.size);

        const int plays = m_database->playCount(item.id);
        if (plays > 1)
            parts << QStringLiteral("已打开 %1 次").arg(plays);

        parts << humanizeTime(item.modified);

        auto *row = makeListRow(iconForType(item.type),
                                item.fileName,
                                parts.join(QStringLiteral(" · ")),
                                item.path);

        const MediaItem captured = item;
        connect(row, &QPushButton::clicked, this, [this, captured]() {
            emit itemRequested(captured);
        });

        m_recentItemsLayout->addWidget(row);
    }
}

void HomePage::rebuildFavorites()
{
    clearLayout(m_favoritesLayout);

    const QList<MediaItem> favorites = m_database->queryItems(
        QString(), MediaType::Unknown, true, 0, kMaxFavorites, QStringLiteral("mtime DESC"));

    const int totalFavorites = m_database->countItems(QString(), MediaType::Unknown, true);

    if (favorites.isEmpty()) {
        auto *label = new QLabel(
            QStringLiteral("还没有收藏。在资源上右键或点详情面板的星标即可收藏。"),
            m_favoritesCard);
        label->setWordWrap(true);
        label->setStyleSheet(QStringLiteral("color: %1; font-size: 12px; padding: 8px;")
                                 .arg(ThemeManager::textDisabled().name()));
        m_favoritesLayout->addWidget(label);
        return;
    }

    for (const MediaItem &item : favorites) {
        QStringList parts;
        parts << mediaTypeName(item.type);
        if (item.width > 0 && item.height > 0)
            parts << QStringLiteral("%1×%2").arg(item.width).arg(item.height);
        parts << formatSize(item.size);

        auto *row = makeListRow(Icons::StarFilled,
                                item.fileName,
                                parts.join(QStringLiteral(" · ")),
                                item.path);

        const MediaItem captured = item;
        connect(row, &QPushButton::clicked, this, [this, captured]() {
            emit itemRequested(captured);
        });

        m_favoritesLayout->addWidget(row);
    }

    // 超过展示上限时明确说明，而不是静默截断
    if (totalFavorites > kMaxFavorites) {
        auto *more = new QLabel(
            QStringLiteral("还有 %1 项收藏未显示").arg(totalFavorites - kMaxFavorites),
            m_favoritesCard);
        more->setStyleSheet(QStringLiteral("color: %1; font-size: 11px; padding: 6px 8px;")
                                .arg(ThemeManager::textDisabled().name()));
        m_favoritesLayout->addWidget(more);
    }
}

} // namespace media
