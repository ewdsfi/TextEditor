#include "EditorView.h"

#include "TextLayout.h"

#include <QApplication>
#include <QClipboard>
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
// minimum digits in the gutter
// we start at 5 so the gutter doesn't jump around while you type the first lines
const int MinimumDigits = 5;

// blank space kept on both sides of the line numbers
const int GutterPadding = 10;

// gap between the gutter and the first text pixel
const int TextPadding = 6;

// scroll cap for content width, long lines map into it proportionally
// without a cap a 1M-char line gives us a scrollbar nobody can drag,
// so we squeeze everything into this many pixels instead
const int MaxContentWidth = 1024 * 1024;

// dark theme, straight from the VS Code palette - the values are RGBA bytes
const QColor TextBackground(30, 30, 30);

const QColor GutterBackground(37, 37, 38);

const QColor GutterForeground(133, 133, 133);

const QColor GutterActiveForeground(197, 197, 197);

const QColor GutterSeparator(60, 60, 60);

const QColor TextForeground(212, 212, 212);

const QColor SelectionBackground(38, 79, 120);

const QColor CurrentLineBackground(40, 40, 40);

const QColor CaretColor(174, 175, 173);
} // namespace

EditorView::EditorView(QWidget *parent)
    : QWidget(parent)
{
    // Consolas: monospaced and complete, no dependency on the system default font
    // if it's missing we still ask for a fixed pitch font, so the layout maths holds
    _font = QFont(QStringLiteral("Consolas"), 12);
    _font.setStyleHint(QFont::Monospace);
    _font.setFixedPitch(true);
    _gutterFont = _font;
    _gutterFont.setPointSize(std::max(8, _font.pointSize() - 1));

    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::IBeamCursor);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMinimumSize(160, 80);

    // the row height comes from the font, everything vertical is a multiple of it
    const QFontMetricsF metrics(_font);
    _lineHeight = static_cast<int>(metrics.height() + 0.5);

    // a broken font shouldn't give us a zero-height row, that would be division by zero later
    if (_lineHeight <= 0) {
        _lineHeight = 16;
    }

    // gutter = enough room for MinimumDigits digits plus the padding on both sides
    _gutterWidth = MinimumDigits * static_cast<int>(metrics.horizontalAdvance(QLatin1Char('0'))) + GutterPadding * 2;

    // one layout object, reloaded line by line - we never keep the whole file laid out
    _layout = new TextLayout();
    _layout->setFontMetrics(metrics);

    _blinkTimer = new QTimer(this);

    connect(_blinkTimer, &QTimer::timeout, this, &EditorView::blinkCursor);
    _blinkTimer->start(500);

    // the drag timer only runs while the mouse is held outside the widget
    _dragTimer = new QTimer(this);

    connect(_dragTimer, &QTimer::timeout, this, &EditorView::autoScrollOnDrag);
    _dragTimer->setInterval(30);
}

EditorView::~EditorView()
{
    // the parent widget already kills the timers, the layout is ours to release
    delete _layout;
}

// ===================== Basic setup =====================

void EditorView::setDocument(Document *document)
{
    // the view never owns the document, swapping it here just re-points us
    _document = document;
    resetViewState();
}

void EditorView::setEditorFont(const QFont &font)
{
    _font = font;
    // force fixed pitch no matter what the caller handed us - our width maths assumes it
    _font.setFixedPitch(true);
    _gutterFont = _font;
    _gutterFont.setPointSize(std::max(8, _font.pointSize() - 1));

    const QFontMetricsF metrics(_font);
    _lineHeight = static_cast<int>(metrics.height() + 0.5);

    if (_lineHeight <= 0) {
        _lineHeight = 16;
    }

    _gutterWidth = MinimumDigits * static_cast<int>(metrics.horizontalAdvance(QLatin1Char('0'))) + GutterPadding * 2;
    // the layout caches metrics, so it has to hear about the change too
    _layout->setFontMetrics(metrics);
    update();
}

