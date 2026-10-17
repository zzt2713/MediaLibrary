#include "SidebarWidget.h"
#include "Icons.h"
#include "ThemeManager.h"
#include "../core/MediaDatabase.h"
#include "../core/MediaScanner.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QListWidget>
#include <QTreeWidget>
#include <QTreeView>
#include <QStyledItemDelegate>
#include <QStyleOptionViewItem>
#include <QPainter>
#include <QPainterPath>
#include <QLineEdit>
#include <QLabel>
#include <QPushButton>
#include <QMenu>
#include <QAction>
#include <QDesktopServices>
#include <QUrl>
#include <QFileInfo>
#include <QDir>
#include <QSet>
#include <QTimer>
#include <QDebug>
#include <functional>

namespace media {

const QString SidebarWidget::GroupHome       = QStringLiteral("home");
const QString SidebarWidget::GroupAll        = QStringLiteral("all");
const QString SidebarWidget::GroupImages     = QStringLiteral("images");
const QString SidebarWidget::GroupVideos     = QStringLiteral("videos");
const QString SidebarWidget::GroupAudios     = QStringLiteral("audios");
const QString SidebarWidget::GroupFavorites  = QStringLiteral("favorites");
const QString SidebarWidget::GroupRecent     = QStringLiteral("recent");
const QString SidebarWidget::GroupDuplicates = QStringLiteral("duplicates");
const QString SidebarWidget::GroupMissing    = QStringLiteral("missing");

namespace {

/*!
 * 目录路径存在 QTreeWidgetItem 的自定义数据里。
 * 用 role 存而不是靠文本反查：文本可能重复，也可能被改。
 */
constexpr int kPathRole = Qt::UserRole + 1;

/*! 标签项的真实标签名。第 0 行是"不限标签"，这里存空串。 */
constexpr int kTagRole = Qt::UserRole + 2;

/*!
 * 目录树节点上限。一个有十万个子目录的根会让建树本身变成卡顿源，
 * 超过就截断并提示用户用筛选框搜索。
 */
constexpr int kMaxTreeNodes = 4000;

/*!
 * \brief 目录树的行绘制代理：整行一个圆角块。
 *
 * 为什么不用 QSS 的 #FolderTree::item 直接画：
 * QTreeView::drawRow() 把一行拆成"缩进区（分支箭头所在）+ 文本区"两段，
 * 分别调用 PE_PanelItemViewRow 向样式表要背景。两段各自套用 ::item 的
 * border-radius，选中时就会画出"左边一个圆角小方块 + 右边一个圆角长条"，
 * 中间夹着分支箭头的缝隙 —— 这正是用户说的"右边有个小方块"。
 *
 * 样式表没办法让两段合并成一块，所以改由 delegate 在 drawRow 之后
 * 一次性铺满整行的背景，再把文字与图标画上去。
 * 前提是 QSS 里 #FolderTree::item 的背景必须是 transparent，
 * 否则 Qt 会在 delegate 之前先画一遍那两段。
 */
/*!
 * \brief 目录树控件：屏蔽 Qt 自己画的分支箭头。
 *
 * QTreeView::drawRow() 的绘制顺序是「缩进区背景 → 文本区背景 → 分支箭头 →
 * delegate」。delegate 画在最后，它铺的整行背景会把箭头盖住；
 * 而如果只在选中/悬停时才铺背景，未选中行又会出现"Qt 的箭头 + 我画的箭头"
 * 两份。所以直接把 Qt 的箭头绘制关掉，箭头统一由 FolderTreeDelegate 画，
 * 位置与点击热区仍由 QTreeView 的缩进几何决定，展开/收起行为不变。
 */
class FolderTree : public QTreeWidget
{
public:
    explicit FolderTree(QWidget *parent = nullptr)
        : QTreeWidget(parent)
    {
    }

protected:
    void drawBranches(QPainter *, const QRect &, const QModelIndex &) const override
    {
        // 故意留空：箭头由 delegate 绘制，见 FolderTreeDelegate::paint
    }
};

/*!
 * \brief 目录树的行绘制代理：整行一个圆角块。
 *
 * 为什么不用 QSS 的 #FolderTree::item 直接画：
 * QTreeView::drawRow() 把一行拆成"缩进区（分支箭头所在）+ 文本区"两段，
 * 分别调用 PE_PanelItemViewRow 向样式表要背景。两段各自套用 ::item 的
 * border-radius，选中时就会画出"左边一个圆角小方块 + 右边一个圆角长条"，
 * 中间夹着分支箭头的缝隙 —— 这正是用户说的"右边有个小方块"。
 *
 * 样式表没办法让两段合并成一块，所以改由 delegate 一次性铺满整行背景，
 * 再把箭头、图标、文字画上去。
 * 前提是 QSS 里 #FolderTree::item 的背景必须是 transparent，
 * 否则 Qt 会在 delegate 之前先画一遍那两段。
 */
class FolderTreeDelegate : public QStyledItemDelegate
{
public:
    explicit FolderTreeDelegate(QObject *parent = nullptr)
        : QStyledItemDelegate(parent)
    {
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);

