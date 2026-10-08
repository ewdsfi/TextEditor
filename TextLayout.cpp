#include "TextLayout.h"

#include <algorithm>

/// 锚点采样间隔：每隔这么多 UTF-16 单元记录一次累计宽度与字节数。
static const int AnchorStep = 32;

/// 代理对替换成的占位字符，全角宽度让光标位置不至于突然跳动。
static const QChar SurrogatePlaceholder(0x3000);

TextLayout::TextLayout()
    : _metrics(QFont())
{
}

void TextLayout::setFontMetrics(const QFontMetricsF &metrics)
{
    _metrics = metrics;
    _glyphWidths.clear();
    _valid = false;
    _maxWidth = 0.0;
}

void TextLayout::setLine(uint32_t line, uint32_t byteLength, const QString &text)
{
    if (_valid && _layout.line == line && _layout.byteLength == byteLength && _layout.text == text) {
        return;
    }

    _layout.line = line;
    _layout.byteLength = byteLength;
    _layout.text = text;

    measureLine();

    if (_layout.width > _maxWidth) {
        _maxWidth = _layout.width;
    }

    _valid = true;
}

// ---------------------------------------------------------------------------
// 测量
// ---------------------------------------------------------------------------

void TextLayout::measureLine()
{
    // 先把代理对换成一个普通占位字符，后面的宽度表与列号就能一一对应
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

    addAnchor(0, 0.0, 0);

    double width = 0.0;
    int bytes = 0;
    int unit = 0;

    while (unit < codeUnits && width < MaxMeasureWidth) {
        const uint32_t codePoint = text.at(unit).unicode();
        const int pieceBytes = utf8Length(codePoint);
        const double advance = glyphWidth(text, unit);

        _layout.widths[unit] = advance;
        _layout.charBytes[unit] = pieceBytes;

        width += advance;
        bytes += pieceBytes;
        unit++;

        if (unit % AnchorStep == 0) {
            addAnchor(unit, width, bytes);
        }
    }

    if (unit < codeUnits) {
        // 达到宽度上限后不再逐字测量，剩余宽度按已测部分的平均值估算
        const int remaining = codeUnits - unit;
        const double average = (unit > 0) ? (width / unit) : static_cast<double>(_metrics.averageCharWidth());
        const double guess = (average > 0.0) ? average : 1.0;

        for (int k = unit; k < codeUnits; k++) {
            _layout.widths[k] = guess;
            _layout.charBytes[k] = 1;
        }

        width += remaining * guess;
        bytes += remaining;
    }

    _layout.totalUnits = static_cast<uint32_t>(codeUnits);
    _layout.width = width;
    _layout.byteLength = static_cast<uint32_t>(bytes + removedUnits);

    if (codeUnits > 0 && _layout.anchorUnits.last() != codeUnits) {
        addAnchor(codeUnits, width, bytes);
    }
}

double TextLayout::glyphWidth(const QString &text, int index) const
{
    const uint32_t codePoint = codePointAt(text, index);
    QHash<uint32_t, double>::const_iterator cached = _glyphWidths.constFind(codePoint);

    if (cached != _glyphWidths.constEnd()) {
        return cached.value();
    }

    double advance = static_cast<double>(_metrics.horizontalAdvance(text.at(index)));

    // 宽度为 0 的字符退化为 1 像素，保证光标不会与相邻字符重叠
    if (advance <= 0.0) {
        advance = 1.0;
    }

    _glyphWidths.insert(codePoint, advance);

    return advance;
}

uint32_t TextLayout::codePointAt(const QString &text, int index)
{
    const uint32_t unit = text.at(index).unicode();

    if (unit >= 0xD800 && unit <= 0xDBFF && index + 1 < text.size()) {
        const uint32_t low = text.at(index + 1).unicode();

        if (low >= 0xDC00 && low <= 0xDFFF) {
            return 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
        }
    }

    return unit;
}

void TextLayout::addAnchor(int unit, qreal width, int byte)
{
    _layout.anchorUnits.append(unit);
    _layout.anchorWidths.append(width);
    _layout.anchorBytes.append(byte);
}

int TextLayout::utf8Length(uint32_t codePoint)
{
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

// ---------------------------------------------------------------------------
// 换算
// ---------------------------------------------------------------------------

double TextLayout::xForColumn(int column) const
{
    if (!_valid) {
        return 0.0;
    }

    if (column <= 0) {
        return 0.0;
    }

    if (column >= static_cast<int>(_layout.totalUnits)) {
        return _layout.width;
    }

    const int anchor = anchorForValue(_layout.anchorUnits, column);
    const int start = _layout.anchorUnits.at(anchor);
    double width = _layout.anchorWidths.at(anchor);

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

    if (x >= _layout.width) {
        return static_cast<int>(_layout.totalUnits);
    }

    // 先按宽度把像素定位到某个锚点，再在其后逐字推进
    const QVector<qreal> &widths = _layout.anchorWidths;
    QVector<qreal>::const_iterator found = std::upper_bound(widths.begin(), widths.end(), static_cast<qreal>(x));
    int index = static_cast<int>(found - widths.begin()) - 1;

    if (index < 0) {
        index = 0;
    }

    int unit = _layout.anchorUnits.at(index);
    double width = _layout.anchorWidths.at(index);

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

    const int anchor = anchorForValue(_layout.anchorBytes, byte);
    int accumulated = _layout.anchorBytes.at(anchor);
    int unit = _layout.anchorUnits.at(anchor);

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

    if (column >= static_cast<int>(_layout.totalUnits)) {
        return static_cast<int>(_layout.byteLength);
    }

    const int anchor = anchorForValue(_layout.anchorUnits, column);
    int accumulated = _layout.anchorBytes.at(anchor);
    const int start = _layout.anchorUnits.at(anchor);

    for (int unit = start; unit < column; unit++) {
        accumulated += _layout.charBytes.at(unit);
    }

    return accumulated;
}

int TextLayout::clampColumn(int column) const
{
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
    if (data == nullptr || offset >= byteLength) {
        return (offset > byteLength) ? byteLength : offset;
    }

    while (offset > 0 && (static_cast<unsigned char>(data[offset]) & 0xC0) == 0x80) {
        offset--;
    }

    return offset;
}

// ---------------------------------------------------------------------------
// 内部工具
// ---------------------------------------------------------------------------

int TextLayout::anchorForValue(const QVector<int> &values, int value) const
{
    QVector<int>::const_iterator found = std::upper_bound(values.begin(), values.end(), value);
    const int index = static_cast<int>(found - values.begin()) - 1;

    return (index < 0) ? 0 : index;
}
