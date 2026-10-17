#ifndef SMARTSEARCH_H
#define SMARTSEARCH_H

#include <QObject>
#include <QListWidget>
#include <QStringList>
#include <QMap>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>

// 搜索结果结构体
struct SearchResult {
    QString filePath;
    QString title;
    QString description;
    double confidence;
    QStringList tags;
    QString category;
    QDate date;
    QSize size;
};

class SmartSearch : public QObject
{
    Q_OBJECT

public:
    explicit SmartSearch(QObject *parent = nullptr);
    ~SmartSearch();

    // 搜索类型
    enum SearchType {
        FileName,      // 文件名搜索
        Content,       // 内容搜索（基于描述）
        Visual,        // 视觉相似度搜索
        DateRange,     // 日期范围搜索
        Tag,           // 标签搜索
        Advanced       // 高级综合搜索
    };

    // 基本搜索功能
    void searchByKeyword(const QString &keyword, SearchType type = FileName);
    void searchByDateRange(const QDate &startDate, const QDate &endDate);
    void searchByTags(const QStringList &tags);

    // 智能搜索功能
    void semanticSearch(const QString &query);
    void similarImages(const QString &referenceImage);
    void faceSearch(const QString &faceDescription);
    void sceneSearch(const QString &sceneType);

    // 批量搜索
    void batchSearch(const QStringList &queries);

    // 获取搜索历史
    QStringList getSearchHistory() const;
    void clearSearchHistory();

signals:
    // 搜索完成信号
    void searchComplete(const QList<SearchResult> &results);
    void searchProgress(int progress, int total);

    // 搜索状态信号
    void searchStarted();
    void searchStatus(const QString &status);

    // 错误信号
    void searchFailed(const QString &error);

public slots:
    void cancelSearch();

private slots:
    void onRequestFinished();

private:
    QNetworkAccessManager *m_networkManager;
    QNetworkReply *m_currentReply;
    bool m_isSearching;
    QStringList m_searchHistory;

    // 搜索配置
    const QString BaiduVisionAPI = "https://aip.baidubce.com/rest/2.0/image-classification/v1/image_classification";
    const QString QianfanAPI = "https://aip.baidubce.com/rpc/2.0/ai_custom/v1/wenxinworkshop/chat/completions";

    // 缓存
    QMap<QString, QList<SearchResult>> m_searchCache;

    void initNetwork();
    void processImageForSearch(const QString &imagePath);
    void processTextForSearch(const QString &text);
    QList<SearchResult> parseSearchResponse(const QByteArray &data);
    QString generateSearchPrompt(const QString &query);
    void addToCache(const QString &query, const QList<SearchResult> &results);
    QList<SearchResult> getFromCache(const QString &query);

    // 本地搜索引擎（免费且快速）
    QList<SearchResult> localSearch(const QString &keyword);
    QList<SearchResult> visualSimilaritySearch(const QString &referencePath);
    QList<SearchResult> dateRangeSearch(const QDate &start, const QDate &end);
    QList<SearchResult> tagBasedSearch(const QStringList &tags);
    QString averageHash(const QString &imagePath);
};

// 搜索过滤器
class SearchFilter : public QObject
{
    Q_OBJECT

public:
    explicit SearchFilter(QObject *parent = nullptr);
    ~SearchFilter();

    void setResults(const QList<SearchResult> &results);
    QList<SearchResult> getFilteredResults() const;
    void setMinimumConfidence(double confidence);
    void setCategoryFilter(const QString &category);
    void setDateFilter(const QDate &start, const QDate &end);
    void setSizeFilter(int minWidth, int minHeight);

signals:
    void filterResultsChanged(const QList<SearchResult> &results);

private:
    QList<SearchResult> m_results;
    double m_minConfidence;
    QString m_categoryFilter;
    QDate m_dateStart;
    QDate m_dateEnd;
    int m_minWidth;
    int m_minHeight;

    QList<SearchResult> applyFilters() const;
};

#endif // SMARTSEARCH_H