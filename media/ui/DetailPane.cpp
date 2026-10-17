#include "DetailPane.h"
#include "Metrics.h"
#include "Icons.h"
#include "ThemeManager.h"
#include "../core/MediaDatabase.h"
#include "../tasks/ThumbnailTask.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QScrollArea>
#include <QLineEdit>
#include <QPushButton>
#include <QToolButton>
#include <QFrame>
#include <QPainter>
#include <QPainterPath>
#include <QFileInfo>
#include <QDir>
#include <QFile>
#include <QDesktopServices>
#include <QUrl>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QDebug>

namespace media {

namespace {

/*! 详情面板里统一的"键"标签样式。 */
QLabel *makeKeyLabel(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("DetailKey"));
    label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    label->setMinimumWidth(64);
    return label;
}

/*! 节标题（"基本信息"、"文件信息"…）。 */
QLabel *makeSectionTitle(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("DetailSectionTitle"));
    return label;
}

} // namespace

DetailPane::DetailPane(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("DetailPane"));
    setMinimumWidth(Metrics::Panel::DetailMin);
    setMaximumWidth(Metrics::Panel::DetailMax);

    buildUi();
    clear();
}

DetailPane::~DetailPane() = default;

void DetailPane::setDatabase(MediaDatabase *database)
{
    m_database = database;
}

void DetailPane::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // 空状态：没有选中任何东西时显示
    m_placeholder = new QLabel(QStringLiteral("未选择资源"), this);
    m_placeholder->setAlignment(Qt::AlignCenter);
    m_placeholder->setWordWrap(true);
    m_placeholder->setStyleSheet(
        QStringLiteral("color: %1; font-size: 13px; padding: 40px 20px;")
            .arg(ThemeManager::textDisabled().name()));
    root->addWidget(m_placeholder);

    // 内容区（可滚动，因为元数据行数不定）
    m_scroll = new QScrollArea(this);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    m_content = new QWidget();
    auto *contentLayout = new QVBoxLayout(m_content);
    contentLayout->setContentsMargins(14, 14, 14, 14);
    contentLayout->setSpacing(8);

    // ---------- 预览 ----------
    m_preview = new QLabel(m_content);
    m_preview->setObjectName(QStringLiteral("DetailPreview"));
    m_preview->setAlignment(Qt::AlignCenter);
    m_preview->setMinimumHeight(160);
    m_preview->setMaximumHeight(240);
    m_preview->setCursor(Qt::PointingHandCursor);
    m_preview->setToolTip(QStringLiteral("双击打开预览"));
    // 用事件过滤器捕双击：QLabel 没有 doubleClicked 信号
    m_preview->installEventFilter(this);
    contentLayout->addWidget(m_preview);

    // ---------- 文件名 + 收藏 ----------
    auto *titleRow = new QHBoxLayout();
    titleRow->setSpacing(8);

    m_fileName = new QLabel(m_content);
    m_fileName->setObjectName(QStringLiteral("DetailFileName"));
    m_fileName->setWordWrap(true);
    m_fileName->setTextInteractionFlags(Qt::TextSelectableByMouse);
    titleRow->addWidget(m_fileName, 1);

    m_favoriteBtn = new QPushButton(m_content);
    m_favoriteBtn->setProperty("iconButton", true);
    m_favoriteBtn->setCheckable(true);
    m_favoriteBtn->setCursor(Qt::PointingHandCursor);
    m_favoriteBtn->setToolTip(QStringLiteral("收藏 / 取消收藏"));
    connect(m_favoriteBtn, &QPushButton::clicked, this, &DetailPane::onFavoriteClicked);
    titleRow->addWidget(m_favoriteBtn);

    contentLayout->addLayout(titleRow);

    // 类型徽标
    m_typeBadge = new QLabel(m_content);
    m_typeBadge->setProperty("tag", true);
    m_typeBadge->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    auto *badgeRow = new QHBoxLayout();
    badgeRow->addWidget(m_typeBadge);
    badgeRow->addStretch();
    contentLayout->addLayout(badgeRow);

    // ---------- 元数据表 ----------
    contentLayout->addWidget(makeSectionTitle(QStringLiteral("文件信息"), m_content));

    m_infoGrid = new QGridLayout();
    m_infoGrid->setHorizontalSpacing(10);
    m_infoGrid->setVerticalSpacing(6);
    m_infoGrid->setColumnStretch(1, 1);
    contentLayout->addLayout(m_infoGrid);

    // ---------- 标签 ----------
    contentLayout->addWidget(makeSectionTitle(QStringLiteral("标签"), m_content));

    m_tagArea = new QWidget(m_content);
    m_tagLayout = new QVBoxLayout(m_tagArea);
    m_tagLayout->setContentsMargins(0, 0, 0, 0);
    m_tagLayout->setSpacing(6);
    contentLayout->addWidget(m_tagArea);

    /*!
     * 新增标签的输入框。
     *
     * 之前它就是一个裸 QLineEdit，用全局输入控件样式：圆角 7px、内边距 6px 10px，
     * 和上方 chip（圆角 11px、高 24px）明显是两个视觉体系，放在一起很跳。
     * 现在给它单独的对象名与胶囊外形，让"新增"和"已有标签"看起来是一组东西，
     * 而不是从别处搬来的一个表单输入框。
     */
    m_tagInput = new QLineEdit(m_content);
    m_tagInput->setObjectName(QStringLiteral("DetailTagInput"));
    m_tagInput->setFixedHeight(28);
    m_tagInput->setPlaceholderText(QStringLiteral("+ 添加标签"));
    m_tagInput->setToolTip(QStringLiteral("输入标签名称后回车添加"));
    m_tagInput->setClearButtonEnabled(true);
    connect(m_tagInput, &QLineEdit::returnPressed,
            this, &DetailPane::onTagInputReturnPressed);
    contentLayout->addWidget(m_tagInput);

    contentLayout->addStretch();

    m_scroll->setWidget(m_content);
    root->addWidget(m_scroll, 1);
}

