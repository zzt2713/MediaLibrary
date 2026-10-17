#include "ThumbnailGrid.h"
#include "Icons.h"
#include "ThemeManager.h"
#include "../model/MediaModel.h"
#include "../model/MediaFilterProxyModel.h"
#include "../tasks/ThumbnailTask.h"

#include <QPainter>
#include <QPainterPath>
#include <QStackedWidget>
#include <QTableView>
#include <QHeaderView>
#include <QVBoxLayout>
#include <QScrollBar>
#include <QApplication>
#include <QMenu>
#include <QAction>
#include <QFileInfo>
#include <QDir>
#include <QDesktopServices>
#include <QUrl>
#include <QDebug>

namespace media {

namespace {

/*!
 * 卡片内缩略图的内边距。
 *
 * 从 8 收到 4：图片之间的真实间距 = 左右内边距 + 卡片内缩 + 视图 spacing，
 * 8 时相邻两张图隔 26px，网格看起来是"一堆小图浮在空地上"而不是铺开的墙。
 * 4 是下限——再小的话，卡片圆角和文件名行的呼吸空间就没了
 * （文件名行高按 kLabelHeight 留出，底部还剩 2px，正好）。
 */
constexpr int kCardPadding = 4;
constexpr int kLabelHeight = 34;
constexpr int kCardRadius = 10;

/*!
 * 已转换 QPixmap 缓存的总字节上限（64MB）。
 *
 * 单张的估算成本是相当准确的：pixmapFor() 存的就是缩略图区大小
 * （最大 320×320）的 RGB32 数据，即 w*h*4 字节，320 时约 400KB。
 * 64MB 大致对应一百多张大缩略图，足够覆盖"一屏十几张 + 来回滚动"
 * 的正常使用，又不会因为浏览过整个目录而无限增长。
 */
constexpr qint64 kMaxPixmapCacheBytes = 64LL * 1024 * 1024;

} // namespace

// ================================================================ 尺寸映射

int ThumbnailGrid::edgeFromSlider(int sliderValue)
{
    const int v = qBound(0, sliderValue, 100);
    // 96 ~ 320 线性映射。下限 96 保证还能看清内容，上限 320 避免单屏只能放一张。
    return 96 + (v * (320 - 96)) / 100;
}

int ThumbnailGrid::sliderFromEdge(int edge)
{
    const int e = qBound(96, edge, 320);
    return ((e - 96) * 100) / (320 - 96);
}

// ================================================================ Delegate

ThumbnailDelegate::ThumbnailDelegate(QObject *parent)
    : QStyledItemDelegate(parent)
{
}

void ThumbnailDelegate::setThumbnailEdge(int edge)
{
    const int bounded = qBound(96, edge, 320);
    if (bounded == m_edge)
        return;

    m_edge = bounded;

    // 缩略图尺寸变了：已转换的 QPixmap 是按旧尺寸画的，作废重来。
    // 清空是安全的，下一次 paint() 会按需重新转换。
    clearPixmapCache();
}

void ThumbnailDelegate::clearPixmapCache() const
{
    m_pixmapCache.clear();
    m_pixmapCacheBytes = 0;
    m_placeholderCache.clear();
}

const QPixmap *ThumbnailDelegate::pixmapFor(const QModelIndex &index, qint64 itemId,
                                           const QSize &target) const
{
    if (itemId <= 0 || target.isEmpty())
        return nullptr;

    const QImage image = index.data(MediaModel::ThumbnailRole).value<QImage>();
    if (image.isNull())
        return nullptr;

    // 命中缓存时顺便校验来源图没变。
    //
    // 这里比较 QImage::cacheKey()（O(1)）而不是 QImage::operator==：
    // 后者只有在两个 QImage 共享同一份数据时才是常数级，一旦数据不同
    // 就会退化成逐像素 memcmp（见 Qt 源码 qimage.cpp 中 operator== 的实现）。
    // 而"数据不同"正是这里要识别的换图场景，用 == 等于在该路径上做了
    // 一次全图比对。cacheKey 在 QImage 分离出新数据时会变，正是需要的语义。
    const qint64 sourceKey = image.cacheKey();

    auto it = m_pixmapCache.find(itemId);
    if (it != m_pixmapCache.end() && it.value().sourceKey == sourceKey)
        return &it.value().pixmap;

    PixmapEntry entry;
    entry.sourceKey = sourceKey;
    // 转换与缩放都发生在 GUI 线程（paint 的调用栈里），
    // 这正是 QPixmap 被允许出现的场合。
    //
    // 缩放尽量让平台后端做：QPixmap::scaled 在 Qt 6 下走
    // QImage 的重采样实现，效率和先 fromImage 再 scaled 一样，
    // 但少一次中间对象的构造。
    const QPixmap raw = QPixmap::fromImage(image);
    /*!
     * Cover 式缩放（等比放大到至少盖满目标再裁掉溢出）。
     *
     * 之前是 KeepAspectRatio（contain）：非正方形的图在正方形缩略图区里
     * 上下/左右留出底色条，网格看起来"没铺开、全是空隙"。
     * 现在裁边填满，比例仍完全一致（等比缩放，绝不拉伸变形），
     * 裁掉的边缘在预览窗口里能看到完整原图。
     */
    entry.pixmap = (raw.size() == target)
                       ? raw
                       : raw.scaled(target, Qt::KeepAspectRatioByExpanding,
                                    Qt::SmoothTransformation);

    // 走到这里有两种情况：该 itemId 从未缓存，或缓存里是旧图（sourceKey 不符）。
    // 后者的旧条目会被下面的 insert 覆盖，它的字节数要先从统计里扣掉，
    // 否则反复换图会让计数虚高，提前触发清空。
    qint64 replacedBytes = 0;
    if (it != m_pixmapCache.end()) {
        replacedBytes = qint64(it.value().pixmap.width())
                        * it.value().pixmap.height() * 4;
    }

    // 容量检查必须放在 insert 之前：pixmapFor() 返回的是指向 QHash 内元素的
    // 指针，调用方（paint）会立刻使用它。若插入之后再清理，刚插进去的那张
    // 就可能被一起清掉，返回的指针当场失效。
    const qint64 entryBytes = qint64(entry.pixmap.width()) * entry.pixmap.height() * 4;
    if (m_pixmapCacheBytes - replacedBytes + entryBytes > kMaxPixmapCacheBytes)
        clearPixmapCache();
    else
        m_pixmapCacheBytes -= replacedBytes;

    it = m_pixmapCache.insert(itemId, entry);
    m_pixmapCacheBytes += entryBytes;
    return &it.value().pixmap;
}

const QPixmap *ThumbnailDelegate::placeholderFor(MediaType type,
                                                 const QString &durationText,
                                                 const QSize &target) const
{
    if (target.isEmpty())
        return nullptr;

    // 键里带上时长文本：视频占位图角标上会画时长，不同时长必须是不同条目。
    const QString key = QStringLiteral("%1|%2|%3x%4")
                            .arg(static_cast<int>(type))
                            .arg(durationText)
                            .arg(target.width())
                            .arg(target.height());

    auto it = m_placeholderCache.find(key);
    if (it != m_placeholderCache.end())
        return &it.value();

    // 键里带了时长文本，理论上一个全是视频的目录能产生几百个不同键。
    // 占位图是 edge×edge×4 字节，无上限地攒着不合适，所以给个小容量：
    // 超出就整体清掉重建。可见的加载中项就那么几十个，不会真的反复冲到上限。
    constexpr int kMaxPlaceholders = 24;
    if (m_placeholderCache.size() >= kMaxPlaceholders)
        m_placeholderCache.clear();

    // m_edge 用 target 的高度：缩略图区是正方形，二者一致。
    const QImage phImage = ThumbnailTask::makePlaceholder(type, target.height(),
                                                          durationText);
    if (phImage.isNull())
        return nullptr;

    const QPixmap ph = QPixmap::fromImage(phImage);
    const QPixmap scaled = (ph.size() == target)
                               ? ph
                               : ph.scaled(target, Qt::KeepAspectRatio,
                                           Qt::SmoothTransformation);
    it = m_placeholderCache.insert(key, scaled);
    return &it.value();
}

QSize ThumbnailDelegate::sizeHint(const QStyleOptionViewItem &option,
                                  const QModelIndex &index) const
{
    Q_UNUSED(option)
    Q_UNUSED(index)

    // 卡片尺寸固定，不依赖内容。固定尺寸让 QListView 的布局计算可以缓存，
    // 在大列表滚动时明显更快；也让网格排列整齐。
    const int w = m_edge + kCardPadding * 2;
    const int h = m_edge + kCardPadding * 2 + (m_showFileName ? kLabelHeight : 0);

    return QSize(w, h);
}

void ThumbnailDelegate::paintBadge(QPainter *painter, const QRect &cardRect,
                                   const QString &text, const QColor &bg) const
{
    if (text.isEmpty())
        return;

    QFont f = painter->font();
    f.setPixelSize(11);
    f.setBold(false);
    painter->setFont(f);

    const QFontMetrics fm(f);
    const int tw = fm.horizontalAdvance(text) + 12;
    const int th = fm.height() + 6;

    const QRect badge(cardRect.right() - tw - 4,
                      cardRect.bottom() - th - 4,
                      tw, th);

    painter->setPen(Qt::NoPen);
    painter->setBrush(bg);
    painter->drawRoundedRect(badge, th / 2.0, th / 2.0);

    painter->setPen(QColor(0xff, 0xff, 0xff, 220));
    painter->drawText(badge, Qt::AlignCenter, text);
}

void ThumbnailDelegate::paint(QPainter *painter,
                              const QStyleOptionViewItem &option,
                              const QModelIndex &index) const
{
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setRenderHint(QPainter::SmoothPixmapTransform, true);

    const QRect itemRect = option.rect;
    const bool selected = option.state & QStyle::State_Selected;
    const bool hovered = option.state & QStyle::State_MouseOver;

    // ---------- 卡片背景 ----------
    // 内缩 2px 而不是 3：给选中描边（1.5px）留出位置即可，每边再省 1px。
    // 连同 kCardPadding 8→4，相邻图片的可见间距从约 26px 收到 12px。
    const QRect cardRect = itemRect.adjusted(2, 2, -2, -2);

    QPainterPath cardPath;
    cardPath.addRoundedRect(cardRect, kCardRadius, kCardRadius);

    if (selected) {
        painter->fillPath(cardPath, ThemeManager::accentSubtle());
    } else if (hovered) {
        painter->fillPath(cardPath, ThemeManager::surfaceHover());
    }

    // 选中态描边：用强调色画 1.5px 边框
    if (selected) {
        painter->setPen(QPen(ThemeManager::accent(), 1.5));
        painter->setBrush(Qt::NoBrush);
        painter->drawPath(cardPath);
    }

    // ---------- 缩略图区域 ----------
    // 水平居中于卡片（卡片比缩略图宽出的部分每边一半），垂直方向留 kCardPadding
    const int hPad = qMax(0, (cardRect.width() - m_edge) / 2);
    const QRect thumbArea(cardRect.left() + hPad,
                          cardRect.top() + kCardPadding,
                          m_edge,
                          m_edge);

    const int state = index.data(MediaModel::ThumbnailStateRole).toInt();
    const int type = index.data(MediaModel::TypeRole).toInt();
    const bool missing = index.data(MediaModel::MissingRole).toBool();

    // 缩略图区底色：图片已 Cover 填满，这里只在加载中（占位图半透明）和
    // 失效（画文字）时可见
    painter->setPen(Qt::NoPen);
    painter->setBrush(ThemeManager::surfaceBackground());
    painter->drawRoundedRect(thumbArea, 7, 7);

    // 缩略图在模型里是 QImage，这里通过带缓存的 pixmapFor() 换成 QPixmap，
    // 并且**连缩放结果一起缓存**。不直接在 paint() 里 fromImage + scaled 的原因
    // 见头文件里的说明：每帧重采样会明显拖慢重绘。
    const qint64 itemId = index.data(MediaModel::IdRole).toLongLong();
    const QPixmap *thumbPtr = pixmapFor(index, itemId, thumbArea.size());

    if (thumbPtr && !thumbPtr->isNull()) {
        const QPixmap &scaled = *thumbPtr;

        // 居中绘制：缩放在 pixmapFor() 里按 Cover（等比放大后裁边）做完，
        // 与 thumbArea 等大，摆正即可；比例绝不变形。
        const QPoint topLeft(thumbArea.left() + (thumbArea.width() - scaled.width()) / 2,
                             thumbArea.top() + (thumbArea.height() - scaled.height()) / 2);

        // 裁剪圆角，让缩略图与卡片底色融合
        painter->save();
        QPainterPath clip;
        clip.addRoundedRect(thumbArea, 7, 7);
        painter->setClipPath(clip);
        painter->drawPixmap(topLeft, scaled);
        painter->restore();
    } else if (state == 1) {
        // 加载中：画一个半透明的类型占位图。
        // 不用旋转动画：每帧重绘整个可视区代价太高，静止的占位已经够用，
        // 而且它会让用户明白"这里马上会有东西"，避免布局看起来是空的。
        //
        // makePlaceholder 返回 QImage（它在工作线程里也会被调用，
        // 不能碰 QPixmap）。这里在 GUI 线程转一次，并连同缩放结果缓存 ——
        // 占位图只由「类型 + 时长」决定，组合数很少，缓存几乎不占内存；
        // 而一个屏幕几十项同时处于加载态时，逐帧重算的重绘成本很可观。
        const QPixmap *phPtr = placeholderFor(
            static_cast<MediaType>(type),
            index.data(MediaModel::DurationTextRole).toString(),
            thumbArea.size());
        if (phPtr && !phPtr->isNull()) {
            const QPixmap &scaled = *phPtr;
            const QPoint topLeft(thumbArea.left() + (thumbArea.width() - scaled.width()) / 2,
                                 thumbArea.top() + (thumbArea.height() - scaled.height()) / 2);
            painter->setOpacity(0.55);
            painter->drawPixmap(topLeft, scaled);
            painter->setOpacity(1.0);
        }
    } else {
        // 未请求或失败：画类型图标，保证格子不空着
        QString iconName;
        QColor iconColor = ThemeManager::textDisabled();
        switch (static_cast<MediaType>(type)) {
        case MediaType::Image: iconName = Icons::Image; iconColor = ThemeManager::typeColorImage(); break;
        case MediaType::Video: iconName = Icons::Video; iconColor = ThemeManager::typeColorVideo(); break;
        case MediaType::Audio: iconName = Icons::Audio; iconColor = ThemeManager::typeColorAudio(); break;
        default: iconName = Icons::Info; break;
        }

        const int iconSize = qMin(48, m_edge / 3);
        const QPixmap iconPm = Icons::pixmap(iconName, iconColor, iconSize);
        const QPoint topLeft(thumbArea.center().x() - iconSize / 2,
                             thumbArea.center().y() - iconSize / 2);
        painter->setOpacity(0.7);
        painter->drawPixmap(topLeft, iconPm);
        painter->setOpacity(1.0);
    }

    // ---------- 角标 ----------
    // 视频/音频时长
    if (static_cast<MediaType>(type) == MediaType::Video
        || static_cast<MediaType>(type) == MediaType::Audio) {
        const QString dur = index.data(MediaModel::DurationTextRole).toString();
        if (dur != QStringLiteral("--")) {
            paintBadge(painter, thumbArea, dur, QColor(0, 0, 0, 165));
        }
    }

    // 失效标记：盖一层灰并写字
    if (missing) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(0, 0, 0, 150));
        painter->drawRoundedRect(thumbArea, 7, 7);

