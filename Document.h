#ifndef DOCUMENT_H
#define DOCUMENT_H

#include "TextBuffer.h"

#include <QString>

#include <string>

// ===================== Document =====================

/// One file plus the text buffer behind it. we always read as UTF-8, there is
/// no encoding picker in this editor; on Windows we hand CRLF back out again
class Document
{
public:
    Document();                          // builds an empty buffer, no file attached yet

    ~Document();                         // we own the buffer, deleting the document drops the text

    Document(const Document &) = delete; // two objects sharing one TextBuffer would be a mess
    Document &operator=(const Document &) = delete;

    /// Reads the whole file and hands the bytes to the buffer, true on success.
    /// false means the file could not be opened at all. a leading BOM is dropped
    /// here, otherwise an invisible character sits at the top of the first line
    bool loadFromFile(const QString &path);

    /// Writes the buffer back to the current path, remember to check the result.
    /// false means the open failed, or not all the bytes made it to disk.
    /// WATCH OUT: there is no Save As any more, an unnamed document can't be written
    bool save();

    void createNew();                    // drops text and path, brand new unnamed document

    void setText(const std::string &text);  // replaces everything, handy to seed a buffer

    TextBuffer *buffer() const { return _buffer; }   // the view draws through this, we own it

    /// one line as a QString, the trailing newline is not part of it. only that
    /// single line is copied out, so a huge line is never duplicated whole
    QString lineText(uint32_t line) const;

    uint32_t maxLineBytes() const { return _buffer->maxLineBytes(); }   // longest line in bytes

    bool isEmpty() const;                // true when there is not a single byte in the buffer

    /// the buffer stamps every edit, so a version mismatch means unsaved work,
    /// and that mismatch is what puts the star into the window title
    bool isModified() const { return _buffer->version() != _savedVersion; }

    const QString &filePath() const { return _filePath; }   // empty, the status bar shows the untitled placeholder

    QString title() const;               // window title text, a trailing star marks unsaved work

    /// Insert at a byte offset, returns how many bytes really went into the buffer.
    /// remember CRLF is normalized to LF on the way in, so the count can be smaller
    /// than the text you handed over - don't assume the two match
    uint32_t insertText(uint32_t offset, const std::string &text);

    uint32_t removeRange(uint32_t startOffset, uint32_t endOffset);   // half open [start, end), empty range does nothing

private:
    TextBuffer *_buffer = nullptr;   // the real text lives here, we own it
    QString _filePath;               // stays empty until a load or a save names the file
    uint32_t _savedVersion = 0;  // buffer version at the last save
};

#endif // DOCUMENT_H