bool DetailPane::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_preview && event->type() == QEvent::MouseButtonDblClick) {
        onPreviewDoubleClicked();
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

void DetailPane::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);

    /*!
     * chip 的换行位置依赖标签区宽度，宽度变了要重排，
     * 否则拖宽面板后 chip 仍按旧宽度换行，右侧留白越来越不对称。
     * 12px 死区挡住拖动过程的连续触发，避免逐像素重建控件。
     */
    if (!m_tagArea || !m_tagArea->isVisible())
        return;

    const int w = m_tagArea->width();
    if (qAbs(w - m_lastChipRebuildWidth) < 12)
        return;

    rebuildTagChips();
}

void DetailPane::onPreviewDoubleClicked()
{
    if (m_current.isValid())
        emit playRequested(m_current);
}

void DetailPane::setRowValue(QLabel *valueLabel, const QString &text)
{
    if (valueLabel)
        valueLabel->setText(text.isEmpty() ? QStringLiteral("--") : text);
}

void DetailPane::clearGrid()
{
    // 删掉旧的行。QGridLayout 里的 widget 需要手动 deleteLater，
    // 只 clearLayout 不会销毁控件。
    while (QLayoutItem *item = m_infoGrid->takeAt(0)) {
        if (QWidget *w = item->widget())
            w->deleteLater();
        delete item;
    }
    m_valueLabels.clear();
}

void DetailPane::clear()
{
    m_current = MediaItem();
    m_currentTags.clear();

    m_placeholder->setVisible(true);
    m_scroll->setVisible(false);
}

void DetailPane::showMultiSelection(int count, qint64 totalBytes)
{
    m_current = MediaItem();
    m_currentTags.clear();

    m_placeholder->setText(QStringLiteral("已选择 %1 项\n合计 %2")
                               .arg(count)
                               .arg(formatSize(totalBytes)));
    m_placeholder->setVisible(true);
    m_scroll->setVisible(false);
}