        /*!
         * 整行背景：从视口最左边一直铺到最右边。
         *
         * QTreeView 传给 delegate 的 option.rect 已经扣掉了缩进宽度
         * （见 QTreeView::drawRow 里 opt.rect.setRect(i + position, ...)），
         * 直接用它画背景，缩进那一列就是空的 —— 用户看到的"左边空白"。
         * 这里改从视口左边界起算，整行一个圆角块。
         */
        const QAbstractItemView *view = qobject_cast<const QAbstractItemView *>(option.widget);
        const int viewWidth = view ? view->viewport()->width() : option.rect.right() + 1;
        const QRect rowRect = QRect(0, option.rect.y(), viewWidth, option.rect.height())
                                  .adjusted(2, 1, -2, -1);

        const bool selected = option.state & QStyle::State_Selected;
        const bool hovered = option.state & QStyle::State_MouseOver;
        const bool enabled = option.state & QStyle::State_Enabled;

        if (selected || (hovered && enabled)) {
            QPainterPath path;
            path.addRoundedRect(rowRect, 6, 6);
            painter->fillPath(path, selected ? ThemeManager::accentSubtle()
                                             : ThemeManager::surfaceHover());
        }

        /*!
         * 分支箭头：展开时朝下，收起时朝右。
         * 位置取 option.rect 左边再往左一个缩进宽度 —— 那正是该层箭头所在的格子。
         */
        const QAbstractItemModel *model = index.model();
        if (model && model->hasChildren(index)) {
            const int indent = view ? static_cast<const QTreeView *>(view)->indentation() : 14;
            const QRect arrowCell(option.rect.left() - indent, rowRect.top(), indent, rowRect.height());
            const bool open = option.state & QStyle::State_Open;

            const qreal cx = arrowCell.center().x() + 0.5;
            const qreal cy = arrowCell.center().y() + 0.5;
            const qreal s = 3.4;

            QPainterPath tri;
            if (open) {
                tri.moveTo(cx - s, cy - s * 0.6);
                tri.lineTo(cx + s, cy - s * 0.6);
                tri.lineTo(cx, cy + s * 0.9);
            } else {
                tri.moveTo(cx - s * 0.6, cy - s);
                tri.lineTo(cx + s * 0.9, cy);
                tri.lineTo(cx - s * 0.6, cy + s);
            }
            tri.closeSubpath();

            painter->setPen(Qt::NoPen);
            painter->setBrush(selected ? ThemeManager::textSecondary()
                                       : ThemeManager::textDisabled());
            painter->drawPath(tri);
        }

        /*!
         * 文字与图标。
         *
         * 默认实现从 option.rect 左边界起画，紧贴缩进区，左侧显得很空。
         * 这里显式留出 8px 内边距，让"箭头 → 图标 → 文字"的间距均匀。
         */
        const QIcon icon = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));
        const QString text = index.data(Qt::DisplayRole).toString();

        QFont font = option.font;
        if (selected)
            font.setWeight(QFont::DemiBold);
        painter->setFont(font);

        const QFontMetrics fm(font);
        const int iconSize = option.decorationSize.isValid() ? option.decorationSize.width() : 16;
        const int leftPad = 8;
        int x = option.rect.left() + leftPad;

        if (!icon.isNull()) {
            const QPixmap pm = icon.pixmap(iconSize, iconSize);
            const qreal dpr = pm.devicePixelRatio() > 0 ? pm.devicePixelRatio() : 1.0;
            const int logicalW = qRound(pm.width() / dpr);
            const int logicalH = qRound(pm.height() / dpr);
            painter->drawPixmap(QPoint(x, rowRect.top() + (rowRect.height() - logicalH) / 2), pm);
            x += logicalW + 6;
        }

        QColor textColor = ThemeManager::textSecondary();
        if (!enabled) {
            textColor = ThemeManager::textDisabled();
        } else if (selected) {
            textColor = ThemeManager::textPrimary();
        } else {
            const QBrush fg = index.data(Qt::ForegroundRole).value<QBrush>();
            if (fg.style() != Qt::NoBrush)
                textColor = fg.color();
        }
        painter->setPen(textColor);

