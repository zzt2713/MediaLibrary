#include "MediaFilterProxyModel.h"

namespace media {

MediaFilterProxyModel::MediaFilterProxyModel(QObject *parent)
    : QSortFilterProxyModel(parent)
{
    // 大小写不敏感，且不依赖 locale：搜索文件名时这是用户期望的行为
    setFilterCaseSensitivity(Qt::CaseInsensitive);
    setSortCaseSensitivity(Qt::CaseInsensitive);

    // 排序在代理里做，因为可能按"尺寸/类型"排序而它们不体现在显示文本上
    setDynamicSortFilter(true);

    sort(0, Qt::DescendingOrder);
}

void MediaFilterProxyModel::setSearchText(const QString &text)
{
    if (m_searchText == text)
        return;

    m_searchText = text;
    invalidateFilter();
}

void MediaFilterProxyModel::setTypeFilter(MediaType type)
{
    if (m_typeFilter == type)
        return;

    m_typeFilter = type;
    invalidateFilter();
}

void MediaFilterProxyModel::setOnlyFavorites(bool only)
{
    if (m_onlyFavorites == only)
        return;

    m_onlyFavorites = only;
    invalidateFilter();
}

void MediaFilterProxyModel::setOnlyMissing(bool only)
{
    if (m_onlyMissing == only)
        return;

    m_onlyMissing = only;
    invalidateFilter();
}

void MediaFilterProxyModel::setDateRange(const QDateTime &from, const QDateTime &to)
{
    if (m_dateFrom == from && m_dateTo == to)
        return;

    m_dateFrom = from;
    m_dateTo = to;
    invalidateFilter();
}

void MediaFilterProxyModel::setSizeRange(qint64 minBytes, qint64 maxBytes)
{
    if (m_minSize == minBytes && m_maxSize == maxBytes)
        return;

    m_minSize = minBytes;
    m_maxSize = maxBytes;
    invalidateFilter();
}

void MediaFilterProxyModel::setRequiredTags(const QStringList &tags)
{
    if (m_requiredTags == tags)
        return;

    m_requiredTags = tags;
    invalidateFilter();
}

void MediaFilterProxyModel::clearFilters()
{
    m_searchText.clear();
    m_typeFilter = MediaType::Unknown;
    m_onlyFavorites = false;
    m_onlyMissing = false;
    m_dateFrom = QDateTime();
    m_dateTo = QDateTime();
    m_minSize = -1;
    m_maxSize = -1;
    m_requiredTags.clear();

    invalidateFilter();
}

void MediaFilterProxyModel::setSortKey(const QString &key)
{
    if (m_sortKey == key)
        return;

    m_sortKey = key;
    sort(0, Qt::AscendingOrder);   // lessThan 内部按 key 决定方向
    invalidate();
}

int MediaFilterProxyModel::filteredOutCount() const
{
    QAbstractItemModel *src = sourceModel();
    if (!src)
        return 0;

    return qMax(0, src->rowCount() - rowCount());
}

int MediaFilterProxyModel::visibleCount() const
{
    return rowCount();
}

int MediaFilterProxyModel::sourceRowFor(int proxyRow) const
{
    const QModelIndex proxyIdx = index(proxyRow, 0);
    if (!proxyIdx.isValid())
        return -1;

    return mapToSource(proxyIdx).row();
}

bool MediaFilterProxyModel::filterAcceptsRow(int sourceRow,
                                             const QModelIndex &sourceParent) const
{
    QAbstractItemModel *src = sourceModel();
    if (!src)
        return false;

    const QModelIndex idx = src->index(sourceRow, 0, sourceParent);
    if (!idx.isValid())
        return false;

    // 文本搜索：匹配文件名。
    // 不做模糊匹配算法（如拼音首字母）——那属于可扩展项，
    // 这里保证行为可预期：子串包含即可。
    if (!m_searchText.isEmpty()) {
        const QString name = src->data(idx, MediaModel::FileNameRole).toString();
        if (!name.contains(m_searchText, Qt::CaseInsensitive))
            return false;
    }

    if (m_typeFilter != MediaType::Unknown) {
        const int t = src->data(idx, MediaModel::TypeRole).toInt();
        if (t != static_cast<int>(m_typeFilter))
            return false;
    }

    if (m_onlyFavorites && !src->data(idx, MediaModel::FavoriteRole).toBool())
        return false;

    if (m_onlyMissing) {
        if (!src->data(idx, MediaModel::MissingRole).toBool())
            return false;
    } else {
        // 默认隐藏失效记录：它们只是保留元数据，不该混在正常浏览里
        if (src->data(idx, MediaModel::MissingRole).toBool())
            return false;
    }

    if (m_dateFrom.isValid() || m_dateTo.isValid()) {
        const QDateTime dt = src->data(idx, MediaModel::ModifiedRole).toDateTime();
        if (m_dateFrom.isValid() && dt < m_dateFrom)
            return false;
        if (m_dateTo.isValid() && dt > m_dateTo)
            return false;
    }

    if (m_minSize >= 0 || m_maxSize >= 0) {
        const qint64 size = src->data(idx, MediaModel::SizeRole).toLongLong();
        if (m_minSize >= 0 && size < m_minSize)
            return false;
        if (m_maxSize >= 0 && size > m_maxSize)
            return false;
    }

    if (!m_requiredTags.isEmpty()) {
        // 要求全部命中（AND 语义）。"包含任一"（OR）在标签量大时噪音太大，
        // 用户更常见的心智模型是"缩小范围"。
        const QStringList itemTags = src->data(idx, MediaModel::TagsRole).toStringList();
        for (const QString &tag : m_requiredTags) {
            if (!itemTags.contains(tag, Qt::CaseInsensitive))
                return false;
        }
    }

    return true;
}

bool MediaFilterProxyModel::lessThan(const QModelIndex &left,
                                     const QModelIndex &right) const
{
    const int lType = left.data(MediaModel::TypeRole).toInt();
    const int rType = right.data(MediaModel::TypeRole).toInt();

    // 升/降序由键名后缀决定，因此这里统一返回"按升序规则比较"的结果。
    // 调用方 setSortKey 时固定用 AscendingOrder，避免两处方向叠加相消。
    const bool descending = m_sortKey.endsWith(QLatin1String("_desc"));

    auto compareLess = [descending](auto lv, auto rv) -> bool {
        return descending ? (lv > rv) : (lv < rv);
    };

    if (m_sortKey.startsWith(QLatin1String("name"))) {
        const QString ln = left.data(MediaModel::FileNameRole).toString();
        const QString rn = right.data(MediaModel::FileNameRole).toString();
        // 文件名用 localeAwareCompare，让 "img2" 排在 "img10" 前面
        const int cmp = QString::localeAwareCompare(ln, rn);
        return descending ? (cmp > 0) : (cmp < 0);
    }

    if (m_sortKey.startsWith(QLatin1String("size")))
        return compareLess(left.data(MediaModel::SizeRole).toLongLong(),
                           right.data(MediaModel::SizeRole).toLongLong());

    if (m_sortKey.startsWith(QLatin1String("type"))) {
        if (lType != rType)
            return compareLess(lType, rType);
        const QString ln = left.data(MediaModel::FileNameRole).toString();
        const QString rn = right.data(MediaModel::FileNameRole).toString();
        return QString::localeAwareCompare(ln, rn) < 0;
    }

    // 默认按修改时间。用毫秒时间戳比较，避免 QDateTime 在时区上的歧义。
    const qint64 lt = left.data(MediaModel::ModifiedRole).toDateTime().toMSecsSinceEpoch();
    const qint64 rt = right.data(MediaModel::ModifiedRole).toDateTime().toMSecsSinceEpoch();
    return compareLess(lt, rt);
}

} // namespace media
