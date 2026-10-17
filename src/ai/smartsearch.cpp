#include "smartsearch.h"
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QDirIterator>
#include <QImage>
#include <QCryptographicHash>
#include <QDebug>
#include <algorithm>
#include <QNetworkReply>
#include <QStandardPaths>
#include <QSize>
#include <QJsonArray>

SmartSearch::SmartSearch(QObject *parent)
    : QObject(parent)
    , m_networkManager(new QNetworkAccessManager(this))
    , m_currentReply(nullptr)
    , m_isSearching(false)
{
    initNetwork();
}

SmartSearch::~SmartSearch()
{
    if (m_currentReply) {
        m_currentReply->abort();
        m_currentReply->deleteLater();
    }
}

void SmartSearch::initNetwork()
{
    m_networkManager->setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
    m_networkManager->setTransferTimeout(30000);
}

void SmartSearch::searchByKeyword(const QString &keyword, SearchType type)
{
    if (keyword.isEmpty()) {
        emit searchFailed("搜索关键词不能为空");
        return;
    }

    emit searchStarted();
    emit searchStatus("正在搜索: " + keyword);

    // 检查缓存
    QString cacheKey = "keyword_" + keyword + "_" + QString::number(type);
    if (m_searchCache.contains(cacheKey)) {
        emit searchComplete(m_searchCache[cacheKey]);
        return;
    }

    // 执行搜索
    QList<SearchResult> results;

    switch (type) {
    case FileName:
        results = localSearch(keyword);
        break;
    case Content:
        // 语义搜索会自动发出信号，这里直接返回空结果
        results = QList<SearchResult>();
        break;
    case Visual:
        // 需要参考图像，暂时返回空列表
        break;
    case DateRange:
    case Tag:
    case Advanced:
        // 综合搜索
        results = localSearch(keyword);
        break;
    }

    // 添加到缓存
    addToCache(cacheKey, results);

    emit searchComplete(results);
    m_searchHistory.append(keyword);
}

void SmartSearch::searchByDateRange(const QDate &startDate, const QDate &endDate)
{
    emit searchStarted();
    emit searchStatus("正在搜索日期范围: " + startDate.toString() + " 至 " + endDate.toString());

    QList<SearchResult> results = dateRangeSearch(startDate, endDate);
    emit searchComplete(results);
}

void SmartSearch::searchByTags(const QStringList &tags)
{
    if (tags.isEmpty()) {
        emit searchFailed("标签不能为空");
        return;
    }

    emit searchStarted();
    emit searchStatus("正在搜索标签: " + tags.join(", "));

    QList<SearchResult> results = tagBasedSearch(tags);
    emit searchComplete(results);
}

void SmartSearch::semanticSearch(const QString &query)
{
    emit searchStarted();
    emit searchStatus("正在执行语义搜索: " + query);

    // 使用本地语义搜索（基于关键词匹配）
    QList<SearchResult> results = localSearch(query);

    // 如果结果为空，尝试AI搜索
    if (results.isEmpty()) {
        processTextForSearch(query);
    } else {
        emit searchComplete(results);
    }
}

void SmartSearch::similarImages(const QString &referenceImage)
{
    if (!QFile::exists(referenceImage)) {
        emit searchFailed("参考图像不存在: " + referenceImage);
        return;
    }

    emit searchStarted();
    emit searchStatus("正在查找相似图像...");

    QList<SearchResult> results = visualSimilaritySearch(referenceImage);
    emit searchComplete(results);
}

void SmartSearch::faceSearch(const QString &faceDescription)
{
    emit searchStarted();
    emit searchStatus("正在搜索人脸: " + faceDescription);

    // 本地人脸搜索（基于文件名标签）
    QList<SearchResult> results;
    QString cacheKey = "face_" + faceDescription;

    if (m_searchCache.contains(cacheKey)) {
        results = m_searchCache[cacheKey];
    } else {
        // 在实际应用中，这里应该调用人脸识别API
        // 现在返回空结果
        results = QList<SearchResult>();
    }

    emit searchComplete(results);
}

