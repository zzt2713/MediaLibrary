#include "ThemeManager.h"

#include <QApplication>
#include <QPalette>
#include <QFont>
#include <QFile>
#include <QDir>
#include <QDebug>

namespace media {

namespace {

/*!
 * QSS 占位符 → 实际色值。
 *
 * 用 %NAME% 而不是 Qt 的 qproperty 或资源系统，
 * 是因为 QSS 本身不支持变量，而占位符替换是最简单可靠的办法。
 */
struct Placeholder { const char *name; QColor color; };

const QList<Placeholder> &placeholderTable()
{
    static const QList<Placeholder> table = {
        { "WINDOW_BG",      ThemeManager::windowBackground() },
        { "PANEL_BG",       ThemeManager::panelBackground() },
        { "SURFACE",        ThemeManager::surfaceBackground() },
        { "SURFACE_HOVER",  ThemeManager::surfaceHover() },
        { "SURFACE_PRESSED", ThemeManager::surfacePressed() },
        { "BORDER",         ThemeManager::borderColor() },
        { "BORDER_SUBTLE",  ThemeManager::borderSubtle() },
        { "TEXT_PRIMARY",   ThemeManager::textPrimary() },
        { "TEXT_SECONDARY", ThemeManager::textSecondary() },
        { "TEXT_DISABLED",  ThemeManager::textDisabled() },
        { "ACCENT",         ThemeManager::accent() },
        { "ACCENT_HOVER",   ThemeManager::accentHover() },
        { "ACCENT_SUBTLE",  ThemeManager::accentSubtle() },
        { "FAVORITE",       ThemeManager::favoriteColor() },
        { "DANGER",         ThemeManager::dangerColor() },
        { "SUCCESS",        ThemeManager::successColor() },
    };
    return table;
}

/*! 读取 QSS 文件内容。找不到文件时返回空串，调用方会退回仅调色板方案。 */
QString readStyleSheetFile()
{
    // 依次尝试几个可能的位置：
    // 1. 资源系统（若把 qss 加进了 qrc）
    // 2. 可执行文件同级的 resources/ 目录（部署后）
    // 3. 源码树里的相对路径（开发时直接从构建目录运行）
    const QStringList candidates = {
        QStringLiteral(":/media/style.qss"),
        QDir(QCoreApplication::applicationDirPath())
            .filePath(QStringLiteral("resources/style.qss")),
        QDir(QCoreApplication::applicationDirPath())
            .filePath(QStringLiteral("../media/ui/resources/style.qss")),
        QDir(QCoreApplication::applicationDirPath())
            .filePath(QStringLiteral("../../media/ui/resources/style.qss")),
        QDir(QCoreApplication::applicationDirPath())
            .filePath(QStringLiteral("../../../media/ui/resources/style.qss")),
    };

    for (const QString &path : candidates) {
        QFile file(path);
        if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            const QString content = QString::fromUtf8(file.readAll());
            file.close();
            if (!content.trimmed().isEmpty()) {
                qDebug() << "[ThemeManager] loaded stylesheet from" << path;
                return content;
            }
        }
    }

    qWarning() << "[ThemeManager] style.qss not found; falling back to palette only";
    return QString();
}

} // namespace

ThemeManager::ThemeManager(QObject *parent)
    : QObject(parent)
{
}

QString ThemeManager::styleSheet()
{
    QString qss = readStyleSheetFile();
    if (qss.isEmpty())
        return qss;

    for (const Placeholder &p : placeholderTable()) {
        const QString token = QStringLiteral("%") + QString::fromLatin1(p.name) + QStringLiteral("%");
        // 用 name() 而不是 hex 拼接，避免 alpha 通道影响
        qss.replace(token, p.color.name(QColor::HexRgb));
    }

    return qss;
}

void ThemeManager::apply(QApplication *app)
{
    if (!app)
        return;

    applyFont(app);
    applyPalette(app);

    const QString qss = styleSheet();
    if (!qss.isEmpty())
        app->setStyleSheet(qss);
}

void ThemeManager::applyFont(QApplication *app)
{
    QFont font = app->font();

    // 中文界面优先用雅黑/苹方，数字与拉丁字符回退到 Segoe UI。
    // 不硬设 pointSize，保留系统 DPI 缩放行为——设死了在 150% 缩放下会偏小。
    font.setFamily(QStringLiteral("Microsoft YaHei UI"));
    font.setStyleStrategy(QFont::PreferAntialias);

    app->setFont(font);
}

void ThemeManager::applyPalette(QApplication *app)
{
    QPalette p;

    // 即使 QSS 覆盖了大部分控件，QPalette 仍然影响原生绘制部分，
    // 而且 QSS 加载失败时它是唯一的兜底。两边都设，保证不会出现白底黑字。
    p.setColor(QPalette::Window, windowBackground());
    p.setColor(QPalette::WindowText, textPrimary());
    p.setColor(QPalette::Base, surfaceBackground());
    p.setColor(QPalette::AlternateBase, panelBackground());
    p.setColor(QPalette::ToolTipBase, surfaceBackground());
    p.setColor(QPalette::ToolTipText, textPrimary());
    p.setColor(QPalette::Text, textPrimary());
    p.setColor(QPalette::Button, surfaceBackground());
    p.setColor(QPalette::ButtonText, textPrimary());
    p.setColor(QPalette::BrightText, dangerColor());
    p.setColor(QPalette::Link, accent());
    p.setColor(QPalette::Highlight, accentSubtle());
    p.setColor(QPalette::HighlightedText, textPrimary());
    p.setColor(QPalette::PlaceholderText, textDisabled());

    p.setColor(QPalette::Disabled, QPalette::Text, textDisabled());
    p.setColor(QPalette::Disabled, QPalette::ButtonText, textDisabled());
    p.setColor(QPalette::Disabled, QPalette::WindowText, textDisabled());

    app->setPalette(p);
}

} // namespace media
