#ifndef TEXTLAYOUT_H
#define TEXTLAYOUT_H

#include <QFontMetricsF>
#include <QHash>
#include <QString>
#include <QVector>

/// Measured data of one single line: UTF-16 unit widths plus sampled anchors, all UTF-16 based
struct LineLayout
{
    uint32_t line = 0;
    uint32_t byteLength = 0;
    uint32_t totalUnits = 0;        // how many UTF-16 units the line has, this is also the max column
    double width = 0.0;
    QString text;                   // surrogate pairs already folded into one placeholder, see measureLine()
    QVector<qreal> widths;          // pixel advance of every single UTF-16 unit
    QVector<int> charBytes;         // UTF-8 byte length of every single UTF-16 unit
    QVector<int> anchorUnits;       // sampled unit index, the next two tables must keep this same order
    QVector<qreal> anchorWidths;
    QVector<int> anchorBytes;
};

/// Layout of one very long line. We only measure and sample it, so hit testing
/// costs about the same whether the line is 1 thousand or 1 million characters long.
class TextLayout
{
public:
    TextLayout();

    /// give us the metrics, changing the font also throws away every cached width
    void setFontMetrics(const QFontMetricsF &metrics);
    /// measure a line: its number(so we can skip work when nothing changed),
    /// the UTF-8 length of the whole original line, and the text itself
    void setLine(uint32_t line, uint32_t byteLength, const QString &text);

    /// simple readers of the current measurement: units(columns), UTF-8 bytes,
    /// pixel width, whether a line is loaded, and the widest width seen so far
    int totalUnits() const { return static_cast<int>(_layout.totalUnits); }

    int byteLength() const { return static_cast<int>(_layout.byteLength); }

    double lineWidth() const { return _layout.width; }

    /// the text we actually measured, surrogate pairs are placeholders here, don't feed
    /// it back into the buffer as real text
    const QString &text() const { return _layout.text; }

    /// false before the first setLine(), the other readers hand back safe defaults then
    bool hasLine() const { return _valid; }
    double maxWidth() const { return _maxWidth; }

    /// true when some line went past the measuring cap, its tail is only an estimate then
    bool hasTruncatedLine() const { return _maxWidth > MaxMeasureWidth; }

    /// column to pixel, measured from the left edge of the line
    double xForColumn(int column) const;

    /// pixel to column, i.e. which column the caret lands on when you click at x
    int columnForX(double x) const;

    /// UTF-8 byte offset to column, for mapping a buffer position to a caret column
    int columnForByte(int byte) const;

    /// column back to the UTF-8 byte offset where that unit starts, the inverse direction
    int byteForColumn(int column) const;

    /// clamp a column into [0, totalUnits], handy before you index anything
    int clampColumn(int column) const;

    /// walk an offset back to the start of its UTF-8 character, offsets inside a multibyte
    /// character are moved left, we never hand you half of a character
    static uint32_t snapToCharStart(const char *data, uint32_t byteLength, uint32_t offset);

    // pixel cap for measuring a single line
    static const int MaxMeasureWidth = 1024 * 1024;

private:
    /// measure the line we got from setLine(), fill every table in _layout
    void measureLine();

    /// the two anchor helpers: binary search the last sample whose accumulated value is
    /// still <= value, and append a new sample, note the three anchor tables go together
    int anchorForValue(const QVector<int> &values, int value) const;

    void addAnchor(int unit, qreal width, int byte);

    /// UTF-8 byte length of a code point: 1 / 2 / 3 / 4
    static int utf8Length(uint32_t codePoint);

    /// pixel width of one unit, cached by code point because we can't call the font a million times
    double glyphWidth(const QString &text, int index) const;

    /// decode the code point at index, we read the low surrogate when there is one
    static uint32_t codePointAt(const QString &text, int index);

    QFontMetricsF _metrics;
    LineLayout _layout;
    bool _valid = false;
    double _maxWidth = 0.0;
    mutable QHash<uint32_t, double> _glyphWidths;
};

#endif // TEXTLAYOUT_H