void SmartSearch::sceneSearch(const QString &sceneType)
{
    emit searchStarted();
    emit searchStatus("正在搜索场景: " + sceneType);

    // 基于场景类型的搜索
    QList<SearchResult> results;
    QString cacheKey = "scene_" + sceneType;

    if (m_searchCache.contains(cacheKey)) {
        results = m_searchCache[cacheKey];
    } else {
        // 在实际应用中，这里应该调用场景识别API
        // 现在使用本地搜索
        results = localSearch(sceneType);
    }

    emit searchComplete(results);
}

void SmartSearch::batchSearch(const QStringList &queries)
{
    QList<SearchResult> allResults;

    emit searchStarted();
    emit searchStatus("开始批量搜索...");

    for (int i = 0; i < queries.size(); ++i) {
        emit searchProgress(i, queries.size());
        QList<SearchResult> results = localSearch(queries[i]);
        allResults.append(results);
    }

    // 去重
    auto uniqueResults = std::unique(allResults.begin(), allResults.end(),
        [](const SearchResult &a, const SearchResult &b) {
            return a.filePath == b.filePath;
        });
    allResults.erase(uniqueResults, allResults.end());

    emit searchComplete(allResults);
}

QStringList SmartSearch::getSearchHistory() const
{
    return m_searchHistory;
}

void SmartSearch::clearSearchHistory()
{
    m_searchHistory.clear();
    m_searchCache.clear();
}

void SmartSearch::cancelSearch()
{
    if (m_currentReply && m_currentReply->isRunning()) {
        m_currentReply->abort();
        m_isSearching = false;
        emit searchStatus("搜索已取消");
    }
}

void SmartSearch::onRequestFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) return;

    reply->deleteLater();
    m_currentReply = nullptr;

    if (reply->error() != QNetworkReply::NetworkError::NoError) {
        emit searchFailed("网络错误: " + reply->errorString());
        return;
    }

    QByteArray data = reply->readAll();
    QList<SearchResult> results = parseSearchResponse(data);
    emit searchComplete(results);
    m_isSearching = false;
}

void SmartSearch::processImageForSearch(const QString &imagePath)
{
    // 这里可以集成图像识别API
    // 目前使用本地处理
    QFileInfo info(imagePath);
    QImage img(imagePath);
    SearchResult result;
    result.filePath = imagePath;
    result.title = info.fileName();
    result.description = "图像文件";
    result.confidence = 1.0;
    result.date = info.lastModified().date();
    result.size = img.isNull() ? QSize(0, 0) : img.size();

    // 基于文件名生成标签
    QString fileName = info.fileName().toLower();
    if (fileName.contains("photo")) {
        result.tags << "照片";
    }
    if (fileName.contains("vacation")) {
        result.tags << "旅行";
    }
    if (fileName.contains("family")) {
        result.tags << "家庭";
    }

    emit searchComplete(QList<SearchResult>() << result);
}

void SmartSearch::processTextForSearch(const QString &text)
{
    // 使用本地文本分析
    QList<SearchResult> results;
    QStringList searchPaths = {
        QStandardPaths::writableLocation(QStandardPaths::PicturesLocation),
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
    };

    for (const QString &searchPath : searchPaths) {
        QDirIterator it(searchPath, QStringList() << "*.jpg" << "*.jpeg" << "*.png",
                       QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            emit searchProgress(results.size(), 100); // 更新进度

            QString filePath = it.next();
            QFileInfo info(filePath);
            QString content = info.fileName().toLower() + " " +
                           info.absolutePath().toLower();

            if (content.contains(text.toLower())) {
                QImage img(filePath);
                SearchResult result;
                result.filePath = filePath;
                result.title = info.fileName();
                result.description = "匹配文件";
                result.confidence = 0.8;
                result.date = info.lastModified().date();
                result.size = img.isNull() ? QSize(0, 0) : img.size();
                results.append(result);
            }
        }
    }

    emit searchComplete(results);
}

