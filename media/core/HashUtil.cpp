#include "HashUtil.h"

#include <QFile>
#include <QFileInfo>
// 计算 MD5 用。之前漏了这个包含，
// 因为 HashUtil.h 只声明返回 QString 而不暴露 QCryptographicHash，编译器不会提示。
#include <QCryptographicHash>
// addData(QByteArrayView) 需要这个类型
#include <QByteArrayView>

namespace media {

namespace {
constexpr qint64 kChunkSize = 1 << 20;   // 1MB
}

QString HashUtil::fileMd5(const QString &path, const volatile bool *cancel)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QString();   // 无权限 / 已被删除，交由调用方决定如何标记

    QCryptographicHash hash(QCryptographicHash::Md5);
    QByteArray buffer;
    buffer.resize(kChunkSize);

    while (!file.atEnd()) {
        if (cancel && *cancel)
            return QString();

        const qint64 read = file.read(buffer.data(), kChunkSize);
        if (read < 0)
            return QString();   // 读失败（网络盘掉线等）
        if (read > 0)
            hash.addData(QByteArrayView(buffer.constData(), read));
    }
    return QString::fromLatin1(hash.result().toHex());
}

quint64 HashUtil::perceptualHash(const QImage &image)
{
    if (image.isNull())
        return 0;

    // 9x8：每行取 9 个像素，得到 8 组相邻比较，共 64 位
    const QImage small = image
        .convertToFormat(QImage::Format_Grayscale8)
        .scaled(9, 8, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

    if (small.isNull())
        return 0;

    quint64 bits = 0;
    int bit = 0;
    for (int y = 0; y < 8; ++y) {
        const uchar *line = small.constScanLine(y);
        for (int x = 0; x < 8; ++x) {
            if (line[x] < line[x + 1])   // 右侧更亮则置位
                bits |= (quint64(1) << bit);
            ++bit;
        }
    }
    return bits;
}

quint64 HashUtil::perceptualHash(const QString &path)
{
    // 直接 QImage 构造会按需解码，比先读全部字节省内存
    QImage image(path);
    return perceptualHash(image);
}

int HashUtil::hammingDistance(quint64 a, quint64 b)
{
    quint64 diff = a ^ b;
    int count = 0;
    while (diff) {
        diff &= (diff - 1);   // 抹掉最低位的 1
        ++count;
    }
    return count;
}

} // namespace media
