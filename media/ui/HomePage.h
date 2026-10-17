#ifndef HOMEPAGE_H
#define HOMEPAGE_H

#include "../core/MediaTypes.h"

#include <QWidget>
#include <QList>
#include <QString>
#include <QHBoxLayout>
#include <QVBoxLayout>

class QVBoxLayout;
class QHBoxLayout;
class QLabel;
class QScrollArea;
class QPushButton;

namespace media {

class MediaDatabase;

/*!
 * \brief 首页。
 *
 * 任务书明确要求"首页不要只是简单的文件列表"，并给出了应包含的内容：
 * 最近访问的目录、最近浏览的资源、收藏入口、常用目录快捷入口、
 * 最近任务状态、以及引导用户添加目录的空状态。
 *
 * 一条硬性约束：**所有数据必须来自实际数据库状态，没有数据时展示空状态，
 * 不允许硬编码虚假的数量与历史记录**。所以这个页面的每个区块都有
 * "有数据 → 渲染真实内容 / 无数据 → 渲染引导"两条分支。
 *
 * 首页是"导航页"而不是"内容页"：它只展示少量最近项作为入口，
 * 点击后把用户送到真正的浏览界面（由 MainWindow 接管跳转）。
 */
class HomePage : public QWidget
{
    Q_OBJECT

public:
    explicit HomePage(QWidget *parent = nullptr);
    ~HomePage() override;

    void setDatabase(MediaDatabase *database);

    /*! 重新从数据库拉取统计数据并重建各区块。 */
    void refresh();

    /*!
     * \brief 如果后台有变更、首页内容已过期，则刷新一次。
     *
     * 由 MainWindow 在"用户切回首页"时调用。
     * 这样扫描期间首页不会被反复重建，切回来时又能看到最新数据。
     */
    void refreshIfNeeded();

signals:
    /*! 用户点了"添加目录"。 */
    void addFolderRequested();
    /*! 用户点了某个最近目录或常用目录。 */
    void folderRequested(const QString &path);
    /*! 用户点了某个最近浏览的资源。 */
    void itemRequested(const media::MediaItem &item);
    /*! 用户点了"图片/视频/音频/收藏"这类入口。 */
    void smartGroupRequested(const QString &group);
    /*! 用户点了"最近播放"。 */
    void recentPlaybackRequested();
    /*! 用户点了"查看全部资源"。 */
    void browseAllRequested();

private:
    /*! 一个统计卡片（大字数字 + 说明）。 */
    QWidget *makeStatCard(const QString &title, const QString &value,
                          const QString &caption, const QString &accentColorHex);

    /*!
     * \brief 一个可点击的列表项：图标 + 主标题 + 副标题。
     *
     * 返回 QPushButton* 而不是 QWidget*：调用方需要 connect 它的 clicked 信号，
     * 返回基类指针会让 &QPushButton::clicked 与实参类型不匹配（C2665）。
     * 内部确实是 QPushButton（用扁平样式做成"列表行"的外观）。
     */
    QPushButton *makeListRow(const QString &iconName, const QString &title,
                             const QString &subtitle, const QString &tooltip);

    /*! 区块标题 + 右侧动作按钮。 */
    QWidget *makeSectionHeader(const QString &title, const QString &actionText = QString());

    /*! 清空一个容器布局里的所有子部件（用于重建）。 */
    static void clearLayout(QLayout *layout);

    void buildUi();
    void rebuildStats();
    void rebuildRecentFolders();
    void rebuildRecentItems();
    void rebuildFavorites();
    void rebuildEmptyState();

    MediaDatabase *m_database = nullptr;

    QScrollArea *m_scroll = nullptr;
    QWidget     *m_content = nullptr;

    /*! 空状态页与内容页互斥显示。 */
    QWidget *m_emptyPage = nullptr;
    QWidget *m_statsRow = nullptr;
    QWidget *m_recentFoldersCard = nullptr;
    QWidget *m_recentItemsCard = nullptr;
    QWidget *m_favoritesCard = nullptr;

    /*! 各区块的内容容器，refresh 时清空重建。 */
    QVBoxLayout *m_recentFoldersLayout = nullptr;
    QVBoxLayout *m_recentItemsLayout = nullptr;
    QVBoxLayout *m_favoritesLayout = nullptr;
    QHBoxLayout *m_statsLayout = nullptr;

    /*! 无数据时提示用的文字，随状态变化。 */
    QLabel *m_emptyHint = nullptr;

    /*!
     * 库内容已变化、首页显示的还是旧数据的标记。
     *
     * 扫描时 libraryChanged 会高频触发，若每次都重建首页，
     * 会持续闪烁且明显变慢。这里标脏，等用户切回首页时再刷新。
     */
    bool m_needsRefresh = false;
};

} // namespace media

#endif // HOMEPAGE_H
