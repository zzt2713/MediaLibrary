#ifndef SIDEBARWIDGET_H
#define SIDEBARWIDGET_H

#include "../core/MediaTypes.h"
// 需要完整的 MediaDatabase 定义：成员里按值持有 MediaDatabase::Stats，
// 前向声明只能用于指针与引用，不能用于按值的嵌套类型。
#include "../core/MediaDatabase.h"

#include <QWidget>
#include <QString>
#include <QHash>

class QListWidget;
class QListWidgetItem;
class QTreeWidget;
class QTreeWidgetItem;
class QLineEdit;
class QLabel;
class QStackedWidget;
class QTimer;

namespace media {

/*!
 * \brief 左侧导航栏：智能分组 + 目录树 + 标签。
 *
 * 三种导航合在一个控件里，用堆叠页切换而不是塞进同一棵树。
 * 原因：智能分组（所有图片/收藏/最近）和物理目录是两类概念，
 * 混在一棵树里会让用户分不清「点这个是筛选还是展开目录」。
 * 参考的界面截图也是把它们分成两块。
 */
class SidebarWidget : public QWidget
{
    Q_OBJECT

public:
    explicit SidebarWidget(QWidget *parent = nullptr);
    ~SidebarWidget() override;

    void setDatabase(MediaDatabase *database);

    /*! 重新从数据库加载目录树与计数。会立刻刷新，不走去抖。 */
    void reload();

    /*!
     * \brief 用缓存的计数值刷新界面，不查数据库。
     *
     * 供外部在"已经知道库变了"之后频繁调用而不产生查询开销。
     * 需要拿到最新计数时请调 refreshCountsFromDatabase()。
     */
    void updateCounts();

    /*!
     * \brief 立刻查库并刷新计数与标签列表，不走去抖。
     *
     * updateCounts() 只重绘缓存值，不重建侧边栏的标签清单。
     * 所以标签增删之后必须调这个：删掉某条目最后一个"旅行"标签时，
     * 侧边栏标签区还留着"旅行"，点它筛选出空结果。
     */
    void refreshCountsFromDatabase();

    /*! 当前选中的智能分组。返回空串表示选中的是目录。 */
    QString currentSmartGroup() const { return m_currentSmartGroup; }

    /*! 当前选中的目录（智能分组模式下为空）。 */
    QString currentFolder() const { return m_currentFolder; }

    /*! 外部（如打开项目）触发选中某个目录。 */
    void selectFolder(const QString &path);

    /*!
     * \brief 外部触发选中某个智能分组。
     *
     * 与 selectFolder 对称，供首页的入口按钮使用：首页点了"图片"，
     * 左侧的高亮必须跟着走，否则用户会看到一个没有高亮的选中态。
     * 走这个方法而不是让 MainWindow 自己拼 QListWidgetItem：
     * 侧边栏的内部结构（user role 存什么、怎么清目录树的选中）不该外泄。
     */
    void selectSmartGroup(const QString &group);

    /*!
     * \brief 设置当前生效的标签筛选，并同步侧边栏高亮。
     *
     * 不发出 tagSelected，因此不会形成 MainWindow → SidebarWidget → MainWindow
     * 的回路。供外部在"筛选条件由别处改变"后调用，例如切分组、切目录时
     * 标签条件被清空，侧边栏要跟着取消高亮。
     */
    void setActiveTag(const QString &tag);

    /*! 当前高亮的标签。空串表示未按标签筛选。 */
    QString activeTag() const { return m_activeTag; }

    /*! 展开/折叠所有目录。 */
    void expandAll();
    void collapseAll();

    /* ==================== 分组键常量 ====================
     *
     * 必须是 public：MainWindow::applySmartGroup() 需要按这些键分发筛选逻辑。
     * 若为 private，外部只能硬编码 "all"/"images" 这类字面量，
     * 一旦改名就会静默失配（筛选不生效但不报错），非常难排查。
     *
     * GroupHome 与其余项性质不同：首页不对应任何 SQL 条件，只是导航目标。
     * 但放进同一份常量是为了让"侧边栏能选中什么"只有一处定义。
     *
     * 位置要求：必须在本行之前有显式的 public:，
     * 且不能落在 signals: 或 private slots: 区间内 ——
     * 否则 moc 会把它们当作信号/槽声明，报
     * "Not a signal or slot declaration"。
     */
    static const QString GroupHome;
    static const QString GroupAll;
    static const QString GroupImages;
    static const QString GroupVideos;
    static const QString GroupAudios;
    static const QString GroupFavorites;
    static const QString GroupRecent;
    static const QString GroupDuplicates;
    static const QString GroupMissing;

signals:
    /*! 选中了智能分组。group 取值见上面的 Group* 常量。 */
    void smartGroupSelected(const QString &group, const QString &displayName);
    /*! 选中了某个目录。 */
    void folderSelected(const QString &path);
    /*! 右键菜单请求在文件管理器中打开。 */
    void revealInExplorerRequested(const QString &path);
    /*! 请求把目录加入资源库。 */
    void addFolderRequested();
    /*! 请求移除某个目录。 */
    void removeFolderRequested(const QString &path);
    /*! 选中了某个标签。\c tag 为空串表示清除标签筛选。 */
    void tagSelected(const QString &tag);
    /*! 请求删除某个标签（从全部资源上解绑）。 */
    void tagDeleteRequested(const QString &tag);

private slots:
    void onSmartGroupClicked(QListWidgetItem *item);
    void onFolderClicked(QTreeWidgetItem *item, int column);
    void onFolderContextMenu(const QPoint &pos);
    void onSmartContextMenu(const QPoint &pos);
    void onTagContextMenu(const QPoint &pos);
    void onFilterTextChanged(const QString &text);

private:
    void buildSmartGroups();
    void rebuildFolderTree();
    QTreeWidgetItem *addFolderNode(QTreeWidgetItem *parent, const QString &path,
                                   const QHash<QString, int> &counts, int depth);
    void setCountBadge(QListWidgetItem *item, int count);
    void applyFilter(const QString &text, QTreeWidgetItem *parent = nullptr);

    QListWidget *m_smartList = nullptr;
    QLineEdit   *m_folderFilter = nullptr;
    QTreeWidget *m_folderTree = nullptr;
    QLabel      *m_tagTitle = nullptr;
    QListWidget *m_tagList = nullptr;

    MediaDatabase *m_database = nullptr;

    /*!
     * 计数刷新去抖定时器。
     * 扫描期间 libraryChanged 每 64 个文件触发一次，必须合并，
     * 否则 stats() + duplicateGroups() + folderCounts() + 重建目录树
     * 会被执行上百次，扫描速度会明显下降。
     */
    QTimer *m_countDebounce = nullptr;

    /*! 上一次查到的计数值。updateCounts() 只渲染它，不查库。 */
    MediaDatabase::Stats m_cachedStats;
    int m_cachedRecentCount = 0;
    int m_cachedDuplicateGroupCount = 0;

    QString m_currentSmartGroup;
    QString m_currentFolder;

    /*! 当前生效的标签筛选条件。空串表示不限。 */
    QString m_activeTag;

    /*! 目录路径 → 计数。一次查询拿全，避免逐节点查库。 */
    QHash<QString, int> m_folderCounts;

    /*! 智能分组项指针，更新计数时用。 */
    QHash<QString, QListWidgetItem *> m_smartItems;

    /*! 目录树最大展开深度。超过这个深度的层不预建节点，避免建一棵巨树。 */
    static constexpr int kMaxTreeDepth = 4;
};

} // namespace media

#endif // SIDEBARWIDGET_H
