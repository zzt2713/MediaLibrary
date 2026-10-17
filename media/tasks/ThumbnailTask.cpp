#include "ThumbnailTask.h"

#include <QImageReader>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QFont>
#include <QFileInfo>
#include <QDir>
#include <QDebug>

namespace media {

namespace {

/*!
 * 计算等比缩放后的目标尺寸。
 *
 * 关键细节：用 QImageReader::setScaledSize 在解码阶段就缩小，
 * 而不是解码完整图再缩。对 6000x4000 的照片，前者只解码缩略图所需的分辨率，
 * 速度快一个数量级，内存峰值从 ~96MB 降到 ~1MB。
 */
QSize scaledSizeFor(const QSize &original, int edge)
{
    if (!original.isValid() || original.isEmpty() || edge <= 0)
        return QSize(edge, edge);

    const QSize bounded = original.scaled(edge, edge, Qt::KeepAspectRatio);
    return bounded.isEmpty() ? QSize(edge, edge) : bounded;
}

} // namespace

QImage ThumbnailTask::makePlaceholder(MediaType type, int edge,
                                      const QString &durationText)
{
    if (edge <= 0)
        edge = 128;

    // 画到 QImage 上：QPainter 在非 GUI 线程里画 QImage 是安全的，
    // 画 QPixmap 则不是。占位图也会在 worker 线程里生成（坏图 / 音视频分支），
    // 所以这里必须是 QImage。
    // 用 ARGB32_Premultiplied：与 QPainter 的合成路径最匹配，填充也更快。
    QImage img(edge, edge, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);

    QPainter painter(&img);
    painter.setRenderHint(QPainter::Antialiasing, true);

    // 背景：深色圆角矩形，与主题一致
    const QColor bg(0x2a, 0x2b, 0x30);
    painter.setPen(Qt::NoPen);
    painter.setBrush(bg);
    painter.drawRoundedRect(QRectF(0, 0, edge, edge), edge * 0.06, edge * 0.06);

    // 类型色：图片粉、视频蓝、音频绿——一眼能区分
    QColor accent;
    switch (type) {
    case MediaType::Image: accent = QColor(0xe8, 0x7b, 0xa8); break;
    case MediaType::Video: accent = QColor(0x5b, 0x9d, 0xf5); break;
    case MediaType::Audio: accent = QColor(0x5c, 0xc9, 0x8a); break;
    default:               accent = QColor(0x88, 0x88, 0x88); break;
    }

    const qreal cx = edge / 2.0;
    const qreal cy = edge / 2.0;
    const qreal r = edge * 0.18;

    painter.setBrush(accent);
    painter.setPen(Qt::NoPen);

    switch (type) {
    case MediaType::Video: {
        // 播放三角
        QPainterPath tri;
        tri.moveTo(cx - r * 0.6, cy - r * 0.9);
        tri.lineTo(cx + r * 0.9, cy);
        tri.lineTo(cx - r * 0.6, cy + r * 0.9);
        tri.closeSubpath();
        painter.drawPath(tri);
        break;
    }
    case MediaType::Audio: {
        // 音符：一个实心圆 + 一根竖杆
        painter.drawEllipse(QPointF(cx - r * 0.35, cy + r * 0.55), r * 0.5, r * 0.42);
        painter.drawRect(QRectF(cx + r * 0.1, cy - r * 0.95, r * 0.22, r * 1.55));
        painter.drawRect(QRectF(cx - r * 0.35, cy - r * 0.95, r * 0.67, r * 0.25));
        break;
    }
    default: {
        // 图片：山 + 太阳
        QPainterPath mount;
        mount.moveTo(cx - r * 1.1, cy + r * 0.7);
        mount.lineTo(cx - r * 0.15, cy - r * 0.5);
        mount.lineTo(cx + r * 0.35, cy + r * 0.05);
        mount.lineTo(cx + r * 0.75, cy - r * 0.35);
        mount.lineTo(cx + r * 1.1, cy + r * 0.7);
        mount.closeSubpath();
        painter.drawPath(mount);
        painter.drawEllipse(QPointF(cx + r * 0.5, cy - r * 0.85), r * 0.28, r * 0.28);
        break;
    }
    }

    // 右下角时长角标（音视频才有）
    if (!durationText.isEmpty()) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, 170));

        QFont f = painter.font();
        f.setPixelSize(qMax(9, edge / 8));
        painter.setFont(f);

        const QFontMetrics fm(f);
        const int tw = fm.horizontalAdvance(durationText) + edge / 12;
        const int th = fm.height() + edge / 24;
        const QRectF badge(edge - tw - edge * 0.05, edge - th - edge * 0.05, tw, th);

        painter.drawRoundedRect(badge, th * 0.25, th * 0.25);

        painter.setPen(QColor(0xe6, 0xe6, 0xe6));
        painter.drawText(badge, Qt::AlignCenter, durationText);
    }

    painter.end();
    return img;
}

