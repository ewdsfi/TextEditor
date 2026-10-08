#include "EditorView.h"

#include "TextLayout.h"

#include <QApplication>
#include <QClipboard>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>

namespace
{
/// 行号栏的最小位数，保证只有几行时也不会太窄。
const int MinimumDigits = 5;

/// 行号与文本之间的留白。
const int GutterPadding = 10;

/// 文字区左右两侧的额外留白。
const int TextPadding = 6;

/// 内容宽度的滚动上限，超长单行按比例映射到这段范围内滚动。
const int MaxContentWidth = 1024 * 1024;

/// 文本区背景色。
const QColor TextBackground(30, 30, 30);

/// 行号栏背景色。
const QColor GutterBackground(37, 37, 38);

/// 行号颜色。
const QColor GutterForeground(133, 133, 133);

/// 当前行行号颜色。
const QColor GutterActiveForeground(197, 197, 197);

/// 行号栏分隔线颜色。
const QColor GutterSeparator(60, 60, 60);

/// 正文颜色。
const QColor TextForeground(212, 212, 212);

/// 选区填充色。
const QColor SelectionBackground(38, 79, 120);

/// 当前行高亮色。
const QColor CurrentLineBackground(40, 40, 40);

/// 光标颜色。
const QColor CaretColor(174, 175, 173);
} // namespace

EditorView::EditorView(QWidget *parent)
    : QWidget(parent)
{
    _font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    _font.setPointSize(12);
    _font.setFixedPitch(true);
    _gutterFont = _font;
    _gutterFont.setPointSize(std::max(8, _font.pointSize() - 1));

    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::IBeamCursor);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMinimumSize(160, 80);

    const QFontMetricsF metrics(_font);
    _lineHeight = static_cast<int>(metrics.height() + 0.5);

    if (_lineHeight <= 0) {
        _lineHeight = 16;
    }

    _gutterWidth = MinimumDigits * static_cast<int>(metrics.horizontalAdvance(QLatin1Char('0'))) + GutterPadding * 2;

    _layout = new TextLayout();
    _layout->setFontMetrics(metrics);

    _blinkTimer = new QTimer(this);

    connect(_blinkTimer, &QTimer::timeout, this, &EditorView::blinkCursor);
    _blinkTimer->start(500);

    _dragTimer = new QTimer(this);

    connect(_dragTimer, &QTimer::timeout, this, &EditorView::autoScrollOnDrag);
    _dragTimer->setInterval(30);
}

EditorView::~EditorView()
{
    delete _layout;
}

// ---------------------------------------------------------------------------
// 基础设置
// ---------------------------------------------------------------------------

void EditorView::setDocument(Document *document)
{
    _document = document;
    resetViewState();
}

void EditorView::setEditorFont(const QFont &font)
{
    _font = font;
    _font.setFixedPitch(true);
    _gutterFont = _font;
    _gutterFont.setPointSize(std::max(8, _font.pointSize() - 1));

    const QFontMetricsF metrics(_font);
    _lineHeight = static_cast<int>(metrics.height() + 0.5);

    if (_lineHeight <= 0) {
        _lineHeight = 16;
    }

    _gutterWidth = MinimumDigits * static_cast<int>(metrics.horizontalAdvance(QLatin1Char('0'))) + GutterPadding * 2;
    _layout->setFontMetrics(metrics);
    update();
}

void EditorView::setCursorBlinkRate(int perSecond)
{
    _blinkRate = perSecond;

    if (perSecond <= 0) {
        _blinkTimer->stop();
        _cursorVisible = true;
        update();

        return;
    }

    _blinkTimer->start(1000 / perSecond);
    restartBlink();
}

void EditorView::resetViewState()
{
    _scrollX = 0;
    _scrollY = 0;
    _cursorLine = 0;
    _cursorColumn = 0;
    _preferredColumn = 0;
    _hasAnchor = false;
    _anchorLine = 0;
    _anchorColumn = 0;
    _clickCount = 0;
    _dragging = false;
    _dragTimer->stop();
    restartBlink();
    update();

    emit cursorMoved();
}

void EditorView::refreshLayout()
{
    update();
}

// ---------------------------------------------------------------------------
// 滚动与尺寸
// ---------------------------------------------------------------------------

