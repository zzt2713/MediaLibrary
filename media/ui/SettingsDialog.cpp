#include "SettingsDialog.h"
#include "Icons.h"
#include "ThemeManager.h"
#include "../core/SettingsManager.h"
#include "../core/AppPaths.h"
#include "../core/Logger.h"
#include "../core/ThumbnailCache.h"
#include "../tasks/TaskManager.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QTabWidget>
#include <QCheckBox>
#include <QSpinBox>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QListWidget>
#include <QLineEdit>
#include <QDialogButtonBox>
#include <QMessageBox>
#include <QDesktopServices>
#include <QUrl>
#include <QDir>
#include <QFileInfo>
#include <QScrollArea>
#include <QFrame>
#include <QDebug>

namespace media {

namespace {

/*!
 * 打开目录，失败时给出可见反馈。
 *
 * 之前 openUrl 的返回值被丢弃：路径无效或系统 shell 拒绝时，
 * 点按钮毫无反应也毫无提示，用户只能得出"打不开"的结论。
 */
void openFolderWithFeedback(QWidget *parent, const QString &dir, const QString &what)
{
    if (dir.isEmpty()) {
        QMessageBox::warning(parent, what,
                             QStringLiteral("%1失败：路径为空。").arg(what));
        return;
    }

    if (!AppPaths::ensureDir(dir)) {
        QMessageBox::warning(parent, what,
                             QStringLiteral("无法创建目录：\n%1").arg(dir));
        return;
    }

    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(dir))) {
        QMessageBox::warning(parent, what,
                             QStringLiteral("系统未能打开该目录：\n%1\n\n"
                                            "请手动复制此路径到文件管理器。").arg(dir));
    }
}

/*! 把一个"标签 + 说明"的只读路径行做成小部件，供复制与打开。 */
QWidget *makePathRow(const QString &caption, QLabel **valueOut, QWidget *parent)
{
    auto *row = new QWidget(parent);
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    auto *label = new QLabel(caption, row);
    label->setObjectName(QStringLiteral("DetailKey"));
    label->setMinimumWidth(72);
    layout->addWidget(label);

    auto *value = new QLabel(row);
    value->setObjectName(QStringLiteral("DetailValue"));
    value->setTextInteractionFlags(Qt::TextSelectableByMouse);
    value->setWordWrap(true);
    value->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    layout->addWidget(value, 1);

    if (valueOut)
        *valueOut = value;

    return row;
}

/*! 生成一个带说明文字的小标题区，让设置项的意义可读。 */
QLabel *makeHint(const QString &text, QWidget *parent)
{
    auto *hint = new QLabel(text, parent);
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color: %1; font-size: 12px;")
                            .arg(ThemeManager::textDisabled().name()));
    return hint;
}

} // namespace

SettingsDialog::SettingsDialog(SettingsManager *settings,
                               TaskManager *taskManager,
                               ThumbnailCache *cache,
                               QWidget *parent)
    : QDialog(parent)
    , m_settings(settings)
    , m_taskManager(taskManager)
    , m_cache(cache)
{
    setWindowTitle(QStringLiteral("设置"));
    setMinimumSize(620, 560);
    resize(680, 620);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto *tabs = new QTabWidget(this);
    tabs->addTab(buildAppearancePage(),  QStringLiteral("外观"));
    tabs->addTab(buildScanPage(),        QStringLiteral("浏览与扫描"));
    tabs->addTab(buildCachePage(),       QStringLiteral("缓存"));
    tabs->addTab(buildPlaybackPage(),    QStringLiteral("播放"));
    tabs->addTab(buildAdvancedPage(),    QStringLiteral("高级"));

    root->addWidget(tabs, 1);

    // 底部：恢复默认 + 关闭
    auto *footer = new QWidget(this);
    footer->setObjectName(QStringLiteral("TopBar"));
    auto *footerLayout = new QHBoxLayout(footer);
    footerLayout->setContentsMargins(12, 10, 12, 10);

    auto *resetBtn = new QPushButton(Icons::get(Icons::Refresh, ThemeManager::textSecondary(), 16),
                                     QStringLiteral("恢复默认设置"), footer);
    resetBtn->setCursor(Qt::PointingHandCursor);
    connect(resetBtn, &QPushButton::clicked, this, &SettingsDialog::onResetDefaults);
    footerLayout->addWidget(resetBtn);

    footerLayout->addStretch();

    auto *closeBtn = new QPushButton(QStringLiteral("关闭"), footer);
    closeBtn->setDefault(true);
    closeBtn->setCursor(Qt::PointingHandCursor);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    footerLayout->addWidget(closeBtn);

    root->addWidget(footer);

    refreshPaths();
}

