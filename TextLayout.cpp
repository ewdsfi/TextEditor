#include "TextLayout.h"

#include <algorithm>

// anchor sampling interval, in UTF-16 units
static const int AnchorStep = 32;

// surrogate pair placeholder, keeps the full width
static const QChar SurrogatePlaceholder(0x3000);

TextLayout::TextLayout()
    : _metrics(QFont())
{
}

void TextLayout::setFontMetrics(const QFontMetricsF &metrics)
{
    _metrics = metrics;

    // the font changed, so every cached width is a lie now, throw them all away
    _glyphWidths.clear();
    _valid = false;
    _maxWidth = 0.0;
}

void TextLayout::setLine(uint32_t line, uint32_t byteLength, const QString &text)
{
    // same line, same length, same contents: the previous measurement is still good,
    // and this early out is what keeps scrolling from re-measuring every frame
    if (_valid && _layout.line == line && _layout.byteLength == byteLength && _layout.text == text) {
        return;
    }

    _layout.line = line;
    _layout.byteLength = byteLength;
    _layout.text = text;

    measureLine();

    // maxWidth only ever grows, it is the horizontal scroll range of the document
    if (_layout.width > _maxWidth) {
        _maxWidth = _layout.width;
    }

    _valid = true;
}

// ===================== Measuring =====================

void TextLayout::measureLine()
{
    // fold a surrogate pair into one placeholder
    // we do this so the tables below stay indexable by UTF-16 unit,
    // and remember to KEEP THE PAIR INTACT: dropping only one half leaves a
    // dangling surrogate that no font can measure
    QString text = _layout.text;
    int removedUnits = 0;

    for (int i = 0; i + 1 < text.size(); i++) {
        if (text.at(i).isHighSurrogate() && text.at(i + 1).isLowSurrogate()) {
            text[i] = SurrogatePlaceholder;
            text.remove(i + 1, 1);
            removedUnits++;
        }
    }

    _layout.text = text;

    const int codeUnits = text.size();

    _layout.widths.resize(codeUnits);
    _layout.charBytes.resize(codeUnits);
    _layout.anchorUnits.clear();
    _layout.anchorWidths.clear();
    _layout.anchorBytes.clear();

    // the very first anchor is the line start, it gives everyone a floor to fall back on
    addAnchor(0, 0.0, 0);

    double width = 0.0;
    int bytes = 0;
    int unit = 0;

    // one pass over the line, walking a unit at a time and summing the advances,
    // we stop as soon as the line gets absurdly wide, the rest is estimated below
    while (unit < codeUnits && width < MaxMeasureWidth) {
        const uint32_t codePoint = text.at(unit).unicode();
        const int pieceBytes = utf8Length(codePoint);
        const double advance = glyphWidth(text, unit);

        _layout.widths[unit] = advance;
        _layout.charBytes[unit] = pieceBytes;

        width += advance;
        bytes += pieceBytes;
        unit++;

        // drop a sample every AnchorStep units, this is the only reason
        // a click on a 1M character line is not a 1M character scan
        if (unit % AnchorStep == 0) {
            addAnchor(unit, width, bytes);
        }
    }

    if (unit < codeUnits) {
        // past the measuring cap, estimate the rest from the average width
        // we take the mean of what we did measure, the tail is only used for
        // scrolling and the scrollbar, so an average is close enough
        const int remaining = codeUnits - unit;
        const double average = (unit > 0) ? (width / unit) : static_cast<double>(_metrics.averageCharWidth());
        const double guess = (average > 0.0) ? average : 1.0;

        // fill in the guessed values, we assume one byte per unit here
        for (int k = unit; k < codeUnits; k++) {
            _layout.widths[k] = guess;
            _layout.charBytes[k] = 1;
        }

        width += remaining * guess;
        bytes += remaining;
    }

    _layout.totalUnits = static_cast<uint32_t>(codeUnits);
    _layout.width = width;

    // add back the units we folded away, so byteLength still describes the real line
    _layout.byteLength = static_cast<uint32_t>(bytes + removedUnits);

    // close the table with the line end, otherwise the last chunk has no right edge
    if (codeUnits > 0 && _layout.anchorUnits.last() != codeUnits) {
        addAnchor(codeUnits, width, bytes);
    }
}

double TextLayout::glyphWidth(const QString &text, int index) const
{
    const uint32_t codePoint = codePointAt(text, index);
    QHash<uint32_t, double>::const_iterator cached = _glyphWidths.constFind(codePoint);

    // asked before? take the shortcut
    if (cached != _glyphWidths.constEnd()) {
        return cached.value();
    }

    double advance = static_cast<double>(_metrics.horizontalAdvance(text.at(index)));

    // zero-width characters fall back to 1 pixel
    // think of combining marks and zero width joiners, a caret sitting on one of
    // them would otherwise have no width at all to move over
    if (advance <= 0.0) {
        advance = 1.0;
    }

    _glyphWidths.insert(codePoint, advance);

    return advance;
}

uint32_t TextLayout::codePointAt(const QString &text, int index)
{
    const uint32_t unit = text.at(index).unicode();

    // high surrogate followed by a low one: join them into the real code point
    if (unit >= 0xD800 && unit <= 0xDBFF && index + 1 < text.size()) {
        const uint32_t low = text.at(index + 1).unicode();

        if (low >= 0xDC00 && low <= 0xDFFF) {
            return 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
        }
    }

    // plain BMP character, or a lone surrogate at the very end
    return unit;
}

