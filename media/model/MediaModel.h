#ifndef MEDIAMODEL_H
#define MEDIAMODEL_H

#include "../core/MediaTypes.h"

#include <QAbstractListModel>
#include <QHash>
#include <QImage>
#include <QStringList>

namespace media {

class MediaDatabase;

/*!
 * \brief 资源列表模型。网格视图与列表视图共用同一份数据。
 *
 * 分页策略：
 * 构造时不加载任何数据。setFilter() 触发第一页加载，canFetchMore() 返回是否还有下一页，
 * 视图滚到底部时自动调 fetchMore()。这样打开十万张图的目录也不会卡住——
 * 首屏只查 limit 条。
 *
 * 缩略图不由模型生成：模型只维护每个条目的加载状态（ThumbnailStateRole），
 * 实际的请求由视图按可视区发起（ThumbnailGrid::requestVisibleThumbnails），
 * 经 MainWindow 接到任务队列，结果通过 onThumbnailReady() 回填。
 * 这样模型不需要知道线程与缓存的存在，也不需要知道"哪一屏看得见"，可以独立测试。
 *
 * 注：thumbnailRequested 信号目前工程内没有接收者（接收端连的是
 * ThumbnailGrid::requestThumbnail）。保留声明是为了不破坏公开 API，
 * 但不要以为发它就能触发加载。
 *
 * ThumbnailRole 里放的是 QImage，不是 QPixmap。这一点很关键：
 * 缩略图是在工作线程里生成、经队列投递回主线程的，而 QPixmap 只能在
 * GUI 线程构造与使用；QVariant 想安全地跨线程携带图像数据，只能是 QImage。
 * 转 QPixmap 的动作留给绘制侧（ThumbnailDelegate）在主线程完成。
 */
class MediaModel : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Roles {
        IdRole = Qt::UserRole + 1,
        PathRole,
        FileNameRole,
        TypeRole,
        SizeRole,
        SizeTextRole,
        ModifiedRole,
        FavoriteRole,
        MissingRole,
        DimensionsRole,
        DurationRole,
        DurationTextRole,
        TakenAtRole,
        CameraRole,
        TagsRole,
        PlayCountRole,
        ThumbnailRole,      // 已就绪的缩略图（可用时）
        ThumbnailStateRole  // 0=未请求 1=加载中 2=完成 3=失败
    };

    explicit MediaModel(QObject *parent = nullptr);
    ~MediaModel() override;

    void setDatabase(MediaDatabase *database);

    /*! 缩略图边长。改变时会丢弃已加载的缩略图并重新请求。 */
    void setThumbnailEdge(int edge);
    int thumbnailEdge() const { return m_thumbEdge; }

    /*! 每页条数。默认 200。 */
    void setPageSize(int size);

    // ---------- QAbstractListModel ----------
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    /*! 列表视图的表头。基类默认返回 "section + 1"，表头会显示成 "1"。 */
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    bool canFetchMore(const QModelIndex &parent) const override;
    void fetchMore(const QModelIndex &parent) override;
    QHash<int, QByteArray> roleNames() const override;

    // ---------- 数据操作 ----------

    /*!
     * \brief 设置筛选条件并重载第一页。
     * \param folderPath 空串表示全部
     * \param type       Unknown 表示全部
     */
    void setFilter(const QString &folderPath, MediaType type,
                   bool onlyFavorites = false, const QString &searchText = QString());

    /*!
     * \brief 只看失效记录（missing = 1）。
     *
     * 与 setFilter 分开：两者语义互斥。之前「失效记录」分组是
     * 「模型排除 missing + 代理要求 missing」两层条件互斥，导致该分组恒为空。
     */
    void setFilterMissing();

    QString folderPath() const { return m_folderPath; }
    MediaType typeFilter() const { return m_typeFilter; }
    bool onlyFavorites() const { return m_onlyFavorites; }
    QString searchText() const { return m_searchText; }

    /*! 设置排序。orderBy 只接受内部白名单字符串。 */
    void setSortOrder(const QString &orderBy);

    /*! 重新加载（清空当前数据并从第一页开始）。 */
    void reload();

    /*!
     * \brief 用调用方给定的完整列表替换模型内容（虚拟列表模式）。
     *
     * 用于「最近播放」「重复项」这类不对应单条 SQL 分页查询的视图：
     * 数据由调用方一次性查好灌进来，模型不再走 loadPage，
     * 也不会因 canFetchMore 再去数据库取下一页。
     * 空列表即清空视图（"最近播放"没有记录时应显示空态，
     * 而不是错误地回退成全部资源）。
     *
     * 之后的 reload()（排序变化、库变更）会重放这份列表，不会切回数据库。
     * 要回到普通分页模式，请调用 setFilter()。
     */
    void setVirtualItems(const QList<MediaItem> &items);

    /*! 取某一行对应的完整条目。 */
    MediaItem itemAt(int row) const;

    /*! 按 id 找行号，找不到返回 -1。 */
    int rowForId(qint64 id) const;

    /*! 更新单个条目的显示状态（收藏切换等），只发对应单元格的 dataChanged。 */
    void updateItemState(qint64 id, bool favorite, const QStringList &tags);

    /*! 重新请求某一行的缩略图（例如缓存被清空后）。 */
    void refreshThumbnail(int row);

    /*! 已加载的条目总数（不含未加载的后续页）。 */
    int loadedCount() const { return m_items.size(); }

    /*! 库中的总条数（用于判断 canFetchMore）。 */
    int totalCount() const { return m_totalCount; }

    /*! 缩略图回调：由缩略图管理器在结果就绪时调用（主线程）。 */
    void onThumbnailReady(qint64 itemId, const QImage &image, bool ok);

signals:
    /*! 需要为某个资源请求缩略图。由 MainWindow 连接到底层任务队列。 */
    void thumbnailRequested(const media::MediaItem &item, int edge, qint64 requestId);

    /*! 数据加载状态变化。 */
    void loadingChanged(bool loading);
    void loadError(const QString &message);

private:
    void loadPage(int offset, int limit);
    QVariant dataForRole(const MediaItem &item, int role) const;

    MediaDatabase *m_database = nullptr;

    QList<MediaItem> m_items;
    QHash<qint64, int> m_idToRow;
    QHash<qint64, QImage> m_thumbnails;
    QHash<qint64, int> m_thumbState;

    QString   m_folderPath;
    MediaType m_typeFilter = MediaType::Unknown;
    bool      m_onlyFavorites = false;
    QString   m_searchText;
    bool      m_includeMissing = false;   // true = 只看失效记录
    QString   m_orderBy = QStringLiteral("mtime DESC");

    /*!
     * 虚拟列表模式（最近播放 / 重复项）。
     * 开启后 reload() 重放 m_virtualItems 而不是查库，
     * canFetchMore 恒为 false（m_totalCount 与条目数相等）。
     */
    bool m_virtualMode = false;
    QList<MediaItem> m_virtualItems;

    int m_pageSize = 200;
    int m_totalCount = 0;
    int m_thumbEdge = 160;
    bool m_loading = false;
    bool m_hasMore = false;
};

} // namespace media

#endif // MEDIAMODEL_H