        const QRect textRect(x, rowRect.top(), qMax(0, rowRect.right() - x - 2), rowRect.height());
        const QString elided = fm.elidedText(text, Qt::ElideMiddle, textRect.width());
        painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, elided);

        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QSize s = QStyledItemDelegate::sizeHint(option, index);
        s.setHeight(30);
        return s;
    }
};

} // namespace

SidebarWidget::SidebarWidget(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("Sidebar"));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // ---------- 智能分组 ----------
    auto *smartTitle = new QLabel(QStringLiteral("资源库"), this);
    smartTitle->setObjectName(QStringLiteral("SidebarSectionTitle"));
    layout->addWidget(smartTitle);

    m_smartList = new QListWidget(this);
    m_smartList->setObjectName(QStringLiteral("SidebarList"));
    m_smartList->setFrameShape(QFrame::NoFrame);
    m_smartList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_smartList->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_smartList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_smartList->setIconSize(QSize(17, 17));
    /*
     * 9 项（首页 + 8 个智能分组），按行高 38 预留。
     * 这里跟随条目数改动：写死行高乘以项目数，是为了让列表不出现滚动条，
     * 也不需要为一次布局测量写额外的 sizeHint 逻辑。
     */
    m_smartList->setFixedHeight(9 * 38 + 8);
    layout->addWidget(m_smartList);

    // ---------- 目录 ----------
    auto *folderHeader = new QWidget(this);
    auto *folderHeaderLayout = new QHBoxLayout(folderHeader);
    folderHeaderLayout->setContentsMargins(0, 0, 8, 0);
    folderHeaderLayout->setSpacing(0);

    auto *folderTitle = new QLabel(QStringLiteral("目录"), folderHeader);
    folderTitle->setObjectName(QStringLiteral("SidebarSectionTitle"));
    folderHeaderLayout->addWidget(folderTitle);
    folderHeaderLayout->addStretch();

    auto *addBtn = new QPushButton(folderHeader);
    addBtn->setProperty("iconButton", true);
    addBtn->setIcon(Icons::get(Icons::Add, ThemeManager::textSecondary(), 16));
    addBtn->setToolTip(QStringLiteral("添加目录到资源库"));
    addBtn->setCursor(Qt::PointingHandCursor);
    folderHeaderLayout->addWidget(addBtn);

    layout->addWidget(folderHeader);

    m_folderFilter = new QLineEdit(this);
    m_folderFilter->setPlaceholderText(QStringLiteral("筛选目录…"));
    m_folderFilter->setClearButtonEnabled(true);
    m_folderFilter->setFixedHeight(28);
    auto *filterWrap = new QWidget(this);
    auto *filterLayout = new QHBoxLayout(filterWrap);
    filterLayout->setContentsMargins(8, 2, 8, 6);
    filterLayout->addWidget(m_folderFilter);
    layout->addWidget(filterWrap);

    m_folderTree = new FolderTree(this);
    m_folderTree->setObjectName(QStringLiteral("FolderTree"));
    m_folderTree->setHeaderHidden(true);
    m_folderTree->setFrameShape(QFrame::NoFrame);
    m_folderTree->setIndentation(14);
    m_folderTree->setAnimated(false);          // 动画在大树上会明显拖慢展开
    m_folderTree->setUniformRowHeights(true);  // 行高一致时 Qt 可以跳过逐项测量
    m_folderTree->setIconSize(QSize(16, 16));
    m_folderTree->setExpandsOnDoubleClick(true);
    m_folderTree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_folderTree->setItemDelegate(new FolderTreeDelegate(m_folderTree));
    layout->addWidget(m_folderTree, 1);

    // ---------- 标签 ----------
    m_tagTitle = new QLabel(QStringLiteral("标签"), this);
    m_tagTitle->setObjectName(QStringLiteral("SidebarSectionTitle"));
    layout->addWidget(m_tagTitle);

    m_tagList = new QListWidget(this);
    m_tagList->setObjectName(QStringLiteral("SidebarList"));
    m_tagList->setFrameShape(QFrame::NoFrame);
    m_tagList->setMaximumHeight(140);
    m_tagList->setIconSize(QSize(14, 14));
    m_tagList->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(m_tagList);

    // ---------- 连接 ----------
    connect(m_smartList, &QListWidget::itemClicked,
            this, &SidebarWidget::onSmartGroupClicked);
    connect(m_folderTree, &QTreeWidget::itemClicked,
            this, &SidebarWidget::onFolderClicked);
    connect(m_folderTree, &QTreeWidget::customContextMenuRequested,
            this, &SidebarWidget::onFolderContextMenu);
    connect(m_smartList, &QListWidget::customContextMenuRequested,
            this, &SidebarWidget::onSmartContextMenu);
    connect(m_folderFilter, &QLineEdit::textChanged,
            this, &SidebarWidget::onFilterTextChanged);
    connect(addBtn, &QPushButton::clicked,
            this, &SidebarWidget::addFolderRequested);
    connect(m_tagList, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        if (!item)
            return;
        // 第 0 行是"不限标签"：空串让接收方清掉筛选，而不是当字面量 "none" 去匹配
        const bool clear = (m_tagList->row(item) == 0);
        emit tagSelected(clear ? QString() : item->text());
    });
    connect(m_tagList, &QListWidget::customContextMenuRequested,
            this, &SidebarWidget::onTagContextMenu);

    // 计数刷新去抖：扫描时 libraryChanged 会高频触发，合并成一次重算
    m_countDebounce = new QTimer(this);
    m_countDebounce->setSingleShot(true);
    m_countDebounce->setInterval(600);
    connect(m_countDebounce, &QTimer::timeout,
            this, &SidebarWidget::refreshCountsFromDatabase);

    buildSmartGroups();
}

