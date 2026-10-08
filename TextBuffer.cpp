#include "TextBuffer.h"

#include <algorithm>
#include <cstdio>

/// 把字节下标限制在 [0, limit] 之内。
static uint32_t clampTo(uint32_t value, uint32_t limit)
{
    return (value > limit) ? limit : value;
}

/// 一行通常不超过 64KB，超过就切块，避免单块过大拖慢切片与换算。
static const size_t AverageChunkSize = 64 * 1024;

/// 文本块数量上限，超过就整理一次，把相邻块合并回单个缓冲区。
static const size_t MaxPieceCount = 4096;

TextBuffer::TextBuffer()
{
    _buffers.push_back(new StringBuffer());
    _buffers[0]->lineStarts.push_back(0);
}

TextBuffer::~TextBuffer()
{
    for (size_t i = 0; i < _buffers.size(); i++) {
        delete _buffers[i];
    }
}

void TextBuffer::clear()
{
    for (size_t i = 1; i < _buffers.size(); i++) {
        delete _buffers[i];
    }

    _buffers.resize(1);
    _buffers[0]->text.clear();
    _buffers[0]->lineStarts.clear();
    _buffers[0]->lineStarts.push_back(0);
    _pieces.clear();
    _cacheValid = false;
    _version++;
    computeBufferMetadata();
}

// ---------------------------------------------------------------------------
// 判定与统计工具
// ---------------------------------------------------------------------------

bool TextBuffer::isUtf8Continuation(char c) const
{
    return (static_cast<unsigned char>(c) & 0xC0) == 0x80;
}

uint32_t TextBuffer::countNewlines(const char *data, size_t byteLength) const
{
    uint32_t count = 0;

    for (size_t i = 0; i < byteLength; i++) {
        if (data[i] == '\n') {
            count++;
        }
    }

    return count;
}

uint32_t TextBuffer::countCodeUnits(const char *data, size_t byteLength) const
{
    uint32_t count = 0;

    for (size_t i = 0; i < byteLength; i++) {
        if (!isUtf8Continuation(data[i])) {
            count++;
        }
    }

    return count;
}

uint32_t TextBuffer::pieceStart(const Piece &piece) const
{
    const StringBuffer *buffer = _buffers[piece.bufferIndex];

    return buffer->lineStarts[piece.startLine] + piece.startColumn;
}

uint32_t TextBuffer::pieceIndexAt(uint32_t offset, uint32_t &inside) const
{
    uint32_t walked = 0;

    for (size_t i = 0; i < _pieces.size(); i++) {
        const uint32_t length = _pieces[i].byteLength;

        // 落在块内，或正好停在块尾时都归属本块，切分点不会跑到下一块
        if (offset <= walked + length) {
            inside = offset - walked;

            return static_cast<uint32_t>(i);
        }

        walked += length;
    }

    inside = 0;

    return static_cast<uint32_t>(_pieces.size());
}

uint32_t TextBuffer::lineStartOffset(uint32_t line) const
{
    if (line == 0 || _pieces.empty()) {
        return 0;
    }

    if (line >= _lineCount) {
        return _byteCount;
    }

    uint32_t walked = 0;
    uint32_t linesSeen = 0;

    for (size_t i = 0; i < _pieces.size(); i++) {
        const Piece &piece = _pieces[i];

        if (linesSeen + piece.newlineCount >= line) {
            const StringBuffer *buffer = _buffers[piece.bufferIndex];
            const uint32_t base = buffer->lineStarts[piece.startLine] + piece.startColumn;
            const uint32_t bufferLine = piece.startLine + (line - linesSeen);

            // 目标行就是块的起始行时，行的开头在块之前，行首即块起点
            if (bufferLine == piece.startLine && linesSeen < line) {
                return walked;
            }

            if (bufferLine == piece.startLine) {
                return walked;
            }

            // 否则行首紧跟在本行前一个换行符之后，该换行符落在本块内
            const uint32_t lineHead = buffer->lineStarts[bufferLine];

            if (lineHead <= base) {
                return walked;
            }

            return walked + (lineHead - base);
        }

        linesSeen += piece.newlineCount;
        walked += piece.byteLength;
    }

    return _byteCount;
}
std::string TextBuffer::getText() const
{
    return getTextRange(0, _byteCount);
}

