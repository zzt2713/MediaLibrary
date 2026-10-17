#include "PreviewDialog.h"
#include "Icons.h"
#include "ThemeManager.h"
#include "../core/MediaDatabase.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QStackedWidget>
#include <QGraphicsScene>
#include <QGraphicsPixmapItem>
#include <QMediaPlayer>
#include <QAudioOutput>
#include <QVideoWidget>
#include <QTimer>
#include <QKeyEvent>
#include <QCloseEvent>
#include <QWheelEvent>
#include <QMouseEvent>
#include <QScrollBar>
#include <QFileInfo>
#include <QDir>
#include <QFile>
#include <QImageReader>
#include <QtMath>

namespace media {

namespace {

/*! 缩放范围限制。低于 0.05 只会看到一片噪点，高于 20 倍没有实际意义。 */
constexpr double kMinScale = 0.05;
constexpr double kMaxScale = 20.0;

/*! 播放位置写库的最小间隔，避免高频 IO。 */
constexpr int kRecordIntervalMs = 5000;

} // namespace

// ================================================================ ImageViewer

ImageViewer::ImageViewer(QWidget *parent)
    : QGraphicsView(parent)
{
    m_scene = new QGraphicsScene(this);
    setScene(m_scene);

    setRenderHint(QPainter::SmoothPixmapTransform, true);
    setRenderHint(QPainter::Antialiasing, true);
    setDragMode(QGraphicsView::NoDrag);          // 我们自己处理拖拽，以区分点击与拖动
    setTransformationAnchor(QGraphicsView::AnchorUnderMouse);  // 滚轮缩放时以鼠标位置为锚点
    setResizeAnchor(QGraphicsView::AnchorViewCenter);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setBackgroundBrush(QColor(0x14, 0x15, 0x18));
    setFrameShape(QFrame::NoFrame);
    setFocusPolicy(Qt::StrongFocus);             // 需要接收键盘事件

    m_scene->setBackgroundBrush(QColor(0x14, 0x15, 0x18));
}

bool ImageViewer::loadImage(const QString &path)
{
    clearImage();

    QImageReader reader(path);
    reader.setAutoTransform(true);   // 按 EXIF 方向摆正

    if (!reader.canRead()) {
        emit loadFailed(path, QStringLiteral("无法读取该图片（可能已损坏或格式不受支持）"));
        return false;
    }

    const QImage image = reader.read();
    if (image.isNull()) {
        emit loadFailed(path, QStringLiteral("图片解码失败"));
        return false;
    }

    QPixmap pixmap = QPixmap::fromImage(image);

    m_item = m_scene->addPixmap(pixmap);
    m_item->setTransformationMode(Qt::SmoothTransformation);

    // 场景矩形留一点边距，让图片不会紧贴视口边缘
    m_scene->setSceneRect(pixmap.rect());

    m_currentPath = path;
    m_fitToWindow = true;
    fitInView();

    return true;
}

void ImageViewer::clearImage()
{
    m_scene->clear();
    m_item = nullptr;
    m_currentPath.clear();
    m_scale = 1.0;
}

void ImageViewer::setFitToWindow(bool fit)
{
    m_fitToWindow = fit;

    if (fit) {
        fitInView();
    } else {
        // 退出适应模式时保持当前比例，不跳回 100%
        m_scale = transform().m11();
        emit scaleChanged(m_scale);
    }
}

void ImageViewer::fitInView()
{
    if (!m_item)
        return;

    // QGraphicsView::fitInView 是基类方法，这里不递归
    QGraphicsView::fitInView(m_item, Qt::KeepAspectRatio);

    m_scale = transform().m11();
    m_fitToWindow = true;
    emit scaleChanged(m_scale);
}

void ImageViewer::zoomToOriginal()
{
    if (!m_item)
        return;

    m_fitToWindow = false;
    resetTransform();
    m_scale = 1.0;
    emit scaleChanged(m_scale);
}

void ImageViewer::zoomBy(double factor)
{
    if (!m_item)
        return;

    const double target = m_scale * factor;
    applyScale(target);
}

void ImageViewer::applyScale(double newScale)
{
    newScale = qBound(kMinScale, newScale, kMaxScale);

    if (qFuzzyCompare(newScale, m_scale))
        return;

    // 用相对比例再 resetTransform，避免浮点误差在连续缩放中累积
    const double relative = newScale / m_scale;

    m_fitToWindow = false;
    scale(relative, relative);
    m_scale = newScale;

    emit scaleChanged(m_scale);
}

void ImageViewer::wheelEvent(QWheelEvent *event)
{
    if (!m_item) {
        QGraphicsView::wheelEvent(event);
        return;
    }

    /*!
     * 预览窗口里的图片一律用滚轮缩放，不要求按 Ctrl。
     *
     * Ctrl+滚轮是通用图形软件的约定，但这里不适用：预览窗口整个视口
     * 就只有这一张图，普通滚动没有别的内容可滚。要求按 Ctrl 反而让用户
     * 第一次拿到手时不知道能缩放。
     *
     * 缩放以鼠标位置为锚点（构造函数里设了 AnchorUnderMouse），
     * 所以滚到哪儿就放大哪儿，符合"对着细节看"的直觉。
     */
    const double steps = event->angleDelta().y() / 120.0;
    if (steps != 0.0) {
        zoomBy(qPow(1.15, steps));
        event->accept();
        return;
    }

    QGraphicsView::wheelEvent(event);
}

void ImageViewer::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && m_item) {
        m_dragging = true;
        m_dragMoved = false;
        m_lastDragPos = event->pos();
        updateCursorForDrag(true);
        event->accept();
        return;
    }

    QGraphicsView::mousePressEvent(event);
}

