#include "aifeaturedialog.h"
#include <QFileDialog>
#include <QMessageBox>
#include <QJsonObject>
#include <QJsonDocument>
#include <QPixmap>
#include <QPainter>
#include <QImageReader>
#include <QBuffer>
#include <QDateTime>
#include <QSettings>
#include <QDebug>
#include <QStandardPaths>
#include <QLineEdit>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QUrl>
#include <QHttpMultiPart>

AIFeatureDialog::AIFeatureDialog(QWidget *parent)
    : QDialog(parent)
    , m_imageGenerator(new ImageGenerator(this))
    , m_imageProcessor(new ImageProcessor(this))
    , m_smartSearch(new SmartSearch(this))
{
    setWindowTitle("AI 增强功能");
    setMinimumSize(1000, 700);
    resize(1200, 800);

    initTabWidget();
    initConnections();
    applyModernStyle();
}

AIFeatureDialog::~AIFeatureDialog()
{
}

void AIFeatureDialog::initTabWidget()
{
    m_tabWidget = new QTabWidget(this);

    // 创建三个主要功能页面
    m_generatePage = new QWidget();
    m_processPage = new QWidget();
    m_searchPage = new QWidget();

    // 初始化各个页面
    initGeneratePage();
    initProcessPage();
    initSearchPage();

    // 添加到标签页
    m_tabWidget->addTab(m_generatePage, "🎨 AI图像生成");
    m_tabWidget->addTab(m_processPage, "✨ AI图像处理");
    m_tabWidget->addTab(m_searchPage, "🔍 智能搜索");

    // 主布局
    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->addWidget(m_tabWidget);

    // 结果显示区域
    m_resultsScrollArea = new QScrollArea(this);
    m_resultsScrollArea->setWidgetResizable(true);
    m_resultsScrollArea->setMinimumHeight(150);
    m_resultsScrollArea->setVisible(false);

    QWidget *resultsWidget = new QWidget();
    QVBoxLayout *resultsLayout = new QVBoxLayout(resultsWidget);

    m_processResultEdit = new QTextEdit();
    m_processResultEdit->setReadOnly(true);
    resultsLayout->addWidget(m_processResultEdit);

    m_resultsScrollArea->setWidget(resultsWidget);
    mainLayout->addWidget(m_resultsScrollArea);

    // 底部按钮
    QHBoxLayout *bottomLayout = new QHBoxLayout();

    QPushButton *clearBtn = new QPushButton("清空结果");
    QPushButton *exportBtn = new QPushButton("导出结果");
    QPushButton *closeBtn = new QPushButton("关闭");

    bottomLayout->addStretch();
    bottomLayout->addWidget(clearBtn);
    bottomLayout->addWidget(exportBtn);
    bottomLayout->addWidget(closeBtn);
    bottomLayout->setContentsMargins(10, 10, 10, 10);

    mainLayout->addLayout(bottomLayout);
    setLayout(mainLayout);

    // 连接底部按钮信号
    connect(clearBtn, &QPushButton::clicked, this, &AIFeatureDialog::onClearResults);
    connect(exportBtn, &QPushButton::clicked, this, &AIFeatureDialog::onExportResults);
    connect(closeBtn, &QPushButton::clicked, this, &AIFeatureDialog::accept);
}

