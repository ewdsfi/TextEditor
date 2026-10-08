#ifndef TEXTBUFFER_H
#define TEXTBUFFER_H

#include <cstdint>
#include <string>
#include <vector>

/// 缓冲区编号：每个 Piece 指向一个只读的字符串缓冲区。
typedef uint32_t BufferIndex;

/// 文本块（Piece）：指向某个缓冲区中的一段连续字节区间。
/// 位置用「行号 + 该行内的字符列」表示，通过缓冲区自身的行首表换算成字节下标，
/// 这样在只追加的缓冲区里插入文本不会让已有的 Piece 失效。
struct Piece
{
    BufferIndex bufferIndex = 0;   ///< 所属缓冲区编号
    uint32_t startLine = 0;        ///< 起始行号（缓冲区内的行号）
    uint32_t startColumn = 0;      ///< 起始行内字符列
    uint32_t endLine = 0;          ///< 结束行号
    uint32_t endColumn = 0;        ///< 结束行内字符列（不含）
    uint32_t byteLength = 0;       ///< 字节长度
    uint32_t newlineCount = 0;     ///< 该区间内的换行符个数
};

/// 只读字符串缓冲区：一块文本加上它的行首字节下标表。
struct StringBuffer
{
    std::string text;                  ///< 全部文本
    std::vector<uint32_t> lineStarts;  ///< 行首在 text 中的字节下标
};

/// 文档中的一个位置，行号与列号均从 0 开始。
struct TextPosition
{
    uint32_t line = 0;    ///< 行号，0 基
    uint32_t column = 0;  ///< 行内字节列，0 基
};

/// 一行的起点信息，供视图按行取内容与排版使用。
struct LineProperties
{
    uint32_t startOffset = 0;      ///< 该行在文档中的起始字节下标
    uint32_t byteLength = 0;       ///< 该行的字节长度
    uint32_t codeUnits = 0;        ///< 该行的 UTF-8 字符个数
    bool endsWithNewline = false;  ///< 行尾是否带换行符
};

/// 基于 Piece Table 的文本缓冲区，纯 C++ 实现，不依赖 Qt。
/// 按文档顺序维护一个文本块列表，每块指向只读缓冲区中的一段区间；
/// 插入、删除只改动块列表，因此超长单行也能保持高效的局部编辑。
/// 块数量超过上限时会整理一次，把相邻块合并回单个缓冲区，避免块列表无限增长。
class TextBuffer
{
public:
    /// 构造一个空缓冲区。
    TextBuffer();

    /// 释放全部缓冲区。
    ~TextBuffer();

    /// 禁止拷贝，缓冲区独占底层存储。
    TextBuffer(const TextBuffer &) = delete;

    /// 禁止赋值，缓冲区独占底层存储。
    TextBuffer &operator=(const TextBuffer &) = delete;

    /// 清空全部内容，回到只有一个空行的初始状态。
    void clear();

    /// 读出全部文本，单行超长时会一次性构造整个字符串，仅用于保存等场景。
    std::string getText() const;

    /// 读出 [startOffset, endOffset) 区间内的文本。
    std::string getTextRange(uint32_t startOffset, uint32_t endOffset) const;

    /// 读出某一行的完整文本（不含行尾换行符）。
    std::string getLineContent(uint32_t line) const;

    /// 读出某一行中 [column, column + unitCount) 的片段，供视图只取可见段。
    std::string getLineSegment(uint32_t line, uint32_t column, uint32_t unitCount) const;

    /// 取得某一行的起点信息。
    LineProperties lineProperties(uint32_t line) const;

    /// 在 offset 处插入文本，返回插入的字节数。
    /// 传入的 CRLF 与单独的 CR 都会被归一成 LF，保证换行符不会被块边界劈开。
    uint32_t insertText(uint32_t offset, const char *text, size_t byteLength);

    /// 删除 [startOffset, endOffset) 区间，返回删除的字节数。
    uint32_t deleteRange(uint32_t startOffset, uint32_t endOffset);

    /// 用 text 替换 [startOffset, endOffset) 区间，返回替换前被删除的字节数。
    uint32_t replaceRange(uint32_t startOffset, uint32_t endOffset, const char *text, size_t byteLength);

    /// 行号、字节列换算成文档字节下标。
    uint32_t offsetAt(uint32_t line, uint32_t column) const;

    /// 文档字节下标换算成行号与字节列。
    TextPosition positionAt(uint32_t offset) const;

    /// 行数，空白文档也有 1 行。
    uint32_t lineCount() const { return _lineCount; }

    /// 全文字节数。
    uint32_t byteCount() const { return _byteCount; }

    /// 最长一行的字节数，供滚动条估算内容宽度。
    uint32_t maxLineBytes() const { return _maxLineBytes; }

    /// 修改版本号，每次修改自增，供上层缓存失效使用。
    uint32_t version() const { return _version; }

    /// 取第 line 行的行首在文档中的字节下标。
    uint32_t lineStartOffset(uint32_t line) const;

    /// 自检：校验块区间、行数与字节数是否自洽，全部正确返回 true。
    bool checkIntegrity() const;

    /// 自检并把失败原因写入 reason，返回 nullptr 表示一切正常。
    const char *checkIntegrity(const char *&reason) const;

    /// 打印块列表，用于调试定位。
    void dumpTree() const;

private:
    /// 把第 index 块在块内偏移 inside 处一分为二。
    void splitPiece(uint32_t index, uint32_t inside);

    /// 合并相邻且首尾相接的块。
    void mergePieces();

    /// 整体整理：把全部内容写进一个新的缓冲区，块列表收缩为一块。
    void compact();

    /// 按平均块大小切分文本，避免单块过大导致切片与换算变慢。
    void splitChunks(const char *text, size_t byteLength,
                     std::vector<uint32_t> &offsets, std::vector<uint32_t> &lengths) const;

    /// 把一段文本登记为新缓冲区并生成对应的 Piece。
    Piece makePiece(const char *text, size_t byteLength);

    /// 重新统计总字节数、总行数与最长行。
    void computeBufferMetadata();

    /// 查找覆盖指定字节下标的块，inside 返回块内偏移。
    uint32_t pieceIndexAt(uint32_t offset, uint32_t &inside) const;

    /// 取得块内容在所属缓冲区中的起始字节下标。
    uint32_t pieceStart(const Piece &piece) const;

    /// 判断片段是否为 UTF-8 续字节。
    bool isUtf8Continuation(char c) const;

    /// 统计一段文本中的换行符个数。
    uint32_t countNewlines(const char *data, size_t byteLength) const;

    /// 统计一段文本中的 UTF-8 字符个数。
    uint32_t countCodeUnits(const char *data, size_t byteLength) const;

    std::vector<StringBuffer *> _buffers;  ///< 全部只读缓冲区
    std::vector<Piece> _pieces;            ///< 按文档顺序排列的文本块
    uint32_t _lineCount = 1;               ///< 总行数
    uint32_t _byteCount = 0;               ///< 总字节数
    uint32_t _maxLineBytes = 0;            ///< 最长一行的字节数
    uint32_t _version = 0;                 ///< 修改版本号

    mutable uint32_t _cacheLine = 0;          ///< 行信息缓存：行号
    mutable LineProperties _cacheProperties;  ///< 行信息缓存：内容
    mutable bool _cacheValid = false;         ///< 行信息缓存是否有效
};

#endif // TEXTBUFFER_H
