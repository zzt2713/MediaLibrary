#include "SettingsManager.h"
#include "AppPaths.h"
#include "Logger.h"

#include <QFileInfo>
#include <QDir>
#include <QDebug>
// Keys:: / Defaults:: 里的字符串常量声明为 QLatin1String。
// 用 QLatin1String 而不是裸 const char* 的原因：它自带长度信息，
// QString::fromLatin1() 才有对应重载可以接受（Qt 6 去掉了接受 const char* 的版本）。
#include <QLatin1String>

namespace media {

namespace {

/*!
 * 设置键。集中定义在一个匿名命名空间里。
 *
 * 用 constexpr char 而不是 #define：有作用域、有类型，调试器里能看。
 * 前缀分组（ui/、scan/、cache/…）让 ini 文件本身可读，
 * 用户手动编辑时也能看懂哪块是哪块。
 */
namespace Keys {
constexpr QLatin1String ScanOnStartup { "scan/onStartup" };
constexpr QLatin1String RestoreLastFolder { "ui/restoreLastFolder" };
constexpr QLatin1String LastFolder { "ui/lastFolder" };
constexpr QLatin1String ViewMode { "ui/viewMode" };
constexpr QLatin1String SortKey { "ui/sortKey" };
constexpr QLatin1String ThumbnailSize { "ui/thumbnailSize" };
constexpr QLatin1String InspectorVisible { "ui/inspectorVisible" };
constexpr QLatin1String SidebarWidth { "ui/sidebarWidth" };
constexpr QLatin1String InspectorWidth { "ui/inspectorWidth" };
constexpr QLatin1String WindowGeometry { "ui/windowGeometry" };
constexpr QLatin1String WindowState { "ui/windowState" };
constexpr QLatin1String ShowCacheStats { "ui/showCacheStats" };

constexpr QLatin1String RecursiveScan { "scan/recursive" };
constexpr QLatin1String ReadExifOnScan { "scan/readExif" };
constexpr QLatin1String SkipHidden { "scan/skipHidden" };
constexpr QLatin1String ExtraSkipDirs { "scan/extraSkipDirs" };
constexpr QLatin1String WatchFileSystem { "scan/watchFileSystem" };

constexpr QLatin1String ThumbMemoryMb { "cache/thumbnailMemoryMb" };
constexpr QLatin1String ThumbDiskMb { "cache/thumbnailDiskMb" };

constexpr QLatin1String Volume { "playback/volume" };
constexpr QLatin1String AutoPlayNext { "playback/autoPlayNext" };
constexpr QLatin1String ResumePlayback { "playback/resumePlayback" };
constexpr QLatin1String RecordHistory { "playback/recordHistory" };

constexpr QLatin1String LogLevel { "logging/level" };
} // namespace Keys

/*!
 * 默认值。集中在这里，让"程序默认是什么行为"一目了然，
 * 而不是散落在三十个 getter 里。
 */
namespace Defaults {
constexpr bool   kScanOnStartup   = false;  // 大库上启动即扫描会让程序看起来卡住
constexpr bool   kRestoreLastFolder = true;
constexpr QLatin1String kViewMode { "grid" };
constexpr QLatin1String kSortKey { "mtime_desc" };
constexpr int    kThumbnailSize   = 28;     // 滑块值，约对应 160px
constexpr bool   kInspectorVisible = true;
constexpr int    kSidebarWidth    = 220;
constexpr int    kInspectorWidth  = 320;
constexpr bool   kShowCacheStats  = true;

constexpr bool   kRecursiveScan   = true;
constexpr bool   kReadExifOnScan  = true;
constexpr bool   kSkipHidden      = true;
constexpr bool   kWatchFileSystem = true;

constexpr int    kThumbMemoryMb   = 128;
constexpr int    kThumbDiskMb     = 1024;

constexpr int    kVolume          = 80;
constexpr bool   kAutoPlayNext    = true;
constexpr bool   kResumePlayback  = true;
constexpr bool   kRecordHistory   = true;

constexpr QLatin1String kLogLevel { "INFO" }; // 默认 INFO：Debug 量太大，日常不需要
} // namespace Defaults

} // namespace

SettingsManager::SettingsManager(QObject *parent)
    : QObject(parent)
{
    /*!
     * 用 IniFormat + 显式路径，而不是默认的"平台原生格式"。
     *
     * Windows 上默认会写注册表——那意味着设置无法随程序文件夹一起带走，
     * 排查问题时也没法直接看文件。ini 更透明。
     *
     * 关于编码：Qt 5 时代需要 setIniCodec("UTF-8")，Qt 6 已移除该 API
     * （ini 文件的编码在 Qt 6 中固定按 UTF-8 处理），所以这里不再设置。
     */
    m_settings = new QSettings(AppPaths::configFile(), QSettings::IniFormat, this);

    LOG_INFO("Settings") << "config file: " << m_settings->fileName();
}

SettingsManager::~SettingsManager()
{
    // 析构时兜底写盘。QSettings 自己也会在析构时写，
    // 但显式 sync 能保证"对象销毁 → 数据落盘"这个顺序在日志里可见。
    sync();
}

void SettingsManager::sync()
{
    if (m_settings) {
        m_settings->sync();

        // 检查写盘是否真的成功。磁盘满/无权限时 sync 会失败但不会抛异常，
        // 不检查的话用户会以为设置保存了，重启后却发现没生效。
        if (m_settings->status() != QSettings::NoError) {
            const QString reason = (m_settings->status() == QSettings::AccessError)
                                       ? QStringLiteral("无写入权限或磁盘只读")
                                       : QStringLiteral("格式错误或磁盘空间不足");
            LOG_ERROR("Settings") << "failed to save settings: " << reason;
        }
    }
}

QString SettingsManager::configFilePath() const
{
    return m_settings ? m_settings->fileName() : AppPaths::configFile();
}

QVariant SettingsManager::value(const QString &key, const QVariant &defaultValue) const
{
    if (!m_settings)
        return defaultValue;

    // key 已经是 QString（Keys:: 的 QLatin1String 在传参时隐式转换过来的），
    // 不需要也不能再 fromLatin1 —— 那是对 QString 做 Latin-1 转换，
    // 既多余又会导致重载不匹配（C2665）。
    return m_settings->value(key, defaultValue);
}

void SettingsManager::setValue(const QString &key, const QVariant &val)
{
    if (!m_settings)
        return;

    // 同上：key 已是 QString，直接用。
    // 也不发不必要的转换开销 —— 这个函数在启动时会被调用几十次。
    if (m_settings->value(key) == val)
        return;

    m_settings->setValue(key, val);
    emit settingChanged(key);
}

void SettingsManager::resetToDefaults()
{
    if (!m_settings)
        return;

    m_settings->clear();
    sync();
    emit settingChanged(QString());
}

// ---------------------------------------------------------------- 启动行为

bool SettingsManager::scanOnStartup() const
{
    return value(Keys::ScanOnStartup, Defaults::kScanOnStartup).toBool();
}
void SettingsManager::setScanOnStartup(bool enable)
{
    setValue(Keys::ScanOnStartup, enable);
}

bool SettingsManager::restoreLastFolder() const
{
    return value(Keys::RestoreLastFolder, Defaults::kRestoreLastFolder).toBool();
}
void SettingsManager::setRestoreLastFolder(bool enable)
{
    setValue(Keys::RestoreLastFolder, enable);
}

QString SettingsManager::lastFolder() const
{
    return value(Keys::LastFolder, QString()).toString();
}
void SettingsManager::setLastFolder(const QString &path)
{
    setValue(Keys::LastFolder, path);
}

QString SettingsManager::viewMode() const
{
    return value(Keys::ViewMode, QString(Defaults::kViewMode)).toString();
}
void SettingsManager::setViewMode(const QString &mode)
{
    setValue(Keys::ViewMode, mode);
}

QString SettingsManager::sortKey() const
{
    return value(Keys::SortKey, QString(Defaults::kSortKey)).toString();
}
void SettingsManager::setSortKey(const QString &key)
{
    setValue(Keys::SortKey, key);
}

int SettingsManager::thumbnailSize() const
{
    return value(Keys::ThumbnailSize, Defaults::kThumbnailSize).toInt();
}
void SettingsManager::setThumbnailSize(int val)
{
    setValue(Keys::ThumbnailSize, qBound(0, val, 100));
}

// ---------------------------------------------------------------- 扫描

bool SettingsManager::recursiveScan() const
{
    return value(Keys::RecursiveScan, Defaults::kRecursiveScan).toBool();
}
void SettingsManager::setRecursiveScan(bool enable)
{
    setValue(Keys::RecursiveScan, enable);
}

bool SettingsManager::readExifOnScan() const
{
    return value(Keys::ReadExifOnScan, Defaults::kReadExifOnScan).toBool();
}
void SettingsManager::setReadExifOnScan(bool enable)
{
    setValue(Keys::ReadExifOnScan, enable);
}

bool SettingsManager::skipHidden() const
{
    return value(Keys::SkipHidden, Defaults::kSkipHidden).toBool();
}
void SettingsManager::setSkipHidden(bool enable)
{
    setValue(Keys::SkipHidden, enable);
}

QStringList SettingsManager::extraSkipDirs() const
{
    return value(Keys::ExtraSkipDirs, QStringList()).toStringList();
}
void SettingsManager::setExtraSkipDirs(const QStringList &dirs)
{
    setValue(Keys::ExtraSkipDirs, dirs);
}

bool SettingsManager::watchFileSystem() const
{
    return value(Keys::WatchFileSystem, Defaults::kWatchFileSystem).toBool();
}
void SettingsManager::setWatchFileSystem(bool enable)
{
    setValue(Keys::WatchFileSystem, enable);
}

// ---------------------------------------------------------------- 缓存

int SettingsManager::thumbnailMemoryLimitMb() const
{
    return value(Keys::ThumbMemoryMb, Defaults::kThumbMemoryMb).toInt();
}
void SettingsManager::setThumbnailMemoryLimitMb(int mb)
{
    // 下限 16MB 保证缓存有意义，上限 2048MB 防止填一个离谱值把内存吃光
    setValue(Keys::ThumbMemoryMb, qBound(16, mb, 2048));
}

int SettingsManager::thumbnailDiskLimitMb() const
{
    return value(Keys::ThumbDiskMb, Defaults::kThumbDiskMb).toInt();
}
void SettingsManager::setThumbnailDiskLimitMb(int mb)
{
    setValue(Keys::ThumbDiskMb, qBound(64, mb, 65536));
}

// ---------------------------------------------------------------- 界面

bool SettingsManager::inspectorVisible() const
{
    return value(Keys::InspectorVisible, Defaults::kInspectorVisible).toBool();
}
void SettingsManager::setInspectorVisible(bool visible)
{
    setValue(Keys::InspectorVisible, visible);
}

int SettingsManager::sidebarWidth() const
{
    return value(Keys::SidebarWidth, Defaults::kSidebarWidth).toInt();
}
void SettingsManager::setSidebarWidth(int width)
{
    setValue(Keys::SidebarWidth, qBound(150, width, 500));
}

int SettingsManager::inspectorWidth() const
{
    return value(Keys::InspectorWidth, Defaults::kInspectorWidth).toInt();
}
void SettingsManager::setInspectorWidth(int width)
{
    setValue(Keys::InspectorWidth, qBound(240, width, 600));
}

QByteArray SettingsManager::windowGeometry() const
{
    return value(Keys::WindowGeometry, QByteArray()).toByteArray();
}
void SettingsManager::setWindowGeometry(const QByteArray &geometry)
{
    setValue(Keys::WindowGeometry, geometry);
}

QByteArray SettingsManager::windowState() const
{
    return value(Keys::WindowState, QByteArray()).toByteArray();
}
void SettingsManager::setWindowState(const QByteArray &state)
{
    setValue(Keys::WindowState, state);
}

bool SettingsManager::showCacheStats() const
{
    return value(Keys::ShowCacheStats, Defaults::kShowCacheStats).toBool();
}
void SettingsManager::setShowCacheStats(bool show)
{
    setValue(Keys::ShowCacheStats, show);
}

// ---------------------------------------------------------------- 播放

int SettingsManager::volume() const
{
    return value(Keys::Volume, Defaults::kVolume).toInt();
}
void SettingsManager::setVolume(int val)
{
    setValue(Keys::Volume, qBound(0, val, 100));
}

bool SettingsManager::autoPlayNext() const
{
    return value(Keys::AutoPlayNext, Defaults::kAutoPlayNext).toBool();
}
void SettingsManager::setAutoPlayNext(bool enable)
{
    setValue(Keys::AutoPlayNext, enable);
}

bool SettingsManager::resumePlayback() const
{
    return value(Keys::ResumePlayback, Defaults::kResumePlayback).toBool();
}
void SettingsManager::setResumePlayback(bool enable)
{
    setValue(Keys::ResumePlayback, enable);
}

bool SettingsManager::recordPlaybackHistory() const
{
    return value(Keys::RecordHistory, Defaults::kRecordHistory).toBool();
}
void SettingsManager::setRecordPlaybackHistory(bool enable)
{
    setValue(Keys::RecordHistory, enable);
}

// ---------------------------------------------------------------- 日志

QString SettingsManager::logLevel() const
{
    return value(Keys::LogLevel, QString(Defaults::kLogLevel)).toString();
}
void SettingsManager::setLogLevel(const QString &level)
{
    setValue(Keys::LogLevel, level);
}

} // namespace media