void AIFeatureDialog::initGeneratePage()
{
    QVBoxLayout *layout = new QVBoxLayout(m_generatePage);

    // 输入区域
    QGroupBox *inputGroup = new QGroupBox("输入设置", m_generatePage);
    QVBoxLayout *inputLayout = new QVBoxLayout(inputGroup);

    // 提示词输入
    QVBoxLayout *promptLayout = new QVBoxLayout();
    QLabel *promptLabel = new QLabel("描述词 (必填):");
    m_promptEdit = new QLineEdit();
    m_promptEdit->setPlaceholderText("描述你想要生成的图像，例如：'一只可爱的小猫在花园里玩耍'");
    promptLayout->addWidget(promptLabel);
    promptLayout->addWidget(m_promptEdit);
    inputLayout->addLayout(promptLayout);

    // 负面提示词
    QVBoxLayout *negativeLayout = new QVBoxLayout();
    QLabel *negativeLabel = new QLabel("负面提示词 (可选):");
    m_negativePromptEdit = new QLineEdit();
    m_negativePromptEdit->setPlaceholderText("描述你不想要出现在图像中的内容，例如：'模糊、低质量'");
    negativeLayout->addWidget(negativeLabel);
    negativeLayout->addWidget(m_negativePromptEdit);
    inputLayout->addLayout(negativeLayout);

    // 风格选择
    QHBoxLayout *styleLayout = new QHBoxLayout();
    QLabel *styleLabel = new QLabel("艺术风格:");
    m_styleCombo = new QComboBox();
    m_styleCombo->addItems({"写实", "卡通", "油画", "水彩", "素描", "动漫", "科幻"});
    styleLayout->addWidget(styleLabel);
    styleLayout->addWidget(m_styleCombo);
    inputLayout->addLayout(styleLayout);

    layout->addWidget(inputGroup);

    // 控制按钮
    QHBoxLayout *buttonLayout = new QHBoxLayout();
    m_generateBtn = new QPushButton("生成图像");
    m_cancelGenerateBtn = new QPushButton("取消");
    m_cancelGenerateBtn->setEnabled(false);
    buttonLayout->addStretch();
    buttonLayout->addWidget(m_generateBtn);
    buttonLayout->addWidget(m_cancelGenerateBtn);
    layout->addLayout(buttonLayout);

    // 进度条
    m_generateProgress = new QProgressBar();
    m_generateProgress->setRange(0, 100);
    m_generateProgress->setVisible(false);
    layout->addWidget(m_generateProgress);

    // 结果预览
    QGroupBox *resultGroup = new QGroupBox("生成结果", m_generatePage);
    QVBoxLayout *resultLayout = new QVBoxLayout(resultGroup);

    m_generatePreviewLabel = new QLabel();
    m_generatePreviewLabel->setMinimumSize(400, 300);
    m_generatePreviewLabel->setAlignment(Qt::AlignCenter);
    m_generatePreviewLabel->setStyleSheet("background-color: #f0f0f0; border: 1px solid #ddd;");
    m_generatePreviewLabel->setText("生成结果将显示在这里");
    resultLayout->addWidget(m_generatePreviewLabel);

    // 结果详情
    m_generateResultEdit = new QTextEdit();
    m_generateResultEdit->setReadOnly(true);
    m_generateResultEdit->setMaximumHeight(150);
    resultLayout->addWidget(m_generateResultEdit);

    layout->addWidget(resultGroup);
    layout->addStretch();
}

void AIFeatureDialog::initProcessPage()
{
    QVBoxLayout *layout = new QVBoxLayout(m_processPage);

    // 图像列表区域
    QGroupBox *imageGroup = new QGroupBox("待处理图像", m_processPage);
    QVBoxLayout *imageLayout = new QVBoxLayout(imageGroup);

    m_imageListWidget = new QListWidget();
    m_imageListWidget->setSelectionMode(QAbstractItemView::ExtendedSelection);
    imageLayout->addWidget(m_imageListWidget);

    // 图像列表操作按钮
    QHBoxLayout *imageButtonLayout = new QHBoxLayout();
    m_addImageBtn = new QPushButton("添加图像");
    m_removeImageBtn = new QPushButton("移除选中");
    imageButtonLayout->addWidget(m_addImageBtn);
    imageButtonLayout->addWidget(m_removeImageBtn);
    imageLayout->addLayout(imageButtonLayout);

    layout->addWidget(imageGroup);

    // 处理选项
    QGroupBox *processGroup = new QGroupBox("处理选项", m_processPage);
    QVBoxLayout *processLayout = new QVBoxLayout(processGroup);

    // 处理类型
    QHBoxLayout *typeLayout = new QHBoxLayout();
    QLabel *typeLabel = new QLabel("处理类型:");
    m_processTypeCombo = new QComboBox();
    m_processTypeCombo->addItems({"背景移除", "图像增强", "风格转换", "智能标签"});
    typeLayout->addWidget(typeLabel);
    typeLayout->addWidget(m_processTypeCombo);
    processLayout->addLayout(typeLayout);

    // 处理按钮
    QVBoxLayout *processButtonLayout = new QVBoxLayout();
    m_removeBgBtn = new QPushButton("移除背景");
    m_enhanceBtn = new QPushButton("增强图像");
    m_styleTransferBtn = new QPushButton("风格转换");
    m_batchProcessBtn = new QPushButton("批量处理");

    processButtonLayout->addWidget(m_removeBgBtn);
    processButtonLayout->addWidget(m_enhanceBtn);
    processButtonLayout->addWidget(m_styleTransferBtn);
    processButtonLayout->addWidget(m_batchProcessBtn);
    processLayout->addLayout(processButtonLayout);

    layout->addWidget(processGroup);

    // 状态显示
    m_processStatusLabel = new QLabel("就绪");
    m_processStatusLabel->setStyleSheet("color: #666; padding: 5px;");
    layout->addWidget(m_processStatusLabel);

    // 结果区域
    m_processResultEdit = new QTextEdit();
    m_processResultEdit->setReadOnly(true);
    m_processResultEdit->setVisible(false);
    layout->addWidget(m_processResultEdit);
}

