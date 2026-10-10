/*
 * mupdf_wm.c - 用 MuPDF 生成"水印覆盖层" PDF
 *
 * 目的: 导出的 PDF 使用**嵌入字体子集**(系统黑体), 因此在任何阅读器里
 * 都与预览完全一致 —— 不再依赖阅读器对 STSong-Light 等"非嵌入标准 CJK
 * 字体"的替换 (不同阅读器会替换成宋体/楷体/其它, 导致字体不一致).
 *
 * 用法: pwWmBuild() 生成一个单页 PDF (页面尺寸与目标页相同, 只含水印),
 * 调用方再把它第 0 页的内容流与资源并入目标文档.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mupdf/fitz.h"
#include "mupdf/pdf.h"

#include "mupdf_wm.h"

void pwWmFree(pwWatermark *wm)
{
    if (!wm) return;
    free(wm->data);
    wm->data = NULL;
    wm->len = 0;
}

/* 打开一个可用的 CJK 黑体 */
static fz_font *loadHeiFont(fz_context *ctx, const char *preferred)
{
    const char *cands[6];
    int i, n = 0;
    fz_font *font = NULL;

    if (preferred && preferred[0]) cands[n++] = preferred;
    cands[n++] = "C:/Windows/Fonts/simhei.ttf";   /* 黑体 */
    cands[n++] = "C:/Windows/Fonts/msyh.ttc";     /* 微软雅黑 */
    cands[n++] = "C:/Windows/Fonts/Deng.ttf";     /* 等线 */
    cands[n++] = NULL;

    for (i = 0; cands[i] && !font; i++) {
        fz_buffer *fb = NULL;
        fz_try(ctx) { fb = fz_read_file(ctx, cands[i]); }
        fz_catch(ctx) { fb = NULL; }
        if (!fb) continue;
        fz_try(ctx) {
            font = fz_new_font_from_buffer(ctx, "wmhei", fb, 0, 0);
        }
        fz_catch(ctx) { font = NULL; }
        fz_drop_buffer(ctx, fb);
    }
    return font;
}

/*
 * 生成水印覆盖层 PDF.
 *   text      : UTF-8 水印文字
 *   pw, ph    : 页面尺寸 (显示尺寸)
 *   mx, my    : 每个水印"起点"的页面坐标 (PDF 用户坐标, 原点左下),
 *               已按 angleDeg 做过半宽偏移 (与文本矩阵约定一致)
 *   fontSize  : 字号 (pt)
 *   angleDeg  : 逆时针角度 (度)
 *   color     : 0..1 的 RGB
 *   opacity   : 0..1
 * 成功返回 0, out->data/len 为 PDF 字节 (调用方 pwWmFree 释放).
 */
