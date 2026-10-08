#include "EditorWidget.h"

#include <QApplication>

/// 程序入口：创建应用与主窗口，命令行带路径时直接打开该文件。
int main(int argc, char *argv[])
{
    QApplication application(argc, argv);

    application.setApplicationName(QStringLiteral("TextEditor"));
    application.setApplicationDisplayName(QStringLiteral("文本编辑器"));

    EditorWidget window;

    window.show();

    if (argc > 1) {
        window.openFileFromCommandLine(QString::fromLocal8Bit(argv[1]));
    }

    return application.exec();
}
