#ifndef IMAGEPROCESSOR_H
#define IMAGEPROCESSOR_H

#include <QObject>
#include <QImage>
#include <QStringList>
#include <QNetworkAccessManager>
#include <QNetworkReply>

class ImageProcessor : public QObject
{
    Q_OBJECT

public:
    explicit ImageProcessor(QObject *parent = nullptr);
    ~ImageProcessor();

    // 图像处理功能
    void removeBackground(const QString &imagePath);
    void enhanceImage(const QString &imagePath);
    void applyStyleTransfer(const QString &imagePath, const QString &style);
    void detectObjects(const QString &imagePath);
    void generateSmartTags(const QString &imagePath);
    void createThumbnail(const QString &imagePath, int size = 200);

    // 批量处理
    void batchProcessImages(const QStringList &imagePaths, const QString &operation);
    void batchRemoveBackground(const QStringList &imagePaths);
    void batchEnhanceImages(const QStringList &imagePaths);

signals:
    // 处理完成信号
    void processingComplete(const QString &outputPath, const QImage &result);
    void batchProcessingComplete(const QStringList &outputPaths);

    // 进度信号
    void processingProgress(int current, int total);
    void processingStatus(const QString &status);

    // 错误信号
    void processingFailed(const QString &error);

public slots:
    void cancelProcessing();

private slots:
    void onRequestFinished();
    void onImageDownloaded();

private:
    QNetworkAccessManager *m_networkManager;
    QNetworkReply *m_currentReply;
    QStringList m_processingQueue;
    bool m_isProcessing;
    QString m_currentOperation;
    QString m_outputDirectory;

    // API配置
    const QString REMOVE_BG_API_URL = "https://api.remove.bg/v1.0/removebg";
    const QString CLOUDINARY_API_URL = "https://api.cloudinary.com/v1_1/your_cloud_name/image/upload";

    void initNetwork();
    QString prepareOutputPath(const QString &inputPath);
    void processNextImage();
    void handleImageResponse(const QByteArray &data);
    void handleError(const QString &error);
};

// 处理结果结构体
struct ProcessingResult {
    QString inputPath;
    QString outputPath;
    bool success;
    QString error;
    QByteArray imageData;
};

#endif // IMAGEPROCESSOR_H