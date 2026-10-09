#include "EditorWidget.h"

#include "Document.h"
#include "EditorView.h"

#include <QAction>
#include <QCloseEvent>
#include <QFileDialog>
#include <QGridLayout>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QScrollBar>
#include <QStatusBar>
#include <QVBoxLayout>

#include <algorithm>

namespace
{
const int HorizontalStep = 24;  // horizontal scroll step in pixels
} // namespace

EditorWidget::EditorWidget(QWidget *parent)
    : QWidget(parent)
{
    // menu bar on top, editor in the middle, status bar at the bottom, all laid out in code
    QVBoxLayout *main = new QVBoxLayout(this);

    main->setContentsMargins(0, 0, 0, 0);
    main->setSpacing(0);   // no gaps, the scroll bars should hug the view

    _view = new EditorView(this);
    _verticalBar = new QScrollBar(Qt::Vertical, this);
    _horizontalBar = new QScrollBar(Qt::Horizontal, this);

    // the little dead square where the two scroll bars meet, without it the
    // bottom right corner of the window looks broken
    _corner = new QWidget(this);

    // steal the sizes of both bars, so the corner lines up with them
    _corner->setFixedSize(_verticalBar->sizeHint().width(), _horizontalBar->sizeHint().height());

    // view at (0,0), vertical bar to its right, horizontal bar below it and the
    // corner in the leftover cell - every widget is placed from code, no .ui file
    QGridLayout *editing = new QGridLayout();

    editing->setContentsMargins(0, 0, 0, 0);
    editing->setSpacing(0);
    editing->addWidget(_view, 0, 0);
    editing->addWidget(_verticalBar, 0, 1);
    editing->addWidget(_corner, 1, 0);
    editing->addWidget(_horizontalBar, 1, 1);

    // only the view is allowed to grow, the two bars keep their own size
    editing->setRowStretch(0, 1);
    editing->setColumnStretch(0, 1);

    createMenus();
    createStatusBar();

    main->addWidget(_menuBar);
    main->addLayout(editing, 1);
    main->addWidget(_statusBar);

    // the view reports cursor moves and edits, the scroll bars report being dragged
    connect(_view, &EditorView::cursorMoved, this, &EditorWidget::updateStatus);
    connect(_view, &EditorView::cursorMoved, this, &EditorWidget::syncScrollBars);
    connect(_view, &EditorView::documentEdited, this, &EditorWidget::updateTitle);
    connect(_view, &EditorView::documentEdited, this, &EditorWidget::syncScrollBars);
    // these two could feed the view right back into the bars forever,
    // _syncingBars is what breaks that loop
    connect(_verticalBar, &QScrollBar::valueChanged, this, &EditorWidget::onVerticalScroll);
    connect(_horizontalBar, &QScrollBar::valueChanged, this, &EditorWidget::onHorizontalScroll);

    setWindowTitle(QStringLiteral("文本编辑器"));
    resize(1000, 700);
    setMinimumSize(480, 320);

    // hand the window a blank document straight away, so the title and the
    // status bar are never left in a half built state
    setDocument(new Document());
}

EditorWidget::~EditorWidget()
{
    // the view and the bars are Qt children and clean themselves up,
    // the document is the only thing we own by hand
    delete _document;
}

// ===================== UI setup =====================

void EditorWidget::createMenus()
{
    // the menu bar hangs off this window, both menus off the menu bar
    _menuBar = new QMenuBar(this);
    _fileMenu = _menuBar->addMenu(QStringLiteral("文件"));
    _editMenu = _menuBar->addMenu(QStringLiteral("编辑"));

    // QKeySequence gives us Ctrl+N without hard coding any key ourselves
    QAction *newAction = _fileMenu->addAction(QStringLiteral("新建"));

    newAction->setShortcut(QKeySequence::New);
    connect(newAction, &QAction::triggered, this, &EditorWidget::newFile);

    QAction *openAction = _fileMenu->addAction(QStringLiteral("打开..."));

    openAction->setShortcut(QKeySequence::Open);
    connect(openAction, &QAction::triggered, this, &EditorWidget::openFile);

    QAction *saveAction = _fileMenu->addAction(QStringLiteral("保存"));

    saveAction->setShortcut(QKeySequence::Save);
    connect(saveAction, &QAction::triggered, this, &EditorWidget::saveFile);

    // keep Quit away from Save, one of them asks questions and the other doesn't
    _fileMenu->addSeparator();

    QAction *exitAction = _fileMenu->addAction(QStringLiteral("退出"));

    // exit only closes the window, closeEvent() does the asking
    connect(exitAction, &QAction::triggered, this, &QWidget::close);

    // the four clipboard actions go straight to the view,
    // this window keeps no selection state of its own
    QAction *cutAction = _editMenu->addAction(QStringLiteral("剪切"));

    connect(cutAction, &QAction::triggered, _view, &EditorView::cut);

    QAction *copyAction = _editMenu->addAction(QStringLiteral("复制"));

    connect(copyAction, &QAction::triggered, _view, &EditorView::copy);

    QAction *pasteAction = _editMenu->addAction(QStringLiteral("粘贴"));

    connect(pasteAction, &QAction::triggered, _view, &EditorView::paste);

    _editMenu->addSeparator();

    // Select All sits behind the separator, it is not a clipboard action
    QAction *selectAllAction = _editMenu->addAction(QStringLiteral("全选"));

    connect(selectAllAction, &QAction::triggered, _view, &EditorView::selectAll);
}