int pwWmBuild(const char *text, double pw, double ph,
              const double *mx, const double *my, int nMarks,
              double fontSize, double angleDeg,
              double r, double g, double b, double opacity,
              const char *fontFile, pwWatermark *out)
{
    fz_context *ctx;
    fz_font *font = NULL;
    fz_buffer *buf = NULL;
    fz_output *outp = NULL;
    unsigned char *bytes = NULL;
    int ok = -1;

    if (out) { out->data = NULL; out->len = 0; }
    if (!out || !text || !*text || nMarks <= 0) return -1;
    if (pw < 1) pw = 1;
    if (ph < 1) ph = 1;

    ctx = fz_new_context(NULL, NULL, FZ_STORE_DEFAULT);
    if (!ctx) return -1;
    fz_register_document_handlers(ctx);

    fz_try(ctx) {
        pdf_document *doc = pdf_create_document(ctx);
        fz_rect mediabox = fz_make_rect(0, 0, (float)pw, (float)ph);
        pdf_obj *res = NULL;
        fz_buffer *cont = NULL;
        fz_device *dev;
        fz_colorspace *cs;
        float color[3];
        fz_color_params cp;
        int i;

        dev = pdf_page_write(ctx, doc, mediabox, &res, &cont);
        font = loadHeiFont(ctx, fontFile);
        if (!font)
            fz_throw(ctx, FZ_ERROR_FORMAT, "no CJK font found");

        cs = fz_device_rgb(ctx);
        cp = fz_default_color_params;
        color[0] = (float)r;
        color[1] = (float)g;
        color[2] = (float)b;

        /*
         * 关键: 用一个"占位"文本让 MuPDF 建立并嵌入字体的子集.
         * 真正的内容流随后由我们自己拼写 (见下), 因为 pdf_page_write 的设备
         * 末尾会强制乘一个 y 翻转, 使得经由它输出的文本矩阵必然是负行列式,
         * 从而字形被上下镜像. 自己写 Tm 才能保证方向正确.
         */
        {
            fz_text *tx = fz_new_text(ctx);
            fz_matrix trm = fz_scale((float)fontSize, (float)fontSize);
            fz_show_string(ctx, tx, font, trm, text, 0, 0,
                           FZ_BIDI_LTR, FZ_LANG_UNSET);
            fz_fill_text(ctx, dev, tx, fz_identity, cs, color, 1.0f, cp);
            fz_drop_text(ctx, tx);
        }
        (void)nMarks;

        fz_close_device(ctx, dev);
        fz_drop_device(ctx, dev);

        /*
         * 关键: 内容流完全由我们自写.
         *
         * pdf_page_write 返回的 device 会在内容里写一个 y 翻转, 使得经它输出
         * 的文本矩阵必然是负行列式 -> 字形上下镜像. 因此这里只用 MuPDF 把字体
         * 建立/嵌入好 (上面的占位文本), 真正的内容流换成标准 PDF 写法:
         *   Tm = [cos, sin, -sin, cos, e, f]     (行列式 = 1, 不镜像)
         * 不加任何翻转 cm, 与手写 PDF 完全一致.
         */
        {
            fz_buffer *nb = fz_new_buffer(ctx, 1024);
            fz_append_printf(ctx, nb, "q\n");
            fz_append_printf(ctx, nb, "%.4f %.4f %.4f rg\n", r, g, b);
            for (i = 0; i < nMarks; i++) {
                double rad = (double)angleDeg * M_PI / 180.0;
                double cc = cos(rad), ss = sin(rad);
                fz_append_printf(ctx, nb,
                    "BT\n/F0 %.4f Tf\n"
                    "%.6f %.6f %.6f %.6f %.6f %.6f Tm\n",
                    fontSize, cc, ss, -ss, cc, mx[i], my[i]);
                fz_append_string(ctx, nb, "<");
                {
                    const unsigned char *u = (const unsigned char *)text;
                    while (*u) {
                        int cp = 0, len = 1, okc = 1, k;
                        if (u[0] < 0x80) { cp = u[0]; len = 1; }
                        else if ((u[0] & 0xE0) == 0xC0) { cp = u[0] & 0x1F; len = 2; }
                        else if ((u[0] & 0xF0) == 0xE0) { cp = u[0] & 0x0F; len = 3; }
                        else if ((u[0] & 0xF8) == 0xF0) { cp = u[0] & 0x07; len = 4; }
                        else { u++; continue; }
                        for (k = 1; k < len; k++) {
                            if ((u[k] & 0xC0) != 0x80) { okc = 0; break; }
                            cp = (cp << 6) | (u[k] & 0x3F);
                        }
                        if (!okc) { u++; continue; }
                        {
                            int gid = 0;
                            fz_try(ctx) { gid = fz_encode_character(ctx, font, cp); }
                            fz_catch(ctx) { gid = 0; }
                            fz_append_printf(ctx, nb, "%04X", gid & 0xFFFF);
                        }
                        u += len;
                    }
                }
                fz_append_string(ctx, nb, "> Tj\nET\n");
            }
            fz_append_printf(ctx, nb, "Q\n");
            fz_drop_buffer(ctx, cont);
            cont = nb;
        }

        pdf_insert_page(ctx, doc, -1,
                        pdf_add_page(ctx, doc, mediabox, 0, res, cont));
        fz_drop_buffer(ctx, cont);

        /* 只保留水印用到的字形, 否则会嵌入整个 CJK 字体 (约 10MB) */
        {
            int pageNo = 0;
            pdf_subset_fonts(ctx, doc, 1, &pageNo);
        }

        buf = fz_new_buffer(ctx, 8192);
        outp = fz_new_output_with_buffer(ctx, buf);
        pdf_write_document(ctx, doc, outp, NULL);
        fz_close_output(ctx, outp);
        fz_drop_output(ctx, outp);
        outp = NULL;

        bytes = (unsigned char *)malloc(buf->len ? buf->len : 1);
        if (!bytes)
            fz_throw(ctx, FZ_ERROR_SYSTEM, "oom");
        memcpy(bytes, buf->data, buf->len);
        out->data = bytes;
        out->len = buf->len;
        bytes = NULL;
        ok = 0;
    }
    fz_catch(ctx) {
        ok = -1;
    }

    if (buf) fz_drop_buffer(ctx, buf);
    if (outp) fz_drop_output(ctx, outp);
    if (font) fz_drop_font(ctx, font);
    free(bytes);
    fz_drop_context(ctx);
    return ok;
}
