#include "DuplicateService.h"
#include "HashUtil.h"
#include "MediaDatabase.h"
#include "Logger.h"

#include <QSet>
#include <QHash>

namespace media {

DuplicateService::DuplicateService(MediaDatabase *database)
    : m_database(database)
{
}

DuplicateService::Result DuplicateService::findExactDuplicates(const ProgressCallback &onProgress)
{
    Result result;

    if (!m_database || !m_database->isOpen())
        return result;

    /*!
     * 只给还没算过哈希的记录补算。
     * 增量：已算过的跳过，所以第二次打开几乎瞬间完成。
     *
     * 上限 5000：一次算太多会让模态对话框停留过久。剩下的下次再算。
     */
    const QList<MediaItem> pending = m_database->itemsWithoutHash(5000);

    if (pending.isEmpty()) {
        // 没有待算的，直接看库里已有的哈希分组
        const QHash<QString, QList<MediaItem>> groups = m_database->duplicateGroups();
        result.groupCount = groups.size();
        for (auto it = groups.constBegin(); it != groups.constEnd(); ++it) {
            result.groups.append(it.value());
            result.itemCount += it.value().size();
        }
        return result;
    }

    int done = 0;
    for (const MediaItem &item : pending) {
        if (onProgress && !onProgress(done, pending.size())) {
            result.cancelled = true;
            break;
        }

        const QString hash = HashUtil::fileMd5(item.path);
        if (!hash.isEmpty())
            m_database->setContentHash(item.id, hash);

        ++done;
    }

    // 收尾再报一次，让进度条能走到满格
    if (onProgress && !result.cancelled)
        onProgress(pending.size(), pending.size());

    result.scanned = done;

    const QHash<QString, QList<MediaItem>> groups = m_database->duplicateGroups();
    result.groupCount = groups.size();
    for (auto it = groups.constBegin(); it != groups.constEnd(); ++it) {
        result.groups.append(it.value());
        result.itemCount += it.value().size();
    }

    return result;
}

DuplicateService::Result DuplicateService::findSimilarImages(const ProgressCallback &onProgress)
{
    Result result;
    result.threshold = HashUtil::kSimilarThreshold;

    if (!m_database || !m_database->isOpen())
        return result;

    QList<MediaItem> images;
    const QList<MediaItem> all = m_database->allItems();
    for (const MediaItem &item : all) {
        if (item.type == MediaType::Image)
            images.append(item);
    }

    if (images.isEmpty())
        return result;

    // 限制规模并明确告知，避免用户在一个大库上等到失控
    if (images.size() > kMaxVisualScanImages) {
        result.truncated = true;
        result.truncatedLimit = kMaxVisualScanImages;
        images = images.mid(0, kMaxVisualScanImages);
    }

    int done = 0;
    for (const MediaItem &item : images) {
        if (onProgress && !onProgress(done, images.size())) {
            result.cancelled = true;
            break;
        }

        if (item.visualHash.isEmpty()) {
            const quint64 ph = HashUtil::perceptualHash(item.path);
            if (ph != 0)
                m_database->setVisualHash(item.id, QString::number(ph));
        }

        ++done;
    }

    if (onProgress && !result.cancelled)
        onProgress(images.size(), images.size());

    /*!
     * 分组：对算过哈希的图片两两比对。
     *
     * O(n²) 在这里是可接受的：n 被限制在 2000，且只在用户主动触发时执行一次。
     * 每张图只需要和"尚未入组"的图比较，所以内层从 i+1 开始，
     * 已入组的直接跳过 —— 一把梭全比会做很多重复工作。
     */
    QList<MediaItem> hashed;
    hashed.reserve(images.size());
    for (const MediaItem &item : images) {
        if (item.visualHash.isEmpty()) {
            // 本项本次没算出哈希（或之前也没算），重新查一次拿最新值：
            // 上面的循环只更新了数据库，手里这份 item 还是旧快照。
            const MediaItem fresh = m_database->itemById(item.id);
            if (!fresh.visualHash.isEmpty())
                hashed.append(fresh);
        } else {
            hashed.append(item);
        }
    }

    QSet<qint64> used;

    for (int i = 0; i < hashed.size(); ++i) {
        if (used.contains(hashed.at(i).id))
            continue;

        bool okA = false;
        const quint64 hashA = hashed.at(i).visualHash.toULongLong(&okA);
        if (!okA)
            continue;

        QList<MediaItem> group;
        group.append(hashed.at(i));

        for (int j = i + 1; j < hashed.size(); ++j) {
            if (used.contains(hashed.at(j).id))
                continue;

            bool okB = false;
            const quint64 hashB = hashed.at(j).visualHash.toULongLong(&okB);
            if (!okB)
                continue;

            if (HashUtil::hammingDistance(hashA, hashB) <= HashUtil::kSimilarThreshold) {
                group.append(hashed.at(j));
                used.insert(hashed.at(j).id);
            }
        }

        if (group.size() > 1) {
            used.insert(hashed.at(i).id);
            result.groups.append(group);
            result.itemCount += group.size();
        }
    }

    result.scanned = hashed.size();
    result.groupCount = result.groups.size();

    LOG_DEBUG("Duplicate") << "similar scan: hashed=" << hashed.size()
                           << "groups=" << result.groupCount;

    return result;
}

} // namespace media