SidebarWidget::~SidebarWidget() = default;

void SidebarWidget::setDatabase(MediaDatabase *database)
{
    if (m_database == database)
        return;

    if (m_database)
        disconnect(m_database, nullptr, this, nullptr);

    m_database = database;

    if (m_database) {
        /*!
         * 库变化时只启动去抖定时器，不立刻重算。
         *
         * 为什么必须去抖：扫描时 upsertItems 每 64 个文件就发一次 libraryChanged，
         * 一万个文件就是 150 多次。如果每次都跑 stats() + duplicateGroups() +
         * folderCounts() 并重建目录树，扫描会慢到不可接受，
         * 而且目录树的展开状态会被反复重建。
         *
         * 600ms 的延迟在用户感知里仍是"即时刷新"，但把 150 次重算压成几次。
         */
        connect(m_database, &MediaDatabase::libraryChanged, this, [this]() {
            if (m_countDebounce)
                m_countDebounce->start();
        });
    }

    reload();
}

void SidebarWidget::buildSmartGroups()
{
    struct Entry
    {
        QString group;
        QString text;
        QString icon;
    };

    const QList<Entry> entries = {
        { GroupHome,       QStringLiteral("首页"),     Icons::Search },
        { GroupAll,        QStringLiteral("全部资源"), Icons::Folder },
        { GroupImages,     QStringLiteral("图片"),     Icons::Image },
        { GroupVideos,     QStringLiteral("视频"),     Icons::Video },
        { GroupAudios,     QStringLiteral("音频"),     Icons::Audio },
        { GroupFavorites,  QStringLiteral("收藏"),     Icons::StarFilled },
        { GroupRecent,     QStringLiteral("最近打开"), Icons::Clock },
        { GroupDuplicates, QStringLiteral("重复项"),   Icons::Duplicate },
        { GroupMissing,    QStringLiteral("失效记录"), Icons::Trash },
    };

    m_smartList->clear();
    m_smartItems.clear();

    for (const Entry &e : entries) {
        auto *item = new QListWidgetItem(m_smartList);
        item->setText(e.text);
        item->setIcon(Icons::get(e.icon, ThemeManager::textSecondary(), 17));
        item->setData(Qt::UserRole, e.group);
        item->setToolTip(e.text);
        m_smartItems.insert(e.group, item);
    }

    /*
     * 默认选中首页，与 MainWindow 的初始页保持一致。
     *
     * setCurrentRow 不会发出 itemClicked，所以这里只是"视觉上高亮"，
     * 真正的页面切换由 MainWindow 在构造末尾自己完成 ——
     * 否则侧边栏会在构造期间（数据库还没接上）就驱动一次导航。
     */
    if (m_smartList->count() > 0)
        m_smartList->setCurrentRow(0);
}

void SidebarWidget::setCountBadge(QListWidgetItem *item, int count)
{
    if (!item)
        return;

    // 计数直接拼在文本后面，兜底用空格对齐。
    // 没有用自定义 delegate 画右侧徽标：那需要额外处理选中态与高 DPI，
    // 收益仅有视觉上的对齐。
    const QString base = item->data(Qt::UserRole + 10).toString().isEmpty()
                             ? item->text().section(QLatin1Char('\t'), 0, 0)
                             : item->data(Qt::UserRole + 10).toString();

    item->setData(Qt::UserRole + 10, base);

    if (count > 0)
        item->setText(QStringLiteral("%1\t%2").arg(base).arg(count));
    else
        item->setText(base);
}

