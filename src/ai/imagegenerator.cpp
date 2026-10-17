#include "imagegenerator.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QDir>
#include <QDebug>
#include <QJsonArray>
#include <QBuffer>
#include <QImage>
#include <QBuffer>

ImageGenerator::ImageGenerator(QObject *parent)
    : QObject(parent)
    , m_networkManager(new QNetworkAccessManager(this))
    , m_currentReply(nullptr)
    , m_isBusy(false)
{
    initNetwork();
}

ImageGenerator::~ImageGenerator()
{
    if (m_currentReply) {
        m_currentReply->abort();
        m_currentReply->deleteLater();
    }
}

void ImageGenerator::initNetwork()
{
    // 设置超时
    m_networkManager->setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
    m_networkManager->setTransferTimeout(30000); // 30秒超时
}

void ImageGenerator::generateImage(const QString &prompt, const QString &negativePrompt)
{
    if (m_isBusy) {
        emit generationFailed("生成器正在处理中，请稍候");
        return;
    }

    m_isBusy = true;
    m_lastError.clear();

    QUrl url(DEEPSEEK_API_URL);
    QNetworkRequest request(url);

    // 设置请求头
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Authorization", "Bearer sk-or-v1-example-token"); // 需要替换为实际token

    // 构建请求体
    QJsonObject jsonData;
    jsonData["prompt"] = prompt;
    if (!negativePrompt.isEmpty()) {
        jsonData["negative_prompt"] = negativePrompt;
    }
    jsonData["size"] = "1024x1024";
    jsonData["quality"] = "standard";

    QJsonDocument jsonDoc(jsonData);
    QByteArray requestData = jsonDoc.toJson();

    qDebug() << "发送AI图像生成请求:" << prompt;

    m_currentReply = m_networkManager->post(request, requestData);

    connect(m_currentReply, &QNetworkReply::finished,
            this, &ImageGenerator::onRequestFinished);
    connect(m_currentReply, &QNetworkReply::uploadProgress,
            this, &ImageGenerator::onProgress);
}

void ImageGenerator::generateImageWithStyle(const QString &prompt, const QString &style)
{
    QString fullPrompt = prompt + ", style: " + style;
    generateImage(fullPrompt);
}

void ImageGenerator::generateWithOpenSource(const QString &prompt, const QString &model)
{
    if (m_isBusy) {
        emit generationFailed("生成器正在处理中，请稍候");
        return;
    }

    m_isBusy = true;
    m_lastError.clear();

    QUrl url(HF_API_URL);
    QNetworkRequest request(url);

    // 设置请求头
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Authorization", "Bearer " + HUGGINGFACE_TOKEN.toUtf8());

    // 构建请求体
    QJsonObject jsonData;
    jsonData["inputs"] = prompt;
    jsonData["options"] = QJsonObject{
        {"wait_for_model", true}
    };

    QJsonDocument jsonDoc(jsonData);
    QByteArray requestData = jsonDoc.toJson();

    qDebug() << "发送开源模型生成请求:" << prompt;

    m_currentReply = m_networkManager->post(request, requestData);

    connect(m_currentReply, &QNetworkReply::finished,
            this, &ImageGenerator::onRequestFinished);
    connect(m_currentReply, &QNetworkReply::uploadProgress,
            this, &ImageGenerator::onProgress);
}

void ImageGenerator::cancelGeneration()
{
    if (m_currentReply && m_currentReply->isRunning()) {
        m_currentReply->abort();
        m_isBusy = false;
        emit generationFailed("生成已取消");
    }
}

bool ImageGenerator::isAvailable() const
{
    return !m_isBusy;
}

QString ImageGenerator::getLastError() const
{
    return m_lastError;
}

void ImageGenerator::onRequestFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) return;

    reply->deleteLater();
    m_currentReply = nullptr;

    if (reply->error() != QNetworkReply::NetworkError::NoError) {
        handleError(reply->error(), reply->errorString());
        return;
    }

    QByteArray responseData = reply->readAll();
    handleResponse(responseData);
}

void ImageGenerator::onProgress(qint64 bytesSent, qint64 bytesTotal)
{
    if (bytesTotal > 0) {
        int progress = (bytesSent * 100) / bytesTotal;
        emit progressUpdated(progress);
    }
}

void ImageGenerator::handleResponse(const QByteArray &data)
{
    QJsonDocument jsonDoc = QJsonDocument::fromJson(data);
    if (jsonDoc.isNull()) {
        m_lastError = "无效的JSON响应";
        emit generationFailed(m_lastError);
        return;
    }

    QJsonObject jsonObj = jsonDoc.object();

    // 处理DeepSeek API响应
    if (jsonObj.contains("data")) {
        QJsonArray dataArray = jsonObj["data"].toArray();
        if (!dataArray.isEmpty()) {
            QJsonObject imageData = dataArray[0].toObject();
            QString imageUrl = imageData["url"].toString();

            // 下载图像
            QUrl url(imageUrl);
            QNetworkRequest request(url);
            m_currentReply = m_networkManager->get(request);
            connect(m_currentReply, &QNetworkReply::finished,
                    this, [this]() {
                        QByteArray imageData = m_currentReply->readAll();
                        if (m_currentReply->error() == QNetworkReply::NoError) {
                            emit generationComplete(m_currentReply->url().toString(), imageData);
                        } else {
                            emit generationFailed("下载图像失败: " + m_currentReply->errorString());
                        }
                        m_isBusy = false;
                    });
            return;
        }
    }

    // 处理Hugging Face API响应（直接返回图像数据）
    // 检查响应是否是图像数据（二进制）
    QBuffer buffer;
    buffer.setData(data);
    buffer.open(QIODevice::ReadOnly);
    QImage image;
    if (image.load(&buffer, "png") || image.load(&buffer, "jpg") || image.load(&buffer, "jpeg")) {
        emit generationComplete("", data);
        m_isBusy = false;
        return;
    }

    m_lastError = "无法解析响应";
    emit generationFailed(m_lastError);
}

void ImageGenerator::handleError(QNetworkReply::NetworkError error, const QString &errorString)
{
    m_isBusy = false;
    m_lastError = errorString;

    QString errorMessage;
    switch (error) {
        case QNetworkReply::NetworkError::ConnectionRefusedError:
            errorMessage = "连接被拒绝，请检查网络";
            break;
        case QNetworkReply::NetworkError::TimeoutError:
            errorMessage = "请求超时，请重试";
            break;
        case QNetworkReply::NetworkError::UnknownNetworkError:
            errorMessage = "未知网络错误";
            break;
        default:
            errorMessage = errorString;
    }

    emit generationFailed(errorMessage);
}

void ImageGenerator::saveImageToFile(const QByteArray &imageData, const QString &path)
{
    QFile file(path);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(imageData);
        file.close();
    }
}