void EditorView::setScrollOffsets(int y, int x)
{
    const int maximumY = std::max(0, contentHeight() - height());
    const int maximumX = std::max(0, contentWidth() - std::max(1, width() - _gutterWidth - TextPadding));

    const int nextY = std::min(std::max(0, y), maximumY);
    const int nextX = std::min(std::max(0, x), maximumX);

    if (nextY == _scrollY && nextX == _scrollX) {
        return;
    }

    _scrollY = nextY;
    _scrollX = nextX;
    update();
}

void EditorView::scrollBy(int dy)
{
    setScrollOffsets(_scrollY + dy, _scrollX);
}

void EditorView::ensureCursorVisible(bool centerVertically)
{
    if (_document == nullptr) {
        return;
    }

    const int caretY = yForLine(_cursorLine);
    const int maximumY = std::max(0, contentHeight() - height());

    if (centerVertically) {
        _scrollY = std::min(std::max(0, caretY - height() / 2), maximumY);
    } else if (caretY < _scrollY) {
        _scrollY = caretY;
    } else if (caretY + _lineHeight > _scrollY + height()) {
        _scrollY = caretY + _lineHeight - height();
    }

    prepareLine(_cursorLine);

    const double caretX = _layout->xForColumn(_cursorColumn);
    const int viewWidth = std::max(1, width() - _gutterWidth - TextPadding);

    if (caretX < static_cast<double>(_scrollX)) {
        _scrollX = static_cast<int>(caretX);
    } else if (caretX + 4.0 > static_cast<double>(_scrollX + viewWidth)) {
        _scrollX = static_cast<int>(caretX + 4.0) - viewWidth;
    }

    const int maximumX = std::max(0, contentWidth() - viewWidth);
    _scrollX = std::min(std::max(0, _scrollX), maximumX);
    _scrollY = std::min(std::max(0, _scrollY), maximumY);
}

int EditorView::contentHeight()
{
    if (_document == nullptr) {
        return _lineHeight;
    }

    const int64_t lines = static_cast<int64_t>(_document->buffer()->lineCount());
    const int64_t total = lines * _lineHeight;

    return static_cast<int>(std::min<int64_t>(total, 0x7FFFFFF0));
}

int EditorView::contentWidth()
{
    if (_document == nullptr) {
        return 0;
    }

    const uint32_t bytes = _document->maxLineBytes();
    const double approximate = static_cast<double>(bytes) * 8.0;

    return static_cast<int>(std::min<double>(std::max(approximate, _layout->maxWidth()), MaxContentWidth));
}

int EditorView::visibleRows() const
{
    return std::max(1, height() / _lineHeight);
}

// ---------------------------------------------------------------------------
// 光标与选区
// ---------------------------------------------------------------------------

bool EditorView::hasSelection() const
{
    return _hasAnchor && (_anchorLine != _cursorLine || _anchorColumn != _cursorColumn);
}

TextPosition EditorView::selectionStart() const
{
    TextPosition position;

    if (!hasSelection()) {
        position.line = static_cast<uint32_t>(_cursorLine);
        position.column = static_cast<uint32_t>(_cursorColumn);

        return position;
    }

    const bool anchorFirst = (_anchorLine < _cursorLine)
                             || (_anchorLine == _cursorLine && _anchorColumn <= _cursorColumn);

    position.line = static_cast<uint32_t>(anchorFirst ? _anchorLine : _cursorLine);
    position.column = static_cast<uint32_t>(anchorFirst ? _anchorColumn : _cursorColumn);

    return position;
}

TextPosition EditorView::selectionEnd() const
{
    TextPosition position;

    if (!hasSelection()) {
        position.line = static_cast<uint32_t>(_cursorLine);
        position.column = static_cast<uint32_t>(_cursorColumn);

        return position;
    }

    const bool anchorFirst = (_anchorLine < _cursorLine)
                             || (_anchorLine == _cursorLine && _anchorColumn <= _cursorColumn);

    position.line = static_cast<uint32_t>(anchorFirst ? _cursorLine : _anchorLine);
    position.column = static_cast<uint32_t>(anchorFirst ? _cursorColumn : _anchorColumn);

    return position;
}

