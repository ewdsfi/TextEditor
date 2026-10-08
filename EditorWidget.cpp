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
/// 横向滚动条单步滚动的像素数。
const int HorizontalStep = 24;
} // namespace

EditorWidget::EditorWidget(QWidget *parent)
    : QWidget(parent)
{
    // 用代码完成全部排版：菜单栏在上，编辑区居中，状态栏在下
    QVBoxLayout *main = new QVBoxLayout(this);

    main->setContentsMargins(0, 0, 0, 0);
    main->setSpacing(0);

    _view = new EditorView(this);
    _verticalBar = new QScrollBar(Qt::Vertical, this);
    _horizontalBar = new QScrollBar(Qt::Horizontal, this);
    _corner = new QWidget(this);

    _corner->setFixedSize(_verticalBar->sizeHint().width(), _horizontalBar->sizeHint().height());

    QGridLayout *editing = new QGridLayout();

    editing->setContentsMargins(0, 0, 0, 0);
    editing->setSpacing(0);
    editing->addWidget(_view, 0, 0);
    editing->addWidget(_verticalBar, 0, 1);
    editing->addWidget(_corner, 1, 0);
    editing->addWidget(_horizontalBar, 1, 1);
    editing->setRowStretch(0, 1);
    editing->setColumnStretch(0, 1);

    createMenus();
    createStatusBar();

    main->addWidget(_menuBar);
    main->addLayout(editing, 1);
    main->addWidget(_statusBar);

    connect(_view, &EditorView::cursorMoved, this, &EditorWidget::updateStatus);
    connect(_view, &EditorView::cursorMoved, this, &EditorWidget::syncScrollBars);
    connect(_view, &EditorView::documentEdited, this, &EditorWidget::updateTitle);
    connect(_view, &EditorView::documentEdited, this, &EditorWidget::syncScrollBars);
    connect(_verticalBar, &QScrollBar::valueChanged, this, &EditorWidget::onVerticalScroll);
    connect(_horizontalBar, &QScrollBar::valueChanged, this, &EditorWidget::onHorizontalScroll);

    setWindowTitle(QStringLiteral("文本编辑器"));
    resize(1000, 700);
    setMinimumSize(480, 320);

    setDocument(new Document());
}

EditorWidget::~EditorWidget()
{
    delete _document;
}

// ---------------------------------------------------------------------------
// 界面搭建
// ---------------------------------------------------------------------------

void EditorWidget::createMenus()
{
    _menuBar = new QMenuBar(this);
    _fileMenu = _menuBar->addMenu(QStringLiteral("文件"));
    _editMenu = _menuBar->addMenu(QStringLiteral("编辑"));

    QAction *newAction = _fileMenu->addAction(QStringLiteral("新建"));

    newAction->setShortcut(QKeySequence::New);
    connect(newAction, &QAction::triggered, this, &EditorWidget::newFile);

    QAction *openAction = _fileMenu->addAction(QStringLiteral("打开..."));

    openAction->setShortcut(QKeySequence::Open);
    connect(openAction, &QAction::triggered, this, &EditorWidget::openFile);

    QAction *saveAction = _fileMenu->addAction(QStringLiteral("保存"));

    saveAction->setShortcut(QKeySequence::Save);
    connect(saveAction, &QAction::triggered, this, &EditorWidget::saveFile);

    QAction *saveAsAction = _fileMenu->addAction(QStringLiteral("另存为..."));

    saveAsAction->setShortcut(QKeySequence::SaveAs);
    connect(saveAsAction, &QAction::triggered, this, &EditorWidget::saveFileAs);

    _fileMenu->addSeparator();

    _lineEndingAction = _fileMenu->addAction(QStringLiteral("换行风格：LF"));

    connect(_lineEndingAction, &QAction::triggered, this, &EditorWidget::toggleLineEnding);

    _fileMenu->addSeparator();

    QAction *exitAction = _fileMenu->addAction(QStringLiteral("退出"));

    connect(exitAction, &QAction::triggered, this, &QWidget::close);

    QAction *undoAction = _editMenu->addAction(QStringLiteral("撤销"));

    connect(undoAction, &QAction::triggered, _view, &EditorView::undo);

    QAction *redoAction = _editMenu->addAction(QStringLiteral("重做"));

    connect(redoAction, &QAction::triggered, _view, &EditorView::redo);

    _editMenu->addSeparator();

    QAction *cutAction = _editMenu->addAction(QStringLiteral("剪切"));

    connect(cutAction, &QAction::triggered, _view, &EditorView::cut);

    QAction *copyAction = _editMenu->addAction(QStringLiteral("复制"));

    connect(copyAction, &QAction::triggered, _view, &EditorView::copy);

    QAction *pasteAction = _editMenu->addAction(QStringLiteral("粘贴"));

    connect(pasteAction, &QAction::triggered, _view, &EditorView::paste);

    _editMenu->addSeparator();

    QAction *selectAllAction = _editMenu->addAction(QStringLiteral("全选"));

    connect(selectAllAction, &QAction::triggered, _view, &EditorView::selectAll);
}