QList<SearchResult> SmartSearch::parseSearchResponse(const QByteArray &data)
{
    QJsonDocument jsonDoc = QJsonDocument::fromJson(data);
    if (jsonDoc.isNull()) {
        return QList<SearchResult>();
    }

    QList<SearchResult> results;
    QJsonArray jsonArray = jsonDoc.array();

    for (const QJsonValue &value : jsonArray) {
        QJsonObject obj = value.toObject();
        SearchResult result;
        result.filePath = obj["path"].toString();
        result.title = obj["title"].toString();
        result.description = obj["description"].toString();
        result.confidence = obj["confidence"].toDouble();
        result.date = QDate::fromString(obj["date"].toString(), "yyyy-MM-dd");
        result.size = QSize(obj["width"].toInt(), obj["height"].toInt());

        QJsonArray tagsArray = obj["tags"].toArray();
        for (const QJsonValue &tagValue : tagsArray) {
            result.tags.append(tagValue.toString());
        }

        results.append(result);
    }

    return results;
}

QList<SearchResult> SmartSearch::localSearch(const QString &keyword)
{
    QList<SearchResult> results;
    QStringList searchPaths = {
        QStandardPaths::writableLocation(QStandardPaths::PicturesLocation),
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
    };

    QString searchKeyword = keyword.toLower();

    for (const QString &searchPath : searchPaths) {
        QDirIterator it(searchPath, QStringList() << "*.jpg" << "*.jpeg" << "*.png",
                       QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            emit searchProgress(results.size(), 100);

            QString filePath = it.next();
            QFileInfo info(filePath);
            QString fileName = info.fileName().toLower();

            // 基本关键词匹配
            if (fileName.contains(searchKeyword)) {
                QImage img(filePath);
                SearchResult result;
                result.filePath = filePath;
                result.title = info.fileName();
                result.description = "通过关键词匹配";
                result.confidence = 0.9;
                result.date = info.lastModified().date();
                result.size = img.isNull() ? QSize(0, 0) : img.size();
                results.append(result);
            }
        }
    }

    return results;
}

QList<SearchResult> SmartSearch::visualSimilaritySearch(const QString &referencePath)
{
    QList<SearchResult> results;
    QString referenceHash = averageHash(referencePath);

    // 在实际应用中，这里应该使用视觉相似度算法
    // 现在返回空列表

    // 简单示例：返回相同路径作为结果
    SearchResult result;
    result.filePath = referencePath;
    result.title = QFileInfo(referencePath).fileName();
    result.description = "参考图像";
    result.confidence = 1.0;
    result.date = QFileInfo(referencePath).lastModified().date();
    result.size = QImage(referencePath).size();
    results.append(result);
    return results;
}

QString SmartSearch::averageHash(const QString &imagePath)
{
    QImage image(imagePath);
    if (image.isNull()) {
        return "";
    }

    // 缩放到 8x8
    QImage small = image.scaled(8, 8, Qt::KeepAspectRatioByExpanding, Qt::FastTransformation);
    small = small.convertToFormat(QImage::Format_Grayscale8);

    // 计算平均值
    double avg = 0;
    uchar *bits = small.bits();
    for (int i = 0; i < 64; i++) {
        avg += bits[i];
    }
    avg /= 64;

    // 生成哈希值
    QString hash;
    for (int i = 0; i < 64; i++) {
        hash += (bits[i] >= avg) ? "1" : "0";
    }

    return hash;
}

void SmartSearch::addToCache(const QString &query, const QList<SearchResult> &results)
{
    m_searchCache.insert(query, results);
}