SettingsDialog::~SettingsDialog() = default;

// ================================================================ 外观

QWidget *SettingsDialog::buildAppearancePage()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(18, 18, 18, 18);
    layout->setSpacing(14);

    // ---------- 视图 ----------
    auto *viewGroup = new QGroupBox(QStringLiteral("视图"), page);
    auto *viewForm = new QFormLayout(viewGroup);
    viewForm->setSpacing(10);

    m_viewMode = new QComboBox(viewGroup);
    m_viewMode->addItem(QStringLiteral("网格视图"), QStringLiteral("grid"));
    m_viewMode->addItem(QStringLiteral("列表视图"), QStringLiteral("list"));
    const QString currentMode = m_settings->viewMode();
    m_viewMode->setCurrentIndex(currentMode == QLatin1String("list") ? 1 : 0);
    connect(m_viewMode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        m_settings->setViewMode(m_viewMode->currentData().toString());
    });
    viewForm->addRow(QStringLiteral("默认视图"), m_viewMode);

    m_thumbSize = new QSpinBox(viewGroup);
    m_thumbSize->setRange(0, 100);
    m_thumbSize->setSingleStep(4);
    m_thumbSize->setValue(m_settings->thumbnailSize());
    m_thumbSize->setSuffix(QStringLiteral("（0 最小，100 最大）"));
    connect(m_thumbSize, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int value) {
        m_settings->setThumbnailSize(value);
        emit thumbnailSizeChanged(value);
    });
    viewForm->addRow(QStringLiteral("缩略图尺寸"), m_thumbSize);

    m_inspectorVisible = new QCheckBox(QStringLiteral("显示右侧资源检查器"), viewGroup);
    m_inspectorVisible->setChecked(m_settings->inspectorVisible());
    connect(m_inspectorVisible, &QCheckBox::toggled, this, [this](bool checked) {
        m_settings->setInspectorVisible(checked);
    });
    viewForm->addRow(QString(), m_inspectorVisible);

    layout->addWidget(viewGroup);

    // ---------- 启动 ----------
    auto *startupGroup = new QGroupBox(QStringLiteral("启动"), page);
    auto *startupForm = new QFormLayout(startupGroup);
    startupForm->setSpacing(10);

    m_restoreLastFolder = new QCheckBox(
        QStringLiteral("启动时恢复上次浏览的目录"), startupGroup);
    m_restoreLastFolder->setChecked(m_settings->restoreLastFolder());
    connect(m_restoreLastFolder, &QCheckBox::toggled, this, [this](bool checked) {
        m_settings->setRestoreLastFolder(checked);
    });
    startupForm->addRow(QString(), m_restoreLastFolder);

    m_scanOnStartup = new QCheckBox(
        QStringLiteral("启动时自动重新扫描已登记的目录"), startupGroup);
    m_scanOnStartup->setChecked(m_settings->scanOnStartup());
    connect(m_scanOnStartup, &QCheckBox::toggled, this, [this](bool checked) {
        m_settings->setScanOnStartup(checked);
    });
    startupForm->addRow(QString(), m_scanOnStartup);
    startupForm->addRow(QString(), makeHint(
        QStringLiteral("关闭时程序启动后直接查库显示已有内容（几乎瞬时）。"
                       "大目录首次扫描可能需要数十秒，因此默认关闭。"),
        startupGroup));

    layout->addWidget(startupGroup);

    // ---------- 状态栏 ----------
    auto *statusGroup = new QGroupBox(QStringLiteral("状态栏"), page);
    auto *statusForm = new QFormLayout(statusGroup);

    m_showCacheStats = new QCheckBox(
        QStringLiteral("显示缩略图缓存命中率"), statusGroup);
    m_showCacheStats->setChecked(m_settings->showCacheStats());
    connect(m_showCacheStats, &QCheckBox::toggled, this, [this](bool checked) {
        m_settings->setShowCacheStats(checked);
    });
    statusForm->addRow(QString(), m_showCacheStats);

    layout->addWidget(statusGroup);
    layout->addStretch();

    return page;
}