        QFont f = painter->font();
        f.setPixelSize(11);
        painter->setFont(f);
        painter->setPen(QColor(0xe5, 0x6b, 0x6b));
        painter->drawText(thumbArea, Qt::AlignCenter, QStringLiteral("文件已失效"));
    }

    // 收藏星标：右上角
    if (index.data(MediaModel::FavoriteRole).toBool()) {
        const int starSize = 16;
        const QPixmap star = Icons::pixmap(Icons::StarFilled, ThemeManager::favoriteColor(), starSize);
        painter->drawPixmap(thumbArea.right() - starSize - 5,
                            thumbArea.top() + 5,
                            star);
    }

    // ---------- 文件名与信息 ----------
    if (m_showFileName) {
        const QRect labelRect(cardRect.left() + 4,
                              thumbArea.bottom() + 4,
                              cardRect.width() - 8,
                              kLabelHeight - 6);

        QFont nameFont = painter->font();
        nameFont.setPixelSize(12);
        painter->setFont(nameFont);

        const QString name = index.data(MediaModel::FileNameRole).toString();

        // 名字太长时用中间省略，保留扩展名。
        // Qt 的 ElideMiddle 正好符合这个需求（"verylongname.jpg" → "very...jpg"）。
        const QFontMetrics fm(nameFont);
        const QString elided = fm.elidedText(name, Qt::ElideMiddle, labelRect.width());

        painter->setPen(missing ? ThemeManager::textDisabled()
                                : (selected ? ThemeManager::textPrimary()
                                            : ThemeManager::textSecondary()));
        painter->drawText(labelRect, Qt::AlignHCenter | Qt::AlignTop, elided);
    }

    painter->restore();
}

