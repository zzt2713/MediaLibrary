#ifndef APPPATHS_H
#define APPPATHS_H

#include <QString>

namespace media {

/*!
 * \brief 应用路径的统一出口。
 *
 * 存在的理由（任务书明确要求）：
 * 把所有"文件该放哪"的知识收在一个地方，不让 QStandardPaths 的调用
 * 散落在各个模块里。散落的后果是——想改数据库位置得全局搜索，
 * 而且很容易有人在某个角落写出相对路径，把运行时数据落到程序安装目录。
 *
 * 关键原则：**运行时数据一律不进程序安装目录**。
 * 安装目录在 Windows 上通常是 Program Files，普通用户没有写权限；
 * 即使有权限，卸载时数据也会被一起删掉。所以全部走 QStandardPaths。
 *
 * 所有 getter 都返回已确保存在的目录（内部会自动 mkpath）。
 */
class AppPaths
{
public:
    /*! 应用数据根目录。所有子目录都挂在它下面。 */
    static QString dataRoot();

    /*! 数据库文件完整路径（含文件名）。 */
    static QString databaseFile();

    /*! 日志目录。 */
    static QString logDir();
    /*! 日志文件完整路径。按启动时间分文件，便于排查历史问题。 */
    static QString logFile();

    /*! 缩略图磁盘缓存目录。 */
    static QString thumbnailCacheDir();

    /*! 配置目录（QSettings IniFormat 文件所在处）。 */
    static QString configDir();
    /*! 配置文件完整路径。 */
    static QString configFile();

    /*!
     * \brief 确保某个目录存在。不存在则递归创建。
     * \return 创建成功或已存在时返回 true
     */
    static bool ensureDir(const QString &dir);

    /*!
     * \brief 校验一个目录路径是否可以安全写入。
     *
     * 用途：任务书要求"不要把数据库写入程序安装目录"。
     * 这个方法检查目标是否落在可能只读的位置（如 Program Files），
     * 并实际尝试创建来验证权限。
     */
    static bool isWritableLocation(const QString &dir);

private:
    /*! 禁止实例化：这是一个纯静态工具类。 */
    AppPaths() = delete;
};

} // namespace media

#endif // APPPATHS_H