// ================================================================ 浏览与扫描

QWidget *SettingsDialog::buildScanPage()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(18, 18, 18, 18);
    layout->setSpacing(14);

    auto *scanGroup = new QGroupBox(QStringLiteral("扫描行为"), page);
    auto *scanForm = new QFormLayout(scanGroup);
    scanForm->setSpacing(10);

    m_recursive = new QCheckBox(QStringLiteral("递归扫描子目录"), scanGroup);
    m_recursive->setChecked(m_settings->recursiveScan());
    connect(m_recursive, &QCheckBox::toggled, this, [this](bool checked) {
        m_settings->setRecursiveScan(checked);
    });
    scanForm->addRow(QString(), m_recursive);

    m_readExif = new QCheckBox(QStringLiteral("扫描时读取 EXIF 元数据"), scanGroup);
    m_readExif->setChecked(m_settings->readExifOnScan());
    connect(m_readExif, &QCheckBox::toggled, this, [this](bool checked) {
        m_settings->setReadExifOnScan(checked);
    });
    scanForm->addRow(QString(), m_readExif);
    scanForm->addRow(QString(), makeHint(
        QStringLiteral("开启后详情面板能显示拍摄时间、相机型号等；"
                       "关闭可让首次扫描快约一半。选项在下次扫描时生效。"),
        scanGroup));

    m_skipHidden = new QCheckBox(QStringLiteral("跳过隐藏目录与隐藏文件"), scanGroup);
    m_skipHidden->setChecked(m_settings->skipHidden());
    connect(m_skipHidden, &QCheckBox::toggled, this, [this](bool checked) {
        m_settings->setSkipHidden(checked);
    });
    scanForm->addRow(QString(), m_skipHidden);

    layout->addWidget(scanGroup);

    auto *fsGroup = new QGroupBox(QStringLiteral("实时监控"), page);
    auto *fsLayout = new QVBoxLayout(fsGroup);

    m_watchFs = new QCheckBox(QStringLiteral("监控目录变化并自动刷新资源库"), fsGroup);
    m_watchFs->setChecked(m_settings->watchFileSystem());
    connect(m_watchFs, &QCheckBox::toggled, this, [this](bool checked) {
        m_settings->setWatchFileSystem(checked);
    });
    fsLayout->addWidget(m_watchFs);
    fsLayout->addWidget(makeHint(
        QStringLiteral("使用系统文件监控感知目录增删改，变更后会合并触发一次增量扫描。"
                       "在不稳定的网络盘上可能产生噪音事件，可以关闭后手动刷新。"),
        fsGroup));

    layout->addWidget(fsGroup);

    // ---------- 跳过目录 ----------
    auto *skipGroup = new QGroupBox(QStringLiteral("额外跳过的目录名"), page);
    auto *skipLayout = new QVBoxLayout(skipGroup);

    skipLayout->addWidget(makeHint(
        QStringLiteral("不区分大小写，每行一个。此列表会在内置跳过规则"
                       "（$RECYCLE.BIN、System Volume Information、.git 等）之外额外生效。"),
        skipGroup));

    m_skipDirs = new QListWidget(skipGroup);
    m_skipDirs->setMaximumHeight(140);
    m_skipDirs->setAlternatingRowColors(true);
    m_skipDirs->setSelectionMode(QAbstractItemView::SingleSelection);

    // 允许直接编辑行内文本，编辑完回车即保存，不需要额外的增删按钮
    m_skipDirs->setEditTriggers(QAbstractItemView::DoubleClicked
                                | QAbstractItemView::EditKeyPressed);

    const QStringList existing = m_settings->extraSkipDirs();
    for (const QString &dir : existing)
        new QListWidgetItem(dir, m_skipDirs);

    skipLayout->addWidget(m_skipDirs);

    auto *skipBtnRow = new QHBoxLayout();
    auto *addSkipBtn = new QPushButton(Icons::get(Icons::Add, ThemeManager::textSecondary(), 15),
                                       QStringLiteral("添加"), skipGroup);
    auto *removeSkipBtn = new QPushButton(Icons::get(Icons::Trash, ThemeManager::dangerColor(), 15),
                                          QStringLiteral("移除选中"), skipGroup);

    // 双击空白处新增一条，让用户可以直接输入
    connect(addSkipBtn, &QPushButton::clicked, this, [this]() {
        auto *item = new QListWidgetItem(QStringLiteral("新目录名"), m_skipDirs);
        item->setFlags(item->flags() | Qt::ItemIsEditable);
        m_skipDirs->setCurrentItem(item);
        m_skipDirs->editItem(item);
    });

    connect(removeSkipBtn, &QPushButton::clicked, this, [this]() {
        const int row = m_skipDirs->currentRow();
        if (row < 0)
            return;
        delete m_skipDirs->takeItem(row);
        // 删除后立刻持久化，避免用户忘了点别处导致没保存
        QStringList dirs;
        for (int i = 0; i < m_skipDirs->count(); ++i)
            dirs << m_skipDirs->item(i)->text();
        m_settings->setExtraSkipDirs(dirs);
    });

    // 任何编辑（含行内重命名、新增项）都同步到设置
    connect(m_skipDirs, &QListWidget::itemChanged, this, [this](QListWidgetItem *) {
        QStringList dirs;
        for (int i = 0; i < m_skipDirs->count(); ++i) {
            const QString text = m_skipDirs->item(i)->text().trimmed();
            if (!text.isEmpty())
                dirs << text;
        }
        m_settings->setExtraSkipDirs(dirs);
    });

    skipBtnRow->addWidget(addSkipBtn);
    skipBtnRow->addWidget(removeSkipBtn);
    skipBtnRow->addStretch();
    skipLayout->addLayout(skipBtnRow);

    layout->addWidget(skipGroup, 1);

    return page;
}

