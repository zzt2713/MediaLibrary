#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "../core/MediaTypes.h"
#include "../core/MediaDatabase.h"
#include "../core/ThumbnailCache.h"

#include <QMainWindow>
#include <QHash>
#include <QSet>
#include <QImage>

class QSplitter;
class QStackedWidget;

namespace media {

class SidebarWidget;
class ThumbnailGrid;
class DetailPane;
class PreviewDialog;
class MediaModel;
class MediaFilterProxyModel;
class TaskManager;
class TaskPanelWidget;
class HomePage;
class SettingsManager;
class FileWatcher;
class RenameService;
class TopBarWidget;
class StatusBarWidget;
class EmptyStateWidget;

/*!
 * \brief 应用主窗口。负责组装各模块并协调它们之间的交互。
 *
 * 这个类刻意承担"协调者"的角色，而不承载业务逻辑：
 * 扫描在 MediaScanner，存储在 MediaDatabase，异步在 TaskManager，
 * 查重在 DuplicateService，展示在 MediaModel/ThumbnailGrid，编辑在 DetailPane。
 * 主窗口只做「把这些连起来 + 处理用户操作」。
 *
 * 界面上"自己有一摊状态"的部分已经各自成模块：
 *  - TopBarWidget    工具条（搜索/排序/视图模式/尺寸，控件状态内部联动）
 *  - StatusBarWidget 状态栏（计数/选中/扫描进度/缓存统计，含自动刷新）
 *  - EmptyStateWidget 空状态页（三种原因的引导文案）
 * 主窗口只往里喂数据、收语义化信号，不再直接操作它们的子控件。
 *
 * 菜单保留在本类内（buildMenus）。菜单项不持有任何状态，只发出一次性的
 * 动作，拆成独立类之后仍要来回传同一套回调，属于为形式而拆；
 * 但从 connectSignals 里挪了出去，让那个函数只做信号连接。
 *
 * 线程归属（这是本项目最需要能讲清楚的一点）：
 *  - 主线程：所有 UI 对象、MediaModel、ThumbnailCache（有内部锁）、
 *    FileWatcher、TaskManager 对象本身（它的 slot 在主线程执行）
 *  - 工作线程：ScanTask / ThumbnailTask 的 runTask()，
 *    它们直接调 MediaDatabase（每线程独立连接，安全）
 *
 * 缩略图结果从工作线程回到主线程有一条明确的路径，见 onThumbnailResult。
 *
 * 这条路径上传递的图像数据一律是 QImage。QPixmap 依赖平台后端存储、
 * 只能在 GUI 线程使用，任何跨线程传递它的写法都是未定义行为；
 * QImage 是纯 CPU 侧缓冲区，经 Qt::QueuedConnection 投递是安全的。
 * QPixmap 只在 UI 类内部（ThumbnailDelegate / DetailPane）按需转换。
 */
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private slots:
    // 导航
    void onSmartGroupSelected(const QString &group, const QString &displayName);
    void onFolderSelected(const QString &path);

    // 缩略图
    void onThumbnailRequested(const media::MediaItem &item, int edge);
    void onThumbnailResult(qint64 itemId, const media::ThumbKey &key,
                           const QImage &image, bool ok);

    // 扫描
    void onScanProgress(const media::ScanProgress &progress);
    void onScanFinished(const QString &rootPath, int affected, int total);
    void onScanWarning(const QString &path, const QString &message);

    // 目录变更
    void onDirectoryChanged(const QString &rootPath);

    // 交互
    void onItemActivated(const media::MediaItem &item);
    void onSelectionChanged(const media::MediaItem &item);
    void onItemContextMenu(const media::MediaItem &item, const QPoint &globalPos);
    void onFavoriteToggled(qint64 itemId, bool favorite);
    /*! F2 / 右键「重命名」。只改数据库索引路径，不动磁盘文件。
     *  \param preselected 右键菜单带上的目标；空条目（默认）表示用当前选中项。 */
    void onRenameRequested(const media::MediaItem &preselected = media::MediaItem());
    void onTagAdded(qint64 itemId, const QString &tag);
    void onTagRemoved(qint64 itemId, const QString &tag);

    /*! 标签变更后统一同步模型、详情面板与侧边栏（onTagAdded/onTagRemoved 共用）。 */
    void syncTagsToUi(qint64 itemId);

