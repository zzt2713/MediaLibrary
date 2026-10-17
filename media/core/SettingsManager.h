#ifndef SETTINGSMANAGER_H
#define SETTINGSMANAGER_H

#include <QObject>
#include <QString>
#include <QVariant>
#include <QStringList>
#include <QByteArray>
#include <QSettings>

namespace media {

/*!
 * \brief 应用设置的持久化与访问入口。
 *
 * 用 QSettings(IniFormat) 而不是注册表：
 *  - ini 文件可以直接打开查看，排查"为什么设置没生效"时非常有用；
 *  - 便于备份与迁移；
 *  - 跨平台行为一致。
 *
 * 所有设置项都通过具名的 getter/setter 访问，不暴露泛化的
 * "getValue(key)"。理由是：字符串 key 散落在各处时，
 * 拼错一个字母不会报错，只会静默用默认值，这类 bug 极难发现。
 * 把 key 收在本类内部，拼写错误变成编译错误。
 *
 * 不继承 QObject 也可以，但继承后可以在设置变化时发信号，
 * 让 UI 能响应（例如主题切换、缩略图尺寸变化）。
 */
class SettingsManager : public QObject
{
    Q_OBJECT

public:
    explicit SettingsManager(QObject *parent = nullptr);
    ~SettingsManager() override;

    // ---------- 通用 ----------

    /*! 把所有设置写回磁盘。析构时会自动调用。 */
    void sync();

    /*! 恢复全部设置为默认值。 */
    void resetToDefaults();

    /*! 配置文件路径，供"打开设置文件"这类操作使用。 */
    QString configFilePath() const;

    // ---------- 启动行为 ----------

    /*! 启动时是否自动扫描已登记的根目录。 */
    bool scanOnStartup() const;
    void setScanOnStartup(bool enable);

    /*! 是否在启动时恢复上次选中的目录。 */
    bool restoreLastFolder() const;
    void setRestoreLastFolder(bool enable);

    /*! 上次选中的目录路径。 */
    QString lastFolder() const;
    void setLastFolder(const QString &path);

    /*! 上次使用的视图模式（"grid" / "list"）。 */
    QString viewMode() const;
    void setViewMode(const QString &mode);

    /*! 上次的排序键。 */
    QString sortKey() const;
    void setSortKey(const QString &key);

    /*! 上次的缩略图尺寸（滑块值 0-100）。 */
    int thumbnailSize() const;
    void setThumbnailSize(int value);

    // ---------- 扫描 ----------

    /*! 扫描时是否递归子目录。 */
    bool recursiveScan() const;
    void setRecursiveScan(bool enable);

    /*! 扫描时是否读取 EXIF。关闭可以明显加快首次扫描。 */
    bool readExifOnScan() const;
    void setReadExifOnScan(bool enable);

    /*! 是否跳过隐藏目录与隐藏文件。 */
    bool skipHidden() const;
    void setSkipHidden(bool enable);

    /*! 自定义的跳过目录名列表（除内置的那些之外）。 */
    QStringList extraSkipDirs() const;
    void setExtraSkipDirs(const QStringList &dirs);

    /*! 是否启用文件系统监控。 */
    bool watchFileSystem() const;
    void setWatchFileSystem(bool enable);

    // ---------- 缓存 ----------

    /*! 缩略图内存缓存上限（MB）。 */
    int thumbnailMemoryLimitMb() const;
    void setThumbnailMemoryLimitMb(int mb);

    /*! 缩略图磁盘缓存上限（MB）。 */
    int thumbnailDiskLimitMb() const;
    void setThumbnailDiskLimitMb(int mb);

    // ---------- 界面 ----------

    /*! 右侧详情面板是否可见。 */
    bool inspectorVisible() const;
    void setInspectorVisible(bool visible);

    /*! 左侧栏宽度与详情面板宽度，用于恢复窗口布局。 */
    int sidebarWidth() const;
    void setSidebarWidth(int width);
    int inspectorWidth() const;
    void setInspectorWidth(int width);

    /*! 主窗口几何与状态。空 QByteArray 表示没有保存过。 */
    QByteArray windowGeometry() const;
    void setWindowGeometry(const QByteArray &geometry);
    QByteArray windowState() const;
    void setWindowState(const QByteArray &state);

    /*! 状态栏是否显示缓存统计。 */
    bool showCacheStats() const;
    void setShowCacheStats(bool show);

    // ---------- 播放 ----------

    /*! 播放音量（0-100）。 */
    int volume() const;
    void setVolume(int volume);

    /*! 视频播完后是否自动播下一个。 */
    bool autoPlayNext() const;
    void setAutoPlayNext(bool enable);

    /*! 是否记住并续播上次位置。 */
    bool resumePlayback() const;
    void setResumePlayback(bool enable);

    /*! 是否记录播放历史。 */
    bool recordPlaybackHistory() const;
    void setRecordPlaybackHistory(bool enable);

    // ---------- 日志 ----------

    /*! 日志级别（"DEBUG"/"INFO"/"WARNING"/"ERROR"）。 */
    QString logLevel() const;
    void setLogLevel(const QString &level);

signals:
    /*! 任何设置项变化时发出，参数为内部 key。
     *  UI 可以据此做粗粒度的响应，不需要为每个设置项连一个信号。 */
    void settingChanged(const QString &key);

private:
    /*! 统一的读入口。带默认值，避免各处重复写 fallback。 */
    QVariant value(const QString &key, const QVariant &defaultValue) const;
    /*! 统一的写入入口，内部会发 settingChanged。 */
    void setValue(const QString &key, const QVariant &value);

    /*!
     * QSettings 直接按值持有即可，不需要 pimpl。
     * pimpl 的价值在于隐藏昂贵的头文件依赖，而 QSettings 本身很轻，
     * 为一个可移动的小类型加一层间接跳转是纯粹的复杂度。
     */
    mutable QSettings *m_settings = nullptr;
};

} // namespace media

#endif // SETTINGSMANAGER_H
