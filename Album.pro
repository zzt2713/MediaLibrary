QT += core gui widgets
QT += sql multimedia multimediawidgets

CONFIG += c++17

# 把警告当错误会让第三方头文件的问题也中断构建，这里只开高等级警告
msvc {
    QMAKE_CXXFLAGS += /W3
} else {
    QMAKE_CXXFLAGS += -Wall -Wextra
}

TARGET   = MediaLibrary
TEMPLATE = app

# 头文件搜索路径：源码里用的是 "media/core/xxx.h" 这种从根开始的相对路径，
# 所以只需要把工程根目录加进来。
INCLUDEPATH += $$PWD

HEADERS += \
    media/core/MediaTypes.h \
    media/core/MediaDatabase.h \
    media/core/MediaScanner.h \
    media/core/ThumbnailCache.h \
    media/core/HashUtil.h \
    media/core/ExifReader.h \
    media/core/AppPaths.h \
    media/core/Logger.h \
    media/core/SettingsManager.h \
    media/core/DuplicateService.h \
    media/core/RenameService.h \
    media/tasks/TaskQueue.h \
    media/tasks/TaskManager.h \
    media/tasks/ScanTask.h \
    media/tasks/ThumbnailTask.h \
    media/tasks/FileWatcher.h \
    media/model/MediaModel.h \
    media/model/MediaFilterProxyModel.h \
    media/ui/ThemeManager.h \
    media/ui/Metrics.h \
    media/ui/Icons.h \
    media/ui/HomePage.h \
    media/ui/SidebarWidget.h \
    media/ui/ThumbnailGrid.h \
    media/ui/DetailPane.h \
    media/ui/PreviewDialog.h \
    media/ui/SettingsDialog.h \
    media/ui/TaskPanelWidget.h \
    media/ui/TopBarWidget.h \
    media/ui/StatusBarWidget.h \
    media/ui/EmptyStateWidget.h \
    media/ui/MainWindow.h

SOURCES += \
    main.cpp \
    media/core/MediaTypes.cpp \
    media/core/MediaDatabase.cpp \
    media/core/MediaScanner.cpp \
    media/core/ThumbnailCache.cpp \
    media/core/HashUtil.cpp \
    media/core/ExifReader.cpp \
    media/core/AppPaths.cpp \
    media/core/Logger.cpp \
    media/core/SettingsManager.cpp \
    media/core/DuplicateService.cpp \
    media/core/RenameService.cpp \
    media/tasks/TaskQueue.cpp \
    media/tasks/TaskManager.cpp \
    media/tasks/ScanTask.cpp \
    media/tasks/ThumbnailTask.cpp \
    media/tasks/FileWatcher.cpp \
    media/model/MediaModel.cpp \
    media/model/MediaFilterProxyModel.cpp \
    media/ui/ThemeManager.cpp \
    media/ui/Icons.cpp \
    media/ui/HomePage.cpp \
    media/ui/SidebarWidget.cpp \
    media/ui/ThumbnailGrid.cpp \
    media/ui/DetailPane.cpp \
    media/ui/PreviewDialog.cpp \
    media/ui/SettingsDialog.cpp \
    media/ui/TaskPanelWidget.cpp \
    media/ui/TopBarWidget.cpp \
    media/ui/StatusBarWidget.cpp \
    media/ui/EmptyStateWidget.cpp \
    media/ui/MainWindow.cpp

RESOURCES += media/resources.qrc

win32 {
    RC_ICONS = icon/head.ico
}

qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target

