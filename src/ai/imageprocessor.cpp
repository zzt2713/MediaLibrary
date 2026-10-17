#include "imageprocessor.h"
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QHttpMultiPart>
#include <QNetworkRequest>
#include <QBuffer>
#include <QImageReader>
#include <QMessageBox>
#include <QPainter>
#include <QDebug>
#include <algorithm>
#include <QDir>
#include <QUrlQuery>
#include <QJsonArray>

ImageProcessor::ImageProcessor(QObject *parent)
    : QObject(parent)
    , m_networkManager(new QNetworkAccessManager(this))
    , m_currentReply(nullptr)
    , m_isProcessing(false)
{
    initNetwork();
}

ImageProcessor::~ImageProcessor()
{
    if (m_currentReply) {
        m_currentReply->abort();
        m_currentReply->deleteLater();
    }
}

void ImageProcessor::initNetwork()
{
    m_networkManager->setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
    m_networkManager->setTransferTimeout(30000); // 30秒超时
}

void ImageProcessor::removeBackground(const QString &imagePath)
{
    if (!QFile::exists(imagePath)) {
        emit processingFailed("文件不存在: " + imagePath);
        return;
    }

    emit processingStatus("正在移除背景...");

    QUrl url(REMOVE_BG_API_URL);
    QNetworkRequest request(url);

    // 设置请求头
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
    request.setRawHeader("X-Api-Key", "your_remove_bg_key"); // 需要注册获取

    // 构建请求参数
    QUrlQuery query;
    query.addQueryItem("image_url", QUrl::fromLocalFile(imagePath).toString());
    query.addQueryItem("size", "auto");
    query.addQueryItem("format", "auto");

    QByteArray data = query.toString(QUrl::FullyEncoded).toUtf8();
    m_currentReply = m_networkManager->post(request, data);

    connect(m_currentReply, &QNetworkReply::finished,
            this, &ImageProcessor::onRequestFinished);
}

void ImageProcessor::enhanceImage(const QString &imagePath)
{
    emit processingStatus("正在增强图像质量...");

    // 使用本地OpenCV进行图像增强（免费且快速）
    QImage image(imagePath);
    if (image.isNull()) {
        emit processingFailed("无法加载图像: " + imagePath);
        return;
    }

    // 简单的图像增强算法
    QImage enhanced = image.convertToFormat(QImage::Format_RGB888);

    // 调整亮度
    int brightness = 20;
    uchar *bits = enhanced.bits();
    for (int i = 0; i < enhanced.sizeInBytes(); i += 3) {
        bits[i] = qBound(0, bits[i] + brightness, 255);
        bits[i + 1] = qBound(0, bits[i + 1] + brightness, 255);
        bits[i + 2] = qBound(0, bits[i + 2] + brightness, 255);
    }

    // 保存增强后的图像
    QString outputPath = prepareOutputPath(imagePath);
    enhanced.save(outputPath);

    emit processingComplete(outputPath, enhanced);
}

void ImageProcessor::applyStyleTransfer(const QString &imagePath, const QString &style)
{
    emit processingStatus("正在应用风格转换...");

    // 使用本地风格转换（简化版本）
    QImage image(imagePath);
    if (image.isNull()) {
        emit processingFailed("无法加载图像: " + imagePath);
        return;
    }

    // 根据风格应用滤镜
    QImage styled = image.copy();
    QPainter painter(&styled);

    if (style.toLower() == "cartoon") {
        // 卡通风格效果
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        QColor overlay(255, 230, 180);
        painter.fillRect(styled.rect(), overlay);
        painter.setOpacity(0.3);
    } else if (style.toLower() == "vintage") {
        // 复古风格效果
        QColor overlay(150, 100, 50);
        painter.fillRect(styled.rect(), overlay);
        painter.setOpacity(0.4);
    } else {
        // 默认风格
        QColor overlay(255, 200, 200);
        painter.fillRect(styled.rect(), overlay);
        painter.setOpacity(0.2);
    }

    painter.end();

    // 保存风格转换后的图像
    QString outputPath = prepareOutputPath(imagePath);
    styled.save(outputPath);

    emit processingComplete(outputPath, styled);
}

