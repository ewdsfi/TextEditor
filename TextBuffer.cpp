#include "TextBuffer.h"

#include <algorithm>
#include <cstdio>

/// Clamps a byte offset into [0, limit]
/// everything that comes in from the outside goes through here, so a stale offset handed
/// over by the view can never walk the piece list off its end
static uint32_t clampTo(uint32_t value, uint32_t limit)
{
    return (value > limit) ? limit : value;
}

/// Chunk cap, keeps one piece from growing too slow to slice or convert
/// one big paste is cut into pieces of this size, so a single insert can't create one giant
/// piece that every later lookup has to scan through
static const size_t AverageChunkSize = 64 * 1024;

/// Piece cap, past this the buffer is compacted into a single piece
/// every edit adds a couple of pieces, so without this cap a long editing session would leave
/// thousands of tiny ones behind and each lookup would turn into a stroll through all of them
static const size_t MaxPieceCount = 4096;

TextBuffer::TextBuffer()
{
    // one empty buffer always exists, so callers may read _buffers[0] without checking first

    _buffers.push_back(new StringBuffer());
    _buffers[0]->lineStarts.push_back(0);
}

TextBuffer::~TextBuffer()
{
    // _buffers holds raw pointers, so nobody else is going to release them for us

    for (size_t i = 0; i < _buffers.size(); i++) {
        delete _buffers[i];
    }
}

void TextBuffer::clear()
{
    // keep _buffers[0] and reuse it, other code may already be holding on to it

    for (size_t i = 1; i < _buffers.size(); i++) {
        delete _buffers[i];
    }

    _buffers.resize(1);

    // reset the first buffer in place instead of allocating a new one

    _buffers[0]->text.clear();
    _buffers[0]->lineStarts.clear();
    _buffers[0]->lineStarts.push_back(0);
    _pieces.clear();

    // the cached line may not even exist anymore, so drop it before anything else reads it

    _cacheValid = false;
    _version++;
    computeBufferMetadata();
}

// ===================== Predicates and statistics =====================

bool TextBuffer::isUtf8Continuation(char c) const
{
    // every UTF-8 continuation byte looks like 10xxxxxx, nothing else can start with that

    return (static_cast<unsigned char>(c) & 0xC0) == 0x80;
}

uint32_t TextBuffer::countNewlines(const char *data, size_t byteLength) const
{
    // we only count '\n' here, a lone '\r' would slip through and break the line table
    // insertText() normalizes those away before any text reaches a buffer, so they never exist

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
    // a character starts at every byte that is not a continuation byte, so counting those is
    // enough - there is no need to decode the actual codepoints or validate them

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
    // line start plus the column inside that line, both live in the buffer's own line table

    const StringBuffer *buffer = _buffers[piece.bufferIndex];

    return buffer->lineStarts[piece.startLine] + piece.startColumn;
}

uint32_t TextBuffer::pieceIndexAt(uint32_t offset, uint32_t &inside) const
{
    // plain linear scan from the front, the piece count stays in the hundreds thanks to
    // compact(), and the cache in lineProperties() hides the repeated calls made by painting

    uint32_t walked = 0;

    for (size_t i = 0; i < _pieces.size(); i++) {
        const uint32_t length = _pieces[i].byteLength;

        // a point sitting exactly at the piece end still belongs here, so split points stay put
        if (offset <= walked + length) {
            inside = offset - walked;

            return static_cast<uint32_t>(i);
        }

        walked += length;
    }

    // the offset is past the last piece, the result is then a valid "one past the end" index

    inside = 0;

    return static_cast<uint32_t>(_pieces.size());
}

