#include "MediaModel.h"
#include "../core/MediaDatabase.h"

#include <QDebug>

namespace media {

namespace {

/*!
 * 排序白名单。
 * 模型不把外部字符串拼进 SQL，只接受这里的键，从根源上排除注入。
 */
const QHash<QString, QString> kSortKeys = {
    { QStringLiteral("name_asc"),    QStringLiteral("file_name COLLATE NOCASE ASC") },
    { QStringLiteral("name_desc"),   QStringLiteral("file_name COLLATE NOCASE DESC") },
    { QStringLiteral("mtime_desc"),  QStringLiteral("mtime DESC") },
    { QStringLiteral("mtime_asc"),   QStringLiteral("mtime ASC") },
    { QStringLiteral("size_desc"),   QStringLiteral("size DESC") },
    { QStringLiteral("size_asc"),    QStringLiteral("size ASC") },
    { QStringLiteral("type_asc"),    QStringLiteral("type ASC, file_name COLLATE NOCASE ASC") }
};

QString resolveOrderBy(const QString &key)
{
    return kSortKeys.value(key, QStringLiteral("mtime DESC"));
}

} // namespace

MediaModel::MediaModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

MediaModel::~MediaModel() = default;

void MediaModel::setDatabase(MediaDatabase *database)
{
    if (m_database == database)
        return;

    if (m_database)
        disconnect(m_database, nullptr, this, nullptr);

    m_database = database;

    if (m_database) {
        // 库内容变化时不要盲目 reload：扫描过程中会频繁触发，
        // 全量重载会让正在滚动的视图不断跳位。交给调用方显式 reload。
        connect(m_database, &MediaDatabase::libraryChanged,
                this, [this]() {
                    // 只更新总数，不重建列表；行数据的变化由 updateItemState 精确处理
                    if (!m_database || m_virtualMode)
                        return;
                    const int newTotal = m_database->countItems(
                        m_folderPath, m_typeFilter, m_onlyFavorites,
                        m_searchText, m_includeMissing);
                    if (newTotal != m_totalCount) {
                        m_totalCount = newTotal;
                        m_hasMore = m_items.size() < m_totalCount;
                    }
                });
    }
}

void MediaModel::setThumbnailEdge(int edge)
{
    edge = qBound(64, edge, 512);
    if (m_thumbEdge == edge)
        return;

    m_thumbEdge = edge;

    // 尺寸变了，旧缩略图作废。保留条目本身，只丢图并重新请求，
    // 这样不必整表重载，视图位置也不会跳。
    if (!m_items.isEmpty()) {
        m_thumbnails.clear();
        m_thumbState.clear();

        emit dataChanged(index(0), index(m_items.size() - 1),
                         { ThumbnailRole, ThumbnailStateRole });

        // 只重置状态，不再逐行 emit thumbnailRequested。
        // 请求由视图按可视区发起（ThumbnailGrid::requestVisibleThumbnails）——
        // 模型不知道"哪一屏看得见"，对全部已加载行发请求只会让视图无从取舍。
        // （另：thumbnailRequested 目前工程内没有任何接收者，见 MediaModel.h 的说明。）
        for (int row = 0; row < m_items.size(); ++row)
            m_thumbState.insert(m_items.at(row).id, 1);
    }
}

void MediaModel::setPageSize(int size)
{
    m_pageSize = qBound(20, size, 2000);
}

void MediaModel::setSortOrder(const QString &orderBy)
{
    const QString resolved = resolveOrderBy(orderBy);
    if (m_orderBy == resolved)
        return;

    m_orderBy = resolved;
    reload();
}

int MediaModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return m_items.size();
}

QVariant MediaModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_items.size())
        return QVariant();

    return dataForRole(m_items.at(index.row()), role);
}

QVariant MediaModel::dataForRole(const MediaItem &item, int role) const
{
    switch (role) {
    case Qt::DisplayRole:
    case FileNameRole:
        return item.fileName;

    case IdRole:
        return item.id;

    case PathRole:
        return item.path;

    case TypeRole:
        return static_cast<int>(item.type);

    case SizeRole:
        return item.size;

    case SizeTextRole:
        return formatSize(item.size);

    case ModifiedRole:
        return item.modified;

    case FavoriteRole:
        return item.favorite;

    case MissingRole:
        return item.missing;

    case DimensionsRole:
        if (item.width > 0 && item.height > 0)
            return QStringLiteral("%1 × %2").arg(item.width).arg(item.height);
        return QStringLiteral("--");

    case DurationRole:
        return item.durationMs;

    case DurationTextRole:
        return item.durationMs >= 0 ? formatDuration(item.durationMs)
                                    : QStringLiteral("--");

    case TakenAtRole:
        return item.takenAt;

    case CameraRole: {
        const QString make = item.cameraMake.trimmed();
        const QString model = item.cameraModel.trimmed();
        if (make.isEmpty() && model.isEmpty())
            return QStringLiteral("--");
        if (make.isEmpty())
            return model;
        if (model.isEmpty())
            return make;
        return QStringLiteral("%1 %2").arg(make, model);
    }

    case TagsRole:
        return item.tags;

    case PlayCountRole:
        // 播放次数由数据库统计，模型侧不做缓存，避免过期数据
        if (m_database && item.id > 0)
            return m_database->playCount(item.id);
        return 0;

    case ThumbnailRole: {
        auto it = m_thumbnails.constFind(item.id);
        if (it != m_thumbnails.constEnd())
            return it.value();
        return QVariant();
    }

    case ThumbnailStateRole:
        return m_thumbState.value(item.id, 0);

    default:
        return QVariant();
    }
}

