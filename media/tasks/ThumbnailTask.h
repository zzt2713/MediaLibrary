#ifndef THUMBNAILTASK_H
#define THUMBNAILTASK_H

#include "TaskQueue.h"
#include "../core/MediaTypes.h"
#include "../core/ThumbnailCache.h"
#include "../core/MediaDatabase.h"

#include <QPointer>
#include <QString>
#include <QImage>
#include <functional>

namespace media {

/*!
 * \brief 单张资源的缩略图生成任务。
 *
 * 生成策略按类型分派：
 *  - 图片：QImageReader 直接读并缩放。用 setScaledSize 让解码器在解码阶段就缩，
 *    比"整张解码再 QImage::scaled"快好几倍，内存峰值也低得多（大图尤其明显）。
 *  - 视频：不可行。视频抽帧需要额外解码库，本项目不引第三方依赖，
 *    因此视频缩略图统一走占位图（带时长角标），并在 UI 上如实说明这是已知限制。
 *  - 音频：同样走占位图（音符图标 + 时长）。音频封面需要解析 ID3 标签，
 *    同样超出「无额外依赖」的范围，README 里列为可扩展项。
 *
 * 之所以每张图一个任务而不是一个任务处理一整批：
 * 单张任务粒度高，队列的并发上限才能真正起到「限制同时解码数」的作用。
 * 若一个任务处理一百张，那并发上限就形同虚设了。
 *
 * 线程边界（最重要的一点）：本类全程只碰 QImage，绝不碰 QPixmap。
 * QPixmap 是绑定平台后端存储的绘图设备，Qt 明确要求只在 GUI 线程使用；
 * 在工作线程里构造或绘制 QPixmap 是未定义行为，Windows 上会直接崩。
 * QImage 则只是 CPU 侧的像素缓冲区，可以安全地跨线程传递。
 * 转成 QPixmap 的时机被推迟到 GUI 线程真正要绘制的那一刻（delegate / 预览面板）。
 */
class ThumbnailTask : public QueuedTask
{
public:
    ThumbnailTask(TaskQueue *queue,
                  ThumbnailCache *cache,
                  MediaDatabase *database,
                  const MediaItem &item,
                  int edge);

    ~ThumbnailTask() override;

    /*!
     * 结果回调。在 worker 线程内被调用。
     * \param itemId 资源 id
     * \param key    缓存键，调用方拿它回查缓存
     * \param image  生成的缩略图；失败时为空
     * \param ok     true 表示成功（含"用了占位图但仍算生成了结果"）
     */
    using ResultCallback = std::function<void(qint64, const ThumbKey &, const QImage &, bool)>;

    void setResultCallback(ResultCallback cb) { m_result = std::move(cb); }

    /*! 生成失败时通知（损坏文件、无权限）。UI 可据此显示明确的错误提示。 */
    using ErrorCallback = std::function<void(qint64, const QString &)>;
    void setErrorCallback(ErrorCallback cb) { m_error = std::move(cb); }

    /*!
     * \brief 生成占位图。
     *
     * 做成静态公开方法，UI 在等待真实缩略图时可以先画它，
     * 保证网格布局在加载过程中不跳动。
     *
     * 返回 QImage 而不是 QPixmap：这个方法既在工作线程里被 runTask 调用，
     * 也被 GUI 线程的 delegate 直接调用。让它保持在 QImage 这一侧，
     * 工作线程就完全不需要接触 QPixmap；GUI 侧自己转一次即可。
     */
    static QImage makePlaceholder(MediaType type, int edge, const QString &durationText = QString());

private:
    void runTask() override;

    /*!
     * \brief 按缩略图边长解码图片。
     * \param outWidth/outHeight 输出原始尺寸，供写库
     */
    QImage decodeImage(const QString &path, int edge, int *outWidth, int *outHeight) const;

    QPointer<ThumbnailCache> m_cache;
    QPointer<MediaDatabase>  m_database;
    MediaItem m_item;
    int m_edge;
    ResultCallback m_result;
    ErrorCallback  m_error;
};

} // namespace media

#endif // THUMBNAILTASK_H
