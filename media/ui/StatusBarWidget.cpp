#include "StatusBarWidget.h"
#include "../core/ThumbnailCache.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QTimer>

namespace media {

namespace {

/*! 缓存统计的刷新间隔。这些计数变化很慢，没必要每帧重算。 */
constexpr int kCacheRefreshIntervalMs = 2000;

} // namespace

StatusBarWidget::StatusBarWidget(ThumbnailCache *cache, QWidget *parent)
    : QWidget(parent)
    , m_cacheSource(cache)
{
    setObjectName(QStringLiteral("StatusBar"));

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(12, 4, 12, 4);
    layout->setSpacing(16);

    m_count = new QLabel(this);
    layout->addWidget(m_count);

    m_selection = new QLabel(this);
    layout->addWidget(m_selection);

    m_progress = new QProgressBar(this);
    m_progress->setFixedWidth(140);
    m_progress->setVisible(false);
    layout->addWidget(m_progress);

    m_scanMessage = new QLabel(this);
    layout->addWidget(m_scanMessage);

    layout->addStretch();

    m_cache = new QLabel(this);
    layout->addWidget(m_cache);

    m_autoRefreshTimer = new QTimer(this);
    m_autoRefreshTimer->setInterval(kCacheRefreshIntervalMs);
    connect(m_autoRefreshTimer, &QTimer::timeout, this, &StatusBarWidget::refreshCacheStats);
    m_autoRefreshTimer->start();

    m_transientTimer = new QTimer(this);
    m_transientTimer->setSingleShot(true);
    connect(m_transientTimer, &QTimer::timeout, this, [this]() {
        if (m_scanMessage->text() == m_transientText) {
            m_scanMessage->clear();
            m_transientText.clear();
        }
    });
}

void StatusBarWidget::setItemCount(int total, int visibleAfterFilter)
{
    if (visibleAfterFilter >= 0) {
        // 有筛选时提示"筛选后 / 总数"，让用户知道列表被收窄了
        m_count->setText(QStringLiteral("共 %1 项 · 筛选后 %2")
                             .arg(total)
                             .arg(visibleAfterFilter));
        return;
    }

    m_count->setText(QStringLiteral("共 %1 项").arg(total));
}

void StatusBarWidget::setSelectionSummary(const QList<MediaItem> &selected)
{
    if (selected.isEmpty()) {
        m_selection->clear();
        return;
    }

    if (selected.size() == 1) {
        m_selection->setText(QStringLiteral("已选 %1").arg(selected.first().fileName));
        return;
    }

    qint64 bytes = 0;
    for (const MediaItem &item : selected)
        bytes += item.size;

    m_selection->setText(QStringLiteral("已选 %1 项 · %2")
                             .arg(selected.size())
                             .arg(formatSize(bytes)));
}

void StatusBarWidget::setScanMessage(const QString &text)
{
    m_scanMessage->setText(text);
}

void StatusBarWidget::showTransientMessage(const QString &text, int timeoutMs)
{
    m_scanMessage->setText(text);
    m_transientText = text;
    m_transientTimer->start(timeoutMs);
}

void StatusBarWidget::setScanProgressVisible(bool visible)
{
    m_progress->setVisible(visible);
}

void StatusBarWidget::beginScanProgress()
{
    // 范围设为 0-0 会让 QProgressBar 显示为不确定进度的循环滚动
    m_progress->setRange(0, 0);
}

void StatusBarWidget::setScanProgress(int discovered, int total)
{
    if (!m_progress->isVisible() || total <= 0)
        return;

    m_progress->setRange(0, total);
    m_progress->setValue(qMin(discovered, total));
}

void StatusBarWidget::setCacheStatsVisible(bool visible)
{
    m_cache->setVisible(visible);
}

void StatusBarWidget::refreshCacheStats()
{
    if (!m_cacheSource)
        return;

    const ThumbnailCache::Stats s = m_cacheSource->stats();
    const int total = s.memoryHits + s.diskHits + s.misses;
    const int hits = s.memoryHits + s.diskHits;
    const double rate = total > 0 ? (hits * 100.0 / total) : 0.0;

    m_cache->setText(QStringLiteral("缓存 %1 项 / %2 · 命中率 %3%")
                         .arg(s.memoryEntries)
                         .arg(formatSize(s.memoryBytes))
                         .arg(QString::number(rate, 'f', 0)));
}

void StatusBarWidget::stopAutoRefresh()
{
    m_autoRefreshTimer->stop();
    m_transientTimer->stop();
}

} // namespace media
