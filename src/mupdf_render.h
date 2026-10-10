/*
 * mupdf_render.h - MuPDF 页面渲染接口 (供 GUI 预览使用)
 */
#ifndef MUPDF_RENDER_H
#define MUPDF_RENDER_H

#include <windows.h>
#include <stddef.h>

/* 初始化 MuPDF; fontDir 为内置字体目录 (resources/fonts), 可为 NULL */
int  pwMuInit(const char *fontDir);
void pwMuShutdown(void);

/*
 * 渲染 PDF 的指定页面为 32 位 DIB (调用方负责 DeleteObject).
 * 返回 0 成功, 非 0 失败 (err 中给出原因).
 */
int  pwMuRenderPage(const wchar_t *path, int pageIndex, HBITMAP *bmp,
                    int *outW, int *outH, int maxDim,
                    wchar_t *err, size_t errMax);

#endif