void ImageViewer::mouseMoveEvent(QMouseEvent *event)
{
    if (m_dragging) {
        const QPoint delta = event->pos() - m_lastDragPos;

        // 位移超过 3px 才算拖动，避免轻微抖动被当成拖动而吞掉点击
        if (delta.manhattanLength() > 3)
            m_dragMoved = true;

        if (m_dragMoved) {
            m_lastDragPos = event->pos();
            horizontalScrollBar()->setValue(horizontalScrollBar()->value() - delta.x());
            verticalScrollBar()->setValue(verticalScrollBar()->value() - delta.y());
        }

        event->accept();
        return;
    }

    QGraphicsView::mouseMoveEvent(event);
}

void ImageViewer::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && m_dragging) {
        m_dragging = false;
        updateCursorForDrag(false);
        event->accept();
        return;
    }

    QGraphicsView::mouseReleaseEvent(event);
}

void ImageViewer::updateCursorForDrag(bool dragging)
{
    setCursor(dragging ? Qt::ClosedHandCursor : Qt::OpenHandCursor);
}

void ImageViewer::resizeEvent(QResizeEvent *event)
{
    QGraphicsView::resizeEvent(event);

    // 适应模式下窗口尺寸变化要重新适应，否则会留下空白或裁切
    if (m_fitToWindow && m_item)
        QGraphicsView::fitInView(m_item, Qt::KeepAspectRatio);
}

void ImageViewer::keyPressEvent(QKeyEvent *event)
{
    switch (event->key()) {
    case Qt::Key_Plus:
    case Qt::Key_Equal:
        zoomBy(1.2);
        return;
    case Qt::Key_Minus:
        zoomBy(1.0 / 1.2);
        return;
    case Qt::Key_0:
        fitInView();
        return;
    case Qt::Key_1:
        zoomToOriginal();
        return;
    default:
        break;
    }

    // 其他按键交给对话框处理（方向键切上下张、Esc 关闭）
    event->ignore();
}

// ================================================================ PreviewDialog

PreviewDialog::PreviewDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("预览"));
    setWindowFlags(windowFlags() | Qt::WindowMaximizeButtonHint);
    resize(1100, 760);

    buildUi();

    m_player = new QMediaPlayer(this);
    m_audioOutput = new QAudioOutput(this);
    m_player->setAudioOutput(m_audioOutput);
    m_audioOutput->setVolume(0.8f);

    // 音量滑块与播放器对齐。滑块的 valueChanged 已连到 onVolumeChanged，
    // 这里 setValue 会把两边的状态一次性同步好。
    if (m_volumeSlider)
        setVolume(m_volumeSlider->value());

    connect(m_player, &QMediaPlayer::durationChanged,
            this, &PreviewDialog::onDurationChanged);
    connect(m_player, &QMediaPlayer::positionChanged,
            this, &PreviewDialog::onPositionChanged);
    connect(m_player, &QMediaPlayer::mediaStatusChanged,
            this, [this](QMediaPlayer::MediaStatus status) {
                onMediaStatusChanged(static_cast<int>(status));
            });
    connect(m_player, &QMediaPlayer::errorOccurred,
            this, [this](QMediaPlayer::Error, const QString &errorString) {
                m_infoLabel->setText(QStringLiteral("播放失败：%1").arg(errorString));
            });

    // 播放位置节流写库：每 5 秒最多一次
    m_recordTimer = new QTimer(this);
    m_recordTimer->setInterval(kRecordIntervalMs);
    connect(m_recordTimer, &QTimer::timeout, this, [this]() {
        if (m_current.id > 0 && m_player && m_player->playbackState() == QMediaPlayer::PlayingState)
            emit playbackRecorded(m_current.id, m_player->position());
    });
    m_recordTimer->start();
}

