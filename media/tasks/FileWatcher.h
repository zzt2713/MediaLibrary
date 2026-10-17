#ifndef FILEWATCHER_H
#define FILEWATCHER_H

#include <QObject>
#include <QFileSystemWatcher>
#include <QTimer>
#include <QSet>
#include <QStringList>

namespace media {

/*!
 * \brief 基于 QFileSystemWatcher 的目录变更监控。
 *
 * 三个必须处理的实际问题：
 *
 * 1. 风暴合并。复制一个含 500 张图的文件夹，会瞬间产生几百次目录变更事件。
 *    每次都触发重扫等于把磁盘打满。所以用单次触发的去抖定时器，
 *    在安静 800ms 之后才真正发起一次扫描。
 *
 * 2. 句柄上限。QFileSystemWatcher 在 Windows 上底层用 ReadDirectoryChangesW，
 *    每个被监控目录占一个句柄；Linux 上受 inotify watch 数限制。
 *    监控几千个子目录会直接失败，所以只监控「已登记的根目录」及其直接子目录，
 *    上限 256 个，超出部分靠手动刷新兜底。
 *
 * 3. 递归失效。QFileSystemWatcher 不递归，子目录里新增文件不会触发事件。
 *    因此每次变更后重新枚举直接子目录并更新监控列表——新增的子目录也能被纳管。
 */
class FileWatcher : public QObject
{
    Q_OBJECT

public:
    explicit FileWatcher(QObject *parent = nullptr);
    ~FileWatcher() override;

    /*! 设置要监控的根目录集合。会替换掉之前的监控列表。 */
    void setRoots(const QStringList &roots);

    /*! 追加一个根目录。 */
    void addRoot(const QString &path);

    /*! 移除一个根目录。 */
    void removeRoot(const QString &path);

    /*! 当前正在监控的目录数量，供状态显示。 */
    int watchedCount() const;

    /*! 临时挂起监控。全量扫描期间应挂起，避免扫描自身的写入触发事件循环。 */
    void setSuspended(bool suspended);
    bool isSuspended() const { return m_suspended; }

    /*! 变更去抖时长（毫秒）。默认 800。 */
    void setDebounceInterval(int ms);

signals:
    /*!
     * \brief 某个根目录下检测到变更，需要重新扫描。
     *
     * 只报告根目录而不是具体文件：重扫本身是增量的（靠 mtime/size 比对），
     * 精确定位到单个文件带来的收益，抵不过维护映射关系的复杂度。
     */
    void directoryChanged(const QString &rootPath);

    /*! 监控列表发生变化（数量增减）。 */
    void watchListChanged(int count);

private slots:
    void onDirectoryChanged(const QString &path);
    void onFileChanged(const QString &path);
    void flushPending();

private:
    void rebuildWatchList();
    /*! 找到某个变更路径归属的根目录。变更可能来自子目录，需要向上查找。 */
    QString rootFor(const QString &changedPath) const;

    QFileSystemWatcher *m_watcher = nullptr;
    QTimer *m_debounceTimer = nullptr;
    QTimer *m_rebuildTimer = nullptr;

    QStringList m_roots;
    QSet<QString> m_pendingRoots;    // 待处理变更的根目录集合
    bool m_suspended = false;
    int  m_debounceMs = 800;

    /*! 单个根目录下最多监控的直接子目录数，防止句柄耗尽。 */
    static constexpr int kMaxWatchedPerRoot = 64;
    static constexpr int kMaxWatchedTotal = 256;
};

} // namespace media

#endif // FILEWATCHER_H