// ================================================================ 缓存

QWidget *SettingsDialog::buildCachePage()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(18, 18, 18, 18);
    layout->setSpacing(14);

    auto *limitGroup = new QGroupBox(QStringLiteral("容量上限"), page);
    auto *limitForm = new QFormLayout(limitGroup);
    limitForm->setSpacing(10);

    m_memoryLimit = new QSpinBox(limitGroup);
    m_memoryLimit->setRange(16, 2048);
    m_memoryLimit->setSingleStep(16);
    m_memoryLimit->setSuffix(QStringLiteral(" MB"));
    m_memoryLimit->setValue(m_settings->thumbnailMemoryLimitMb());
    connect(m_memoryLimit, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int value) {
        m_settings->setThumbnailMemoryLimitMb(value);
        emit cacheLimitsChanged(value, m_settings->thumbnailDiskLimitMb());
    });
    limitForm->addRow(QStringLiteral("内存缓存"), m_memoryLimit);
    limitForm->addRow(QString(), makeHint(
        QStringLiteral("按字节数而非条目数限制。同样 1000 条缓存，"
                       "缩略图尺寸不同可能相差十倍，按条目控制会让大缩略图时内存失控。"),
        limitGroup));

    m_diskLimit = new QSpinBox(limitGroup);
    m_diskLimit->setRange(64, 65536);
    m_diskLimit->setSingleStep(128);
    m_diskLimit->setSuffix(QStringLiteral(" MB"));
    m_diskLimit->setValue(m_settings->thumbnailDiskLimitMb());
    connect(m_diskLimit, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int value) {
        m_settings->setThumbnailDiskLimitMb(value);
        emit cacheLimitsChanged(m_settings->thumbnailMemoryLimitMb(), value);
    });
    limitForm->addRow(QStringLiteral("磁盘缓存"), m_diskLimit);

    layout->addWidget(limitGroup);

    // ---------- 当前占用 ----------
    auto *usageGroup = new QGroupBox(QStringLiteral("当前状态"), page);
    auto *usageLayout = new QVBoxLayout(usageGroup);

    m_cacheUsage = new QLabel(usageGroup);
    m_cacheUsage->setTextInteractionFlags(Qt::TextSelectableByMouse);
    usageLayout->addWidget(m_cacheUsage);

    if (m_cache) {
        const ThumbnailCache::Stats s = m_cache->stats();
        const int total = s.memoryHits + s.diskHits + s.misses;
        const double rate = total > 0 ? (s.memoryHits + s.diskHits) * 100.0 / total : 0.0;

        m_cacheUsage->setText(QStringLiteral(
            "内存：%1 项 / %2\n"
            "命中：内存 %3 · 磁盘 %4 · 未命中 %5（总命中率 %6%）\n"
            "缓存目录：%7")
            .arg(s.memoryEntries)
            .arg(formatSize(s.memoryBytes))
            .arg(s.memoryHits)
            .arg(s.diskHits)
            .arg(s.misses)
            .arg(QString::number(rate, 'f', 1))
            .arg(s.cacheDir));
    } else {
        m_cacheUsage->setText(QStringLiteral("缓存模块不可用。"));
    }

    usageLayout->addWidget(makeHint(
        QStringLiteral("缓存键为「路径 + 修改时间 + 文件大小 + 缩略图尺寸」，"
                       "任一变化都会导致键变化，旧缓存自动失效。"),
        usageGroup));

    layout->addWidget(usageGroup);

    // ---------- 操作 ----------
    auto *actionGroup = new QGroupBox(QStringLiteral("维护"), page);
    auto *actionLayout = new QVBoxLayout(actionGroup);
    actionLayout->setSpacing(8);

    auto *clearCacheBtn = new QPushButton(
        Icons::get(Icons::Trash, ThemeManager::dangerColor(), 16),
        QStringLiteral("清空缩略图缓存"), actionGroup);
    clearCacheBtn->setProperty("danger", true);
    clearCacheBtn->setCursor(Qt::PointingHandCursor);
    connect(clearCacheBtn, &QPushButton::clicked, this, &SettingsDialog::onClearThumbnailCache);
    actionLayout->addWidget(clearCacheBtn);
    actionLayout->addWidget(makeHint(
        QStringLiteral("删除所有已生成的缩略图文件。下次浏览会重新生成，"
                       "可能较慢。此操作不影响资源索引、收藏与标签。"),
        actionGroup));

    layout->addWidget(actionGroup);
    layout->addStretch();

    return page;
}

