#include "FileWatcher.h"
#include "../core/MediaScanner.h"

#include <QDir>
#include <QFileInfo>
#include <QDebug>

namespace media {

FileWatcher::FileWatcher(QObject *parent)
    : QObject(parent)
{
    m_watcher = new QFileSystemWatcher(this);

    connect(m_watcher, &QFileSystemWatcher::directoryChanged,
            this, &FileWatcher::onDirectoryChanged);
    connect(m_watcher, &QFileSystemWatcher::fileChanged,
            this, &FileWatcher::onFileChanged);

    // 去抖：单次触发，安静下来才真正处理
    m_debounceTimer = new QTimer(this);
    m_debounceTimer->setSingleShot(true);
    m_debounceTimer->setInterval(m_debounceMs);
    connect(m_debounceTimer, &QTimer::timeout, this, &FileWatcher::flushPending);

    /*!
     * 周期性重建监控列表。
     *
     * 为什么要定时重建：文件系统监控本质上会丢事件——
     * 网络盘断连、句柄被系统回收、监控上限被触顶、程序休眠后恢复。
     * 每 30 秒对账一次，能把这些"静默失效"补齐，代价极低。
     */
    m_rebuildTimer = new QTimer(this);
    m_rebuildTimer->setInterval(30000);
    connect(m_rebuildTimer, &QTimer::timeout, this, &FileWatcher::rebuildWatchList);
    m_rebuildTimer->start();
}

FileWatcher::~FileWatcher()
{
    // QFileSystemWatcher 是子对象，会自动析构；这里只需停掉定时器
    if (m_debounceTimer)
        m_debounceTimer->stop();
    if (m_rebuildTimer)
        m_rebuildTimer->stop();
}

void FileWatcher::setDebounceInterval(int ms)
{
    m_debounceMs = qMax(100, ms);
    if (m_debounceTimer)
        m_debounceTimer->setInterval(m_debounceMs);
}

void FileWatcher::setRoots(const QStringList &roots)
{
    m_roots.clear();
    for (const QString &r : roots) {
        const QString norm = QDir::cleanPath(QDir::fromNativeSeparators(r));
        if (!norm.isEmpty() && !m_roots.contains(norm) && QFileInfo(norm).isDir())
            m_roots.append(norm);
    }
    rebuildWatchList();
}

void FileWatcher::addRoot(const QString &path)
{
    const QString norm = QDir::cleanPath(QDir::fromNativeSeparators(path));
    if (norm.isEmpty() || m_roots.contains(norm))
        return;
    if (!QFileInfo(norm).isDir())
        return;

    m_roots.append(norm);
    rebuildWatchList();
}

void FileWatcher::removeRoot(const QString &path)
{
    const QString norm = QDir::cleanPath(QDir::fromNativeSeparators(path));
    if (m_roots.removeAll(norm) > 0)
        rebuildWatchList();
}

void FileWatcher::setSuspended(bool suspended)
{
    if (m_suspended == suspended)
        return;

    m_suspended = suspended;

    if (suspended) {
        // 挂起时丢掉累积的待处理项：扫描完成后本来就会统一刷新，
        // 这些事件已经没有意义了。
        m_pendingRoots.clear();
        if (m_debounceTimer)
            m_debounceTimer->stop();
    }
}

int FileWatcher::watchedCount() const
{
    return m_watcher ? m_watcher->directories().size() : 0;
}

void FileWatcher::rebuildWatchList()
{
    if (!m_watcher)
        return;

    // 先算目标列表，再整体替换。逐个 add/remove 容易在异常路径漏掉。
    QStringList desired;

    for (const QString &root : m_roots) {
        if (!QFileInfo(root).isDir())
            continue;

        // 根目录本身
        desired.append(root);

        // 直接子目录。不递归：句柄会不够，深层变更靠去抖后的重扫覆盖。
        int added = 0;
        const QList<QString> subs = MediaScanner::listSubDirectories(root);
        for (const QString &sub : subs) {
            if (added >= kMaxWatchedPerRoot)
                break;
            desired.append(sub);
            ++added;
        }

        if (subs.size() > kMaxWatchedPerRoot) {
            qDebug() << "[FileWatcher] root" << root
                     << "has" << subs.size() << "subdirs, watching only first"
                     << kMaxWatchedPerRoot << "(rest covered by manual refresh)";
        }

        if (desired.size() >= kMaxWatchedTotal)
            break;
    }

    // 总量封顶
    if (desired.size() > kMaxWatchedTotal)
        desired = desired.mid(0, kMaxWatchedTotal);

    const QStringList current = m_watcher->directories();
    if (current == desired)
        return;   // 没变化就不折腾，避免无意义的系统调用

    if (!current.isEmpty())
        m_watcher->removePaths(current);

    if (!desired.isEmpty()) {
        const QStringList failed = m_watcher->addPaths(desired);
        if (!failed.isEmpty())
            qDebug() << "[FileWatcher] failed to watch" << failed.size() << "paths";
    }

    emit watchListChanged(m_watcher->directories().size());
}

QString FileWatcher::rootFor(const QString &changedPath) const
{
    const QString changed = QDir::cleanPath(QDir::fromNativeSeparators(changedPath));

    // 从最长匹配开始找：嵌套根目录时应该归给更具体的那个，
    // 否则外层根重扫会覆盖内层的增量结果。
    QString best;
    for (const QString &root : m_roots) {
        if (changed == root
            || changed.startsWith(root + QLatin1Char('/'))) {
            if (root.size() > best.size())
                best = root;
        }
    }
    return best;
}

void FileWatcher::onDirectoryChanged(const QString &path)
{
    if (m_suspended)
        return;

    const QString root = rootFor(path);
    if (root.isEmpty())
        return;

    m_pendingRoots.insert(root);

    // 重启去抖定时器：持续有事件就持续推迟，直到安静满一个周期
    if (m_debounceTimer)
        m_debounceTimer->start();
}

void FileWatcher::onFileChanged(const QString &path)
{
    if (m_suspended)
        return;

    const QString root = rootFor(QFileInfo(path).absolutePath());
    if (root.isEmpty())
        return;

    m_pendingRoots.insert(root);

    if (m_debounceTimer)
        m_debounceTimer->start();
}

void FileWatcher::flushPending()
{
    if (m_suspended) {
        m_pendingRoots.clear();
        return;
    }

    // 取出并清空，避免在发出信号的过程中又被新事件填回来导致重复处理
    const QSet<QString> pending = m_pendingRoots;
    m_pendingRoots.clear();

    for (const QString &root : pending) {
        if (!QFileInfo(root).isDir()) {
            // 根目录本身没了（移动硬盘拔出 / 网络盘断开）
            qDebug() << "[FileWatcher] root disappeared:" << root;
        }
        emit directoryChanged(root);
    }

    // 变更可能新增/删除了子目录，重建监控列表
    if (!pending.isEmpty())
        rebuildWatchList();
}

} // namespace media
