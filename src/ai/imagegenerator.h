#ifndef IMAGEGENERATOR_H
#define IMAGEGENERATOR_H

#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QJsonObject>
#include <QImage>

class ImageGenerator : public QObject
{
    Q_OBJECT

public:
    explicit ImageGenerator(QObject *parent = nullptr);
    ~ImageGenerator();

    // 使用DeepSeek API生成图像（免费）
    void generateImage(const QString &prompt, const QString &negativePrompt = "");
    void generateImageWithStyle(const QString &prompt, const QString &style);

    // 使用Hugging Face开源模型（免费）
    void generateWithOpenSource(const QString &prompt, const QString &model = "stabilityai/stable-diffusion-2-1");

    // 检查API状态
    bool isAvailable() const;
    QString getLastError() const;

signals:
    // 生成成功信号
    void generationComplete(const QString &imageUrl, const QByteArray &imageData);
    // 生成失败信号
    void generationFailed(const QString &error);
    // 进度信号
    void progressUpdated(int progress);

public slots:
    void cancelGeneration();

private slots:
    void onRequestFinished();
    void onProgress(qint64 bytesSent, qint64 bytesTotal);

private:
    QNetworkAccessManager *m_networkManager;
    QNetworkReply *m_currentReply;
    QString m_lastError;
    bool m_isBusy;

    // API相关配置
    const QString DEEPSEEK_API_URL = "https://api.deepseek.com/v1/images/generations";
    const QString HF_API_URL = "https://api-inference.huggingface.co/models/stabilityai/stable-diffusion-2-1";
    const QString HUGGINGFACE_TOKEN = "hf_your_token_here"; // 需要注册获取

    void initNetwork();
    QString prepareRequestData(const QString &prompt, const QString &negativePrompt = "");
    void handleResponse(const QByteArray &data);
    void handleError(QNetworkReply::NetworkError error, const QString &errorString);
    void saveImageToFile(const QByteArray &imageData, const QString &path);
};

#endif // IMAGEGENERATOR_H