void DetailPane::showItem(const MediaItem &item)
{
    if (!item.isValid()) {
        clear();
        return;
    }

    m_current = item;

    m_placeholder->setVisible(false);
    m_scroll->setVisible(true);

    // ---------- 预览 ----------
    // 这里先放占位图，真实缩略图由 MainWindow 通过 setPreviewImage 回填。
    // 面板不等缩略图，立刻显示出来，避免切换选中时界面"卡一下"。
    // makePlaceholder 返回 QImage（它在工作线程里也会被调用），
    // 这里在 GUI 线程转成 QPixmap 再交给 QLabel。
    const QPixmap ph = QPixmap::fromImage(ThumbnailTask::makePlaceholder(item.type, 200));
    m_preview->setPixmap(ph.scaled(m_preview->size(), Qt::KeepAspectRatio,
                                   Qt::SmoothTransformation));

    // ---------- 标题 ----------
    m_fileName->setText(item.fileName);

    m_favoriteBtn->setChecked(item.favorite);
    m_favoriteBtn->setIcon(Icons::get(item.favorite ? Icons::StarFilled : Icons::Star,
                                      item.favorite ? ThemeManager::favoriteColor()
                                                    : ThemeManager::textSecondary(),
                                      17));

    // ---------- 类型徽标 ----------
    QColor typeColor;
    switch (item.type) {
    case MediaType::Image: typeColor = ThemeManager::typeColorImage(); break;
    case MediaType::Video: typeColor = ThemeManager::typeColorVideo(); break;
    case MediaType::Audio: typeColor = ThemeManager::typeColorAudio(); break;
    default:               typeColor = ThemeManager::textDisabled(); break;
    }
    m_typeBadge->setText(mediaTypeName(item.type));
    m_typeBadge->setStyleSheet(
        QStringLiteral("QLabel { background-color: rgba(%1,%2,%3,60); "
                       "border: 1px solid rgba(%1,%2,%3,140); "
                       "border-radius: 10px; padding: 2px 10px; color: %4; font-size: 12px; }")
            .arg(typeColor.red()).arg(typeColor.green()).arg(typeColor.blue())
            .arg(typeColor.name()));

    // ---------- 元数据 ----------
    clearGrid();

    int row = 0;
    auto addRow = [&](const QString &key, const QString &value) {
        QLabel *k = makeKeyLabel(key, m_content);
        QLabel *v = new QLabel(m_content);
        v->setObjectName(QStringLiteral("DetailValue"));
        v->setWordWrap(true);
        v->setTextInteractionFlags(Qt::TextSelectableByMouse);

        // 路径这类长文本要能选中复制，但不该撑破面板宽度
        v->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

        m_infoGrid->addWidget(k, row, 0, Qt::AlignRight | Qt::AlignTop);
        m_infoGrid->addWidget(v, row, 1, Qt::AlignLeft | Qt::AlignTop);

        setRowValue(v, value);
        m_valueLabels.append(v);
        ++row;
    };

    // 格式：扩展名大写。没有扩展名时如实说明，不猜类型。
    const QString suffix = QFileInfo(item.path).suffix().toUpper();
    addRow(QStringLiteral("格式"),
           suffix.isEmpty() ? QStringLiteral("无扩展名") : suffix);

    if (item.width > 0 && item.height > 0)
        addRow(QStringLiteral("分辨率"), QStringLiteral("%1 × %2").arg(item.width).arg(item.height));
    else if (item.type == MediaType::Image)
        addRow(QStringLiteral("分辨率"), QStringLiteral("未读取或文件损坏"));

    if (item.type == MediaType::Video || item.type == MediaType::Audio) {
        addRow(QStringLiteral("时长"),
               item.durationMs >= 0 ? formatDuration(item.durationMs)
                                    : QStringLiteral("未知（需解析媒体头）"));
    }

    addRow(QStringLiteral("大小"), formatSize(item.size));
    addRow(QStringLiteral("修改时间"),
           item.modified.isValid() ? item.modified.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
                                   : QStringLiteral("--"));

    if (item.takenAt.isValid() && item.takenAt != item.modified)
        addRow(QStringLiteral("拍摄时间"),
               item.takenAt.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));

    const QString camera = [&item]() {
        const QString make = item.cameraMake.trimmed();
        const QString model = item.cameraModel.trimmed();
        if (make.isEmpty() && model.isEmpty())
            return QString();
        if (make.isEmpty())
            return model;
        return QStringLiteral("%1 %2").arg(make, model);
    }();
    if (!camera.isEmpty())
        addRow(QStringLiteral("设备"), camera);

    if (!item.contentHash.isEmpty())
        addRow(QStringLiteral("内容哈希"), item.contentHash.left(16) + QStringLiteral("…"));

    if (m_database && item.id > 0) {
        const int plays = m_database->playCount(item.id);
        if (plays > 0)
            addRow(QStringLiteral("播放次数"), QString::number(plays));
    }

    addRow(QStringLiteral("所在目录"), item.folderPath);
    addRow(QStringLiteral("完整路径"), item.path);

    if (item.missing)
        addRow(QStringLiteral("状态"), QStringLiteral("文件已失效（可能已被移动或删除）"));

    // ---------- 标签 ----------
    m_currentTags = item.id > 0 && m_database
                        ? m_database->tagsForItem(item.id)
                        : QStringList();
    rebuildTagChips();
}

