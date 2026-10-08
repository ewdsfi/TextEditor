#ifndef EDITORVIEW_H
#define EDITORVIEW_H

#include "Document.h"

#include <QPoint>
#include <QString>
#include <QWidget>

class QKeyEvent;
class QMouseEvent;
class QPaintEvent;
class QTimer;
class QWheelEvent;
class TextLayout;

/// 编辑器视图：负责文本绘制、光标与选区显示、行号栏绘制以及键盘鼠标交互。
/// 视图只通过 Document 暴露的接口访问文本模型，不直接触碰 Piece Tree。
class EditorView : public QWidget
{
    Q_OBJECT

public:
    /// 构造视图并初始化字体、排版器、光标闪烁与拖拽滚动定时器。
    explicit EditorView(QWidget *parent = nullptr);

    /// 释放视图自有的排版器。
    ~EditorView() override;

    /// 绑定文档，视图不持有文档的生命周期。
    void setDocument(Document *document);

    /// 当前文档。
    Document *document() const { return _document; }

    /// 设置编辑字体，同时刷新排版度量。
    void setEditorFont(const QFont &font);

    /// 当前编辑字体。
    const QFont &editorFont() const { return _font; }

    /// 设置光标每秒闪烁次数，0 表示不闪烁。
    void setCursorBlinkRate(int perSecond);

    /// 行号栏宽度，也是文字区的左边界。
    int gutterWidth() const { return _gutterWidth; }

    /// 行高，供外层换算滚动范围使用。
    int lineHeight() const { return _lineHeight; }

    /// 可视区域内能显示的行数。
    int visibleRows() const;

    /// 内容总高度，即行数乘行高。
    int contentHeight();

    /// 内容总宽度，取已排版行与超长行估算值的较大者。
    int contentWidth();

    /// 光标所在行号。
    int cursorLine() const { return _cursorLine; }

    /// 光标所在列号。
    int cursorColumn() const { return _cursorColumn; }

    /// 是否存在有效选区。
    bool hasSelection() const;

    /// 选区起点，无选区时返回光标位置。
    TextPosition selectionStart() const;

    /// 选区终点，无选区时返回光标位置。
    TextPosition selectionEnd() const;

    /// 选区涉及的字节数，无选区时为 0。
    int selectionLength() const;

    /// 选中全部文本。
    void selectAll();

    /// 清除选区，保留光标位置。
    void clearSelection();

    /// 取得选区文本，无选区时返回空串。
    QString selectedText() const;

    /// 把光标移动到指定行列，extend 为真时扩展选区。
    void moveCursorTo(int line, int column, bool extend);

    /// 把光标移动到指定字节下标处。
    void moveCursorToOffset(uint32_t offset, bool extend);

    /// 光标所在位置的字节下标。
    uint32_t cursorOffset() const;

    /// 用一段文本替换选区，无选区时即为普通插入。
    void insertTextAtCursor(const QString &text);

    /// 在光标处换行。
    void insertNewLine();

    /// 删除光标前的一个字符或选区。
    void backspace();

    /// 删除光标后的一个字符或选区。
    void deleteForward();

    /// 删除光标前的一个词。
    void deleteWordBefore();

    /// 复制选区到剪贴板。
    void copy();

    /// 剪切选区到剪贴板。
    void cut();

    /// 粘贴剪贴板内容。
    void paste();

    /// 撤销一次编辑。
    void undo();

    /// 重做一次编辑。
    void redo();

    /// 滚动到指定位置，越界会自动收敛。
    void setScrollOffsets(int y, int x);

    /// 竖向滚动指定像素。
    void scrollBy(int dy);

    /// 把光标滚动进可视区域，centerVertically 为真时尽量居中。
    void ensureCursorVisible(bool centerVertically = false);

    /// 文档被整体替换后重置光标、选区与滚动位置。
    void resetViewState();

    /// 刷新滚动条所需的尺寸信息，并重绘视图。
    void refreshLayout();

signals:
    /// 光标或选区发生变化时发出。
    void cursorMoved();

