#ifndef AIFEATUREDIALOG_H
#define AIFEATUREDIALOG_H

#include <QDialog>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTabWidget>
#include <QPushButton>
#include <QLineEdit>
#include <QTextEdit>
#include <QListWidget>
#include <QLabel>
#include <QProgressBar>
#include <QComboBox>
#include <QSpinBox>
#include <QGroupBox>
#include <QScrollArea>
#include <QSplitter>
#include <QFrame>
#include <QNetworkReply>
#include "ai/imagegenerator.h"
#include "ai/imageprocessor.h"
#include "ai/smartsearch.h"

class AIFeatureDialog : public QDialog
{
    Q_OBJECT

public:
    explicit AIFeatureDialog(QWidget *parent = nullptr);
    ~AIFeatureDialog();

public slots:
    // 关于对话框
    void SlotAbout();

signals:
    void aiFeatureCompleted(const QString &feature, const QString &result);

private slots:
    // AI图像生成相关槽函数
    void onGenerateImageClicked();
    void onStyleSelected(const QString &style);
    void onGeneratorProgress(int progress);
    void onGeneratorComplete(const QString &imageUrl, const QByteArray &imageData);
    void onGeneratorFailed(const QString &error);

    // AI图像处理相关槽函数
    void onProcessImageClicked();
    void onRemoveBackgroundClicked();
    void onEnhanceImageClicked();
    void onStyleTransferClicked();
    void onBatchProcessClicked();
    void onProcessorStatus(const QString &status);
    void onProcessorComplete(const QString &outputPath, const QImage &result);
    void onProcessorFailed(const QString &error);

    // AI智能搜索相关槽函数
    void onSearchClicked();
    void onSearchTypeChanged(int index);
    void onSemanticSearchClicked();
    void onSearchComplete(const QList<SearchResult> &results);
    void onSearchFailed(const QString &error);

    // 其他槽函数
    void onShowResults();
    void onClearResults();
    void onExportResults();

private:
    // 主界面布局
    QTabWidget *m_tabWidget;
    QSplitter *m_mainSplitter;
    QScrollArea *m_resultsScrollArea;

    // AI图像生成页
    QWidget *m_generatePage;
    QLineEdit *m_promptEdit;
    QLineEdit *m_negativePromptEdit;
    QComboBox *m_styleCombo;
    QPushButton *m_generateBtn;
    QPushButton *m_cancelGenerateBtn;
    QProgressBar *m_generateProgress;
    QTextEdit *m_generateResultEdit;
    QLabel *m_generatePreviewLabel;

    // AI图像处理页
    QWidget *m_processPage;
    QListWidget *m_imageListWidget;
    QPushButton *m_addImageBtn;
    QPushButton *m_removeImageBtn;
    QPushButton *m_removeBgBtn;
    QPushButton *m_enhanceBtn;
    QPushButton *m_styleTransferBtn;
    QPushButton *m_batchProcessBtn;
    QComboBox *m_processTypeCombo;
    QLabel *m_processStatusLabel;
    QTextEdit *m_processResultEdit;

    // AI智能搜索页
    QWidget *m_searchPage;
    QLineEdit *m_searchEdit;
    QComboBox *m_searchTypeCombo;
    QPushButton *m_searchBtn;
    QPushButton *m_semanticSearchBtn;
    QListWidget *m_searchResultsList;
    QTextEdit *m_searchDetailEdit;
    SearchFilter *m_searchFilter;

    // AI工具实例
    ImageGenerator *m_imageGenerator;
    ImageProcessor *m_imageProcessor;
    SmartSearch *m_smartSearch;

    // 结果管理
    QList<QPixmap> m_resultImages;
    QStringList m_resultPaths;

    // UI初始化函数
    void initTabWidget();
    void initGeneratePage();
    void initProcessPage();
    void initSearchPage();
    void initConnections();
    void applyModernStyle();
    void showProgress(const QString &message, int progress = 0);
    void showError(const QString &message);
    void showSuccess(const QString &message);

    // 辅助函数
    void addImageToList(const QString &path);
    void updateResultsDisplay();
    void exportResultsToText();


};

#endif // AIFEATUREDIALOG_H
