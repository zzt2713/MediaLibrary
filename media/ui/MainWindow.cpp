#include "MainWindow.h"
#include "Metrics.h"
#include "SidebarWidget.h"
#include "ThumbnailGrid.h"
#include "DetailPane.h"
#include "PreviewDialog.h"
#include "HomePage.h"
#include "TaskPanelWidget.h"
#include "SettingsDialog.h"
#include "TopBarWidget.h"
#include "StatusBarWidget.h"
#include "EmptyStateWidget.h"
#include "Icons.h"
#include "ThemeManager.h"

#include "../model/MediaModel.h"
#include "../model/MediaFilterProxyModel.h"
#include "../tasks/TaskManager.h"
#include "../tasks/FileWatcher.h"
#include "../core/DuplicateService.h"
#include "../core/RenameService.h"
#include "../core/MediaScanner.h"
#include "../core/SettingsManager.h"
#include "../core/AppPaths.h"
#include "../core/Logger.h"

#include <QVBoxLayout>
#include <QProgressDialog>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QMenuBar>
#include <QMenu>
#include <QAction>
#include <QMessageBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFile>
#include <QInputDialog>
#include <QDir>
#include <QDateTime>
#include <QCloseEvent>
#include <QKeyEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QDesktopServices>
#include <QUrl>
#include <QProcess>
#include <QApplication>
#include <QClipboard>
#include <QStandardPaths>
#include <QDebug>
#include <QEventLoop>
#include <algorithm>

namespace media {

namespace {

/*!
 * 自动扫描的最小间隔。
 *
 * 文件系统监控在复制大文件夹时会连着报很多次变更；
 * 即使 FileWatcher 已经做了去抖，这里再加一道"同一目录 3 秒内不重复扫描"，
 * 避免极端情况下扫描任务堆积。
 */
constexpr qint64 kMinRescanIntervalMs = 3000;

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("媒体资源库"));
    /*!
     * 最小宽度必须与三个面板的最小宽度自洽：
     *   侧栏 180 + 中心 320 + 详情 260 = 760
     * 之前这里写 560，小于三者之和。QSplitter 在总宽不足时只能违反子部件的
     * minimumWidth 来满足 setSizes，结果是两侧被压得比设定值更窄，
     * 侧边栏文字被截断——用户看到的不是"正常的小窗口"，而是挤压变形。
     * 更窄的窗口要靠允许面板收起（见下方 setChildrenCollapsible）来腾空间，
     * 而不是让主窗口宣称一个做不到的下限。
     */
    setMinimumSize(760, 560);
    resize(1280, 800);

    // 接受文件拖放：把文件夹拖进来即添加为资源目录
    setAcceptDrops(true);

    // ---------- 核心对象 ----------
    m_settings = new SettingsManager(this);
    m_database = new MediaDatabase(this);
    m_thumbCache = new ThumbnailCache(this);
    m_taskManager = new TaskManager(this);
    m_fileWatcher = new FileWatcher(this);
    m_rename = new RenameService(m_database, this);

    m_taskManager->initialize(m_database, m_thumbCache);

    // ---------- 模型 ----------
    m_model = new MediaModel(this);
    m_proxy = new MediaFilterProxyModel(this);
    m_proxy->setSourceModel(m_model);

    buildUi();
    buildMenus();
    buildStatusBar();
    connectSignals();

    // ---------- 日志级别 ----------
    Logger::instance().setMinimumLevel(
        Logger::levelFromString(m_settings->logLevel()));

    // ---------- 数据库 ----------
    if (!m_database->open(AppPaths::databaseFile())) {
        QMessageBox::critical(this, QStringLiteral("资源库打开失败"),
                              QStringLiteral("无法打开资源库数据库：\n%1\n\n"
                                             "程序仍可使用，但不会保存索引与收藏。")
                                  .arg(m_database->lastError()));
    } else {
        m_model->setDatabase(m_database);
        m_sidebar->setDatabase(m_database);
        m_detail->setDatabase(m_database);
        m_homePage->setDatabase(m_database);
        restoreLibrary();
    }

    // ---------- 应用设置 ----------
    applySettings();

    // 与侧边栏默认高亮（首页）保持一致。
    // restoreLibrary() 可能已经把中心页切到空态页（首运行时），
    // 这里显式切回首页并重新设好当前分组，避免两处状态不一致。
    m_currentSmartGroup = SidebarWidget::GroupHome;
    m_centerStack->setCurrentWidget(m_homePage);

    // 恢复上次浏览的目录（设置开启且目录仍在库里时）
    if (m_settings->restoreLastFolder()) {
        const QString last = m_settings->lastFolder();
        if (!last.isEmpty() && m_database->isOpen()
            && m_database->folderExists(last)) {
            m_sidebar->selectFolder(last);
        }
    }

    updateStatusBar();
    updateEmptyState();
}

MainWindow::~MainWindow()
{
    /*!
     * 析构顺序很重要，这里是本项目最容易崩的地方之一：
     *
     * 1. 先停文件监控，避免它再触发新扫描
     * 2. 取消所有任务并等线程结束 —— 必须做，否则工作线程还在访问
     *    MediaDatabase，而它随后会被析构，导致 use-after-free
     * 3. 挂起扫描标记
     * 4. 最后才让 database/cache 随 QObject 树析构
     */
    if (m_fileWatcher)
        m_fileWatcher->setSuspended(true);

    if (m_taskManager)
        m_taskManager->shutdown(8000);

    // 停掉状态栏的自动刷新：收尾阶段数据库与缓存正在被释放，
    // 定时器再跑一轮就会去读已经不该动的东西。
    if (m_statusBar)
        m_statusBar->stopAutoRefresh();

    if (m_database && m_database->isOpen())
        m_database->close();
}

// ================================================================ UI 构建

void MainWindow::buildUi()
{
    auto *central = new QWidget(this);
    auto *rootLayout = new QVBoxLayout(central);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    m_topBar = new TopBarWidget(central);
    rootLayout->addWidget(m_topBar);

    // ---------- 三栏主体 ----------
    m_splitter = new QSplitter(Qt::Horizontal, central);
    /*!
     * 允许子面板被压到 0 收起。
     *
     * 之前是 setChildrenCollapsible(false)，配合侧栏 180 + 详情 260 的下限，
     * 窗口拖窄时中间的资源区先被挤没——而它才是主内容区。
     * 现在窄窗口下由 QSplitter 自己决定收起侧栏或检查器，
     * 保证中间始终可用。收起后仍可通过拖动分隔条拉回来。
     */
    m_splitter->setChildrenCollapsible(true);
    m_splitter->setHandleWidth(1);
    m_splitter->setObjectName(QStringLiteral("MainSplitter"));

    m_sidebar = new SidebarWidget(m_splitter);
    m_sidebar->setMinimumWidth(Metrics::Panel::SidebarMin);
    m_sidebar->setMaximumWidth(Metrics::Panel::SidebarMax);

    // 中间：首页 / 空状态 / 网格 三页堆叠
    m_centerStack = new QStackedWidget(m_splitter);

    m_homePage = new HomePage(m_centerStack);

    m_emptyPage = new EmptyStateWidget(m_centerStack);
    connect(m_emptyPage, &EmptyStateWidget::addFolderRequested,
            this, &MainWindow::onAddFolderClicked);

    m_grid = new ThumbnailGrid(m_centerStack);

    // 资源区是三栏里的主内容，必须有下限：窄窗口时宁可收起侧栏与检查器，
    // 也不能把这里压成一条缝。网格至少能横放两列缩略图（每列约 150px）。
    m_centerStack->setMinimumWidth(320);

    m_centerStack->addWidget(m_homePage);
    m_centerStack->addWidget(m_emptyPage);
    m_centerStack->addWidget(m_grid);
    // 默认落在首页：与侧边栏"首页"默认高亮保持一致
    m_centerStack->setCurrentWidget(m_homePage);

    m_detail = new DetailPane(m_splitter);
    m_detail->setDatabase(m_database);

    m_splitter->addWidget(m_sidebar);
    m_splitter->addWidget(m_centerStack);
    m_splitter->addWidget(m_detail);

    // 初始比例：侧栏 220，中间吃掉剩余，详情 320。
    // 不把中间写死成固定值——窗口变窄时它应当是被牺牲的最后一个。
    m_splitter->setStretchFactor(0, 0);
    m_splitter->setStretchFactor(1, 1);
    m_splitter->setStretchFactor(2, 0);
    m_splitter->setSizes({ 222, 636, 322 });

    rootLayout->addWidget(m_splitter, 1);

    // ---------- 任务面板（状态栏上方，可折叠） ----------
    m_taskPanel = new TaskPanelWidget(central);
    m_taskPanel->setTaskManager(m_taskManager);
    rootLayout->addWidget(m_taskPanel);

    setCentralWidget(central);

    // 模型接到视图上
    m_grid->setModel(m_model, m_proxy);
}

void MainWindow::buildStatusBar()
{
    m_statusBar = new StatusBarWidget(m_thumbCache, this);

    statusBar()->addPermanentWidget(m_statusBar, 1);
    statusBar()->setSizeGripEnabled(true);
}