void SidebarWidget::updateCounts()
{
    if (!m_database || !m_database->isOpen())
        return;

    // 只渲染缓存值，不查数据库。
    // 查库的部分统一在 refreshCountsFromDatabase() 里做一次。
    setCountBadge(m_smartItems.value(GroupAll), m_cachedStats.total);
    setCountBadge(m_smartItems.value(GroupImages), m_cachedStats.images);
    setCountBadge(m_smartItems.value(GroupVideos), m_cachedStats.videos);
    setCountBadge(m_smartItems.value(GroupAudios), m_cachedStats.audios);
    setCountBadge(m_smartItems.value(GroupFavorites), m_cachedStats.favorites);
    setCountBadge(m_smartItems.value(GroupMissing), m_cachedStats.missing);

    setCountBadge(m_smartItems.value(GroupRecent), m_cachedRecentCount);
    setCountBadge(m_smartItems.value(GroupDuplicates), m_cachedDuplicateGroupCount);
}

void SidebarWidget::refreshCountsFromDatabase()
{
    if (!m_database || !m_database->isOpen())
        return;

    // 这一组查询相对贵（stats 是多条聚合，duplicateGroups 要扫哈希索引），
    // 所以只在这个去抖定时器里跑一次，而不是每次 libraryChanged 都跑。
    m_cachedStats = m_database->stats();

    // 只计数不取内容。recentPlayed() 会连表查回完整的 MediaItem，
    // 为了一个数字付这个代价不划算，所以直接用轻量计数。
    m_cachedRecentCount = m_database->playHistoryCount();

    // duplicateGroups() 会拉回所有重复条目。它本身就是"重复项"分组要用的数据，
    // 一次查询服务于两处（计数与分组内容），因此这里接受它的开销。
    m_cachedDuplicateGroupCount = m_database->duplicateGroups().size();

    // 目录树计数：一次 GROUP BY 拿全，不逐节点查库
    m_folderCounts = m_database->folderCounts();
    rebuildFolderTree();

    // 标签列表
    const QStringList tags = m_database->allTags();
    const QString previouslyActiveTag = m_activeTag;
    m_activeTag.clear();
    m_tagList->clear();

    /*!
     * 第 0 行固定为"不限标签"。
     *
     * 没有这一行，标签条件一旦设上就没有任何地方能清掉——
     * 之后无论切目录还是切分组，筛选都会一直生效，
     * 用户看到的是"明明没选标签，列表却总是少一半"。
     */
    auto *clearItem = new QListWidgetItem(QStringLiteral("不限标签"), m_tagList);
    clearItem->setIcon(Icons::get(Icons::Search, ThemeManager::textSecondary(), 14));
    clearItem->setData(kTagRole, QString());
    // 标签数为 0 时隐藏整块，这一行不该单独留在那里
    clearItem->setHidden(tags.isEmpty());

    for (const QString &tag : tags) {
        auto *item = new QListWidgetItem(tag, m_tagList);
        // 用已收藏的星形作为标签图标：形状简单、语义上能表示"标记"，
        // 不值得为它单独画一个新图标。
        item->setIcon(Icons::get(Icons::StarFilled, ThemeManager::textDisabled(), 14));
        item->setData(kTagRole, tag);
    }
    m_tagTitle->setVisible(true);
    m_tagList->setVisible(!tags.isEmpty());

    // 重建会让所有选中态丢失，把上次生效的标签重新高亮
    setActiveTag(previouslyActiveTag);

    updateCounts();
}

void SidebarWidget::setActiveTag(const QString &tag)
{
    m_activeTag = tag;

    if (m_tagList->count() == 0)
        return;

    QListWidgetItem *target = nullptr;
    if (tag.isEmpty()) {
        target = m_tagList->item(0);
    } else {
        for (int i = 1; i < m_tagList->count(); ++i) {
            QListWidgetItem *it = m_tagList->item(i);
            if (it && it->data(kTagRole).toString().compare(tag, Qt::CaseInsensitive) == 0) {
                target = it;
                break;
            }
        }
    }

    if (target)
        m_tagList->setCurrentItem(target);
    else
        m_tagList->setCurrentItem(nullptr);
}

void SidebarWidget::reload()
{
    // reload 是"显式要求立刻刷新"的入口（添加目录、移除目录、扫描结束），
    // 所以这里直接查一次，不走 600ms 去抖——用户刚做完操作，期望立即看到结果。
    // refreshCountsFromDatabase() 内部会更新 m_folderCounts 并重建树。
    refreshCountsFromDatabase();
}