int EditorView::selectionLength() const
{
    if (_document == nullptr || !hasSelection()) {
        return 0;
    }

    const TextPosition start = selectionStart();
    const TextPosition end = selectionEnd();
    const TextBuffer *buffer = _document->buffer();
    const uint32_t from = buffer->offsetAt(start.line, start.column);
    const uint32_t to = buffer->offsetAt(end.line, end.column);

    if (to <= from) {
        return 0;
    }

    return static_cast<int>(std::min<uint32_t>(to - from, 0x7FFFFFF0));
}

void EditorView::selectAll()
{
    if (_document == nullptr) {
        return;
    }

    _anchorLine = 0;
    _anchorColumn = 0;
    _hasAnchor = true;
    _cursorLine = static_cast<int>(_document->buffer()->lineCount()) - 1;
    _cursorColumn = lineUnits(_cursorLine);
    _preferredColumn = _cursorColumn;
    restartBlink();
    ensureCursorVisible();
    update();

    emit cursorMoved();
}

void EditorView::clearSelection()
{
    _hasAnchor = false;
    update();

    emit cursorMoved();
}

QString EditorView::selectedText() const
{
    if (_document == nullptr || !hasSelection()) {
        return QString();
    }

    const TextPosition start = selectionStart();
    const TextPosition end = selectionEnd();
    const TextBuffer *buffer = _document->buffer();
    const uint32_t from = buffer->offsetAt(start.line, start.column);
    const uint32_t to = buffer->offsetAt(end.line, end.column);

    if (to <= from) {
        return QString();
    }

    const std::string text = buffer->getTextRange(from, to);

    return QString::fromUtf8(text.data(), static_cast<int>(text.size()));
}

uint32_t EditorView::cursorOffset() const
{
    if (_document == nullptr) {
        return 0;
    }

    return _document->buffer()->offsetAt(static_cast<uint32_t>(_cursorLine), static_cast<uint32_t>(_cursorColumn));
}

void EditorView::moveCursorTo(int line, int column, bool extend)
{
    setCursorPosition(line, column, extend);
    restartBlink();
    ensureCursorVisible();
    update();

    emit cursorMoved();
}

void EditorView::moveCursorToOffset(uint32_t offset, bool extend)
{
    if (_document == nullptr) {
        return;
    }

    const int previousLine = _cursorLine;
    const int previousColumn = _cursorColumn;
    const TextPosition position = _document->buffer()->positionAt(offset);

    if (extend && !_hasAnchor) {
        _hasAnchor = true;
        _anchorLine = previousLine;
        _anchorColumn = previousColumn;
    } else if (!extend) {
        _hasAnchor = false;
    }

    _cursorLine = static_cast<int>(position.line);
    prepareLine(_cursorLine);
    _cursorColumn = std::min(static_cast<int>(position.column), _layout->totalUnits());
    _preferredColumn = _cursorColumn;

    restartBlink();
    ensureCursorVisible();
    update();

    emit cursorMoved();
}

void EditorView::setCursorPosition(int line, int column, bool extend)
{
    if (_document == nullptr) {
        return;
    }

    const int lineCount = static_cast<int>(_document->buffer()->lineCount());
    const int targetLine = std::min(std::max(0, line), lineCount - 1);

    prepareLine(targetLine);

    const int maximumColumn = _layout->totalUnits();
    const int targetColumn = std::min(std::max(0, column), maximumColumn);

    if (extend) {
        if (!_hasAnchor) {
            _hasAnchor = true;
            _anchorLine = _cursorLine;
            _anchorColumn = _cursorColumn;
        }
    } else {
        _hasAnchor = false;
    }

    _cursorLine = targetLine;
    _cursorColumn = targetColumn;
    _preferredColumn = targetColumn;
}

void EditorView::moveCursorBy(int lineDelta, int columnDelta, bool extend)
{
    if (_document == nullptr) {
        return;
    }

    const int lineCount = static_cast<int>(_document->buffer()->lineCount());

    if (lineDelta != 0) {
        const int targetLine = std::min(std::max(0, _cursorLine + lineDelta), lineCount - 1);

        prepareLine(targetLine);

        // 竖向移动时保留首选列，目标行较短则贴到行尾
        const int column = std::min(_preferredColumn, _layout->totalUnits());
        setCursorPosition(targetLine, column, extend);
        _preferredColumn = column;
    } else {
        prepareLine(_cursorLine);

        const int column = std::min(std::max(0, _cursorColumn + columnDelta), _layout->totalUnits());
        setCursorPosition(_cursorLine, column, extend);
    }

    restartBlink();
    ensureCursorVisible();
    update();

    emit cursorMoved();
}

