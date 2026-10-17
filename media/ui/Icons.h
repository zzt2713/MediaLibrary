#ifndef ICONS_H
#define ICONS_H

#include <QIcon>
#include <QColor>
#include <QPixmap>
#include <QString>

namespace media {

/*!
 * \brief 矢量图标工厂。
 *
 * 为什么不用 png 资源：
 *  - 每换一个尺寸就要多一份图，还要为高 DPI 准备 @2x；
 *  - 深色主题下图标颜色写死在图里，无法跟随主题或选中态变化；
 *  - 引入图片资源意味着 qrc、路径、打包等一系列额外出错点。
 *
 * 用 QPainter 直接画，图标在任何 DPI 下都锐利，颜色可参数化。
 * 代价是每画一次要跑一遍绘制代码——所以图标按「名称+颜色+尺寸」缓存，
 * 实际只在首次请求时绘制一次。
 */
class Icons
{
public:
    /*!
     * \brief 取一个图标。
     * \param name  图标名，见下面的常量
     * \param color 描边色；默认取主题主文字色
     * \param size  像素尺寸；默认 18
     */
    static QIcon get(const QString &name, const QColor &color = QColor(), int size = 18);

    /*! 取图标对应的 QPixmap（需要直接画的时候用）。 */
    static QPixmap pixmap(const QString &name, const QColor &color = QColor(), int size = 18);

    /*! 清空缓存。主题切换后调用。 */
    static void clearCache();

    // ---------- 图标名常量 ----------
    // 侧边栏
    static const QString Folder;        // 文件夹
    static const QString Image;         // 图片
    static const QString Video;         // 视频
    static const QString Audio;         // 音频
    static const QString Star;          // 收藏
    static const QString StarFilled;    // 已收藏
    static const QString Clock;         // 最近播放
    static const QString Duplicate;     // 重复项
    static const QString Trash;         // 失效 / 清理

    // 工具栏
    static const QString Search;
    static const QString GridView;
    static const QString ListView;
    static const QString Refresh;
    static const QString Settings;
    static const QString Add;           // 添加目录
    static const QString Close;         // 关闭标签
    static const QString ChevronLeft;
    static const QString ChevronRight;
    static const QString More;          // 更多
    static const QString Play;
    static const QString Pause;
    static const QString Stop;
    static const QString VolumeUp;
    static const QString VolumeMute;
    static const QString Info;          // 元数据
    static const QString ZoomIn;
    static const QString ZoomOut;
    static const QString FitScreen;
    static const QString Rotate;
    static const QString Clear;         // 清空
};

} // namespace media

#endif // ICONS_H