std::string TextBuffer::getTextRange(uint32_t startOffset, uint32_t endOffset) const
{

    std::string out;

    if (startOffset >= endOffset) {
        return out;
    }

    endOffset = clampTo(endOffset, _byteCount);

    if (startOffset >= endOffset) {
        return out;
    }

    uint32_t inside = 0;
    uint32_t index = pieceIndexAt(startOffset, inside);
    uint32_t reader = startOffset;

    while (index < _pieces.size() && reader < endOffset) {
        const Piece &piece = _pieces[index];
        const StringBuffer *buffer = _buffers[piece.bufferIndex];
        const uint32_t base = pieceStart(piece);
        const uint32_t takeFrom = base + inside;
        // 本块最多取到块尾，且不超过区间末尾
        const uint32_t available = piece.byteLength - inside;
        const uint32_t wanted = endOffset - reader;
        const uint32_t taken = std::min(available, wanted);
        const uint32_t takeTo = takeFrom + taken;

        if (takeTo > takeFrom) {
            out.append(buffer->text.data() + takeFrom, takeTo - takeFrom);
        }

        reader += taken;
        inside = 0;
        index++;
    }

    return out;
}

std::string TextBuffer::getLineContent(uint32_t line) const
{
    if (line >= _lineCount) {
        return std::string();
    }

    const LineProperties properties = lineProperties(line);

    return getTextRange(properties.startOffset, properties.startOffset + properties.byteLength);
}

std::string TextBuffer::getLineSegment(uint32_t line, uint32_t column, uint32_t unitCount) const
{
    if (line >= _lineCount) {
        return std::string();
    }

    const LineProperties properties = lineProperties(line);

    if (column >= properties.byteLength) {
        return std::string();
    }

    const uint32_t start = properties.startOffset + column;
    const uint32_t end = clampTo(start + unitCount, properties.startOffset + properties.byteLength);

    return getTextRange(start, end);
}

LineProperties TextBuffer::lineProperties(uint32_t line) const
{
    if (_cacheValid && _cacheLine == line) {
        return _cacheProperties;
    }

    LineProperties properties;

    if (line >= _lineCount) {
        return properties;
    }

    const uint32_t from = lineStartOffset(line);
    uint32_t to = _byteCount;

    if (line + 1 < _lineCount) {
        // 下一行行首回退一个字节就是本行的换行符
        to = lineStartOffset(line + 1) - 1;
    }


    properties.startOffset = from;
    properties.byteLength = (to > from) ? (to - from) : 0;
    properties.endsWithNewline = (line + 1 < _lineCount);

    const std::string content = getTextRange(from, from + properties.byteLength);

    properties.codeUnits = countCodeUnits(content.data(), content.size());

    _cacheLine = line;
    _cacheProperties = properties;
    _cacheValid = true;

    return properties;
}

// ---------------------------------------------------------------------------
// 位置换算
// ---------------------------------------------------------------------------

uint32_t TextBuffer::offsetAt(uint32_t line, uint32_t column) const
{
    if (line >= _lineCount) {
        return _byteCount;
    }

    return clampTo(lineStartOffset(line) + column, _byteCount);
}

TextPosition TextBuffer::positionAt(uint32_t offset) const
{
    offset = clampTo(offset, _byteCount);

    TextPosition position;

    // 顺序累计：每走过一个块，把它内部的换行一并计入行列
    uint32_t walked = 0;

    for (size_t i = 0; i < _pieces.size(); i++) {
        const Piece &piece = _pieces[i];
        const StringBuffer *buffer = _buffers[piece.bufferIndex];
        const uint32_t base = pieceStart(piece);
        const uint32_t limit = std::min(piece.byteLength, offset - walked);

        for (uint32_t p = 0; p < limit; p++) {
            if (buffer->text[base + p] == '\n') {
                position.line++;
                position.column = 0;
            } else {
                position.column++;
            }
        }

        walked += piece.byteLength;

        if (walked >= offset) {
            return position;
        }
    }

    return position;
}

// ---------------------------------------------------------------------------
// 修改接口
// ---------------------------------------------------------------------------