uint32_t TextBuffer::lineStartOffset(uint32_t line) const
{
    // line 0 always begins at the very start, and an empty document has no piece at all

    if (line == 0 || _pieces.empty()) {
        return 0;
    }

    // asking past the last line is not an error, callers use that as "end of document"

    if (line >= _lineCount) {
        return _byteCount;
    }

    uint32_t walked = 0;
    uint32_t linesSeen = 0;

    for (size_t i = 0; i < _pieces.size(); i++) {
        const Piece &piece = _pieces[i];

        // the wanted line sits inside this piece as soon as the running newline count reaches it

        if (linesSeen + piece.newlineCount >= line) {
            const StringBuffer *buffer = _buffers[piece.bufferIndex];
            const uint32_t base = buffer->lineStarts[piece.startLine] + piece.startColumn;
            const uint32_t bufferLine = piece.startLine + (line - linesSeen);

            // the target line starts at this piece, so its head is the piece start
            if (bufferLine == piece.startLine && linesSeen < line) {
                return walked;
            }

            // a line head that falls on the piece start is simply the piece start, nothing to add

            if (bufferLine == piece.startLine) {
                return walked;
            }

            // otherwise the line head follows the previous newline, which lies inside this piece
            const uint32_t lineHead = buffer->lineStarts[bufferLine];

            // a head before the piece can only come from the buffer start, and then the piece
            // start already is the line start we want

            if (lineHead <= base) {
                return walked;
            }

            return walked + (lineHead - base);
        }

        // this piece ends above the target line, skip it and keep walking

        linesSeen += piece.newlineCount;
        walked += piece.byteLength;
    }

    // fell off the end of the piece list, that only happens for a line past the last one

    return _byteCount;
}
std::string TextBuffer::getText() const
{
    // the whole document in one string, memory heavy but that is what saving a file needs

    return getTextRange(0, _byteCount);
}

std::string TextBuffer::getTextRange(uint32_t startOffset, uint32_t endOffset) const
{

    // the pieces between the two offsets hold all the bytes, so we just concatenate them

    std::string out;

    // empty or reversed range, and we don't even need to touch the piece list

    if (startOffset >= endOffset) {
        return out;
    }

    // callers pass offsets from their own bookkeeping, so clamp before trusting them

    endOffset = clampTo(endOffset, _byteCount);

    if (startOffset >= endOffset) {
        return out;
    }

    // start from the piece that holds startOffset, inside is then our offset within that piece

    uint32_t inside = 0;
    uint32_t index = pieceIndexAt(startOffset, inside);
    uint32_t reader = startOffset;

    while (index < _pieces.size() && reader < endOffset) {
        const Piece &piece = _pieces[index];
        const StringBuffer *buffer = _buffers[piece.bufferIndex];
        const uint32_t base = pieceStart(piece);
        const uint32_t takeFrom = base + inside;

        // take only what is left in this piece and only what is still wanted

        const uint32_t available = piece.byteLength - inside;
        const uint32_t wanted = endOffset - reader;
        const uint32_t taken = std::min(available, wanted);
        const uint32_t takeTo = takeFrom + taken;

        if (takeTo > takeFrom) {
            // the guard also skips the pointless append when we start exactly at a piece end

            out.append(buffer->text.data() + takeFrom, takeTo - takeFrom);
        }

        // the next piece starts at its own beginning, so inside no longer applies

        reader += taken;
        inside = 0;
        index++;
    }

    return out;
}

std::string TextBuffer::getLineContent(uint32_t line) const
{
    // a line past the end is not an error, you just get nothing back

    if (line >= _lineCount) {
        return std::string();
    }

    const LineProperties properties = lineProperties(line);

    // the newline byte itself is not part of the content, byteLength already excludes it

    return getTextRange(properties.startOffset, properties.startOffset + properties.byteLength);
}

std::string TextBuffer::getLineSegment(uint32_t line, uint32_t column, uint32_t unitCount) const
{
    if (line >= _lineCount) {
        return std::string();
    }

    const LineProperties properties = lineProperties(line);

    // the whole line is shorter than the requested column, so there is nothing left to hand out

    if (column >= properties.byteLength) {
        return std::string();
    }

    // unitCount is in characters while our offsets are in bytes, so the end is clamped against
    // the line end and we can never spill over into the next line

    const uint32_t start = properties.startOffset + column;
    const uint32_t end = clampTo(start + unitCount, properties.startOffset + properties.byteLength);

    return getTextRange(start, end);
}