void AIFeatureDialog::initSearchPage()
{
    QVBoxLayout *layout = new QVBoxLayout(m_searchPage);

    // 搜索区域
    QGroupBox *searchGroup = new QGroupBox("智能搜索", m_searchPage);
    QVBoxLayout *searchLayout = new QVBoxLayout(searchGroup);

    // 搜索输入
    QHBoxLayout *inputLayout = new QHBoxLayout();
    m_searchEdit = new QLineEdit();
    m_searchEdit->setPlaceholderText("输入搜索关键词、描述或图像文件路径");
    m_searchTypeCombo = new QComboBox();
    m_searchTypeCombo->addItems({"文件名搜索", "内容搜索", "日期搜索", "标签搜索"});
    inputLayout->addWidget(m_searchEdit);
    inputLayout->addWidget(m_searchTypeCombo);
    searchLayout->addLayout(inputLayout);

    // 搜索按钮
    QHBoxLayout *buttonLayout = new QHBoxLayout();
    m_searchBtn = new QPushButton("开始搜索");
    m_semanticSearchBtn = new QPushButton("语义搜索");
    buttonLayout->addStretch();
    buttonLayout->addWidget(m_searchBtn);
    buttonLayout->addWidget(m_semanticSearchBtn);
    searchLayout->addLayout(buttonLayout);

    layout->addWidget(searchGroup);

    // 结果列表
    QGroupBox *resultGroup = new QGroupBox("搜索结果", m_searchPage);
    QVBoxLayout *resultLayout = new QVBoxLayout(resultGroup);

    // 结果列表
    m_searchResultsList = new QListWidget();
    m_searchResultsList->setSelectionMode(QAbstractItemView::SingleSelection);
    resultLayout->addWidget(m_searchResultsList);

    // 结果详情
    m_searchDetailEdit = new QTextEdit();
    m_searchDetailEdit->setReadOnly(true);
    m_searchDetailEdit->setVisible(false);
    resultLayout->addWidget(m_searchDetailEdit);

    layout->addWidget(resultGroup);
    layout->addStretch();
}