// ================================================================ 播放

QWidget *SettingsDialog::buildPlaybackPage()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(18, 18, 18, 18);
    layout->setSpacing(14);

    auto *group = new QGroupBox(QStringLiteral("播放"), page);
    auto *form = new QFormLayout(group);
    form->setSpacing(10);

    m_volume = new QSpinBox(group);
    m_volume->setRange(0, 100);
    m_volume->setSuffix(QStringLiteral(" %"));
    m_volume->setValue(m_settings->volume());
    connect(m_volume, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &SettingsDialog::onVolumeChanged);
    form->addRow(QStringLiteral("默认音量"), m_volume);

    m_autoNext = new QCheckBox(QStringLiteral("播放结束后自动播放下一个"), group);
    m_autoNext->setChecked(m_settings->autoPlayNext());
    connect(m_autoNext, &QCheckBox::toggled, this, [this](bool checked) {
        m_settings->setAutoPlayNext(checked);
    });
    form->addRow(QString(), m_autoNext);

    m_resume = new QCheckBox(QStringLiteral("记住播放位置并续播"), group);
    m_resume->setChecked(m_settings->resumePlayback());
    connect(m_resume, &QCheckBox::toggled, this, [this](bool checked) {
        m_settings->setResumePlayback(checked);
    });
    form->addRow(QString(), m_resume);

    m_recordHistory = new QCheckBox(QStringLiteral("记录播放历史"), group);
    m_recordHistory->setChecked(m_settings->recordPlaybackHistory());
    connect(m_recordHistory, &QCheckBox::toggled, this, [this](bool checked) {
        m_settings->setRecordPlaybackHistory(checked);
    });
    form->addRow(QString(), m_recordHistory);
    form->addRow(QString(), makeHint(
        QStringLiteral("关闭后将不再写入播放记录，「最近播放」分组也会停止更新。"
                       "已有的历史记录不会被删除。"),
        group));

    layout->addWidget(group);
    layout->addStretch();

    return page;
}

// ================================================================ 高级