LineProperties TextBuffer::lineProperties(uint32_t line) const
{
    // the view asks for the same line over and over while painting, so one cache slot is
    // already enough to make the whole paint loop cheap

    if (_cacheValid && _cacheLine == line) {
        return _cacheProperties;
    }

    LineProperties properties;

    // out of range, hand back the zero value instead of asserting

    if (line >= _lineCount) {
        return properties;
    }

    const uint32_t from = lineStartOffset(line);
    uint32_t to = _byteCount;

    if (line + 1 < _lineCount) {
        // one byte before the next line head is this line's newline
        to = lineStartOffset(line + 1) - 1;
    }


    properties.startOffset = from;

    // the last line has no newline, so it simply runs to the end of the document

    properties.byteLength = (to > from) ? (to - from) : 0;
    properties.endsWithNewline = (line + 1 < _lineCount);

    // we only need the characters to count code units, the temporary string is dropped right after

    const std::string content = getTextRange(from, from + properties.byteLength);

    properties.codeUnits = countCodeUnits(content.data(), content.size());

    // remember the answer, the very next call is most likely the same line again

    _cacheLine = line;
    _cacheProperties = properties;
    _cacheValid = true;

    return properties;
}

// ===================== Position conversion =====================

uint32_t TextBuffer::offsetAt(uint32_t line, uint32_t column) const
{
    // a line past the end resolves to the end of the document, not to an error

    if (line >= _lineCount) {
        return _byteCount;
    }

    // the column may point past the line end or even into the next line, the clamp covers both

    return clampTo(lineStartOffset(line) + column, _byteCount);
}

TextPosition TextBuffer::positionAt(uint32_t offset) const
{
    offset = clampTo(offset, _byteCount);

    TextPosition position;

    // walk the pieces in order, counting the newlines inside each one
    // this is O(offset), so please keep it out of the per frame painting path

    uint32_t walked = 0;

    for (size_t i = 0; i < _pieces.size(); i++) {
        const Piece &piece = _pieces[i];
        const StringBuffer *buffer = _buffers[piece.bufferIndex];
        const uint32_t base = pieceStart(piece);

        // only look at the bytes up to the offset, anything past it is none of our business

        const uint32_t limit = std::min(piece.byteLength, offset - walked);

        for (uint32_t p = 0; p < limit; p++) {
            // a newline starts the next line, anything else just moves the column along

            if (buffer->text[base + p] == '\n') {
                position.line++;
                position.column = 0;
            } else {
                position.column++;
            }
        }

        walked += piece.byteLength;

        // the offset is inside this piece, so the answer is already complete

        if (walked >= offset) {
            return position;
        }
    }

    return position;
}

// ===================== Edit operations =====================

Piece TextBuffer::makePiece(const char *text, size_t byteLength)
{
    StringBuffer *buffer = new StringBuffer();

    // the text is copied, a buffer must never share its bytes with the caller's string

    buffer->text.assign(text, byteLength);
    buffer->lineStarts.push_back(0);

    // build the line table while we are here, the new buffer starts a line at offset 0

    for (size_t i = 0; i < byteLength; i++) {
        if (text[i] == '\n') {
            // the next line begins right after the newline byte

            buffer->lineStarts.push_back(static_cast<uint32_t>(i + 1));
        }
    }

    _buffers.push_back(buffer);

    const uint32_t lastLine = static_cast<uint32_t>(buffer->lineStarts.size() - 1);

    // one piece covering the whole buffer: it starts at 0:0 and ends where the last line ends,
    // the trailing part after the final newline is the last line's column

    Piece piece;
    piece.bufferIndex = static_cast<BufferIndex>(_buffers.size() - 1);
    piece.startLine = 0;
    piece.startColumn = 0;
    piece.endLine = lastLine;
    piece.endColumn = static_cast<uint32_t>(byteLength) - buffer->lineStarts[lastLine];
    piece.byteLength = static_cast<uint32_t>(byteLength);

    // lineStarts[0] is not a newline, so the number of newlines equals the last line index

    piece.newlineCount = lastLine;

    return piece;
}

