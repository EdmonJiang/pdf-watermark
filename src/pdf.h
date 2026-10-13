/*
 * pdf.h - 迷你 PDF 解析 / 重写引擎 (纯 C, 无第三方库)
 *
 * 支持: 经典 xref 表、PDF1.5 交叉引用流(xref stream + Flate + PNG predictor)、
 *       对象流 ObjStm、页面树遍历、增量式全量重写保存。
 * 水印通过给每个页面追加一个内容流(文本 + ExtGState 透明度)实现,
 * 原有页面内容完全不动, 因此文字/矢量图保真。
 */
#ifndef PDF_H
#define PDF_H

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>
#include <windows.h>

typedef struct PdfDoc PdfDoc;

#define WM_MODE_GRID   0
#define WM_MODE_SINGLE 1

typedef struct {
    const wchar_t *text;      /* 水印文字 (UTF-16, 内部转 GBK/Helvetica) */
    double size;               /* 字号 (pt) */
    double angle;              /* 旋转角度 (度, 逆时针) */
    double opacity;            /* 0..1 */
    double r, g, b;            /* 颜色 0..1 */
    int mode;                  /* WM_MODE_GRID / WM_MODE_SINGLE */
    int hboxes, vboxes;        /* 网格: 水平/垂直重复数 */
    double ix, iy;             /* 单点模式: 相对位置 0..1 */
} WmParams;

/* 解析: 成功返回 PdfDoc*, 失败返回 NULL 并在 err 中给出原因 */
PdfDoc *pdfLoad(const uint8_t *data, size_t len, wchar_t *err, size_t errMax);
void    pdfFree(PdfDoc *d);

int     pdfPageCount(PdfDoc *d);
void    pdfGetPage(PdfDoc *d, int idx, double *w, double *h);

/* 旋转所有页面 (顺时针 delta 度, 0/90/180/270); 幂等, 传 0 恢复原方向 */
void    pdfRotateAll(PdfDoc *d, int delta);

/* 给所有页面追加水印(修改内部对象), 返回 0 成功。
 * 重复调用不会叠加: 本工具写入的旧水印内容流会被自动剥离。 */
int     pdfApplyWatermark(PdfDoc *d, const WmParams *p, wchar_t *err, size_t errMax);

/* 渲染指定页面为 32 位 DIB (调用方 DeleteObject), maxDim 为最长边像素上限 */
int     pdfRenderPage(PdfDoc *d, int pageIndex, HBITMAP *bmp, int *w, int *h,
                      int maxDim);

/* 统计文档中本工具写入的水印内容流对象数 (0 = 无水印) */
int     pdfCountWatermarks(PdfDoc *d);

/* 计算水印位置(页面坐标 pt, 原点左下).
 * x/y 为调用方缓冲区; max 为容量. 返回实际写入的位置数.
 * GUI 预览用此函数保证与写入 PDF 的布局完全一致. */
int     pdfWatermarkMarks(const WmParams *p, double pw, double ph,
                          double *x, double *y, int max);

/* 重写并保存到文件, 返回 0 成功 */
int     pdfWrite(PdfDoc *d, const wchar_t *path, wchar_t *err, size_t errMax);

#endif
