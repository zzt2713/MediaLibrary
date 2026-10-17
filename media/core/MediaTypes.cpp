#include "MediaTypes.h"

#include <QCryptographicHash>
#include <QFileInfo>
#include <QStringList>

namespace media {

namespace {

/*!
 * 扩展名集合。故意写死而不走 QMimeDatabase：
 * MIME 查询对每个文件都要读文件头，十万级目录会明显拖慢扫描。
 */
const QStringList kImageExt = {
    "jpg", "jpeg", "png", "bmp", "gif", "webp", "tif", "tiff",
    "ico", "heic", "heif", "avif", "svg", "jfif"
};

const QStringList kVideoExt = {
    "mp4", "mkv", "avi", "mov", "wmv", "flv", "webm", "m4v", "mpg", "mpeg", "ts", "3gp"
};

const QStringList kAudioExt = {
    "mp3", "wav", "flac", "aac", "ogg", "m4a", "wma", "opus", "ape", "aiff", "mid"
};

} // namespace

QString ThumbKey::toCacheKey() const
{
    // 用 \n 分隔，避免路径中恰好含分隔符造成不同组合撞到同一摘要
    const QByteArray raw = (path + QLatin1Char('\n')
                            + QString::number(mtimeMs) + QLatin1Char('\n')
                            + QString::number(size) + QLatin1Char('\n')
                            + QString::number(edge)).toUtf8();
    return QString::fromLatin1(
        QCryptographicHash::hash(raw, QCryptographicHash::Md5).toHex());
}

MediaType typeFromExtension(const QString &suffix)
{
    const QString ext = suffix.toLower();
    if (ext.isEmpty())
        return MediaType::Unknown;

    if (kImageExt.contains(ext))
        return MediaType::Image;
    if (kVideoExt.contains(ext))
        return MediaType::Video;
    if (kAudioExt.contains(ext))
        return MediaType::Audio;
    return MediaType::Unknown;
}

MediaType typeFromPath(const QString &path)
{
    const int dot = path.lastIndexOf(QLatin1Char('.'));
    const int slash = path.lastIndexOf(QLatin1Char('/'));
    if (dot < 0 || dot < slash)
        return MediaType::Unknown;   // 无扩展名，或点在目录名里
    return typeFromExtension(path.mid(dot + 1));
}

QString mediaTypeName(MediaType type)
{
    switch (type) {
    case MediaType::Image: return QStringLiteral("图片");
    case MediaType::Video: return QStringLiteral("视频");
    case MediaType::Audio: return QStringLiteral("音频");
    default:               return QStringLiteral("未知");
    }
}

QString mediaTypePrefix(MediaType type)
{
    switch (type) {
    case MediaType::Image: return QStringLiteral("img");
    case MediaType::Video: return QStringLiteral("vid");
    case MediaType::Audio: return QStringLiteral("aud");
    default:               return QStringLiteral("unk");
    }
}

QString formatSize(qint64 bytes)
{
    if (bytes < 0)
        return QStringLiteral("--");

    constexpr double kKB = 1024.0;
    constexpr double kMB = kKB * 1024.0;
    constexpr double kGB = kMB * 1024.0;
    const double v = static_cast<double>(bytes);

    if (v < kKB)
        return QStringLiteral("%1 B").arg(bytes);
    if (v < kMB)
        return QStringLiteral("%1 KB").arg(v / kKB, 0, 'f', 1);
    if (v < kGB)
        return QStringLiteral("%1 MB").arg(v / kMB, 0, 'f', 1);
    return QStringLiteral("%1 GB").arg(v / kGB, 0, 'f', 2);
}

QString formatDuration(double ms)
{
    if (ms < 0)
        return QStringLiteral("--");

    const qint64 totalSec = static_cast<qint64>(ms / 1000.0);
    const qint64 h = totalSec / 3600;
    const qint64 m = (totalSec % 3600) / 60;
    const qint64 s = totalSec % 60;

    if (h > 0) {
        return QStringLiteral("%1:%2:%3")
            .arg(h)
            .arg(m, 2, 10, QLatin1Char('0'))
            .arg(s, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1:%2")
        .arg(m, 2, 10, QLatin1Char('0'))
        .arg(s, 2, 10, QLatin1Char('0'));
}

} // namespace media