void DetailPane::rebuildTagChips()
{
    // 先记录本次重排依据的宽度。必须在开头记录：无标签分支会提前 return，
    // 放在函数末尾的话空列表状态下 resizeEvent 判定永远不命中，
    // 拖动面板时会逐次触发重建。
    m_lastChipRebuildWidth = m_tagArea->width();

    /*!
     * 清空旧的 chip。
     *
     * 必须递归清理：chip 不是直接放进 m_tagLayout 的，而是先塞进一个
     * QHBoxLayout 再 addLayout()。只判断 item->widget() 的话，
     * 这些嵌套布局永远取不到、里面的 QPushButton 也就永远不会被销毁——
     * 它们的父对象是 m_tagArea，会一直留在界面上。
     * 表现就是：点 × 删掉标签，标签还在原地。
     */
    clearTagLayout();

    if (m_currentTags.isEmpty()) {
        // 空状态与 chip 同字号，避免提示文字比标签本身更显眼
        auto *empty = new QLabel(QStringLiteral("暂无标签"), m_tagArea);
        empty->setObjectName(QStringLiteral("DetailTagEmpty"));
        empty->setFixedHeight(24);
        m_tagLayout->addWidget(empty);
        return;
    }

    /*!
     * 每个 chip 是「标签文字 + 一个小 ×」，放在一个横向流式布局里。
     * 用行包装的方式模拟流式布局（Qt 没有内置 FlowLayout）。
     */
    QHBoxLayout *currentRow = nullptr;
    int usedWidth = 0;
    /*!
     * 换行宽度取标签区的实际宽度，不再写死 250。
     *
     * 写死的后果：面板拖宽后 chip 仍按 250 换行，右侧空出一大块，
     * 看起来像"位置没对齐"；面板比 250 窄时则会被挤出可视区。
     * 首次构建（布局还没跑、宽度为 0）退回 250，与旧行为一致。
     */
    int availableWidth = m_tagArea->width() - 2;
    if (availableWidth < 60)
        availableWidth = 250;

    for (const QString &tag : m_currentTags) {
        /*!
         * 按显示宽度估算 chip 占位，不能用字符数乘一个固定值：
         * 中文字形宽度约为拉丁字母的两倍，一律按 8px 算会让含中文的标签
         * 估算过窄，chip 被布局挤压、文字被截断。
         * 这里逐字区分：CJK 记 12px，其余记 7px，再加上 ×、内边距与边框。
         */
        int textWidth = 0;
        for (const QChar ch : tag) {
            const ushort u = ch.unicode();
            const bool cjk = (u >= 0x2E80 && u <= 0x9FFF)
                          || (u >= 0xF900 && u <= 0xFAFF)
                          || (u >= 0xFF00 && u <= 0xFF60);
            textWidth += cjk ? 12 : 7;
        }
        // × 图标与间距 + 左右内边距 + 边框
        const int chipWidth = textWidth + 34;

        if (!currentRow || usedWidth + chipWidth > availableWidth) {
            currentRow = new QHBoxLayout();
            currentRow->setSpacing(6);
            currentRow->setContentsMargins(0, 0, 0, 0);
            m_tagLayout->addLayout(currentRow);
            usedWidth = 0;
        }

        /*!
         * chip = 一个胶囊按钮 + 一个独立的 × 小按钮。
         *
         * 之前是把 "标签　×" 拼成一个字符串塞进单个 QPushButton：
         * 点文字也会删标签，误触概率高；而且整块按钮 hover 时变成红色实心，
         * 一个标签变红很扎眼。现在文字部分只负责显示，× 单独可点，
         * hover 只让 × 变红。
         */
        auto *chip = new QWidget(m_tagArea);
        chip->setProperty("chip", true);
        chip->setFixedHeight(24);
        chip->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);

        auto *chipLayout = new QHBoxLayout(chip);
        chipLayout->setContentsMargins(9, 0, 4, 0);
        chipLayout->setSpacing(2);

        auto *chipText = new QLabel(tag, chip);
        chipText->setProperty("chipText", true);
        chipText->setAttribute(Qt::WA_TransparentForMouseEvents);
        chipLayout->addWidget(chipText);

        auto *closeBtn = new QToolButton(chip);
        closeBtn->setProperty("chipClose", true);
        closeBtn->setCursor(Qt::PointingHandCursor);
        closeBtn->setToolTip(QStringLiteral("移除标签「%1」").arg(tag));
        closeBtn->setIcon(Icons::get(Icons::Close, ThemeManager::textDisabled(), 11));
        closeBtn->setIconSize(QSize(11, 11));
        closeBtn->setFixedSize(16, 16);
        closeBtn->setAutoRaise(true);

        const QString tagCopy = tag;
        connect(closeBtn, &QToolButton::clicked, this, [this, tagCopy]() {
            if (m_current.id > 0)
                emit tagRemoved(m_current.id, tagCopy);
        });

        chipLayout->addWidget(closeBtn);

        currentRow->addWidget(chip);
        usedWidth += chipWidth + 6;
    }

    if (currentRow)
        currentRow->addStretch();
}