// ---------------------------------------------------------------------------
// 编辑
// ---------------------------------------------------------------------------

void EditorView::insertTextAtCursor(const QString &text)
{
    if (_document == nullptr) {
        return;
    }

    QString normalized = text;
    normalized.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    normalized.replace(QLatin1Char('\r'), QLatin1Char('\n'));

    const TextPosition start = selectionStart();
    const TextPosition end = selectionEnd();
    const uint32_t startOffset = _document->buffer()->offsetAt(start.line, start.column);
    const uint32_t endOffset = _document->buffer()->offsetAt(end.line, end.column);

    if (hasSelection() && endOffset > startOffset) {
        _document->setEditCursor(startOffset);
        _document->removeRange(startOffset, endOffset);
    }

    if (normalized.isEmpty()) {
        moveCursorToOffset(startOffset, false);
        update();

        emit documentEdited();

        return;
    }

    const QByteArray utf8 = normalized.toUtf8();
    const std::string payload(utf8.constData(), static_cast<size_t>(utf8.size()));

    _document->setEditCursor(startOffset);
    _document->insertText(startOffset, payload);

    moveCursorToOffset(startOffset + static_cast<uint32_t>(payload.size()), false);
    update();

    emit documentEdited();
}

void EditorView::insertNewLine()
{
    insertTextAtCursor(QStringLiteral("\n"));
}

void EditorView::backspace()
{
    if (_document == nullptr) {
        return;
    }

    if (hasSelection()) {
        insertTextAtCursor(QString());

        return;
    }

    const uint32_t offset = cursorOffset();

    if (offset == 0) {
        return;
    }

    uint32_t length = bytesBeforeColumn(_cursorLine, _cursorColumn);

    if (length == 0 || length > offset) {
        length = 1;
    }

    _document->setEditCursor(offset);
    _document->removeRange(offset - length, offset);
    moveCursorToOffset(offset - length, false);
    update();

    emit documentEdited();
}

void EditorView::deleteForward()
{
    if (_document == nullptr) {
        return;
    }

    if (hasSelection()) {
        insertTextAtCursor(QString());

        return;
    }

    const uint32_t offset = cursorOffset();

    if (offset >= _document->buffer()->byteCount()) {
        return;
    }

    uint32_t length = bytesAfterColumn(_cursorLine, _cursorColumn);

    if (length == 0) {
        length = 1;
    }

    _document->setEditCursor(offset);
    _document->removeRange(offset, offset + length);
    restartBlink();
    update();

    emit documentEdited();
}

void EditorView::deleteWordBefore()
{
    if (_document == nullptr) {
        return;
    }

    if (hasSelection()) {
        insertTextAtCursor(QString());

        return;
    }

    const uint32_t offset = cursorOffset();

    if (offset == 0) {
        return;
    }

    const QString text = lineText(_cursorLine);
    int column = std::min(_cursorColumn, static_cast<int>(text.size()));

    while (column > 0 && !isWordChar(text.at(column - 1))) {
        column--;
    }

    while (column > 0 && isWordChar(text.at(column - 1))) {
        column--;
    }

    const uint32_t target = _document->buffer()->offsetAt(static_cast<uint32_t>(_cursorLine),
                                                         static_cast<uint32_t>(column));

    _document->setEditCursor(offset);
    _document->removeRange(target, offset);
    moveCursorToOffset(target, false);
    update();

    emit documentEdited();
}

// ---------------------------------------------------------------------------
// 剪贴板与撤销
// ---------------------------------------------------------------------------

void EditorView::copy()
{
    if (!hasSelection()) {
        return;
    }

    QApplication::clipboard()->setText(selectedText());
}

void EditorView::cut()
{
    if (!hasSelection()) {
        return;
    }

    copy();
    insertTextAtCursor(QString());
}

void EditorView::paste()
{
    if (_document == nullptr) {
        return;
    }

    const QString text = QApplication::clipboard()->text();

    if (text.isEmpty()) {
        return;
    }

    insertTextAtCursor(text);
}

void EditorView::undo()
{
    if (_document == nullptr || !_document->canUndo()) {
        return;
    }

    _document->undo();
    moveCursorToOffset(_document->editCursor(), false);
    update();

    emit documentEdited();
}

