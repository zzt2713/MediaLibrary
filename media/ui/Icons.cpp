#include "Icons.h"
#include "ThemeManager.h"

#include <QPainter>
#include <QPainterPath>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QtMath>

namespace media {

const QString Icons::Folder      = QStringLiteral("folder");
const QString Icons::Image       = QStringLiteral("image");
const QString Icons::Video       = QStringLiteral("video");
const QString Icons::Audio       = QStringLiteral("audio");
const QString Icons::Star        = QStringLiteral("star");
const QString Icons::StarFilled  = QStringLiteral("star_filled");
const QString Icons::Clock       = QStringLiteral("clock");
const QString Icons::Duplicate   = QStringLiteral("duplicate");
const QString Icons::Trash       = QStringLiteral("trash");

const QString Icons::Search      = QStringLiteral("search");
const QString Icons::GridView    = QStringLiteral("grid");
const QString Icons::ListView    = QStringLiteral("list");
const QString Icons::Refresh     = QStringLiteral("refresh");
const QString Icons::Settings    = QStringLiteral("settings");
const QString Icons::Add         = QStringLiteral("add");
const QString Icons::Close       = QStringLiteral("close");
const QString Icons::ChevronLeft = QStringLiteral("chevron_left");
const QString Icons::ChevronRight = QStringLiteral("chevron_right");
const QString Icons::More        = QStringLiteral("more");
const QString Icons::Play        = QStringLiteral("play");
const QString Icons::Pause       = QStringLiteral("pause");
const QString Icons::Stop        = QStringLiteral("stop");
const QString Icons::VolumeUp    = QStringLiteral("volume_up");
const QString Icons::VolumeMute  = QStringLiteral("volume_mute");
const QString Icons::Info        = QStringLiteral("info");
const QString Icons::ZoomIn      = QStringLiteral("zoom_in");
const QString Icons::ZoomOut     = QStringLiteral("zoom_out");
const QString Icons::FitScreen   = QStringLiteral("fit_screen");
const QString Icons::Rotate      = QStringLiteral("rotate");
const QString Icons::Clear       = QStringLiteral("clear");

namespace {

QMutex g_cacheMutex;
QHash<QString, QPixmap> g_cache;
QHash<QString, QIcon> g_iconCache;

QString cacheKey(const QString &name, const QColor &color, int size)
{
    return QStringLiteral("%1|%2|%3").arg(name, color.name(QColor::HexArgb)).arg(size);
}

/*!
 * 绘制上下文。
 *
 * 所有图标都在一个 24x24 的逻辑坐标系里绘制，再统一缩放到目标尺寸。
 * 好处是图标形状只需要按一套坐标写一次，改尺寸不用重调所有数字。
 */
struct DrawCtx
{
    QPainter *painter;
    qreal scale;

