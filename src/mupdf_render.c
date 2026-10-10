/*
 * mupdf_render.c - 用 MuPDF 渲染 PDF 页面为 HBITMAP (供 GUI 预览)
 *
 * 相比手写的迷你渲染器, MuPDF 完整支持:
 *   - Type0/CID 字体 + /ToUnicode CMap (文字不再乱码)
 *   - 裁剪路径 (W n)、软掩码、透明度组
 *   - DCT/JPX/JBIG2/CCITT 图像, ICC 色彩管理
 *   - 各种滤镜链与加密文档
 */
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "mupdf/fitz.h"
#include "mupdf/pdf.h"

#include "mupdf_render.h"

static fz_context *g_ctx = NULL;
static char g_fontDir[MAX_PATH] = "";

/* ---------- 内置字体: 运行时从 resources/fonts 加载 ---------- */
static unsigned char *g_fontCache[128];
static int g_fontCacheN = 0;

static const char *FORGES[] = { "urw", "droid", "noto", "sil", "han", NULL };

static const unsigned char *loadBuiltinFont(const char *name, int *size)
{
    char path[MAX_PATH * 2];
    int i;
    FILE *f;
    long n;
    unsigned char *b;

    *size = 0;
    if (!name || !g_fontDir[0]) return NULL;
    for (i = 0; FORGES[i]; i++) {
        snprintf(path, sizeof(path), "%s/%s/%s", g_fontDir, FORGES[i], name);
        f = fopen(path, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n <= 0) { fclose(f); continue; }
        b = (unsigned char *)malloc((size_t)n);
        if (!b) { fclose(f); continue; }
        if (fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); continue; }
        fclose(f);
        *size = (int)n;
        if (g_fontCacheN < (int)(sizeof(g_fontCache)/sizeof(g_fontCache[0])))
            g_fontCache[g_fontCacheN++] = b;
        return b;
    }
    return NULL;
}

/* noto_sumatra.c 提供 */
extern void fz_set_builtin_font_loader(
    const unsigned char *(*loader)(const char *file_name, int *size));

/* ---------- 初始化 ---------- */
int pwMuInit(const char *fontDir)
{
    fz_set_builtin_font_loader(loadBuiltinFont);
    if (fontDir) {
        strncpy(g_fontDir, fontDir, sizeof(g_fontDir) - 1);
        g_fontDir[sizeof(g_fontDir) - 1] = 0;
    }
    if (!g_ctx) {
        g_ctx = fz_new_context(NULL, NULL, FZ_STORE_DEFAULT);
        if (!g_ctx) return 0;
        fz_register_document_handlers(g_ctx);
    }
    return g_ctx != NULL;
}

void pwMuShutdown(void)
{
    if (g_ctx) { fz_drop_context(g_ctx); g_ctx = NULL; }
}

/*
 * 渲染指定页面为 32 位 DIB (调用方 DeleteObject).
 * 返回 0 成功.
 */
int pwMuRenderPage(const wchar_t *path, int pageIndex, HBITMAP *bmp,
                   int *outW, int *outH, int maxDim, wchar_t *err, size_t errMax)
{
    fz_document *doc = NULL;
    fz_page *page = NULL;
    fz_pixmap *pix = NULL;
    fz_matrix ctm;
    fz_rect bounds;
    float zoom;
    int W, H;
    BITMAPINFO bi;
    void *bits = NULL;
    HBITMAP hb = NULL;
    char cpath[MAX_PATH * 2];
    int ok = 0;

    if (err && errMax) err[0] = 0;
    if (!g_ctx && !pwMuInit(NULL)) {
        if (err) wcsncpy(err, L"MuPDF 初始化失败", errMax - 1);
        return -1;
    }

    /* 宽字符路径 -> UTF-8 */
    if (WideCharToMultiByte(CP_UTF8, 0, path, -1, cpath, sizeof(cpath),
                            NULL, NULL) == 0) {
        if (err) wcsncpy(err, L"路径转换失败", errMax - 1);
        return -1;
    }

    fz_try(g_ctx) {
        doc = fz_open_document(g_ctx, cpath);
        if (pageIndex < 0 || pageIndex >= fz_count_pages(g_ctx, doc))
            fz_throw(g_ctx, FZ_ERROR_ARGUMENT, "page out of range");
        page = fz_load_page(g_ctx, doc, pageIndex);
        bounds = fz_bound_page(g_ctx, page);
        {
            float pw = bounds.x1 - bounds.x0;
            float ph = bounds.y1 - bounds.y0;
            if (pw < 1) pw = 1;
            if (ph < 1) ph = 1;
            zoom = (float)maxDim / (pw > ph ? pw : ph);
            W = (int)(pw * zoom + 0.5f);
            H = (int)(ph * zoom + 0.5f);
            if (W < 1) W = 1;
            if (H < 1) H = 1;
            ctm = fz_scale(zoom, zoom);
            pix = fz_new_pixmap_from_page(g_ctx, page, ctm,
                                          fz_device_rgb(g_ctx), 0);
        }
    }
    fz_catch(g_ctx) {
        if (err && errMax) {
            wchar_t m[256];
            MultiByteToWideChar(CP_UTF8, 0, fz_caught_message(g_ctx), -1, m, 256);
            wcsncpy(err, m, errMax - 1);
            err[errMax-1] = 0;
        }
        if (page) fz_drop_page(g_ctx, page);
        if (doc) fz_drop_document(g_ctx, doc);
        return -1;
    }

    /* 转成 32 位 top-down DIB */
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    hb = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (hb && bits) {
        int y;
        int n = pix->n;                 /* 每像素分量数 */
        for (y = 0; y < H; y++) {
            const unsigned char *s = pix->samples + (size_t)y * pix->stride;
            unsigned char *d = (unsigned char *)bits + (size_t)y * W * 4;
            int x;
            for (x = 0; x < W; x++) {
                unsigned char r, g, b;
                if (n >= 3) {
                    r = s[x*n + 0]; g = s[x*n + 1]; b = s[x*n + 2];
                } else {
                    r = g = b = s[x*n];
                }
                d[x*4+0] = b;
                d[x*4+1] = g;
                d[x*4+2] = r;
                d[x*4+3] = 255;
            }
        }
        ok = 1;
    }

    fz_drop_pixmap(g_ctx, pix);
    fz_drop_page(g_ctx, page);
    fz_drop_document(g_ctx, doc);

    if (!ok) { if (hb) DeleteObject(hb); return -1; }
    *bmp = hb;
    *outW = W;
    *outH = H;
    return 0;
}