void EditorView::redo()
{
    if (_document == nullptr || !_document->canRedo()) {
        return;
    }

    _document->redo();
    moveCursorToOffset(_document->editCursor(), false);
    update();

    emit documentEdited();
}

// ---------------------------------------------------------------------------
// 绘制
// ---------------------------------------------------------------------------

void EditorView::paintEvent(QPaintEvent *event)
{
    QPainter painter(this);
    painter.setFont(_font);
    painter.fillRect(event->rect(), TextBackground);

    int lineCount = 1;

    if (_document != nullptr) {
        lineCount = static_cast<int>(_document->buffer()->lineCount());
    }

    // 行号位数变化时同步调整行号栏宽度
    int digits = MinimumDigits;

    for (int value = lineCount; value >= 10; value /= 10) {
        digits++;
    }

    const int wantedGutter = digits * static_cast<int>(QFontMetricsF(_font).horizontalAdvance(QLatin1Char('0')))
                             + GutterPadding * 2;

    if (wantedGutter != _gutterWidth) {
        _gutterWidth = wantedGutter;
    }

    const QRect gutterRect(0, 0, _gutterWidth, height());
    const QRect textRect(_gutterWidth, 0, std::max(0, width() - _gutterWidth), height());
    const int firstLine = std::max(0, _scrollY / _lineHeight);
    const int lastLine = std::min(lineCount - 1, (_scrollY + height()) / _lineHeight);
    const int textShift = _gutterWidth + TextPadding - _scrollX;
    const int lineEnd = std::max(_gutterWidth, textRect.right());

    painter.fillRect(gutterRect, GutterBackground);
    painter.setClipRect(textRect);

    const TextPosition selectionFrom = selectionStart();
    const TextPosition selectionTo = selectionEnd();
    const bool selecting = hasSelection();
    const bool focused = hasFocus();

    for (int line = firstLine; line <= lastLine; line++) {
        const int y = yForLine(line) - _scrollY;
        QString text;
        uint32_t byteLength = 0;

        if (_document != nullptr) {
            text = lineText(line);
            byteLength = lineByteLength(line);
        }

        _layout->setLine(static_cast<uint32_t>(line), byteLength, text);

        const double lineWidth = _layout->lineWidth();

        if (!selecting && line == _cursorLine) {
            painter.fillRect(QRect(textRect.left(), y, textRect.width(), _lineHeight), CurrentLineBackground);
        }

        if (selecting && line >= static_cast<int>(selectionFrom.line) && line <= static_cast<int>(selectionTo.line)) {
            int from = 0;
            int to = _layout->totalUnits();

            if (line == static_cast<int>(selectionFrom.line)) {
                from = static_cast<int>(selectionFrom.column);
            }

            if (line == static_cast<int>(selectionTo.line)) {
                to = static_cast<int>(selectionTo.column);
            }

            const double left = _layout->xForColumn(from);
            double right = _layout->xForColumn(to);

            // 跨行选区在行尾补一小段宽度，视觉上把换行符也纳入选区
            if (line == static_cast<int>(selectionTo.line) && static_cast<int>(selectionTo.column) >= _layout->totalUnits()) {
                right += 6.0;
            }

            const int x1 = textShift + static_cast<int>(left);
            const int x2 = std::min(textShift + static_cast<int>(right), lineEnd);

            if (x2 > x1) {
                painter.fillRect(QRect(x1, y, x2 - x1, _lineHeight), SelectionBackground);
            }
        }

        if (lineWidth > 0.0 || !text.isEmpty()) {
            painter.setPen(TextForeground);
            painter.drawText(QPointF(textShift, y + QFontMetrics(_font).ascent()), text);
        }
    }

    // 光标：失焦时常亮，聚焦时按闪烁状态绘制
    if (_document != nullptr && (_cursorVisible || !focused)) {
        prepareLine(_cursorLine);

        const double caretX = _layout->xForColumn(_layout->clampColumn(_cursorColumn));
        const int x = _gutterWidth + TextPadding + static_cast<int>(caretX) - _scrollX;
        const int y = yForLine(_cursorLine) - _scrollY;

        painter.fillRect(QRect(x, y, 2, _lineHeight), CaretColor);
    }

    // 行号栏
    painter.setClipRect(gutterRect);
    painter.setFont(_gutterFont);
    painter.setPen(GutterSeparator);
    painter.drawLine(_gutterWidth - 1, 0, _gutterWidth - 1, height());

    const QFontMetrics gutterMetrics(_gutterFont);
    const int numberHeight = gutterMetrics.ascent() + gutterMetrics.descent();
    const int numberRight = _gutterWidth - GutterPadding;

    for (int line = firstLine; line <= lastLine; line++) {
        const int y = yForLine(line) - _scrollY;
        const int boxTop = y + (_lineHeight - numberHeight) / 2;
        const QString number = QString::number(line + 1);

        painter.setPen(line == _cursorLine ? GutterActiveForeground : GutterForeground);
        painter.drawText(QRect(0, boxTop, numberRight, numberHeight),
                         Qt::AlignRight | Qt::AlignVCenter,
                         number);
    }

    painter.setClipping(false);
}

