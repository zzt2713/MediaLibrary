#ifndef SETTINGSDIALOG_H
#define SETTINGSDIALOG_H

#include <QDialog>

class QCheckBox;
class QSpinBox;
class QComboBox;
class QLabel;
class QPushButton;
class QListWidget;
class QTabWidget;

namespace media {

class SettingsManager;
class TaskManager;
class ThumbnailCache;

/*!
 * \brief 设置窗口。
 *
 * 内容按"用户能不能理解"分组，而不是按代码模块分组：
 *  - 外观：视图模式、缩略图尺寸、面板显隐（改完立刻看到效果，不需要重启）
 *  - 浏览与扫描：递归、EXIF、隐藏文件、跳过目录（影响下一次扫描）
 *  - 缓存：内存/磁盘上限、清空入口
 *  - 播放：音量、自动下一个、续播、历史记录
 *  - 高级：并发数、日志级别、配置/日志文件位置（便于排查）
 *
 * 大部分设置即时写入 SettingsManager，不需要"确定"才生效；
 * 底部按钮的语义是"关闭"而不是"提交"。只有那些需要重建资源的设置
 * （如缓存上限）才在关闭时统一应用。
 */
class SettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SettingsDialog(SettingsManager *settings,
                            TaskManager *taskManager,
                            ThumbnailCache *cache,
                            QWidget *parent = nullptr);
    ~SettingsDialog() override;

signals:
    /*! 缓存上限变化，需要 MainWindow 重新应用到 ThumbnailCache。 */
    void cacheLimitsChanged(int memoryMb, int diskMb);
    /*! 缩略图尺寸变化，需要 MainWindow 重排网格。 */
    void thumbnailSizeChanged(int sliderValue);

private slots:
    void onOpenLogFolder();
    void onOpenConfigFolder();
    void onClearLogs();
    void onClearThumbnailCache();
    void onResetDefaults();
    void onVolumeChanged(int value);

private:
    QWidget *buildAppearancePage();
    QWidget *buildScanPage();
    QWidget *buildCachePage();
    QWidget *buildPlaybackPage();
    QWidget *buildAdvancedPage();

    /*! 日志路径等信息需要在打开时刷新（可能在外部被清理过）。 */
    void refreshPaths();

    SettingsManager *m_settings = nullptr;
    TaskManager *m_taskManager = nullptr;
    ThumbnailCache *m_cache = nullptr;

    // 外观
    QComboBox *m_viewMode = nullptr;
    QSpinBox  *m_thumbSize = nullptr;
    QCheckBox *m_inspectorVisible = nullptr;
    QCheckBox *m_showCacheStats = nullptr;
    QCheckBox *m_restoreLastFolder = nullptr;

    // 扫描
    QCheckBox *m_scanOnStartup = nullptr;
    QCheckBox *m_recursive = nullptr;
    QCheckBox *m_readExif = nullptr;
    QCheckBox *m_skipHidden = nullptr;
    QCheckBox *m_watchFs = nullptr;
    QListWidget *m_skipDirs = nullptr;

    // 缓存
    QSpinBox *m_memoryLimit = nullptr;
    QSpinBox *m_diskLimit = nullptr;
    QLabel   *m_cacheUsage = nullptr;

    // 播放
    QSpinBox  *m_volume = nullptr;
    QCheckBox *m_autoNext = nullptr;
    QCheckBox *m_resume = nullptr;
    QCheckBox *m_recordHistory = nullptr;

    // 高级
    QSpinBox  *m_concurrency = nullptr;
    QComboBox *m_logLevel = nullptr;
    QLabel    *m_configPath = nullptr;
    QLabel    *m_logPath = nullptr;
    QLabel    *m_dataPath = nullptr;
};

} // namespace media

#endif // SETTINGSDIALOG_H