void SidebarWidget::rebuildFolderTree()
{
    if (!m_database || !m_database->isOpen()) {
        m_folderTree->clear();
        return;
    }

    // 记住展开状态与当前选中，重建后恢复。
    // 不恢复的话，每次库变更都会让用户展开的目录全部折叠。
    QSet<QString> expanded;
    std::function<void(QTreeWidgetItem *)> collectExpanded = [&](QTreeWidgetItem *item) {
        if (!item)
            return;
        if (item->isExpanded())
            expanded.insert(item->data(0, kPathRole).toString());
        for (int i = 0; i < item->childCount(); ++i)
            collectExpanded(item->child(i));
    };
    for (int i = 0; i < m_folderTree->topLevelItemCount(); ++i)
        collectExpanded(m_folderTree->topLevelItem(i));

    const QString previouslySelected = m_currentFolder;

    m_folderTree->clear();

    int nodeCount = 0;
    const QList<QString> roots = m_database->allFolderPaths();

    for (const QString &root : roots) {
        QTreeWidgetItem *node = addFolderNode(nullptr, root, m_folderCounts, 0);
        if (node)
            ++nodeCount;

        if (nodeCount >= kMaxTreeNodes)
            break;
    }

    if (roots.isEmpty()) {
        // 空状态提示，避免用户面对一片空白不知道要做什么
        auto *placeholder = new QTreeWidgetItem(m_folderTree);
        placeholder->setText(0, QStringLiteral("尚未添加目录，点上方 + 添加"));
        // 不给 ItemIsEnabled：这一行不是节点，只是提示文字。
        // 禁用态下 Qt 不会为它画 hover/selected 的低色，也就不会出现
        // 一个孤零零的灰色圆角块。真正要拦截的交互（点击、右键）
        // 在 onFolderClicked / onFolderContextMenu 里已经按空 kPathRole 提前 return。
        placeholder->setFlags(Qt::NoItemFlags);
        placeholder->setForeground(0, ThemeManager::textDisabled());
        return;
    }

    // 恢复展开状态；根节点默认展开
    std::function<void(QTreeWidgetItem *, int)> restore = [&](QTreeWidgetItem *item, int depth) {
        if (!item)
            return;
        const QString path = item->data(0, kPathRole).toString();
        if (expanded.contains(path) || depth == 0)
            item->setExpanded(true);
        for (int i = 0; i < item->childCount(); ++i)
            restore(item->child(i), depth + 1);
    };
    for (int i = 0; i < m_folderTree->topLevelItemCount(); ++i)
        restore(m_folderTree->topLevelItem(i), 0);

    // 恢复选中
    if (!previouslySelected.isEmpty()) {
        std::function<QTreeWidgetItem *(QTreeWidgetItem *)> find = [&](QTreeWidgetItem *item) -> QTreeWidgetItem * {
            if (!item)
                return nullptr;
            if (item->data(0, kPathRole).toString() == previouslySelected)
                return item;
            for (int i = 0; i < item->childCount(); ++i) {
                if (QTreeWidgetItem *found = find(item->child(i)))
                    return found;
            }
            return nullptr;
        };
        for (int i = 0; i < m_folderTree->topLevelItemCount(); ++i) {
            if (QTreeWidgetItem *found = find(m_folderTree->topLevelItem(i))) {
                m_folderTree->setCurrentItem(found);
                break;
            }
        }
    }

    // 筛选框里有内容时，重建后要重新应用筛选
    if (!m_folderFilter->text().isEmpty())
        applyFilter(m_folderFilter->text());
}

