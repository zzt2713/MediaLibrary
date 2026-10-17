#ifndef PREVIEWDIALOG_H
#define PREVIEWDIALOG_H

#include "../core/MediaTypes.h"

#include <QDialog>
#include <QGraphicsView>
#include <QGraphicsScene>
#include <QPixmap>

class QLabel;
class QPushButton;
class QSlider;
class QGraphicsPixmapItem;
class QStackedWidget;
class QMediaPlayer;
class QAudioOutput;
class QVideoWidget;
class QTimer;

namespace media {

class MediaDatabase;

/*!
 * \brief 图片查看控件：支持滚轮缩放、拖拽平移、适应窗口、100% 原始尺寸。
 *
 * 用 QGraphicsView 而不是自己算缩放矩阵：
 * 它内置了平滑变换、滚动条、场景坐标转换，
 * 自己实现这些在大图上很容易出现偏移与模糊。
 */
class ImageViewer : public QGraphicsView
{
    Q_OBJECT

public:
    explicit ImageViewer(QWidget *parent = nullptr);

    bool loadImage(const QString &path);
    void clearImage();

    void setFitToWindow(bool fit);
    bool isFitToWindow() const { return m_fitToWindow; }

    /*! 缩放到原始尺寸（100%）。 */
    void zoomToOriginal();

    /*! 相对当前比例缩放。factor > 1 放大。 */
    void zoomBy(double factor);

    /*! 适应窗口。 */
    void fitInView();

    double scaleFactor() const { return m_scale; }

signals:
    void scaleChanged(double scale);
    void loadFailed(const QString &path, const QString &reason);

protected:
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    void applyScale(double newScale);
    void updateCursorForDrag(bool dragging);

    QGraphicsScene *m_scene = nullptr;
    QGraphicsPixmapItem *m_item = nullptr;
    QString m_currentPath;

    double m_scale = 1.0;
    bool m_fitToWindow = true;
    bool m_dragging = false;
    bool m_dragMoved = false;
    QPoint m_lastDragPos;
};

/*!
 * \brief 统一的媒体预览对话框。
 *
 * 同一个对话框承载三种媒体：
 *  - 图片：ImageViewer + 缩放工具栏
 *  - 视频：QVideoWidget + 播放控制
 *  - 音频：波形占位 + 播放控制
 *
 * 做成一个对话框而不是三个，好处是"下一个/上一个"可以统一处理，
 * 用户按空格/方向键在资源之间切换时不会遇到功能缺失。
 */
class PreviewDialog : public QDialog
{
    Q_OBJECT

public:
    explicit PreviewDialog(QWidget *parent = nullptr);
    ~PreviewDialog() override;

    void setDatabase(MediaDatabase *database);

    /*! 设置默认音量（0-100），来自设置界面的 playback/volume。 */
    void setVolume(int percent);

    /*! 是否从上次位置续播。关闭时每次打开都从 0 开始。 */
    void setResumePlayback(bool enable);

    /*! 播完是否自动切下一个。 */
    void setAutoPlayNext(bool enable);

    /*! 打开某个资源。 */
    void openItem(const MediaItem &item);

    /*!
     * \brief 设置同目录的可浏览列表，支持上一个/下一个。
     * \param items 列表（通常是当前视图里的全部项）
     * \param currentIndex 当前项在列表中的位置
     */
    void setPlaylist(const QList<MediaItem> &items, int currentIndex);

    /*! 当前显示的条目。 */
    MediaItem currentItem() const { return m_current; }

signals:
    /*! 播放位置变化，用于写入播放记录。 */
    void playbackRecorded(qint64 itemId, double positionMs);
    /*! 切换到了另一个资源。 */
    void itemChanged(const media::MediaItem &item);

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onPlayPauseClicked();
    void onStopClicked();
    void onPrevious();
    void onNext();
    void onPositionSliderPressed();
    void onPositionSliderReleased();
    void onPositionSliderMoved(int value);
    void onDurationChanged(qint64 durationMs);
    void onPositionChanged(qint64 positionMs);
    void onMediaStatusChanged(int status);
    void onVolumeChanged(int value);
    void onMuteToggled(bool muted);

private:
    void buildUi();
    void showImage(const MediaItem &item);
    void showVideo(const MediaItem &item);
    void showAudio(const MediaItem &item);
    void stopPlayback();

    void updatePlayButton();
    void updateNavigationButtons();
    void updateTitle();
    void setControlsEnabled(bool enabled);

    /*!
     * \brief 按媒体类型切换控制栏分组显隐。
     *
     * 图片页只显示图片工具组，视频/音频页只显示播放控制组。
     * 翻页组（上一张/下一张）三类媒体共用，始终显示。
     */
    void setControlBarsVisible(MediaType type);

    /*! 把毫秒格式化成 mm:ss，供滑块标签使用。 */
    static QString formatMs(qint64 ms);

    QStackedWidget *m_stack = nullptr;

    ImageViewer *m_imageView = nullptr;
    QVideoWidget *m_videoWidget = nullptr;
    QWidget     *m_audioPanel = nullptr;
    QLabel      *m_audioArtwork = nullptr;

    QLabel      *m_titleLabel = nullptr;
    QLabel      *m_infoLabel = nullptr;

    QPushButton *m_prevBtn = nullptr;
    QPushButton *m_nextBtn = nullptr;

    /*!
     * 两组控制栏容器，按媒体类型整组显隐。
     *
     * m_playbackBar：播放 / 停止 / 进度 / 音量，只属于视频与音频。
     * m_imageBar：缩小 / 缩放比 / 放大 / 适应窗口 / 原始尺寸，只属于图片。
     *
     * 分装而不是平铺在同一个布局里逐个 setEnabled，
     * 是为了让"图片预览里出现音量滑块"这类串用从结构上不可能发生。
     */
    QWidget     *m_playbackBar = nullptr;
    QWidget     *m_imageBar = nullptr;

    QPushButton *m_playBtn = nullptr;
    QPushButton *m_stopBtn = nullptr;
    QPushButton *m_muteBtn = nullptr;
    QPushButton *m_fitBtn = nullptr;
    QPushButton *m_zoomInBtn = nullptr;
    QPushButton *m_zoomOutBtn = nullptr;
    QPushButton *m_originalBtn = nullptr;
    QLabel      *m_scaleLabel = nullptr;

    QSlider     *m_positionSlider = nullptr;
    QSlider     *m_volumeSlider = nullptr;
    QLabel      *m_positionLabel = nullptr;
    QLabel      *m_durationLabel = nullptr;

    QMediaPlayer *m_player = nullptr;
    QAudioOutput *m_audioOutput = nullptr;

    /*!
     * 位置滑块有个经典陷阱：播放时每秒触发十几次 positionChanged，
     * 如果每次都回写滑块，用户就没法拖动（会被不断拽回去）。
     * 用这个标志在用户拖动期间暂停回写。
     */
    bool m_sliderDragging = false;

    MediaItem m_current;
    QList<MediaItem> m_playlist;
    int m_playlistIndex = -1;

    MediaDatabase *m_database = nullptr;

    /*!
     * 播放位置记录节流。
     * 每个 positionChanged 都写库会造成大量无意义的 IO，
     * 这里限制最多每 5 秒写一次。
     */
    QTimer *m_recordTimer = nullptr;

    /*! 是否从上次位置续播（设置项 playback/resumePlayback）。 */
    bool m_resumePlayback = true;

    /*! 播完是否自动切下一个（设置项 playback/autoPlayNext）。 */
    bool m_autoPlayNext = true;
};

} // namespace media

#endif // PREVIEWDIALOG_H