void TextBuffer::splitChunks(const char *text, size_t byteLength,
                             std::vector<uint32_t> &offsets, std::vector<uint32_t> &lengths) const
{
    // small enough to stay as one piece, no point in splitting it up

    if (byteLength <= AverageChunkSize) {
        offsets.push_back(0);
        lengths.push_back(static_cast<uint32_t>(byteLength));

        return;
    }

    size_t cursor = 0;

    while (cursor < byteLength) {
        size_t take = std::min(AverageChunkSize, byteLength - cursor);
        size_t back = 0;

        // back off when the cut lands on a continuation byte, so no character is cut in half
        // three steps back is the most UTF-8 can ever need, 4 is just a safety net

        while (back < 4 && cursor + take < byteLength && isUtf8Continuation(text[cursor + take])) {
            take--;
            back++;
        }

        // offsets and lengths are parallel arrays, they always grow together

        offsets.push_back(static_cast<uint32_t>(cursor));
        lengths.push_back(static_cast<uint32_t>(take));
        cursor += take;
    }
}

void TextBuffer::splitPiece(uint32_t index, uint32_t inside)
{
    // nothing to cut: an offset of 0 or one at the very end means the piece already fits

    if (index >= _pieces.size() || inside == 0 || inside >= _pieces[index].byteLength) {
        return;
    }

    const Piece original = _pieces[index];
    const StringBuffer *buffer = _buffers[original.bufferIndex];
    const uint32_t base = pieceStart(original);
    const uint32_t cut = base + inside;

    // binary search for the line holding the cut point
    // the buffer's line table is sorted by construction and never changes afterwards

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

    // count the newlines of the left half, the right one just takes whatever is left over

    left.newlineCount = countNewlines(buffer->text.data() + base, inside);

    Piece right = original;

    // the right half starts exactly where the left one stopped

    right.startLine = low;
    right.startColumn = left.endColumn;
    right.byteLength = original.byteLength - inside;
    right.newlineCount = original.newlineCount - left.newlineCount;

    // the original piece becomes the left half and the right one slides in behind it

    _pieces[index] = left;
    _pieces.insert(_pieces.begin() + index + 1, right);
}

