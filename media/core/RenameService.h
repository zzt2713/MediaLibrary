#ifndef RENAMESERVICE_H
#define RENAMESERVICE_H

#include "../core/MediaTypes.h"

#include <QObject>
#include <QString>

class QWidget;

namespace media {

class MediaDatabase;

/*!
 * \brief 资源重命名。
 *
 * 从 MainWindow 里抽出来，因为它本来就不是"主窗口职责"：
 * 这是一段包含 5 类校验 + 两阶段写入的独立业务流程，
 * 混在主窗口的槽里会让 onRenameRequested 独自占掉上百行。
 *
 * 保留原有行为，一行逻辑都没改：
 *
 *  1. 校验名称（非空、未改变、扩展名必须保留、不能重名）
 *  2. 校验源文件存在
 *  3. 先改磁盘，成功后再改索引
 *
 * 顺序不能反：先改索引再改磁盘的话，磁盘改名一旦失败
 * 会留下"索引指向不存在的文件"的中间状态，表现为条目突然变成失效记录。
 *
 * 对话与提示由调用方传入 parent，本类不持有任何 UI 对象。
 */
class RenameService : public QObject
{
    Q_OBJECT

public:
    explicit RenameService(MediaDatabase *database, QObject *parent = nullptr);

    /*!
     * \brief 执行重命名。
     * \param item     要改名的资源
     * \param parent   对话框的宿主窗口，用于居中显示
     * \return 实际执行了改名并成功时为 true；用户取消、校验失败或任一阶段失败均为 false
     */
    bool rename(const MediaItem &item, QWidget *parent);

private:
    MediaDatabase *m_database = nullptr;
};

} // namespace media

#endif // RENAMESERVICE_H