Piece TextBuffer::makePiece(const char *text, size_t byteLength)
{
    StringBuffer *buffer = new StringBuffer();

    buffer->text.assign(text, byteLength);
    buffer->lineStarts.push_back(0);

    for (size_t i = 0; i < byteLength; i++) {
        if (text[i] == '\n') {
            buffer->lineStarts.push_back(static_cast<uint32_t>(i + 1));
        }
    }

    _buffers.push_back(buffer);

    const uint32_t lastLine = static_cast<uint32_t>(buffer->lineStarts.size() - 1);

    Piece piece;
    piece.bufferIndex = static_cast<BufferIndex>(_buffers.size() - 1);
    piece.startLine = 0;
    piece.startColumn = 0;
    piece.endLine = lastLine;
    piece.endColumn = static_cast<uint32_t>(byteLength) - buffer->lineStarts[lastLine];
    piece.byteLength = static_cast<uint32_t>(byteLength);
    piece.newlineCount = lastLine;

    return piece;
}

void TextBuffer::splitChunks(const char *text, size_t byteLength,
                             std::vector<uint32_t> &offsets, std::vector<uint32_t> &lengths) const
{
    if (byteLength <= AverageChunkSize) {
        offsets.push_back(0);
        lengths.push_back(static_cast<uint32_t>(byteLength));

        return;
    }

    size_t cursor = 0;

    while (cursor < byteLength) {
        size_t take = std::min(AverageChunkSize, byteLength - cursor);
        size_t back = 0;

        // 落点若压在 UTF-8 续字节上就往前退，避免把一个字符切成两半
        while (back < 4 && cursor + take < byteLength && isUtf8Continuation(text[cursor + take])) {
            take--;
            back++;
        }

        offsets.push_back(static_cast<uint32_t>(cursor));
        lengths.push_back(static_cast<uint32_t>(take));
        cursor += take;
    }
}

void TextBuffer::splitPiece(uint32_t index, uint32_t inside)
{
    // 把第 index 块在块内偏移 inside 处一分为二
    if (index >= _pieces.size() || inside == 0 || inside >= _pieces[index].byteLength) {
        return;
    }

    const Piece original = _pieces[index];
    const StringBuffer *buffer = _buffers[original.bufferIndex];
    const uint32_t base = pieceStart(original);
    const uint32_t cut = base + inside;

    // 二分定位切割点所在行
    uint32_t low = original.startLine;
    uint32_t high = original.endLine;

    while (low < high) {
        const uint32_t mid = low + (high - low + 1) / 2;

        if (buffer->lineStarts[mid] <= cut) {
            low = mid;
        } else {
            high = mid - 1;
        }
    }

    Piece left = original;
    left.byteLength = inside;
    left.endLine = low;
    left.endColumn = cut - buffer->lineStarts[low];
    left.newlineCount = countNewlines(buffer->text.data() + base, inside);

    Piece right = original;
    right.startLine = low;
    right.startColumn = left.endColumn;
    right.byteLength = original.byteLength - inside;
    right.newlineCount = original.newlineCount - left.newlineCount;

    _pieces[index] = left;
    _pieces.insert(_pieces.begin() + index + 1, right);
}

void TextBuffer::mergePieces()
{
    std::vector<Piece> merged;

    for (size_t i = 0; i < _pieces.size(); i++) {
        const Piece &piece = _pieces[i];

        if (piece.byteLength == 0) {
            continue;
        }

        if (!merged.empty() && merged.back().bufferIndex == piece.bufferIndex) {
            Piece &last = merged.back();
            const StringBuffer *buffer = _buffers[piece.bufferIndex];
            const uint32_t lastEnd = buffer->lineStarts[last.endLine] + last.endColumn;
            const uint32_t pieceBegin = buffer->lineStarts[piece.startLine] + piece.startColumn;

            // 同一缓冲区里首尾相接的两块可以合成一块，减少块的数量
            if (lastEnd == pieceBegin) {
                last.endLine = piece.endLine;
                last.endColumn = piece.endColumn;
                last.byteLength += piece.byteLength;
                last.newlineCount += piece.newlineCount;

                continue;
            }
        }

        merged.push_back(piece);
    }

    _pieces.swap(merged);
}

