# PDF Watermark (C / Windows)

一个用于 **Windows 的 PDF 水印工具**,带图形界面和命令行两种模式。
纯 C 实现,自带 PDF 解析/重写引擎,使用 [MuPDF](https://mupdf.com/) 做页面渲染,
水印字体以**子集方式嵌入**输出文件,因此在任何阅读器里显示效果都一致。

> 本项目是基于 [bastienlc/pdf-watermark](https://github.com/bastienlc/pdf-watermark)
> (Python 版)的 C / Windows 重写实现,面向需要原生 GUI 与免 Python 环境的场景。

---

## 特性

- **两种模式**
  - `grid` —— 在每页上按网格重复水印
  - `insert` —— 在每页指定位置放置单个水印
- **图形界面 (GUI)**:实时预览、滑块调参、颜色选择器
- **命令行 (CLI)**:便于批处理与脚本调用
- **水印参数**:文字、字号 (px)、颜色、旋转角度、透明度、网格行列数、页边距
- **CJK 支持**:中文水印以 GBK/Identity-H 编码,**黑体字形子集嵌入** PDF(约 +130 KB/文件),
  不依赖阅读器的字体替换,预览与输出完全一致
- **真实页面预览**:用 MuPDF 渲染页面后叠加水印,所见即所得
- **正确的页面旋转**:支持 `/Rotate 0/90/180/270`,横版页面方向正确
- **幂等**:对同一文件反复保存不会叠加水印(只会替换上次写入的水印)
- **进度保真**:原页面内容以字节级原样保留(文字可选中、矢量图不失真)
- **无需第三方 PDF 库**:解析/xref/对象流/写回全部为自研代码,仅渲染用 MuPDF

---

## 快速开始

### 1. 获取依赖

需要以下三样(都不在仓库里,见 [依赖说明](#依赖说明)):

| 依赖 | 说明 |
|---|---|
| MuPDF 源码 | 用于构建 `libmupdf.a`(渲染 + 字体子集化) |
| 第三方 amalgamation | SumatraPDF 的 `ext/a-*` 目录 |
| 内置字体 | MuPDF 的 `resources/fonts`(urw/droid/sil/han) |

推荐目录结构(与脚本默认路径一致):

```
<父目录>/
├── sumatrapdf/            # git clone https://github.com/sumatrapdfreader/sumatrapdf
├── mupdf_build/           # 由 tools/build_mupdf.sh 生成 libmupdf.a
└── pdf-watermark/         # 本项目
```

### 2. 构建 MuPDF 静态库

```sh
cd pdf-watermark
sh tools/build_mupdf.sh
# 产物: ../mupdf_build/libmupdf.a
```

### 3. 放置内置字体

把 MuPDF 的字体目录复制到 `pdf-watermark/mupdf_fonts/`:

```
mupdf_fonts/
├── urw/     # Helvetica/Times/Courier 的标准替代 (Nimbus)
├── droid/   # CJK 回退字体
├── sil/     # Charis SIL
└── han/     # 思源宋体 (可选, CJK)
```

### 4. 编译

```bat
build.bat
```

或使用 make:

```sh
make
```

> 默认编译器路径为 TDM-GCC 64-bit。若路径不同,设置环境变量 `CC`:
> ```bat
> set CC=D:\mingw64\bin\gcc.exe
> build.bat
> ```

产物:`pdfwatermark.exe`

---

## 使用

### 图形界面

直接双击 `pdfwatermark.exe`,或:

```bat
pdfwatermark.exe
pdfwatermark.exe  input.pdf      :: 启动并自动打开该文件
```

界面左侧为参数区(文字、字号、颜色、角度、透明度、网格),右侧为页面预览。
调参即时预览,点“保存”写出文件。

### 命令行

```
pdfwatermark.exe <grid|insert> <文字> <输入.pdf> [选项]
```

**选项**

| 选项 | 说明 | 默认 |
|---|---|---|
| `-s, --save <文件>` | 输出文件 | `<原名>_watermark.pdf` |
| `-o, --opacity <0..1>` | 透明度 | `0.2` |
| `-a, --angle <度>` | 倾斜角度 (逆时针) | `45` |
| `-tc, --text-color <#rrggbb>` | 文字颜色 | `#FF0000` |
| `-ts, --text-size <px>` | 字号 (像素, 最大 200) | `50` |
| `-h, --horizontal-boxes <n>` | 水平重复数 | `3` |
| `-v, --vertical-boxes <n>` | 垂直重复数 | `6` |
| `-m, --margin` | 网格留边距 | 切边 |
| `-x, --x <0..1>` / `-y, --y <0..1>` | `insert` 模式位置 | `0.5` |

**示例**

```bat
:: 网格水印
pdfwatermark.exe grid "机密文件" input.pdf -s out.pdf

:: 单个居中水印,半透明蓝色,30 度
pdfwatermark.exe insert "CONFIDENTIAL" input.pdf -s out.pdf ^
    -tc #0000FF -o 0.35 -a 30 -x 0.5 -y 0.5

:: 中文水印,80px,20% 透明
pdfwatermark.exe grid "内部资料 请勿外传" input.pdf -s out.pdf -ts 80 -o 0.2
```

---

## 实现要点

源码位于 `src/`:

| 文件 | 职责 |
|---|---|
| `pdf.c` / `pdf.h` | PDF 解析与重写:经典 xref、xref 流(PNG predictor)、对象流 `ObjStm`、页面树、`/Rotate` 继承、写回、水印注入 |
| `pdfwatermark.c` | GUI (Win32 + GDI) 与 CLI 入口、参数解析、预览绘制 |
| `mupdf_render.c` / `.h` | 用 MuPDF 把页面渲染成 `HBITMAP` 供预览 |
| `mupdf_wm.c` / `.h` | 用 MuPDF 生成**嵌入字体子集**的水印覆盖层,再并入目标文档 |
| `ccitt.c` / `ccitt.h` | CCITT G3/G4 传真解码(自研渲染路径使用) |

### 水印是怎么写进去的

1. 用 MuPDF 建立字体并做**子集化**(只嵌入水印用到的字形,~130 KB)
2. **自己书写内容流**,使用标准 PDF 文本矩阵 `Tm = [cos, sin, -sin, cos, e, f]`
   (行列式 = +1,保证字形不被镜像)
3. 用一对 `q` / `Q` 把**原页面内容包起来**,避免原内容残留的 `cm`
   污染水印(例如某些生成器会留下未配对的 y 翻转)
4. 把字体资源并入页面 `/Resources`
5. 重新保存时,识别并剥离上次写入的水印内容流(靠 `/PWWM` 标记),实现幂等

### 关于 `/Rotate`

页面 `MediaBox` 可能是竖版而 `/Rotate 90` 显示为横版。水印按**显示方向**布局,
并在内容流开头插入旋转矩阵映射回页面坐标:

| /Rotate | 矩阵 |
|---|---|
| 90 | `0 1 -1 0 pw 0` |
| 180 | `-1 0 0 -1 pw ph` |
| 270 | `0 -1 1 0 0 ph` |

---

## 依赖说明

以下内容**未包含**在本仓库中(体积过大 / 属第三方):

- **MuPDF** —— 渲染引擎,AGPL-3.0。
  <https://github.com/ArtifexSoftware/mupdf>
- **SumatraPDF** —— 本项目使用其 `ext/` 下 vendored 的第三方库
  amalgamation(freetype / harfbuzz / jbig2dec / lcms2 / openjpeg /
  libjpeg-turbo / zlib / brotli)。<https://github.com/sumatrapdfreader/sumatrapdf>
- **字体** —— MuPDF 的 `resources/fonts`。

> ⚠️ **许可证提醒**:MuPDF 采用 **AGPL-3.0**。链接它会使整个程序受 AGPL 约束
> (向使用者提供源码)。若需闭源分发,请考虑 MuPDF 的商业授权,
> 或改用其它许可宽松的渲染后端。

---

## 已知限制

- 仅支持 **Windows**(Win32 API + GDI 预览)
- 预览只渲染**第一页**;保存时对所有页面生效
- 内置字体不随仓库分发,需自行放置(见上文)

---

## 致谢

- 布局与参数设计参考 [bastienlc/pdf-watermark](https://github.com/bastienlc/pdf-watermark)
- 渲染与字体嵌入使用 [MuPDF](https://mupdf.com/)(Artifex Software)
- 第三方库打包形式取自 [SumatraPDF](https://github.com/sumatrapdfreader/sumatrapdf)

## 许可证

本仓库自有代码(见 `src/`, `tools/`)以 MIT 许可发布,见 `LICENSE`。
**但链接 MuPDF 后,整体分发须遵守 AGPL-3.0**,详见上文。