void EditorWidget::createStatusBar()
{
    // three labels: cursor position, buffer stats, and the path on the far right
    _positionLabel = new QLabel(this);
    _infoLabel = new QLabel(this);
    _pathLabel = new QLabel(this);

    // give the first two a floor, so the labels don't jitter while typing
    _positionLabel->setMinimumWidth(180);
    _positionLabel->setContentsMargins(8, 0, 0, 0);
    _infoLabel->setMinimumWidth(220);
    _pathLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    _pathLabel->setContentsMargins(0, 0, 8, 0);

    _statusBar = new QStatusBar(this);

    _statusBar->setSizeGripEnabled(false);   // no size grip, the bar is 24 pixels tall and stays that way
    _statusBar->addWidget(_positionLabel, 0);
    _statusBar->addWidget(_infoLabel, 0);
    _statusBar->addPermanentWidget(_pathLabel, 1);   // eats the leftover room, long paths just clip
    _statusBar->setFixedHeight(24);
}

// ===================== Document management =====================

void EditorWidget::setDocument(Document *document)
{
    // the old document is ours and nobody else keeps a pointer to it
    delete _document;
    _document = document;
    _view->setDocument(_document);
    // typing should just work after a file has been opened
    _view->setFocus();
    updateTitle();
    updateStatus();
    // the new text can be a lot longer, work out the ranges right away
    syncScrollBars();
}

void EditorWidget::loadFile(const QString &path)
{
    // Cancel means stop, the user wanted to keep what is on screen
    if (!confirmSave()) {
        return;
    }

    // build the new document first, a failed load must not destroy the old one
    Document *document = new Document();

    if (!document->loadFromFile(path)) {
        // nothing has been taken over yet, so we can just throw it away
        delete document;
        QMessageBox::warning(this, QStringLiteral("打开失败"), QStringLiteral("无法读取文件：\n") + path);

        return;
    }

    setDocument(document);
}

void EditorWidget::openFileFromCommandLine(const QString &path)
{
    // no confirmSave() here, this runs before the window has anything worth losing
    Document *document = new Document();

    if (!document->loadFromFile(path)) {
        // a bad command line argument should not stop the editor from opening
        delete document;

        return;
    }

    setDocument(document);
}

void EditorWidget::newFile()
{
    // same question as everywhere else, unsaved work comes first
    if (!confirmSave()) {
        return;
    }

    Document *document = new Document();

    // a fresh document has no path at all, that is what makes it untitled
    document->createNew();
    setDocument(document);
}

void EditorWidget::openFile()
{
    if (!confirmSave()) {
        return;
    }

    const QString path = QFileDialog::getOpenFileName(this,
                                                      QStringLiteral("打开文件"),
                                                      QString(),
                                                      QStringLiteral("所有文件 (*.*)"));

    // empty means the user cancelled the dialog, there is nothing to load
    if (path.isEmpty()) {
        return;
    }

    loadFile(path);
}

void EditorWidget::saveFile()
{
    if (_document == nullptr) {
        return;
    }

    // an unnamed document has nowhere to go
    if (_document->filePath().isEmpty()) {
        QMessageBox::warning(this,
                             QStringLiteral("无法保存"),
                             QStringLiteral("当前文档还没有对应的文件，请先打开一个已有文件。"));

        return;
    }

    if (!_document->save()) {
        QMessageBox::warning(this,
                             QStringLiteral("保存失败"),
                             QStringLiteral("无法写入文件：\n") + _document->filePath());

        return;
    }

    // the star in the title is stale now, repaint it
    updateTitle();
}

