#include "EditorWidget.h"

#include <QApplication>

// ===================== Entry point =====================

int main(int argc, char *argv[])
{
    QApplication application(argc, argv);

    application.setApplicationName(QStringLiteral("TextEditor"));   // the name the platform sees
    application.setApplicationDisplayName(QStringLiteral("文本编辑器"));

    EditorWidget window;

    window.show();

    // open the file passed on the command line
    if (argc > 1) {
        // no confirmSave() is involved, the window is empty and there is nothing to lose
        window.openFileFromCommandLine(QString::fromLocal8Bit(argv[1]));
    }

    return application.exec();   // blocks until the last window is gone
}