QTreeWidgetItem *SidebarWidget::addFolderNode(QTreeWidgetItem *parent,
                                              const QString &path,
                                              const QHash<QString, int> &counts,
                                              int depth)
{
    if (path.isEmpty() || depth > kMaxTreeDepth)
        return nullptr;

    QFileInfo info(path);
    if (!info.exists() || !info.isDir())
        return nullptr;   // 目录已不存在（盘拔出 / 被删），不进树

    auto *node = parent ? new QTreeWidgetItem(parent)
                        : new QTreeWidgetItem(m_folderTree);

    int ownCount = counts.value(QDir::cleanPath(path), 0);

    // 如果这个目录本身没有直接资源，但子目录有，则显示"含子目录总数"，
    // 否则用户会看到一个 0 的目录却能在右边看到一堆图，产生困惑。
    if (ownCount == 0) {
        int agg = 0;
        const QString prefix = QDir::cleanPath(path) + QLatin1Char('/');
        for (auto it = counts.constBegin(); it != counts.constEnd(); ++it) {
            if (it.key().startsWith(prefix))
                agg += it.value();
        }
        ownCount = agg;
    }

    node->setText(0, ownCount > 0
                         ? QStringLiteral("%1  (%2)").arg(info.fileName()).arg(ownCount)
                         : info.fileName());
    node->setIcon(0, Icons::get(Icons::Folder, ThemeManager::textSecondary(), 16));
    node->setData(0, kPathRole, QDir::cleanPath(path));
    node->setToolTip(0, path);

    // 有资源的目录用主文字色，空目录用次要色，一眼区分哪些有内容
    node->setForeground(0, ownCount > 0 ? ThemeManager::textSecondary()
                                        : ThemeManager::textDisabled());

    // 递归建子节点，但受深度限制。
    // 不预建过深的层级：一个 photo/2020/01/01/xxx 结构建满会很慢，
    // 用户展开到那层时再靠右侧网格筛选也够用。
    if (depth < kMaxTreeDepth) {
        const QList<QString> subs = MediaScanner::listSubDirectories(path);
        for (const QString &sub : subs) {
            // 只建"这棵子树里有资源"的节点，避免把临时目录也建进来
            bool relevant = counts.contains(QDir::cleanPath(sub));
            if (!relevant) {
                const QString prefix = QDir::cleanPath(sub) + QLatin1Char('/');
                for (auto it = counts.constBegin(); it != counts.constEnd(); ++it) {
                    if (it.key().startsWith(prefix)) {
                        relevant = true;
                        break;
                    }
                }
            }
            if (!relevant)
                continue;

            addFolderNode(node, sub, counts, depth + 1);
        }
    }

    return node;
}

void SidebarWidget::onSmartGroupClicked(QListWidgetItem *item)
{
    if (!item)
        return;

    const QString group = item->data(Qt::UserRole).toString();
    if (group.isEmpty())
        return;

    m_currentSmartGroup = group;
    m_currentFolder.clear();

    // 切到智能分组时清掉目录树的选中，避免两处同时高亮造成误导
    m_folderTree->clearSelection();
    m_folderTree->setCurrentItem(nullptr);

    emit smartGroupSelected(group, item->text().section(QLatin1Char('\t'), 0, 0));
}

void SidebarWidget::onFolderClicked(QTreeWidgetItem *item, int column)
{
    Q_UNUSED(column)
    if (!item)
        return;

    const QString path = item->data(0, kPathRole).toString();
    if (path.isEmpty())
        return;   // 占位提示行

    m_currentFolder = path;
    m_currentSmartGroup.clear();

    m_smartList->clearSelection();
    m_smartList->setCurrentItem(nullptr);

    emit folderSelected(path);
}

void SidebarWidget::onFolderContextMenu(const QPoint &pos)
{
    QTreeWidgetItem *item = m_folderTree->itemAt(pos);
    if (!item)
        return;

    const QString path = item->data(0, kPathRole).toString();
    if (path.isEmpty())
        return;

    QMenu menu(this);
    QAction *reveal = menu.addAction(Icons::get(Icons::Folder, ThemeManager::textSecondary(), 16),
                                     QStringLiteral("在文件管理器中打开"));
    menu.addSeparator();
    QAction *refresh = menu.addAction(Icons::get(Icons::Refresh, ThemeManager::textSecondary(), 16),
                                      QStringLiteral("重新扫描此目录"));
    menu.addSeparator();
    QAction *remove = menu.addAction(Icons::get(Icons::Trash, ThemeManager::dangerColor(), 16),
                                     QStringLiteral("从资源库移除"));

    // 只有"根目录"可以被移除；子目录没有独立记录，移除它没有意义
    const bool isRoot = m_database && m_database->folderExists(path);
    remove->setEnabled(isRoot);
    if (!isRoot)
        remove->setToolTip(QStringLiteral("只有已添加的根目录可以移除"));

    QAction *chosen = menu.exec(m_folderTree->viewport()->mapToGlobal(pos));
    if (!chosen)
        return;

    if (chosen == reveal) {
        emit revealInExplorerRequested(path);
    } else if (chosen == refresh) {
        emit folderSelected(path);   // 选中即触发重新扫描（由 MainWindow 决定策略）
    } else if (chosen == remove) {
        emit removeFolderRequested(path);
    }
}

void SidebarWidget::onSmartContextMenu(const QPoint &pos)
{
    // 智能分组没有右键操作，留空是刻意选择：
    // 这些是固定视图，加菜单只会让界面更复杂。
    Q_UNUSED(pos)
}