void ImageProcessor::detectObjects(const QString &imagePath)
{
    emit processingStatus("正在检测对象...");

    // 使用简单的边缘检测（本地处理）
    QImage image(imagePath);
    if (image.isNull()) {
        emit processingFailed("无法加载图像: " + imagePath);
        return;
    }

    // 转换为灰度图
    QImage gray = image.convertToFormat(QImage::Format_Grayscale8);

    // 应用简单的边缘检测算法
    QImage result = gray.copy();
    uchar *srcBits = gray.bits();
    uchar *dstBits = result.bits();

    // Sobel边缘检测（简化版）
    int width = gray.width();
    int height = gray.height();

    for (int y = 1; y < height - 1; y++) {
        for (int x = 1; x < width - 1; x++) {
            int gx = -srcBits[(y-1)*width + x-1] + srcBits[(y-1)*width + x+1]
                   - 2*srcBits[y*width + x-1] + 2*srcBits[y*width + x+1]
                   - srcBits[(y+1)*width + x-1] + srcBits[(y+1)*width + x+1];

            int gy = -srcBits[(y-1)*width + x-1] - 2*srcBits[(y-1)*width + x] - srcBits[(y-1)*width + x+1]
                   + srcBits[(y+1)*width + x-1] + 2*srcBits[(y+1)*width + x] + srcBits[(y+1)*width + x+1];

            int magnitude = sqrt(gx*gx + gy*gy);
            uchar value = qBound(0, magnitude, 255);

            dstBits[y*width + x] = value;
        }
    }

    // 保存检测结果
    QString outputPath = prepareOutputPath(imagePath);
    result.save(outputPath);

    emit processingComplete(outputPath, result);
}

void ImageProcessor::generateSmartTags(const QString &imagePath)
{
    emit processingStatus("正在生成智能标签...");

    // 使用本地分析生成标签
    QStringList tags;

    // 检测图像尺寸
    QImage image(imagePath);
    if (image.isNull()) {
        emit processingFailed("无法加载图像: " + imagePath);
        return;
    }

    // 基于特征生成标签
    if (image.width() > image.height()) {
        tags << "横向照片";
    } else if (image.height() > image.width()) {
        tags << "纵向照片";
    } else {
        tags << "正方形照片";
    }

    // 检测颜色特征
    QRgb dominantColor = image.pixel(image.width()/2, image.height()/2);
    QColor color(dominantColor);
    if (color.lightness() > 200) {
        tags << "明亮";
    } else if (color.lightness() < 50) {
        tags << "暗淡";
    }

    // 基于文件名生成标签
    QFileInfo info(imagePath);
    QString fileName = info.fileName().toLower();
    if (fileName.contains("vacation")) {
        tags << "旅行";
    } else if (fileName.contains("family")) {
        tags << "家庭";
    } else if (fileName.contains("nature")) {
        tags << "自然";
    }

    // 保存标签到文件
    QString tagPath = imagePath + ".tags";
    QFile tagFile(tagPath);
    if (tagFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream out(&tagFile);
        out << "智能生成的标签:\n";
        for (const QString &tag : tags) {
            out << "- " << tag << "\n";
        }
        tagFile.close();
    }

    emit processingStatus("标签已生成: " + tags.join(", "));
}

void ImageProcessor::createThumbnail(const QString &imagePath, int size)
{
    emit processingStatus("正在创建缩略图...");

    QImage image(imagePath);
    if (image.isNull()) {
        emit processingFailed("无法加载图像: " + imagePath);
        return;
    }

    // 创建缩略图
    QImage thumbnail = image.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);

    // 保存缩略图
    QFileInfo info(imagePath);
    QString thumbPath = info.absolutePath() + "/thumbnails/" + info.baseName() + "_thumb.jpg";
    QDir().mkpath(info.absolutePath() + "/thumbnails");

    thumbnail.save(thumbPath);

    emit processingComplete(thumbPath, thumbnail);
}

