#ifndef THUMBNAILGRID_H
#define THUMBNAILGRID_H

#include "../core/MediaTypes.h"

#include <QWidget>
#include <QListView>
#include <QStyledItemDelegate>
#include <QSlider>
#include <QString>
#include <QSet>
#include <QHash>
#include <QImage>
#include <QPixmap>
#include <QTimer>

class QStackedWidget;
class QTableView;
class QLabel;

namespace media {

class MediaModel;
class MediaFilterProxyModel;

/*!
 * \brief 缩略图绘制 delegate。
 *
 * 为什么自己画而不是用 QListWidget 的图标模式：
 *  - 需要在缩略图下方显示文件名、类型与尺寸；
 *  - 需要画收藏星标、失效/损坏标记、选中描边；
 *  - 需要精确控制卡片间距与圆角，让它看起来像现代设计而非默认样式。
 *
 * 自定义 delegate 的另一个好处是 draw 只在需要时被调用，
 * 不可见的项完全不参与绘制，天然支持大列表。
 */
class ThumbnailDelegate : public QStyledItemDelegate
{
    Q_OBJECT

public:
    explicit ThumbnailDelegate(QObject *parent = nullptr);

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override;

    void setThumbnailEdge(int edge);
    int thumbnailEdge() const { return m_edge; }

    /*! 是否在卡片上显示文件名。列表模式下由外部关闭。 */
    void setShowFileName(bool show) { m_showFileName = show; }

private:
    /*! 在卡片右下角画一个小角标（视频时长 / 收藏 / 损坏标记）。 */
    void paintBadge(QPainter *painter, const QRect &cardRect,
                    const QString &text, const QColor &bg) const;

    int m_edge = 160;
    bool m_showFileName = true;

    /*!
     * \brief QImage → QPixmap 的转换缓存，只在 GUI 线程访问。
     *
     * 为什么必须有这个缓存：
     * 模型里存的是 QImage（跨线程传递安全），但绘制只能用 QPixmap。
     * 如果每帧都在 paint() 里调 QPixmap::fromImage，那就是把整张图
     * 的像素重新上传一次平台后端存储——网格里几十张可见项，
     * 每次重绘都要做几十次，直接表现为滚动卡顿。
     *
     * 缓存里存的是**已经缩放到缩略图区域的**那张 pixmap，不是原始尺寸的。
     * 因为 paint() 里的等比缩放（SmoothTransformation）是逐像素重采样，
     * 比 fromImage 还贵；每帧重算一次等于白做。缩略图区尺寸在整个
     * 缓存生命周期内是常量（它只由 m_edge 决定，而 m_edge 一变就清缓存），
     * 所以缩放结果可以安全地一起缓存下来。
     *
     * 缓存策略：
     *  - 键是 itemId，值是转换好的 QPixmap，另附来源图的 cacheKey()，
     *    用来识别"同一 itemId 换了另一张图"（见 pixmapFor）。
     *  - 按总字节数设上限（见 .cpp 里的 kMaxPixmapCacheBytes）。缩略图尺寸
     *    是滑块可调的，大尺寸下单张就不小；只滚动浏览一遍大目录而不设上限，
     *    会按"浏览过的条目数"一路涨上去。超限时整体清空——重算只涉及
     *    当前可视的几十项，比起让内存无界增长更划算。
     *  - 转换时机是 GUI 线程的 paint()，也就是"尽可能晚"——
     *    正好符合 QPixmap 只能在 GUI 线程使用这一前提。
     */
    struct PixmapEntry
    {
        QPixmap pixmap;    // 已缩放到 target 尺寸
        qint64  sourceKey = 0;   // 转换来源的 QImage::cacheKey()，用于识别换图
    };
    mutable QHash<qint64, PixmapEntry> m_pixmapCache;
    mutable qint64 m_pixmapCacheBytes = 0;   // m_pixmapCache 里 pixmap 的估算总字节数

    /*!
     * \brief 取（必要时转换并缩放）某项的 QPixmap，尺寸已适配 target。
     *
     * 之所以用 mutable：绘制是 const 操作，但缓存填充是必要的副作用。
     * 这是 QStyledItemDelegate 里常规的做法。
     */
    const QPixmap *pixmapFor(const QModelIndex &index, qint64 itemId,
                             const QSize &target) const;

