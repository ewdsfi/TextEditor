#ifndef DOCUMENT_H
#define DOCUMENT_H

#include "TextBuffer.h"

#include <QByteArray>
#include <QString>
#include <QVector>

/// 文章使用的换行风格。
enum LineEnding
{
    LineEndingLf = 0,  ///< Unix 风格 \n
    LineEndingCrLf = 1 ///< Windows 风格 \r\n
};

/// 文章使用的编码，内部统一按 UTF-8 存放。
enum TextEncoding
{
    EncodingUtf8 = 0,    ///< 无 BOM 的 UTF-8
    EncodingUtf8Bom = 1, ///< 带 BOM 的 UTF-8
    EncodingUtf16Le = 2, ///< 小端 UTF-16
    EncodingUtf16Be = 3  ///< 大端 UTF-16
};

/// 一次编辑的逆操作，用于撤销与重做。
struct UndoRecord
{
    uint32_t position = 0;        ///< 被替换区间在文档中的起始字节下标
    uint32_t removedLength = 0;   ///< 被替换掉的字节数
    std::string removedText;      ///< 被替换掉的原文本
    std::string insertedText;     ///< 新插入的文本
    uint32_t cursorBefore = 0;    ///< 编辑前的光标字节下标
    uint32_t cursorAfter = 0;     ///< 编辑后的光标字节下标
};

/// 文档：把纯 C++ 的文本缓冲区包装成可读写的文件实体，
/// 负责文件加载与保存、编码与换行风格识别、编码格式转换以及撤销重做栈。
class Document
{
public:
    /// 创建一个空白文档，行尾默认使用 LF。
    Document();

    /// 释放文本缓冲区与撤销栈。
    ~Document();

    /// 禁止拷贝，文档独占文本缓冲区。
    Document(const Document &) = delete;

    /// 禁止赋值，文档独占文本缓冲区。
    Document &operator=(const Document &) = delete;

    /// 读取文件并替换当前内容，成功返回 true。
    bool loadFromFile(const QString &path);

    /// 保存到当前文件，若尚未命名则要求先指定路径。
    bool save();

    /// 另存到指定路径，成功后会切换当前文件路径。
    bool saveAs(const QString &path);

    /// 清空内容并解除文件关联，回到未命名状态。
    void createNew();

    /// 用一段文本替换文档全部内容。
    void setText(const std::string &text);

    /// 文本缓冲区，视图通过它直接访问模型。
    TextBuffer *buffer() const { return _buffer; }

    /// 取得某一行的文本，越界返回空串。
    QString lineText(uint32_t line) const;

    /// 最长一行的字节数，供水平滚动条估算内容宽度。
    uint32_t maxLineBytes() const { return _buffer->maxLineBytes(); }

    /// 文档是否为空（只有一个空行）。
    bool isEmpty() const;

    /// 文档是否已被修改。
    bool isModified() const { return _buffer->version() != _savedVersion; }

    /// 当前文件路径，未命名时为空串。
    const QString &filePath() const { return _filePath; }

    /// 窗口标题使用的名称，已修改时带星号。
    QString title() const;

    /// 当前换行风格。
    LineEnding lineEnding() const { return _lineEnding; }

    /// 当前编码格式。
    TextEncoding encoding() const { return _encoding; }

    /// 切换换行风格，文档内容会随之整体转换。
    void setLineEnding(LineEnding lineEnding);

    /// 在指定字节下标处插入文本，返回插入的字节数。
    uint32_t insertText(uint32_t offset, const std::string &text);

    /// 删除一段文本，返回被删除的字节数。
    uint32_t removeRange(uint32_t startOffset, uint32_t endOffset);

    /// 用文本替换一段区间，内部记录一条撤销记录。
    uint32_t replaceRange(uint32_t startOffset, uint32_t endOffset, const std::string &text);

    /// 把光标移动到指定位置，用于编辑时记录撤销点。
    void setEditCursor(uint32_t cursor) { _editCursor = cursor; }

    /// 最近一次编辑后的光标位置，撤销与重做后由视图读取。
    uint32_t editCursor() const { return _editCursor; }

    /// 是否可以撤销。
    bool canUndo() const { return !_undoStack.isEmpty(); }

    /// 是否可以重做。
    bool canRedo() const { return !_redoStack.isEmpty(); }

    /// 撤销一次编辑，成功返回 true。
    bool undo();

    /// 重做一次编辑，成功返回 true。
    bool redo();

    /// 清空撤销与重做记录。
    void clearHistory();

    /// 释放撤销记录占用的内存。
    void releaseUndoMemory();

private:
    /// 把一条逆操作压入撤销栈，同时丢弃重做记录。
    void pushUndo(const UndoRecord &record);

    /// 识别换行风格并转换、规范化成内部的 \n。
    void normalizeText(QByteArray &data);

    /// 按内部换行风格把缓冲区文本转换成待写盘的字节序列。
    QByteArray outputBytes() const;

    /// 把当前缓冲区内容调整成指定的换行风格。
    void convertLineEnding(LineEnding lineEnding);

    /// 统计一段字节序列里占主导的换行风格。
    static LineEnding detectLineEnding(const QByteArray &data, bool *hasMixed);

    TextBuffer *_buffer = nullptr;      ///< 文本缓冲区
    QString _filePath;                  ///< 当前文件路径
    LineEnding _lineEnding = LineEndingLf;   ///< 当前换行风格
    TextEncoding _encoding = EncodingUtf8;   ///< 当前编码格式
    uint32_t _savedVersion = 0;         ///< 最近一次保存时的缓冲区版本号
    uint32_t _editCursor = 0;           ///< 当前光标的字节下标

    QVector<UndoRecord> _undoStack;     ///< 撤销栈
    QVector<UndoRecord> _redoStack;     ///< 重做栈
};

#endif // DOCUMENT_H