PreviewDialog::~PreviewDialog()
{
    // 必须先停播放再析构：
    // QMediaPlayer 在播放时析构会异步等待后端线程，
    // 如果它引用的 QVideoWidget 先没了，会崩在析构里。
    stopPlayback();
}

void PreviewDialog::setDatabase(MediaDatabase *database)
{
    m_database = database;
}

void PreviewDialog::setVolume(int percent)
{
    const int v = qBound(0, percent, 100);
    if (m_volumeSlider)
        m_volumeSlider->setValue(v);
    if (m_audioOutput)
        m_audioOutput->setVolume(v / 100.0);
}

void PreviewDialog::setResumePlayback(bool enable)
{
    m_resumePlayback = enable;
}

void PreviewDialog::setAutoPlayNext(bool enable)
{
    m_autoPlayNext = enable;
}

void PreviewDialog::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ---------- 顶部信息条 ----------
    auto *titleBar = new QWidget(this);
    titleBar->setObjectName(QStringLiteral("TopBar"));
    auto *titleLayout = new QHBoxLayout(titleBar);
    titleLayout->setContentsMargins(14, 8, 14, 8);
    titleLayout->setSpacing(10);

    m_titleLabel = new QLabel(titleBar);
    m_titleLabel->setObjectName(QStringLiteral("AppTitle"));
    titleLayout->addWidget(m_titleLabel, 1);

    m_infoLabel = new QLabel(titleBar);
    m_infoLabel->setStyleSheet(QStringLiteral("color: %1;")
                                   .arg(ThemeManager::textSecondary().name()));
    titleLayout->addWidget(m_infoLabel);

    root->addWidget(titleBar);

    // ---------- 主体（堆叠：图片 / 视频 / 音频） ----------
    m_stack = new QStackedWidget(this);

    m_imageView = new ImageViewer(m_stack);
    m_imageView->setCursor(Qt::OpenHandCursor);
    connect(m_imageView, &ImageViewer::scaleChanged, this, [this](double scale) {
        m_scaleLabel->setText(QStringLiteral("%1%").arg(qRound(scale * 100)));
    });
    connect(m_imageView, &ImageViewer::loadFailed, this,
            [this](const QString &, const QString &reason) {
                m_infoLabel->setText(reason);
            });

    m_videoWidget = new QVideoWidget(m_stack);
    m_videoWidget->setStyleSheet(QStringLiteral("background-color: #000000;"));

    // 音频面板：没有封面时显示一个大的音符占位
    m_audioPanel = new QWidget(m_stack);
    auto *audioLayout = new QVBoxLayout(m_audioPanel);
    audioLayout->setAlignment(Qt::AlignCenter);

    m_audioArtwork = new QLabel(m_audioPanel);
    m_audioArtwork->setAlignment(Qt::AlignCenter);
    m_audioArtwork->setFixedSize(220, 220);
    audioLayout->addWidget(m_audioArtwork, 0, Qt::AlignCenter);
    audioLayout->addSpacing(16);

    auto *audioHint = new QLabel(QStringLiteral("音频播放中"), m_audioPanel);
    audioHint->setAlignment(Qt::AlignCenter);
    audioHint->setStyleSheet(QStringLiteral("color: %1; font-size: 14px;")
                                 .arg(ThemeManager::textSecondary().name()));
    audioLayout->addWidget(audioHint);

    m_stack->addWidget(m_imageView);
    m_stack->addWidget(m_videoWidget);
    m_stack->addWidget(m_audioPanel);

    root->addWidget(m_stack, 1);

    // ---------- 底部控制栏 ----------
    auto *controlBar = new QWidget(this);
    controlBar->setObjectName(QStringLiteral("TopBar"));
    auto *controlLayout = new QHBoxLayout(controlBar);
    controlLayout->setContentsMargins(12, 8, 12, 8);
    controlLayout->setSpacing(8);

    /*!
     * 控件按「职责」分成两组装在各自容器里，按媒体类型整组显隐：
     *
     *   m_playbackBar  上一张 / 播放 / 停止 / 下一张 / 进度 / 音量
     *   m_imageBar     缩小 / 缩放比 / 放大 / 适应窗口 / 原始尺寸
     *
     * 之前这些控件直接平铺在同一个布局里，看图片时靠逐个 setEnabled(false)
     * 置灰。那只是"不可点"，控件（包括音量滑块）依然占着位置、依然可见，
     * 属于审计任务书明确禁止的"通过禁用或隐藏控件让表面看起来正常"。
     * 整组装进独立容器后，切媒体类型时只改两个 setVisible，
     * 布局也自动重排，不会留下空白缝隙。
     */
    auto *navGroup = new QWidget(controlBar);          // 两类媒体共用的翻页
    auto *navLayout = new QHBoxLayout(navGroup);
    navLayout->setContentsMargins(0, 0, 0, 0);
    navLayout->setSpacing(8);

    // 上一张 / 下一张
    m_prevBtn = new QPushButton(navGroup);
    m_prevBtn->setProperty("iconButton", true);
    m_prevBtn->setIcon(Icons::get(Icons::ChevronLeft, ThemeManager::textPrimary(), 18));
    m_prevBtn->setToolTip(QStringLiteral("上一个 (←)"));
    connect(m_prevBtn, &QPushButton::clicked, this, &PreviewDialog::onPrevious);
    navLayout->addWidget(m_prevBtn);

    m_nextBtn = new QPushButton(navGroup);
    m_nextBtn->setProperty("iconButton", true);
    m_nextBtn->setIcon(Icons::get(Icons::ChevronRight, ThemeManager::textPrimary(), 18));
    m_nextBtn->setToolTip(QStringLiteral("下一个 (→)"));
    connect(m_nextBtn, &QPushButton::clicked, this, &PreviewDialog::onNext);
    navLayout->addWidget(m_nextBtn);

    controlLayout->addWidget(navGroup);

    controlLayout->addSpacing(8);

    // ---------- 播放控制组（仅视频 / 音频） ----------
    m_playbackBar = new QWidget(controlBar);
    auto *playbackLayout = new QHBoxLayout(m_playbackBar);
    playbackLayout->setContentsMargins(0, 0, 0, 0);
    playbackLayout->setSpacing(8);

    m_playBtn = new QPushButton(m_playbackBar);
    m_playBtn->setProperty("iconButton", true);
    m_playBtn->setIcon(Icons::get(Icons::Play, ThemeManager::textPrimary(), 18));
    m_playBtn->setToolTip(QStringLiteral("播放 / 暂停 (空格)"));
    connect(m_playBtn, &QPushButton::clicked, this, &PreviewDialog::onPlayPauseClicked);
    playbackLayout->addWidget(m_playBtn);

    m_stopBtn = new QPushButton(m_playbackBar);
    m_stopBtn->setProperty("iconButton", true);
    m_stopBtn->setIcon(Icons::get(Icons::Stop, ThemeManager::textPrimary(), 16));
    m_stopBtn->setToolTip(QStringLiteral("停止"));
    connect(m_stopBtn, &QPushButton::clicked, this, &PreviewDialog::onStopClicked);
    playbackLayout->addWidget(m_stopBtn);

    playbackLayout->addSpacing(8);

    // 进度
    m_positionLabel = new QLabel(QStringLiteral("00:00"), m_playbackBar);
    m_positionLabel->setStyleSheet(QStringLiteral("color: %1;")
                                       .arg(ThemeManager::textSecondary().name()));
    m_positionLabel->setMinimumWidth(42);
    playbackLayout->addWidget(m_positionLabel);

    m_positionSlider = new QSlider(Qt::Horizontal, m_playbackBar);
    m_positionSlider->setRange(0, 0);
    m_positionSlider->setToolTip(QStringLiteral("播放进度"));
    connect(m_positionSlider, &QSlider::sliderPressed,
            this, &PreviewDialog::onPositionSliderPressed);
    connect(m_positionSlider, &QSlider::sliderReleased,
            this, &PreviewDialog::onPositionSliderReleased);
    connect(m_positionSlider, &QSlider::sliderMoved,
            this, &PreviewDialog::onPositionSliderMoved);
    playbackLayout->addWidget(m_positionSlider, 1);

    m_durationLabel = new QLabel(QStringLiteral("00:00"), m_playbackBar);
    m_durationLabel->setStyleSheet(QStringLiteral("color: %1;")
                                       .arg(ThemeManager::textSecondary().name()));
    m_durationLabel->setMinimumWidth(42);
    playbackLayout->addWidget(m_durationLabel);

    playbackLayout->addSpacing(8);

    // 音量
    m_muteBtn = new QPushButton(m_playbackBar);
    m_muteBtn->setProperty("iconButton", true);
    m_muteBtn->setIcon(Icons::get(Icons::VolumeUp, ThemeManager::textPrimary(), 17));
    m_muteBtn->setToolTip(QStringLiteral("静音"));
    connect(m_muteBtn, &QPushButton::clicked, this, [this]() {
        if (!m_audioOutput)
            return;
        m_audioOutput->setMuted(!m_audioOutput->isMuted());
        onMuteToggled(m_audioOutput->isMuted());
    });
    playbackLayout->addWidget(m_muteBtn);

    m_volumeSlider = new QSlider(Qt::Horizontal, m_playbackBar);
    m_volumeSlider->setRange(0, 100);
    m_volumeSlider->setFixedWidth(84);
    m_volumeSlider->setToolTip(QStringLiteral("音量"));
    playbackLayout->addWidget(m_volumeSlider);

    controlLayout->addWidget(m_playbackBar);

    // 右侧弹簧：把图片工具推到控制栏尾部
    controlLayout->addStretch(1);

    // ---------- 图片工具组（仅图片） ----------
    m_imageBar = new QWidget(controlBar);
    auto *imageLayout = new QHBoxLayout(m_imageBar);
    imageLayout->setContentsMargins(0, 0, 0, 0);
    imageLayout->setSpacing(8);

    m_zoomOutBtn = new QPushButton(m_imageBar);
    m_zoomOutBtn->setProperty("iconButton", true);
    m_zoomOutBtn->setIcon(Icons::get(Icons::ZoomOut, ThemeManager::textPrimary(), 17));
    m_zoomOutBtn->setToolTip(QStringLiteral("缩小 (滚轮 / -)"));
    connect(m_zoomOutBtn, &QPushButton::clicked, this, [this]() { m_imageView->zoomBy(1.0 / 1.25); });
    imageLayout->addWidget(m_zoomOutBtn);

    m_scaleLabel = new QLabel(QStringLiteral("100%"), m_imageBar);
    m_scaleLabel->setStyleSheet(QStringLiteral("color: %1;")
                                    .arg(ThemeManager::textSecondary().name()));
    m_scaleLabel->setMinimumWidth(46);
    m_scaleLabel->setAlignment(Qt::AlignCenter);
    imageLayout->addWidget(m_scaleLabel);

    m_zoomInBtn = new QPushButton(m_imageBar);
    m_zoomInBtn->setProperty("iconButton", true);
    m_zoomInBtn->setIcon(Icons::get(Icons::ZoomIn, ThemeManager::textPrimary(), 17));
    m_zoomInBtn->setToolTip(QStringLiteral("放大 (滚轮 / +)"));
    connect(m_zoomInBtn, &QPushButton::clicked, this, [this]() { m_imageView->zoomBy(1.25); });
    imageLayout->addWidget(m_zoomInBtn);

    m_fitBtn = new QPushButton(m_imageBar);
    m_fitBtn->setProperty("iconButton", true);
    m_fitBtn->setIcon(Icons::get(Icons::FitScreen, ThemeManager::textPrimary(), 17));
    m_fitBtn->setToolTip(QStringLiteral("适应窗口 (0)"));
    connect(m_fitBtn, &QPushButton::clicked, this, [this]() { m_imageView->fitInView(); });
    imageLayout->addWidget(m_fitBtn);

    m_originalBtn = new QPushButton(m_imageBar);
    m_originalBtn->setProperty("iconButton", true);
    m_originalBtn->setIcon(Icons::get(Icons::Rotate, ThemeManager::textPrimary(), 17));
    m_originalBtn->setToolTip(QStringLiteral("原始尺寸 100% (1)"));
    connect(m_originalBtn, &QPushButton::clicked, this, [this]() { m_imageView->zoomToOriginal(); });
    imageLayout->addWidget(m_originalBtn);

    controlLayout->addWidget(m_imageBar);

    root->addWidget(controlBar);

    // 音量滑块的初值由构造函数里的 SettingsManager 负责（见下方 setVolume），
    // 这里不再写死 80，否则设置界面里的默认音量对预览窗口无效。
    connect(m_volumeSlider, &QSlider::valueChanged,
            this, &PreviewDialog::onVolumeChanged);

    // 初始状态：没有任何媒体，两组控制都收起，只留翻页
    setControlBarsVisible(MediaType::Image);
    setControlsEnabled(false);
}