// ================================================================ Grid

ThumbnailGrid::ThumbnailGrid(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_stack = new QStackedWidget(this);
    layout->addWidget(m_stack);

    // 滚动去抖：停止滚动 150ms 后才请求缩略图。
    // 快速拖动时只在停下来那一刻发一批请求，避免请求风暴。
    m_scrollDebounce = new QTimer(this);
    m_scrollDebounce->setSingleShot(true);
    m_scrollDebounce->setInterval(150);
    connect(m_scrollDebounce, &QTimer::timeout,
            this, &ThumbnailGrid::requestVisibleThumbnails);

    setupGrid();
    setupList();

    m_stack->addWidget(m_grid);
    m_stack->addWidget(m_list);

    applyViewMode();
}

ThumbnailGrid::~ThumbnailGrid() = default;

void ThumbnailGrid::setupGrid()
{
    m_grid = new QListView(this);
    m_grid->setObjectName(QStringLiteral("ThumbnailGrid"));

    m_delegate = new ThumbnailDelegate(this);
    m_grid->setItemDelegate(m_delegate);

    m_grid->setViewMode(QListView::IconMode);
    m_grid->setResizeMode(QListView::Adjust);
    m_grid->setMovement(QListView::Static);        // 不允许拖动重排
    m_grid->setFlow(QListView::LeftToRight);
    m_grid->setWrapping(true);
    m_grid->setSpacing(4);
    m_grid->setUniformItemSizes(true);             // 固定尺寸 → 布局计算可缓存
    m_grid->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_grid->setSelectionRectVisible(true);
    m_grid->setMouseTracking(true);                // 悬停高亮需要
    m_grid->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_grid->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_grid->setContextMenuPolicy(Qt::CustomContextMenu);
    m_grid->setEditTriggers(QAbstractItemView::NoEditTriggers);

    connect(m_grid, &QListView::clicked, this, &ThumbnailGrid::onItemClicked);
    connect(m_grid, &QListView::doubleClicked, this, &ThumbnailGrid::onItemDoubleClicked);
    connect(m_grid, &QListView::customContextMenuRequested,
            this, &ThumbnailGrid::onContextMenu);

    // selectionModel()->selectionChanged 不在这里连：此刻视图还没有选择模型，
    // 原因与正确位置见 setModel()。原来那处父 if (selectionModel()) 判断永远不成立，
    // 属于从未生效的代码。
}

