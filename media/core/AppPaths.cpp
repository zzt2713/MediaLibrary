#include "AppPaths.h"

#include <QStandardPaths>
#include <QCoreApplication>
#include <QDir>
#include <QDateTime>
#include <QFileInfo>
#include <QDebug>

namespace media {

namespace {

/*!
 * 便携模式的数据目录名，位于 exe 同级。
 *
 * 为什么不用 QStandardPaths::AppDataLocation：实测本机环境下进程写不进
 *   %APPDATA%\MediaLibrary（Roaming 目录从未被创建，QSettings 退避到了
 *   LocalLow），SQLite 因此报 "unable to open database file" 而拒绝启动。
 *   把数据放到 exe 旁边的 medialibrary\ 里，路径确定、必然可写、便于携带。
 *
 * 代价（已在任务书中标注）：任务目标.txt 要求"数据写 QStandardPaths、
 * 不进程序目录"。这是本机学习项目下的刻意取舍；若日后打包进 Program Files，
 * 需改回 QStandardPaths——所以下面保留了回退链，exe 目录不可写时自动退回去。
 *
 * 注意：exe 旁即构建目录，clean 重建可能清掉数据（库、日志、缓存）。
 */
const char *kPortableDirName = "medialibrary";

/*! QStandardPaths 的应用数据目录（Windows: %APPDATA%\<org>\<app>）。 */
const char *kAppDirName = "MediaLibrary";

} // namespace

QString AppPaths::dataRoot()
{
    /*!
     * 首选：exe 同级的 medialibrary\（便携、可写、路径确定）。
     * isWritableLocation 会真写一次探针文件——目录存在但只读时也能识别，
     * 避免"建出来了但 SQLite 还是写不进去"这种半失败状态。
     */
    const QString portable =
        QDir(QCoreApplication::applicationDirPath())
            .filePath(QString::fromLatin1(kPortableDirName));

    if (isWritableLocation(portable))
        return portable;

    /*!
     * 回退：exe 目录不可写（典型是装进了 Program Files）。
     * 退回 QStandardPaths 的 AppDataLocation，它在正常桌面环境总是可写。
     */
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (base.isEmpty())
        base = QDir::temp().filePath(QString::fromLatin1(kAppDirName));

    qWarning() << "[AppPaths] portable dir not writable, falling back to" << base;
    ensureDir(base);
    return base;
}

QString AppPaths::databaseFile()
{
    return QDir(dataRoot()).filePath(QStringLiteral("library.db"));
}

QString AppPaths::logDir()
{
    const QString dir = QDir(dataRoot()).filePath(QStringLiteral("logs"));
    ensureDir(dir);
    return dir;
}

QString AppPaths::logFile()
{
    // 按日期分文件。单个日志文件无限增长会让排查变难（打开慢、难定位），
    // 按天切分是成本最低的方案——不需要引入日志轮转库。
    const QString name = QStringLiteral("app-%1.log")
                             .arg(QDate::currentDate().toString(QStringLiteral("yyyy-MM-dd")));
    return QDir(logDir()).filePath(name);
}

QString AppPaths::thumbnailCacheDir()
{
    // 缓存放在 dataRoot() 下，与数据库/日志/配置同根，便于携带与清理。
    // 丢了只是重新生成缩略图，没有数据损失。
    const QString dir = QDir(dataRoot()).filePath(QStringLiteral("thumbnails"));
    ensureDir(dir);
    return dir;
}

QString AppPaths::configDir()
{
    // 配置放在 dataRoot() 下的 config\ 子目录，与数据库分开：
    // 用户可能只想重置配置而不想丢索引与收藏。
    const QString base = QDir(dataRoot()).filePath(QStringLiteral("config"));
    ensureDir(base);
    return base;
}

QString AppPaths::configFile()
{
    return QDir(configDir()).filePath(QStringLiteral("settings.ini"));
}

bool AppPaths::ensureDir(const QString &dir)
{
    if (dir.isEmpty())
        return false;

    QDir d(dir);
    if (d.exists())
        return true;

    if (d.mkpath(QStringLiteral(".")))
        return true;

    qWarning() << "[AppPaths] failed to create directory:" << dir;
    return false;
}

bool AppPaths::isWritableLocation(const QString &dir)
{
    if (dir.isEmpty())
        return false;

    if (!ensureDir(dir))
        return false;

    /*!
     * 不止看 exists()，而是真的写一个文件试试。
     *
     * 原因：目录可能存在但只读（比如被放在 Program Files 下的旧数据目录），
     * QFileInfo::isWritable() 在 Windows ACL 下的判断并不总是可靠。
     * 实际写一次是最直接的验证——失败就说明真的不能写。
     */
    const QString probe = QDir(dir).filePath(
        QStringLiteral(".write_probe_%1").arg(QDateTime::currentMSecsSinceEpoch()));

    QFile file(probe);
    if (!file.open(QIODevice::WriteOnly))
        return false;

    file.close();
    file.remove();

    return true;
}

} // namespace media