void AIFeatureDialog::initConnections()
{
    // AI图像生成信号连接
    connect(m_generateBtn, &QPushButton::clicked, this, &AIFeatureDialog::onGenerateImageClicked);
    connect(m_cancelGenerateBtn, &QPushButton::clicked, this, [this]() {
        m_imageGenerator->cancelGeneration();
    });
    connect(m_styleCombo, &QComboBox::currentTextChanged, this, &AIFeatureDialog::onStyleSelected);
    connect(m_imageGenerator, &ImageGenerator::progressUpdated,
            this, &AIFeatureDialog::onGeneratorProgress);
    connect(m_imageGenerator, &ImageGenerator::generationComplete,
            this, &AIFeatureDialog::onGeneratorComplete);
    connect(m_imageGenerator, &ImageGenerator::generationFailed,
            this, &AIFeatureDialog::onGeneratorFailed);

    // AI图像处理信号连接
    connect(m_addImageBtn, &QPushButton::clicked, this, [this]() {
        QStringList files = QFileDialog::getOpenFileNames(
            this, "选择图像", "", "图像文件 (*.jpg *.jpeg *.png *.bmp *.gif)");
        for (const QString &file : files) {
            addImageToList(file);
        }
    });
    connect(m_removeImageBtn, &QPushButton::clicked, this, [this]() {
        QList<QListWidgetItem*> items = m_imageListWidget->selectedItems();
        for (QListWidgetItem *item : items) {
            delete item;
        }
    });
    connect(m_removeBgBtn, &QPushButton::clicked, this, &AIFeatureDialog::onRemoveBackgroundClicked);
    connect(m_enhanceBtn, &QPushButton::clicked, this, &AIFeatureDialog::onEnhanceImageClicked);
    connect(m_styleTransferBtn, &QPushButton::clicked, this, &AIFeatureDialog::onStyleTransferClicked);
    connect(m_batchProcessBtn, &QPushButton::clicked, this, &AIFeatureDialog::onBatchProcessClicked);
    connect(m_imageProcessor, &ImageProcessor::processingStatus,
            this, &AIFeatureDialog::onProcessorStatus);
    connect(m_imageProcessor, &ImageProcessor::processingComplete,
            this, &AIFeatureDialog::onProcessorComplete);
    connect(m_imageProcessor, &ImageProcessor::processingFailed,
            this, &AIFeatureDialog::onProcessorFailed);

    // AI智能搜索信号连接
    connect(m_searchBtn, &QPushButton::clicked, this, &AIFeatureDialog::onSearchClicked);
    connect(m_searchTypeCombo, &QComboBox::currentIndexChanged, this, &AIFeatureDialog::onSearchTypeChanged);
    connect(m_semanticSearchBtn, &QPushButton::clicked, this, &AIFeatureDialog::onSemanticSearchClicked);
    connect(m_smartSearch, &SmartSearch::searchProgress,
            this, [this](int progress, int total) {
                m_processStatusLabel->setText(QString("搜索进度: %1/%2").arg(progress).arg(total));
            });
    connect(m_smartSearch, &SmartSearch::searchStatus,
            this, [this](const QString &status) {
                m_processStatusLabel->setText(status);
            });
    connect(m_smartSearch, &SmartSearch::searchComplete,
            this, &AIFeatureDialog::onSearchComplete);
    connect(m_smartSearch, &SmartSearch::searchFailed,
            this, &AIFeatureDialog::onSearchFailed);

    // 结果列表信号连接
    connect(m_searchResultsList, &QListWidget::itemSelectionChanged, this, &AIFeatureDialog::onShowResults);
}

void AIFeatureDialog::applyModernStyle()
{
    // 设置现代样式
    QString style = R"(
        QTabWidget::pane {
            border: 1px solid #ccc;
            background-color: #f9f9f9;
            border-radius: 4px;
        }

        QTabWidget::tab-bar {
            left: 5px;
        }

        QTabBar::tab {
            background: #e0e0e0;
            color: #333;
            padding: 8px 20px;
            margin-right: 2px;
            border-radius: 4px 4px 0 0;
        }

        QTabBar::tab:selected {
            background: #4CAF50;
            color: white;
        }

        QGroupBox {
            border: 1px solid #ddd;
            border-radius: 5px;
            margin-top: 10px;
            padding-top: 10px;
        }

        QGroupBox::title {
            subcontrol-origin: margin;
            left: 10px;
            padding: 0 5px 0 5px;
        }

        QPushButton {
            background-color: #4CAF50;
            color: white;
            border: none;
            padding: 8px 16px;
            border-radius: 4px;
            font-weight: bold;
        }

        QPushButton:hover {
            background-color: #45a049;
        }

        QPushButton:disabled {
            background-color: #cccccc;
            color: #666666;
        }

        QLineEdit, QTextEdit, QComboBox {
            border: 1px solid #ddd;
            border-radius: 4px;
            padding: 5px;
        }

        QListWidget {
            border: 1px solid #ddd;
            border-radius: 4px;
        }
    )";

    setStyleSheet(style);
}

void AIFeatureDialog::onGenerateImageClicked()
{
    QString prompt = m_promptEdit->text().trimmed();
    if (prompt.isEmpty()) {
        QMessageBox::warning(this, "提示", "请输入描述词");
        return;
    }

    m_generateProgress->setVisible(true);
    m_generateProgress->setValue(0);
    m_generateBtn->setEnabled(false);
    m_cancelGenerateBtn->setEnabled(true);
    m_generatePreviewLabel->clear();
    m_generateResultEdit->clear();

    // 根据选择的风格生成
    QString selectedStyle = m_styleCombo->currentText();
    if (selectedStyle != "写实") {
        m_imageGenerator->generateImageWithStyle(prompt, selectedStyle);
    } else {
        m_imageGenerator->generateImage(prompt, m_negativePromptEdit->text().trimmed());
    }
}