void ThumbnailGrid::setupList()
{
    m_list = new QTableView(this);
    m_list->setObjectName(QStringLiteral("MediaListView"));
    m_list->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_list->setAlternatingRowColors(true);
    m_list->setShowGrid(false);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    m_list->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_list->verticalHeader()->setVisible(false);
    m_list->verticalHeader()->setDefaultSectionSize(30);
    // 单列表格：让唯一那列占满宽度，不要留一条空白尾巴
    m_list->horizontalHeader()->setStretchLastSection(true);
    m_list->horizontalHeader()->setHighlightSections(false);
    m_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);

    // 列标题由 MediaModel::headerData 提供，见 setModel()

    connect(m_list, &QTableView::doubleClicked, this, [this](const QModelIndex &idx) {
        onItemDoubleClicked(idx);
    });
    connect(m_list, &QTableView::customContextMenuRequested,
            this, &ThumbnailGrid::onContextMenu);

    // selectionModel()->selectionChanged 不在这里连，原因同 setupGrid()。
}

void ThumbnailGrid::setModel(MediaModel *model, MediaFilterProxyModel *proxy)
{
    m_model = model;
    m_proxy = proxy;

    m_grid->setModel(proxy);
    m_list->setModel(proxy);

    /*!
     * 选中项变化时通知外部（驱动详情面板与状态栏的选中统计）。
     *
     * 必须连在这里，不能放在 setupGrid()/setupList()：视图构造完成时
     * QAbstractItemViewPrivate 的构造函数把 selectionModel 初始化成 nullptr，
     * 此刻 view->selectionModel() 返回空指针，那里原来的 if (selectionModel())
     * 判断永远不成立 —— 连接从未建立过。
     *
     * 而真正的选择模型是下面这两句 setModel() 内部新建并替换上去的
     * （QAbstractItemView::setModel() 里 new QItemSelectionModel(d->model, this)
     * 再 setSelectionModel()），所以只有在这之后，视图的选择模型才是
     * 最终那个、且此后不再被替换的对象。
     *
     * 两个槽都是成员函数指针，所以 Qt::UniqueConnection 在这里是有效的；
     * 保证 setModel() 被重复调用时不会叠加连接。
     */
    if (m_grid->selectionModel()) {
        connect(m_grid->selectionModel(), &QItemSelectionModel::selectionChanged,
                this, &ThumbnailGrid::onSelectionChanged, Qt::UniqueConnection);
    }
    if (m_list->selectionModel()) {
        connect(m_list->selectionModel(), &QItemSelectionModel::selectionChanged,
                this, &ThumbnailGrid::onSelectionChanged, Qt::UniqueConnection);
    }

    /*!
     * 列表视图是单列的（MediaModel 是列表模型，只提供一列）。
     *
     * 列标题不在这里设：原来这里调的是 proxy->setHeaderData(...)，
     * 而那是一条从未生效的调用 —— QSortFilterProxyModel::setHeaderData
     * 会把请求转发到源模型，MediaModel 没有重写它，QAbstractItemModel
     * 的基础实现直接 return false，静默失败。
     * 结果就是表头显示 QAbstractItemModel::headerData 默认返回的 "1"。
     * 现在标题由 MediaModel::headerData 提供。
     */
    if (proxy && proxy->columnCount() > 0)
        m_list->setColumnWidth(0, 320);

    m_requested.clear();

    // 模型插入新行后要请求缩略图。这里连 rowsInserted 而不是每插一行请求一次，
    // 分页加载会一次插入几百行，逐行处理会产生几百次调用。
    //
    // 这几处 connect 都不用 Qt::UniqueConnection：该标志只对"成员函数指针"形式的槽
    // 有效（见 QtCore/qobject.h 的 Q_ASSERT_X：Unique connection requires the slot
    // to be a pointer to a member function）。传 lambda 时它在 release 下静默失效，
    // 在 debug 下直接断言中断。setModel() 目前只被调用一次（MainWindow 构造期），
    // 重复连接的风险由调用方保证；将来若需多次调用，应先 disconnect 再连。
    if (model) {
        connect(model, &MediaModel::rowsInserted, this,
                [this](const QModelIndex &, int, int) {
                    if (m_scrollDebounce)
                        m_scrollDebounce->start();
                });

        connect(model, &MediaModel::modelReset, this, [this]() {
            m_requested.clear();
            if (m_scrollDebounce)
                m_scrollDebounce->start();
        });

        /*!
         * 一批缩略图完成后要继续请求下一批。
         *
         * requestVisibleThumbnails() 单次最多发 kMaxPerBatch 个请求，
         * 而它原本只在「模型插入行」和「滚动」时被触发。结果是：打开一个大目录时，
         * 首批 60 张加载完就停住了，剩下的必须等用户滚动才会继续 ——
         * 看起来就是"缩略图一张一张慢慢冒出来"。
         *
         * 缩略图到位时模型会发 dataChanged，用它作为推进信号即可。
         * 这里刻意只在定时器**未激活**时启动：若每个数据变化都 restart，
         * 连续到达的缩略图会把定时器一直往后推，反而永远等不到触发时机。
         */
        connect(model, &MediaModel::dataChanged, this,
                [this](const QModelIndex &, const QModelIndex &, const QList<int> &) {
                    if (m_scrollDebounce && !m_scrollDebounce->isActive())
                        m_scrollDebounce->start();
                });
    }

    // 滚动时去抖请求。这两处的槽是成员函数指针，用 UniqueConnection 是有效的
    // （滚动条在视图内部重建时可能重复触发，避免重复连接）
    if (m_grid->verticalScrollBar()) {
        connect(m_grid->verticalScrollBar(), &QScrollBar::valueChanged,
                this, &ThumbnailGrid::onScrolled, Qt::UniqueConnection);
    }
    if (m_list->verticalScrollBar()) {
        connect(m_list->verticalScrollBar(), &QScrollBar::valueChanged,
                this, &ThumbnailGrid::onScrolled, Qt::UniqueConnection);
    }

    m_scrollDebounce->start();
}

