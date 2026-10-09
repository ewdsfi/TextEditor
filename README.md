# 文本编辑器（C++17 / Qt 6）

一个仿 VS Code 的桌面文本编辑器，核心目标是**高效编辑与渲染超长单行文本**
（单行 100 万字符级别）。文本模型使用分块文本缓冲区（Piece Table）实现，
与 Qt 完全解耦；视图只渲染可见区域，因此打开大文件不会卡顿。

## 模块划分

```
main.cpp            程序入口
TextBuffer.h/.cpp   文本模型：Piece Table，纯 C++，不依赖 Qt
Document.h/.cpp     文档层：按 UTF-8 读写文件、修改状态跟踪
TextLayout.h/.cpp   排版层：单行按需测量，列号 / 字节 / 像素互相换算
EditorView.h/.cpp   视图层：绘制文本与行号、光标闪烁、键盘鼠标交互
EditorWidget.h/.cpp 窗口层：代码排版、菜单、滚动条、状态栏
```

依赖方向单向向下：`EditorWidget → EditorView → TextLayout / Document → TextBuffer`，
越往下越与界面无关，替换视图不影响模型。

## 核心设计

### 1. 文本模型：Piece Table

一份文档由若干**文本块（Piece）**按顺序拼成，每块指向某个只读缓冲区中的一段
连续字节区间。插入和删除只改动块列表，从不改动已有缓冲区，因此：

- 编辑代价与文档总长度无关，只与块数量相关；
- 打开大文件后随机位置插入同样廉价；
- 已有缓冲区只读不改，块引用始终有效。

每块自带「字节长度」与「换行符个数」，并给出在缓冲区内的起始与结束位置
（行号 + 行内列）。缓冲区自带行首表，因此块内定位是二分查找而不是线性扫描。

块数量超过上限（4096）时自动整理一次：把全部内容顺序写进一个新的缓冲区，
块列表收缩回一块。这样既保留了增量编辑的低开销，又不会让块列表无限膨胀。

换行符在写入模型前统一归一成 `\n`，这样换行符永远不会被块边界劈开，
行首表与块的统计信息始终自洽；写盘时的行尾由 `Document` 决定，
Windows 下补成 `\r\n`。

### 2. 超长单行的渲染

单行排版（`TextLayout`）把一行明文转换成可测量的 UTF-16 序列，并按固定间隔
（每 32 个单元）记录锚点，保存「单元下标 / 累计像素宽度 / 行内字节偏移」三元组。

- 列号 → 像素：二分定位锚点后只推进几十个字符；
- 像素 → 列号：同样只在一小段内推进；
- 字节 ↔ 列：按锚点累计，多字节字符与代理对都不会被切坏。

因此无论一行有 100 个字符还是 100 万个字符，光标定位与点击命中的开销都在
微秒级。行宽超过上限（100 万像素）后停止逐字测量，剩余部分按已测部分的平均
宽度补齐，保证排版开销有上界。

视图绘制时先由滚动偏移算出可见的首末行号，再对可见的每一行取文本并绘制，
只渲染可见区域；横向滚动时同样只把可见的那一段交给 `QPainter`。

### 3. 光标与选区

- 光标位置用「行号 + 列号」表示，列号是 UTF-16 单元下标；
- 点击、拖拽、双击选词、三击选行；
- 方向键、Home/End、PageUp/PageDown、按词删除；
- 光标闪烁由定时器驱动，失焦时常亮。

### 4. 文件操作

`Document` 负责加载与保存：

- 文件一律按 UTF-8 打开（忽略文件头部的 BOM），内部统一用 `\n` 表示换行；
- 保存时 Windows 下写成 `\r\n`，其它平台保持 `\n`；
- 保存只写回文档已有的文件路径，未命名的文档没有可写目标；
- 修改状态跟踪，关闭或新建前提示保存。

编辑字体固定为 Windows 自带的 `Consolas`（12 号，等宽），不随系统默认字体变化。

## 快捷键

| 快捷键 | 功能 |
| --- | --- |
| Ctrl + A | 全选 |
| Ctrl + C | 复制 |
| Ctrl + X | 剪切 |
| Ctrl + V | 粘贴 |
| Ctrl + N / O / S | 新建 / 打开 / 保存 |

## 构建与运行

需要 Qt 6（Widgets 模块）与支持 C++17 的编译器。

```bash
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=<Qt 安装路径>
cmake --build build
./build/TextEditor.exe [可选的文件路径]
```

## 正确性验证

`deepseek-temp/test_buffer.cpp` 是文本模型的独立测试，用 `std::string` 作为参考
模型逐项比对，并校验块结构的自洽性：

```bash
cd deepseek-temp
g++ -std=c++17 -O1 -o test_buffer test_buffer.cpp ../TextBuffer.cpp
./test_buffer
```

覆盖范围：空缓冲区、多行读写、CRLF 归一、100 万字符单行的分块构建与中间插入、
跨块大范围删除、行首行尾边界、块边界处的插入、以及 8 组各 600 步的随机模糊测试。
当前结果：**通过 4905 项，失败 0 项**。

`deepseek-temp/test_document.cpp` 验证文件层：UTF-8（含 BOM）读取、CRLF 归一入库、
Windows 下按 CRLF 写盘、编辑后写回原文件、修改状态、空文件与未命名文档。

```bash
cd deepseek-temp
g++ -std=c++17 -finput-charset=UTF-8 -DQT_CORE_LIB \
    -isystem <Qt>/include/QtCore -isystem <Qt>/include -isystem <Qt>/mkspecs/win32-g++ \
    test_document.cpp ../Document.cpp ../TextBuffer.cpp -L <Qt>/lib -lQt6Core -o test_document
./test_document
```

当前结果：**通过 28 项，失败 0 项**。