QVariant MediaModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    /*!
     * 这一列是给列表视图用的（网格视图不显示表头）。
     *
     * 之所以必须在这里给名字：QAbstractItemModel 的默认实现对
     * DisplayRole 返回 section + 1，而 QTableView 会把它当成列标题画出来，
     * 于是列表视图表头显示的是一个孤零零的 "1"。
     *
     * 也不要指望靠代理模型的 setHeaderData 来设：QSortFilterProxyModel
     * 会把调用一路转发到源模型的 setHeaderData，源模型不重写它时
     * QAbstractItemModel 的实现直接返回 false，是彻底的静默失败。
     */
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return QVariant();

    return section == 0 ? QStringLiteral("名称") : QVariant();
}

Qt::ItemFlags MediaModel::flags(const QModelIndex &index) const
{
    if (!index.isValid())
        return Qt::NoItemFlags;

    // 允许拖拽：方便把资源拖到播放器、或拖到外部程序
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled;
}

bool MediaModel::canFetchMore(const QModelIndex &parent) const
{
    if (parent.isValid())
        return false;

    // 还没拿到总数时不做判断，避免首屏就误判"已加载完"
    if (m_totalCount <= 0)
        return false;

    return m_items.size() < m_totalCount;
}

void MediaModel::fetchMore(const QModelIndex &parent)
{
    if (parent.isValid() || m_loading)
        return;

    if (!canFetchMore(parent))
        return;

    loadPage(m_items.size(), m_pageSize);
}

void MediaModel::loadPage(int offset, int limit)
{
    if (!m_database || !m_database->isOpen()) {
        emit loadError(QStringLiteral("资源库未打开"));
        return;
    }

    m_loading = true;
    emit loadingChanged(true);

    QList<MediaItem> page;

    if (!m_searchText.trimmed().isEmpty()) {
        // 带搜索文本时走库查询，覆盖全库。
        // 早期版本为了省索引只在此处做内存过滤，结果是大库里搜不到
        // 尚未加载页中的文件 —— 对用户来说等同于"文件不存在"，比慢更糟。
        page = m_database->queryItems(m_folderPath, m_typeFilter,
                                      m_onlyFavorites, offset, limit, m_orderBy,
                                      m_searchText, m_includeMissing);
    } else {
        page = m_database->queryItems(m_folderPath, m_typeFilter,
                                      m_onlyFavorites, offset, limit, m_orderBy,
                                      QString(), m_includeMissing);
    }

    if (!page.isEmpty()) {
        const int first = m_items.size();
        const int last = first + page.size() - 1;

        beginInsertRows(QModelIndex(), first, last);

        for (const MediaItem &item : page) {
            m_idToRow.insert(item.id, m_items.size());
            m_items.append(item);
        }

        endInsertRows();

        // 只标记状态，不发请求：请求由视图按可视区发起（见 ThumbnailGrid）。
        // 模型不知道哪一屏看得见，逐行发信号并不会让加载变快或变慢
        // （thumbnailRequested 目前没有接收者），但会让模型假装"全部 200 行
        // 都在加载中"，与视图实际的按需加载语义混淆。
        for (int row = first; row <= last; ++row) {
            const MediaItem &item = m_items.at(row);
            if (m_thumbState.contains(item.id))
                continue;
            m_thumbState.insert(item.id, 1);   // 1 = 加载中
        }
    }

    m_hasMore = m_items.size() < m_totalCount;

    m_loading = false;
    emit loadingChanged(false);
}

void MediaModel::setFilter(const QString &folderPath, MediaType type,
                           bool onlyFavorites, const QString &searchText)
{
    // 普通分页查询模式，退出虚拟列表模式
    m_virtualMode = false;
    m_virtualItems.clear();

    m_folderPath = folderPath;
    m_typeFilter = type;
    m_onlyFavorites = onlyFavorites;
    m_searchText = searchText;
    m_includeMissing = false;   // 普通视图只看有效记录

    reload();
}

