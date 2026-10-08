#include "Document.h"

#include <QFile>
#include <QFileInfo>

#include <string>

/// 撤销栈保留的最大步数，超出后丢弃最早的记录。
static const int MaxUndoSteps = 2000;

/// 把 UTF-16 字节序列转换成 UTF-8 字符串。
static std::string utf16ToUtf8(const QByteArray &data, bool bigEndian)
{
    std::string out;
    out.reserve(static_cast<size_t>(data.size()));

    const unsigned char *bytes = reinterpret_cast<const unsigned char *>(data.constData());
    const int pairCount = data.size() / 2;

    for (int i = 0; i < pairCount; i++) {
        uint32_t unit = bigEndian ? (static_cast<uint32_t>(bytes[i * 2]) << 8) | bytes[i * 2 + 1]
                                  : (static_cast<uint32_t>(bytes[i * 2 + 1]) << 8) | bytes[i * 2];
        uint32_t code = unit;

        // 高位代理后面紧跟低位代理时合成一个码点
        if (unit >= 0xD800 && unit <= 0xDBFF && i + 1 < pairCount) {
            const uint32_t low = bigEndian ? (static_cast<uint32_t>(bytes[(i + 1) * 2]) << 8) | bytes[(i + 1) * 2 + 1]
                                           : (static_cast<uint32_t>(bytes[(i + 1) * 2 + 1]) << 8) | bytes[(i + 1) * 2];

            if (low >= 0xDC00 && low <= 0xDFFF) {
                code = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
                i++;
            }
        }

        if (code < 0x80) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else if (code < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }

    return out;
}

Document::Document()
{
    _buffer = new TextBuffer();
}

Document::~Document()
{
    delete _buffer;
}

// ---------------------------------------------------------------------------
// 文件读写
// ---------------------------------------------------------------------------

bool Document::loadFromFile(const QString &path)
{
    QFile file(path);

    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }

    QByteArray data = file.readAll();
    file.close();

    TextEncoding encoding = EncodingUtf8;
    std::string text;

    if (data.size() >= 3 && static_cast<unsigned char>(data.at(0)) == 0xEF
        && static_cast<unsigned char>(data.at(1)) == 0xBB && static_cast<unsigned char>(data.at(2)) == 0xBF) {
        encoding = EncodingUtf8Bom;
        text.assign(data.constData() + 3, static_cast<size_t>(data.size() - 3));
    } else if (data.size() >= 2 && static_cast<unsigned char>(data.at(0)) == 0xFF
               && static_cast<unsigned char>(data.at(1)) == 0xFE) {
        encoding = EncodingUtf16Le;
        text = utf16ToUtf8(data.mid(2), false);
    } else if (data.size() >= 2 && static_cast<unsigned char>(data.at(0)) == 0xFE
               && static_cast<unsigned char>(data.at(1)) == 0xFF) {
        encoding = EncodingUtf16Be;
        text = utf16ToUtf8(data.mid(2), true);
    } else {
        text.assign(data.constData(), static_cast<size_t>(data.size()));
    }

    QByteArray normalized(text.data(), static_cast<int>(text.size()));
    normalizeText(normalized);

    _buffer->clear();
    _buffer->insertText(0, normalized.constData(), static_cast<size_t>(normalized.size()));

    // 重新按文件内容推断换行风格
    QByteArray raw = data;
    bool mixed = false;
    _lineEnding = detectLineEnding(raw, &mixed);
    _encoding = encoding;
    _filePath = path;
    _savedVersion = _buffer->version();
    _undoStack.clear();
    _redoStack.clear();
    _editCursor = 0;

    return true;
}

bool Document::save()
{
    if (_filePath.isEmpty()) {
        return false;
    }

    return saveAs(_filePath);
}

bool Document::saveAs(const QString &path)
{
    QFile file(path);

    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }

    const QByteArray bytes = outputBytes();
    const qint64 written = file.write(bytes);
    file.close();

    if (written != bytes.size()) {
        return false;
    }

    _filePath = path;
    _savedVersion = _buffer->version();

    return true;
}

void Document::createNew()
{
    _buffer->clear();
    _undoStack.clear();
    _redoStack.clear();
    _filePath.clear();
    _savedVersion = _buffer->version();
    _editCursor = 0;
}

void Document::setText(const std::string &text)
{
    QByteArray normalized(text.data(), static_cast<int>(text.size()));
    normalizeText(normalized);

    _buffer->clear();
    _buffer->insertText(0, normalized.constData(), static_cast<size_t>(normalized.size()));
    _savedVersion = _buffer->version();
}

bool Document::isEmpty() const
{
    return _buffer->byteCount() == 0;
}

QString Document::lineText(uint32_t line) const
{
    if (line >= _buffer->lineCount()) {
        return QString();
    }

    // 只取这一行，超长单行不会把整个文档都复制出来
    const std::string content = _buffer->getLineContent(line);

    return QString::fromUtf8(content.data(), static_cast<int>(content.size()));
}

QString Document::title() const
{
    QString name = _filePath.isEmpty() ? QStringLiteral("未命名") : QFileInfo(_filePath).fileName();

    if (isModified()) {
        name += QLatin1Char('*');
    }

    return name;
}

void Document::normalizeText(QByteArray &data)
{
    // 统一压成 \n，缓冲区内部因此不存在跨块的 CRLF 问题
    data.replace("\r\n", "\n");
    data.replace('\r', '\n');
}