    /// 文档内容被编辑后发出。
    void documentEdited();

protected:
    /// 绘制行号栏、文本、选区与光标。
    void paintEvent(QPaintEvent *event) override;

    /// 处理按键、快捷键与文本录入。
    void keyPressEvent(QKeyEvent *event) override;

    /// 处理点击定位光标。
    void mousePressEvent(QMouseEvent *event) override;

    /// 处理拖拽选择与拖拽期间的自动滚动。
    void mouseMoveEvent(QMouseEvent *event) override;

    /// 结束拖拽选择。
    void mouseReleaseEvent(QMouseEvent *event) override;

    /// 处理双击选词。
    void mouseDoubleClickEvent(QMouseEvent *event) override;

    /// 处理滚轮滚动。
    void wheelEvent(QWheelEvent *event) override;

    /// 获得焦点后让光标开始闪烁。
    void focusInEvent(QFocusEvent *event) override;

    /// 失去焦点后停止闪烁并保持光标可见。
    void focusOutEvent(QFocusEvent *event) override;

private slots:
    /// 切换光标可见性，实现闪烁。
    void blinkCursor();

    /// 拖拽到可视区域外时持续滚动。
    void autoScrollOnDrag();

private:
    /// 把控件坐标换算成文档位置，越界自动收敛。
    TextPosition positionAtPoint(const QPoint &point) const;

    /// 计算一行的起始纵坐标，以文档顶端为零点。
    int yForLine(int line) const;

    /// 保证指定行的排版信息可用。
    void prepareLine(int line);

    /// 重绘整个视图。
    void updateTextArea();

    /// 只重绘光标附近区域。
    void updateCursorRect();

    /// 设定光标位置，extend 为真时保留选区。
    void setCursorPosition(int line, int column, bool extend);

    /// 按行列增量移动光标，竖向移动保留首选列。
    void moveCursorBy(int lineDelta, int columnDelta, bool extend);

    /// 取得一行的全文，越界返回空串。
    QString lineText(int line);

    /// 取得一行的字节数，越界返回 0。
    uint32_t lineByteLength(int line);

    /// 取得一行的 UTF-16 单元数，即最大列号。
    int lineUnits(int line);

    /// 选中某个位置所在的单词。
    void selectWordAt(int line, int column);

    /// 取得某个列号左侧一个字符的字节数，用于退格。
    uint32_t bytesBeforeColumn(int line, int column);

    /// 取得某个列号右侧一个字符的字节数，用于删除键。
    uint32_t bytesAfterColumn(int line, int column);

    /// 判断字符是否参与构成一个词。
    static bool isWordChar(const QChar &ch);

    /// 重置闪烁计时并让光标立刻可见。
    void restartBlink();

    Document *_document = nullptr;  ///< 当前文档，不持有所有权
    TextLayout *_layout = nullptr;  ///< 当前行的排版器
    QFont _font;                    ///< 编辑字体
    QFont _gutterFont;              ///< 行号字体
    int _lineHeight = 16;           ///< 行高
    int _gutterWidth = 60;          ///< 行号栏宽度

    int _scrollY = 0;               ///< 竖向滚动偏移
    int _scrollX = 0;               ///< 横向滚动偏移

    int _cursorLine = 0;            ///< 光标行号
    int _cursorColumn = 0;          ///< 光标列号
    int _preferredColumn = 0;       ///< 竖向移动时保留的首选列
    bool _hasAnchor = false;        ///< 是否存在选择锚点
    int _anchorLine = 0;            ///< 选择锚点行号
    int _anchorColumn = 0;          ///< 选择锚点列号

    int _clickCount = 0;            ///< 连续点击次数
    QPoint _dragPoint;              ///< 最近一次拖拽位置
    bool _dragging = false;         ///< 是否处于拖拽选择状态

    int _blinkRate = 1;             ///< 每秒闪烁次数
    bool _cursorVisible = true;     ///< 光标当前是否可见
    QTimer *_blinkTimer = nullptr;  ///< 光标闪烁定时器
    QTimer *_dragTimer = nullptr;   ///< 拖拽自动滚动定时器
};

#endif // EDITORVIEW_H