void EditorView::updateTextArea()
{
    update();

    emit cursorMoved();
}

void EditorView::updateCursorRect()
{
    update();
}

// ---------------------------------------------------------------------------
// 事件
// ---------------------------------------------------------------------------

void EditorView::keyPressEvent(QKeyEvent *event)
{
    if (_document == nullptr) {
        QWidget::keyPressEvent(event);

        return;
    }

    const bool extend = event->modifiers().testFlag(Qt::ShiftModifier);
    const bool control = event->modifiers().testFlag(Qt::ControlModifier)
                         || event->modifiers().testFlag(Qt::MetaModifier);
    const int key = event->key();

    if (control) {
        switch (key) {
        case Qt::Key_A:
            selectAll();

            return;
        case Qt::Key_C:
            copy();

            return;
        case Qt::Key_X:
            cut();

            return;
        case Qt::Key_V:
            paste();

            return;
        case Qt::Key_Z:
            undo();

            return;
        case Qt::Key_Y:
            redo();

            return;
        case Qt::Key_Home:
            moveCursorTo(0, 0, extend);

            return;
        case Qt::Key_End:
            moveCursorTo(static_cast<int>(_document->buffer()->lineCount()) - 1, 0, extend);
            ensureCursorVisible();

            return;
        case Qt::Key_Backspace:
            deleteWordBefore();

            return;
        default:
            break;
        }
    }

    switch (key) {
    case Qt::Key_Left:
        moveCursorBy(0, -1, extend);

        return;
    case Qt::Key_Right:
        moveCursorBy(0, 1, extend);

        return;
    case Qt::Key_Up:
        moveCursorBy(-1, 0, extend);

        return;
    case Qt::Key_Down:
        moveCursorBy(1, 0, extend);

        return;
    case Qt::Key_Home:
        moveCursorTo(_cursorLine, 0, extend);

        return;
    case Qt::Key_End:
        moveCursorTo(_cursorLine, lineUnits(_cursorLine), extend);

        return;
    case Qt::Key_PageUp:
        moveCursorTo(std::max(0, _cursorLine - visibleRows()), _cursorColumn, extend);
        ensureCursorVisible();

        return;
    case Qt::Key_PageDown:
        moveCursorTo(std::min(static_cast<int>(_document->buffer()->lineCount()) - 1,
                              _cursorLine + visibleRows()),
                     _cursorColumn,
                     extend);
        ensureCursorVisible();

        return;
    case Qt::Key_Backspace:
        backspace();

        return;
    case Qt::Key_Delete:
        deleteForward();

        return;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        insertNewLine();

        return;
    case Qt::Key_Tab:
        insertTextAtCursor(QStringLiteral("\t"));

        return;
    default:
        break;
    }

    const QString text = event->text();

    if (!text.isEmpty() && text.at(0).unicode() >= 0x20 && text.at(0) != QChar(0x7F)) {
        insertTextAtCursor(text);

        return;
    }

    QWidget::keyPressEvent(event);
}

void EditorView::mousePressEvent(QMouseEvent *event)
{
    if (_document == nullptr || event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);

        return;
    }

    setFocus(Qt::MouseFocusReason);
    _clickCount++;

    if (_clickCount > 3) {
        _clickCount = 1;
    }

    const TextPosition position = positionAtPoint(event->pos());
    const int line = static_cast<int>(position.line);
    const int column = static_cast<int>(position.column);

    if (_clickCount >= 3) {
        _hasAnchor = true;
        _anchorLine = line;
        _anchorColumn = 0;
        _cursorLine = line;
        _cursorColumn = lineUnits(line);
        _preferredColumn = _cursorColumn;
    } else if (_clickCount == 2) {
        selectWordAt(line, column);
    } else {
        setCursorPosition(line, column, false);
    }

    _dragging = true;
    _dragPoint = event->pos();
    restartBlink();
    ensureCursorVisible();
    update();

    emit cursorMoved();
}