void EditorView::setCursorBlinkRate(int perSecond)
{
    _blinkRate = perSecond;

    if (perSecond <= 0) {
        // 0 means "don't blink", the caret just stays on
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
    // a fresh document starts at the very top left with the caret at 0,0
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

// ===================== Scrolling and size =====================

void EditorView::setScrollOffsets(int y, int x)
{
    // clamp both offsets, callers are allowed to ask for silly values
    const int maximumY = std::max(0, contentHeight() - height());
    const int maximumX = std::max(0, contentWidth() - std::max(1, width() - _gutterWidth - TextPadding));

    const int nextY = std::min(std::max(0, y), maximumY);
    const int nextX = std::min(std::max(0, x), maximumX);

    // nothing moved, don't repaint for free
    if (nextY == _scrollY && nextX == _scrollX) {
        return;
    }

    _scrollY = nextY;
    _scrollX = nextX;
    update();
}

void EditorView::scrollBy(int dy)
{
    // vertical-only shorthand, x stays exactly where it was
    setScrollOffsets(_scrollY + dy, _scrollX);
}

void EditorView::ensureCursorVisible(bool centerVertically)
{
    if (_document == nullptr) {
        return;
    }

    const int caretY = yForLine(_cursorLine);
    const int maximumY = std::max(0, contentHeight() - height());

    // ---- vertical ---- //
    if (centerVertically) {
        // put the caret line in the middle, but never scroll past the ends
        _scrollY = std::min(std::max(0, caretY - height() / 2), maximumY);
    } else if (caretY < _scrollY) {
        _scrollY = caretY;
    } else if (caretY + _lineHeight > _scrollY + height()) {
        _scrollY = caretY + _lineHeight - height();
    }

    // we need the caret line laid out before we can ask for its pixel x
    prepareLine(_cursorLine);

    const double caretX = _layout->xForColumn(_cursorColumn);
    const int viewWidth = std::max(1, width() - _gutterWidth - TextPadding);

    // ---- horizontal ---- //
    // _scrollX is a pixel offset and caretX is one too, so this compares directly
    if (caretX < static_cast<double>(_scrollX)) {
        _scrollX = static_cast<int>(caretX);
    } else if (caretX + 4.0 > static_cast<double>(_scrollX + viewWidth)) {
        // the +4 keeps a sliver of room to the right of the caret
        _scrollX = static_cast<int>(caretX + 4.0) - viewWidth;
    }

    // centering above may have overshot, so clamp one last time
    const int maximumX = std::max(0, contentWidth() - viewWidth);
    _scrollX = std::min(std::max(0, _scrollX), maximumX);
    _scrollY = std::min(std::max(0, _scrollY), maximumY);
}

int EditorView::contentHeight()
{
    if (_document == nullptr) {
        return _lineHeight;
    }

    // cheap: line count times row height, no line ever has to be laid out for this
    const int64_t lines = static_cast<int64_t>(_document->buffer()->lineCount());
    const int64_t total = lines * _lineHeight;

    // the result has to fit in an int, a huge piece tree could overflow
    return static_cast<int>(std::min<int64_t>(total, 0x7FFFFFF0));
}

int EditorView::contentWidth()
{
    if (_document == nullptr) {
        return 0;
    }

    // estimate the width of a very long line from its byte count
    // 8 pixels per byte is deliberately generous, and it beats laying out a million chars
    const uint32_t bytes = _document->maxLineBytes();
    const double approximate = static_cast<double>(bytes) * 8.0;

    // take whatever is wider: the true width of the lines we did lay out, or the estimate
    return static_cast<int>(std::min<double>(std::max(approximate, _layout->maxWidth()), MaxContentWidth));
}

int EditorView::visibleRows() const
{
    // never hand out 0, page up/down would turn into a no-op
    return std::max(1, height() / _lineHeight);
}

// ===================== Cursor and selection =====================

bool EditorView::hasSelection() const
{
    // an anchor that sits exactly on the caret isn't a selection, it's just a click
    return _hasAnchor && (_anchorLine != _cursorLine || _anchorColumn != _cursorColumn);
}

TextPosition EditorView::selectionStart() const
{
    TextPosition position;

    // with no selection the "start" is simply the caret, that keeps callers dumb
    if (!hasSelection()) {
        position.line = static_cast<uint32_t>(_cursorLine);
        position.column = static_cast<uint32_t>(_cursorColumn);

        return position;
    }

    // the anchor may be after the caret if you dragged backwards, so sort them here
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

    // same comparison as selectionStart(), only the two ends are swapped
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
    // the buffer answers in byte offsets, that's the only common currency here
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

    // anchor at the very first character, caret at the end of the last line
    _anchorLine = 0;
    _anchorColumn = 0;
    _hasAnchor = true;
    _cursorLine = static_cast<int>(_document->buffer()->lineCount()) - 1;
    // lineUnits lays the line out and hands back the UTF-16 unit count = the last column
    _cursorColumn = lineUnits(_cursorLine);
    _preferredColumn = _cursorColumn;
    restartBlink();
    ensureCursorVisible();
    update();

    emit cursorMoved();
}

void EditorView::clearSelection()
{
    // drop the anchor, the caret stays exactly where it is
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

    // one contiguous copy straight out of the buffer, then decode as UTF-8
    const std::string text = buffer->getTextRange(from, to);

    return QString::fromUtf8(text.data(), static_cast<int>(text.size()));
}

uint32_t EditorView::cursorOffset() const
{
    if (_document == nullptr) {
        return 0;
    }

    // the buffer does the line/column -> offset maths, we don't want a second copy of it
    return _document->buffer()->offsetAt(static_cast<uint32_t>(_cursorLine), static_cast<uint32_t>(_cursorColumn));
}

void EditorView::moveCursorTo(int line, int column, bool extend)
{
    // the actual clamping lives in setCursorPosition, we just finish the job here
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

    // remember where we were, extend has to anchor on the old spot, not the new one
    const int previousLine = _cursorLine;
    const int previousColumn = _cursorColumn;
    const TextPosition position = _document->buffer()->positionAt(offset);

    // ---- anchor bookkeeping ---- //
    if (extend && !_hasAnchor) {
        _hasAnchor = true;
        _anchorLine = previousLine;
        _anchorColumn = previousColumn;
    } else if (!extend) {
        _hasAnchor = false;
    }

    _cursorLine = static_cast<int>(position.line);
    prepareLine(_cursorLine);
    // clamp to the laid-out unit count, the offset may sit right after a trailing newline
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

    // ---- clamp the line ---- //
    const int lineCount = static_cast<int>(_document->buffer()->lineCount());
    const int targetLine = std::min(std::max(0, line), lineCount - 1);

    prepareLine(targetLine);

    // ---- clamp the column ---- //
    // totalUnits is only meaningful for the line we just loaded into the layout
    const int maximumColumn = _layout->totalUnits();
    const int targetColumn = std::min(std::max(0, column), maximumColumn);

    // ---- anchor ---- //
    if (extend) {
        if (!_hasAnchor) {
            // first shift-extend of this run: pin the anchor to the caret we're leaving
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
        // moving vertically, so ignore columnDelta entirely
        const int targetLine = std::min(std::max(0, _cursorLine + lineDelta), lineCount - 1);

        prepareLine(targetLine);

        // keep the preferred column, clamp to the end of a shorter line
        // we clamp the column but NOT the stored preference, that's the whole trick:
        // walk down through a short line and back up and you land where you started
        const int column = std::min(_preferredColumn, _layout->totalUnits());
        setCursorPosition(targetLine, column, extend);
        _preferredColumn = column;
    } else {
        // moving horizontally, so the preferred column is whatever we land on
        prepareLine(_cursorLine);

        const int column = std::min(std::max(0, _cursorColumn + columnDelta), _layout->totalUnits());
        setCursorPosition(_cursorLine, column, extend);
    }

    restartBlink();
    ensureCursorVisible();
    update();

    emit cursorMoved();
}

// ===================== Editing =====================

void EditorView::insertTextAtCursor(const QString &text)
{
    if (_document == nullptr) {
        return;
    }

    // the selection, collapsed or not, always tells us where the edit starts
    const TextPosition start = selectionStart();
    const TextPosition end = selectionEnd();
    const uint32_t startOffset = _document->buffer()->offsetAt(start.line, start.column);
    const uint32_t endOffset = _document->buffer()->offsetAt(end.line, end.column);

    // typing over a selection replaces it, so wipe it out first
    if (hasSelection() && endOffset > startOffset) {
        _document->removeRange(startOffset, endOffset);
    }

    // an empty payload means "just delete the selection", which is exactly what backspace wants
    if (text.isEmpty()) {
        moveCursorToOffset(startOffset, false);
        update();

        emit documentEdited();

        return;
    }

    // the buffer normalizes line endings, the cursor advances by the bytes actually written
    // that's why we use the return value instead of utf8.size(): CRLF may have become LF
    const QByteArray utf8 = text.toUtf8();
    const std::string payload(utf8.constData(), static_cast<size_t>(utf8.size()));
    const uint32_t inserted = _document->insertText(startOffset, payload);

    moveCursorToOffset(startOffset + inserted, false);
    update();

    emit documentEdited();
}

void EditorView::insertNewLine()
{
    // just text - the buffer takes care of turning \n into whatever it stores
    insertTextAtCursor(QStringLiteral("\n"));
}

void EditorView::backspace()
{
    if (_document == nullptr) {
        return;
    }

    // with something selected, backspace is the same as deleting the selection
    if (hasSelection()) {
        insertTextAtCursor(QString());

        return;
    }

    const uint32_t offset = cursorOffset();

    if (offset == 0) {
        return;
    }

    // how many bytes the character in front of the caret owns
    uint32_t length = bytesBeforeColumn(_cursorLine, _cursorColumn);

    // 0 means any multi-byte mess was cut short by the buffer, fall back to one byte
    if (length == 0 || length > offset) {
        length = 1;
    }

    // note: if we're at the start of a line this eats the newline above and joins the lines
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

    // same deal as backspace: selection first
    if (hasSelection()) {
        insertTextAtCursor(QString());

        return;
    }

    const uint32_t offset = cursorOffset();

    // nothing left to delete
    if (offset >= _document->buffer()->byteCount()) {
        return;
    }

    uint32_t length = bytesAfterColumn(_cursorLine, _cursorColumn);

    if (length == 0) {
        length = 1;
    }

    // at the end of a line this removes the newline, so the next line slides up
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

    // work on the decoded line, columns are UTF-16 units and match text.at() indices
    const QString text = lineText(_cursorLine);
    int column = std::min(_cursorColumn, static_cast<int>(text.size()));

    // walk back over the whitespace and punctuation first,
    // so "foo, bar" deletes ", bar" and not just "bar"
    while (column > 0 && !isWordChar(text.at(column - 1))) {
        column--;
    }

    while (column > 0 && isWordChar(text.at(column - 1))) {
        column--;
    }

    // convert the column back to a byte offset before touching the buffer
    const uint32_t target = _document->buffer()->offsetAt(static_cast<uint32_t>(_cursorLine),
                                                         static_cast<uint32_t>(column));

    _document->removeRange(target, offset);
    moveCursorToOffset(target, false);
    update();

    emit documentEdited();
}

// ===================== Clipboard =====================

void EditorView::copy()
{
    // nothing selected, nothing to copy - don't clobber the clipboard with an empty string
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

    // copy first, then the insert-of-nothing deletes the selection for us
    copy();
    insertTextAtCursor(QString());
}

void EditorView::paste()
{
    if (_document == nullptr) {
        return;
    }

    const QString text = QApplication::clipboard()->text();

    // an empty clipboard is a no-op, not a "delete the selection"
    if (text.isEmpty()) {
        return;
    }

    insertTextAtCursor(text);
}

// ===================== Painting =====================

void EditorView::paintEvent(QPaintEvent *event)
{
    QPainter painter(this);
    painter.setFont(_font);
    painter.fillRect(event->rect(), TextBackground);

    int lineCount = 1;

    if (_document != nullptr) {
        lineCount = static_cast<int>(_document->buffer()->lineCount());
    }

    // gutter width follows the digit count
    // count the digits of lineCount instead of asking QFontMetrics every frame
    int digits = MinimumDigits;

    for (int value = lineCount; value >= 10; value /= 10) {
        digits++;
    }

    const int wantedGutter = digits * static_cast<int>(QFontMetricsF(_font).horizontalAdvance(QLatin1Char('0')))
                             + GutterPadding * 2;

    // only update it when the digit count actually changed, otherwise the text would jitter
    if (wantedGutter != _gutterWidth) {
        _gutterWidth = wantedGutter;
    }

    const QRect gutterRect(0, 0, _gutterWidth, height());
    const QRect textRect(_gutterWidth, 0, std::max(0, width() - _gutterWidth), height());
    // only the visible lines are drawn
    // this is the one rule that makes a 1M-char document usable: never touch a line
    // outside [firstLine, lastLine], no matter how curious the caret is
    const int firstLine = std::max(0, _scrollY / _lineHeight);
    const int lastLine = std::min(lineCount - 1, (_scrollY + height()) / _lineHeight);
    // where document x=0 lands on screen, gutter and padding included, minus the scroll
    const int textShift = _gutterWidth + TextPadding - _scrollX;
    // selection rectangles stop at the text area, never bleed into the gutter
    const int lineEnd = std::max(_gutterWidth, textRect.right());

    painter.fillRect(gutterRect, GutterBackground);
    painter.setClipRect(textRect);

    // unpack the selection once, we're about to test it for every visible line
    const TextPosition selectionFrom = selectionStart();
    const TextPosition selectionTo = selectionEnd();
    const bool selecting = hasSelection();
    const bool focused = hasFocus();

    for (int line = firstLine; line <= lastLine; line++) {
        const int y = yForLine(line) - _scrollY;
        QString text;
        uint32_t byteLength = 0;

        if (_document != nullptr) {
            // one decode + one length lookup per visible line, that's all the I/O we do
            text = lineText(line);
            byteLength = lineByteLength(line);
        }

        // hand the line to the layout, every xForColumn below refers to this line only
        _layout->setLine(static_cast<uint32_t>(line), byteLength, text);

        const double lineWidth = _layout->lineWidth();

        // ---- current line ---- //
        // highlight it only when there's no selection, otherwise the two colors fight
        if (!selecting && line == _cursorLine) {
            painter.fillRect(QRect(textRect.left(), y, textRect.width(), _lineHeight), CurrentLineBackground);
        }

        // ---- selection ---- //
        if (selecting && line >= static_cast<int>(selectionFrom.line) && line <= static_cast<int>(selectionTo.line)) {
            // a full-line selection by default, trimmed at both ends below
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

            // a selection crossing lines gets extra width at the end of the line
            // otherwise the selection visually stops short of the newline it includes
            if (line == static_cast<int>(selectionTo.line) && static_cast<int>(selectionTo.column) >= _layout->totalUnits()) {
                right += 6.0;
            }

            const int x1 = textShift + static_cast<int>(left);
            // clamp to the right edge so a long selected line doesn't paint over the scrollbar
            const int x2 = std::min(textShift + static_cast<int>(right), lineEnd);

            if (x2 > x1) {
                painter.fillRect(QRect(x1, y, x2 - x1, _lineHeight), SelectionBackground);
            }
        }

        if (lineWidth > 0.0 || !text.isEmpty()) {
            // QPainter draws from the baseline, so add the ascent to our top-left y
            painter.setPen(TextForeground);
            painter.drawText(QPointF(textShift, y + QFontMetrics(_font).ascent()), text);
        }
    }

    // stays solid while unfocused
    // hmm, a caret that blinks in an unfocused window is just noise, so keep it on
    if (_document != nullptr && (_cursorVisible || !focused)) {
        prepareLine(_cursorLine);

        const double caretX = _layout->xForColumn(_layout->clampColumn(_cursorColumn));
        const int x = _gutterWidth + TextPadding + static_cast<int>(caretX) - _scrollX;
        const int y = yForLine(_cursorLine) - _scrollY;

        painter.fillRect(QRect(x, y, 2, _lineHeight), CaretColor);
    }

    // ---- gutter ---- //
    painter.setClipRect(gutterRect);
    painter.setFont(_gutterFont);
    painter.setPen(GutterSeparator);
    painter.drawLine(_gutterWidth - 1, 0, _gutterWidth - 1, height());

    const QFontMetrics gutterMetrics(_gutterFont);
    const int numberHeight = gutterMetrics.ascent() + gutterMetrics.descent();
    const int numberRight = _gutterWidth - GutterPadding;

    for (int line = firstLine; line <= lastLine; line++) {
        const int y = yForLine(line) - _scrollY;
        // centre the digits in the row, a smaller font in a taller row looks off otherwise
        const int boxTop = y + (_lineHeight - numberHeight) / 2;
        // humans count from 1
        const QString number = QString::number(line + 1);

        painter.setPen(line == _cursorLine ? GutterActiveForeground : GutterForeground);
        painter.drawText(QRect(0, boxTop, numberRight, numberHeight),
                         Qt::AlignRight | Qt::AlignVCenter,
                         number);
    }

    // hand the clip back, someone else may still want to paint
    painter.setClipping(false);
}

void EditorView::updateTextArea()
{
    update();

    emit cursorMoved();
}

void EditorView::updateCursorRect()
{
    // the whole widget is repainted anyway, a partial update wouldn't buy us much
    update();
}

// ===================== Events =====================

void EditorView::keyPressEvent(QKeyEvent *event)
{
    if (_document == nullptr) {
        // no document, let the base class deal with it
        QWidget::keyPressEvent(event);

        return;
    }

    const bool extend = event->modifiers().testFlag(Qt::ShiftModifier);
    const bool control = event->modifiers().testFlag(Qt::ControlModifier)
                         || event->modifiers().testFlag(Qt::MetaModifier);
    const int key = event->key();

    // ---- shortcuts ---- //
    // ctrl/meta combos are checked first so they can't be swallowed by the plain keys below
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
        case Qt::Key_Home:
            // ctrl+home jumps to the very beginning of the document
            moveCursorTo(0, 0, extend);

            return;
        case Qt::Key_End:
            // ctrl+end jumps to the very end, meaning the caret goes after the last character
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

    // ---- navigation and editing ---- //
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
        // end of the current line, in UTF-16 units
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
        // a tab is just text here, no column alignment games
        insertTextAtCursor(QStringLiteral("\t"));

        return;
    default:
        break;
    }

    // ---- plain text input ---- //
    const QString text = event->text();

    // skip control characters (arrows carry text too) and skip DEL,
    // only printable input should ever reach the buffer
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
    // single, double, triple click - and then back to single
    _clickCount++;

    if (_clickCount > 3) {
        _clickCount = 1;
    }

    const TextPosition position = positionAtPoint(event->pos());
    const int line = static_cast<int>(position.line);
    const int column = static_cast<int>(position.column);

    // ---- 1/2/3 clicks ---- //
    if (_clickCount >= 3) {
        // triple click selects the whole line, anchor at the front, caret at the back
        _hasAnchor = true;
        _anchorLine = line;
        _anchorColumn = 0;
        _cursorLine = line;
        _cursorColumn = lineUnits(line);
        _preferredColumn = _cursorColumn;
    } else if (_clickCount == 2) {
        selectWordAt(line, column);
    } else {
        // a plain click drops the selection and parks the caret under the mouse
        setCursorPosition(line, column, false);
    }

    // start a drag right away, the user may hold the button and pull
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

    // dragging out of the widget means "keep going", so we hand over to the timer
    const bool outside = event->pos().y() < 0 || event->pos().y() > height() || event->pos().x() < 0
                         || event->pos().x() > width();

    if (outside && !_dragTimer->isActive()) {
        _dragTimer->start();
    } else if (!outside && _dragTimer->isActive()) {
        _dragTimer->stop();
    }

    const TextPosition position = positionAtPoint(event->pos());

    // the first drag movement is what pins the anchor, unless a click already did
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

    // stop the auto scroll, otherwise the view keeps running away after the mouse is up
    _dragging = false;
    _dragTimer->stop();
}

void EditorView::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (_document == nullptr || event->button() != Qt::LeftButton) {
        QWidget::mouseDoubleClickEvent(event);

        return;
    }

    // Qt sends press -> double click, so the click counter is already at 2 here
    const TextPosition position = positionAtPoint(event->pos());
    selectWordAt(static_cast<int>(position.line), static_cast<int>(position.column));
    // pin it anyway, a triple click can follow on the same press
    _clickCount = 2;
    _dragging = true;
    _dragPoint = event->pos();
    restartBlink();
    update();

    emit cursorMoved();
}

void EditorView::wheelEvent(QWheelEvent *event)
{
    // one notch is 120 units of angle, that's the convention every mouse follows
    const int steps = event->angleDelta().y() / 120;
    const int sideways = event->angleDelta().x() / 120;

    if (steps != 0) {
        // one wheel notch scrolls three rows, feels about right with a 16px line
        scrollBy(-steps * 3 * _lineHeight);
    }

    if (sideways != 0) {
        // horizontal scrolling moves the pixel offset, so we convert space widths to pixels
        const int step = std::max(1, QFontMetrics(_font).horizontalAdvance(QLatin1Char(' '))) * 3;
        setScrollOffsets(_scrollY, _scrollX - sideways * step);
    }

    // eat the event, the parent shouldn't also scroll
    event->accept();
}

void EditorView::focusInEvent(QFocusEvent *event)
{
    QWidget::focusInEvent(event);
    // come back to a visible caret, blinking again from the start
    restartBlink();
    update();
}

void EditorView::focusOutEvent(QFocusEvent *event)
{
    QWidget::focusOutEvent(event);
    // leave the caret solid while we're unfocused, the blink timer is ignored until we return
    _cursorVisible = true;
    update();
}

void EditorView::blinkCursor()
{
    // an unfocused view has nothing to blink for
    if (_blinkRate <= 0 || !hasFocus()) {
        return;
    }

    _cursorVisible = !_cursorVisible;
    updateCursorRect();
}

void EditorView::autoScrollOnDrag()
{
    if (!_dragging) {
        // the drag ended somewhere else, stop ticking
        _dragTimer->stop();

        return;
    }

    // one row of tolerance at the top and bottom edge
    const int margin = _lineHeight;

    if (_dragPoint.y() < margin) {
        scrollBy(-_lineHeight);
    } else if (_dragPoint.y() > height() - margin) {
        scrollBy(_lineHeight);
    }

    // left of the gutter counts as "keep scrolling left"
    if (_dragPoint.x() < _gutterWidth + margin) {
        setScrollOffsets(_scrollY, _scrollX - 16);
    } else if (_dragPoint.x() > width() - margin) {
        setScrollOffsets(_scrollY, _scrollX + 16);
    }

    // after scrolling, recompute where the (stale) mouse position now points
    const TextPosition position = positionAtPoint(_dragPoint);
    setCursorPosition(static_cast<int>(position.line), static_cast<int>(position.column), true);
    update();

    emit cursorMoved();
}

// ===================== Coordinate conversion =====================

TextPosition EditorView::positionAtPoint(const QPoint &point) const
{
    TextPosition position;

    if (_document == nullptr) {
        return position;
    }

    const int lineCount = static_cast<int>(_document->buffer()->lineCount());
    // this is the exact inverse of what paintEvent does to place a line on screen
    const int line = std::min(std::max(0, (point.y() + _scrollY) / _lineHeight), lineCount - 1);
    // undo the gutter, the text padding and the horizontal scroll to get a document x
    const int x = point.x() - _gutterWidth - TextPadding + _scrollX;

    // we're const, but the layout cache is mutable in spirit - this is the one honest cast
    EditorView *self = const_cast<EditorView *>(this);

    self->prepareLine(line);

    position.line = static_cast<uint32_t>(line);
    // the layout clamps the answer to the line's unit count, no need to double check
    position.column = static_cast<uint32_t>(std::max(0, _layout->columnForX(static_cast<double>(x))));

    return position;
}

int EditorView::yForLine(int line) const
{
    // every row is exactly _lineHeight tall, that's the whole vertical model
    return line * _lineHeight;
}

void EditorView::prepareLine(int line)
{
    if (_document == nullptr) {
        return;
    }

    const int lineCount = static_cast<int>(_document->buffer()->lineCount());
    // same clamping as setCursorPosition, callers pass whatever they have
    const int target = std::min(std::max(0, line), lineCount - 1);

    _layout->setLine(static_cast<uint32_t>(target), lineByteLength(target), lineText(target));
}

QString EditorView::lineText(int line)
{
    if (_document == nullptr) {
        return QString();
    }

    const int lineCount = static_cast<int>(_document->buffer()->lineCount());

    // out of range: hand back an empty line instead of asserting
    if (line < 0 || line >= lineCount) {
        return QString();
    }

    // Document does the UTF-8 decode, we don't want a second decoder lying around
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

    // cached inside the piece tree, so this lookup is cheap
    return _document->buffer()->lineProperties(static_cast<uint32_t>(line)).byteLength;
}

int EditorView::lineUnits(int line)
{
    // loads the line as a side effect, so the layout must be asked right after
    prepareLine(line);

    // WATCH OUT: this is the UTF-16 unit count, NOT the byte length and NOT the
    // number of code points - a surrogate pair counts as two units
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
        // backspace at the line start also removes the newline above
        // (1 byte, because the buffer stores a single '\n' no matter what the file had)
        return (offset > 0) ? 1 : 0;
    }

    // grab the bytes we're standing on and trim back to the start of the last character
    const std::string tail = buffer->getTextRange(lineStart, offset);
    const uint32_t size = static_cast<uint32_t>(tail.size());
    const uint32_t characterStart = TextLayout::snapToCharStart(tail.data(), size, size - 1);

    // 1 for ASCII, up to 4 for an emoji - deleting half a character would corrupt the file
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
    // the last byte that belongs to this line, newline excluded
    const uint32_t limit = properties.startOffset + properties.byteLength;

    if (offset >= limit) {
        // delete at the line end also removes the newline
        return (offset < buffer->byteCount()) ? 1 : 0;
    }

    const std::string rest = buffer->getTextRange(offset, limit);
    uint32_t length = 1;

    // removes the following UTF-8 continuation bytes as well
    // mask 0xC0 == 0x80 is the classic "is this a trailing byte" test
    while (offset + length < limit && (static_cast<unsigned char>(rest[length]) & 0xC0) == 0x80) {
        length++;
    }

    return length;
}

void EditorView::selectWordAt(int line, int column)
{
    const QString text = lineText(line);
    const int length = static_cast<int>(text.size());
    // start == end == the clicked column until the two walks below push them apart
    int start = std::min(std::max(0, column), length);
    int end = start;

    if (start < length && isWordChar(text.at(start))) {
        // the caret is inside a word, so grow to the word's boundaries
        while (start > 0 && isWordChar(text.at(start - 1))) {
            start--;
        }

        while (end < length && isWordChar(text.at(end))) {
            end++;
        }
    } else {
        // the caret is on punctuation or at the line start, so select the punctuation run
        // double clicking just after a word should not select that word
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
    // underscores count as word characters, punctuation does not
    return ch.isLetterOrNumber() || ch == QLatin1Char('_');
}

void EditorView::restartBlink()
{
    // always restart from "visible" - the caret must not disappear right after typing
    _cursorVisible = true;

    if (_blinkRate > 0) {
        _blinkTimer->start(1000 / _blinkRate);
    }
}