void SidebarWidget::onTagContextMenu(const QPoint &pos)
{
    QListWidgetItem *item = m_tagList->itemAt(pos);
    if (!item)
        return;

    // 第 0 行是"不限标签"，没有可删的东西
    const QString tag = item->data(kTagRole).toString();
    if (tag.isEmpty())
        return;

    QMenu menu(this);
    QAction *del = menu.addAction(
        Icons::get(Icons::Trash, ThemeManager::dangerColor(), 16),
        QStringLiteral("删除标签「%1」").arg(tag));
    del->setToolTip(QStringLiteral("从所有资源上移除该标签"));

    QAction *chosen = menu.exec(m_tagList->viewport()->mapToGlobal(pos));
    if (chosen == del)
        emit tagDeleteRequested(tag);
}

void SidebarWidget::onFilterTextChanged(const QString &text)
{
    applyFilter(text);
}

void SidebarWidget::applyFilter(const QString &text, QTreeWidgetItem *parent)
{
    const QString needle = text.trimmed();

    if (!parent) {
        // 顶层调用：对每个根节点递归处理
        for (int i = 0; i < m_folderTree->topLevelItemCount(); ++i) {
            QTreeWidgetItem *item = m_folderTree->topLevelItem(i);
            if (item->data(0, kPathRole).toString().isEmpty())
                continue;   // 占位行不参与筛选

            const bool selfMatch = needle.isEmpty()
                                   || item->text(0).contains(needle, Qt::CaseInsensitive);
            bool childMatch = false;
            for (int j = 0; j < item->childCount(); ++j) {
                applyFilter(text, item->child(j));
                if (!item->child(j)->isHidden())
                    childMatch = true;
            }

            // 自身匹配或任一子节点匹配则显示；否则隐藏。
            // 隐藏用 setHidden 而不是删除：筛选是临时的，删了还要重建树。
            item->setHidden(!(selfMatch || childMatch));

            if (!needle.isEmpty() && childMatch)
                item->setExpanded(true);
        }
        return;
    }

    // 递归分支
    const bool selfMatch = needle.isEmpty()
                           || parent->text(0).contains(needle, Qt::CaseInsensitive);
    bool childMatch = false;
    for (int j = 0; j < parent->childCount(); ++j) {
        applyFilter(text, parent->child(j));
        if (!parent->child(j)->isHidden())
            childMatch = true;
    }
    parent->setHidden(!(selfMatch || childMatch));
}

void SidebarWidget::selectFolder(const QString &path)
{
    const QString norm = QDir::cleanPath(path);

    std::function<QTreeWidgetItem *(QTreeWidgetItem *)> find = [&](QTreeWidgetItem *item) -> QTreeWidgetItem * {
        if (!item)
            return nullptr;
        if (item->data(0, kPathRole).toString() == norm)
            return item;
        for (int i = 0; i < item->childCount(); ++i) {
            if (QTreeWidgetItem *found = find(item->child(i)))
                return found;
        }
        return nullptr;
    };

    for (int i = 0; i < m_folderTree->topLevelItemCount(); ++i) {
        if (QTreeWidgetItem *found = find(m_folderTree->topLevelItem(i))) {
            m_folderTree->setCurrentItem(found);
            m_folderTree->scrollToItem(found);
            onFolderClicked(found, 0);
            return;
        }
    }

    // 树里没有（可能是新加的目录还没重建），直接发信号让上层处理
    m_currentFolder = norm;
    m_currentSmartGroup.clear();
    emit folderSelected(norm);
}

void SidebarWidget::selectSmartGroup(const QString &group)
{
    if (group.isEmpty())
        return;

    QListWidgetItem *item = m_smartItems.value(group);
    if (!item) {
        // 未知分组键：不静默吞掉，否则首页的入口点到没反应会很难查
        qWarning() << "[SidebarWidget] unknown smart group" << group;
        return;
    }

    // 先设高亮，再复用 onSmartGroupClicked 的统一出口，
    // 保证"外部选中"和"用户点击"这两条路径产生完全一致的状态变更。
    m_smartList->setCurrentItem(item);
    onSmartGroupClicked(item);
}

void SidebarWidget::expandAll()
{
    m_folderTree->expandAll();
}

void SidebarWidget::collapseAll()
{
    m_folderTree->collapseAll();

    // 收起后把根节点重新展开，否则用户看不到任何东西
    for (int i = 0; i < m_folderTree->topLevelItemCount(); ++i)
        m_folderTree->topLevelItem(i)->setExpanded(true);
}

} // namespace media