bool EditorWidget::confirmSave()
{
    if (_document == nullptr || !_document->isModified()) {
        return true;
    }

    // Save, Discard, Cancel - Cancel is the only answer that stops the caller
    const QMessageBox::StandardButton answer = QMessageBox::question(
        this,
        QStringLiteral("未保存的修改"),
        QStringLiteral("「%1」有未保存的修改，是否保存？").arg(_document->title()),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
        QMessageBox::Save);

    if (answer == QMessageBox::Cancel) {
        return false;
    }

    if (answer == QMessageBox::Save) {
        saveFile();

        // saveFile() can fail and pop its own warning, so check the state again
        // instead of trusting the button the user pressed
        return !_document->isModified();
    }

    // Discard, the user knows what they are doing
    return true;
}

// ===================== Status refresh =====================

void EditorWidget::updateTitle()
{
    // no document means a plain window title, no star anywhere
    if (_document == nullptr) {
        setWindowTitle(QStringLiteral("文本编辑器"));

        return;
    }

    setWindowTitle(_document->title() + QStringLiteral(" - 文本编辑器"));
    // the path label lives down in the status bar, keep it in step with the title
    updateStatus();
}

void EditorWidget::updateStatus()
{
    // updateTitle() calls us, so the labels may not exist yet on the very first run
    if (_document == nullptr || _positionLabel == nullptr) {
        return;
    }

    // the view counts from zero, humans count from one
    const int line = _view->cursorLine() + 1;
    const int column = _view->cursorColumn() + 1;
    const int selected = _view->selectionLength();

    // a selection is worth mentioning, otherwise keep the label short
    if (selected > 0) {
        _positionLabel->setText(QStringLiteral("行 %1，列 %2（已选 %3 字符）").arg(line).arg(column).arg(selected));
    } else {
        _positionLabel->setText(QStringLiteral("行 %1，列 %2").arg(line).arg(column));
    }

    // counts come straight from the buffer, we cache nothing here
    const uint32_t bytes = _document->buffer()->byteCount();
    const uint32_t lines = _document->buffer()->lineCount();

    _infoLabel->setText(QStringLiteral("%1 行 · %2 字节").arg(lines).arg(bytes));

    // an unnamed document shows the same placeholder as the title bar
    _pathLabel->setText(_document->filePath().isEmpty() ? QStringLiteral("未命名") : _document->filePath());
}

// ===================== Scroll bars =====================

void EditorWidget::syncScrollBars()
{
    // the guard also covers the case of being called before a document exists
    if (_syncingBars || _document == nullptr) {
        return;
    }

    // from here on the setRange() calls below must not bounce back into the view
    _syncingBars = true;

    // width() and height() can be 0 during startup, max(1, ...) keeps the ranges sane
    const int viewHeight = std::max(1, _view->height());
    const int viewWidth = std::max(1, _view->width() - _view->gutterWidth());

    // the view reports its content size in pixels, the bars work in the same units
    _verticalBar->setRange(0, std::max(0, _view->contentHeight() - viewHeight));
    // one page is exactly one screenful, a single click is three lines
    _verticalBar->setPageStep(viewHeight);
    _verticalBar->setSingleStep(_view->lineHeight() * 3);

    _horizontalBar->setRange(0, std::max(0, _view->contentWidth() - viewWidth));
    _horizontalBar->setPageStep(viewWidth);
    _horizontalBar->setSingleStep(HorizontalStep);

    // release the guard again, real user scrolling has to reach the view
    _syncingBars = false;
}

void EditorWidget::onVerticalScroll(int value)
{
    // our own setRange() / setValue() would come back as a valueChanged(), ignore it
    if (_syncingBars) {
        return;
    }

    // keep the other axis where it is, the view wants both offsets at once
    _syncingBars = true;
    _view->setScrollOffsets(value, _horizontalBar->value());
    _syncingBars = false;
}

void EditorWidget::onHorizontalScroll(int value)
{
    // same guard, the vertical offset has to survive the round trip
    if (_syncingBars) {
        return;
    }

    _syncingBars = true;
    _view->setScrollOffsets(_verticalBar->value(), value);
    _syncingBars = false;
}

// ===================== Window events =====================

void EditorWidget::closeEvent(QCloseEvent *event)
{
    if (confirmSave()) {
        event->accept();

        return;
    }

    // Cancel was pressed, the window stays open and the document stays dirty
    event->ignore();
}