void PreviewDialog::setControlsEnabled(bool enabled)
{
    if (m_playBtn) m_playBtn->setEnabled(enabled);
    if (m_stopBtn) m_stopBtn->setEnabled(enabled);
    if (m_positionSlider) m_positionSlider->setEnabled(enabled);
}

/*!
 * 按媒体类型切换控制栏分组显隐。
 *
 * 图片只显示图片工具组，视频/音频只显示播放控制组，翻页组始终保留
 * （上一张/下一张对三类媒体都有意义）。
 *
 * 这个函数只改 UI 可见性，不碰播放器状态：
 * 播放器的启停在 openItem / showVideo / showAudio 里显式完成，
 * 这里再插一手 pause 会让"谁负责停机"变得含糊。
 */
void PreviewDialog::setControlBarsVisible(MediaType type)
{
    const bool imageMode = (type == MediaType::Image);

    if (m_playbackBar)
        m_playbackBar->setVisible(!imageMode);
    if (m_imageBar)
        m_imageBar->setVisible(imageMode);
}

void PreviewDialog::openItem(const MediaItem &item)
{
    if (!item.isValid())
        return;

    stopPlayback();

    m_current = item;
    updateTitle();

    m_infoLabel->clear();

    switch (item.type) {
    case MediaType::Image:
        showImage(item);
        break;
    case MediaType::Video:
        showVideo(item);
        break;
    case MediaType::Audio:
        showAudio(item);
        break;
    default:
        m_infoLabel->setText(QStringLiteral("不支持的媒体类型"));
        break;
    }

    updateNavigationButtons();
    emit itemChanged(item);
}