void EditorWidget::createStatusBar()
{
    _positionLabel = new QLabel(this);
    _infoLabel = new QLabel(this);
    _pathLabel = new QLabel(this);

    _positionLabel->setMinimumWidth(180);
    _positionLabel->setContentsMargins(8, 0, 0, 0);
    _infoLabel->setMinimumWidth(220);
    _pathLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    _pathLabel->setContentsMargins(0, 0, 8, 0);

    _statusBar = new QStatusBar(this);

    _statusBar->setSizeGripEnabled(false);
    _statusBar->addWidget(_positionLabel, 0);
    _statusBar->addWidget(_infoLabel, 0);
    _statusBar->addPermanentWidget(_pathLabel, 1);
    _statusBar->setFixedHeight(24);
}

// ---------------------------------------------------------------------------
// 文档管理
// ---------------------------------------------------------------------------

void EditorWidget::setDocument(Document *document)
{
    delete _document;
    _document = document;
    _view->setDocument(_document);
    _view->setFocus();
    updateTitle();
    updateStatus();
    syncScrollBars();
}

void EditorWidget::loadFile(const QString &path)
{
    if (!confirmSave()) {
        return;
    }

    Document *document = new Document();

    if (!document->loadFromFile(path)) {
        delete document;
        QMessageBox::warning(this, QStringLiteral("打开失败"), QStringLiteral("无法读取文件：\n") + path);

        return;
    }

    setDocument(document);
}

void EditorWidget::openFileFromCommandLine(const QString &path)
{
    Document *document = new Document();

    if (!document->loadFromFile(path)) {
        delete document;

        return;
    }

    setDocument(document);
}

void EditorWidget::newFile()
{
    if (!confirmSave()) {
        return;
    }

    Document *document = new Document();

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

    if (_document->filePath().isEmpty()) {
        saveFileAs();

        return;
    }

    if (!_document->save()) {
        QMessageBox::warning(this,
                             QStringLiteral("保存失败"),
                             QStringLiteral("无法写入文件：\n") + _document->filePath());

        return;
    }

    updateTitle();
}

void EditorWidget::saveFileAs()
{
    if (_document == nullptr) {
        return;
    }

    const QString path = QFileDialog::getSaveFileName(this,
                                                      QStringLiteral("另存为"),
                                                      _document->filePath(),
                                                      QStringLiteral("所有文件 (*.*)"));

    if (path.isEmpty()) {
        return;
    }

    if (!_document->saveAs(path)) {
        QMessageBox::warning(this, QStringLiteral("保存失败"), QStringLiteral("无法写入文件：\n") + path);

        return;
    }

    updateTitle();
}