QByteArray Document::outputBytes() const
{
    std::string text = _buffer->getText();
    QByteArray bytes(text.data(), static_cast<int>(text.size()));

    if (_lineEnding == LineEndingCrLf) {
        bytes.replace('\n', QByteArray("\r\n"));
    }

    if (_encoding == EncodingUtf8Bom) {
        bytes.prepend(QByteArray::fromHex("EFBBBF"));
    }

    return bytes;
}

// ---------------------------------------------------------------------------
// 换行风格
// ---------------------------------------------------------------------------

LineEnding Document::detectLineEnding(const QByteArray &data, bool *hasMixed)
{
    int lf = 0;
    int crlf = 0;
    int cr = 0;

    for (int i = 0; i < data.size(); i++) {
        if (data.at(i) == '\n') {
            if (i > 0 && data.at(i - 1) == '\r') {
                crlf++;
            } else {
                lf++;
            }
        } else if (data.at(i) == '\r' && (i + 1 >= data.size() || data.at(i + 1) != '\n')) {
            cr++;
        }
    }

    if (hasMixed != nullptr) {
        *hasMixed = ((lf > 0 ? 1 : 0) + (crlf > 0 ? 1 : 0) + (cr > 0 ? 1 : 0)) > 1;
    }

    if (lf == 0 && crlf == 0 && cr == 0) {
        return LineEndingLf;
    }

    return (crlf >= lf && crlf >= cr) ? LineEndingCrLf : LineEndingLf;
}

void Document::setLineEnding(LineEnding lineEnding)
{
    if (_lineEnding == lineEnding) {
        return;
    }

    _lineEnding = lineEnding;
    convertLineEnding(lineEnding);
    _undoStack.clear();
    _redoStack.clear();
}

void Document::convertLineEnding(LineEnding lineEnding)
{
    std::string text = _buffer->getText();

    if (text.empty()) {
        return;
    }

    std::string converted;
    converted.reserve(text.size() + text.size() / 16);

    for (size_t i = 0; i < text.size(); i++) {
        if (text[i] != '\n') {
            converted.push_back(text[i]);

            continue;
        }

        if (lineEnding == LineEndingCrLf) {
            converted.push_back('\r');
        }

        converted.push_back('\n');
    }

    if (converted == text) {
        return;
    }

    _buffer->clear();
    _buffer->insertText(0, converted.data(), converted.size());
    _savedVersion = _buffer->version();
}

// ---------------------------------------------------------------------------
// 编辑与撤销
// ---------------------------------------------------------------------------

uint32_t Document::insertText(uint32_t offset, const std::string &text)
{
    if (text.empty()) {
        return 0;
    }

    UndoRecord record;
    record.position = offset;
    record.removedLength = 0;
    record.insertedText = text;
    record.cursorBefore = _editCursor;
    record.cursorAfter = offset + static_cast<uint32_t>(text.size());

    const uint32_t inserted = _buffer->insertText(offset, text.data(), text.size());
    pushUndo(record);

    return inserted;
}

uint32_t Document::removeRange(uint32_t startOffset, uint32_t endOffset)
{
    if (startOffset >= endOffset) {
        return 0;
    }

    UndoRecord record;
    record.position = startOffset;
    record.removedLength = endOffset - startOffset;
    record.removedText = _buffer->getTextRange(startOffset, endOffset);
    record.cursorBefore = _editCursor;
    record.cursorAfter = startOffset;

    const uint32_t removed = _buffer->deleteRange(startOffset, endOffset);
    pushUndo(record);

    return removed;
}

uint32_t Document::replaceRange(uint32_t startOffset, uint32_t endOffset, const std::string &text)
{
    if (startOffset == endOffset && text.empty()) {
        return 0;
    }

    UndoRecord record;
    record.position = startOffset;
    record.removedLength = (endOffset > startOffset) ? (endOffset - startOffset) : 0;
    record.removedText = (endOffset > startOffset) ? _buffer->getTextRange(startOffset, endOffset) : std::string();
    record.insertedText = text;
    record.cursorBefore = _editCursor;
    record.cursorAfter = startOffset + static_cast<uint32_t>(text.size());

    const uint32_t removed = _buffer->replaceRange(startOffset, endOffset, text.data(), text.size());
    pushUndo(record);

    return removed;
}

void Document::pushUndo(const UndoRecord &record)
{
    if (_undoStack.size() >= MaxUndoSteps) {
        _undoStack.removeFirst();
    }

    _undoStack.append(record);
    _redoStack.clear();
}

bool Document::undo()
{
    if (_undoStack.isEmpty()) {
        return false;
    }

    const UndoRecord record = _undoStack.takeLast();
    _buffer->replaceRange(record.position,
                          record.position + static_cast<uint32_t>(record.insertedText.size()),
                          record.removedText.data(),
                          record.removedText.size());
    _redoStack.append(record);
    _editCursor = record.cursorBefore;

    return true;
}

bool Document::redo()
{
    if (_redoStack.isEmpty()) {
        return false;
    }

    const UndoRecord record = _redoStack.takeLast();
    _buffer->replaceRange(record.position,
                          record.position + static_cast<uint32_t>(record.removedText.size()),
                          record.insertedText.data(),
                          record.insertedText.size());
    _undoStack.append(record);
    _editCursor = record.cursorAfter;

    return true;
}

void Document::clearHistory()
{
    _undoStack.clear();
    _redoStack.clear();
}

void Document::releaseUndoMemory()
{
    for (int i = 0; i < _undoStack.size(); i++) {
        _undoStack[i].removedText.clear();
        _undoStack[i].insertedText.clear();
    }
}
