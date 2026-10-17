#ifndef MEDIAFILTERPROXYMODEL_H
#define MEDIAFILTERPROXYMODEL_H

#include "MediaModel.h"

#include <QSortFilterProxyModel>
#include <QDateTime>

namespace media {

/*!
 * \brief 在已加载数据之上做筛选与排序的代理模型。
 *
 * 职责边界（这点必须讲清楚，否则容易被问倒）：
 *  - 分页、总量、SQL 查询在 MediaModel 里做，代理不碰数据库。
 *  - 代理只处理「当前已加载的那个子集」的过滤与排序。
 *
 * 为什么不全放到 SQL 里：
 * 切筛选条件时若每次都走数据库，需要重置模型、重建分页，视图会跳到顶部，
 * 用户在滚动中改一下"只看收藏"就丢失位置，体验很差。
 * 放代理里切换是纯内存操作，即时生效且保留滚动位置。
 *
 * 代价：筛选范围限于已加载的页。搜索框里的文本由 MediaModel 单独走一次
 * 库查询兜底，两者配合覆盖完整（README 中说明）。
 */
class MediaFilterProxyModel : public QSortFilterProxyModel
{
    Q_OBJECT

public:
    explicit MediaFilterProxyModel(QObject *parent = nullptr);

    // ---------- 筛选条件 ----------

    void setSearchText(const QString &text);
    QString searchText() const { return m_searchText; }

    void setTypeFilter(MediaType type);
    MediaType typeFilter() const { return m_typeFilter; }

    void setOnlyFavorites(bool only);
    bool onlyFavorites() const { return m_onlyFavorites; }

    void setOnlyMissing(bool only);
    bool onlyMissing() const { return m_onlyMissing; }

    /*! 时间范围筛选。传无效 QDateTime 表示不限。 */
    void setDateRange(const QDateTime &from, const QDateTime &to);

    /*! 大小范围筛选（字节）。负值表示不限。 */
    void setSizeRange(qint64 minBytes, qint64 maxBytes);

    /*! 标签筛选：要求条目包含全部指定标签。空列表表示不限。 */
    void setRequiredTags(const QStringList &tags);

    /*! 一次性清空所有筛选条件。 */
    void clearFilters();

    // ---------- 排序 ----------

    /*! 用直观的键名设置排序，内部映射到 sort(column, order)。 */
    void setSortKey(const QString &key);
    QString sortKey() const { return m_sortKey; }

    /*! 当前被代理过滤掉多少行，用于状态栏提示"已筛选" */
    int filteredOutCount() const;

    /*! 可见（通过筛选）的行数。 */
    int visibleCount() const;

    /*! 便捷方法：取第 row 行在源模型里的行号。 */
    int sourceRowFor(int proxyRow) const;

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override;
    bool lessThan(const QModelIndex &left, const QModelIndex &right) const override;

private:
    QString   m_searchText;
    MediaType m_typeFilter = MediaType::Unknown;
    bool      m_onlyFavorites = false;
    bool      m_onlyMissing = false;
    QDateTime m_dateFrom;
    QDateTime m_dateTo;
    qint64    m_minSize = -1;
    qint64    m_maxSize = -1;
    QStringList m_requiredTags;
    QString   m_sortKey = QStringLiteral("mtime_desc");
};

} // namespace media

#endif // MEDIAFILTERPROXYMODEL_H
