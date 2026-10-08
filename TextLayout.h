#ifndef TEXTLAYOUT_H
#define TEXTLAYOUT_H

#include <QFontMetricsF>
#include <QHash>
#include <QString>
#include <QVector>

/// 单行的排版结果：把一行明文转成可测量的 UTF-16 序列，
/// 并按固定间隔记录锚点，使得列号、字节下标与像素之间的换算不必扫描整行。
struct LineLayout
{
    uint32_t line = 0;              ///< 行号
    uint32_t byteLength = 0;        ///< 该行的字节数
    uint32_t totalUnits = 0;        ///< 该行的 UTF-16 单元数，即最大列号
    double width = 0.0;             ///< 该行的像素宽度
    QString text;                   ///< 该行明文，代理对已替换成占位字符
    QVector<qreal> widths;          ///< 每个 UTF-16 单元占的像素宽度
    QVector<int> charBytes;         ///< 每个 UTF-16 单元的 UTF-8 字节数
    QVector<int> anchorUnits;       ///< 锚点处的 UTF-16 单元下标
    QVector<qreal> anchorWidths;    ///< 锚点处的累计像素宽度
    QVector<int> anchorBytes;       ///< 锚点处的行内字节下标
};

/// 文本排版器：按需为某一行建立排版信息，并在列号、字节下标与像素之间换算。
/// 超长单行只测量一次并按固定间隔采样，因此光标定位与命中测试的开销与行长无关。
class TextLayout
{
public:
    /// 构造排版器，字体度量由调用方提供。
    TextLayout();

    /// 设置等宽字体度量，变化后丢弃已有缓存。
    void setFontMetrics(const QFontMetricsF &metrics);

    /// 登记当前行的内容并测量，内容与字节数都没变时直接复用缓存。
    void setLine(uint32_t line, uint32_t byteLength, const QString &text);

    /// 当前行的 UTF-16 单元数，即最大列号。
    int totalUnits() const { return static_cast<int>(_layout.totalUnits); }

    /// 当前行的字节数。
    int byteLength() const { return static_cast<int>(_layout.byteLength); }

    /// 当前行的像素宽度。
    double lineWidth() const { return _layout.width; }

    /// 当前行的明文。
    const QString &text() const { return _layout.text; }

    /// 是否已经有行完成排版。
    bool hasLine() const { return _valid; }

    /// 已排版行的最大像素宽度。
    double maxWidth() const { return _maxWidth; }

    /// 是否有行超过宽度上限，这类行的排版宽度按估算补齐。
    bool hasTruncatedLine() const { return _maxWidth > MaxMeasureWidth; }

    /// 把列号换算成行内像素横坐标。
    double xForColumn(int column) const;

    /// 把行内像素横坐标换算成列号。
    int columnForX(double x) const;

    /// 把行内字节下标换算成列号。
    int columnForByte(int byte) const;

    /// 把列号换算成行内字节下标。
    int byteForColumn(int column) const;

    /// 把列号收敛到合法范围，并避免落在代理对中间。
    int clampColumn(int column) const;

    /// 把字节下标回退到 UTF-8 字符起始位置。
    static uint32_t snapToCharStart(const char *data, uint32_t byteLength, uint32_t offset);

    /// 单行最多测量多少像素，避免超长行的排版开销失控。
    static const int MaxMeasureWidth = 1024 * 1024;

private:
    /// 测量当前行，填充宽度表、字节长度表与锚点表。
    void measureLine();

    /// 找累计值不超过 value 的最后一个锚点，返回锚点序号。
    int anchorForValue(const QVector<int> &values, int value) const;

    /// 记录一个锚点。
    void addAnchor(int unit, qreal width, int byte);

    /// 计算一个 Unicode 码点编码成 UTF-8 后的字节数。
    static int utf8Length(uint32_t codePoint);

    /// 取得某个字符的显示宽度，代理对按整个码点缓存。
    double glyphWidth(const QString &text, int index) const;

    /// 把字符换算成缓存用的码点。
    static uint32_t codePointAt(const QString &text, int index);

    QFontMetricsF _metrics;      ///< 字体度量
    LineLayout _layout;          ///< 当前行的排版信息
    bool _valid = false;         ///< 当前行缓存是否可用
    double _maxWidth = 0.0;      ///< 已排版行中的最大像素宽度
    mutable QHash<uint32_t, double> _glyphWidths;  ///< 字符宽度缓存
};

#endif // TEXTLAYOUT_H
