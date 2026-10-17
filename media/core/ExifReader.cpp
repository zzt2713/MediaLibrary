#include "ExifReader.h"

#include <QImageReader>
#include <QFileInfo>
#include <QRegularExpression>
#include <QHash>

namespace media {

namespace {

/*!
 * 把 EXIF 的 "2023:11:29 09:03:12" 解析成 QDateTime。
 * 分隔符可能是 ':' 也可能是 '-'，且部分相机带时区后缀，这里统一清洗。
 */
QDateTime parseExifDateTime(const QString &raw)
{
    if (raw.isEmpty())
        return QDateTime();

    QString s = raw.trimmed();
    const int spaceIdx = s.indexOf(QLatin1Char(' '));
    QString datePart = (spaceIdx > 0) ? s.left(spaceIdx) : s;
    QString timePart = (spaceIdx > 0) ? s.mid(spaceIdx + 1).trimmed() : QString();

    datePart.replace(QLatin1Char(':'), QLatin1Char('-'));
    // 去掉 "+08:00" 这类尾巴，Qt 的格式解析不认
    timePart.remove(QRegularExpression(QStringLiteral("[+-]\\d{2}:?\\d{2}$")));
    timePart = timePart.trimmed();

    const QString combined = datePart + QLatin1Char(' ') + timePart;
    QDateTime dt = QDateTime::fromString(combined, QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    if (!dt.isValid())
        dt = QDateTime::fromString(datePart, QStringLiteral("yyyy-MM-dd"));
    return dt;
}

/*!
 * EXIF 的 FNumber 常写成 "28/10"，转成 2.8。
 * 也兼容直接是 "2.8" 的情况。
 */
double parseRational(const QString &raw)
{
    if (raw.isEmpty())
        return 0.0;

    const QString s = raw.trimmed();
    const int slash = s.indexOf(QLatin1Char('/'));
    if (slash > 0) {
        bool okNum = false, okDen = false;
        const double num = s.left(slash).toDouble(&okNum);
        const double den = s.mid(slash + 1).toDouble(&okDen);
        if (okNum && okDen && den != 0.0)
            return num / den;
        return 0.0;
    }

    bool ok = false;
    const double v = s.toDouble(&ok);
    return ok ? v : 0.0;
}

int parseIso(const QString &raw)
{
    bool ok = false;
    const int v = raw.trimmed().toInt(&ok);
    return ok ? v : 0;
}

} // namespace

ImageMetadata ExifReader::read(const QString &path)
{
    ImageMetadata meta;

    QImageReader reader(path);
    reader.setAutoTransform(false);   // 我们要的是原始尺寸，不应用旋转

    // size() 对 JPEG/PNG 只读文件头就已足够，不会解码整张图
    const QSize size = reader.size();
    if (size.isValid()) {
        meta.width = size.width();
        meta.height = size.height();
    }

    // 有些格式（如部分 TIFF）size() 拿不到，退化为完整读取
    if (!size.isValid()) {
        const QImage img = reader.read();
        if (!img.isNull()) {
            meta.width = img.width();
            meta.height = img.height();
        }
    }

    if (meta.width <= 0 || meta.height <= 0)
        return meta;   // 连尺寸都读不出，说明文件损坏或无权限

    /*!
     * Qt 6 的 QImageReader 没有返回整张元数据表的无参 text()：
     * 只有 QStringList textKeys() 与 QString text(const QString &key)。
     * （实测签名见 Qt 6.8 的 qimagereader.h:58-59）
     *
     * 所以这里主动遍历 textKeys() 把表拼出来，一次取全比逐个 key 调用更省 ——
     * 每次 text(key) 都要过一遍插件的元数据查询路径。
     */
    const QStringList textKeys = reader.textKeys();
    if (textKeys.isEmpty())
        return meta;   // 没有元数据块，但尺寸有效，仍是可用结果

    QHash<QString, QString> text;
    text.reserve(textKeys.size());
    for (const QString &key : textKeys) {
        const QString value = reader.text(key);
        if (!value.isEmpty())
            text.insert(key, value);
    }

    if (text.isEmpty())
        return meta;

    meta.hasExif = true;

    auto pick = [&text](std::initializer_list<const char *> keys) -> QString {
        for (const char *k : keys) {
            auto it = text.constFind(QString::fromLatin1(k));
            if (it != text.constEnd() && !it.value().trimmed().isEmpty())
                return it.value().trimmed();
        }
        return QString();
    };

    const QString rawDate = pick({ "DateTimeOriginal", "DateTimeDigitized", "DateTime" });
    meta.takenAt = parseExifDateTime(rawDate);

    meta.cameraMake  = pick({ "Make" });
    meta.cameraModel = pick({ "Model" });
    meta.lens        = pick({ "LensModel", "Lens" });
    meta.orientation = pick({ "Orientation" });

    meta.aperture    = parseRational(pick({ "FNumber", "ApertureValue" }));
    meta.exposure    = parseRational(pick({ "ExposureTime" }));
    meta.focalLength = parseRational(pick({ "FocalLength" }));
    meta.iso         = parseIso(pick({ "ISOSpeedRatings", "PhotographicSensitivity" }));

    // 移除 "Make" 重复出现在 Model 前缀里的情况（"Canon Canon EOS R5"）
    if (!meta.cameraModel.isEmpty() && !meta.cameraMake.isEmpty()
        && meta.cameraModel.startsWith(meta.cameraMake, Qt::CaseInsensitive)) {
        meta.cameraModel = meta.cameraModel.mid(meta.cameraMake.size()).trimmed();
    }

    return meta;
}

bool ExifReader::imageSize(const QString &path, int *width, int *height)
{
    if (!width || !height)
        return false;

    QImageReader reader(path);
    const QSize size = reader.size();
    if (!size.isValid())
        return false;

    *width = size.width();
    *height = size.height();
    return true;
}

} // namespace media