void PreviewDialog::showImage(const MediaItem &item)
{
    m_stack->setCurrentWidget(m_imageView);

    /*!
     * 整组切换显隐：图片页不出现音量滑块、进度条、播放按钮中的任何一个。
     *
     * 播放组整组隐藏，所以不需要再对 m_playBtn / m_stopBtn / m_positionSlider
     * 做 setEnabled —— 那是在为"不可见的控件维护状态"，属于多余操作。
     * 空格快捷键也由下方 keyPressEvent 按 m_current.type 拦截。
     */
    setControlBarsVisible(MediaType::Image);

    if (!m_imageView->loadImage(item.path)) {
        // loadImage 已经发过 loadFailed，这里补一句路径信息方便定位
        m_infoLabel->setText(m_infoLabel->text().isEmpty()
                                 ? QStringLiteral("无法打开图片")
                                 : m_infoLabel->text());
    }
}

void PreviewDialog::showVideo(const MediaItem &item)
{
    m_stack->setCurrentWidget(m_videoWidget);

    setControlBarsVisible(MediaType::Video);
    setControlsEnabled(true);

    if (!m_player)
        return;

    /*!
     * 播放前先重置进度显示。
     *
     * setSource 之后 durationChanged/positionChanged 是异步到达的，
     * 在它们到达之前，进度条与时间标签仍显示上一个媒体的值
     * （可能是一个长视频的 01:23:45），用户会看到进度条闪一下旧内容。
     */
    m_positionSlider->setRange(0, 0);
    m_positionSlider->setValue(0);
    m_positionLabel->setText(QStringLiteral("00:00"));
    m_durationLabel->setText(QStringLiteral("00:00"));
    m_sliderDragging = false;

    m_player->setVideoOutput(m_videoWidget);
    m_player->setSource(QUrl::fromLocalFile(item.path));

    // 从上次位置续播：对长视频很有用。
    // 设置里关掉"记住播放位置并续播"时不再恢复位置，每次从 0 开始。
    if (m_resumePlayback && m_database && item.id > 0) {
        const double last = m_database->lastPosition(item.id);
        if (last > 3000 && (item.durationMs <= 0 || last < item.durationMs - 5000))
            m_player->setPosition(static_cast<qint64>(last));
    }

    m_player->play();
    updatePlayButton();
}