void AIFeatureDialog::onStyleSelected(const QString &style)
{
    // 可以根据风格更新UI或预设参数
    Q_UNUSED(style);
}

void AIFeatureDialog::onGeneratorProgress(int progress)
{
    m_generateProgress->setValue(progress);
}

void AIFeatureDialog::onGeneratorComplete(const QString &imageUrl, const QByteArray &imageData)
{
    m_generateProgress->setVisible(false);
    m_generateBtn->setEnabled(true);
    m_cancelGenerateBtn->setEnabled(false);

    if (!imageData.isEmpty()) {
        // 显示生成的图像
        QPixmap pixmap;
        if (pixmap.loadFromData(imageData)) {
            m_generatePreviewLabel->setPixmap(pixmap.scaled(400, 300, Qt::KeepAspectRatio, Qt::SmoothTransformation));
            m_generateResultEdit->setPlainText("图像生成成功！\n"
                                              "图像URL: " + imageUrl + "\n"
                                              "图像大小: " + QString::number(imageData.size()) + " 字节");

            // 保存结果
            QString timestamp = QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss");
            QString savePath = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation) +
                              "/AI_Generated_" + timestamp + ".png";
            QFile file(savePath);
            if (file.open(QIODevice::WriteOnly)) {
                file.write(imageData);
                file.close();
                showSuccess("图像已保存到: " + savePath);
            }
        }
    } else {
        m_generateResultEdit->setPlainText("生成完成！请查看图像URL:\n" + imageUrl);
    }
}

void AIFeatureDialog::onGeneratorFailed(const QString &error)
{
    m_generateProgress->setVisible(false);
    m_generateBtn->setEnabled(true);
    m_cancelGenerateBtn->setEnabled(false);
    showError("生成失败: " + error);
}

void AIFeatureDialog::onProcessImageClicked()
{
    // 可以添加单张图像处理的逻辑
    Q_UNUSED(m_processTypeCombo);
}

void AIFeatureDialog::onRemoveBackgroundClicked()
{
    if (m_imageListWidget->count() == 0) {
        QMessageBox::warning(this, "提示", "请先选择要处理的图像");
        return;
    }

    QList<QListWidgetItem*> selectedItems = m_imageListWidget->selectedItems();
    if (selectedItems.isEmpty()) {
        QMessageBox::warning(this, "提示", "请选择要移除背景的图像");
        return;
    }

    QStringList paths;
    for (QListWidgetItem *item : selectedItems) {
        paths.append(item->text());
    }

    m_imageProcessor->batchRemoveBackground(paths);
}

void AIFeatureDialog::onEnhanceImageClicked()
{
    if (m_imageListWidget->count() == 0) {
        QMessageBox::warning(this, "提示", "请先选择要处理的图像");
        return;
    }

    QList<QListWidgetItem*> selectedItems = m_imageListWidget->selectedItems();
    if (selectedItems.isEmpty()) {
        QMessageBox::warning(this, "提示", "请选择要增强的图像");
        return;
    }

    QStringList paths;
    for (QListWidgetItem *item : selectedItems) {
        paths.append(item->text());
    }

    m_imageProcessor->batchEnhanceImages(paths);
}

void AIFeatureDialog::onStyleTransferClicked()
{
    QMessageBox::information(this, "提示", "风格转换功能正在开发中...");
}

void AIFeatureDialog::onBatchProcessClicked()
{
    if (m_imageListWidget->count() == 0) {
        QMessageBox::warning(this, "提示", "没有可处理的图像");
        return;
    }

    QString operation = m_processTypeCombo->currentText();
    QStringList paths;
    for (int i = 0; i < m_imageListWidget->count(); ++i) {
        paths.append(m_imageListWidget->item(i)->text());
    }

    m_imageProcessor->batchProcessImages(paths, operation.toLower().replace(" ", "_"));
}

void AIFeatureDialog::onProcessorStatus(const QString &status)
{
    m_processStatusLabel->setText(status);
}