QWidget *SettingsDialog::buildAdvancedPage()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(18, 18, 18, 18);
    layout->setSpacing(14);

    // ---------- 性能 ----------
    auto *perfGroup = new QGroupBox(QStringLiteral("性能"), page);
    auto *perfForm = new QFormLayout(perfGroup);
    perfForm->setSpacing(10);

    m_concurrency = new QSpinBox(perfGroup);
    m_concurrency->setRange(1, 32);
    m_concurrency->setValue(m_taskManager ? m_taskManager->concurrency() : 4);
    connect(m_concurrency, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int value) {
        if (m_taskManager)
            m_taskManager->setConcurrency(value);
    });
    perfForm->addRow(QStringLiteral("后台任务并发数"), m_concurrency);
    perfForm->addRow(QString(), makeHint(
        QStringLiteral("限制同时执行的扫描/缩略图任务数量。调高在 SSD 上可能更快，"
                       "但内存峰值会上升；机械硬盘上调高通常反而变慢（IO 排队）。"
                       "修改立即生效。"),
        perfGroup));

    layout->addWidget(perfGroup);

    // ---------- 日志 ----------
    auto *logGroup = new QGroupBox(QStringLiteral("日志"), page);
    auto *logLayout = new QVBoxLayout(logGroup);
    logLayout->setSpacing(10);

    auto *levelForm = new QFormLayout();
    m_logLevel = new QComboBox(logGroup);
    m_logLevel->addItem(QStringLiteral("Debug（最详细）"), QStringLiteral("DEBUG"));
    m_logLevel->addItem(QStringLiteral("Info（推荐）"),    QStringLiteral("INFO"));
    m_logLevel->addItem(QStringLiteral("Warning"),         QStringLiteral("WARNING"));
    m_logLevel->addItem(QStringLiteral("Error（仅错误）"),  QStringLiteral("ERROR"));

    const QString currentLevel = m_settings->logLevel();
    for (int i = 0; i < m_logLevel->count(); ++i) {
        if (m_logLevel->itemData(i).toString() == currentLevel) {
            m_logLevel->setCurrentIndex(i);
            break;
        }
    }

    connect(m_logLevel, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        const QString level = m_logLevel->currentData().toString();
        m_settings->setLogLevel(level);
        // 立即生效，不需要重启
        Logger::instance().setMinimumLevel(Logger::levelFromString(level));
    });
    levelForm->addRow(QStringLiteral("日志级别"), m_logLevel);
    logLayout->addLayout(levelForm);

    logLayout->addWidget(makePathRow(QStringLiteral("日志文件"), &m_logPath, logGroup));

    auto *logBtnRow = new QHBoxLayout();
    auto *openLogBtn = new QPushButton(Icons::get(Icons::Folder, ThemeManager::textSecondary(), 15),
                                       QStringLiteral("打开日志目录"), logGroup);
    auto *clearLogBtn = new QPushButton(Icons::get(Icons::Trash, ThemeManager::dangerColor(), 15),
                                        QStringLiteral("清空日志"), logGroup);
    connect(openLogBtn, &QPushButton::clicked, this, &SettingsDialog::onOpenLogFolder);
    connect(clearLogBtn, &QPushButton::clicked, this, &SettingsDialog::onClearLogs);
    logBtnRow->addWidget(openLogBtn);
    logBtnRow->addWidget(clearLogBtn);
    logBtnRow->addStretch();
    logLayout->addLayout(logBtnRow);

    layout->addWidget(logGroup);

    // ---------- 文件位置 ----------
    auto *pathGroup = new QGroupBox(QStringLiteral("文件位置"), page);
    auto *pathLayout = new QVBoxLayout(pathGroup);
    pathLayout->setSpacing(8);

    pathLayout->addWidget(makePathRow(QStringLiteral("配置文件"), &m_configPath, pathGroup));
    pathLayout->addWidget(makePathRow(QStringLiteral("数据目录"), &m_dataPath, pathGroup));

    auto *openConfigBtn = new QPushButton(
        Icons::get(Icons::Folder, ThemeManager::textSecondary(), 15),
        QStringLiteral("打开配置目录"), pathGroup);
    openConfigBtn->setCursor(Qt::PointingHandCursor);
    connect(openConfigBtn, &QPushButton::clicked, this, &SettingsDialog::onOpenConfigFolder);

    auto *configBtnRow = new QHBoxLayout();
    configBtnRow->addWidget(openConfigBtn);
    configBtnRow->addStretch();
    pathLayout->addLayout(configBtnRow);

    pathLayout->addWidget(makeHint(
        QStringLiteral("运行时数据一律不写入程序安装目录——安装目录通常只读，"
                       "且卸载时会连同数据一起删除。"),
        pathGroup));

    layout->addWidget(pathGroup);
    layout->addStretch();

    return page;
}