// ================================================================ 菜单

void MainWindow::buildMenus()
{
    auto *fileMenu = menuBar()->addMenu(QStringLiteral("文件(&F)"));

    auto *addAction = fileMenu->addAction(Icons::get(Icons::Add, ThemeManager::textSecondary(), 16),
                                          QStringLiteral("添加目录…"));
    addAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+O")));
    connect(addAction, &QAction::triggered, this, &MainWindow::onAddFolderClicked);

    auto *refreshAction = fileMenu->addAction(Icons::get(Icons::Refresh, ThemeManager::textSecondary(), 16),
                                              QStringLiteral("重新扫描"));
    refreshAction->setShortcut(QKeySequence::Refresh);
    connect(refreshAction, &QAction::triggered, this, &MainWindow::onRefreshClicked);

    fileMenu->addSeparator();

    auto *settingsAction = fileMenu->addAction(Icons::get(Icons::Settings, ThemeManager::textSecondary(), 16),
                                               QStringLiteral("设置…"));
    settingsAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+,")));
    connect(settingsAction, &QAction::triggered, this, &MainWindow::onSettingsClicked);

    fileMenu->addSeparator();

    auto *quitAction = fileMenu->addAction(QStringLiteral("退出"));
    quitAction->setShortcut(QKeySequence::Quit);
    connect(quitAction, &QAction::triggered, this, &QWidget::close);

    // ---------- 工具菜单 ----------
    auto *toolMenu = menuBar()->addMenu(QStringLiteral("工具(&T)"));

    auto *dupAction = toolMenu->addAction(Icons::get(Icons::Duplicate, ThemeManager::textSecondary(), 16),
                                          QStringLiteral("查找完全相同的文件（MD5）"));
    connect(dupAction, &QAction::triggered, this, [this]() { runDuplicateDetection(false); });

    auto *similarAction = toolMenu->addAction(Icons::get(Icons::Duplicate, ThemeManager::textSecondary(), 16),
                                              QStringLiteral("查找视觉相似的图片（感知哈希）"));
    connect(similarAction, &QAction::triggered, this, [this]() { runDuplicateDetection(true); });

    toolMenu->addSeparator();

    auto *clearCacheAction = toolMenu->addAction(Icons::get(Icons::Trash, ThemeManager::textSecondary(), 16),
                                                 QStringLiteral("清空缩略图缓存"));
    connect(clearCacheAction, &QAction::triggered, this, &MainWindow::onClearThumbnailCache);

    auto *clearPlaybackAction = toolMenu->addAction(Icons::get(Icons::Clear, ThemeManager::textSecondary(), 16),
                                                    QStringLiteral("清空播放记录"));
    connect(clearPlaybackAction, &QAction::triggered, this, &MainWindow::onClearPlaybackHistory);

    toolMenu->addSeparator();

    auto *purgeAction = toolMenu->addAction(Icons::get(Icons::Trash, ThemeManager::dangerColor(), 16),
                                            QStringLiteral("清理失效记录…"));
    connect(purgeAction, &QAction::triggered, this, &MainWindow::onPurgeMissing);

    // ---------- 帮助 ----------
    auto *helpMenu = menuBar()->addMenu(QStringLiteral("帮助(&H)"));

    auto *perfAction = helpMenu->addAction(Icons::get(Icons::Info, ThemeManager::textSecondary(), 16),
                                           QStringLiteral("性能与统计信息"));
    connect(perfAction, &QAction::triggered, this, &MainWindow::onShowPerformanceInfo);

    auto *aboutAction = helpMenu->addAction(QStringLiteral("关于"));
    connect(aboutAction, &QAction::triggered, this, &MainWindow::onShowAbout);
}

// ================================================================ 信号连接

void MainWindow::connectSignals()
{
    // ---------- 侧边栏 ----------
    connect(m_sidebar, &SidebarWidget::smartGroupSelected,
            this, &MainWindow::onSmartGroupSelected);
    connect(m_sidebar, &SidebarWidget::folderSelected,
            this, &MainWindow::onFolderSelected);
    connect(m_sidebar, &SidebarWidget::addFolderRequested,
            this, &MainWindow::onAddFolderClicked);
    connect(m_sidebar, &SidebarWidget::removeFolderRequested,
            this, &MainWindow::removeFolder);
    connect(m_sidebar, &SidebarWidget::revealInExplorerRequested,
            this, [this](const QString &path) {
                // 返回值必须查：失败时静默无反应的话，用户无法区分
                // "没响应"和"打开了别的窗口"
                if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path)))
                    QMessageBox::warning(this, QStringLiteral("打开目录失败"),
                                         QStringLiteral("系统未能打开：\n%1").arg(path));
            });
    connect(m_sidebar, &SidebarWidget::tagSelected, this,
            [this](const QString &tag) {
        // 空串来自侧边栏的"不限标签"行：清掉条件。
        // 侧边栏的高亮已经由它自己更新，这里不再回调回去（否则成环）。
        m_proxy->setRequiredTags(tag.isEmpty() ? QStringList() : QStringList{ tag });
        m_sidebar->setActiveTag(tag);
        updateStatusBar();
    });
    connect(m_sidebar, &SidebarWidget::tagDeleteRequested, this,
            [this](const QString &tag) {
        if (!m_database || !m_database->isOpen())
            return;

        const auto answer = QMessageBox::question(
            this, QStringLiteral("删除标签"),
            QStringLiteral("删除标签「%1」？\n\n该标签会从所有资源上移除，"
                           "之后不能再用它筛选。").arg(tag));
        if (answer != QMessageBox::Yes)
            return;

        if (!m_database->deleteTag(tag)) {
            QMessageBox::warning(this, QStringLiteral("删除标签失败"),
                                 QStringLiteral("无法删除标签「%1」：\n%2")
                                     .arg(tag, m_database->lastError()));
            return;
        }

        // 正用它筛选时先清掉条件，否则结果列表会空着却看不出原因
        if (m_sidebar->activeTag() == tag)
            m_proxy->setRequiredTags(QStringList());

        // 标签挂在所有资源上，网格每行的标签角标都要跟着变，整表重载最省事；
        // 侧边栏标签清单重建（refreshCountsFromDatabase 会重查 allTags）；
        // 详情面板的 chip 也要刷新。
        m_model->reload();
        m_sidebar->refreshCountsFromDatabase();
        if (m_detail->currentItemId() > 0) {
            const MediaItem item = m_database->itemById(m_detail->currentItemId());
            if (item.isValid())
                m_detail->showItem(item);
        }
        updateStatusBar();
        m_statusBar->showTransientMessage(
            QStringLiteral("已删除标签「%1」").arg(tag), 3000);
    });

    // ---------- 首页 ----------
    connect(m_homePage, &HomePage::addFolderRequested,
            this, &MainWindow::onAddFolderClicked);
    connect(m_homePage, &HomePage::folderRequested,
            this, &MainWindow::onHomeFolderRequested);
    connect(m_homePage, &HomePage::itemRequested,
            this, &MainWindow::onHomeItemRequested);
    connect(m_homePage, &HomePage::smartGroupRequested,
            this, &MainWindow::onHomeSmartGroupRequested);
    connect(m_homePage, &HomePage::recentPlaybackRequested, this, [this]() {
        m_sidebar->selectSmartGroup(SidebarWidget::GroupRecent);
    });

    // ---------- 任务面板 ----------
    connect(m_taskPanel, &TaskPanelWidget::cancelAllRequested,
            this, &MainWindow::onTaskCancelAll);
    connect(m_taskPanel, &TaskPanelWidget::cancelThumbnailsRequested,
            this, &MainWindow::onTaskCancelThumbnails);

    // ---------- 网格 ----------
    connect(m_grid, &ThumbnailGrid::requestThumbnail,
            this, &MainWindow::onThumbnailRequested);
    connect(m_grid, &ThumbnailGrid::itemActivated,
            this, &MainWindow::onItemActivated);
    connect(m_grid, &ThumbnailGrid::selectionChanged,
            this, &MainWindow::onSelectionChanged);
    connect(m_grid, &ThumbnailGrid::contextMenuRequested,
            this, &MainWindow::onItemContextMenu);

    // ---------- 详情面板 ----------
    connect(m_detail, &DetailPane::favoriteToggled,
            this, &MainWindow::onFavoriteToggled);
    connect(m_detail, &DetailPane::tagAdded,
            this, &MainWindow::onTagAdded);
    connect(m_detail, &DetailPane::tagRemoved,
            this, &MainWindow::onTagRemoved);
    connect(m_detail, &DetailPane::playRequested,
            this, [this](const MediaItem &item) { openPreview(item, true); });

    // ---------- 工具条（控件状态在 TopBarWidget 内部维护） ----------
    connect(m_topBar, &TopBarWidget::addFolderRequested,
            this, &MainWindow::onAddFolderClicked);
    connect(m_topBar, &TopBarWidget::refreshRequested,
            this, &MainWindow::onRefreshClicked);
    connect(m_topBar, &TopBarWidget::settingsRequested,
            this, &MainWindow::onSettingsClicked);
    connect(m_topBar, &TopBarWidget::searchTextChanged,
            this, &MainWindow::onSearchTextChanged);
    connect(m_topBar, &TopBarWidget::searchSubmitted,
            this, &MainWindow::onSearchSubmitted);
    connect(m_topBar, &TopBarWidget::sortKeyChanged,
            this, &MainWindow::onSortKeyChanged);
    connect(m_topBar, &TopBarWidget::viewModeChanged,
            this, &MainWindow::onViewModeChanged);
    connect(m_topBar, &TopBarWidget::thumbnailSizeChanged,
            this, &MainWindow::onThumbnailSizeChanged);

    // ---------- 扫描与缩略图（统一经 TaskManager） ----------
    connect(m_taskManager, &TaskManager::scanProgress, this,
            [this](const ScanProgress &progress, const QString &) {
                onScanProgress(progress);
            });
    connect(m_taskManager, &TaskManager::scanWarning,
            this, &MainWindow::onScanWarning);
    connect(m_taskManager, &TaskManager::scanFinished,
            this, &MainWindow::onScanFinished);
    connect(m_taskManager, &TaskManager::thumbnailReady,
            this, &MainWindow::onThumbnailResult);
    connect(m_taskManager, &TaskManager::thumbnailFailed, this,
            [this](qint64 itemId, const QString &reason) {
                Q_UNUSED(itemId)
                // 缩略图失败不弹窗（大目录里常有坏图），只记日志
                LOG_DEBUG("Thumbnail") << "failed: " << reason;
            });

    // ---------- 文件监控 ----------
    connect(m_fileWatcher, &FileWatcher::directoryChanged,
            this, &MainWindow::onDirectoryChanged);

    // ---------- 后台任务状态（状态栏提示） ----------
    connect(m_taskManager, &TaskManager::tasksChanged, this, [this]() {
        if (m_scanInProgress)
            return;

        const int pending = m_taskManager->pendingThumbnailCount();
        m_statusBar->setScanMessage(pending > 0
                                        ? QStringLiteral("正在加载缩略图 %1 项…").arg(pending)
                                        : QString());
    });
}

