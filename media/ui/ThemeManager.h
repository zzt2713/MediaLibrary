#ifndef THEMEMANAGER_H
#define THEMEMANAGER_H

#include <QObject>
#include <QColor>
#include <QString>

class QApplication;

namespace media {

/*!
 * \brief 深色主题的集中定义。
 *
 * 所有颜色只在这里出现一次，UI 代码通过语义名引用（如 ThemeManager::accent()），
 * 不写死十六进制。这样改主题只需改这一个文件，也不会出现
 * "同一个蓝色在五个文件里有五个略不同的值"。
 *
 * QSS 文件（resources/style.qss）里保留了少量十六进制值——QSS 不支持变量。
 * 那些值必须与这里的常量保持一致，改动时两边同步。
 * 应用启动时会做一次字符串替换，把 QSS 里的占位符换成这里的实际值，
 * 从而消除"两处不一致"的风险。
 */
class ThemeManager : public QObject
{
    Q_OBJECT

public:
    explicit ThemeManager(QObject *parent = nullptr);

    /*!
     * \brief 应用深色主题到整个应用。
     *
     * 做三件事：
     *  1. 设置 QPalette（影响原生绘制的控件，如滚动条箭头、菜单）
     *  2. 加载并填充 QSS
     *  3. 设置应用字体
     */
    void apply(QApplication *app);

    // ---------- 语义色 ----------

    static QColor windowBackground()   { return QColor(0x1b, 0x1c, 0x1f); }  // 最外层
    static QColor panelBackground()    { return QColor(0x23, 0x24, 0x28); }  // 面板
    static QColor surfaceBackground()  { return QColor(0x2a, 0x2b, 0x30); }  // 卡片/输入框
    static QColor surfaceHover()       { return QColor(0x32, 0x34, 0x39); }
    static QColor surfacePressed()     { return QColor(0x1f, 0x20, 0x24); }

    static QColor borderColor()        { return QColor(0x3a, 0x3c, 0x42); }
    static QColor borderSubtle()       { return QColor(0x2e, 0x30, 0x35); }

    static QColor textPrimary()        { return QColor(0xe8, 0xea, 0xed); }
    static QColor textSecondary()      { return QColor(0x9a, 0x9d, 0xa4); }
    static QColor textDisabled()       { return QColor(0x5f, 0x62, 0x68); }

    static QColor accent()             { return QColor(0x4c, 0x8d, 0xf6); }  // 主强调色
    static QColor accentHover()        { return QColor(0x5d, 0x9a, 0xf8); }
    static QColor accentSubtle()       { return QColor(0x2b, 0x45, 0x74); }  // 选中底色

    static QColor favoriteColor()      { return QColor(0xf2, 0xb8, 0x4b); }  // 星标
    static QColor dangerColor()        { return QColor(0xe5, 0x6b, 0x6b); }
    static QColor successColor()       { return QColor(0x5c, 0xc9, 0x8a); }

    // 资源类型色，与缩略图占位图里的配色一致
    static QColor typeColorImage()     { return QColor(0xe8, 0x7b, 0xa8); }
    static QColor typeColorVideo()     { return QColor(0x5b, 0x9d, 0xf5); }
    static QColor typeColorAudio()     { return QColor(0x5c, 0xc9, 0x8a); }

    /*! 返回填充过占位符的 QSS 文本。测试与调试时可单独调用。 */
    static QString styleSheet();

private:
    static void applyPalette(QApplication *app);
    static void applyFont(QApplication *app);
};

} // namespace media

#endif // THEMEMANAGER_H