    /*!
     * \brief 加载中占位图的缓存。
     *
     * 占位图同样要 fromImage + 等比缩放，逐帧重算的话，
     * 一个屏幕几十项在加载期间会把重绘成本推高一个数量级 —— 而这恰恰是
     * 用户打开大目录时看到的"转半天"的观感来源之一。
     * 占位图只由「类型 + 时长文本」决定，组合数很少，缓存代价可忽略。
     */
    mutable QHash<QString, QPixmap> m_placeholderCache;

    /*! 取（必要时生成并缩放）加载中占位图，尺寸已适配 target。 */
    const QPixmap *placeholderFor(MediaType type, const QString &durationText,
                                  const QSize &target) const;

    /*! 丢弃全部转换缓存。图像源变化时调用。 */
    void clearPixmapCache() const;
};

/*!
 * \brief 中间区域的缩略图网格 / 列表视图。
 *
 * 懒加载策略（这是这一层最值得讲的部分）：
 * 视图只请求「当前可视区 + 上下各一屏」范围内的缩略图。
 * 滚动时按 150ms 去抖，停下来才批量请求，避免快速拖动滚动条时
 * 一瞬间发出几百个请求、把队列塞满。
 *
 * 已经离开视野且尚未完成的请求会被取消，省下的是磁盘 IO 与解码 CPU。
 */
class ThumbnailGrid : public QWidget
{
    Q_OBJECT

public:
    /*! 视图模式。 */
    enum ViewMode {
        GridMode,
        ListMode
    };

    explicit ThumbnailGrid(QWidget *parent = nullptr);
    ~ThumbnailGrid() override;

    void setModel(MediaModel *model, MediaFilterProxyModel *proxy);

    void setViewMode(ViewMode mode);
    ViewMode viewMode() const { return m_viewMode; }

    /*! 缩略图尺寸（滑块值 0-100 映射到 96-320 像素）。 */
    void setThumbnailSize(int sliderValue);
    int thumbnailSize() const;

    /*!
     * \brief 滑块值（0-100）→ 缩略图边长（像素）。
     *
     * 映射只在这里定义一份。工具栏要显示对应的像素值时直接调它，
     * 而不是自己再算一遍：两处各存一份映射，一旦调整范围就会出现
     * "标签上的数字和实际边长对不上"这种很难察觉的错。
     */
    static int edgeFromSlider(int sliderValue);
    /*! 缩略图边长（像素）→ 滑块值（0-100）。 */
    static int sliderFromEdge(int edge);

    /*! 当前选中项对应的资源。无选中时返回无效 item。 */
    MediaItem currentItem() const;

    /*! 全部选中项。 */
    QList<MediaItem> selectedItems() const;

    /*! 选中指定 id 的项。 */
    void selectItemById(qint64 id);

    /*! 选中一行（按模型行号）。 */
    void selectRow(int proxyRow);

    /*! 当前行号（代理模型坐标系）。 */
    int currentRow() const;

    /*! 已加载项数 / 总项数，供状态栏显示。 */
    int loadedCount() const;
    int totalCount() const;

    /*! 是否为空（没有任何可见项）。 */
    bool isEmpty() const;

    /*! 强制重新请求所有可见项的缩略图（清空缓存后用）。 */
    void refreshVisible();

signals:
    void itemActivated(const media::MediaItem &item);
    void selectionChanged(const media::MediaItem &item);
    void requestThumbnail(const media::MediaItem &item, int edge);
    void contextMenuRequested(const media::MediaItem &item, const QPoint &globalPos);

private slots:
    void onScrolled();
    void onItemClicked(const QModelIndex &index);
    void onItemDoubleClicked(const QModelIndex &index);
    void onSelectionChanged();
    void onContextMenu(const QPoint &pos);
    void requestVisibleThumbnails();

private:
    void applyViewMode();
    void setupGrid();
    void setupList();

    QStackedWidget *m_stack = nullptr;
    QListView *m_grid = nullptr;
    QTableView *m_list = nullptr;

    MediaModel *m_model = nullptr;
    MediaFilterProxyModel *m_proxy = nullptr;
    ThumbnailDelegate *m_delegate = nullptr;

    ViewMode m_viewMode = GridMode;

    /*!
     * 滚动去抖定时器。
     * 没有它的话，拖动滚动条时每一帧都会触发一次可视区计算与请求，
     * 在快速拖动几万项的场景下会造成明显的掉帧。
     */
    QTimer *m_scrollDebounce = nullptr;

    /*! 已请求过缩略图的 id 集合，避免重复请求。 */
    QSet<qint64> m_requested;

    int m_thumbEdge = 160;
};

} // namespace media

#endif // THUMBNAILGRID_H
