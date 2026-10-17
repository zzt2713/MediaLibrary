#include "Logger.h"
#include "AppPaths.h"

#include <QFile>
#include <QDir>
#include <QDateTime>
#include <QMutexLocker>
#include <QDebug>
#include <QCoreApplication>
#include <QThread>

namespace media {

namespace {

/*! 级别名称。固定宽度便于肉眼对齐日志。 */
const char *kLevelNames[] = { "DEBUG", "INFO ", "WARN ", "ERROR" };

/*! 把线程指针转成短标识，用于区分是哪个线程打的日志。 */
QString threadTag()
{
    const Qt::HANDLE handle = QThread::currentThreadId();
    return QStringLiteral("T%1").arg(reinterpret_cast<quintptr>(handle) & 0xffff, 4, 16, QLatin1Char('0'));
}

} // namespace

Logger &Logger::instance()
{
    // C++11 起，函数内静态变量的初始化是线程安全的（magic statics），
    // 所以这里不需要额外加锁。
    static Logger logger;
    return logger;
}

Logger::Logger() = default;

Logger::~Logger()
{
    QMutexLocker locker(&m_mutex);
    if (m_file && m_file->isOpen()) {
        m_stream.flush();
        m_file->close();
    }
}

void Logger::setMinimumLevel(Level level)
{
    QMutexLocker locker(&m_mutex);
    m_minimumLevel = level;
}

Logger::Level Logger::minimumLevel() const
{
    QMutexLocker locker(&m_mutex);
    return m_minimumLevel;
}

QString Logger::levelName(Level level)
{
    const int index = static_cast<int>(level);
    if (index < 0 || index > 3)
        return QStringLiteral("INFO ");
    return QString::fromLatin1(kLevelNames[index]).trimmed();
}

Logger::Level Logger::levelFromString(const QString &name)
{
    const QString upper = name.trimmed().toUpper();
    if (upper == QLatin1String("DEBUG"))   return Debug;
    if (upper == QLatin1String("INFO"))    return Info;
    if (upper == QLatin1String("WARNING") || upper == QLatin1String("WARN")) return Warning;
    if (upper == QLatin1String("ERROR"))   return Error;
    return Debug;
}

bool Logger::ensureOpen()
{
    // 调用方必须已持有 m_mutex
    if (m_file && m_file->isOpen())
        return true;

    // 只尝试打开一次，失败后不再反复重试。
    // 否则每条日志都会走一次失败的 open()，在磁盘满/无权限时会造成明显卡顿。
    if (m_openAttempted)
        return false;

    m_openAttempted = true;

    const QString path = AppPaths::logFile();
    AppPaths::ensureDir(QFileInfo(path).absolutePath());

    m_file = std::make_unique<QFile>(path);
    if (!m_file->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        // 打开失败时保留 m_file 但标记未打开，ensureOpen 下次会返回 false
        qWarning() << "[Logger] cannot open log file:" << path
                   << m_file->errorString();
        m_file.reset();
        return false;
    }

    m_stream.setDevice(m_file.get());
    // 明确用 UTF-8：日志里有中文，用系统默认编码在别的机器上打开会乱码
    m_stream.setEncoding(QStringConverter::Utf8);

    return true;
}

void Logger::log(Level level, const QString &message, const QString &category)
{
    {
        QMutexLocker locker(&m_mutex);

        if (level < m_minimumLevel)
            return;

        if (!ensureOpen()) {
            // 文件不可用时仍然走 qDebug，保证日志至少能看到
            if (m_echoToQt)
                qDebug().noquote() << QStringLiteral("[%1] [%2] %3")
                                          .arg(levelName(level), category, message);
            return;
        }

        const QString timestamp =
            QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));

        // 格式：时间 | 级别 | 线程 | 分类 | 消息
        // 线程标识是必需的——本项目的核心是跨线程任务，
        // 排查问题时"这行日志是哪个线程打的"经常是决定性信息。
        QString line = QStringLiteral("%1 | %2 | %3 | %4 | %5")
                           .arg(timestamp,
                                levelName(level),
                                threadTag(),
                                category.isEmpty() ? QStringLiteral("app") : category,
                                message);

        m_stream << line << '\n';

        // 立即刷新。崩溃时最后几条日志往往最有价值，
        // 缓冲住反而会让它们丢掉。日志量不大，这点开销可以接受。
        m_stream.flush();

        if (m_echoToQt) {
            // 按级别分流到 Qt 的对应通道，便于在 Qt Creator 里按级别过滤
            switch (level) {
            case Warning:
                qWarning().noquote() << line;
                break;
            case Error:
                qCritical().noquote() << line;
                break;
            default:
                qDebug().noquote() << line;
                break;
            }
        }
    }
}

QString Logger::currentLogFile() const
{
    QMutexLocker locker(&m_mutex);
    if (m_file)
        return m_file->fileName();
    return AppPaths::logFile();
}

void Logger::clearLogFile()
{
    QMutexLocker locker(&m_mutex);

    if (m_file && m_file->isOpen()) {
        m_stream.flush();
        m_file->close();
    }

    m_file.reset();
    m_openAttempted = false;

    const QString path = AppPaths::logFile();
    QFile::remove(path);

    // 不在这里立刻重开：下一条日志的 ensureOpen() 会重建，
    // 这样清空操作本身不会又写进去一行内容。
}

} // namespace media
