#ifndef STATUSBARWIDGET_H
#define STATUSBARWIDGET_H

#include "../core/MediaTypes.h"

#include <QWidget>
#include <QString>
#include <QList>

class QLabel;
class QProgressBar;
class QTimer;

namespace media {

class ThumbnailCache;

/*!
 * \brief 底部状态栏的内容区。
 *
 * 抽出来的原因：这一条栏位里有五个各自独立的显示区（条目计数、选中摘要、
 * 扫描进度条、扫描提示文本、缓存统计），而它们的写入点散落在
 * MainWindow 的十几个函数里。写在主窗口里的时候，"刷新状态栏"这件事
 * 变成了十几处直接操作 QLabel 的散点，改一处文案要先把这些位置都找出来。
 *
 * 现在的分工：
 *  - 主窗口只给出"数据"（几个数、几条消息），不接触任何 QLabel
 *  - 文案拼接、进度条显隐、缓存统计的自动刷新都归这里
 *  - 缓存统计的定时刷新也从主窗口搬了进来：它本来就是这一栏自己的事
 */
class StatusBarWidget : public QWidget
{
    Q_OBJECT

public:
    /*!
     * \param cache 缩略图缓存，用于定时显示缓存统计。只借用指针，不接管所有权。
     */
    explicit StatusBarWidget(ThumbnailCache *cache, QWidget *parent = nullptr);

    /*!
     * \brief 条目计数。
     * \param total              库内总条目数
     * \param visibleAfterFilter 筛选后可见的行数；传负数表示"无筛选信息"，只显示总数
     *
     * 是否显示"筛选后"由调用方判断：只有"可见数 < 已加载数"才算真的被筛掉了。
     * 若在这里改用 `可见数 < 总数` 判断，分页加载时会误报——
     * 首屏只加载了 200 条而库里有上万条，看起来就像被筛选过。
     */
    void setItemCount(int total, int visibleAfterFilter = -1);

    /*! 选中项摘要：单项显示文件名，多项显示数量与总大小，空列表清空。 */
    void setSelectionSummary(const QList<MediaItem> &selected);

    /*! 常驻的扫描提示文本（"正在扫描 xxx…"、"已有扫描任务正在进行…"）。传空串清空。 */
    void setScanMessage(const QString &text);

    /*!
     * \brief 显示一条到点自动消失的提示。
     *
     * 用于"跳过无权限目录""功能未实现"这类一次性消息。
     * 到点时只清掉自己设的那条：期间若已有新消息覆盖了标签，
     * 说明那条消息有自己的生命周期（例如扫描仍在进行），不该被这里清掉。
     */
    void showTransientMessage(const QString &text, int timeoutMs = 3000);

    /*! 扫描进度条的显隐。 */
    void setScanProgressVisible(bool visible);

    /*! 总数未知阶段（预统计完成前）：进度条显示为循环滚动。 */
    void beginScanProgress();

    /*! 已知总数：显示确定进度。进度条不可见时忽略，避免扫描结束后又被填回数值。 */
    void setScanProgress(int discovered, int total);

    /*! 显隐缓存统计（跟随设置项）。 */
    void setCacheStatsVisible(bool visible);

    /*! 立即刷新一次缓存统计文本。 */
    void refreshCacheStats();

    /*!
     * \brief 停止自动刷新。
     *
     * 关窗时调用：收尾阶段数据库与缓存正在被释放，
     * 定时器再跑一轮就会去读已经不该动的东西。
     */
    void stopAutoRefresh();

private:
    QLabel       *m_count = nullptr;
    QLabel       *m_selection = nullptr;
    QLabel       *m_scanMessage = nullptr;
    QLabel       *m_cache = nullptr;
    QProgressBar *m_progress = nullptr;

    QTimer *m_autoRefreshTimer = nullptr;
    QTimer *m_transientTimer = nullptr;

    ThumbnailCache *m_cacheSource = nullptr;
    QString         m_transientText;
};

} // namespace media

#endif // STATUSBARWIDGET_H
