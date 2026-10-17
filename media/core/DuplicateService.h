#ifndef DUPLICATESERVICE_H
#define DUPLICATESERVICE_H

#include "MediaTypes.h"

#include <QList>
#include <functional>

namespace media {

class MediaDatabase;

/*!
 * \brief 重复资源检测的业务实现，不依赖任何 UI。
 *
 * 这段逻辑原先长在 MainWindow::runDuplicateDetection() 里，与进度对话框、
 * 提示弹窗、分组切换混在一起，接近 170 行。结果是"怎么算哈希、怎么分组"
 * 这类与界面无关的规则，只有打开主窗口的源码才能读到。
 *
 * 现在这里只负责算法与数据：
 *  - 决定哪些记录需要补算哈希（增量：已算过的跳过）
 *  - 计算 MD5 / 感知哈希并写回数据库
 *  - 按哈希分组，返回分组结果
 * 进度与取消通过回调交给调用方，因此不绑死在 QProgressDialog 上。
 */
class DuplicateService
{
public:
    /*!
     * \brief 进度回调。
     * \param done  已完成数
     * \param total 总数
     * \return false 表示请求中止（用户点了取消）
     *
     * 用回调而不是信号：这个类不继承 QObject，可以在工作线程里直接构造使用。
     * 等重复检测也改成后台任务时（阶段五），这一段不需要再改。
     */
    using ProgressCallback = std::function<bool(int done, int total)>;

    /*!
     * \brief 感知相似检测时参与比对的图片数上限。
     *
     * 两两比对是 O(n²)，n 必须封顶。超出时只分析前 n 张，
     * 并在结果里用 truncated 标记出来，由界面如实告知用户被截断。
     */
    static constexpr int kMaxVisualScanImages = 2000;

    explicit DuplicateService(MediaDatabase *database);

    /*! 检测结果。界面据此组织提示文案。 */
    struct Result
    {
        int  scanned = 0;           //!< 本次实际计算了哈希的条数
        int  groupCount = 0;        //!< 发现的重复组数
        int  itemCount = 0;         //!< 涉及到的条目总数
        int  threshold = 0;         //!< 感知相似使用的汉明距离阈值
        bool cancelled = false;     //!< 是否被用户中途取消
        bool truncated = false;     //!< 是否因规模上限只分析了一部分
        int  truncatedLimit = 0;    //!< 被截断时的上限值

        /*! 分组内容（每组至少两条）。无重复时为空。 */
        QList<QList<MediaItem>> groups;
    };

    /*!
     * \brief 精确重复检测：按文件内容 MD5 判定。
     *
     * 增量执行：只给 content_hash 尚为空的记录补算，所以第二次运行几乎瞬时完成。
     */
    Result findExactDuplicates(const ProgressCallback &onProgress = nullptr);

    /*!
     * \brief 视觉相似检测：按图片的感知哈希（dHash）判定。
     *
     * 只处理图片。两张图哈希的汉明距离不超过 HashUtil::kSimilarThreshold 即归为同组。
     *
     * 限制：当前是同步实现，图片很多时会阻塞调用线程；界面侧因此用了
     * 带进度的模态对话框。改成后台任务是阶段五的事。
     */
    Result findSimilarImages(const ProgressCallback &onProgress = nullptr);

private:
    MediaDatabase *m_database = nullptr;
};

} // namespace media

#endif // DUPLICATESERVICE_H