void PreviewDialog::showAudio(const MediaItem &item)
{
    m_stack->setCurrentWidget(m_audioPanel);

    if (m_audioArtwork) {
        // 没有解析音频封面（需要 ID3 解析库），用大号音符图标代替
        const QPixmap art = Icons::pixmap(Icons::Audio, ThemeManager::typeColorAudio(), 160);
        m_audioArtwork->setPixmap(art);
    }

    setControlBarsVisible(MediaType::Audio);
    setControlsEnabled(true);

    // 与 showVideo 同样：换曲目时清掉上一条的进度显示
    m_positionSlider->setRange(0, 0);
    m_positionSlider->setValue(0);
    m_positionLabel->setText(QStringLiteral("00:00"));
    m_durationLabel->setText(QStringLiteral("00:00"));
    m_sliderDragging = false;

    if (!m_player)
        return;

    // 音频不该有视频输出，否则会在视频窗口上显示黑屏
    m_player->setVideoOutput(nullptr);
    m_player->setSource(QUrl::fromLocalFile(item.path));
    m_player->play();
    updatePlayButton();
}

void PreviewDialog::stopPlayback()
{
    if (m_player) {
        // 先记录最终位置再停，否则用户关窗口时的进度会丢
        if (m_current.id > 0 && m_player->playbackState() != QMediaPlayer::StoppedState)
            emit playbackRecorded(m_current.id, m_player->position());

        m_player->stop();
        m_player->setSource(QUrl());
        m_player->setVideoOutput(nullptr);
    }

    if (m_imageView)
        m_imageView->clearImage();
}

