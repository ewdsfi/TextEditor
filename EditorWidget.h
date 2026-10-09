#ifndef EDITORWIDGET_H
#define EDITORWIDGET_H

#include <QWidget>

class Document;
class EditorView;
class QLabel;
class QMenu;
class QMenuBar;
class QScrollBar;
class QStatusBar;

// ===================== Main window =====================

/// The whole UI is built in code, there is no .ui file anywhere.
/// this window owns the view, the two scroll bars, the menu bar and the status bar
class EditorWidget : public QWidget
{
    Q_OBJECT

public:
    explicit EditorWidget(QWidget *parent = nullptr);   // wires the layout up, installs an empty Document

    ~EditorWidget() override;                           // deletes the document, the widgets are Qt's business

    /// Opens the file given on the command line.
    /// on failure we keep whatever document was already open and say nothing,
    /// a bad argument should not wipe the window clean
    void openFileFromCommandLine(const QString &path);

protected:
    /// Asks about unsaved changes before closing
    void closeEvent(QCloseEvent *event) override;

private slots:
    void syncScrollBars();
    void onVerticalScroll(int value);
    void onHorizontalScroll(int value);
    void newFile();
    void openFile();
    void saveFile();
    void updateStatus();
    void updateTitle();

private:
    void createMenus();
    void createStatusBar();

    void setDocument(Document *document);
    void loadFile(const QString &path);

    /// Asks whether to save, false when the user cancels
    bool confirmSave();

    EditorView *_view = nullptr;
    QScrollBar *_verticalBar = nullptr;
    QScrollBar *_horizontalBar = nullptr;
    QWidget *_corner = nullptr;         // small square between the two scroll bars
    QMenuBar *_menuBar = nullptr;
    QStatusBar *_statusBar = nullptr;
    QMenu *_fileMenu = nullptr;
    QMenu *_editMenu = nullptr;
    QLabel *_positionLabel = nullptr;   // status bar: line, column and selection
    QLabel *_infoLabel = nullptr;       // status bar: line and byte counts
    QLabel *_pathLabel = nullptr;       // status bar: file path
    Document *_document = nullptr;      // owned by the window, freed in setDocument()
    bool _syncingBars = false;          // keeps the scroll bars and the view from triggering each other
};

#endif // EDITORWIDGET_H