void AIFeatureDialog::onProcessorComplete(const QString &outputPath, const QImage &result)
{
    if (!outputPath.isEmpty()) {
        showSuccess("处理完成: " + outputPath);
        m_processResultEdit->append("处理成功:\n" + outputPath + "\n");
        updateResultsDisplay();
    }
}

void AIFeatureDialog::onProcessorFailed(const QString &error)
{
    showError("处理失败: " + error);
    m_processResultEdit->append("处理失败:\n" + error + "\n");
}

void AIFeatureDialog::onSearchClicked()
{
    QString query = m_searchEdit->text().trimmed();
    if (query.isEmpty()) {
        QMessageBox::warning(this, "提示", "请输入搜索内容");
        return;
    }

    int searchType = m_searchTypeCombo->currentIndex();
    m_smartSearch->searchByKeyword(query, static_cast<SmartSearch::SearchType>(searchType));
}

void AIFeatureDialog::onSearchTypeChanged(int index)
{
    Q_UNUSED(index);
    // 可以根据搜索类型更新输入提示
}

void AIFeatureDialog::onSemanticSearchClicked()
{
    QString query = m_searchEdit->text().trimmed();
    if (query.isEmpty()) {
        QMessageBox::warning(this, "提示", "请输入搜索内容");
        return;
    }

    m_smartSearch->semanticSearch(query);
}

void AIFeatureDialog::onSearchComplete(const QList<SearchResult> &results)
{
    m_searchResultsList->clear();

    if (results.isEmpty()) {
        m_searchResultsList->addItem("没有找到匹配的结果");
        return;
    }

    for (const SearchResult &result : results) {
        QString displayText = QString("%1 - %2 (%3)")
                            .arg(result.date.toString("yyyy-MM-dd"))
                            .arg(result.title)
                            .arg(QString::number(result.confidence * 100, 'f', 0) + "%");
        QListWidgetItem *item = new QListWidgetItem(displayText);
        item->setData(Qt::UserRole, result.filePath);
        m_searchResultsList->addItem(item);
    }

    m_processStatusLabel->setText(QString("找到 %1 个结果").arg(results.size()));
}

void AIFeatureDialog::onSearchFailed(const QString &error)
{
    showError("搜索失败: " + error);
}

void AIFeatureDialog::onShowResults()
{
    QListWidgetItem *currentItem = m_searchResultsList->currentItem();
    if (currentItem) {
        QString filePath = currentItem->data(Qt::UserRole).toString();
        m_searchDetailEdit->clear();
        m_searchDetailEdit->append("文件路径: " + filePath);
        m_searchDetailEdit->setVisible(true);
    }
}

void AIFeatureDialog::onClearResults()
{
    m_generatePreviewLabel->clear();
    m_generateResultEdit->clear();
    m_processResultEdit->clear();
    m_searchResultsList->clear();
    m_searchDetailEdit->clear();
    m_resultsScrollArea->setVisible(false);
    m_processResultEdit->setVisible(false);
    m_searchDetailEdit->setVisible(false);
    m_processStatusLabel->setText("就绪");
}

void AIFeatureDialog::onExportResults()
{
    exportResultsToText();
}

void AIFeatureDialog::addImageToList(const QString &path)
{
    if (QFile::exists(path)) {
        QListWidgetItem *item = new QListWidgetItem(path);
        m_imageListWidget->addItem(item);
    }
}

void AIFeatureDialog::updateResultsDisplay()
{
    if (!m_processResultEdit->toPlainText().isEmpty()) {
        m_processResultEdit->setVisible(true);
        m_resultsScrollArea->setVisible(true);
    }
}

void AIFeatureDialog::exportResultsToText()
{
    QString timestamp = QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss");
    QString fileName = QString("AI_Results_%1.txt").arg(timestamp);
    QString filePath = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/" + fileName;

    QFile file(filePath);
    if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream out(&file);

        // 生成结果
        out << "=== AI图像生成结果 ===\n";
        out << m_generateResultEdit->toPlainText() + "\n\n";

        // 处理结果
        out << "=== AI图像处理结果 ===\n";
        out << m_processResultEdit->toPlainText() + "\n\n";

        // 搜索结果
        out << "=== 智能搜索结果 ===\n";
        for (int i = 0; i < m_searchResultsList->count(); ++i) {
            QListWidgetItem *item = m_searchResultsList->item(i);
            if (item) {
                out << item->text() << "\n";
            }
        }

        file.close();
        showSuccess("结果已导出到: " + filePath);
    } else {
        showError("导出失败: 无法创建文件");
    }
}

