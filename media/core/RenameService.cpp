#include "RenameService.h"
#include "MediaDatabase.h"

#include <QWidget>
#include <QInputDialog>
#include <QMessageBox>
#include <QLineEdit>
#include <QFile>
#include <QFileInfo>

namespace media {

RenameService::RenameService(MediaDatabase *database, QObject *parent)
    : QObject(parent)
    , m_database(database)
{
}

bool RenameService::rename(const MediaItem &item, QWidget *parent)
{
    if (!m_database || !item.isValid())
        return false;

    const QString oldName = item.fileName;

    bool ok = false;
    const QString newName = QInputDialog::getText(
        parent, QStringLiteral("重命名"),
        QStringLiteral("新的名称："), QLineEdit::Normal, oldName, &ok);

    if (!ok)
        return false;

    const QString trimmed = newName.trimmed();
    if (trimmed.isEmpty()) {
        QMessageBox::warning(parent, QStringLiteral("名称无效"),
                             QStringLiteral("名称不能为空。"));
        return false;
    }

    // 名称没变就不要写库，避免一次无意义的 reload
    if (trimmed == oldName)
        return false;

    // 扩展名必须保留：把 .jpg 改掉会让整个媒体类型识别跟着失效
    const QString oldSuffix = QFileInfo(item.path).suffix();
    const QString newSuffix = QFileInfo(trimmed).suffix();
    if (!oldSuffix.isEmpty() && newSuffix != oldSuffix) {
        QMessageBox::warning(parent, QStringLiteral("无法修改扩展名"),
                             QStringLiteral("扩展名决定媒体的识别方式，不能修改。\n\n"
                                            "原扩展名：%1").arg(oldSuffix));
        return false;
    }

    // 不带扩展名的文件（罕见但存在）：不接受空后缀的新名字
    if (oldSuffix.isEmpty() && !newSuffix.isEmpty()) {
        QMessageBox::warning(parent, QStringLiteral("无法添加扩展名"),
                             QStringLiteral("原文件没有扩展名，重命名时也必须保持没有。"));
        return false;
    }

    const QString newPath = QFileInfo(item.path).absolutePath()
                            + QLatin1Char('/') + trimmed;

    if (QFileInfo::exists(newPath)) {
        QMessageBox::warning(parent, QStringLiteral("名称冲突"),
                             QStringLiteral("该目录下已存在同名文件：\n%1").arg(trimmed));
        return false;
    }

    /*!
     * 缺失的源文件不能改名。这种情况条目本应是 missing = 1，
     * 走到这里说明索引与磁盘不一致（移动硬盘拔掉过）。
     */
    if (!QFileInfo(item.path).exists()) {
        QMessageBox::warning(parent, QStringLiteral("文件不存在"),
                             QStringLiteral("磁盘上找不到该文件：\n%1\n\n"
                                            "无法重命名。请尝试重新扫描目录。")
                                 .arg(item.path));
        return false;
    }

    // 1) 先改磁盘
    QFile file(item.path);
    if (!file.rename(newPath)) {
        QMessageBox::warning(parent, QStringLiteral("重命名失败"),
                             QStringLiteral("无法修改磁盘上的文件名：\n%1\n\n"
                                            "磁盘文件未被改动，索引也未变更。")
                                 .arg(file.errorString()));
        return false;
    }

    // 2) 再改索引
    if (!m_database->renameItem(item.path, newPath)) {
        QMessageBox::warning(parent, QStringLiteral("重命名失败"),
                             QStringLiteral("磁盘文件已改名为 %1，但索引更新失败：\n%2\n\n"
                                            "请重新扫描该目录以同步索引。")
                                 .arg(trimmed, m_database->lastError()));
        return false;
    }

    return true;
}

} // namespace media