void DetailPane::clearTagLayout()
{
    while (QLayoutItem *item = m_tagLayout->takeAt(0)) {
        // 嵌套布局（chip 的整行）要递归拆：只处理 widget 会漏掉它们，
        // 里面的按钮因此永远不会被销毁，删掉标签后仍留在界面上。
        if (QLayout *child = item->layout()) {
            while (QLayoutItem *sub = child->takeAt(0)) {
                if (QWidget *w = sub->widget())
                    w->deleteLater();
                delete sub;
            }
            delete child;
        }

        if (QWidget *w = item->widget())
            w->deleteLater();

        delete item;
    }
}

void DetailPane::onTagInputReturnPressed()
{
    const QString tag = m_tagInput->text().trimmed();
    if (tag.isEmpty() || m_current.id <= 0)
        return;

    m_tagInput->clear();

    if (m_currentTags.contains(tag, Qt::CaseInsensitive))
        return;   // 已经有了，不重复添加

    m_currentTags.append(tag);
    rebuildTagChips();

    emit tagAdded(m_current.id, tag);
}

void DetailPane::onFavoriteClicked()
{
    if (m_current.id <= 0)
        return;

    const bool newState = m_favoriteBtn->isChecked();
    m_current.favorite = newState;

    m_favoriteBtn->setIcon(Icons::get(newState ? Icons::StarFilled : Icons::Star,
                                      newState ? ThemeManager::favoriteColor()
                                               : ThemeManager::textSecondary(),
                                      17));

    emit favoriteToggled(m_current.id, newState);
}

void DetailPane::setPreviewImage(const QImage &image)
{
    if (image.isNull() || !m_scroll->isVisible())
        return;

    // 等比缩放到预览区大小，保留白色/深色边距让图居中
    const QSize target = m_preview->size();
    if (target.isEmpty())
        return;

    // 在 GUI 线程（本方法由 MainWindow 的槽调用）完成 QImage → QPixmap。
    // 先缩再转：QPixmap::fromImage 的代价与像素数量成正比，
    // 先缩到预览区大小可以少上传大量像素。
    const QImage scaled = image.scaled(target, Qt::KeepAspectRatio,
                                       Qt::SmoothTransformation);
    m_preview->setPixmap(QPixmap::fromImage(scaled));
}

} // namespace media