void AIFeatureDialog::showProgress(const QString &message, int progress)
{
    m_processStatusLabel->setText(message);
    if (progress > 0) {
        m_generateProgress->setValue(progress);
    }
}

void AIFeatureDialog::showError(const QString &message)
{
    QMessageBox::critical(this, "错误", message);
}

void AIFeatureDialog::showSuccess(const QString &message)
{
    QMessageBox::information(this, "成功", message);
}

void AIFeatureDialog::SlotAbout()
{
    QDialog aboutDlg(this);
    aboutDlg.setWindowModality(Qt::ApplicationModal);
    aboutDlg.setWindowTitle(tr("关于"));

    aboutDlg.setWindowFlags(Qt::Dialog | Qt::WindowTitleHint | Qt::WindowCloseButtonHint | Qt::MSWindowsFixedSizeDialogHint);

    QLabel* versionLabel = new QLabel("版本: v1.1.1 ", &aboutDlg);
    versionLabel->setStyleSheet("color: #ffffff; font-size: 22px; font-weight: bold; margin-bottom: 10px;");
    versionLabel->setAlignment(Qt::AlignCenter);

    QLabel* infoLabel = new QLabel(&aboutDlg);
    infoLabel->setOpenExternalLinks(true);
    infoLabel->setWordWrap(true);
    infoLabel->setAlignment(Qt::AlignCenter);

    infoLabel->setText(
        "<div style='line-height: 1.5;text-align: left;'>"
        "<span style='font-weight:bold; color:#ffffff;text-align: center;'>更新日志：</span><br>"
        "- 新增：AI智能对话功能<br>"
        "- 新增：视频播放器功能<br>"
        "- 新增：图片列表缩略图切换<br>"
        "- 优化：图片轮播更加丝滑，支持手势拖拽<br>"
        "- 项目链接："
        "<a href='https://github.com/zzt2713/QT_PICTURE' style='color:#409EFF; text-decoration: none;'>访问 GitHub 项目主页</a><br>"
        "<p style='text-align: center;color:#666;'>PICTRUE © 2026 Fool. All rights reserved.</p>"
        "</div>"
        );

    QPushButton* okBtn = new QPushButton("确定", &aboutDlg);
    okBtn->setFixedWidth(120);
    okBtn->setCursor(Qt::PointingHandCursor);
    connect(okBtn, &QPushButton::clicked, &aboutDlg, &QDialog::accept);

    QVBoxLayout* mainLayout = new QVBoxLayout(&aboutDlg);
    mainLayout->addWidget(versionLabel);
    mainLayout->addWidget(infoLabel);
    mainLayout->addStretch();
    mainLayout->addWidget(okBtn, 0, Qt::AlignCenter);
    mainLayout->setContentsMargins(40, 40, 40, 30);

    // 设置样式
    aboutDlg.setStyleSheet(
        "QDialog { background-color: rgb(46, 47, 48); color: #eeeeee; }"
        "QLabel { color: #dddddd; font-family: 'Microsoft YaHei'; font-size: 15px; }"
        "a { color: #ffffff; text-decoration: none; font-weight: bold; }"
        "a:hover { color: #66b1ff; text-decoration: underline; }"
        "QPushButton { background-color: #007acc; color: white; border-radius: 6px; padding: 8px; font-size: 14px; }"
        "QPushButton:hover { background-color: #0099ff; }"
        "QPushButton:pressed { background-color: #005c99; }"
        );

    aboutDlg.adjustSize();

    // 2. 获取主窗口的中心点坐标
    // 注意：使用 frameGeometry() 会比 geometry() 更准确，因为它包含了标题栏
    QRect parentRect = this->frameGeometry();

    int x = parentRect.x() + (parentRect.width() - aboutDlg.width()) / 2;
    int y = parentRect.y() + (parentRect.height() - aboutDlg.height()) / 2;

    // 3. 移动并显示
    aboutDlg.move(x, y);
    aboutDlg.exec();
}