void EditorView::mouseMoveEvent(QMouseEvent *event)
{
    if (!_dragging) {
        QWidget::mouseMoveEvent(event);

        return;
    }

    _dragPoint = event->pos();

    const bool outside = event->pos().y() < 0 || event->pos().y() > height() || event->pos().x() < 0
                         || event->pos().x() > width();

    if (outside && !_dragTimer->isActive()) {
        _dragTimer->start();
    } else if (!outside && _dragTimer->isActive()) {
        _dragTimer->stop();
    }

    const TextPosition position = positionAtPoint(event->pos());

    if (!_hasAnchor) {
        _hasAnchor = true;
        _anchorLine = _cursorLine;
        _anchorColumn = _cursorColumn;
    }

    setCursorPosition(static_cast<int>(position.line), static_cast<int>(position.column), true);
    _preferredColumn = _cursorColumn;
    ensureCursorVisible();
    update();

    emit cursorMoved();
}

void EditorView::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(event);

        return;
    }

    _dragging = false;
    _dragTimer->stop();
}

void EditorView::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (_document == nullptr || event->button() != Qt::LeftButton) {
        QWidget::mouseDoubleClickEvent(event);

        return;
    }

    const TextPosition position = positionAtPoint(event->pos());
    selectWordAt(static_cast<int>(position.line), static_cast<int>(position.column));
    _clickCount = 2;
    _dragging = true;
    _dragPoint = event->pos();
    restartBlink();
    update();

    emit cursorMoved();
}

void EditorView::wheelEvent(QWheelEvent *event)
{
    const int steps = event->angleDelta().y() / 120;
    const int sideways = event->angleDelta().x() / 120;

    if (steps != 0) {
        scrollBy(-steps * 3 * _lineHeight);
    }

    if (sideways != 0) {
        const int step = std::max(1, QFontMetrics(_font).horizontalAdvance(QLatin1Char(' '))) * 3;
        setScrollOffsets(_scrollY, _scrollX - sideways * step);
    }

    event->accept();
}

void EditorView::focusInEvent(QFocusEvent *event)
{
    QWidget::focusInEvent(event);
    restartBlink();
    update();
}

void EditorView::focusOutEvent(QFocusEvent *event)
{
    QWidget::focusOutEvent(event);
    _cursorVisible = true;
    update();
}

void EditorView::blinkCursor()
{
    if (_blinkRate <= 0 || !hasFocus()) {
        return;
    }

    _cursorVisible = !_cursorVisible;
    updateCursorRect();
}

void EditorView::autoScrollOnDrag()
{
    if (!_dragging) {
        _dragTimer->stop();

        return;
    }

    const int margin = _lineHeight;

    if (_dragPoint.y() < margin) {
        scrollBy(-_lineHeight);
    } else if (_dragPoint.y() > height() - margin) {
        scrollBy(_lineHeight);
    }

    if (_dragPoint.x() < _gutterWidth + margin) {
        setScrollOffsets(_scrollY, _scrollX - 16);
    } else if (_dragPoint.x() > width() - margin) {
        setScrollOffsets(_scrollY, _scrollX + 16);
    }

    const TextPosition position = positionAtPoint(_dragPoint);
    setCursorPosition(static_cast<int>(position.line), static_cast<int>(position.column), true);
    update();

    emit cursorMoved();
}

// ---------------------------------------------------------------------------
// 坐标换算
// ---------------------------------------------------------------------------

TextPosition EditorView::positionAtPoint(const QPoint &point) const
{
    TextPosition position;

    if (_document == nullptr) {
        return position;
    }

    const int lineCount = static_cast<int>(_document->buffer()->lineCount());
    const int line = std::min(std::max(0, (point.y() + _scrollY) / _lineHeight), lineCount - 1);
    const int x = point.x() - _gutterWidth - TextPadding + _scrollX;

    EditorView *self = const_cast<EditorView *>(this);

    self->prepareLine(line);

    position.line = static_cast<uint32_t>(line);
    position.column = static_cast<uint32_t>(std::max(0, _layout->columnForX(static_cast<double>(x))));

    return position;
}

