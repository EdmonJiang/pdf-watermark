/*
 * jxl_stub.c - fz_load_jxl* 的空实现
 *
 * MuPDF 的 source/fitz/image.c 无条件引用 JPEG-XL 加载函数, 但本工具
 * 只需要 PDF 预览, 不需要 JXL 支持. 这里提供空实现以便链接通过.
 */
#include "mupdf/fitz.h"

fz_pixmap *fz_load_jxl(fz_context *ctx, const unsigned char *p, size_t n,
                       fz_colorspace *cs)
{
    (void)p; (void)n; (void)cs;
    fz_throw(ctx, FZ_ERROR_FORMAT, "JPEG-XL not supported in this build");
}

void fz_load_jxl_info(fz_context *ctx, const unsigned char *p, size_t n,
                      int *w, int *h, int *xres, int *yres,
                      fz_colorspace **cs)
{
    (void)p; (void)n; (void)w; (void)h; (void)xres; (void)yres; (void)cs;
    fz_throw(ctx, FZ_ERROR_FORMAT, "JPEG-XL not supported in this build");
}

fz_image *fz_new_jxl_image(fz_context *ctx, fz_buffer *buf,
                           fz_colorspace *cs)
{
    (void)buf; (void)cs;
    fz_throw(ctx, FZ_ERROR_FORMAT, "JPEG-XL not supported in this build");
}
