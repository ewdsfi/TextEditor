#ifndef EDITORVIEW_H
#define EDITORVIEW_H

#include "Document.h"

#include <QPoint>
#include <QString>
#include <QWidget>

class QKeyEvent;
class QMouseEvent;
class QPaintEvent;
class QTimer;
class QWheelEvent;
class TextLayout;

/// Self-drawn editor view for text and gutter, reaches the model only through Document
/// we paint everything ourselves - no QTextEdit hides behind this class
/// the money maker here: only the lines that are actually on screen get drawn,
/// that's what keeps a million-character line bearable
class EditorView : public QWidget
{
    Q_OBJECT

public:
    explicit EditorView(QWidget *parent = nullptr);

    ~EditorView() override;

    /// The view does not own the document
    /// just keep the pointer - whoever created the Document is the one who frees it
    void setDocument(Document *document);

    Document *document() const { return _document; }

    /// swapping the font also re-derives the line height and the gutter width,
    /// so don't be surprised when the layout shifts a little after this call
    void setEditorFont(const QFont &font);

    const QFont &editorFont() const { return _font; }

    /// 0 disables blinking
    /// pass 0 and the caret just stays solid forever - handy for screenshots
    void setCursorBlinkRate(int perSecond);

    /// Gutter width, also the left edge of the text area
    /// remember the text area doesn't start at x=0, it starts here
    int gutterWidth() const { return _gutterWidth; }

    int lineHeight() const { return _lineHeight; }

    /// how many whole rows fit in the widget right now
    /// integer division on purpose - a sliver of a row at the bottom doesn't count
    int visibleRows() const;

    int contentHeight();

    /// Larger of the laid-out lines and the estimate for a very long line
    /// we can't know the true pixel width of a 1M-char line without laying it out,
    /// so we estimate from the byte count instead - it's a scrollbar hint, not a promise
    int contentWidth();

    int cursorLine() const { return _cursorLine; }

    /// WATCH OUT: this is a UTF-16 unit index, not a byte index and not a grapheme index!
    /// a surrogate pair eats two of these, a combining mark eats one
    int cursorColumn() const { return _cursorColumn; }

    bool hasSelection() const;

    /// Cursor position when there is no selection
    /// so with no selection both start() and end() hand you the caret itself
    TextPosition selectionStart() const;

    /// Cursor position when there is no selection
    TextPosition selectionEnd() const;

    /// Selection size in bytes, 0 without a selection
    /// bytes, not characters - it's the piece tree's own unit
    int selectionLength() const;

    void selectAll();

    void clearSelection();

    /// pull the selected bytes out and decode them as UTF-8
    /// empty string when nothing is selected
    QString selectedText() const;

    /// extend keeps the anchor and grows the selection
    /// drop extend and the anchor is forgotten, so the old selection dies right there
    void moveCursorTo(int line, int column, bool extend);

    /// same as above but the caller already knows the byte offset
    /// saves us a line/column lookup when we're walking offsets ourselves
    void moveCursorToOffset(uint32_t offset, bool extend);

    /// Byte offset of the cursor
    uint32_t cursorOffset() const;

    /// replacing a selection is free - the caller doesn't have to delete it first
    void insertTextAtCursor(const QString &text);

    void insertNewLine();

    void backspace();

    void deleteForward();

    /// ctrl+backspace: eat the whitespace first, then the word
    void deleteWordBefore();

    void copy();

    void cut();

    void paste();

    /// Out-of-range values are pulled back
    /// NOTE: the x offset is in PIXELS, laying out a long line just to keep the caret
    /// glued to a column would cost way too much
    void setScrollOffsets(int y, int x);

    void scrollBy(int dy);

    /// centerVertically tries to center the line
    void ensureCursorVisible(bool centerVertically = false);

    void resetViewState();

    /// called after the document text changed behind our back
    void refreshLayout();

signals:
    void cursorMoved();

    void documentEdited();

protected:
    void paintEvent(QPaintEvent *event) override;

    void keyPressEvent(QKeyEvent *event) override;

    void mousePressEvent(QMouseEvent *event) override;

    void mouseMoveEvent(QMouseEvent *event) override;

    void mouseReleaseEvent(QMouseEvent *event) override;

    void mouseDoubleClickEvent(QMouseEvent *event) override;

    void wheelEvent(QWheelEvent *event) override;

    void focusInEvent(QFocusEvent *event) override;

    void focusOutEvent(QFocusEvent *event) override;

private slots:
    void blinkCursor();

    void autoScrollOnDrag();

private:
    /// Maps a widget position to a document position
    /// this is the inverse of the paint transform, so it has to undo the gutter and the scroll
    TextPosition positionAtPoint(const QPoint &point) const;

    /// top pixel of a line, before we subtract the scroll offset
    int yForLine(int line) const;

    /// make sure _layout holds the requested line before you ask it anything
    /// every xForColumn / totalUnits call is only valid for the line loaded here
    void prepareLine(int line);

    /// repaint and let the world know the cursor moved
    void updateTextArea();

    void updateCursorRect();

    /// move the caret without touching the anchor bookkeeping, callers do that part
    void setCursorPosition(int line, int column, bool extend);

    /// Vertical moves keep the preferred column
    /// walk down past a short line and back up, and the caret returns to where it was
    void moveCursorBy(int lineDelta, int columnDelta, bool extend);

    /// whole line decoded as UTF-8, without the ending
    QString lineText(int line);

    uint32_t lineByteLength(int line);

    /// UTF-16 unit count, the maximum column
    int lineUnits(int line);

    /// double click lands here: pick the word under the caret, or the run of punctuation
    void selectWordAt(int line, int column);

    /// Bytes of the character left of the column
    /// so this is not always 1 - a CJK char is 3 bytes, an emoji can be 4
    uint32_t bytesBeforeColumn(int line, int column);

    /// Bytes of the character right of the column
    /// it walks over UTF-8 continuation bytes so we never split a character in half
    uint32_t bytesAfterColumn(int line, int column);

    static bool isWordChar(const QChar &ch);

    /// show the caret now and restart the blink phase from a visible state
    void restartBlink();

    Document *_document = nullptr;  ///< not owned
    TextLayout *_layout = nullptr;  ///< layout of a single line, reused for every drawn line
    QFont _font;
    QFont _gutterFont;  ///< one point smaller than the text font
    int _lineHeight = 16;  ///< cached from the font metrics
    int _gutterWidth = 60;  ///< recomputed when the digit count grows

    int _scrollY = 0;
    int _scrollX = 0;  ///< pixels, not columns!

    int _cursorLine = 0;
    int _cursorColumn = 0;
    int _preferredColumn = 0;  ///< preferred column for vertical moves
    bool _hasAnchor = false;
    int _anchorLine = 0;
    int _anchorColumn = 0;  ///< the other end of the selection, wherever the user started

    int _clickCount = 0;  ///< 1 single, 2 double, 3 triple - wraps back to single
    QPoint _dragPoint;  ///< last mouse position, the drag timer keeps re-testing it
    bool _dragging = false;

    int _blinkRate = 1;
    bool _cursorVisible = true;
    QTimer *_blinkTimer = nullptr;
    QTimer *_dragTimer = nullptr;  ///< auto scroll while dragging
};

#endif // EDITORVIEW_H