void TextBuffer::mergePieces()
{
    std::vector<Piece> merged;

    // build a fresh list rather than erasing from the middle of a vector over and over

    for (size_t i = 0; i < _pieces.size(); i++) {
        const Piece &piece = _pieces[i];

        // a piece with no bytes is pure noise, edits leave a few of those behind

        if (piece.byteLength == 0) {
            continue;
        }

        if (!merged.empty() && merged.back().bufferIndex == piece.bufferIndex) {
            Piece &last = merged.back();
            const StringBuffer *buffer = _buffers[piece.bufferIndex];
            const uint32_t lastEnd = buffer->lineStarts[last.endLine] + last.endColumn;
            const uint32_t pieceBegin = buffer->lineStarts[piece.startLine] + piece.startColumn;

            // two pieces contiguous inside one buffer can merge directly
            // pieces from different buffers never merge, even when the text matches, their
            // ranges belong to two different line tables

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
    const std::string all = getText();

    // the new buffer owns a full copy of the document, after this there is exactly one buffer
    // and one piece until somebody edits the text again

    StringBuffer *buffer = new StringBuffer();

    buffer->text = all;
    buffer->lineStarts.push_back(0);

    // rebuild the line table from scratch, the old tables are about to be thrown away anyway

    for (size_t i = 0; i < all.size(); i++) {
        if (all[i] == '\n') {
            buffer->lineStarts.push_back(static_cast<uint32_t>(i + 1));
        }
    }

    // every old buffer is now unreachable, release them here - this is the ONLY place in the
    // whole class where a buffer dies, which is why pieces are allowed to point at them freely

    for (size_t i = 0; i < _buffers.size(); i++) {
        delete _buffers[i];
    }

    _buffers.clear();
    _buffers.push_back(buffer);
    _pieces.clear();

    if (!all.empty()) {
        // an empty document stays piece-less, that is a perfectly valid state

        Piece piece;
        piece.bufferIndex = 0;
        piece.startLine = 0;
        piece.startColumn = 0;
        piece.endLine = static_cast<uint32_t>(buffer->lineStarts.size() - 1);
        piece.endColumn = static_cast<uint32_t>(all.size()) - buffer->lineStarts[piece.endLine];
        piece.byteLength = static_cast<uint32_t>(all.size());

        // same trick as in makePiece(): the last line index is the newline count

        piece.newlineCount = piece.endLine;
        _pieces.push_back(piece);
    }
}

uint32_t TextBuffer::insertText(uint32_t offset, const char *text, size_t byteLength)
{
    // nothing to insert, and a null pointer would crash the copy below anyway

    if (text == nullptr || byteLength == 0) {
        return 0;
    }

    // the offset may come from a stale view, clamp it before using it

    offset = clampTo(offset, _byteCount);

    // newlines are normalized to LF so they never split across pieces and the line table stays consistent
    // do it HERE and not later: buffers are immutable once created, there is no second chance
    // to fix a stray '\r' once it has landed inside one
    std::string normalized;
    normalized.reserve(byteLength);

    for (size_t i = 0; i < byteLength; i++) {
        if (text[i] == '\r') {
            // CRLF counts as a single newline, so swallow the '\n' that follows the '\r'

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

    // a huge paste turns into many pieces here, each one capped at AverageChunkSize

    splitChunks(data, length, offsets, lengths);

    // split first when the insert point is inside a piece
    uint32_t inside = 0;
    uint32_t index = pieceIndexAt(offset, inside);

    if (index < _pieces.size() && inside > 0) {
        // cut the piece in two so the new text lands on a clean piece boundary
        // inside > 0 is the real "we are in the middle" test, since pieceIndexAt() puts a
        // point that sits exactly on the piece end into the piece itself

        splitPiece(index, inside);
        index++;
    }


    for (size_t i = 0; i < offsets.size(); i++) {
        _pieces.insert(_pieces.begin() + index + i, makePiece(data + offsets[i], lengths[i]));

    }

    for (size_t i = 0; i < _pieces.size(); i++) {
    }

    // neighbours brought together by the insert may be contiguous inside one buffer

    mergePieces();

    // past the cap the piece list starts to hurt, one compact() buys us peace for a while

    if (_pieces.size() > MaxPieceCount) {
        compact();
    }

    // the cached line is meaningless now, and the counters have to be recounted from scratch

    _cacheValid = false;
    _version++;
    computeBufferMetadata();

    return static_cast<uint32_t>(length);
}

uint32_t TextBuffer::deleteRange(uint32_t startOffset, uint32_t endOffset)
{
    // nothing to remove, or an empty document which has no piece at all

    if (startOffset >= endOffset || _pieces.empty()) {
        return 0;
    }

    // the end offset may be stale, clamp it and check again - the range may have collapsed

    endOffset = clampTo(endOffset, _byteCount);

    if (startOffset >= endOffset) {
        return 0;
    }

    const uint32_t removed = endOffset - startOffset;

    // align both ends to piece boundaries first
    uint32_t inside = 0;
    uint32_t index = pieceIndexAt(startOffset, inside);

    if (index < _pieces.size() && inside > 0) {
        splitPiece(index, inside);
        index++;
    }

    inside = 0;
    index = pieceIndexAt(endOffset, inside);

    if (index < _pieces.size() && inside > 0) {
        // no need to keep this index around, the scan below finds its pieces on its own

        splitPiece(index, inside);
    }

    // collect the pieces that fall fully inside the range
    uint32_t walked = 0;
    size_t from = _pieces.size();
    size_t to = _pieces.size();

    for (size_t i = 0; i < _pieces.size(); i++) {
        const uint32_t next = walked + _pieces[i].byteLength;

        // after the two splits the range edges sit on piece boundaries, so a piece is either
        // fully inside or fully outside - this plain comparison is all we need

        if (walked >= startOffset && next <= endOffset) {
            if (from == _pieces.size()) {
                from = i;
            }

            to = i + 1;
        }

        walked = next;
    }

    if (from < to) {
        // erase the whole run in one shot, the two split points guarantee it is contiguous

        _pieces.erase(_pieces.begin() + from, _pieces.begin() + to);
    }

    // the pieces on both sides of the hole may now be neighbours

    mergePieces();

    if (_pieces.size() > MaxPieceCount) {
        compact();
    }

    // same housekeeping as insertText(): cache off, version up, counters recounted

    _cacheValid = false;
    _version++;
    computeBufferMetadata();

    return removed;
}

uint32_t TextBuffer::replaceRange(uint32_t startOffset, uint32_t endOffset, const char *text, size_t byteLength)
{
    // the caller may want to know how much went away, so keep the value from deleteRange

    const uint32_t removed = deleteRange(startOffset, endOffset);

    // startOffset needs no adjusting: after the delete everything from there on has moved left

    insertText(startOffset, text, byteLength);

    return removed;
}

// ===================== Statistics =====================

void TextBuffer::computeBufferMetadata()
{
    // a document always has at least one line, even when it is completely empty

    uint32_t lines = 1;
    uint32_t bytes = 0;
    uint32_t longest = 0;
    uint32_t lineStart = 0;  // where the line we are currently measuring began, in document bytes

    for (size_t i = 0; i < _pieces.size(); i++) {
        const Piece &piece = _pieces[i];

        lines += piece.newlineCount;
        bytes += piece.byteLength;

        // a piece without a newline can't end a line, so there is nothing to measure here

        if (piece.newlineCount == 0) {
            continue;
        }

        // settle one line length per newline
        const StringBuffer *buffer = _buffers[piece.bufferIndex];

        for (uint32_t k = 0; k < piece.newlineCount; k++) {
            // lineEnd is the newline byte itself, the line length stops right before it

            const uint32_t lineEnd = buffer->lineStarts[piece.startLine + k + 1] - 1;
            const uint32_t length = (lineEnd > lineStart) ? (lineEnd - lineStart) : 0;

            if (length > longest) {
                longest = length;
            }

            // the next line begins right after that newline

            lineStart = lineEnd + 1;
        }
    }

    // whatever is left after the last newline is the final line, and yes it counts too

    const uint32_t tailLength = (bytes > lineStart) ? (bytes - lineStart) : 0;

    if (tailLength > longest) {
        longest = tailLength;
    }

    _lineCount = lines;
    _byteCount = bytes;
    _maxLineBytes = longest;
}

// ===================== Integrity check and debug =====================

bool TextBuffer::checkIntegrity() const
{
    const char *reason = nullptr;

    return checkIntegrity(reason) == nullptr;
}

const char *TextBuffer::checkIntegrity(const char *&reason) const
{
    // each check writes a reason and moves on, so the first failure is the one you see

    const char *localReason = nullptr;
    uint32_t bytes = 0;
    uint32_t lines = 1;

    for (size_t i = 0; i < _pieces.size() && localReason == nullptr; i++) {
        const Piece &piece = _pieces[i];

        if (piece.bufferIndex >= _buffers.size()) {
            // the piece points at a buffer that does not exist, everything below would crash

            localReason = "buffer index out of range";

            continue;
        }

        if (piece.byteLength == 0) {
            // harmless by itself, but it always means somebody forgot a merge

            localReason = "empty piece";

            continue;
        }

        const StringBuffer *buffer = _buffers[piece.bufferIndex];

        if (piece.startLine >= buffer->lineStarts.size() || piece.endLine >= buffer->lineStarts.size()) {
            // the piece claims a line its buffer does not have, the line table disagrees

            localReason = "piece line out of range";

            continue;
        }

        const uint32_t base = buffer->lineStarts[piece.startLine] + piece.startColumn;

        if (base + piece.byteLength > buffer->text.size()) {
            // the range runs past the end of the buffer, the classic off by one

            localReason = "piece exceeds buffer";

            continue;
        }

        if (countNewlines(buffer->text.data() + base, piece.byteLength) != piece.newlineCount) {
            // recount instead of trusting the cached number, that is the whole point here

            localReason = "newline count wrong";

            continue;
        }

        bytes += piece.byteLength;
        lines += piece.newlineCount;
    }

    // the cached counters have to agree with what we just walked through

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
    // one line for the totals, then one line per piece - no pretty printing on purpose

    std::printf("bytes=%u lines=%u maxLine=%u pieces=%u buffers=%u\n",
                _byteCount,
                _lineCount,
                _maxLineBytes,
                static_cast<unsigned>(_pieces.size()),
                static_cast<unsigned>(_buffers.size()));

    for (size_t i = 0; i < _pieces.size(); i++) {
        const Piece &piece = _pieces[i];

        // index, buffer, start line:column, end line:column, length and newline count

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