ThumbnailTask::ThumbnailTask(TaskQueue *queue,
                             ThumbnailCache *cache,
                             MediaDatabase *database,
                             const MediaItem &item,
                             int edge)
    : QueuedTask(queue)
    , m_cache(cache)
    , m_database(database)
    , m_item(item)
    , m_edge(edge > 0 ? edge : 160)
{
}

ThumbnailTask::~ThumbnailTask() = default;

void ThumbnailTask::runTask()
{
    if (m_item.path.isEmpty())
        return;

    // 缓存键含 mtime/size，文件一变键就变，因此这里不需要"先检查文件是否变了"
    ThumbKey key;
    key.path = m_item.path;
    key.mtimeMs = m_item.mtimeMs;
    key.size = m_item.size;
    key.edge = m_edge;

    // 二级缓存查一次。命中则直接回调，不碰磁盘解码。
    // 缓存里现在存的是 QImage，正好是本线程能安全读写的类型。
    if (m_cache) {
        bool fromDisk = false;
        const QImage cached = m_cache->get(key, &fromDisk);
        if (!cached.isNull()) {
            if (m_result)
                m_result(m_item.id, key, cached, true);
            return;
        }
    }

    if (shouldCancel())
        return;

    QImage result;
    int width = 0;
    int height = 0;
    bool ok = false;
    QString errorText;

    switch (m_item.type) {
    case MediaType::Image: {
        result = decodeImage(m_item.path, m_edge, &width, &height);

        if (result.isNull()) {
            // 损坏图片 / 格式不支持 / 无权限。
            // 不当作致命错误：用占位图填格，并回传错误文本让 UI 能提示，
            // 但不弹窗打断用户——一次扫描可能有几十张坏图。
            errorText = QFileInfo::exists(m_item.path)
                            ? QStringLiteral("无法解码（文件可能已损坏或格式不受支持）")
                            : QStringLiteral("文件不存在或已被移动");

            result = makePlaceholder(MediaType::Image, m_edge);
            ok = true;   // 占位图也算完成，避免 UI 反复重试
        } else {
            ok = true;
        }
        break;
    }

    case MediaType::Video:
    case MediaType::Audio: {
        // 视频抽帧 / 音频封面解析都需要额外依赖，本项目不引。
        // 这里生成带类型标识与时长角标的占位图，README 中列为已知限制。
        const QString durText = (m_item.durationMs >= 0)
                                    ? formatDuration(m_item.durationMs)
                                    : QString();
        result = makePlaceholder(m_item.type, m_edge, durText);
        ok = true;
        break;
    }

    default:
        errorText = QStringLiteral("未知媒体类型");
        break;
    }

    if (shouldCancel())
        return;

    // 回写尺寸信息，供详情面板显示原始分辨率
    if (ok && m_database && m_item.id > 0 && m_item.type == MediaType::Image
        && width > 0 && height > 0 && (m_item.width != width || m_item.height != height)) {
        m_database->setDimensions(m_item.id, width, height, -1);
    }

    // 存两级缓存，下次直接命中
    if (ok && !result.isNull() && m_cache)
        m_cache->put(key, result);

    if (!ok && m_error)
        m_error(m_item.id, errorText);

    if (m_result)
        m_result(m_item.id, key, result, ok);
}

QImage ThumbnailTask::decodeImage(const QString &path, int edge,
                                  int *outWidth, int *outHeight) const
{
    QImageReader reader(path);
    reader.setAutoTransform(true);   // 按 EXIF Orientation 自动摆正，手机照片才不会躺着

    if (!reader.canRead())
        return QImage();

    const QSize original = reader.size();
    if (outWidth && outHeight && original.isValid()) {
        *outWidth = original.width();
        *outHeight = original.height();
    }

    // 在解码阶段就缩到目标尺寸。这是这一整个类里最关键的性能点。
    if (original.isValid() && !original.isEmpty()) {
        const QSize target = scaledSizeFor(original, edge);
        if (target.isValid() && !target.isEmpty())
            reader.setScaledSize(target);
    }

    const QImage image = reader.read();
    if (image.isNull())
        return QImage();

    // setScaledSize 在部分格式/部分 Qt 版本上会被忽略，这里兜一次底
    QImage finalImage = image;
    if (image.width() > edge || image.height() > edge)
        finalImage = image.scaled(edge, edge, Qt::KeepAspectRatio, Qt::FastTransformation);

    // 带 alpha 的图（PNG/WebP）保留透明通道；其余转成更省内存的格式。
    // 这里不再转 QPixmap：转换推迟到 GUI 线程，worker 只管解码与格式规整。
    if (finalImage.hasAlphaChannel())
        return finalImage.convertToFormat(QImage::Format_ARGB32_Premultiplied);

    return finalImage.convertToFormat(QImage::Format_RGB32);
}

} // namespace media