void TextBuffer::compact()
{
    // 把全部内容顺序写进一个写入缓冲区，然后整体重置
    const std::string all = getText();
    StringBuffer *buffer = new StringBuffer();

    buffer->text = all;
    buffer->lineStarts.push_back(0);

    for (size_t i = 0; i < all.size(); i++) {
        if (all[i] == '\n') {
            buffer->lineStarts.push_back(static_cast<uint32_t>(i + 1));
        }
    }

    for (size_t i = 0; i < _buffers.size(); i++) {
        delete _buffers[i];
    }

    _buffers.clear();
    _buffers.push_back(buffer);
    _pieces.clear();

    if (!all.empty()) {
        Piece piece;
        piece.bufferIndex = 0;
        piece.startLine = 0;
        piece.startColumn = 0;
        piece.endLine = static_cast<uint32_t>(buffer->lineStarts.size() - 1);
        piece.endColumn = static_cast<uint32_t>(all.size()) - buffer->lineStarts[piece.endLine];
        piece.byteLength = static_cast<uint32_t>(all.size());
        piece.newlineCount = piece.endLine;
        _pieces.push_back(piece);
    }
}

uint32_t TextBuffer::insertText(uint32_t offset, const char *text, size_t byteLength)
{
    if (text == nullptr || byteLength == 0) {
        return 0;
    }

    offset = clampTo(offset, _byteCount);

    // 换行统一成 \n 后再入库：换行符永远不会被块的边界劈开，
    // 原始换行风格由 Document 在读写时还原
    std::string normalized;
    normalized.reserve(byteLength);

    for (size_t i = 0; i < byteLength; i++) {
        if (text[i] == '\r') {
            if (i + 1 < byteLength && text[i + 1] == '\n') {
                i++;
            }

            normalized.push_back('\n');
        } else {
            normalized.push_back(text[i]);
        }
    }

    const char *data = normalized.data();
    const size_t length = normalized.size();

    std::vector<uint32_t> offsets;
    std::vector<uint32_t> lengths;

    splitChunks(data, length, offsets, lengths);

    // 落点在块内部时先切开，插入位置只算一次
    uint32_t inside = 0;
    uint32_t index = pieceIndexAt(offset, inside);

    if (index < _pieces.size() && inside > 0) {
        splitPiece(index, inside);
        index++;
    }


    for (size_t i = 0; i < offsets.size(); i++) {
        _pieces.insert(_pieces.begin() + index + i, makePiece(data + offsets[i], lengths[i]));

    }

    for (size_t i = 0; i < _pieces.size(); i++) {
    }

    mergePieces();

    if (_pieces.size() > MaxPieceCount) {
        compact();
    }

    _cacheValid = false;
    _version++;
    computeBufferMetadata();

    return static_cast<uint32_t>(length);
}

uint32_t TextBuffer::deleteRange(uint32_t startOffset, uint32_t endOffset)
{
    if (startOffset >= endOffset || _pieces.empty()) {
        return 0;
    }

    endOffset = clampTo(endOffset, _byteCount);

    if (startOffset >= endOffset) {
        return 0;
    }

    const uint32_t removed = endOffset - startOffset;

    // 先把区间两端对齐到块边界
    uint32_t inside = 0;
    uint32_t index = pieceIndexAt(startOffset, inside);

    if (index < _pieces.size() && inside > 0) {
        splitPiece(index, inside);
        index++;
    }

    inside = 0;
    index = pieceIndexAt(endOffset, inside);

    if (index < _pieces.size() && inside > 0) {
        splitPiece(index, inside);
    }

    // 收集完全落在区间内的块
    uint32_t walked = 0;
    size_t from = _pieces.size();
    size_t to = _pieces.size();

    for (size_t i = 0; i < _pieces.size(); i++) {
        const uint32_t next = walked + _pieces[i].byteLength;

        if (walked >= startOffset && next <= endOffset) {
            if (from == _pieces.size()) {
                from = i;
            }

            to = i + 1;
        }

        walked = next;
    }

    if (from < to) {
        _pieces.erase(_pieces.begin() + from, _pieces.begin() + to);
    }

    mergePieces();

    if (_pieces.size() > MaxPieceCount) {
        compact();
    }

    _cacheValid = false;
    _version++;
    computeBufferMetadata();

    return removed;
}