void PreviewDialog::setPlaylist(const QList<MediaItem> &items, int currentIndex)
{
    m_playlist = items;
    m_playlistIndex = currentIndex;
    updateNavigationButtons();
}

void PreviewDialog::onPrevious()
{
    if (m_playlist.isEmpty())
        return;

    // 循环播放：到最后一张再点下一个回到开头。
    // 比禁用按钮更符合"翻看"的心理预期。
    int idx = m_playlistIndex - 1;
    if (idx < 0)
        idx = m_playlist.size() - 1;

    m_playlistIndex = idx;
    openItem(m_playlist.at(idx));
}

void PreviewDialog::onNext()
{
    if (m_playlist.isEmpty())
        return;

    int idx = m_playlistIndex + 1;
    if (idx >= m_playlist.size())
        idx = 0;

    m_playlistIndex = idx;
    openItem(m_playlist.at(idx));
}

void PreviewDialog::updateNavigationButtons()
{
    const bool multi = m_playlist.size() > 1;
    if (m_prevBtn) m_prevBtn->setEnabled(multi);
    if (m_nextBtn) m_nextBtn->setEnabled(multi);
}

void PreviewDialog::updateTitle()
{
    if (!m_current.isValid()) {
        m_titleLabel->setText(QString());
        return;
    }

    m_titleLabel->setText(m_current.fileName);

    QStringList parts;
    parts << mediaTypeName(m_current.type);
    if (m_current.width > 0 && m_current.height > 0)
        parts << QStringLiteral("%1 × %2").arg(m_current.width).arg(m_current.height);
    parts << formatSize(m_current.size);

    if (m_playlist.size() > 1 && m_playlistIndex >= 0)
        parts << QStringLiteral("%1 / %2").arg(m_playlistIndex + 1).arg(m_playlist.size());

    m_titleLabel->setToolTip(m_current.path);
}

void PreviewDialog::onPlayPauseClicked()
{
    if (!m_player || !m_current.isValid())
        return;

    if (m_current.type == MediaType::Image)
        return;

    if (m_player->playbackState() == QMediaPlayer::PlayingState)
        m_player->pause();
    else
        m_player->play();

    updatePlayButton();
}

void PreviewDialog::onStopClicked()
{
    if (!m_player)
        return;

    m_player->stop();

    if (m_current.type != MediaType::Image) {
        // 停止后重新装载，这样再点播放能从头发起
        m_player->setSource(QUrl::fromLocalFile(m_current.path));
    }

    updatePlayButton();
}

void PreviewDialog::updatePlayButton()
{
    if (!m_playBtn || !m_player)
        return;

    const bool playing = m_player->playbackState() == QMediaPlayer::PlayingState;
    m_playBtn->setIcon(Icons::get(playing ? Icons::Pause : Icons::Play,
                                  ThemeManager::textPrimary(), 18));
}

void PreviewDialog::onPositionSliderPressed()
{
    // 用户开始拖动，暂停位置回写，否则滑块会被播放进度不断拽走
    m_sliderDragging = true;
}

void PreviewDialog::onPositionSliderMoved(int value)
{
    // 拖动过程中实时更新左侧时间显示，给用户反馈
    m_positionLabel->setText(formatMs(value));
}

void PreviewDialog::onPositionSliderReleased()
{
    if (!m_player)
        return;

    m_player->setPosition(m_positionSlider->value());
    m_sliderDragging = false;
}

