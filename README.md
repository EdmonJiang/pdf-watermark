# PDF Watermark (C / Windows)

[English](#english) | [中文](#中文)

A native **Windows PDF watermarking tool** with both a GUI and a CLI.
Written in plain C with a self-contained PDF parser/writer. Page rendering uses
[MuPDF](https://mupdf.com/), and the watermark font is **embedded as a subset**
so the output looks identical in every PDF viewer.

> A C / Windows rewrite of [bastienlc/pdf-watermark](https://github.com/bastienlc/pdf-watermark)
> (the Python original), aimed at users who want a native GUI and no Python runtime.

---

## English

### Features

- **Two modes**
  - `grid` — repeat the watermark in a grid on every page
  - `insert` — place a single watermark at a given position on every page
- **GUI** — live preview, slider controls, colour picker
- **CLI** — easy to script and batch
- **Watermark options** — text, size (px), colour, rotation, opacity,
  grid rows/columns
- **CJK support** — Chinese text is encoded as Identity-H and the **SimHei glyph
  subset is embedded** (~+130 KB per file). No reliance on viewer font
  substitution, so preview and output match exactly.
- **True page preview** — pages are rendered with MuPDF, then the watermark is
  overlaid; what you see is what you get
- **Correct page rotation** — honours `/Rotate 0/90/180/270`; landscape pages
  come out right
- **Idempotent** — saving the same file repeatedly replaces the previous
  watermark instead of stacking it
- **Content preserving** — the original page content is copied byte-for-byte
  (text stays selectable, vector art stays sharp)
- **No third-party PDF library for parsing** — xref, object streams, page tree
  and rewriting are all home-grown; MuPDF is only used for rendering and fonts

### Quick start

#### 1. Get the dependencies

Three things are needed (none are shipped in this repo — see
[Third-party components](#third-party-components)):

| Dependency | Purpose |
|---|---|
| MuPDF source | to build `libmupdf.a` (rendering + font subsetting) |
| Third-party amalgamations | SumatraPDF's `ext/a-*` directories |
| Built-in fonts | MuPDF's `resources/fonts` (urw/droid/sil/han) |

Recommended layout (matches the default paths used by the scripts):

```
<parent>/
├── sumatrapdf/            # git clone https://github.com/sumatrapdfreader/sumatrapdf
├── mupdf_build/           # libmupdf.a is produced here
└── pdf-watermark/         # this repository
```

#### 2. Build the MuPDF static library

```sh
cd pdf-watermark
sh tools/build_mupdf.sh
# output: ../mupdf_build/libmupdf.a
```

#### 3. Provide the built-in fonts

Copy MuPDF's font directory to `pdf-watermark/mupdf_fonts/`:

```
mupdf_fonts/
├── urw/     # standard Helvetica/Times/Courier substitutes (Nimbus)
├── droid/   # CJK fallback
├── sil/     # Charis SIL
└── han/     # Source Han Serif (optional, CJK)
```

#### 4. Build

```bat
build.bat
```

or with make:

```sh
make
```

> The default compiler path is TDM-GCC 64-bit. To use a different one, set `CC`:
> ```bat
> set CC=D:\mingw64\bin\gcc.exe
> build.bat
> ```

Output: `pdfwatermark.exe`

### Usage

#### GUI

Double-click `pdfwatermark.exe`, or:

```bat
pdfwatermark.exe
pdfwatermark.exe  input.pdf      :: launch and open that file
```

The left pane holds the parameters (text, size, colour, angle, opacity, grid);
the right pane shows the page preview. Parameters update the preview live;
click **Save** to write the file.

#### CLI

```
pdfwatermark.exe <grid|insert> <text> <input.pdf> [options]
```

**Options**

| Option | Description | Default |
|---|---|---|
| `-s, --save <file>` | output file | `<name>_watermark.pdf` |
| `-o, --opacity <0..1>` | opacity | `0.2` |
| `-a, --angle <deg>` | rotation (counter-clockwise) | `45` |
| `-tc, --text-color <#rrggbb>` | text colour | `#FF0000` |
| `-ts, --text-size <px>` | font size in pixels (max 200) | `50` |
| `-h, --horizontal-boxes <n>` | horizontal repeats | `3` |
| `-v, --vertical-boxes <n>` | vertical repeats | `2` |
| `-x, --x <0..1>` / `-y, --y <0..1>` | position for `insert` mode | `0.5` |

**Examples**

```bat
:: grid watermark
pdfwatermark.exe grid "CONFIDENTIAL" input.pdf -s out.pdf

:: single centred watermark, semi-transparent blue, 30 degrees
pdfwatermark.exe insert "CONFIDENTIAL" input.pdf -s out.pdf ^
    -tc #0000FF -o 0.35 -a 30 -x 0.5 -y 0.5

:: Chinese watermark, 80 px, 20% opacity
pdfwatermark.exe grid "内部资料 请勿外传" input.pdf -s out.pdf -ts 80 -o 0.2
```

### Implementation notes

Sources live in `src/`:

| File | Responsibility |
|---|---|
| `pdf.c` / `pdf.h` | PDF parsing & rewriting: classic xref, xref streams (PNG predictor), object streams (`ObjStm`), page tree, `/Rotate` inheritance, save, watermark injection |
| `pdfwatermark.c` | GUI (Win32 + GDI) and CLI entry points, argument parsing, preview drawing |
| `mupdf_render.c` / `.h` | Render a page to an `HBITMAP` with MuPDF for the preview |
| `mupdf_wm.c` / `.h` | Build an embedded-font-subset watermark overlay with MuPDF, then merge it into the target document |
| `ccitt.c` / `ccitt.h` | CCITT G3/G4 fax decoder (used by the built-in renderer) |

#### How the watermark is written

1. MuPDF creates the font and **subsets it** (only the glyphs used are embedded,
   ~130 KB)
2. The **content stream is written by hand**, using the standard PDF text matrix
   `Tm = [cos, sin, -sin, cos, e, f]` (determinant `+1`, so glyphs are never
   mirrored)
3. The **original page content is wrapped in `q` / `Q`**, so a stray `cm` left
   behind by the source (some generators emit an unpaired y-flip) cannot leak
   into the watermark
4. The font resource is merged into the page `/Resources`
5. On re-save, the previous watermark content stream is detected and stripped
   (via `/PWWM` markers), making the operation idempotent

#### About `/Rotate`

A page's `MediaBox` may be portrait while `/Rotate 90` displays it as landscape.
The watermark is laid out in **display orientation**, and a rotation matrix is
prepended to the content stream to map back into page coordinates:

| /Rotate | Matrix |
|---|---|
| 90 | `0 1 -1 0 pw 0` |
| 180 | `-1 0 0 -1 pw ph` |
| 270 | `0 -1 1 0 0 ph` |

### Third-party components

The following are **not** included in this repository (too large / third-party):

- **MuPDF** — rendering engine, AGPL-3.0.
  <https://github.com/ArtifexSoftware/mupdf>
- **SumatraPDF** — this project uses the vendored third-party amalgamations
  under its `ext/` directory (freetype / harfbuzz / jbig2dec / lcms2 / openjpeg /
  libjpeg-turbo / zlib / brotli).
  <https://github.com/sumatrapdfreader/sumatrapdf>
- **Fonts** — MuPDF's `resources/fonts`.

> ⚠️ **Licence notice**: MuPDF is **AGPL-3.0**. Linking against it makes the
> whole program subject to the AGPL (you must make the complete corresponding
> source available to users). For closed-source distribution, consider a
> commercial MuPDF licence, or a different rendering backend with a permissive
> licence.

### Known limitations

- **Windows only** (Win32 API + GDI preview)
- The preview renders the **first page**; saving applies to all pages
- Built-in fonts are not shipped in the repo — provide them as described above

### Credits

- Layout and option design follow
  [bastienlc/pdf-watermark](https://github.com/bastienlc/pdf-watermark)
- Rendering and font embedding by [MuPDF](https://mupdf.com/) (Artifex Software)
- Third-party bundling format taken from
  [SumatraPDF](https://github.com/sumatrapdfreader/sumatrapdf)

### Licence

The code in this repository (`src/`, `tools/`) is released under the MIT licence,
see `LICENSE`. **However, because it links MuPDF, distributing binaries is
subject to AGPL-3.0** — see above.

---

## 中文

一个用于 **Windows 的 PDF 水印工具**,带图形界面和命令行两种模式。
纯 C 实现,自带 PDF 解析/重写引擎,使用 [MuPDF](https://mupdf.com/) 做页面渲染,
水印字体以**子集方式嵌入**输出文件,因此在任何阅读器里显示效果都一致。

> 本项目是基于 [bastienlc/pdf-watermark](https://github.com/bastienlc/pdf-watermark)
> (Python 版)的 C / Windows 重写实现,面向需要原生 GUI 与免 Python 环境的场景。

### 特性

- **两种模式**
  - `grid` —— 在每页上按网格重复水印
  - `insert` —— 在每页指定位置放置单个水印
- **图形界面 (GUI)**:实时预览、滑块调参、颜色选择器
- **命令行 (CLI)**:便于批处理与脚本调用
- **水印参数**:文字、字号 (px)、颜色、旋转角度、透明度、网格行列数、页边距
- **CJK 支持**:中文水印以 Identity-H 编码,**黑体字形子集嵌入** PDF(约 +130 KB/文件),
  不依赖阅读器的字体替换,预览与输出完全一致
- **真实页面预览**:用 MuPDF 渲染页面后叠加水印,所见即所得
- **正确的页面旋转**:支持 `/Rotate 0/90/180/270`,横版页面方向正确
- **幂等**:对同一文件反复保存不会叠加水印(只会替换上次写入的水印)
- **内容保真**:原页面内容以字节级原样保留(文字可选中、矢量图不失真)
- **解析无需第三方 PDF 库**:xref、对象流、页面树、写回全部为自研代码,
  仅渲染与字体使用 MuPDF

### 快速开始

#### 1. 获取依赖

需要以下三样(都不在仓库里,见[依赖说明](#依赖说明-1)):

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

#### 2. 构建 MuPDF 静态库

```sh
cd pdf-watermark
sh tools/build_mupdf.sh
# 产物: ../mupdf_build/libmupdf.a
```

#### 3. 放置内置字体

把 MuPDF 的字体目录复制到 `pdf-watermark/mupdf_fonts/`:

```
mupdf_fonts/
├── urw/     # Helvetica/Times/Courier 的标准替代 (Nimbus)
├── droid/   # CJK 回退字体
├── sil/     # Charis SIL
└── han/     # 思源宋体 (可选, CJK)
```

#### 4. 编译

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

### 使用

#### 图形界面

直接双击 `pdfwatermark.exe`,或:

```bat
pdfwatermark.exe
pdfwatermark.exe  input.pdf      :: 启动并自动打开该文件
```

界面左侧为参数区(文字、字号、颜色、角度、透明度、网格),右侧为页面预览。
调参即时预览,点“保存”写出文件。

#### 命令行

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
| `-v, --vertical-boxes <n>` | 垂直重复数 | `2` |
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

### 实现要点

源码位于 `src/`:

| 文件 | 职责 |
|---|---|
| `pdf.c` / `pdf.h` | PDF 解析与重写:经典 xref、xref 流(PNG predictor)、对象流 `ObjStm`、页面树、`/Rotate` 继承、写回、水印注入 |
| `pdfwatermark.c` | GUI (Win32 + GDI) 与 CLI 入口、参数解析、预览绘制 |
| `mupdf_render.c` / `.h` | 用 MuPDF 把页面渲染成 `HBITMAP` 供预览 |
| `mupdf_wm.c` / `.h` | 用 MuPDF 生成**嵌入字体子集**的水印覆盖层,再并入目标文档 |
| `ccitt.c` / `ccitt.h` | CCITT G3/G4 传真解码(自研渲染路径使用) |

#### 水印是怎么写进去的

1. 用 MuPDF 建立字体并做**子集化**(只嵌入水印用到的字形,~130 KB)
2. **自己书写内容流**,使用标准 PDF 文本矩阵 `Tm = [cos, sin, -sin, cos, e, f]`
   (行列式 = +1,保证字形不被镜像)
3. 用一对 `q` / `Q` 把**原页面内容包起来**,避免原内容残留的 `cm`
   污染水印(例如某些生成器会留下未配对的 y 翻转)
4. 把字体资源并入页面 `/Resources`
5. 重新保存时,识别并剥离上次写入的水印内容流(靠 `/PWWM` 标记),实现幂等

#### 关于 `/Rotate`

页面 `MediaBox` 可能是竖版而 `/Rotate 90` 显示为横版。水印按**显示方向**布局,
并在内容流开头插入旋转矩阵映射回页面坐标:

| /Rotate | 矩阵 |
|---|---|
| 90 | `0 1 -1 0 pw 0` |
| 180 | `-1 0 0 -1 pw ph` |
| 270 | `0 -1 1 0 0 ph` |

### 依赖说明

以下内容**未包含**在本仓库中(体积过大 / 属第三方):

- **MuPDF** —— 渲染引擎,AGPL-3.0。
  <https://github.com/ArtifexSoftware/mupdf>
- **SumatraPDF** —— 本项目使用其 `ext/` 下 vendored 的第三方库
  amalgamation(freetype / harfbuzz / jbig2dec / lcms2 / openjpeg /
  libjpeg-turbo / zlib / brotli)。
  <https://github.com/sumatrapdfreader/sumatrapdf>
- **字体** —— MuPDF 的 `resources/fonts`。

> ⚠️ **许可证提醒**:MuPDF 采用 **AGPL-3.0**。链接它会使整个程序受 AGPL 约束
> (向使用者提供源码)。若需闭源分发,请考虑 MuPDF 的商业授权,
> 或改用其它许可宽松的渲染后端。

### 已知限制

- 仅支持 **Windows**(Win32 API + GDI 预览)
- 预览只渲染**第一页**;保存时对所有页面生效
- 内置字体不随仓库分发,需自行放置(见上文)

### 致谢

- 布局与参数设计参考
  [bastienlc/pdf-watermark](https://github.com/bastienlc/pdf-watermark)
- 渲染与字体嵌入使用 [MuPDF](https://mupdf.com/)(Artifex Software)
- 第三方库打包形式取自
  [SumatraPDF](https://github.com/sumatrapdfreader/sumatrapdf)

### 许可证

本仓库自有代码(见 `src/`, `tools/`)以 MIT 许可发布,见 `LICENSE`。
**但链接 MuPDF 后,整体分发须遵守 AGPL-3.0**,详见上文。