    QPointF pt(qreal x, qreal y) const { return QPointF(x * scale, y * scale); }
    qreal len(qreal v) const { return v * scale; }
};

// ------------------------------------------------------------ 各图标绘制

void drawFolder(const DrawCtx &c, const QColor &color)
{
    QPainterPath path;
    // 底部圆角矩形 + 左上角一个小凸起，构成经典文件夹轮廓
    path.moveTo(c.pt(3, 7));
    path.lineTo(c.pt(3, 19));
    path.lineTo(c.pt(21, 19));
    path.lineTo(c.pt(21, 8));
    path.lineTo(c.pt(11, 8));
    path.lineTo(c.pt(9, 5));
    path.lineTo(c.pt(3, 5));
    path.closeSubpath();

    c.painter->setBrush(Qt::NoBrush);
    c.painter->setPen(QPen(color, c.len(1.6), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    c.painter->drawPath(path);
}

void drawImage(const DrawCtx &c, const QColor &color)
{
    const QRectF frame = QRectF(c.pt(3, 4), c.pt(21, 20));
    c.painter->setBrush(Qt::NoBrush);
    c.painter->setPen(QPen(color, c.len(1.6), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    c.painter->drawRoundedRect(frame, c.len(2), c.len(2));

    // 山
    QPainterPath mount;
    mount.moveTo(c.pt(4.5, 18));
    mount.lineTo(c.pt(10, 12));
    mount.lineTo(c.pt(14, 16));
    mount.lineTo(c.pt(17.5, 12.5));
    mount.lineTo(c.pt(19.5, 18));
    c.painter->setPen(QPen(color, c.len(1.5), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    c.painter->drawPath(mount);

    // 太阳
    c.painter->setBrush(color);
    c.painter->setPen(Qt::NoPen);
    c.painter->drawEllipse(c.pt(16.5, 7.5), c.len(1.4), c.len(1.4));
}

void drawVideo(const DrawCtx &c, const QColor &color)
{
    const QRectF frame = QRectF(c.pt(2.5, 5.5), c.pt(21.5, 18.5));
    c.painter->setBrush(Qt::NoBrush);
    c.painter->setPen(QPen(color, c.len(1.6), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    c.painter->drawRoundedRect(frame, c.len(2.5), c.len(2.5));

    QPainterPath tri;
    tri.moveTo(c.pt(10.5, 9));
    tri.lineTo(c.pt(16.5, 12));
    tri.lineTo(c.pt(10.5, 15));
    tri.closeSubpath();
    c.painter->setBrush(color);
    c.painter->setPen(Qt::NoPen);
    c.painter->drawPath(tri);
}

void drawAudio(const DrawCtx &c, const QColor &color)
{
    c.painter->setBrush(Qt::NoBrush);
    c.painter->setPen(QPen(color, c.len(1.7), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));

    // 两根竖杆 + 一个底部弧，构成音符
    c.painter->drawLine(c.pt(9, 17), c.pt(9, 6));
    c.painter->drawLine(c.pt(18, 15), c.pt(18, 4));
    c.painter->drawLine(c.pt(9, 6), c.pt(18, 4));

    c.painter->setBrush(color);
    c.painter->setPen(Qt::NoPen);
    c.painter->drawEllipse(c.pt(6.5, 17.5), c.len(2.6), c.len(2.1));
    c.painter->drawEllipse(c.pt(15.5, 15.5), c.len(2.6), c.len(2.1));
}

/*! 五角星。filled 决定是实心还是描边。 */
void drawStar(const DrawCtx &c, const QColor &color, bool filled)
{
    QPainterPath star;
    const QPointF center = c.pt(12, 12.5);
    const qreal outer = c.len(9.0);
    const qreal inner = c.len(3.8);

    for (int i = 0; i < 10; ++i) {
        // 从正上方开始，交替取外顶点与内顶点
        const qreal radius = (i % 2 == 0) ? outer : inner;
        const qreal angle = -M_PI / 2.0 + i * M_PI / 5.0;
        const QPointF p(center.x() + radius * qCos(angle),
                        center.y() + radius * qSin(angle));
        if (i == 0)
            star.moveTo(p);
        else
            star.lineTo(p);
    }
    star.closeSubpath();

    if (filled) {
        c.painter->setBrush(color);
        c.painter->setPen(Qt::NoPen);
    } else {
        c.painter->setBrush(Qt::NoBrush);
        c.painter->setPen(QPen(color, c.len(1.5), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    }
    c.painter->drawPath(star);
}

void drawClock(const DrawCtx &c, const QColor &color)
{
    c.painter->setBrush(Qt::NoBrush);
    c.painter->setPen(QPen(color, c.len(1.6), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    c.painter->drawEllipse(c.pt(12, 12), c.len(8.5), c.len(8.5));

    c.painter->setPen(QPen(color, c.len(1.6), Qt::SolidLine, Qt::RoundCap));
    c.painter->drawLine(c.pt(12, 12), c.pt(12, 7));
    c.painter->drawLine(c.pt(12, 12), c.pt(16, 14));
}

void drawDuplicate(const DrawCtx &c, const QColor &color)
{
    c.painter->setBrush(Qt::NoBrush);
    c.painter->setPen(QPen(color, c.len(1.5), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));

    // 两张叠放的卡片，表示"重复"
    c.painter->drawRoundedRect(QRectF(c.pt(7, 4), c.pt(20, 16)), c.len(2), c.len(2));
    c.painter->drawRoundedRect(QRectF(c.pt(4, 8), c.pt(17, 20)), c.len(2), c.len(2));
}

void drawTrash(const DrawCtx &c, const QColor &color)
{
    c.painter->setBrush(Qt::NoBrush);
    c.painter->setPen(QPen(color, c.len(1.6), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));

    // 桶盖
    c.painter->drawLine(c.pt(4, 7), c.pt(20, 7));
    c.painter->drawLine(c.pt(9.5, 7), c.pt(9.5, 4.5));
    c.painter->drawLine(c.pt(9.5, 4.5), c.pt(14.5, 4.5));
    c.painter->drawLine(c.pt(14.5, 4.5), c.pt(14.5, 7));

    // 桶身
    QPainterPath body;
    body.moveTo(c.pt(6, 7));
    body.lineTo(c.pt(7, 20));
    body.lineTo(c.pt(17, 20));
    body.lineTo(c.pt(18, 7));
    c.painter->drawPath(body);

    c.painter->drawLine(c.pt(10.5, 10), c.pt(10.8, 17));
    c.painter->drawLine(c.pt(13.5, 10), c.pt(13.2, 17));
}

void drawSearch(const DrawCtx &c, const QColor &color)
{
    c.painter->setBrush(Qt::NoBrush);
    c.painter->setPen(QPen(color, c.len(1.8), Qt::SolidLine, Qt::RoundCap));
    c.painter->drawEllipse(c.pt(10.5, 10.5), c.len(6.0), c.len(6.0));
    c.painter->drawLine(c.pt(15, 15), c.pt(20, 20));
}

void drawGrid(const DrawCtx &c, const QColor &color)
{
    c.painter->setBrush(Qt::NoBrush);
    c.painter->setPen(QPen(color, c.len(1.5), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));

    const qreal s = 1.4;   // 圆角
    // 2x2 四个方块，代表网格布局
    const QRectF blocks[4] = {
        QRectF(c.pt(4, 4), c.pt(11, 11)),
        QRectF(c.pt(13, 4), c.pt(20, 11)),
        QRectF(c.pt(4, 13), c.pt(11, 20)),
        QRectF(c.pt(13, 13), c.pt(20, 20)),
    };
    for (const QRectF &r : blocks)
        c.painter->drawRoundedRect(r, c.len(s), c.len(s));
}

void drawList(const DrawCtx &c, const QColor &color)
{
    c.painter->setPen(QPen(color, c.len(1.6), Qt::SolidLine, Qt::RoundCap));
    c.painter->setBrush(Qt::NoBrush);

    // 三行「点 + 线」
    for (int i = 0; i < 3; ++i) {
        const qreal y = 6.5 + i * 5.5;
        c.painter->drawLine(c.pt(9, y), c.pt(20, y));
    }
    c.painter->setBrush(color);
    c.painter->setPen(Qt::NoPen);
    for (int i = 0; i < 3; ++i) {
        const qreal y = 6.5 + i * 5.5;
        c.painter->drawEllipse(c.pt(5, y), c.len(1.2), c.len(1.2));
    }
}

/*! 环形箭头，表示刷新。用 drawArc 画 3/4 圆再补一个箭头尖。 */
void drawRefresh(const DrawCtx &c, const QColor &color)
{
    c.painter->setBrush(Qt::NoBrush);
    c.painter->setPen(QPen(color, c.len(1.8), Qt::SolidLine, Qt::RoundCap));

    QRectF rect(c.pt(4, 4), c.pt(20, 20));
    // Qt 的角度单位是 1/16 度；从 -60 度开始顺时针画 300 度
    c.painter->drawArc(rect, -60 * 16, 300 * 16);

    // 箭头尖：在起点位置画一个小三角
    QPainterPath arrow;
    arrow.moveTo(c.pt(17.5, 3.5));
    arrow.lineTo(c.pt(17.5, 8.5));
    arrow.lineTo(c.pt(12.5, 6.0));
    arrow.closeSubpath();
    c.painter->setBrush(color);
    c.painter->setPen(Qt::NoPen);
    c.painter->drawPath(arrow);
}

void drawSettings(const DrawCtx &c, const QColor &color)
{
    c.painter->setBrush(Qt::NoBrush);
    c.painter->setPen(QPen(color, c.len(1.6), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));

    // 齿轮：一个圆 + 8 根齿。用线画齿比画复杂路径简单，视觉上也成立。
    c.painter->drawEllipse(c.pt(12, 12), c.len(3.2), c.len(3.2));
    c.painter->drawEllipse(c.pt(12, 12), c.len(7.5), c.len(7.5));

    for (int i = 0; i < 8; ++i) {
        const qreal angle = i * M_PI / 4.0;

        // 这里要的是「把单个坐标值按缩放系数换算」，所以用 len() 而不是 pt()。
        // pt(x, y) 是成对坐标的便捷构造，len(v) 才是单值缩放。
        const QPointF inner(c.len(12 + 7.5 * qCos(angle)),
                            c.len(12 + 7.5 * qSin(angle)));
        const QPointF outer(c.len(12 + 9.8 * qCos(angle)),
                            c.len(12 + 9.8 * qSin(angle)));
        c.painter->drawLine(inner, outer);
    }
}

void drawAdd(const DrawCtx &c, const QColor &color)
{
    c.painter->setPen(QPen(color, c.len(1.9), Qt::SolidLine, Qt::RoundCap));
    c.painter->drawLine(c.pt(12, 5), c.pt(12, 19));
    c.painter->drawLine(c.pt(5, 12), c.pt(19, 12));
}

void drawClose(const DrawCtx &c, const QColor &color)
{
    c.painter->setPen(QPen(color, c.len(1.8), Qt::SolidLine, Qt::RoundCap));
    c.painter->drawLine(c.pt(6, 6), c.pt(18, 18));
    c.painter->drawLine(c.pt(18, 6), c.pt(6, 18));
}

void drawChevron(const DrawCtx &c, const QColor &color, bool left)
{
    c.painter->setBrush(Qt::NoBrush);
    c.painter->setPen(QPen(color, c.len(1.9), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));

    QPainterPath p;
    if (left) {
        p.moveTo(c.pt(15, 5));
        p.lineTo(c.pt(9, 12));
        p.lineTo(c.pt(15, 19));
    } else {
        p.moveTo(c.pt(9, 5));
        p.lineTo(c.pt(15, 12));
        p.lineTo(c.pt(9, 19));
    }
    c.painter->drawPath(p);
}

void drawMore(const DrawCtx &c, const QColor &color)
{
    c.painter->setBrush(color);
    c.painter->setPen(Qt::NoPen);
    // 三个横排圆点
    c.painter->drawEllipse(c.pt(5.5, 12), c.len(1.6), c.len(1.6));
    c.painter->drawEllipse(c.pt(12, 12), c.len(1.6), c.len(1.6));
    c.painter->drawEllipse(c.pt(18.5, 12), c.len(1.6), c.len(1.6));
}

void drawPlay(const DrawCtx &c, const QColor &color)
{
    QPainterPath tri;
    tri.moveTo(c.pt(7, 4.5));
    tri.lineTo(c.pt(19.5, 12));
    tri.lineTo(c.pt(7, 19.5));
    tri.closeSubpath();
    c.painter->setBrush(color);
    c.painter->setPen(Qt::NoPen);
    c.painter->drawPath(tri);
}

void drawPause(const DrawCtx &c, const QColor &color)
{
    c.painter->setBrush(color);
    c.painter->setPen(Qt::NoPen);
    c.painter->drawRoundedRect(QRectF(c.pt(7, 5), c.pt(10.5, 19)), c.len(1), c.len(1));
    c.painter->drawRoundedRect(QRectF(c.pt(13.5, 5), c.pt(17, 19)), c.len(1), c.len(1));
}

void drawStop(const DrawCtx &c, const QColor &color)
{
    c.painter->setBrush(color);
    c.painter->setPen(Qt::NoPen);
    c.painter->drawRoundedRect(QRectF(c.pt(6, 6), c.pt(18, 18)), c.len(1.6), c.len(1.6));
}

void drawVolume(const DrawCtx &c, const QColor &color, bool muted)
{
    c.painter->setBrush(color);
    c.painter->setPen(Qt::NoPen);

    // 喇叭主体
    QPainterPath speaker;
    speaker.moveTo(c.pt(4, 9.5));
    speaker.lineTo(c.pt(7.5, 9.5));
    speaker.lineTo(c.pt(11.5, 5.5));
    speaker.lineTo(c.pt(11.5, 18.5));
    speaker.lineTo(c.pt(7.5, 14.5));
    speaker.lineTo(c.pt(4, 14.5));
    speaker.closeSubpath();
    c.painter->drawPath(speaker);

    if (muted) {
        // 画叉
        c.painter->setBrush(Qt::NoBrush);
        c.painter->setPen(QPen(color, c.len(1.7), Qt::SolidLine, Qt::RoundCap));
        c.painter->drawLine(c.pt(15, 9.5), c.pt(20, 14.5));
        c.painter->drawLine(c.pt(20, 9.5), c.pt(15, 14.5));
    } else {
        // 两道弧线
        c.painter->setBrush(Qt::NoBrush);
        c.painter->setPen(QPen(color, c.len(1.6), Qt::SolidLine, Qt::RoundCap));
        c.painter->drawArc(QRectF(c.pt(11, 8), c.pt(19, 16)), -60 * 16, 120 * 16);
        c.painter->drawArc(QRectF(c.pt(11, 5.5), c.pt(22, 18.5)), -60 * 16, 120 * 16);
    }
}

void drawInfo(const DrawCtx &c, const QColor &color)
{
    c.painter->setBrush(Qt::NoBrush);
    c.painter->setPen(QPen(color, c.len(1.6), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    c.painter->drawEllipse(c.pt(12, 12), c.len(8.5), c.len(8.5));

    c.painter->setPen(QPen(color, c.len(1.7), Qt::SolidLine, Qt::RoundCap));
    c.painter->drawLine(c.pt(12, 11), c.pt(12, 16.5));

    c.painter->setBrush(color);
    c.painter->setPen(Qt::NoPen);
    c.painter->drawEllipse(c.pt(12, 8), c.len(1.1), c.len(1.1));
}

void drawZoom(const DrawCtx &c, const QColor &color, bool in)
{
    c.painter->setBrush(Qt::NoBrush);
    c.painter->setPen(QPen(color, c.len(1.7), Qt::SolidLine, Qt::RoundCap));
    c.painter->drawEllipse(c.pt(10.5, 10.5), c.len(6.0), c.len(6.0));
    c.painter->drawLine(c.pt(15, 15), c.pt(20, 20));

    // 放大镜里的加号或减号
    c.painter->drawLine(c.pt(7.8, 10.5), c.pt(13.2, 10.5));
    if (in)
        c.painter->drawLine(c.pt(10.5, 7.8), c.pt(10.5, 13.2));
}

/*! 四个角的括线，表示「适应窗口」。 */
void drawFitScreen(const DrawCtx &c, const QColor &color)
{
    c.painter->setBrush(Qt::NoBrush);
    c.painter->setPen(QPen(color, c.len(1.8), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));

    const qreal o = 3.5;    // 外边界
    const qreal i = 9.0;    // 内拐点

    // 左上
    c.painter->drawLine(c.pt(o, i), c.pt(o, o));
    c.painter->drawLine(c.pt(o, o), c.pt(i, o));
    // 右上
    c.painter->drawLine(c.pt(24 - i, o), c.pt(24 - o, o));
    c.painter->drawLine(c.pt(24 - o, o), c.pt(24 - o, i));
    // 左下
    c.painter->drawLine(c.pt(o, 24 - i), c.pt(o, 24 - o));
    c.painter->drawLine(c.pt(o, 24 - o), c.pt(i, 24 - o));
    // 右下
    c.painter->drawLine(c.pt(24 - i, 24 - o), c.pt(24 - o, 24 - o));
    c.painter->drawLine(c.pt(24 - o, 24 - o), c.pt(24 - o, 24 - i));
}

void drawRotate(const DrawCtx &c, const QColor &color)
{
    c.painter->setBrush(Qt::NoBrush);
    c.painter->setPen(QPen(color, c.len(1.7), Qt::SolidLine, Qt::RoundCap));
    c.painter->drawArc(QRectF(c.pt(5, 5), c.pt(19, 19)), 40 * 16, 280 * 16);

    QPainterPath arrow;
    arrow.moveTo(c.pt(18, 4));
    arrow.lineTo(c.pt(18, 9.5));
    arrow.lineTo(c.pt(12.8, 7.2));
    arrow.closeSubpath();
    c.painter->setBrush(color);
    c.painter->setPen(Qt::NoPen);
    c.painter->drawPath(arrow);
}

void drawClear(const DrawCtx &c, const QColor &color)
{
    c.painter->setBrush(Qt::NoBrush);
    c.painter->setPen(QPen(color, c.len(1.7), Qt::SolidLine, Qt::RoundCap));
    c.painter->drawEllipse(c.pt(12, 12), c.len(8.5), c.len(8.5));
    c.painter->drawLine(c.pt(8.5, 8.5), c.pt(15.5, 15.5));
    c.painter->drawLine(c.pt(15.5, 8.5), c.pt(8.5, 15.5));
}

/*! 按名字分发到具体绘制函数。 */
void drawIcon(const QString &name, QPainter *painter, const QColor &color)
{
    DrawCtx c{ painter, 1.0 };

    if (name == Icons::Folder)           drawFolder(c, color);
    else if (name == Icons::Image)       drawImage(c, color);
    else if (name == Icons::Video)       drawVideo(c, color);
    else if (name == Icons::Audio)       drawAudio(c, color);
    else if (name == Icons::Star)        drawStar(c, color, false);
    else if (name == Icons::StarFilled)  drawStar(c, color, true);
    else if (name == Icons::Clock)       drawClock(c, color);
    else if (name == Icons::Duplicate)   drawDuplicate(c, color);
    else if (name == Icons::Trash)       drawTrash(c, color);
    else if (name == Icons::Search)      drawSearch(c, color);
    else if (name == Icons::GridView)    drawGrid(c, color);
    else if (name == Icons::ListView)    drawList(c, color);
    else if (name == Icons::Refresh)     drawRefresh(c, color);
    else if (name == Icons::Settings)    drawSettings(c, color);
    else if (name == Icons::Add)         drawAdd(c, color);
    else if (name == Icons::Close)       drawClose(c, color);
    else if (name == Icons::ChevronLeft) drawChevron(c, color, true);
    else if (name == Icons::ChevronRight) drawChevron(c, color, false);
    else if (name == Icons::More)        drawMore(c, color);
    else if (name == Icons::Play)        drawPlay(c, color);
    else if (name == Icons::Pause)       drawPause(c, color);
    else if (name == Icons::Stop)        drawStop(c, color);
    else if (name == Icons::VolumeUp)    drawVolume(c, color, false);
    else if (name == Icons::VolumeMute)  drawVolume(c, color, true);
    else if (name == Icons::Info)        drawInfo(c, color);
    else if (name == Icons::ZoomIn)      drawZoom(c, color, true);
    else if (name == Icons::ZoomOut)     drawZoom(c, color, false);
    else if (name == Icons::FitScreen)   drawFitScreen(c, color);
    else if (name == Icons::Rotate)      drawRotate(c, color);
    else if (name == Icons::Clear)       drawClear(c, color);
    // 未知名不画任何东西，返回空 pixmap，调用方会看到空白图标而不是崩溃
}

} // namespace

QPixmap Icons::pixmap(const QString &name, const QColor &color, int size)
{
    const QColor actualColor = color.isValid() ? color : ThemeManager::textPrimary();
    size = qBound(12, size, 256);

    const QString key = cacheKey(name, actualColor, size);

    {
        QMutexLocker locker(&g_cacheMutex);
        auto it = g_cache.constFind(key);
        if (it != g_cache.constEnd())
            return it.value();
    }

    // 按设备像素比绘制，保证在 4K/缩放屏上不模糊
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);

    QPainter painter(&pm);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    // 在 24x24 逻辑坐标里画，再整体缩放到目标尺寸
    painter.scale(size / 24.0, size / 24.0);
    drawIcon(name, &painter, actualColor);
    painter.end();

    {
        QMutexLocker locker(&g_cacheMutex);
        g_cache.insert(key, pm);
    }

    return pm;
}

QIcon Icons::get(const QString &name, const QColor &color, int size)
{
    const QColor actualColor = color.isValid() ? color : ThemeManager::textPrimary();
    const QString key = cacheKey(name, actualColor, size);

    {
        QMutexLocker locker(&g_cacheMutex);
        auto it = g_iconCache.constFind(key);
        if (it != g_iconCache.constEnd())
            return it.value();
    }

    QIcon icon;
    const QPixmap pm = pixmap(name, actualColor, size);
    if (!pm.isNull())
        icon.addPixmap(pm);

    // 禁用/选中态用不同透明度，避免额外绘制
    QPixmap disabled = pm;
    if (!disabled.isNull()) {
        QPainter p(&disabled);
        p.setCompositionMode(QPainter::CompositionMode_SourceAtop);
        p.fillRect(disabled.rect(), QColor(0, 0, 0, 130));
        p.end();
        icon.addPixmap(disabled, QIcon::Disabled);
    }

    {
        QMutexLocker locker(&g_cacheMutex);
        g_iconCache.insert(key, icon);
    }

    return icon;
}

void Icons::clearCache()
{
    QMutexLocker locker(&g_cacheMutex);
    g_cache.clear();
    g_iconCache.clear();
}

} // namespace media
