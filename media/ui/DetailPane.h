#ifndef DETAILPANE_H
#define DETAILPANE_H

#include "../core/MediaTypes.h"

#include <QWidget>
#include <QString>
#include <QStringList>
#include <QImage>
#include <QPixmap>

class QLabel;
class QVBoxLayout;
class QScrollArea;
class QLineEdit;
class QPushButton;
class QGridLayout;
class QEvent;
class QResizeEvent;

namespace media {

class MediaDatabase;

/*!
 * \brief 右侧详情面板：预览图 + 元数据 + 标签 + 操作按钮。
 *
 * 设计上刻意保持只读为主：这个面板是"看清楚这是什么"的地方，
 * 编辑类操作（重命名、移动）都放到右键菜单，避免面板变得臃肿。
 */
class DetailPane : public QWidget
{
    Q_OBJECT

public:
    explicit DetailPane(QWidget *parent = nullptr);
    ~DetailPane() override;

    void setDatabase(MediaDatabase *database);

    /*! 显示某个资源的详情。传无效 item 时切到空状态。 */
    void showItem(const MediaItem &item);

    /*! 清空（多选或未选中时显示）。 */
    void clear();

    /*! 多选时显示"已选 N 项"的汇总信息。 */
    void showMultiSelection(int count, qint64 totalBytes);

    /*! 当前展示的条目 id，用于取消过期请求。 */
    qint64 currentItemId() const { return m_current.id; }

    /*!
     * \brief 外部缩略图就绪后回填预览图。
     *
     * 参数是 QImage 而不是 QPixmap：调用方（MainWindow）拿到的是从工作线程
     * 投递回来的结果，那一侧只有 QImage 可用。转换在这个方法内部完成——
     * 也就是说，QPixmap 的产生点被收拢到 UI 类自己的成员函数里，
     * 调用方不需要知道"哪个线程才允许碰 QPixmap"这件事。
     */
    void setPreviewImage(const QImage &image);

protected:
    /*! QLabel 没有 doubleClicked 信号，用事件过滤器捕预览区的双击。 */
    bool eventFilter(QObject *watched, QEvent *event) override;
    /*! 面板宽度变化后重排标签 chip（换行位置依赖实际宽度）。 */
    void resizeEvent(QResizeEvent *event) override;

signals:
    /*! 用户点击收藏按钮。 */
    void favoriteToggled(qint64 itemId, bool favorite);
    /*! 用户请求播放（双击预览或点播放按钮）。 */
    void playRequested(const media::MediaItem &item);
    /*! 用户添加标签。 */
    void tagAdded(qint64 itemId, const QString &tag);
    /*! 用户移除标签。 */
    void tagRemoved(qint64 itemId, const QString &tag);

private slots:
    void onFavoriteClicked();
    void onTagInputReturnPressed();
    void onPreviewDoubleClicked();

private:
    void buildUi();
    void rebuildTagChips();
    /*! 清空标签区（含嵌套的行布局）。不递归会漏掉 chip 里的按钮。 */
    void clearTagLayout();
    void setRowValue(QLabel *valueLabel, const QString &text);
    void clearGrid();

    /*! 键值行。用 grid 布局让键右对齐、值左对齐。 */
    QGridLayout *m_infoGrid = nullptr;
    QList<QLabel *> m_valueLabels;   // 按添加顺序保存值标签，便于更新

    QLabel   *m_preview = nullptr;
    QLabel   *m_fileName = nullptr;
    QLabel   *m_typeBadge = nullptr;
    QPushButton *m_favoriteBtn = nullptr;

    QWidget  *m_tagArea = nullptr;
    QVBoxLayout *m_tagLayout = nullptr;
    QLineEdit *m_tagInput = nullptr;

    QLabel *m_placeholder = nullptr;  // 空状态提示
    QWidget *m_content = nullptr;     // 有内容时显示的容器
    QScrollArea *m_scroll = nullptr;

    MediaDatabase *m_database = nullptr;
    MediaItem m_current;
    QStringList m_currentTags;
    /*! 上次 chip 重排时的标签区宽度，用于判断 resize 后是否需要重排。 */
    int m_lastChipRebuildWidth = 0;
};

} // namespace media

#endif // DETAILPANE_H
