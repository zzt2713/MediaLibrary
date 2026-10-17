#ifndef LOGGER_H
#define LOGGER_H

#include <QString>
#include <QMutex>
#include <QTextStream>
#include <QFile>
#include <QFileInfo>
#include <memory>

namespace media {

/*!
 * \brief 统一日志记录器。
 *
 * 任务书要求"统一日志记录"，同时要求"不要把所有逻辑写在 MainWindow"。
 * 这里做成独立模块，任何层都能用，且不依赖 UI。
 *
 * 设计取舍：
 *  - 写文件 + 输出到 qDebug() 双通道。文件用于事后排查；
 *    qDebug 走 Qt 的默认消息处理器，在 Qt Creator 里能直接看到。
 *  - 线程安全：内部有 QMutex，工作线程可以直接记日志。
 *    这一点很重要——扫描与缩略图任务都在工作线程里，如果日志不是线程安全的，
 *    会出现交错写入的乱码行，甚至数据竞争。
 *  - 单文件、按天切分（见 AppPaths::logFile）。不引入轮转库。
 *  - 行缓冲刷新：每条日志写完就 flush。性能损失可接受（日志量不大），
 *    换来的是程序崩溃时最后几条日志不丢——崩溃场景下这几条恰恰最关键。
 */
class Logger
{
public:
    enum Level {
        Debug = 0,
        Info,
        Warning,
        Error
    };

    /*! 全局单例。首次调用时惰性初始化，避免静态初始化顺序问题。 */
    static Logger &instance();

    ~Logger();

    Logger(const Logger &) = delete;
    Logger &operator=(const Logger &) = delete;

    /*!
     * \brief 设置最低记录级别。低于此级别的日志被丢弃。
     *
     * 默认为 Debug。Release 构建下可以在 main() 里调成 Info，
     * 减少文件体积。
     */
    void setMinimumLevel(Level level);
    Level minimumLevel() const;

    /*! 是否同时输出到 Qt 的调试通道（qDebug）。 */
    void setEchoToQt(bool echo) { m_echoToQt = echo; }

    /*! 写一条日志。线程安全。 */
    void log(Level level, const QString &message,
             const QString &category = QString());

    // 便捷方法，省掉每次写 Level 枚举
    void debug(const QString &message, const QString &category = QString()) {
        log(Debug, message, category);
    }
    void info(const QString &message, const QString &category = QString()) {
        log(Info, message, category);
    }
    void warn(const QString &message, const QString &category = QString()) {
        log(Warning, message, category);
    }
    void error(const QString &message, const QString &category = QString()) {
        log(Error, message, category);
    }

    /*! 当前日志文件的绝对路径，用于"打开日志目录"这类操作。 */
    QString currentLogFile() const;

    /*! 清空当前日志文件。 */
    void clearLogFile();

    /*! 级别名称，用于格式化输出。 */
    static QString levelName(Level level);

    /*! 把字符串转成级别。无法识别时返回 Debug。 */
    static Level levelFromString(const QString &name);

private:
    Logger();

    /*! 确保日志文件已打开，返回是否可用。 */
    bool ensureOpen();

    mutable QMutex m_mutex;
    std::unique_ptr<QFile> m_file;
    QTextStream m_stream;
    Level m_minimumLevel = Debug;
    bool m_echoToQt = true;
    bool m_openAttempted = false;
};

/*!
 * \brief RAII 风格的日志行构造器，支持流式写法。
 *
 * 用法：LOG_INFO("扫描") << "已处理" << count << "个文件";
 *
 * 为什么需要它：直接调 Logger::info(QString) 时，调用方往往要自己拼字符串
 * （QString::arg 或 +），既冗长又容易漏掉类型转换。
 * 流式写法更接近 qDebug 的习惯。
 */
class LogLine
{
public:
    LogLine(Logger::Level level, const QString &category = QString())
        : m_level(level)
        , m_category(category)
    {
    }

    ~LogLine()
    {
        // 析构时一次性写出去，保证整行日志原子写入（不会和其他线程交错）
        Logger::instance().log(m_level, m_buffer, m_category);
    }

    LogLine &operator<<(const QString &value) { m_buffer += value; return *this; }
    LogLine &operator<<(const char *value) {
        m_buffer += QString::fromUtf8(value); return *this;
    }
    LogLine &operator<<(int value) { m_buffer += QString::number(value); return *this; }
    LogLine &operator<<(qint64 value) { m_buffer += QString::number(value); return *this; }
    LogLine &operator<<(double value) { m_buffer += QString::number(value); return *this; }
    LogLine &operator<<(bool value) { m_buffer += (value ? QStringLiteral("true") : QStringLiteral("false")); return *this; }

private:
    Logger::Level m_level;
    QString m_category;
    QString m_buffer;
};

} // namespace media

/*!
 * 便捷宏。
 *
 * 用宏而不是函数，是为了让日志自带"调用位置所在函数名"这个上下文——
 * 排查问题时"哪个函数打的日志"往往比"哪个文件"更有用。
 *
 * 用法：LOG_INFO("Scan") << "processed " << count << " files";
 * 分类（category）是自由字符串，用来在日志里区分模块，
 * 建议用类名或短模块名，如 "Scan"、"Thumbnail"、"Database"。
 */
#define LOG_DEBUG(cat)   media::LogLine(media::Logger::Debug,   QStringLiteral(cat)) << Q_FUNC_INFO << " | "
#define LOG_INFO(cat)    media::LogLine(media::Logger::Info,    QStringLiteral(cat)) << Q_FUNC_INFO << " | "
#define LOG_WARN(cat)    media::LogLine(media::Logger::Warning, QStringLiteral(cat)) << Q_FUNC_INFO << " | "
#define LOG_ERROR(cat)   media::LogLine(media::Logger::Error,   QStringLiteral(cat)) << Q_FUNC_INFO << " | "

#endif // LOGGER_H