// ================================================================ 菜单动作
//
// 菜单项本身不持有状态，动作体直接放在这几个槽里。
// 把它们从 connectSignals 的 lambda 搬出来，是为了让连接函数只做连接。

void MainWindow::onClearThumbnailCache()
{
    const auto answer = QMessageBox::question(
        this, QStringLiteral("清空缩略图缓存"),
        QStringLiteral("将删除所有已生成的缩略图缓存文件。\n"
                       "下次浏览会重新生成（可能较慢）。\n\n确定继续吗？"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

    if (answer != QMessageBox::Yes)
        return;

    m_thumbCache->clearAll();
    m_grid->refreshVisible();

    // 缓存被清空后内存统计归零，状态栏要立刻反映出来，
    // 否则会显示上一次的旧数字直到自动刷新到点。
    m_statusBar->refreshCacheStats();
}

void MainWindow::onClearPlaybackHistory()
{
    const auto answer = QMessageBox::question(
        this, QStringLiteral("清空播放记录"),
        QStringLiteral("确定要清空全部播放记录吗？此操作不可撤销。"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

    if (answer == QMessageBox::Yes) {
        m_database->clearPlaybackHistory();
        m_sidebar->updateCounts();
    }
}

void MainWindow::onPurgeMissing()
{
    const MediaDatabase::Stats s = m_database->stats();
    if (s.missing == 0) {
        QMessageBox::information(this, QStringLiteral("清理失效记录"),
                                 QStringLiteral("当前没有失效记录。"));
        return;
    }

    const auto answer = QMessageBox::question(
        this, QStringLiteral("清理失效记录"),
        QStringLiteral("资源库中有 %1 条失效记录（文件已不存在）。\n"
                       "清理会连同它们的收藏与标签一起删除。\n\n确定继续吗？")
            .arg(s.missing),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

    if (answer == QMessageBox::Yes) {
        const int n = m_database->purgeMissing();
        QMessageBox::information(this, QStringLiteral("清理完成"),
                                 QStringLiteral("已清理 %1 条记录。").arg(n));
        m_model->reload();
        m_sidebar->reload();
        updateStatusBar();
        updateEmptyState();
    }
}

void MainWindow::onShowPerformanceInfo()
{
    const MediaDatabase::Stats s = m_database->stats();
    const ThumbnailCache::Stats c = m_thumbCache->stats();
    const int total = c.memoryHits + c.diskHits + c.misses;
    const double rate = total > 0 ? (c.memoryHits + c.diskHits) * 100.0 / total : 0.0;

    QMessageBox box(this);
    box.setWindowTitle(QStringLiteral("性能与统计信息"));
    box.setTextFormat(Qt::RichText);
    box.setText(QStringLiteral(
        "<b>资源库</b><br>"
        "总条目：%1（图片 %2 / 视频 %3 / 音频 %4）<br>"
        "收藏：%5 · 失效：%6<br>"
        "总大小：%7<br><br>"
        "<b>缩略图缓存</b><br>"
        "内存条目：%8（%9）<br>"
        "内存命中：%10 · 磁盘命中：%11 · 未命中：%12<br>"
        "命中率：%13%<br>"
        "缓存目录：%14<br><br>"
        "<b>当前会话</b><br>"
        "任务并发上限：%15<br>"
        "已加载 / 总计：%16 / %17<br>"
        "当前筛选后可见：%18")
        .arg(s.total).arg(s.images).arg(s.videos).arg(s.audios)
        .arg(s.favorites).arg(s.missing)
        .arg(formatSize(s.totalBytes))
        .arg(c.memoryEntries).arg(formatSize(c.memoryBytes))
        .arg(c.memoryHits).arg(c.diskHits).arg(c.misses)
        .arg(QString::number(rate, 'f', 1))
        .arg(c.cacheDir)
        .arg(m_taskManager->concurrency())
        .arg(m_model->loadedCount()).arg(m_model->totalCount())
        .arg(m_proxy->rowCount()));
    box.exec();
}

void MainWindow::onShowAbout()
{
    QMessageBox::about(this, QStringLiteral("关于"),
        QStringLiteral("<b>媒体资源库</b><br><br>"
                       "基于 C++17 / Qt Widgets 的本地媒体资源管理器。<br>"
                       "支持图片、视频、音频的索引、浏览、预览与播放。<br><br>"
                       "关键技术：Qt Multimedia、SQLite、QThreadPool 异步任务、<br>"
                       "两级缩略图缓存、感知哈希查重、文件系统监控。"));
}


// ================================================================ 启动与扫描

void MainWindow::restoreLibrary()
{
    const QList<QString> folders = m_database->allFolderPaths();

    if (folders.isEmpty()) {
        updateEmptyState();
        return;
    }

    // 让文件监控盯上这些目录（受"实时监控"设置控制）
    if (m_settings->watchFileSystem())
        m_fileWatcher->setRoots(folders);

    /*!
     * 启动时不立即全量扫描。
     *
     * 理由：全量扫描在大库上要几十秒，会让程序一打开就像卡住。
     * 先直接查库把已有内容显示出来（瞬时），
     * 再由 FileWatcher + 用户手动刷新来保证一致性。
     *
     * 如果库是空的（首次运行），则必须扫一次，否则用户什么都看不到。
     * "启动时自动扫描"设置打开时也走这条路径。
     */
    const int count = m_database->countItems();
    if (count == 0 || m_settings->scanOnStartup()) {
        for (const QString &folder : folders)
            startScan(folder);
    }

    updateStatusBar();
    updateEmptyState();
}

void MainWindow::startScan(const QString &path, bool markMissing)
{
    if (path.isEmpty())
        return;

    if (!m_database || !m_database->isOpen()) {
        QMessageBox::warning(this, QStringLiteral("无法扫描"),
                             QStringLiteral("资源库未打开，无法保存扫描结果。"));
        return;
    }

    // 先归一化：TaskManager 发出的 scanFinished 带的是 cleanPath 后的路径，
    // 两边必须一致，否则 onScanFinished 里的 remove 匹配不上，
    // m_activeScans 会残留，界面永远停在"扫描中"。
    const QString norm = QDir::cleanPath(QDir::fromNativeSeparators(path));

    // 同一目录正在扫描时不重复发起。
    // 用户连点刷新很容易触发，重复扫描会浪费大量 IO。
    if (m_taskManager->isScanning(norm)) {
        LOG_DEBUG("Task") << "scan already running for " << norm;
        return;
    }

    m_activeScans.insert(norm);
    m_scanInProgress = true;

    // 扫描期间挂起监控：扫描会读文件、更新 mtime，某些系统上会触发变更事件，
    // 不挂起可能造成"扫描→事件→再扫描"的循环。
    m_fileWatcher->setSuspended(true);

    m_statusBar->beginScanProgress();
    m_statusBar->setScanMessage(QStringLiteral("正在扫描 %1…").arg(QFileInfo(norm).fileName()));

    // 扫描配置（递归/EXIF/隐藏文件）由设置界面写入 TaskManager，
    // 这里不重复传一遍——两处各存一份配置迟早会不一致。
    // 预统计与符号链接策略是全局固定的，也由 TaskManager 内部设置。
    if (!m_taskManager->startScan(norm, markMissing)) {
        m_activeScans.remove(norm);
        if (m_activeScans.isEmpty()) {
            m_scanInProgress = false;
            m_statusBar->setScanProgressVisible(false);
            m_statusBar->setScanMessage(QString());
            if (m_settings->watchFileSystem())
                m_fileWatcher->setSuspended(false);
        }
    }
}

void MainWindow::onScanProgress(const ScanProgress &progress)
{
    m_statusBar->setScanProgress(progress.discovered, progress.total);
}

void MainWindow::onScanFinished(const QString &rootPath, int affected, int total)
{
    m_activeScans.remove(rootPath);
    m_lastScanTime.insert(rootPath, QDateTime::currentMSecsSinceEpoch());

    if (m_activeScans.isEmpty()) {
        m_scanInProgress = false;
        m_statusBar->setScanProgressVisible(false);
        m_statusBar->setScanMessage(QString());

        // 扫描结束，恢复监控，并把新扫描出的目录纳入监控范围。
        // "实时监控"设置关闭时保持挂起，只做一次性的手动刷新。
        if (m_settings->watchFileSystem()) {
            m_fileWatcher->setSuspended(false);
            m_fileWatcher->setRoots(m_database->allFolderPaths());
        }

        // 首页显示的是聚合统计，扫描后需要跟着更新
        m_homePage->refreshIfNeeded();
    }

    qDebug() << "[MainWindow] scan finished:" << rootPath
             << "affected:" << affected << "discovered:" << total;

    // 扫描写库后刷新界面。
    // 这里不每次扫描都 reload：如果同时有多个目录在扫，
    // 中途 reload 会让列表反复跳。等最后一个扫完再统一刷。
    if (m_activeScans.isEmpty()) {
        m_model->reload();
        m_sidebar->reload();
        m_detail->setDatabase(m_database);
        updateStatusBar();
        updateEmptyState();
    }
}

void MainWindow::onScanWarning(const QString &path, const QString &message)
{
    // 警告不弹窗：一次扫描可能有几十个无权限目录，
    // 弹窗会让人无法操作。只在状态栏显示最近一条。
    qWarning() << "[Scan]" << path << message;
    m_statusBar->showTransientMessage(
        QStringLiteral("跳过 %1：%2").arg(QFileInfo(path).fileName(), message));
}

void MainWindow::onDirectoryChanged(const QString &rootPath)
{
    // 节流：同一目录短时间内多次变更只扫一次
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 last = m_lastScanTime.value(rootPath, 0);

    if (now - last < kMinRescanIntervalMs) {
        qDebug() << "[MainWindow] rescan throttled for" << rootPath;
        return;
    }

    qDebug() << "[MainWindow] directory changed, rescanning" << rootPath;
    startScan(rootPath);
}

// ================================================================ 导航

void MainWindow::onSmartGroupSelected(const QString &group, const QString &displayName)
{
    Q_UNUSED(displayName)

    m_currentSmartGroup = group;
    m_currentFolder.clear();

    // 切换分组会让视图内容整个换掉，旧的缩略图已无意义。
    // 用任务级取消而非队列级：队列里可能还有正在进行的扫描，不该被一起停掉。
    if (m_taskManager)
        m_taskManager->cancelThumbnails();

    if (group == SidebarWidget::GroupHome) {
        // 首页不对应任何 SQL 筛选：只切页面并刷新数据。
        // 模型保持原样（不 reload），这样从首页切回资源页时列表还在。
        m_homePage->refreshIfNeeded();
        m_centerStack->setCurrentWidget(m_homePage);
        m_detail->clear();
        updateStatusBar();
        return;
    }

    applySmartGroup(group);
    updateStatusBar();
    updateEmptyState();

    // 切分组时清掉详情面板，避免显示上一个上下文里的条目
    m_detail->clear();

    // 目录树取消选中，避免两处高亮
    // （SidebarWidget 内部已经处理，这里不再重复）
}

void MainWindow::applySmartGroup(const QString &group)
{
    // 先清掉上一次的筛选，避免条件叠加
    m_proxy->clearFilters();
    m_proxy->setOnlyMissing(false);

    // 清筛选时一并把搜索框清空：
    // 代理里的搜索词已经被 clearFilters() 抹掉了，若界面上还留着字，
    // 用户会看到"框里写着 abc、列表却是全量"这种界面与状态不一致。
    m_topBar->setSearchText(QString());

    // 切分组时标签筛选一并清除。
    // clearFilters() 只清了代理内部的条件，侧边栏自己记的高亮不会跟着变——
    // 不同步就会出现"列表已恢复全量、侧边栏还高亮着某个标签"的矛盾状态。
    // setActiveTag 不回发 tagSelected，不会形成信号回路。
    m_sidebar->setActiveTag(QString());

    // 分组键直接引用 SidebarWidget 的公开常量，
    // 不在这里再抄一份字面量——两处各存一份迟早会改漏一处。

    if (group == SidebarWidget::GroupAll) {
        m_model->setFilter(QString(), MediaType::Unknown, false);

    } else if (group == SidebarWidget::GroupImages) {
        m_model->setFilter(QString(), MediaType::Image, false);

    } else if (group == SidebarWidget::GroupVideos) {
        m_model->setFilter(QString(), MediaType::Video, false);

    } else if (group == SidebarWidget::GroupAudios) {
        m_model->setFilter(QString(), MediaType::Audio, false);

    } else if (group == SidebarWidget::GroupFavorites) {
        m_model->setFilter(QString(), MediaType::Unknown, true);

    } else if (group == SidebarWidget::GroupRecent) {
        // 最近播放：从数据库取，作为虚拟列表灌进模型。
        // 这类"虚拟列表"不适合走分页查询，条数有限（最多 500），一次性给完。
        // 空记录时清空视图，让用户看到空态，而不是错误地回退成全部资源。
        m_model->setVirtualItems(m_database->recentPlayed(500));

    } else if (group == SidebarWidget::GroupDuplicates) {
        // 重复项：先在库里查一次分组，把属于重复组的所有条目拿来展示
        const QHash<QString, QList<MediaItem>> groups = m_database->duplicateGroups();

        QList<MediaItem> flats;
        for (auto it = groups.constBegin(); it != groups.constEnd(); ++it)
            flats.append(it.value());

        // 按路径排序，让同一组的文件相邻显示，便于肉眼比对
        std::sort(flats.begin(), flats.end(), [](const MediaItem &a, const MediaItem &b) {
            if (a.contentHash != b.contentHash)
                return a.contentHash < b.contentHash;
            return a.path < b.path;
        });

        m_model->setVirtualItems(flats);

    } else if (group == SidebarWidget::GroupMissing) {
        // 失效记录：模型直接查 missing = 1 的行。
        // 不能写成「模型查有效行 + 代理只留失效行」——那两层条件互斥，
        // 该分组永远为空（侧边栏角标却非零，两边互相打脸）。
        m_model->setFilterMissing();
        // 代理默认会把失效行滤掉（它们平时不该混在正常浏览里），
        // 这里显式反转，让失效行通过。
        m_proxy->setOnlyMissing(true);
    }
}

void MainWindow::onFolderSelected(const QString &path)
{
    m_currentFolder = path;
    m_currentSmartGroup.clear();

    // 换目录了，之前排队的缩略图已不属于当前视图，取消掉
    if (m_taskManager)
        m_taskManager->cancelThumbnails();

    m_model->setFilter(path, MediaType::Unknown, false);
    m_proxy->clearFilters();

    // 切目录同样清掉标签筛选：上一个目录下打的标签到了新目录没有意义，
    // 而侧边栏的标签清单是按全库统计的，留着会让人以为"这个目录只有这一个标签"。
    m_sidebar->setActiveTag(QString());

    m_detail->clear();
    updateStatusBar();
    updateEmptyState();

    // 选中目录时顺便扫一次：用户主动点目录，通常是希望看到最新内容。
    // 但只在目录确实存在于资源库时扫，避免对树里任意子目录都触发全量扫描。
    if (m_database && m_database->folderExists(path))
        startScan(path);
}

// ================================================================ 缩略图

void MainWindow::onThumbnailRequested(const MediaItem &item, int edge)
{
    if (!item.isValid() || !m_taskManager)
        return;

    // 缓存命中判定、重复请求合并、任务登记与取消都在 TaskManager 里。
    // 结果统一经 thumbnailReady 信号回到 onThumbnailResult（见 connectSignals）。
    m_taskManager->requestThumbnail(item, edge);
}

void MainWindow::onThumbnailResult(qint64 itemId, const ThumbKey &key,
                                   const QImage &image, bool ok)
{
    // 交给模型更新对应行。模型会按 id 重新定位行号，
    // 所以这里不需要关心请求发出后列表是否已经变化。
    // 模型里存的也是 QImage，转换由绘制侧负责。
    m_model->onThumbnailReady(itemId, image, ok);

    // 如果详情面板正显示这个资源，回填预览图。
    // setPreviewImage 内部（GUI 线程）会把 QImage 转成 QPixmap。
    if (m_detail->currentItemId() == itemId && ok && !image.isNull())
        m_detail->setPreviewImage(image);

    Q_UNUSED(key)
}

// ================================================================ 交互

void MainWindow::onItemActivated(const MediaItem &item)
{
    openPreview(item, true);
}

void MainWindow::openPreview(const MediaItem &item, bool buildPlaylist)
{
    if (!item.isValid())
        return;

    if (!m_preview) {
        m_preview = new PreviewDialog(this);
        m_preview->setDatabase(m_database);

        /*!
         * 连接只在这里做一次。
         *
         * m_preview 是复用实例（见 openPreview 末尾不重建），
         * 所以不能把 connect 移到 openPreview 里每次打开都连一遍——
         * 那样播放记录会被写多次。
         */
        connect(m_preview, &PreviewDialog::playbackRecorded, this,
                [this](qint64 itemId, double positionMs) {
                    if (!m_settings->recordPlaybackHistory())
                        return;
                    if (m_database && itemId > 0)
                        m_database->recordPlayback(itemId, positionMs);
                });
    }

    // 音量是运行时设置项：设置界面改完要立刻对已实例化的预览生效
    m_preview->setVolume(m_settings->volume());
    // 续播开关同理。之前这个设置项只被写、从未被读，
    // 用户关掉"记住播放位置并续播"之后视频依然从上次位置开始。
    m_preview->setResumePlayback(m_settings->resumePlayback());
    m_preview->setAutoPlayNext(m_settings->autoPlayNext());

    if (buildPlaylist) {
        // 播放列表用「当前视图里已加载的项」，且经过代理筛选，
        // 这样在"只看收藏"视图里按方向键不会跳到未收藏的图。
        const QList<MediaItem> items = currentViewItems();

        int index = -1;
        for (int i = 0; i < items.size(); ++i) {
            if (items.at(i).id == item.id) {
                index = i;
                break;
            }
        }

        m_preview->setPlaylist(items, index);
    }

    m_preview->openItem(item);
    m_preview->show();
    m_preview->raise();
    m_preview->activateWindow();

    // 记录一次播放（打开即算一次）。
    // 位置要沿用已保存的进度，不能直接写 0：recordPlayback 是 UPSERT，
    // 写 0 会把上次看到一半的位置抹掉，视频的「续播」就永远回不到原处。
    // 首次打开（无记录）时 lastPosition 返回 0，正好是期望的初始值。
    if (m_settings->recordPlaybackHistory() && m_database && item.id > 0)
        m_database->recordPlayback(item.id, m_database->lastPosition(item.id));
}

QList<MediaItem> MainWindow::currentViewItems() const
{
    QList<MediaItem> result;
    if (!m_proxy || !m_model)
        return result;

    const int rows = m_proxy->rowCount();
    result.reserve(rows);

    for (int row = 0; row < rows; ++row) {
        const QModelIndex proxyIdx = m_proxy->index(row, 0);
        const int sourceRow = m_proxy->mapToSource(proxyIdx).row();
        const MediaItem item = m_model->itemAt(sourceRow);
        if (item.isValid())
            result.append(item);
    }

    return result;
}

void MainWindow::onSelectionChanged(const MediaItem &item)
{
    const QList<MediaItem> selected = m_grid->selectedItems();

    if (selected.size() > 1) {
        // 多选：显示汇总而不是最后一个的详情
        qint64 totalBytes = 0;
        for (const MediaItem &sel : selected)
            totalBytes += sel.size;

        m_detail->showMultiSelection(selected.size(), totalBytes);
    } else if (item.isValid()) {
        m_detail->showItem(item);

        // 详情面板的预览需要缩略图。这里主动请求一次，
        // 因为该资源可能在网格里还没被请求过（比如键盘选中）。
        // 请求走 TaskManager：命中缓存会立即发 thumbnailReady，
        // 结果统一回到 onThumbnailResult，由它回填详情面板（按 currentItemId 匹配）。
        if (item.type == MediaType::Image) {
            const int edge = qMax(200, m_grid->thumbnailSize());
            m_taskManager->requestThumbnail(item, edge);
        }
    } else {
        m_detail->clear();
    }

    updateStatusBar();
}

void MainWindow::onItemContextMenu(const MediaItem &item, const QPoint &globalPos)
{
    QMenu menu(this);

    /*!
     * "打开预览"的图标按媒体类型选择。
     *
     * 之前三类媒体统一用 Icons::Play：对视频/音频是对的，
     * 但图片项前面挂一个播放三角，会让人以为点下去会开始播放。
     */
    const QIcon openIcon = Icons::get(
        item.type == MediaType::Image ? Icons::Image : Icons::Play,
        ThemeManager::textSecondary(), 16);

    QAction *open = menu.addAction(openIcon, QStringLiteral("打开预览"));
    QAction *reveal = menu.addAction(Icons::get(Icons::Folder, ThemeManager::textSecondary(), 16),
                                     QStringLiteral("在文件管理器中显示"));
    menu.addSeparator();
    QAction *copyPath = menu.addAction(QStringLiteral("复制完整路径"));
    QAction *copyName = menu.addAction(QStringLiteral("复制文件名"));
    menu.addSeparator();
    QAction *rename = menu.addAction(QStringLiteral("重命名…"));
    menu.addSeparator();
    QAction *favorite = menu.addAction(
        Icons::get(item.favorite ? Icons::StarFilled : Icons::Star,
                   item.favorite ? ThemeManager::favoriteColor() : ThemeManager::textSecondary(), 16),
        item.favorite ? QStringLiteral("取消收藏") : QStringLiteral("添加到收藏"));
    menu.addSeparator();
    QAction *detail = menu.addAction(Icons::get(Icons::Info, ThemeManager::textSecondary(), 16),
                                     QStringLiteral("刷新详细信息"));

    QAction *chosen = menu.exec(globalPos);
    if (!chosen)
        return;

    if (chosen == open) {
        openPreview(item, true);
    } else if (chosen == reveal) {
        /*!
         * Windows 上用 explorer /select, 可以定位并选中文件；
         * 其他平台退化为打开所在目录。
         */
#ifdef Q_OS_WIN
        const QString nativePath = QDir::toNativeSeparators(item.path);
        bool revealed = QProcess::startDetached(QStringLiteral("explorer.exe"),
                                { QStringLiteral("/select,") + nativePath });
        // explorer 启动失败时退化为打开所在目录，总比毫无反应好
        if (!revealed)
            revealed = QDesktopServices::openUrl(QUrl::fromLocalFile(item.folderPath));
#else
        bool revealed = QDesktopServices::openUrl(QUrl::fromLocalFile(item.folderPath));
#endif
        if (!revealed)
            QMessageBox::warning(this, QStringLiteral("打开目录失败"),
                                 QStringLiteral("系统未能打开：\n%1").arg(item.folderPath));
    } else if (chosen == copyPath) {
        QApplication::clipboard()->setText(item.path);
    } else if (chosen == copyName) {
        QApplication::clipboard()->setText(item.fileName);
    } else if (chosen == rename) {
        onRenameRequested(item);
    } else if (chosen == favorite) {
        onFavoriteToggled(item.id, !item.favorite);
    } else if (chosen == detail) {
        m_detail->showItem(item);
    }
}

void MainWindow::onFavoriteToggled(qint64 itemId, bool favorite)
{
    if (!m_database || itemId <= 0)
        return;

    if (!m_database->setFavorite(itemId, favorite)) {
        QMessageBox::warning(this, QStringLiteral("操作失败"),
                             QStringLiteral("无法更新收藏状态。"));
        return;
    }

    // 只更新对应行，不整表重载。收藏一个 5000 项列表里的图
    // 不该让整个视图重绘。
    m_model->updateItemState(itemId, favorite, m_database->tagsForItem(itemId));

    // 侧边栏的收藏计数要跟着变
    m_sidebar->updateCounts();
    updateStatusBar();
}

void MainWindow::onRenameRequested(const MediaItem &preselected)
{
    if (!m_database || !m_database->isOpen())
        return;

    // 右键菜单带上了目标条目；F2 取当前选中项
    const MediaItem item = preselected.isValid() ? preselected : m_grid->currentItem();

    if (!item.isValid()) {
        m_statusBar->showTransientMessage(QStringLiteral("请先选中一个资源"), 2000);
        return;
    }

    // 改名前的名字，用于下面的成功提示
    const QString oldName = item.fileName;

    /*!
     * 业务逻辑在 RenameService 里（5 项校验 + 先磁盘后索引两阶段写入）。
     * 这里只负责"成功了就刷新界面"这件属于主窗口的事。
     *
     * 这段流程留在本类时，单独一个槽就占掉上百行，
     * 主窗口因此承担了本不属于它的文件系统操作职责。
     */
    if (!m_rename->rename(item, this))
        return;   // 用户取消、校验失败或任一阶段失败，提示已在 service 内给出

    m_statusBar->showTransientMessage(
        QStringLiteral("已重命名：%1 → %2")
            .arg(oldName, QFileInfo(item.path).fileName()), 3000);

    m_model->reload();
    m_sidebar->reload();
    updateStatusBar();
    updateEmptyState();
}

void MainWindow::onTagAdded(qint64 itemId, const QString &tag)
{
    if (!m_database || itemId <= 0)
        return;

    m_database->addTag(itemId, tag);
    syncTagsToUi(itemId);
}

void MainWindow::onTagRemoved(qint64 itemId, const QString &tag)
{
    if (!m_database || itemId <= 0)
        return;

    /*!
     * 检查 removeTag 的返回值。
     *
     * 之前这里直接丢弃返回值：DELETE 失败（库被占用、连接断开）时
     * 界面照样走 syncTagsToUi 刷新一遍，用户会看到标签"删掉了"，
     * 下次打开又回来 —— 这比报错更难排查。
     */
    if (!m_database->removeTag(itemId, tag)) {
        QMessageBox::warning(this, QStringLiteral("移除标签失败"),
                             QStringLiteral("无法移除标签「%1」：\n%2")
                                 .arg(tag, m_database->lastError()));
        return;
    }

    syncTagsToUi(itemId);
}

void MainWindow::syncTagsToUi(qint64 itemId)
{
    // 标签变了要同时更新四处，漏掉任何一处都会让界面自相矛盾：
    // 1) 模型：网格/列表项上的标签 Role
    // 2) 详情面板：当前标签 chip 列表
    // 3) 侧边栏：标签清单与计数（有标签筛选在生效时可见性也会跟着变）
    // 4) 状态栏
    const int row = m_model->rowForId(itemId);
    if (row >= 0)
        m_model->updateItemState(itemId, m_model->itemAt(row).favorite,
                                 m_database->tagsForItem(itemId));

    // 只有正在看这个条目时才刷详情面板，避免把面板内容换掉
    if (m_detail->currentItemId() == itemId) {
        MediaItem item = m_database->itemById(itemId);
        if (item.isValid())
            m_detail->showItem(item);
    }

    /*!
     * 侧边栏要重建标签列表，不只是刷角标。
     *
     * updateCounts() 只更新智能分组右侧的计数，不碰 m_tagList 的内容。
     * 于是删掉某条目上最后一个"旅行"标签后，侧边栏标签区仍列着"旅行"，
     * 点它筛选结果是空的 —— 用户看到的是"有这个标签，却搜不到东西"。
     * 重建会重新查 allTags()，已无人使用的标签自然消失。
     * 重建过程中会记住并恢复当前生效的标签高亮，所以不会打断正在进行的筛选。
     */
    m_sidebar->refreshCountsFromDatabase();

    updateStatusBar();
}

// ================================================================ 工具栏动作

void MainWindow::onAddFolderClicked()
{
    const QString dir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("选择要添加到资源库的目录"),
        QStandardPaths::writableLocation(QStandardPaths::PicturesLocation),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);

    if (dir.isEmpty())
        return;

    const QString norm = QDir::cleanPath(QDir::fromNativeSeparators(dir));

    if (!MediaScanner::isReadable(norm)) {
        QMessageBox::warning(this, QStringLiteral("目录不可访问"),
                             QStringLiteral("无法读取该目录：\n%1\n\n"
                                            "请确认目录存在且有访问权限。").arg(norm));
        return;
    }

    if (!m_database->isOpen()) {
        QMessageBox::warning(this, QStringLiteral("资源库未打开"),
                             QStringLiteral("数据库不可用，无法添加目录。"));
        return;
    }

    m_database->addFolder(norm);
    m_fileWatcher->addRoot(norm);
    m_sidebar->reload();

    // 立即扫描，让用户马上看到内容
    startScan(norm);

    // 选中并显示
    m_sidebar->selectFolder(norm);
}

void MainWindow::onRefreshClicked()
{
    if (!m_database || !m_database->isOpen())
        return;

    if (m_scanInProgress) {
        // 正在扫描时再点刷新会让人困惑，给出明确反馈。
        // 用常驻消息而不是瞬时消息：扫描期间这条提示要一直挂着。
        m_statusBar->setScanMessage(QStringLiteral("已有扫描任务正在进行…"));
        return;
    }

    // 优先重扫当前选中的目录；没有选中则重扫全部
    if (!m_currentFolder.isEmpty()) {
        startScan(m_currentFolder);
        return;
    }

    const QList<QString> folders = m_database->allFolderPaths();
    if (folders.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("重新扫描"),
                                 QStringLiteral("还没有添加任何目录。"));
        return;
    }

    for (const QString &folder : folders)
        startScan(folder);
}

void MainWindow::removeFolder(const QString &path)
{
    if (path.isEmpty() || !m_database)
        return;

    const int count = m_database->countItems(path);

    const auto answer = QMessageBox::question(
        this, QStringLiteral("移除目录"),
        QStringLiteral("将从资源库中移除：\n%1\n\n"
                       "会删除该目录下 %2 条索引记录（含收藏与标签）。\n"
                       "磁盘上的原始文件不会被删除。\n\n确定继续吗？")
            .arg(path).arg(count),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

    if (answer != QMessageBox::Yes)
        return;

    m_database->removeFolder(path);
    m_fileWatcher->removeRoot(path);

    if (m_currentFolder == path) {
        m_currentFolder.clear();
        m_model->setFilter(QString(), MediaType::Unknown, false);
    }

    m_sidebar->reload();
    m_model->reload();
    updateStatusBar();
    updateEmptyState();
}

void MainWindow::onSearchTextChanged(const QString &text)
{
    // 在首页输入搜索词时，结果无处显示——先切到「全部资源」，
    // 让用户能看到筛选结果。走侧边栏统一入口，高亮保持同步。
    if (!text.trimmed().isEmpty()
        && m_currentSmartGroup == SidebarWidget::GroupHome) {
        m_sidebar->selectSmartGroup(SidebarWidget::GroupAll);
    }

    // 两层搜索：
    // 1. 代理层在已加载数据里即时筛选（无延迟，覆盖已加载部分）
    // 2. 模型层走一次库查询（覆盖全库，但有 IO 延迟）
    //
    // 这里只做第 1 层，保证输入时不卡。第 2 层留给用户按回车触发，
    // 因为它们同时生效会产生"结果不稳定"的观感。
    m_proxy->setSearchText(text);
    updateStatusBar();
    updateEmptyState();
}

void MainWindow::onSearchSubmitted(const QString &text)
{
    /*!
     * 回车 = 把搜索词下推到数据库，覆盖全库而不只是已加载页。
     *
     * 为什么分两步走：
     * 逐字输入时每敲一个字符都发起一次全库 LIKE 查询，机械盘上大库会明显卡顿；
     * 而且中间态（"ph"、"pho"、"phot"）的结果用户根本不看。
     * 所以输入只做代理层筛选（纯内存、即时），回车才走库查询。
     *
     * 代价是"输入阶段搜不到未加载页"，这一点由回车兜住。
     * 代理层与模型层两处筛选条件相同，不会出现重复叠加——
     * setFilter 传入同一个 searchText，代理再用它筛一遍得到的是同一个集合。
     */
    const QString trimmed = text.trimmed();

    /*!
     * 失效记录视图是特例：它的条件走 setFilterMissing()，没有 searchText 参数。
     * 这里不做"在失效记录里搜索"——那需要给 setFilterMissing 加参并区分
     * includeMissing 与 searchText 的组合，而失效记录通常是用户临时查看后
     * 就清理掉的小集合，为它引入参数组合不值得。
     * 处理方式是明确告知，而不是静默忽略输入。
     */
    if (m_currentSmartGroup == SidebarWidget::GroupMissing) {
        m_model->setFilterMissing();
        if (!trimmed.isEmpty()) {
            m_statusBar->showTransientMessage(
                QStringLiteral("失效记录视图不支持名称搜索，请先切到其他分组"), 2500);
        }
        updateStatusBar();
        updateEmptyState();
        return;
    }

    if (trimmed.isEmpty()) {
        // 清空搜索：退回当前上下文的全量视图，而不是空列表
        if (m_currentFolder.isEmpty()) {
            m_model->setFilter(QString(), typeFilterForCurrentGroup(),
                               m_currentSmartGroup == SidebarWidget::GroupFavorites);
        } else {
            m_model->setFilter(m_currentFolder, MediaType::Unknown, false);
        }
    } else {
        // 保留当前目录 / 类型 / 收藏上下文，只叠加搜索词
        m_model->setFilter(m_currentFolder,
                           typeFilterForCurrentGroup(),
                           m_currentSmartGroup == SidebarWidget::GroupFavorites,
                           trimmed);
    }

    // 代理层保持一致，避免"库查询结果已含关键词、代理又筛一遍"
    // 虽然两次筛的是同一个条件、结果相同，但显式同步能让逻辑一目了然。
    m_proxy->setSearchText(trimmed);

    updateStatusBar();
    updateEmptyState();
}

MediaType MainWindow::typeFilterForCurrentGroup() const
{
    if (m_currentSmartGroup == SidebarWidget::GroupImages)
        return MediaType::Image;
    if (m_currentSmartGroup == SidebarWidget::GroupVideos)
        return MediaType::Video;
    if (m_currentSmartGroup == SidebarWidget::GroupAudios)
        return MediaType::Audio;
    return MediaType::Unknown;
}

void MainWindow::onSortKeyChanged(const QString &key)
{
    if (key.isEmpty())
        return;

    // 排序会改变行的位置，之前按行请求的缩略图结果会对不上号
    // （模型按 id 定位所以不会画错图，但那些任务是在为旧顺序服务，没必要跑完）
    if (m_taskManager)
        m_taskManager->cancelThumbnails();

    m_proxy->setSortKey(key);
    updateStatusBar();
}

void MainWindow::onViewModeChanged(bool gridMode)
{
    // 图标颜色（当前模式深色、另一个浅色）由 TopBarWidget 自己维护，
    // 这里只把模式交给视图。
    m_grid->setViewMode(gridMode ? ThumbnailGrid::GridMode : ThumbnailGrid::ListMode);
}

void MainWindow::onThumbnailSizeChanged(int value)
{
    m_grid->setThumbnailSize(value);
}

// ================================================================ 设置

void MainWindow::onSettingsClicked()
{
    SettingsDialog dlg(m_settings, m_taskManager, m_thumbCache, this);

    // 对话框里的即时改动直接落到对应模块，不需要重启
    connect(&dlg, &SettingsDialog::cacheLimitsChanged, this,
            [this](int memoryMb, int diskMb) {
                m_thumbCache->setMemoryLimit(qint64(memoryMb) * 1024 * 1024);
                m_thumbCache->setDiskLimit(qint64(diskMb) * 1024 * 1024);
            });
    connect(&dlg, &SettingsDialog::thumbnailSizeChanged, this,
            [this](int value) {
                // 走统一入口，保持滑块位置与标签同步
                m_topBar->setThumbnailSliderValue(value);
            });

    dlg.exec();

    // 对话框关闭后重新应用一遍：视图模式、面板显隐、扫描配置等
    // 可能被改过，统一在这里生效，避免逐项挂信号。
    applySettings();
}

void MainWindow::applySettings()
{
    if (!m_settings)
        return;

    // ---------- 扫描与后台任务 ----------
    if (m_taskManager) {
        m_taskManager->setScanOptions(m_settings->recursiveScan(),
                                      m_settings->readExifOnScan(),
                                      m_settings->skipHidden());
        m_taskManager->setExtraSkipDirs(m_settings->extraSkipDirs());
    }

    // ---------- 缓存 ----------
    m_thumbCache->setMemoryLimit(qint64(m_settings->thumbnailMemoryLimitMb()) * 1024 * 1024);
    m_thumbCache->setDiskLimit(qint64(m_settings->thumbnailDiskLimitMb()) * 1024 * 1024);

    // ---------- 文件监控 ----------
    const bool watch = m_settings->watchFileSystem();
    if (m_fileWatcher) {
        if (watch && m_database && m_database->isOpen())
            m_fileWatcher->setRoots(m_database->allFolderPaths());
        else
            m_fileWatcher->setSuspended(true);
    }

    // ---------- 界面 ----------
    // 视图模式与排序键都交给工具条自己处理：
    // 那些控件是互斥按钮组 / 下拉框，值相同时应当 no-op，
    // 由持有控件的类去判断比在这里隔空比较状态干净。
    m_topBar->setViewMode(m_settings->viewMode() == QLatin1String("list")
                              ? ThumbnailGrid::ListMode
                              : ThumbnailGrid::GridMode);
    m_topBar->setSortKey(m_settings->sortKey());

    // 缩略图尺寸：走统一入口（触发网格重排 + 标签更新）
    m_topBar->setThumbnailSliderValue(m_settings->thumbnailSize());

    // 右侧检查器显隐
    if (m_detail)
        m_detail->setVisible(m_settings->inspectorVisible());

    // 状态栏缓存统计
    m_statusBar->setCacheStatsVisible(m_settings->showCacheStats());

    // 窗口几何与分栏宽度只在启动时恢复。
    // 设置对话框关闭后也会走到这里，若再应用一遍，
    // 会用上次保存的值覆盖用户当前会话里拖出来的分栏宽度。
    if (!isVisible()) {
        const QByteArray geometry = m_settings->windowGeometry();
        if (!geometry.isEmpty())
            restoreGeometry(geometry);

        const int sidebarWidth = m_settings->sidebarWidth();
        const int inspectorWidth = m_settings->inspectorWidth();
        if (sidebarWidth > 0 && inspectorWidth > 0) {
            QList<int> sizes = m_splitter->sizes();
            if (sizes.size() == 3) {
                sizes[0] = sidebarWidth;
                sizes[2] = inspectorWidth;
                m_splitter->setSizes(sizes);
            }
        }
    }
}

void MainWindow::saveSettings()
{
    if (!m_settings)
        return;

    m_settings->setWindowGeometry(saveGeometry());

    const QList<int> sizes = m_splitter->sizes();
    if (sizes.size() == 3) {
        m_settings->setSidebarWidth(sizes[0]);
        m_settings->setInspectorWidth(sizes[2]);
    }

    m_settings->setViewMode(m_grid->viewMode() == ThumbnailGrid::ListMode
                                ? QStringLiteral("list")
                                : QStringLiteral("grid"));
    m_settings->setSortKey(m_proxy->sortKey());
    m_settings->setThumbnailSize(m_topBar->thumbnailSliderValue());
    m_settings->setLastFolder(m_currentFolder);

    m_settings->sync();
}

// ================================================================ 首页导航

void MainWindow::onHomeFolderRequested(const QString &path)
{
    // 复用侧边栏的统一入口：高亮与筛选行为完全一致
    m_sidebar->selectFolder(path);
}

void MainWindow::onHomeItemRequested(const MediaItem &item)
{
    openPreview(item, true);
}

void MainWindow::onHomeSmartGroupRequested(const QString &group)
{
    m_sidebar->selectSmartGroup(group);
}

// ================================================================ 任务面板

void MainWindow::onTaskCancelAll()
{
    if (!m_taskManager)
        return;

    // TaskManager 会为每个被取消的扫描补发 scanFinished，
    // 主窗口的收尾逻辑（恢复监控、刷新界面）随之执行，这里不需要重复处理。
    m_taskManager->cancelAllNonBlocking();
}

void MainWindow::onTaskCancelThumbnails()
{
    if (m_taskManager)
        m_taskManager->cancelThumbnails();
}

// ================================================================ 重复检测

void MainWindow::runDuplicateDetection(bool perceptual)
{
    if (!m_database || !m_database->isOpen())
        return;

    // 算法全部在 DuplicateService 里，这里只负责进度对话框与结果提示。
    //
    // 已知限制：整个流程（含感知哈希两两比对）是同步的，在几万条记录上会
    // 明显卡住界面。progress.setValue + processEvents 只能让进度条动起来，
    // 并没有真正把计算挪到后台。彻底解决需要把 DuplicateService 丢进
    // TaskQueue 并补一条"算完回 UI 更新分组"的链路，属于下一阶段的工作，
    // 这里不假装它已经异步了。
    //
    // 好消息是这个过程有增量性：已经算过的哈希会落库，第二次触发
    // 只需处理新增文件，通常瞬间完成。
    DuplicateService service(m_database);

    QProgressDialog progress(
        perceptual ? QStringLiteral("正在计算感知哈希…")
                   : QStringLiteral("正在计算文件哈希…"),
        QStringLiteral("取消"), 0, 0, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    progress.setRange(0, 0);   // 总数在服务内部确定，先用不确定进度

    const DuplicateService::ProgressCallback onProgress =
        [&progress](int done, int total) {
            if (progress.wasCanceled())
                return false;
            if (progress.maximum() != total)
                progress.setRange(0, total);
            progress.setValue(done);
            // 让进度对话框有机会刷新（否则会一直卡在 0%）
            QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
            return true;
        };

    const DuplicateService::Result result = perceptual
        ? service.findSimilarImages(onProgress)
        : service.findExactDuplicates(onProgress);

    progress.setValue(progress.maximum());

    const QString title = perceptual ? QStringLiteral("查找相似图片")
                                    : QStringLiteral("查找重复文件");

    if (result.cancelled) {
        m_statusBar->showTransientMessage(QStringLiteral("查重已取消"));
        return;
    }

    if (result.groups.isEmpty()) {
        // 精确查重命中"无待算哈希"的缓存路径时 scanned 为 0，
        // 这时说"已计算 0 个文件"会让人以为出错，换成基于库内已有哈希的说明。
        QString text;
        if (perceptual) {
            text = QStringLiteral("已分析 %1 张图片，没有发现视觉相似的图片。")
                       .arg(result.scanned);
        } else if (result.scanned == 0) {
            text = QStringLiteral("没有发现内容完全相同的文件。");
        } else {
            text = QStringLiteral("已计算 %1 个文件的哈希，没有发现完全相同的文件。")
                       .arg(result.scanned);
        }
        if (result.truncated) {
            text += QStringLiteral("\n\n注意：图片数量超过 %1 张，只分析了前 %1 张。")
                        .arg(result.truncatedLimit);
        }
        QMessageBox::information(this, title, text);
        return;
    }

    showDuplicateGroups(result.groups);

    // 提示里补上"分析了多少、阈值是多少"这类能让人判断结果可信度的信息
    QString text = perceptual
        ? QStringLiteral("已分析 %1 张图片，发现 %2 组视觉相似的图片"
                         "（汉明距离 ≤ %3）。\n\n"
                         "已切换到「重复项」分组，同一组的图片相邻排列。")
              .arg(result.scanned).arg(result.groupCount).arg(result.threshold)
        : QStringLiteral("发现 %1 组内容完全相同的文件（共 %2 个）。\n\n"
                         "已切换到「重复项」分组，同一组的文件相邻排列，方便比对与清理。")
              .arg(result.groupCount).arg(result.itemCount);

    if (result.truncated) {
        text += QStringLiteral("\n\n注意：图片数量超过 %1 张，只分析了前 %1 张，"
                               "结果可能不完整。")
                    .arg(result.truncatedLimit);
    }

    QMessageBox::information(this, title, text);
}

void MainWindow::showDuplicateGroups(const QList<QList<MediaItem>> &groups)
{
    if (groups.isEmpty())
        return;

    // 先走侧边栏的统一入口切到「重复项」分组。
    // 顺序很关键：selectSmartGroup 会经 smartGroupSelected →
    // applySmartGroup 去库里查一次重复组，用的是"已落库的哈希"。
    // 感知相似的结果并不在 content_hash 里，所以这一步之后必须再由
    // 我们把这批分组覆盖上去，否则用户看到的会是 MD5 那一路的结果。
    m_sidebar->selectSmartGroup(SidebarWidget::GroupDuplicates);

    // 把本次算出的分组铺平后作为虚拟数据给模型。
    // 同一组的条目相邻排列，方便肉眼比对。
    QList<MediaItem> flat;
    flat.reserve(groups.size() * 2);
    for (const QList<MediaItem> &group : groups)
        flat.append(group);

    m_model->setVirtualItems(flat);
    m_currentSmartGroup = SidebarWidget::GroupDuplicates;
    m_currentFolder.clear();

    m_centerStack->setCurrentWidget(m_grid);
    m_detail->clear();
    updateStatusBar();
    updateEmptyState();
}

// ================================================================ 状态与空态

void MainWindow::updateStatusBar()
{
    if (!m_database)
        return;

    const MediaDatabase::Stats s = m_database->stats();

    // "筛选后"只在可见数真的少于已加载数时才显示。
    // 用总数比较会在分页时误报（首屏只加载 200 条而库里有上万条）。
    const int visible = m_proxy ? m_proxy->rowCount() : 0;
    const int loaded = m_model ? m_model->loadedCount() : 0;

    m_statusBar->setItemCount(s.total, visible < loaded ? visible : -1);
    m_statusBar->setSelectionSummary(m_grid->selectedItems());
}

void MainWindow::updateEmptyState()
{
    if (!m_grid || !m_centerStack)
        return;

    // 首页有自己的一套空状态引导，这里不要用资源页的空态把首页顶掉
    if (m_currentSmartGroup == SidebarWidget::GroupHome)
        return;

    const bool hasContent = !m_grid->isEmpty();

    if (hasContent) {
        m_centerStack->setCurrentWidget(m_grid);
        return;
    }

    // 没有可见内容时，区分三种情况给出不同引导：
    // 完全没有目录 / 正在建索引 / 有目录但筛选后为空
    const bool hasFolders = m_database && !m_database->allFolderPaths().isEmpty();

    if (!hasFolders)
        m_emptyPage->setReason(EmptyStateWidget::NoFolder);
    else if (m_scanInProgress)
        m_emptyPage->setReason(EmptyStateWidget::Indexing);
    else
        m_emptyPage->setReason(EmptyStateWidget::FilteredEmpty);

    m_centerStack->setCurrentWidget(m_emptyPage);
}

// ================================================================ 窗口事件

void MainWindow::closeEvent(QCloseEvent *event)
{
    /*!
     * 关窗时必须有序收尾，否则会崩：
     *
     * 1. 关掉预览（它持有 QMediaPlayer，播放中析构会异步等待）
     * 2. 停文件监控（否则可能在收尾过程中又触发扫描）
     * 3. 取消所有任务并等待线程退出（关键！否则工作线程还在用
     *    即将被析构的 MediaDatabase）
     * 4. 关闭数据库连接
     *
     * 每一步的顺序都不能调换。
     */
    if (m_preview) {
        m_preview->close();
        m_preview->deleteLater();
        m_preview = nullptr;
    }

    if (m_fileWatcher)
        m_fileWatcher->setSuspended(true);

    // 保存窗口布局与界面状态（此时各控件仍完好）
    saveSettings();

    if (m_taskManager) {
        // 8 秒足够让单个缩略图任务收尾；超时只记录警告，不强杀线程
        m_taskManager->shutdown(8000);
    }

    // 停掉状态栏的自动刷新：收尾阶段数据库与缓存正在被释放，
    // 定时器再跑一轮就会去读已经不该动的东西。
    if (m_statusBar)
        m_statusBar->stopAutoRefresh();

    if (m_database && m_database->isOpen())
        m_database->close();

    event->accept();
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    // 只接受本地文件夹/文件的拖放
    if (!event->mimeData()->hasUrls())
        return;

    const QList<QUrl> urls = event->mimeData()->urls();
    for (const QUrl &url : urls) {
        if (url.isLocalFile()) {
            event->acceptProposedAction();
            return;
        }
    }
}

void MainWindow::dropEvent(QDropEvent *event)
{
    const QList<QUrl> urls = event->mimeData()->urls();

    QStringList addedFolders;
    int skipped = 0;

    for (const QUrl &url : urls) {
        if (!url.isLocalFile())
            continue;

        const QString path = QDir::cleanPath(QDir::fromNativeSeparators(url.toLocalFile()));
        QFileInfo info(path);

        if (info.isDir()) {
            if (!MediaScanner::isReadable(path)) {
                ++skipped;
                continue;
            }
            m_database->addFolder(path);
            addedFolders << path;
        } else if (info.isFile()) {
            // 拖进来单个文件：把它所在目录加入资源库。
            // 直接加文件本身没有意义，资源库是按目录组织的。
            const QString dir = info.absolutePath();
            if (!MediaScanner::isReadable(dir)) {
                ++skipped;
                continue;
            }
            m_database->addFolder(dir);
            addedFolders << dir;
        }
    }

    if (addedFolders.isEmpty()) {
        if (skipped > 0) {
            QMessageBox::warning(this, QStringLiteral("无法添加"),
                QStringLiteral("有 %1 个路径无法访问（不存在或无权限）。").arg(skipped));
        }
        event->ignore();
        return;
    }

    m_fileWatcher->setRoots(m_database->allFolderPaths());

    for (const QString &folder : addedFolders)
        startScan(folder);

    m_sidebar->reload();

    if (!addedFolders.isEmpty())
        m_sidebar->selectFolder(addedFolders.first());

    event->acceptProposedAction();
}

void MainWindow::keyPressEvent(QKeyEvent *event)
{
    switch (event->key()) {
    case Qt::Key_F5:
        onRefreshClicked();
        return;
    case Qt::Key_F2:
        onRenameRequested();
        return;
    case Qt::Key_Space: {
        // 空格打开预览（与多数媒体管理器一致）
        const MediaItem item = m_grid->currentItem();
        if (item.isValid())
            openPreview(item, true);
        return;
    }
    case Qt::Key_Delete: {
        const QList<MediaItem> selected = m_grid->selectedItems();
        if (selected.isEmpty())
            return;

        // 只从资源库移除索引，绝不删除磁盘文件。
        // 删除物理文件是危险操作，这个程序刻意不提供。
        const auto answer = QMessageBox::question(
            this, QStringLiteral("从资源库移除"),
            QStringLiteral("将把选中的 %1 项从资源库索引中移除。\n\n"
                           "磁盘上的原始文件不会被删除。\n\n确定继续吗？")
                .arg(selected.size()),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

        if (answer != QMessageBox::Yes)
            return;

        for (const MediaItem &item : selected)
            m_database->markMissingByPath(item.path);

        m_model->reload();
        m_sidebar->updateCounts();
        updateStatusBar();
        updateEmptyState();
        return;
    }
    default:
        break;
    }

    QMainWindow::keyPressEvent(event);
}

} // namespace media
