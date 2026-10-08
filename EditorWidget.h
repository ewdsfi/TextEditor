#ifndef EDITORWIDGET_H
#define EDITORWIDGET_H

#include <QWidget>

class Document;
class EditorView;
class QLabel;
class QMenu;
class QMenuBar;
class QScrollBar;
class QStatusBar;

/// 编辑器主窗口：用代码完成整体排版，组合编辑视图、滚动条、菜单栏与状态栏，
/// 并负责新建、打开、保存、另存为等文件级操作。
class EditorWidget : public QWidget
{
    Q_OBJECT

public:
    /// 构造窗口、创建菜单并装配默认文档。
    explicit EditorWidget(QWidget *parent = nullptr);

    /// 释放默认文档。
    ~EditorWidget() override;

    /// 打开命令行传入的文件，失败时保持当前文档不变。
    void openFileFromCommandLine(const QString &path);

protected:
    /// 关闭前询问未保存的修改。
    void closeEvent(QCloseEvent *event) override;

private slots:
    /// 刷新滚动条范围与当前位置。
    void syncScrollBars();

    /// 竖向滚动条被拖动时同步视图。
    void onVerticalScroll(int value);

    /// 横向滚动条被拖动时同步视图。
    void onHorizontalScroll(int value);

    /// 新建空白文档。
    void newFile();

    /// 打开文件。
    void openFile();

    /// 保存到当前文件，未命名时转为另存为。
    void saveFile();

    /// 另存为新文件。
    void saveFileAs();

    /// 光标或选区变化后刷新状态栏。
    void updateStatus();

    /// 内容变化后刷新标题与状态栏。
    void updateTitle();

    /// 切换换行风格。
    void toggleLineEnding();

private:
    /// 创建菜单栏与全部动作。
    void createMenus();

    /// 创建状态栏。
    void createStatusBar();

    /// 把文档装配到视图，接管其生命周期。
    void setDocument(Document *document);

    /// 装载文件，必要时先处理未保存的修改。
    void loadFile(const QString &path);

    /// 询问是否保存当前文档，返回 false 表示用户取消了操作。
    bool confirmSave();

    /// 刷新换行风格按钮的文字。
    void updateLineEndingAction();

    EditorView *_view = nullptr;        ///< 编辑视图
    QScrollBar *_verticalBar = nullptr; ///< 竖向滚动条
    QScrollBar *_horizontalBar = nullptr; ///< 横向滚动条
    QWidget *_corner = nullptr;         ///< 滚动条交角处的小方块
    QMenuBar *_menuBar = nullptr;       ///< 菜单栏
    QStatusBar *_statusBar = nullptr;   ///< 状态栏
    QMenu *_fileMenu = nullptr;         ///< 文件菜单
    QMenu *_editMenu = nullptr;         ///< 编辑菜单
    QLabel *_positionLabel = nullptr;   ///< 状态栏左侧的坐标信息
    QLabel *_infoLabel = nullptr;       ///< 状态栏中途的信息
    QLabel *_pathLabel = nullptr;       ///< 状态栏右侧的文件路径
    QAction *_lineEndingAction = nullptr; ///< 换行风格切换动作
    Document *_document = nullptr;      ///< 当前文档，由窗口持有
    bool _syncingBars = false;          ///< 防止滚动条与视图互相触发
};

#endif // EDITORWIDGET_H