void TextLayout::addAnchor(int unit, qreal width, int byte)
{
    // these three tables are read side by side, so NEVER append to one without the others
    _layout.anchorUnits.append(unit);
    _layout.anchorWidths.append(width);
    _layout.anchorBytes.append(byte);
}

int TextLayout::utf8Length(uint32_t codePoint)
{
    // only four cases in UTF-8, and this is how the byte column of our tables is built
    if (codePoint < 0x80) {
        return 1;
    }

    if (codePoint < 0x800) {
        return 2;
    }

    if (codePoint < 0x10000) {
        return 3;
    }

    return 4;
}

// ===================== Conversion =====================

double TextLayout::xForColumn(int column) const
{
    // nothing measured yet, or the caller wants a position before the line starts
    if (!_valid) {
        return 0.0;
    }

    if (column <= 0) {
        return 0.0;
    }

    // at or past the end: the answer is simply the whole line width
    if (column >= static_cast<int>(_layout.totalUnits)) {
        return _layout.width;
    }

    // find the closest sample at or before the column, we never look further back
    const int anchor = anchorForValue(_layout.anchorUnits, column);
    const int start = _layout.anchorUnits.at(anchor);
    double width = _layout.anchorWidths.at(anchor);

    // then walk the few remaining units one by one
    for (int unit = start; unit < column; unit++) {
        width += _layout.widths.at(unit);
    }

    return width;
}

int TextLayout::columnForX(double x) const
{
    if (!_valid || x <= 0.0) {
        return 0;
    }

    // clicking past the last character means the end of the line
    if (x >= _layout.width) {
        return static_cast<int>(_layout.totalUnits);
    }

    // binary search the anchor, then advance character by character
    // upper_bound gives us the first anchor strictly past x, so step back one
    const QVector<qreal> &widths = _layout.anchorWidths;
    QVector<qreal>::const_iterator found = std::upper_bound(widths.begin(), widths.end(), static_cast<qreal>(x));
    int index = static_cast<int>(found - widths.begin()) - 1;

    // x can only be before the first anchor in odd cases, don't let the index go negative
    if (index < 0) {
        index = 0;
    }

    int unit = _layout.anchorUnits.at(index);
    double width = _layout.anchorWidths.at(index);

    // this is the actual hit test: the click sits inside the unit whose right edge passes x
    while (unit < static_cast<int>(_layout.totalUnits)) {
        const double next = width + _layout.widths.at(unit);

        if (next > x) {
            return unit;
        }

        width = next;
        unit++;
    }

    return static_cast<int>(_layout.totalUnits);
}

int TextLayout::columnForByte(int byte) const
{
    if (!_valid || byte <= 0) {
        return 0;
    }

    if (byte >= static_cast<int>(_layout.byteLength)) {
        return static_cast<int>(_layout.totalUnits);
    }

    // same trick as the pixel side, jump to a sample and walk from there.
    // note the two anchor tables are indexed the same way, so the anchor we get
    // from the byte table also gives us a valid starting unit
    const int anchor = anchorForValue(_layout.anchorBytes, byte);
    int accumulated = _layout.anchorBytes.at(anchor);
    int unit = _layout.anchorUnits.at(anchor);

    // keep eating full characters until the next one would step over the byte we want
    while (unit < static_cast<int>(_layout.totalUnits)) {
        if (accumulated + _layout.charBytes.at(unit) > byte) {
            return unit;
        }

        accumulated += _layout.charBytes.at(unit);
        unit++;
    }

    return static_cast<int>(_layout.totalUnits);
}

int TextLayout::byteForColumn(int column) const
{
    if (!_valid || column <= 0) {
        return 0;
    }

    // an offset sitting exactly at the end of the line maps to the full byte length
    if (column >= static_cast<int>(_layout.totalUnits)) {
        return static_cast<int>(_layout.byteLength);
    }

    const int anchor = anchorForValue(_layout.anchorUnits, column);
    int accumulated = _layout.anchorBytes.at(anchor);
    const int start = _layout.anchorUnits.at(anchor);

    // sum the bytes of the units we are skipping over
    for (int unit = start; unit < column; unit++) {
        accumulated += _layout.charBytes.at(unit);
    }

    return accumulated;
}

int TextLayout::clampColumn(int column) const
{
    // nothing measured, trust the caller, we have no line to clamp against
    if (!_valid) {
        return column;
    }

    if (column <= 0) {
        return 0;
    }

    if (column >= static_cast<int>(_layout.totalUnits)) {
        return static_cast<int>(_layout.totalUnits);
    }

    return column;
}

uint32_t TextLayout::snapToCharStart(const char *data, uint32_t byteLength, uint32_t offset)
{
    // no buffer, or the offset is already out of range, just report the clamped offset
    if (data == nullptr || offset >= byteLength) {
        return (offset > byteLength) ? byteLength : offset;
    }

    // a UTF-8 continuation byte has the top two bits set to 10, we walk left until
    // we hit a leading byte, so we never land in the middle of a character
    while (offset > 0 && (static_cast<unsigned char>(data[offset]) & 0xC0) == 0x80) {
        offset--;
    }

    return offset;
}

// ===================== Internal helpers =====================

int TextLayout::anchorForValue(const QVector<int> &values, int value) const
{
    // upper_bound is fine because the anchors are appended in increasing order
    QVector<int>::const_iterator found = std::upper_bound(values.begin(), values.end(), value);
    const int index = static_cast<int>(found - values.begin()) - 1;

    // value smaller than every anchor: the first one is our floor
    return (index < 0) ? 0 : index;
}