// ================================================================ 操作

void SettingsDialog::refreshPaths()
{
    if (m_configPath)
        m_configPath->setText(m_settings->configFilePath());
    if (m_logPath)
        m_logPath->setText(Logger::instance().currentLogFile());
    if (m_dataPath)
        m_dataPath->setText(AppPaths::dataRoot());
}

void SettingsDialog::onOpenLogFolder()
{
    openFolderWithFeedback(this,
                           QFileInfo(Logger::instance().currentLogFile()).absolutePath(),
                           QStringLiteral("打开日志目录"));
}

void SettingsDialog::onOpenConfigFolder()
{
    openFolderWithFeedback(this,
                           QFileInfo(m_settings->configFilePath()).absolutePath(),
                           QStringLiteral("打开配置目录"));
}

void SettingsDialog::onClearLogs()
{
    const auto answer = QMessageBox::question(
        this, QStringLiteral("清空日志"),
        QStringLiteral("将删除当前的日志文件。\n\n确定继续吗？"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

    if (answer != QMessageBox::Yes)
        return;

    Logger::instance().clearLogFile();
    refreshPaths();

    QMessageBox::information(this, QStringLiteral("清空日志"),
                             QStringLiteral("日志已清空。下一条日志会写入新文件。"));
}

void SettingsDialog::onClearThumbnailCache()
{
    if (!m_cache)
        return;

    const ThumbnailCache::Stats s = m_cache->stats();

    const auto answer = QMessageBox::question(
        this, QStringLiteral("清空缩略图缓存"),
        QStringLiteral("将删除全部已生成的缩略图（内存 %1 项 / %2，以及磁盘缓存）。\n\n"
                       "下次浏览会重新生成，可能较慢。\n"
                       "资源索引、收藏与标签不受影响。\n\n确定继续吗？")
            .arg(s.memoryEntries)
            .arg(formatSize(s.memoryBytes)),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

    if (answer != QMessageBox::Yes)
        return;

    m_cache->clearAll();

    // 清空后立即刷新面板上的统计，让用户看到效果
    const ThumbnailCache::Stats after = m_cache->stats();
    m_cacheUsage->setText(QStringLiteral(
        "内存：%1 项 / %2\n"
        "缓存目录：%3\n\n"
        "已清空。返回主界面浏览资源时会重新生成。")
        .arg(after.memoryEntries)
        .arg(formatSize(after.memoryBytes))
        .arg(after.cacheDir));
}

void SettingsDialog::onResetDefaults()
{
    const auto answer = QMessageBox::question(
        this, QStringLiteral("恢复默认设置"),
        QStringLiteral("将把所有设置恢复为默认值。\n\n"
                       "不会删除资源索引、收藏、标签或播放记录。\n"
                       "但窗口布局与上次浏览的目录会被重置。\n\n确定继续吗？"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

    if (answer != QMessageBox::Yes)
        return;

    m_settings->resetToDefaults();

    /*!
     * 重置后必须让界面控件同步到新值。
     *
     * 这里刻意选择"提示用户重开对话框"而不是逐个控件 setValue：
     * 逐个同步需要维护一份"设置项 ↔ 控件"的映射，
     * 新增设置项时极易漏掉一处，造成界面显示与真实设置不一致——
     * 那种 bug 比多点一次按钮烦人得多。
     */
    QMessageBox::information(this, QStringLiteral("恢复默认设置"),
        QStringLiteral("设置已恢复默认值。\n\n"
                       "重新打开本窗口即可看到全部改动。"));
}

void SettingsDialog::onVolumeChanged(int value)
{
    m_settings->setVolume(value);
}

} // namespace media