void ThumbnailGrid::applyViewMode()
{
    switch (m_viewMode) {
    case GridMode:
        m_stack->setCurrentWidget(m_grid);
        if (m_delegate)
            m_delegate->setShowFileName(true);
        break;
    case ListMode:
        m_stack->setCurrentWidget(m_list);
        break;
    }

    // 切模式后可视区变了，重新请求一批
    m_requested.clear();
    if (m_scrollDebounce)
        m_scrollDebounce->start();
}

void ThumbnailGrid::setViewMode(ViewMode mode)
{
    if (m_viewMode == mode)
        return;

    m_viewMode = mode;
    applyViewMode();
}

void ThumbnailGrid::setThumbnailSize(int sliderValue)
{
    const int edge = edgeFromSlider(sliderValue);
    if (edge == m_thumbEdge)
        return;

    m_thumbEdge = edge;

    if (m_delegate)
        m_delegate->setThumbnailEdge(edge);

    if (m_model)
        m_model->setThumbnailEdge(edge);

    // 尺寸变了，之前"已请求"的记录作废——因为缓存键里的 edge 变了
    m_requested.clear();

    // 通知视图重新计算所有项的尺寸
    m_grid->reset();
    if (m_scrollDebounce)
        m_scrollDebounce->start();
}

int ThumbnailGrid::thumbnailSize() const
{
    return sliderFromEdge(m_thumbEdge);
}