void MediaModel::setFilterMissing()
{
    // 「失效记录」专用：与 setFilter 的区别只在 includeMissing。
    // 单独开一个入口是因为它和"普通筛选"语义互斥，混在参数里
    // 容易被误传（之前正是模型排除 missing、代理又要求 missing，
    // 两层条件互斥导致该分组恒为空）。
    m_virtualMode = false;
    m_virtualItems.clear();

    m_folderPath.clear();
    m_typeFilter = MediaType::Unknown;
    m_onlyFavorites = false;
    m_searchText.clear();
    m_includeMissing = true;

    reload();
}

void MediaModel::setVirtualItems(const QList<MediaItem> &items)
{
    m_virtualMode = true;
    m_virtualItems = items;

    reload();
}

void MediaModel::reload()
{
    beginResetModel();

    m_items.clear();
    m_idToRow.clear();
    m_thumbnails.clear();
    m_thumbState.clear();
    m_hasMore = false;

    endResetModel();

    // 虚拟列表模式：重放调用方给定的数据，不去数据库。
    // canFetchMore 依赖 m_totalCount，这里让它等于条目数，杜绝 fetchMore 取下一页。
    if (m_virtualMode) {
        if (!m_virtualItems.isEmpty()) {
            const int first = 0;
            const int last = m_virtualItems.size() - 1;

            beginInsertRows(QModelIndex(), first, last);

            for (const MediaItem &item : m_virtualItems) {
                m_idToRow.insert(item.id, m_items.size());
                m_items.append(item);
            }

            endInsertRows();
        }

        m_totalCount = m_items.size();

        for (int row = 0; row < m_items.size(); ++row)
            m_thumbState.insert(m_items.at(row).id, 1);

        return;
    }

    if (!m_database || !m_database->isOpen()) {
        m_totalCount = 0;
        emit loadError(QStringLiteral("资源库未打开"));
        return;
    }

    m_totalCount = m_database->countItems(m_folderPath, m_typeFilter, m_onlyFavorites,
                                          m_searchText, m_includeMissing);

    if (m_totalCount == 0)
        return;

    loadPage(0, m_pageSize);
}

MediaItem MediaModel::itemAt(int row) const
{
    if (row < 0 || row >= m_items.size())
        return MediaItem();
    return m_items.at(row);
}

int MediaModel::rowForId(qint64 id) const
{
    return m_idToRow.value(id, -1);
}

void MediaModel::updateItemState(qint64 id, bool favorite, const QStringList &tags)
{
    const int row = rowForId(id);
    if (row < 0)
        return;

    MediaItem &item = m_items[row];
    item.favorite = favorite;
    item.tags = tags;

    // 只刷新这一行的相关角色，不整表重绘。
    // 收藏一个文件却导致整个网格重绘，在几千项时是肉眼可见的卡顿。
    const QModelIndex idx = index(row);
    emit dataChanged(idx, idx, { FavoriteRole, TagsRole });
}

void MediaModel::refreshThumbnail(int row)
{
    if (row < 0 || row >= m_items.size())
        return;

    const MediaItem &item = m_items.at(row);
    m_thumbnails.remove(item.id);
    m_thumbState.insert(item.id, 1);

    const QModelIndex idx = index(row);
    emit dataChanged(idx, idx, { ThumbnailRole, ThumbnailStateRole });

    emit thumbnailRequested(item, m_thumbEdge, item.id);
}

void MediaModel::onThumbnailReady(qint64 itemId, const QImage &image, bool ok)
{
    // 结果可能属于已经被 reload 掉的行，这里必须重新查一次行号，
    // 不能相信请求发起时记录的位置。
    const int row = rowForId(itemId);
    if (row < 0)
        return;

    if (ok && !image.isNull())
        m_thumbnails.insert(itemId, image);

    m_thumbState.insert(itemId, ok ? 2 : 3);

    const QModelIndex idx = index(row);
    emit dataChanged(idx, idx, { ThumbnailRole, ThumbnailStateRole });
}

QHash<int, QByteArray> MediaModel::roleNames() const
{
    QHash<int, QByteArray> names;
    names[IdRole]            = "id";
    names[PathRole]          = "path";
    names[FileNameRole]      = "fileName";
    names[TypeRole]          = "type";
    names[SizeRole]          = "size";
    names[SizeTextRole]      = "sizeText";
    names[ModifiedRole]      = "modified";
    names[FavoriteRole]      = "favorite";
    names[MissingRole]       = "missing";
    names[DimensionsRole]    = "dimensions";
    names[DurationRole]      = "duration";
    names[DurationTextRole]  = "durationText";
    names[TakenAtRole]       = "takenAt";
    names[CameraRole]        = "camera";
    names[TagsRole]          = "tags";
    names[PlayCountRole]     = "playCount";
    names[ThumbnailRole]     = "thumbnail";
    names[ThumbnailStateRole] = "thumbnailState";
    return names;
}

} // namespace media
