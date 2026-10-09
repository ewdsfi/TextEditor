#ifndef TEXTBUFFER_H
#define TEXTBUFFER_H

#include <cstdint>
#include <string>
#include <vector>

/// every document offset fits in 32 bits, so we use this instead of dragging size_t around
typedef uint32_t BufferIndex;

/// A piece: one byte range inside a buffer
/// we keep it as line + column on purpose, buffers only ever grow so a piece stays valid forever
struct Piece
{
    BufferIndex bufferIndex = 0;   // which buffer of _buffers this range lives in
    uint32_t startLine = 0;        // head of the range, said in the buffer's own lines
    uint32_t startColumn = 0;      // and the column inside that line, both are 0-based
    uint32_t endLine = 0;
    uint32_t endColumn = 0;    // exclusive, so an empty piece would have start == end
    uint32_t byteLength = 0;   // cached on purpose: we walk the whole piece list on every lookup
    uint32_t newlineCount = 0; // how many '\n' sit inside, i.e. how many lines this piece spans
};

/// Read-only string buffer
/// new text goes into a fresh buffer and old ones are never touched again, that is the whole trick
struct StringBuffer
{
    std::string text;
    std::vector<uint32_t> lineStarts;  // byte offset of each line start in text, element 0 is always 0
};

/// Document position, both 0-based; watch out, column counts bytes here and not characters
struct TextPosition
{
    uint32_t line = 0;
    uint32_t column = 0;
};

/// Line start info for one line, the view asks for a single line per paint and gets it all here
struct LineProperties
{
    uint32_t startOffset = 0;      // byte offset of the first byte of the line
    uint32_t byteLength = 0;       // line length without the trailing newline
    uint32_t codeUnits = 0;        // number of UTF-8 characters
    bool endsWithNewline = false;  // false on the last line, and then there is no break to draw
};

/// Piece table text buffer, plain C++
/// Edits only rewrite the piece list, which is compacted once it grows past the limit
class TextBuffer
{
public:
    /// starts with one empty buffer, so _buffers[0] is always safe to read
    TextBuffer();

    ~TextBuffer();

    /// buffers are owned by us alone, a copy would hand the same raw pointers to two objects
    TextBuffer(const TextBuffer &) = delete;

    TextBuffer &operator=(const TextBuffer &) = delete;

    /// drops all the text but keeps the object usable, cheaper than building a new one
    void clear();

    /// Builds the whole string at once, only for saving; need a few lines? use getTextRange()
    std::string getText() const;

    /// half open range [startOffset, endOffset), offsets are clamped instead of reported
    std::string getTextRange(uint32_t startOffset, uint32_t endOffset) const;

    /// content of one line without its newline, empty when that line does not exist
    std::string getLineContent(uint32_t line) const;

    /// unitCount counts UTF-8 characters, this is how a huge line reaches the view slice by slice
    std::string getLineSegment(uint32_t line, uint32_t column, uint32_t unitCount) const;

    /// everything about one line in one call, cached since the view keeps asking for the same line
    LineProperties lineProperties(uint32_t line) const;

    /// CRLF and lone CR are normalized to LF here, so hand us the raw bytes and forget about them
    uint32_t insertText(uint32_t offset, const char *text, size_t byteLength);

    /// returns how many bytes really disappeared, both ends are clamped
    uint32_t deleteRange(uint32_t startOffset, uint32_t endOffset);

    /// delete then insert, returns the removed byte count just like deleteRange()
    uint32_t replaceRange(uint32_t startOffset, uint32_t endOffset, const char *text, size_t byteLength);

    /// line/column to byte offset, both ends clamp, so a column past the line end is fine
    uint32_t offsetAt(uint32_t line, uint32_t column) const;

    /// the slow direction, it counts '\n' one byte at a time - don't call it per visible line
    TextPosition positionAt(uint32_t offset) const;

    uint32_t lineCount() const { return _lineCount; }

    uint32_t byteCount() const { return _byteCount; }

    uint32_t maxLineBytes() const { return _maxLineBytes; }

    /// Bumped on every edit so upper layers can drop caches and know their layout went stale
    uint32_t version() const { return _version; }

    /// byte offset of a line head, line 0 is always 0 and anything past the end gives byteCount()
    uint32_t lineStartOffset(uint32_t line) const;

    /// full consistency check, for debugging - it recounts what the invariants promise
    bool checkIntegrity() const;

    /// Writes the failure reason into reason, nullptr when all is well
    const char *checkIntegrity(const char *&reason) const;

    /// dumps the piece list to stdout, purely a debugging helper
    void dumpTree() const;

private:
    /// Splits piece index in two at byte offset inside, a no-op when inside is 0 or the piece length
    void splitPiece(uint32_t index, uint32_t inside);

    /// Merges neighbours contiguous inside one buffer, typing otherwise leaves a trail of tiny pieces
    void mergePieces();

    /// Rewrites all text into a fresh buffer, leaving a single piece - and it is the only place
    /// where old buffers die, which is exactly why pieces may point at them freely
    void compact();

    /// Splits by average chunk size, never inside a UTF-8 character, so a big paste becomes many pieces
    void splitChunks(const char *text, size_t byteLength,
                     std::vector<uint32_t> &offsets, std::vector<uint32_t> &lengths) const;

    /// Registers text as a new buffer and builds its piece, the text is copied so you keep yours
    Piece makePiece(const char *text, size_t byteLength);

    /// recounts lines, bytes and the longest line, so the counters can never drift away from the pieces
    void computeBufferMetadata();

    /// inside gets the offset within the piece, the result is _pieces.size() when the offset is past the end
    uint32_t pieceIndexAt(uint32_t offset, uint32_t &inside) const;

    /// byte offset of the piece head inside its buffer
    uint32_t pieceStart(const Piece &piece) const;

    /// a byte of the form 10xxxxxx is the tail of a multi-byte character, never a character start
    bool isUtf8Continuation(char c) const;

    /// we only count '\n' here, a lone '\r' would slip through and the line table would lie
    uint32_t countNewlines(const char *data, size_t byteLength) const;

    /// counts characters by counting every byte that is not a continuation byte, no decoding needed
    uint32_t countCodeUnits(const char *data, size_t byteLength) const;

    std::vector<StringBuffer *> _buffers;  // raw pointers: every buffer stays alive until compact()
    std::vector<Piece> _pieces;      // in document order, piece i starts where piece i-1 ends
    uint32_t _lineCount = 1;         // an empty document still has one line, an empty one
    uint32_t _byteCount = 0;
    uint32_t _maxLineBytes = 0;      // longest line in bytes, the view sizes the h scrollbar from it
    uint32_t _version = 0;

    mutable uint32_t _cacheLine = 0;          // line info cache
    mutable LineProperties _cacheProperties;  // one slot is enough, painting asks for one line at a time
    mutable bool _cacheValid = false;         // any edit clears this, the cached line may be gone by then
};

#endif // TEXTBUFFER_H