void ThumbnailGrid::onScrolled()
{
    // 每次滚动都重启定时器：连续滚动期间不发请求，停下 150ms 才发。
    if (m_scrollDebounce)
        m_scrollDebounce->start();
}

void ThumbnailGrid::requestVisibleThumbnails()
{
    if (!m_model || !m_proxy)
        return;

    QAbstractItemView *view = (m_viewMode == GridMode)
                                  ? static_cast<QAbstractItemView *>(m_grid)
                                  : static_cast<QAbstractItemView *>(m_list);

    if (!view->model() || view->model()->rowCount() == 0)
        return;

    const QRect viewportRect = view->viewport()->rect();

    // 上下各多请求一屏，滚动时就能命中已有图，不会看到一片空白。
    // 预取太多会浪费 IO，一屏是比较好的折中。
    const int preloadPx = viewportRect.height();
    const QRect prefetchRect = viewportRect.adjusted(0, -preloadPx, 0, preloadPx);

    int requestedNow = 0;
    constexpr int kMaxPerBatch = 60;   // 单批上限，避免一次塞爆队列

    // 取第一个与最后一个可见行。IconMode 下每行的列数由布局决定，
    // 逐个 indexAt 采样比遍历所有行便宜得多。
    const QModelIndex topIdx = view->indexAt(prefetchRect.topLeft());
    const QModelIndex bottomIdx = view->indexAt(prefetchRect.bottomLeft());

    int firstRow = topIdx.isValid() ? topIdx.row() : 0;
    int lastRow = bottomIdx.isValid() ? bottomIdx.row()
                                      : view->model()->rowCount() - 1;

    // indexAt 可能返回空（间距里），此时向前后各扩几行兜底
    firstRow = qMax(0, firstRow - 20);
    lastRow = qMin(view->model()->rowCount() - 1, lastRow + 20);

    for (int row = firstRow; row <= lastRow && requestedNow < kMaxPerBatch; ++row) {
        const QModelIndex idx = view->model()->index(row, 0);
        if (!idx.isValid())
            continue;

        const qint64 id = idx.data(MediaModel::IdRole).toLongLong();
        if (id <= 0 || m_requested.contains(id))
            continue;

        const int state = idx.data(MediaModel::ThumbnailStateRole).toInt();
        if (state == 2)
            continue;   // 已完成，无需再请求

        m_requested.insert(id);
        ++requestedNow;

        // 从代理模型取源行，再拿完整 MediaItem。
        // 直接用 idx.data 拼一个不完整的 item 会丢字段（如 mtime），
        // 而 mtime 是缓存键的组成部分。
        const int sourceRow = m_proxy->mapToSource(idx).row();
        const MediaItem item = m_model->itemAt(sourceRow);
        if (item.isValid())
            emit requestThumbnail(item, m_thumbEdge);
    }
}

