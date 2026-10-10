/*
 * mupdf_wm.h - 用 MuPDF 生成嵌入字体子集的水印覆盖层
 */
#ifndef MUPDF_WM_H
#define MUPDF_WM_H

typedef struct {
    unsigned char *data;   /* PDF 字节 (malloc) */
    size_t len;
} pwWatermark;

void pwWmFree(pwWatermark *wm);

int pwWmBuild(const char *text, double pw, double ph,
              const double *mx, const double *my, int nMarks,
              double fontSize, double angleDeg,
              double r, double g, double b, double opacity,
              const char *fontFile, pwWatermark *out);

#endif