int EditorView::yForLine(int line) const
{
    return line * _lineHeight;
}

void EditorView::prepareLine(int line)
{
    if (_document == nullptr) {
        return;
    }

    const int lineCount = static_cast<int>(_document->buffer()->lineCount());
    const int target = std::min(std::max(0, line), lineCount - 1);

    _layout->setLine(static_cast<uint32_t>(target), lineByteLength(target), lineText(target));
}

QString EditorView::lineText(int line)
{
    if (_document == nullptr) {
        return QString();
    }

    const int lineCount = static_cast<int>(_document->buffer()->lineCount());

    if (line < 0 || line >= lineCount) {
        return QString();
    }

    return _document->lineText(line);
}

uint32_t EditorView::lineByteLength(int line)
{
    if (_document == nullptr) {
        return 0;
    }

    const int lineCount = static_cast<int>(_document->buffer()->lineCount());

    if (line < 0 || line >= lineCount) {
        return 0;
    }

    return _document->buffer()->lineProperties(static_cast<uint32_t>(line)).byteLength;
}

int EditorView::lineUnits(int line)
{
    prepareLine(line);

    return _layout->totalUnits();
}

uint32_t EditorView::bytesBeforeColumn(int line, int column)
{
    if (_document == nullptr) {
        return 0;
    }

    const TextBuffer *buffer = _document->buffer();
    const uint32_t offset = buffer->offsetAt(static_cast<uint32_t>(line), static_cast<uint32_t>(column));
    const uint32_t lineStart = buffer->lineProperties(static_cast<uint32_t>(line)).startOffset;

    if (offset <= lineStart) {
        // 光标在行首，退格要连同上一行的换行符一起删掉
        return (offset > 0) ? 1 : 0;
    }

    const std::string tail = buffer->getTextRange(lineStart, offset);
    const uint32_t size = static_cast<uint32_t>(tail.size());
    const uint32_t characterStart = TextLayout::snapToCharStart(tail.data(), size, size - 1);

    return size - characterStart;
}

uint32_t EditorView::bytesAfterColumn(int line, int column)
{
    if (_document == nullptr) {
        return 0;
    }

    const TextBuffer *buffer = _document->buffer();
    const LineProperties properties = buffer->lineProperties(static_cast<uint32_t>(line));
    const uint32_t offset = buffer->offsetAt(static_cast<uint32_t>(line), static_cast<uint32_t>(column));
    const uint32_t limit = properties.startOffset + properties.byteLength;

    if (offset >= limit) {
        // 光标在行尾，删除键吃掉的是换行符
        return (offset < buffer->byteCount()) ? 1 : 0;
    }

    const std::string rest = buffer->getTextRange(offset, limit);
    uint32_t length = 1;

    // 把后续的 UTF-8 续字节一并吃掉，多字节字符整体删除
    while (offset + length < limit && (static_cast<unsigned char>(rest[length]) & 0xC0) == 0x80) {
        length++;
    }

    return length;
}

void EditorView::selectWordAt(int line, int column)
{
    const QString text = lineText(line);
    const int length = static_cast<int>(text.size());
    int start = std::min(std::max(0, column), length);
    int end = start;

    if (start < length && isWordChar(text.at(start))) {
        while (start > 0 && isWordChar(text.at(start - 1))) {
            start--;
        }

        while (end < length && isWordChar(text.at(end))) {
            end++;
        }
    } else {
        while (start > 0 && !isWordChar(text.at(start - 1))) {
            start--;
        }

        while (end < length && !isWordChar(text.at(end))) {
            end++;
        }
    }

    _hasAnchor = true;
    _anchorLine = line;
    _anchorColumn = start;
    _cursorLine = line;
    _cursorColumn = end;
    _preferredColumn = end;
    restartBlink();
    ensureCursorVisible();
    update();

    emit cursorMoved();
}

bool EditorView::isWordChar(const QChar &ch)
{
    return ch.isLetterOrNumber() || ch == QLatin1Char('_');
}

void EditorView::restartBlink()
{
    _cursorVisible = true;

    if (_blinkRate > 0) {
        _blinkTimer->start(1000 / _blinkRate);
    }
}