void ThumbnailGrid::onItemClicked(const QModelIndex &index)
{
    if (!index.isValid() || !m_model || !m_proxy)
        return;

    // 注：单击时这条路径与选择模型的 selectionChanged 会各发一次
    // selectionChanged，即一次点击触发两次通知（详情面板会重建两次预览）。
    // 保留它是因为单击"已经选中且只选中它"的项时选择集合没有变化，
    // QItemSelectionModel 不会发 selectionChanged（只发 currentChanged），
    // 此时只有这里能通知外部。
    // 上层是幂等的（DetailPane::showItem 直接覆盖，TaskManager::requestThumbnail
    // 按 m_thumbEdgeByItem 去重），所以重复通知只多花一次重绘，不会出错。
    const int sourceRow = m_proxy->mapToSource(index).row();
    const MediaItem item = m_model->itemAt(sourceRow);
    if (item.isValid())
        emit selectionChanged(item);
}

void ThumbnailGrid::onItemDoubleClicked(const QModelIndex &index)
{
    if (!index.isValid() || !m_model || !m_proxy)
        return;

    const int sourceRow = m_proxy->mapToSource(index).row();
    const MediaItem item = m_model->itemAt(sourceRow);
    if (item.isValid())
        emit itemActivated(item);
}

void ThumbnailGrid::onSelectionChanged()
{
    const MediaItem item = currentItem();
    emit selectionChanged(item);
}

