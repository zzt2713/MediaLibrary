#ifndef EMPTYSTATEWIDGET_H
#define EMPTYSTATEWIDGET_H

#include <QWidget>

class QLabel;

namespace media {

/*!
 * \brief 资源页的空状态页。
 *
 * 三种"看不到东西"的原因需要给三种不同的引导，文案不能混用：
 *  - 库里还没有任何目录 → 教用户怎么添加
 *  - 有目录但正在建索引 → 让用户知道要等，且会自动出结果
 *  - 有目录、没在扫描、但筛选后为空 → 提醒可能是筛选条件的问题
 *
 * 之前的写法是主窗口持有两个 QLabel，在 updateEmptyState() 里成对地
 * setText。文案与判断条件分居两个类，改一种情况要同时改两处；
 * 现在判断与文案都收在这里，主窗口只负责告诉我们"现在是什么情况"。
 */
class EmptyStateWidget : public QWidget
{
    Q_OBJECT

public:
    /*! 空状态的原因。 */
    enum Reason {
        NoFolder,       //!< 资源库里还没有任何目录
        Indexing,       //!< 有目录，正在建索引
        FilteredEmpty   //!< 有目录、未在扫描，但当前条件下没有可见内容
    };

    explicit EmptyStateWidget(QWidget *parent = nullptr);

    /*! 按原因切换文案。 */
    void setReason(Reason reason);

signals:
    /*! 用户点了"添加目录"。主窗口接到后走与工具栏相同的那条路径。 */
    void addFolderRequested();

private:
    QLabel *m_title = nullptr;
    QLabel *m_hint = nullptr;
};

} // namespace media

#endif // EMPTYSTATEWIDGET_H