uint32_t TextBuffer::replaceRange(uint32_t startOffset, uint32_t endOffset, const char *text, size_t byteLength)
{
    const uint32_t removed = deleteRange(startOffset, endOffset);

    insertText(startOffset, text, byteLength);

    return removed;
}

// ---------------------------------------------------------------------------
// 统计
// ---------------------------------------------------------------------------

void TextBuffer::computeBufferMetadata()
{
    uint32_t lines = 1;
    uint32_t bytes = 0;
    uint32_t longest = 0;
    uint32_t lineStart = 0;

    for (size_t i = 0; i < _pieces.size(); i++) {
        const Piece &piece = _pieces[i];

        lines += piece.newlineCount;
        bytes += piece.byteLength;

        if (piece.newlineCount == 0) {
            continue;
        }

        // 逐个换行符结算一行长度
        const StringBuffer *buffer = _buffers[piece.bufferIndex];

        for (uint32_t k = 0; k < piece.newlineCount; k++) {
            const uint32_t lineEnd = buffer->lineStarts[piece.startLine + k + 1] - 1;
            const uint32_t length = (lineEnd > lineStart) ? (lineEnd - lineStart) : 0;

            if (length > longest) {
                longest = length;
            }

            lineStart = lineEnd + 1;
        }
    }

    const uint32_t tailLength = (bytes > lineStart) ? (bytes - lineStart) : 0;

    if (tailLength > longest) {
        longest = tailLength;
    }

    _lineCount = lines;
    _byteCount = bytes;
    _maxLineBytes = longest;
}

// ---------------------------------------------------------------------------
// 自检与调试
// ---------------------------------------------------------------------------

bool TextBuffer::checkIntegrity() const
{
    const char *reason = nullptr;

    return checkIntegrity(reason) == nullptr;
}

const char *TextBuffer::checkIntegrity(const char *&reason) const
{
    const char *localReason = nullptr;
    uint32_t bytes = 0;
    uint32_t lines = 1;

    for (size_t i = 0; i < _pieces.size() && localReason == nullptr; i++) {
        const Piece &piece = _pieces[i];

        if (piece.bufferIndex >= _buffers.size()) {
            localReason = "buffer index out of range";

            continue;
        }

        if (piece.byteLength == 0) {
            localReason = "empty piece";

            continue;
        }

        const StringBuffer *buffer = _buffers[piece.bufferIndex];

        if (piece.startLine >= buffer->lineStarts.size() || piece.endLine >= buffer->lineStarts.size()) {
            localReason = "piece line out of range";

            continue;
        }

        const uint32_t base = buffer->lineStarts[piece.startLine] + piece.startColumn;

        if (base + piece.byteLength > buffer->text.size()) {
            localReason = "piece exceeds buffer";

            continue;
        }

        if (countNewlines(buffer->text.data() + base, piece.byteLength) != piece.newlineCount) {
            localReason = "newline count wrong";

            continue;
        }

        bytes += piece.byteLength;
        lines += piece.newlineCount;
    }

    if (localReason == nullptr && bytes != _byteCount) {
        localReason = "byte count mismatch";
    }

    if (localReason == nullptr && lines != _lineCount) {
        localReason = "line count mismatch";
    }

    reason = localReason;

    return localReason;
}

void TextBuffer::dumpTree() const
{
    std::printf("bytes=%u lines=%u maxLine=%u pieces=%u buffers=%u\n",
                _byteCount,
                _lineCount,
                _maxLineBytes,
                static_cast<unsigned>(_pieces.size()),
                static_cast<unsigned>(_buffers.size()));

    for (size_t i = 0; i < _pieces.size(); i++) {
        const Piece &piece = _pieces[i];

        std::printf("  [%u] buf=%u %u:%u..%u:%u len=%u nl=%u\n",
                    static_cast<unsigned>(i),
                    piece.bufferIndex,
                    piece.startLine,
                    piece.startColumn,
                    piece.endLine,
                    piece.endColumn,
                    piece.byteLength,
                    piece.newlineCount);
    }

}
