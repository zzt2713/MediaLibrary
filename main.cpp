#include "media/ui/MainWindow.h"
#include "media/ui/ThemeManager.h"

#include <QApplication>
#include <QSurfaceFormat>
#include <QFileInfo>
#include <QDir>
#include <QStandardPaths>
#include <QDebug>

/*!
 * \brief 应用入口。
 *
 * 启动流程刻意保持简单：
 *  1. 设 OpenGL 表面格式（QVideoWidget 需要，必须在 QApplication 之前设置）
 *  2. 创建 QApplication 并填好应用元信息
 *  3. 应用深色主题
 *  4. 显示主窗口
 *
 * 没有登录、引导向导、启动页——这些在旧版里存在，本版本按需求移除，
 * 让程序打开即用。
 */
int main(int argc, char *argv[])
{
    /*!
     * QVideoWidget 在 Windows 上默认走 ANGLE/D3D 后端。
     * 显式请求一个带深度缓冲的兼容配置，能避免部分机器上视频画面
     * 全黑或花屏。必须放在 QApplication 之前调用，
     * 因为表面格式在创建第一个窗口时就固定了。
     */
    QSurfaceFormat format;
    format.setDepthBufferSize(24);
    format.setStencilBufferSize(8);
    format.setSwapBehavior(QSurfaceFormat::DoubleBuffer);
    format.setSwapInterval(1);   // 垂直同步，减少撕裂
    QSurfaceFormat::setDefaultFormat(format);

    QApplication app(argc, argv);

    // ---------- 应用元信息 ----------
    // 这些值影响 QStandardPaths 的路径（数据库与缩略图缓存就落在这里），
    // 必须在第一次使用 QStandardPaths 之前设置好，否则会落到默认目录。
    app.setApplicationName(QStringLiteral("MediaLibrary"));
    app.setApplicationDisplayName(QStringLiteral("媒体资源库"));
    app.setOrganizationName(QStringLiteral("MediaLibrary"));
    app.setApplicationVersion(QStringLiteral("1.0.0"));

    // 高 DPI 缩放：Qt 6 默认开启。这里只设舍入策略，
    // 让 125%/150% 这类非整数缩放下界面不会出现半像素模糊。
    QApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

    // ---------- 主题 ----------
    // 必须在主窗口构造前应用：MainWindow 构造时就会用到 ThemeManager
    // 的颜色常量来画图标。
    media::ThemeManager theme;
    theme.apply(&app);

    // ---------- 主窗口 ----------
    media::MainWindow window;
    window.show();

    return app.exec();
}
