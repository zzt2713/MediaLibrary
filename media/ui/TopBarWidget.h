#ifndef TOPBARWIDGET_H
#define TOPBARWIDGET_H

#include "ThumbnailGrid.h"

#include <QWidget>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSlider;

namespace media {

/*!
 * \brief 顶部工具条：品牌标识、动作按钮、搜索、排序、视图模式、缩略图尺寸。
 *
 * 抽出来的原因：这一条里塞了六组控件，而主窗口原本要靠成员指针
 * 直接操作它们——改视图模式要同时改两个按钮的图标，改尺寸要同时动
 * 滑块和标签。控件状态与"谁该更新它"分散在两个类里，是 bug 的高发区。
 *
 * 现在控件状态只在这里维护：
 *  - 视图模式的图标跟随选中态、尺寸标签跟随滑块，都在内部联动
 *  - 对外只发语义化信号（用户改了排序键、改了视图模式…）和少量取值接口
 *  - "改了什么要落到哪儿"（模型排序、网格重排）仍由主窗口决定
 */
class TopBarWidget : public QWidget
{
    Q_OBJECT

public:
    explicit TopBarWidget(QWidget *parent = nullptr);

    /*!
     * \brief 按排序键切换下拉选中项。
     *
     * 键在列表里不存在或与当前一致时都不做任何事——
     * 与当前一致时必须跳过，否则会多余地触发一次排序信号。
     */
    void setSortKey(const QString &key);

    /*! 切换视图模式。会同步两个按钮的选中态与图标，并发出 viewModeChanged。 */
    void setViewMode(ThumbnailGrid::ViewMode mode);

    /*! 当前缩略图尺寸的滑块值（0-100）。 */
    int thumbnailSliderValue() const;

    /*! 设置滑块值。值变化时会更新标签并发出 thumbnailSizeChanged。 */
    void setThumbnailSliderValue(int value);

    /*!
     * \brief 以编程方式设置搜索框文本。
     *
     * 与用户手动输入的区别：不会发出 searchTextChanged（否则
     * MainWindow::applySmartGroup 清空搜索框时会绕回自己的槽，
     * 形成"清空→输入→再清空"的无谓往返）。
     * 需要同步触发筛选时请显式发出信号或调用对应槽。
     */
    void setSearchText(const QString &text);
    QString searchText() const;

signals:
    void addFolderRequested();
    void refreshRequested();
    void settingsRequested();

    void searchTextChanged(const QString &text);
    /*! 用户提交了搜索词（回车）。用于触发覆盖全库的查询。 */
    void searchSubmitted(const QString &text);
    /*! 用户切换了排序方式。参数即新的排序键。 */
    void sortKeyChanged(const QString &key);
    /*! 视图模式变化。true 为网格，false 为列表。 */
    void viewModeChanged(bool gridMode);
    /*! 缩略图尺寸滑块值变化。 */
    void thumbnailSizeChanged(int sliderValue);

private:
    void updateViewModeIcons();
    void updateThumbnailSizeLabel();

    QLineEdit   *m_searchBox = nullptr;
    QComboBox   *m_sortCombo = nullptr;
    QPushButton *m_gridButton = nullptr;
    QPushButton *m_listButton = nullptr;
    QSlider     *m_sizeSlider = nullptr;
    QLabel      *m_sizeLabel = nullptr;

    ThumbnailGrid::ViewMode m_viewMode = ThumbnailGrid::GridMode;
};

} // namespace media

#endif // TOPBARWIDGET_H