void PreviewDialog::onDurationChanged(qint64 durationMs)
{
    if (!m_positionSlider)
        return;

    // QSlider 用 int，超过 int 范围的长媒体（> 24 天）会被截断，
    // 实际中不存在这种媒体，这里不做额外处理，但保持类型正确。
    m_positionSlider->setRange(0, static_cast<int>(qMin(durationMs, qint64(INT_MAX))));

    if (m_durationLabel)
        m_durationLabel->setText(formatMs(durationMs));

    // 时长信息回写到库，下次在网格里就能看到角标
    if (m_database && m_current.id > 0 && durationMs > 0 && m_current.durationMs <= 0) {
        m_current.durationMs = static_cast<double>(durationMs);
        m_database->setDimensions(m_current.id, m_current.width, m_current.height, m_current.durationMs);
    }
}

void PreviewDialog::onPositionChanged(qint64 positionMs)
{
    if (m_sliderDragging)
        return;   // 拖动期间不覆盖用户的操作

    if (m_positionSlider)
        m_positionSlider->setValue(static_cast<int>(positionMs));

    if (m_positionLabel)
        m_positionLabel->setText(formatMs(positionMs));
}

void PreviewDialog::onMediaStatusChanged(int status)
{
    const auto mediaStatus = static_cast<QMediaPlayer::MediaStatus>(status);

    switch (mediaStatus) {
    case QMediaPlayer::InvalidMedia:
        m_infoLabel->setText(QStringLiteral("无法播放该文件（格式不受支持或文件已损坏）"));
        break;
    case QMediaPlayer::LoadedMedia:
        m_infoLabel->clear();
        break;
    case QMediaPlayer::EndOfMedia:
        updatePlayButton();

        /*!
         * 播完自动切下一个，符合"连续观看"的预期。
         *
         * 受设置项 playback/autoPlayNext 控制。之前这个设置项只被写、从未被读，
         * 用户取消勾选"播放结束后自动播放下一个"后行为没有任何变化。
         * 只在有播放列表且不止一项时才切。
         */
        if (m_autoPlayNext && m_playlist.size() > 1)
            onNext();
        break;
    default:
        break;
    }
}

void PreviewDialog::onVolumeChanged(int value)
{
    if (m_audioOutput)
        m_audioOutput->setVolume(value / 100.0);

    if (m_muteBtn) {
        const bool muted = value == 0;
        m_muteBtn->setIcon(Icons::get(muted ? Icons::VolumeMute : Icons::VolumeUp,
                                      ThemeManager::textPrimary(), 17));
    }
}

void PreviewDialog::onMuteToggled(bool muted)
{
    if (m_muteBtn)
        m_muteBtn->setIcon(Icons::get(muted ? Icons::VolumeMute : Icons::VolumeUp,
                                      ThemeManager::textPrimary(), 17));
}

QString PreviewDialog::formatMs(qint64 ms)
{
    if (ms <= 0)
        return QStringLiteral("00:00");

    const qint64 totalSec = ms / 1000;
    const qint64 h = totalSec / 3600;
    const qint64 m = (totalSec % 3600) / 60;
    const qint64 s = totalSec % 60;

    if (h > 0) {
        return QStringLiteral("%1:%2:%3")
            .arg(h)
            .arg(m, 2, 10, QLatin1Char('0'))
            .arg(s, 2, 10, QLatin1Char('0'));
    }

    return QStringLiteral("%1:%2")
        .arg(m, 2, 10, QLatin1Char('0'))
        .arg(s, 2, 10, QLatin1Char('0'));
}

void PreviewDialog::keyPressEvent(QKeyEvent *event)
{
    switch (event->key()) {
    case Qt::Key_Escape:
        close();
        return;
    case Qt::Key_Space:
        /*!
         * 空格只对视频/音频有意义。
         *
         * 图片页不该有播放语义：即使播放控制组已经整组隐藏，
         * 键盘事件仍会到达对话框，不拦的话空格会去驱动 QMediaPlayer，
         * 表现为"看图时按空格，后台播放器状态变了"。
         */
        if (m_current.type == MediaType::Image)
            return;
        onPlayPauseClicked();
        return;
    case Qt::Key_Left:
        onPrevious();
        return;
    case Qt::Key_Right:
        onNext();
        return;
    default:
        break;
    }

    QDialog::keyPressEvent(event);
}

void PreviewDialog::closeEvent(QCloseEvent *event)
{
    // 关窗前必须停播放并记录位置。
    // QMediaPlayer 在播放中析构会有异步等待，直接把窗口连同播放器一起销毁会崩。
    stopPlayback();
    QDialog::closeEvent(event);
}

} // namespace media
