#ifndef MEDIATYPES_H
#define MEDIATYPES_H

#include <QString>
#include <QDateTime>
#include <QMetaType>

namespace media {

/*!
 * \brief 资源类型。Unknown 用于占位与异常兜底，不参与索引。
 */
enum class MediaType {
    Unknown = 0,
    Image   = 1,
    Video   = 2,
    Audio   = 3
};

/*!
 * \brief 一条资源记录的完整信息，对应 media_items 表一行。
 *
 * id 为 0 表示尚未写库（新扫描出的对象）。path 是唯一键，所有查询以此为准。
 */
struct MediaItem
{
    qint64      id = 0;
    QString     path;               // 绝对路径，正斜杠形式
    QString     fileName;           // 含扩展名的文件名
    QString     folderPath;         // 所属目录，与 folders.path 对应
    MediaType   type = MediaType::Unknown;
    qint64      size = 0;           // 字节
    QDateTime   modified;           // 文件修改时间，缓存失效与增量同步都依赖它
    qint64      mtimeMs = 0;        // modified 的毫秒时间戳，便于比较
    bool        favorite = false;
    bool        missing = false;    // 文件已被删除 / 移动，记录保留但标记失效
    QString     contentHash;        // MD5，精确重复检测
    QString     visualHash;         // dHash，感知重复检测
    int         width = 0;
    int         height = 0;
    double      durationMs = -1;    // 音视频时长，图片为 -1（未知）
    QString     cameraMake;         // EXIF
    QString     cameraModel;        // EXIF
    QDateTime   takenAt;            // EXIF 拍摄时间，缺失时回退 modified
    QStringList tags;

    bool isValid() const { return !path.isEmpty() && type != MediaType::Unknown; }
};

/*!
 * \brief 扫描进度反馈。total 为 0 时表示总数未知，UI 应显示忙碌态而非进度条。
 */
struct ScanProgress
{
    QString currentPath;    // 当前正在扫描的目录
    int     discovered = 0; // 已发现文件数
    int     total = 0;      // 预统计总数，-1 表示未知
    int     foldersDone = 0;
    int     foldersTotal = 0;
};

/*!
 * \brief 缩略图缓存的键。
 *
 * 同时包含 mtime 与 size，任一变化都会导致键变化，从而自然失效——
 * 这比"监听文件变化再主动清缓存"更可靠，也不会有漏删。
 */
struct ThumbKey
{
    QString path;
    qint64  mtimeMs = 0;
    qint64  size = 0;
    int     edge = 0;   // 缩略图边长（像素）

    bool operator==(const ThumbKey &other) const
    {
        return edge == other.edge && size == other.size
            && mtimeMs == other.mtimeMs && path == other.path;
    }

    /*! 序列化为磁盘缓存文件名（MD5 十六进制）。 */
    QString toCacheKey() const;
};

/*! 重复分组：同一哈希下的所有条目。 */
struct DuplicateGroup
{
    QString hash;
    bool    perceptual = false;     // true 表示 dHash 相似而非 MD5 相同
    QList<MediaItem> items;
};

/*!
 * \brief 目录树节点。仅用于构建 QTreeView 的层级，不持久化。
 */
struct FolderNode
{
    QString path;
    QString name;
    int     mediaCount = 0;
    QList<FolderNode> children;
};

/*! 扩展名 → 类型判定。按扩展名而非内容嗅探，速度快且对媒体库足够。 */
MediaType typeFromExtension(const QString &suffix);
MediaType typeFromPath(const QString &path);

/*! 类型的中文显示名，用于 UI 与筛选下拉框。 */
QString mediaTypeName(MediaType type);

/*! 类型在磁盘缓存中的短前缀，避免不同类别的同名缓存互相覆盖。 */
QString mediaTypePrefix(MediaType type);

QString formatSize(qint64 bytes);
QString formatDuration(double ms);

} // namespace media

Q_DECLARE_METATYPE(media::MediaItem)
Q_DECLARE_METATYPE(media::MediaType)
Q_DECLARE_METATYPE(QList<media::MediaItem>)

#endif // MEDIATYPES_H