    // 工具条（信号来自 TopBarWidget）
    void onAddFolderClicked();
    void onRefreshClicked();
    void onSearchTextChanged(const QString &text);
    /*! 搜索框回车：把搜索词下推到数据库层，覆盖全库而不只是已加载页。 */
    void onSearchSubmitted(const QString &text);
    void onSortKeyChanged(const QString &key);
    void onViewModeChanged(bool gridMode);
    void onThumbnailSizeChanged(int value);
    void onSettingsClicked();

    // 菜单动作
    void onClearThumbnailCache();
    void onClearPlaybackHistory();
    void onPurgeMissing();
    void onShowPerformanceInfo();
    void onShowAbout();

    // 首页导航
    void onHomeFolderRequested(const QString &path);
    void onHomeItemRequested(const media::MediaItem &item);
    void onHomeSmartGroupRequested(const QString &group);

    // 任务面板
    void onTaskCancelAll();
    void onTaskCancelThumbnails();

    // 状态
    void updateStatusBar();
    void updateEmptyState();

private:
    void buildUi();
    void buildMenus();
    void buildStatusBar();
    void connectSignals();

    /*! 启动时恢复上次的目录列表并做增量扫描。 */
    void restoreLibrary();

    /*! 发起一次目录扫描。 */
    void startScan(const QString &path, bool markMissing = true);

    /*! 打开预览窗口。 */
    void openPreview(const MediaItem &item, bool buildPlaylist);

    /*! 计算某个智能分组对应的筛选条件并应用。 */
    void applySmartGroup(const QString &group);

    /*! 当前智能分组对应的媒体类型（用于搜索时保留分组上下文）。 */
    media::MediaType typeFilterForCurrentGroup() const;

    /*!
     * \brief 触发一次重复检测（精确 MD5 或感知哈希），并把结果展示出来。
     *
     * 算法在 DuplicateService，这里只负责进度对话框与结果提示。
     * 同步执行会阻塞界面，是大库上的已知限制（README 有记录）。
     */
    void runDuplicateDetection(bool perceptual);

    /*!
     * \brief 打开"重复项"分组，把指定分组内容作为虚拟数据喂给模型。
     *
     * 精确与感知两种查重共用这条收尾路径：结果已经由 DuplicateService 算好，
     * 这里只负责展示。
     */
    void showDuplicateGroups(const QList<QList<MediaItem>> &groups);

    /*! 移除某个根目录（含确认对话框）。 */
    void removeFolder(const QString &path);

    /*! 把当前视图里的条目转成预览播放列表。 */
    QList<MediaItem> currentViewItems() const;

    // ---------- 核心对象 ----------
    MediaDatabase   *m_database = nullptr;
    ThumbnailCache  *m_thumbCache = nullptr;
    TaskManager     *m_taskManager = nullptr;
    FileWatcher     *m_fileWatcher = nullptr;
    SettingsManager *m_settings = nullptr;
    RenameService   *m_rename = nullptr;

    MediaModel *m_model = nullptr;
    MediaFilterProxyModel *m_proxy = nullptr;

    // ---------- UI ----------
    TopBarWidget    *m_topBar = nullptr;
    StatusBarWidget *m_statusBar = nullptr;
    SidebarWidget   *m_sidebar = nullptr;
    ThumbnailGrid   *m_grid = nullptr;
    DetailPane      *m_detail = nullptr;
    HomePage        *m_homePage = nullptr;
    TaskPanelWidget *m_taskPanel = nullptr;
    QSplitter       *m_splitter = nullptr;

    /*! 中间区的三页堆叠：首页 / 空状态 / 网格。 */
    QStackedWidget  *m_centerStack = nullptr;
    EmptyStateWidget *m_emptyPage = nullptr;

    PreviewDialog *m_preview = nullptr;

    // ---------- 状态 ----------
    QString m_currentFolder;
    QString m_currentSmartGroup;

    /*! 正在扫描的根目录集合，用于状态栏提示。 */
    QSet<QString> m_activeScans;

    /*! 扫描期间挂起文件监控，避免自己的写操作触发事件循环。 */
    bool m_scanInProgress = false;

    /*! 上次自动扫描的时间，防止监控风暴导致反复扫描。 */
    QHash<QString, qint64> m_lastScanTime;

    /*! 把设置应用到界面与各模块（启动时与设置窗口关闭后各调一次）。 */
    void applySettings();
    /*! 把当前界面状态写回设置（关窗时调用）。 */
    void saveSettings();
};

} // namespace media

#endif // MAINWINDOW_H