void ThumbnailGrid::onContextMenu(const QPoint &pos)
{
    QAbstractItemView *view = (m_viewMode == GridMode)
                                  ? static_cast<QAbstractItemView *>(m_grid)
                                  : static_cast<QAbstractItemView *>(m_list);

    const QModelIndex idx = view->indexAt(pos);
    if (!idx.isValid() || !m_model || !m_proxy)
        return;

    const int sourceRow = m_proxy->mapToSource(idx).row();
    const MediaItem item = m_model->itemAt(sourceRow);
    if (!item.isValid())
        return;

    emit contextMenuRequested(item, view->viewport()->mapToGlobal(pos));
}

MediaItem ThumbnailGrid::currentItem() const
{
    if (!m_model || !m_proxy)
        return MediaItem();

    QAbstractItemView *view = (m_viewMode == GridMode)
                                  ? static_cast<QAbstractItemView *>(m_grid)
                                  : static_cast<QAbstractItemView *>(m_list);

    const QModelIndex idx = view->currentIndex();
    if (!idx.isValid())
        return MediaItem();

    const int sourceRow = m_proxy->mapToSource(idx).row();
    return m_model->itemAt(sourceRow);
}

QList<MediaItem> ThumbnailGrid::selectedItems() const
{
    QList<MediaItem> result;
    if (!m_model || !m_proxy)
        return result;

    QAbstractItemView *view = (m_viewMode == GridMode)
                                  ? static_cast<QAbstractItemView *>(m_grid)
                                  : static_cast<QAbstractItemView *>(m_list);

    if (!view->selectionModel())
        return result;

    const QModelIndexList selected = view->selectionModel()->selectedIndexes();
    for (const QModelIndex &idx : selected) {
        if (idx.column() != 0)
            continue;   // 表格视图会有多列，只取第一列避免重复
        const int sourceRow = m_proxy->mapToSource(idx).row();
        const MediaItem item = m_model->itemAt(sourceRow);
        if (item.isValid())
            result.append(item);
    }

    return result;
}

void ThumbnailGrid::selectItemById(qint64 id)
{
    if (!m_model || !m_proxy || id <= 0)
        return;

    const int sourceRow = m_model->rowForId(id);
    if (sourceRow < 0)
        return;

    const QModelIndex proxyIdx = m_proxy->mapFromSource(m_model->index(sourceRow, 0));
    if (!proxyIdx.isValid())
        return;

    QAbstractItemView *view = (m_viewMode == GridMode)
                                  ? static_cast<QAbstractItemView *>(m_grid)
                                  : static_cast<QAbstractItemView *>(m_list);

    view->setCurrentIndex(proxyIdx);
    view->scrollTo(proxyIdx, QAbstractItemView::EnsureVisible);
}

void ThumbnailGrid::selectRow(int proxyRow)
{
    if (!m_proxy)
        return;

    const QModelIndex idx = m_proxy->index(proxyRow, 0);
    if (!idx.isValid())
        return;

    QAbstractItemView *view = (m_viewMode == GridMode)
                                  ? static_cast<QAbstractItemView *>(m_grid)
                                  : static_cast<QAbstractItemView *>(m_list);

    view->setCurrentIndex(idx);
    view->scrollTo(idx, QAbstractItemView::EnsureVisible);
}

int ThumbnailGrid::currentRow() const
{
    QAbstractItemView *view = (m_viewMode == GridMode)
                                  ? static_cast<QAbstractItemView *>(m_grid)
                                  : static_cast<QAbstractItemView *>(m_list);

    const QModelIndex idx = view->currentIndex();
    return idx.isValid() ? idx.row() : -1;
}

int ThumbnailGrid::loadedCount() const
{
    return m_model ? m_model->loadedCount() : 0;
}

int ThumbnailGrid::totalCount() const
{
    return m_model ? m_model->totalCount() : 0;
}

bool ThumbnailGrid::isEmpty() const
{
    return !m_proxy || m_proxy->rowCount() == 0;
}

void ThumbnailGrid::refreshVisible()
{
    m_requested.clear();
    if (m_scrollDebounce)
        m_scrollDebounce->start();
}

} // namespace media
