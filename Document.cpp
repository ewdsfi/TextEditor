#include "Document.h"

#include <QFile>
#include <QFileInfo>

// ===================== File I/O =====================

/// Writes CRLF on Windows, keeps LF elsewhere
static QByteArray outputBytes(const std::string &text)
{
    QByteArray bytes(text.data(), static_cast<int>(text.size()));

#ifdef Q_OS_WIN
    // the buffer only ever holds LF, so the conversion happens here and we
    // never end up writing \r\r\n, which is what a second pass would produce
    bytes.replace('\n', QByteArray("\r\n"));
#endif

    return bytes;
}

Document::Document()
{
    // one buffer for the whole life of the document, loadFromFile() reuses it
    _buffer = new TextBuffer();
}

Document::~Document()
{
    delete _buffer;
}

bool Document::loadFromFile(const QString &path)
{
    QFile file(path);

    // read only, the file on disk stays untouched until the user saves
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }

    QByteArray data = file.readAll();

    file.close();

    // drop a leading BOM, otherwise an invisible character shows up at the top
    const QByteArray bom = QByteArray::fromHex("EFBBBF");

    if (data.startsWith(bom)) {
        data.remove(0, bom.size());
    }

    // start from a clean slate, we can be called on a document that already has text
    _buffer->clear();
    _buffer->insertText(0, data.constData(), static_cast<size_t>(data.size()));

    // from here on the document has a name, the title bar and the status bar use it
    _filePath = path;
    // what is in the buffer is what is on disk right now, so we are not modified
    _savedVersion = _buffer->version();

    return true;
}

bool Document::save()
{
    if (_filePath.isEmpty()) {
        return false;
    }

    QFile file(_filePath);

    // truncate, we rewrite the whole file every single time
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }

    // the LF -> CRLF conversion lives in outputBytes(), the buffer itself is LF only
    const QByteArray bytes = outputBytes(_buffer->getText());
    const qint64 written = file.write(bytes);

    file.close();

    // compare the counts, a short write is a failure even though the file exists
    if (written != bytes.size()) {
        return false;
    }

    // buffer and file agree again, so the star in the title goes away
    _savedVersion = _buffer->version();

    return true;
}

void Document::createNew()
{
    _buffer->clear();
    _filePath.clear();
    _savedVersion = _buffer->version();
}

void Document::setText(const std::string &text)
{
    // treat the incoming text like a fresh load, the document starts out clean
    _buffer->clear();
    _buffer->insertText(0, text.data(), text.size());
    _savedVersion = _buffer->version();
}

bool Document::isEmpty() const
{
    return _buffer->byteCount() == 0;
}

QString Document::lineText(uint32_t line) const
{
    // asking for a line that isn't there is not worth a crash
    if (line >= _buffer->lineCount()) {
        return QString();
    }

    // only this line, so a huge single line is not copied whole
    const std::string content = _buffer->getLineContent(line);

    return QString::fromUtf8(content.data(), static_cast<int>(content.size()));
}

QString Document::title() const
{
    // an unnamed document still needs something in the title bar
    QString name = _filePath.isEmpty() ? QStringLiteral("未命名") : QFileInfo(_filePath).fileName();

    // the star is the only hint the user gets that there is unsaved work
    if (isModified()) {
        name += QLatin1Char('*');
    }

    return name;
}

// ===================== Editing =====================

uint32_t Document::insertText(uint32_t offset, const std::string &text)
{
    // inserting nothing is a no-op, don't bother the tree with it
    if (text.empty()) {
        return 0;
    }

    return _buffer->insertText(offset, text.data(), text.size());
}

uint32_t Document::removeRange(uint32_t startOffset, uint32_t endOffset)
{
    // an empty or a backwards range means there is nothing to delete
    if (startOffset >= endOffset) {
        return 0;
    }

    // half open range: [startOffset, endOffset)
    return _buffer->deleteRange(startOffset, endOffset);
}