void ImageProcessor::batchProcessImages(const QStringList &imagePaths, const QString &operation)
{
    m_processingQueue = imagePaths;
    m_currentOperation = operation;
    m_outputDirectory = QFileInfo(imagePaths.first()).absolutePath() + "/processed";
    QDir().mkpath(m_outputDirectory);

    m_isProcessing = false;
    processNextImage();
}

void ImageProcessor::batchRemoveBackground(const QStringList &imagePaths)
{
    batchProcessImages(imagePaths, "remove_background");
}

void ImageProcessor::batchEnhanceImages(const QStringList &imagePaths)
{
    batchProcessImages(imagePaths, "enhance");
}

void ImageProcessor::processNextImage()
{
    if (m_processingQueue.isEmpty()) {
        emit processingStatus("批量处理完成");
        m_isProcessing = false;
        return;
    }

    if (m_isProcessing) {
        // 处理队列中的下一个
        QString imagePath = m_processingQueue.takeFirst();
        emit processingStatus("正在处理: " + QFileInfo(imagePath).fileName());

        if (m_currentOperation == "remove_background") {
            removeBackground(imagePath);
        } else if (m_currentOperation == "enhance") {
            enhanceImage(imagePath);
        } else if (m_currentOperation == "style_transfer") {
            applyStyleTransfer(imagePath, "default");
        } else if (m_currentOperation == "detect_objects") {
            detectObjects(imagePath);
        }
    }
}

void ImageProcessor::cancelProcessing()
{
    if (m_currentReply && m_currentReply->isRunning()) {
        m_currentReply->abort();
    }
    m_processingQueue.clear();
    m_isProcessing = false;
    emit processingStatus("处理已取消");
}

void ImageProcessor::onRequestFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) return;

    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        handleError("网络错误: " + reply->errorString());
        processNextImage();
        return;
    }

    QByteArray data = reply->readAll();
    handleImageResponse(data);
    processNextImage();
}

void ImageProcessor::onImageDownloaded()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) return;

    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        handleError("下载失败: " + reply->errorString());
        return;
    }

    QByteArray imageData = reply->readAll();
    QImage image;
    if (image.loadFromData(imageData)) {
        emit processingComplete("", image);
    } else {
        handleError("图像格式错误");
    }
}

void ImageProcessor::handleImageResponse(const QByteArray &data)
{
    QJsonDocument jsonDoc = QJsonDocument::fromJson(data);
    if (jsonDoc.isNull()) {
        handleError("无效的响应");
        return;
    }

    QJsonObject jsonObj = jsonDoc.object();

    if (jsonObj.contains("data")) {
        QJsonArray dataArray = jsonObj["data"].toArray();
        if (!dataArray.isEmpty()) {
            QString imageUrl = dataArray[0].toObject()["url"].toString();

            // 下载处理后的图像
            QUrl url(imageUrl);
            QNetworkRequest request(url);
            m_currentReply = m_networkManager->get(request);
            connect(m_currentReply, &QNetworkReply::finished,
                    this, &ImageProcessor::onImageDownloaded);
        }
    } else {
        emit processingStatus("处理完成");
    }
}

void ImageProcessor::handleError(const QString &error)
{
    emit processingFailed(error);
}

QString ImageProcessor::prepareOutputPath(const QString &inputPath)
{
    QFileInfo info(inputPath);
    QString outputPath = m_outputDirectory + "/" + info.fileName();
    int counter = 1;

    while (QFile::exists(outputPath)) {
        QString baseName = info.baseName();
        QString extension = info.suffix();
        outputPath = m_outputDirectory + "/" + baseName + "_" + QString::number(counter) + "." + extension;
        counter++;
    }

    return outputPath;
}
