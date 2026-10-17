#ifndef HASHUTIL_H
#define HASHUTIL_H

#include <QString>
#include <QImage>

namespace media {

/*!
 * \brief 文件哈希工具：精确重复用 MD5，视觉相似用 dHash。
 */
class HashUtil
{
public:
    /*!
     * \brief 分块读取文件计算 MD5。
     *
     * 大视频动辄几个 GB，一次性 readAll 会直接把内存打爆，所以按 1MB 分块流式算。
     * \param path      文件路径
     * \param cancel   取消标志，由调用方持有的共享原子量；为 true 时提前返回空串
     * \return 32 位十六进制摘要；文件不可读时返回空串
     */
    static QString fileMd5(const QString &path, const volatile bool *cancel = nullptr);

    /*!
     * \brief 差分感知哈希（dHash），返回 64 位摘要。
     *
     * 流程：缩放到 9x8 灰度 → 逐行比较相邻像素亮度 → 得到 64 位。
     * 对缩放、轻微压缩、亮度调整不敏感，适合找"看起来一样"的图。
     * \return 不透明的 64 位值；图像不可读时返回 0
     */
    static quint64 perceptualHash(const QImage &image);
    static quint64 perceptualHash(const QString &path);

    /*!
     * \brief 两个感知哈希的汉明距离。
     *
     * 经验阈值：<= 5 基本可判为同一张图；<= 10 属于相似（裁切、滤镜）。
     */
    static int hammingDistance(quint64 a, quint64 b);

    /*! 默认判重阈值，UI 中可调整。 */
    static constexpr int kSimilarThreshold = 8;
};

} // namespace media

#endif // HASHUTIL_H