QList<SearchResult> SmartSearch::dateRangeSearch(const QDate &start, const QDate &end)
{
    QList<SearchResult> results;
    QStringList searchPaths = {
        QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)
    };

    for (const QString &searchPath : searchPaths) {
        QDirIterator it(searchPath, QStringList() << "*.jpg" << "*.jpeg" << "*.png",
                       QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            QString filePath = it.next();
            QFileInfo info(filePath);
            QDate fileDate = info.lastModified().date();

            if (fileDate >= start && fileDate <= end) {
                QImage img(filePath);
                SearchResult result;
                result.filePath = filePath;
                result.title = info.fileName();
                result.description = "通过日期匹配";
                result.confidence = 1.0;
                result.date = fileDate;
                result.size = img.isNull() ? QSize(0, 0) : img.size();
                results.append(result);
            }
        }
    }

    return results;
}

QList<SearchResult> SmartSearch::tagBasedSearch(const QStringList &tags)
{
    QList<SearchResult> results;
    QStringList searchPaths = {
        QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)
    };

    for (const QString &searchPath : searchPaths) {
        QDirIterator it(searchPath, QStringList() << "*.jpg" << "*.jpeg" << "*.png",
                       QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            QString filePath = it.next();
            QFileInfo info(filePath);
            QString fileName = info.fileName().toLower();

            bool match = false;
            for (const QString &tag : tags) {
                if (fileName.contains(tag.toLower())) {
                    match = true;
                    break;
                }
            }

            if (match) {
                QImage img(filePath);
                SearchResult result;
                result.filePath = filePath;
                result.title = info.fileName();
                result.description = "通过标签匹配";
                result.confidence = 0.8;
                result.date = info.lastModified().date();
                result.size = img.isNull() ? QSize(0, 0) : img.size();
                results.append(result);
            }
        }
    }

    return results;
}

// SearchFilter 实现
SearchFilter::SearchFilter(QObject *parent)
    : QObject(parent)
    , m_minConfidence(0.0)
    , m_minWidth(0)
    , m_minHeight(0)
{
}

SearchFilter::~SearchFilter()
{
}

void SearchFilter::setResults(const QList<SearchResult> &results)
{
    m_results = results;
    emit filterResultsChanged(applyFilters());
}

QList<SearchResult> SearchFilter::getFilteredResults() const
{
    return applyFilters();
}

void SearchFilter::setMinimumConfidence(double confidence)
{
    m_minConfidence = confidence;
    emit filterResultsChanged(applyFilters());
}

void SearchFilter::setCategoryFilter(const QString &category)
{
    m_categoryFilter = category;
    emit filterResultsChanged(applyFilters());
}

void SearchFilter::setDateFilter(const QDate &start, const QDate &end)
{
    m_dateStart = start;
    m_dateEnd = end;
    emit filterResultsChanged(applyFilters());
}

void SearchFilter::setSizeFilter(int minWidth, int minHeight)
{
    m_minWidth = minWidth;
    m_minHeight = minHeight;
    emit filterResultsChanged(applyFilters());
}

QList<SearchResult> SearchFilter::applyFilters() const
{
    QList<SearchResult> filtered = m_results;

    // 置信度过滤
    filtered.erase(std::remove_if(filtered.begin(), filtered.end(),
        [this](const SearchResult &result) {
            return result.confidence < m_minConfidence;
        }), filtered.end());

    // 分类过滤
    if (!m_categoryFilter.isEmpty()) {
        filtered.erase(std::remove_if(filtered.begin(), filtered.end(),
            [this](const SearchResult &result) {
                return result.category != m_categoryFilter;
            }), filtered.end());
    }

    // 日期过滤
    if (m_dateStart.isValid() && m_dateEnd.isValid()) {
        filtered.erase(std::remove_if(filtered.begin(), filtered.end(),
            [this](const SearchResult &result) {
                return result.date < m_dateStart || result.date > m_dateEnd;
            }), filtered.end());
    }

    // 尺寸过滤
    if (m_minWidth > 0 || m_minHeight > 0) {
        filtered.erase(std::remove_if(filtered.begin(), filtered.end(),
            [this](const SearchResult &result) {
                return result.size.width() < m_minWidth ||
                       result.size.height() < m_minHeight;
            }), filtered.end());
    }

    return filtered;
}