bool EditorWidget::confirmSave()
{
    if (_document == nullptr || !_document->isModified()) {
        return true;
    }

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

        return !_document->isModified();
    }

    return true;
}

void EditorWidget::toggleLineEnding()
{
    if (_document == nullptr) {
        return;
    }

    const LineEnding next = (_document->lineEnding() == LineEndingLf) ? LineEndingCrLf : LineEndingLf;

    _document->setLineEnding(next);
    _view->resetViewState();
    updateLineEndingAction();
    updateTitle();
    syncScrollBars();
}

// ---------------------------------------------------------------------------
// 状态刷新
// ---------------------------------------------------------------------------

void EditorWidget::updateTitle()
{
    if (_document == nullptr) {
        setWindowTitle(QStringLiteral("文本编辑器"));

        return;
    }

    setWindowTitle(_document->title() + QStringLiteral(" - 文本编辑器"));
    updateStatus();
    updateLineEndingAction();
}

void EditorWidget::updateStatus()
{
    if (_document == nullptr || _positionLabel == nullptr) {
        return;
    }

    const int line = _view->cursorLine() + 1;
    const int column = _view->cursorColumn() + 1;
    const int selected = _view->selectionLength();

    if (selected > 0) {
        _positionLabel->setText(QStringLiteral("行 %1，列 %2（已选 %3 字符）").arg(line).arg(column).arg(selected));
    } else {
        _positionLabel->setText(QStringLiteral("行 %1，列 %2").arg(line).arg(column));
    }

    const uint32_t bytes = _document->buffer()->byteCount();
    const uint32_t lines = _document->buffer()->lineCount();

    _infoLabel->setText(QStringLiteral("%1 行 · %2 字节 · %3")
                            .arg(lines)
                            .arg(bytes)
                            .arg(_document->lineEnding() == LineEndingLf ? QStringLiteral("LF") : QStringLiteral("CRLF")));

    _pathLabel->setText(_document->filePath().isEmpty() ? QStringLiteral("未命名") : _document->filePath());
}

void EditorWidget::updateLineEndingAction()
{
    if (_lineEndingAction == nullptr || _document == nullptr) {
        return;
    }

    _lineEndingAction->setText(_document->lineEnding() == LineEndingLf ? QStringLiteral("换行风格：LF")
                                                                      : QStringLiteral("换行风格：CRLF"));
}

// ---------------------------------------------------------------------------
// 滚动条
// ---------------------------------------------------------------------------

void EditorWidget::syncScrollBars()
{
    if (_syncingBars || _document == nullptr) {
        return;
    }

    _syncingBars = true;

    const int viewHeight = std::max(1, _view->height());
    const int viewWidth = std::max(1, _view->width() - _view->gutterWidth());

    _verticalBar->setRange(0, std::max(0, _view->contentHeight() - viewHeight));
    _verticalBar->setPageStep(viewHeight);
    _verticalBar->setSingleStep(_view->lineHeight() * 3);

    _horizontalBar->setRange(0, std::max(0, _view->contentWidth() - viewWidth));
    _horizontalBar->setPageStep(viewWidth);
    _horizontalBar->setSingleStep(HorizontalStep);

    _syncingBars = false;
}

void EditorWidget::onVerticalScroll(int value)
{
    if (_syncingBars) {
        return;
    }

    _syncingBars = true;
    _view->setScrollOffsets(value, _horizontalBar->value());
    _syncingBars = false;
}

void EditorWidget::onHorizontalScroll(int value)
{
    if (_syncingBars) {
        return;
    }

    _syncingBars = true;
    _view->setScrollOffsets(_verticalBar->value(), value);
    _syncingBars = false;
}

// ---------------------------------------------------------------------------
// 窗口事件
// ---------------------------------------------------------------------------

void EditorWidget::closeEvent(QCloseEvent *event)
{
    if (confirmSave()) {
        event->accept();

        return;
    }

    event->ignore();
}
