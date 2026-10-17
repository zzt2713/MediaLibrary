#ifndef MEDIASCANNER_H
#define MEDIASCANNER_H

#include "MediaTypes.h"

#include <QObject>
#include <QStringList>
#include <QAtomicInt>
#include <QList>
// buildItem() 的签名里用到 QFileInfo，必须完整包含。
// 之前只靠 MediaScanner.cpp 里的传递包含，头文件单独编译就会报
// C4430/C2143（MediaItem buildItem(const QFileInfo &) 无法解析）。
#include <QFileInfo>
#include <functional>

namespace media {

/*!
 * \brief 目录扫描器。纯逻辑，不碰 UI，可在任意线程构造与调用。
 *
 * 扫描分两遍，这是刻意的设计：
 *  1. 预统计（可选）：先数一遍待扫文件数，用于给进度条一个真实的分母。
 *     对大目录要额外走一次遍历，所以设为可选；小目录开，超大目录关。
 *  2. 主扫描：边遍历边产出结果，通过回调交给调用方。
 *
 * 之所以用回调而不是信号：Scanner 会被 QRunnable 在工作线程里同步调用，
 * 回调能直接在工作线程内处理（如批量写库），省掉一次跨线程排队。
 * 进度提示才通过信号发给 UI。
 */
class MediaScanner : public QObject
{
    Q_OBJECT

public:
    explicit MediaScanner(QObject *parent = nullptr);

    struct Options
    {
        bool recursive = true;          // 是否递归子目录
        bool followSymlinks = false;    // 默认不跟随，防符号链接成环导致无限遍历
        bool countFirst = true;         // 是否先预统计总数（进度条用）
        bool readExif = true;           // 是否读图片 EXIF（略慢，但信息完整）
        int  maxDepth = 32;             // 深度上限，防御性限制
        /*! 跳过的目录名（不区分大小写）。这些目录里的东西通常不该进资源库。 */
        QStringList skipDirNames = {
            QStringLiteral("$RECYCLE.BIN"),
            QStringLiteral("System Volume Information"),
            QStringLiteral(".git"),
            QStringLiteral(".svn"),
            QStringLiteral("node_modules"),
            QStringLiteral("__pycache__")
        };
    };

    void setOptions(const Options &options) { m_options = options; }
    Options options() const { return m_options; }

    /*!
     * \brief 请求取消。
     *
     * 用 QAtomicInt 而非 bool：扫描线程读、UI 线程写，
     * 必须保证可见性，否则可能出现"点了取消但循环读到的还是旧值"。
     */
    void cancel() { m_cancelled.storeRelaxed(1); }
    bool isCancelled() const { return m_cancelled.loadRelaxed() != 0; }
    void resetCancel() { m_cancelled.storeRelaxed(0); }

    /*!
     * \brief 扫描单个目录。
     *
     * \param rootPath 根目录
     * \param onBatch  每攒够一批（默认 64 条）回调一次，调用方在此写库。
     *                 传空则只扫描不产出。在工作线程内同步执行。
     * \return 本次扫描发现的媒体文件总数
     */
    int scanDirectory(const QString &rootPath,
                      const std::function<void(const QList<MediaItem> &)> &onBatch);

    /*!
     * \brief 枚举目录的直接子目录，用于构建左侧目录树。
     *
     * 与 scanDirectory 分开：建树只关心目录结构，不需要读每个文件的元数据。
     */
    static QList<QString> listSubDirectories(const QString &rootPath,
                                             const QStringList &skipNames = QStringList());

    /*! 判断某个目录是否可读。扫描前先探一下，好给出"无权限"的明确提示。 */
    static bool isReadable(const QString &path);

signals:
    /*! 进度上报，跨线程发到 UI。 */
    void progress(const media::ScanProgress &progress);
    /*! 遇到问题（无权限、读失败）时上报，不中断扫描。 */
    void warning(const QString &path, const QString &message);

private:
    /*! 填充单个文件的元数据。会做 EXIF 读取，是扫描里最耗时的一步。 */
    MediaItem buildItem(const QFileInfo &info) const;

    Options m_options;
    QAtomicInt m_cancelled{ 0 };
};

} // namespace media

#endif // MEDIASCANNER_H
