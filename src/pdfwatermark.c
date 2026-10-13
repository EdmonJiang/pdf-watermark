/*
 * pdfwatermark.c - PDF 水印工具 (纯 C, Win32)
 *
 * 功能 (参照 pdf-watermark python 项目):
 *   - 打开 PDF, 解析页面尺寸/页数
 *   - 预览界面, 可设置: 水印文字、字号、颜色、倾斜角度、
 *     水平/垂直重复数(网格)、透明度、留边距、单点插入模式
 *   - 保存为 PDF: 给每个页面追加水印内容流, 原有内容原样保留
 *   - 也支持命令行模式 (grid / insert, 参数与 python 版类似)
 *
 * 编译: 见 build.bat
 */
#define _USE_MATH_DEFINES
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "pdf.h"
#include "mupdf_render.h"

#define PW_VERSION L"1.1.0"
#define PW_BUILD_DATE L"2026-10-13"

/* ------------------------------------------------------------------ */
/* 控件 ID                                                             */
/* ------------------------------------------------------------------ */
#define IDC_OPEN        1001
#define IDC_SAVE        1002
#define IDC_TEXT        1003
#define IDC_COLOR       1004
#define IDC_TB_FONT     1005
#define IDC_TB_ANGLE    1006
#define IDC_TB_HBOX     1007
#define IDC_TB_OPACITY  1008
#define IDC_ST_FONT     1009
#define IDC_ST_ANGLE    1010
#define IDC_ST_HBOX     1011
#define IDC_ST_OPACITY  1012
#define IDC_ST_INFO     1013
#define IDC_TB_VBOX     1014
#define IDC_ST_VBOX     1015
#define IDC_CB_SINGLE   1017
#define IDC_BTN_ROTATE  1018
#define IDC_ST_ROTATE   1019

/* ------------------------------------------------------------------ */
/* 全局状态                                                            */
/* ------------------------------------------------------------------ */
static HINSTANCE g_hInst;
static HWND      g_hWnd;

static PdfDoc *g_doc = NULL;
static double  g_pw = 612, g_ph = 792;    /* 第一页尺寸 (pt) */
static wchar_t g_srcPath[MAX_PATH] = L"";

/* 第一页渲染缓存 (预览时叠加在页面底下) */
static HBITMAP g_pageBmp = NULL;
static int     g_pageBmpW = 0, g_pageBmpH = 0;

/* 用户旋转增量 (顺时针 0/90/180/270) 与旋转后的预览底图 */
static int     g_rotate = 0;
static HBITMAP g_viewBmp = NULL;   /* 旋转后的页面底图 (g_viewOwned 时才需释放) */
static int     g_viewBmpW = 0, g_viewBmpH = 0;
static int     g_viewOwned = 0;

static void releasePageBmp(void)
{
    if (g_viewOwned && g_viewBmp) DeleteObject(g_viewBmp);
    g_viewBmp = NULL; g_viewBmpW = g_viewBmpH = 0; g_viewOwned = 0;
    if (g_pageBmp) { DeleteObject(g_pageBmp); g_pageBmp = NULL; }
    g_pageBmpW = g_pageBmpH = 0;
}

/* 把 32bpp top-down DIB 顺时针旋转 turns*90°, 返回新 DIB (turns=0 返回 NULL) */
static HBITMAP rotateBmp90(HBITMAP src, int sw, int sh, int turns,
                           int *ow, int *oh)
{
    DIBSECTION ds;
    BITMAPINFO bi;
    void *bits = NULL;
    HBITMAP dst;
    uint32_t *s, *d;
    int nw, nh, x, y;

    if (turns == 0) { *ow = sw; *oh = sh; return NULL; }
    if (!GetObject(src, sizeof(ds), &ds) || !ds.dsBm.bmBits ||
        ds.dsBm.bmBitsPixel != 32)
        return NULL;
    nw = (turns == 1 || turns == 3) ? sh : sw;
    nh = (turns == 1 || turns == 3) ? sw : sh;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = nw;
    bi.bmiHeader.biHeight = -nh;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    dst = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!dst || !bits) { if (dst) DeleteObject(dst); return NULL; }
    s = (uint32_t *)ds.dsBm.bmBits;
    d = (uint32_t *)bits;
    for (y = 0; y < sh; y++) {
        for (x = 0; x < sw; x++) {
            uint32_t p = s[(size_t)y * sw + x];
            int nx, ny;
            if (turns == 1)      { nx = sh - 1 - y; ny = x; }
            else if (turns == 2) { nx = sw - 1 - x; ny = sh - 1 - y; }
            else                 { nx = y; ny = sw - 1 - x; }
            d[(size_t)ny * nw + nx] = p;
        }
    }
    *ow = nw; *oh = nh;
    return dst;
}

/* 依据 g_rotate 生成预览底图 (g_rotate==0 时直接借用原图) */
static void rebuildView(void)
{
    if (g_viewOwned && g_viewBmp) { DeleteObject(g_viewBmp); g_viewOwned = 0; }
    g_viewBmp = NULL; g_viewBmpW = g_viewBmpH = 0;
    if (!g_pageBmp) return;
    if (g_rotate == 0) {
        g_viewBmp = g_pageBmp;
        g_viewBmpW = g_pageBmpW;
        g_viewBmpH = g_pageBmpH;
    } else {
        int w = 0, h = 0;
        HBITMAP r = rotateBmp90(g_pageBmp, g_pageBmpW, g_pageBmpH,
                                (g_rotate / 90) % 4, &w, &h);
        if (r) { g_viewBmp = r; g_viewBmpW = w; g_viewBmpH = h; g_viewOwned = 1; }
    }
}

static void loadPageBmp(void)
{
    releasePageBmp();
    if (!g_doc || !g_srcPath[0]) return;
    /* 用 MuPDF 渲染原始文件的第一页作为预览底图 */
    if (pwMuRenderPage(g_srcPath, 0, &g_pageBmp, &g_pageBmpW, &g_pageBmpH,
                       1400, NULL, 0) != 0)
        g_pageBmp = NULL;
    rebuildView();
}

/* 当前显示尺寸 (pt): 原始显示尺寸叠加 g_rotate (90/270 时宽高互换) */
static void effPageSize(double *w, double *h)
{
    if (g_rotate == 90 || g_rotate == 270) { *w = g_ph; *h = g_pw; }
    else { *w = g_pw; *h = g_ph; }
}

/* 水印参数 (默认值与 python 版 pdf-watermark 一致) */
static wchar_t g_text[256]  = L"仅用于";
static double  g_fontSize   = 50;     /* px (96dpi 像素), 内部换算为 pt */
static COLORREF g_color     = RGB(255, 0, 0);
static double  g_angle      = 45;     /* -90..90 度 (逆时针) */
static int     g_hboxes     = 3;
static int     g_vboxes     = 2;
static int     g_opacity    = 20;     /* 0..100 % */
#define FONT_MIN_PX 6
#define FONT_DEF_PX 50
#define FONT_MAX_PX 200
#define PX2PT(v)    ((v) * 72.0 / 96.0)   /* 像素 -> 点 (96dpi) */
static int     g_single     = 0;      /* 仅插入单个水印 */
static int     g_dpi        = 96;
#define S(v) MulDiv((v), g_dpi, 96)    /* 逻辑像素 -> 物理像素 */

static void invalidatePreview(void);

static void currentParams(WmParams *p)
{
    p->text = g_text;
    p->size = PX2PT(g_fontSize);
    p->angle = g_angle;
    p->opacity = g_opacity / 100.0;
    p->r = GetRValue(g_color) / 255.0;
    p->g = GetGValue(g_color) / 255.0;
    p->b = GetBValue(g_color) / 255.0;
    p->mode = g_single ? WM_MODE_SINGLE : WM_MODE_GRID;
    p->hboxes = g_hboxes;
    p->vboxes = g_vboxes;
    p->ix = 0.5; p->iy = 0.5;
}

/* ------------------------------------------------------------------ */
/* 打开 / 保存                                                         */
/* ------------------------------------------------------------------ */
static int openPdf(const wchar_t *path)
{
    HANDLE fh = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD size;
    uint8_t *buf;
    DWORD got;
    wchar_t err[128] = L"";
    PdfDoc *d;

    if (fh == INVALID_HANDLE_VALUE) return -1;
    size = GetFileSize(fh, NULL);
    if (size == INVALID_FILE_SIZE || size < 64 || size > 800u * 1024 * 1024) {
        CloseHandle(fh);
        return -2;
    }
    buf = (uint8_t *)malloc(size);
    if (!ReadFile(fh, buf, size, &got, NULL) || got != size) {
        CloseHandle(fh);
        free(buf);
        return -3;
    }
    CloseHandle(fh);

    d = pdfLoad(buf, size, err, 128);
    free(buf);
    if (!d) {
        MessageBoxW(g_hWnd, err[0] ? err : L"无法解析该 PDF 文件。",
                    L"错误", MB_OK | MB_ICONERROR);
        return -4;
    }
    if (g_doc) pdfFree(g_doc);
    g_doc = d;
    wcscpy(g_srcPath, path);
    pdfGetPage(g_doc, 0, &g_pw, &g_ph);
    loadPageBmp();
    return 0;
}

static void updateInfo(void)
{
    wchar_t buf[160];
    if (!g_doc) buf[0] = 0;
    else wsprintfW(buf, L"共 %d 页, 第一页 %d x %d pt  |  调节参数即时预览, 保存时写入全部页面",
                   pdfPageCount(g_doc), (int)(g_pw + 0.5), (int)(g_ph + 0.5));
    SetDlgItemTextW(g_hWnd, IDC_ST_INFO, buf);
}

static void doOpen(void)
{
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW of;
    memset(&of, 0, sizeof(of));
    of.lStructSize = sizeof(of);
    of.hwndOwner = g_hWnd;
    of.lpstrFilter = L"PDF 文件 (*.pdf)\0*.pdf\0所有文件 (*.*)\0*.*\0";
    of.lpstrFile = file;
    of.nMaxFile = MAX_PATH;
    of.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&of)) return;
    if (openPdf(file) != 0) return;
    updateInfo();
    InvalidateRect(g_hWnd, NULL, TRUE);
}

static void doSave(void)
{
    wchar_t file[MAX_PATH];
    wchar_t base[MAX_PATH];
    wchar_t *e;
    wchar_t err[128] = L"";
    WmParams p;
    OPENFILENAMEW sf;

    if (!g_doc) {
        MessageBoxW(g_hWnd, L"请先打开 PDF 文件。", L"提示", MB_OK | MB_ICONINFORMATION);
        return;
    }
    wcscpy(base, g_srcPath);
    e = wcsrchr(base, L'.');
    if (e) *e = 0;
    wsprintfW(file, L"%s_水印.pdf", base);

    memset(&sf, 0, sizeof(sf));
    sf.lStructSize = sizeof(sf);
    sf.hwndOwner = g_hWnd;
    sf.lpstrFilter = L"PDF 文件 (*.pdf)\0*.pdf\0所有文件 (*.*)\0*.*\0";
    sf.lpstrFile = file;
    sf.nMaxFile = MAX_PATH;
    sf.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    sf.nFilterIndex = 1;
    if (!GetSaveFileNameW(&sf)) return;

    currentParams(&p);
    pdfRotateAll(g_doc, g_rotate);          /* 先按当前旋转设置每页 /Rotate */
    if (pdfApplyWatermark(g_doc, &p, err, 128) != 0) {
        MessageBoxW(g_hWnd, err[0] ? err : L"生成水印失败。",
                    L"错误", MB_OK | MB_ICONERROR);
        return;
    }
    if (pdfWrite(g_doc, file, err, 128) != 0) {
        MessageBoxW(g_hWnd, err[0] ? err : L"保存失败。",
                    L"错误", MB_OK | MB_ICONERROR);
        return;
    }
    MessageBoxW(g_hWnd, L"保存成功！", L"提示", MB_OK | MB_ICONINFORMATION);
}

static void doChooseColor(void)
{
    CHOOSECOLORW cc;
    static COLORREF acrCmt[16] = {0};
    memset(&cc, 0, sizeof(cc));
    cc.lStructSize = sizeof(cc);
    cc.hwndOwner = g_hWnd;
    cc.lpCustColors = acrCmt;
    cc.Flags = CC_RGBINIT | CC_FULLOPEN;
    cc.rgbResult = g_color;
    if (ChooseColorW(&cc)) {
        g_color = cc.rgbResult;
        invalidatePreview();
    }
}

/* ------------------------------------------------------------------ */
/* 预览                                                                */
/* ------------------------------------------------------------------ */

/* 预览区边距 (逻辑像素, 经 S() 缩放) */
#define PV_MARGIN 12
#define PV_TOPY   S(124)     /* 参数栏高度以下 */

/* 计算预览区可用矩形 (客户区坐标) */
static void calcPreviewArea(RECT *a)
{
    RECT rc; GetClientRect(g_hWnd, &rc);
    a->left   = S(PV_MARGIN);
    a->top    = PV_TOPY;
    a->right  = rc.right - S(PV_MARGIN);
    a->bottom = rc.bottom - S(PV_MARGIN);
    /* 保证左上角不超出客户区 */
    if (a->left < 0) a->left = 0;
    if (a->right < a->left) a->right = a->left;
    if (a->bottom < a->top) a->bottom = a->top;
}

static void calcPreview(int *dx, int *dy, int *dw, int *dh)
{
    RECT area;
    int areaW, areaH;
    double scale, ew, eh;
    calcPreviewArea(&area);
    areaW = area.right - area.left;
    areaH = area.bottom - area.top;
    *dw = 0; *dh = 0;
    effPageSize(&ew, &eh);
    if (ew > 0 && eh > 0 && areaW > 0 && areaH > 0) {
        scale = (double)areaW / ew;
        if ((double)areaH / eh < scale) scale = (double)areaH / eh;
        *dw = (int)(ew * scale); *dh = (int)(eh * scale);
        if (*dw < 1) *dw = 1;
        if (*dh < 1) *dh = 1;
        *dx = area.left + (areaW - *dw) / 2;
        *dy = area.top  + (areaH - *dh) / 2;
    } else {
        *dx = area.left; *dy = area.top; *dw = areaW; *dh = areaH;
    }
}

static void invalidatePreview(void)
{
    int dx, dy, dw, dh;
    calcPreview(&dx, &dy, &dw, &dh);
    {
        RECT r = { dx - 4, dy - 4, dx + dw + 4, dy + dh + 4 };
        InvalidateRect(g_hWnd, &r, FALSE);
    }
}

/* 在预览画布上绘制水印文字 (与写入 PDF 的布局公式一致) */
static void drawPreviewWatermark(HDC dc, int px, int py, int pw, int ph)
{
    double ew, eh, scale;
    double ang = g_angle * (M_PI / 180.0);
    double cs = cos(ang), sn = sin(ang);
    effPageSize(&ew, &eh);
    scale = (ew > 0) ? (double)pw / ew : 1.0;
    /* 与写入 PDF 保持一致的字体大小: g_fontSize(px) -> pt -> 预览像素 */
    int fontPx = (int)(PX2PT(g_fontSize) * scale + 0.5);
    HFONT font, oldFont;
    WmParams pp;
    double mx[256], my[256];
    int nm, k2;
    XFORM xf;

    if (!g_text[0]) return;
    {
        wchar_t *t = g_text;
        while (*t == L' ') t++;
        if (!*t) return;
    }
    if (fontPx < 4) fontPx = 4;

    /*
     * 字体与写入 PDF 的一致: 统一用黑体 (SimHei).
     * 写入 PDF 时由 MuPDF 嵌入系统黑体的字形子集, 所以两边完全一致.
     */
    {
        /* 统一用黑体: SimHei 同时包含拉丁字母与汉字字形 */
        font = CreateFontW(-fontPx, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                           L"SimHei");
    }
    oldFont = (HFONT)SelectObject(dc, font);

    SetGraphicsMode(dc, GM_ADVANCED);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));   /* 只画白色遮罩, 颜色/透明度在合成时施加 */
    SetTextAlign(dc, TA_CENTER | TA_BASELINE);

    /* 屏幕 y 向下; PDF 逆时针 -> 屏幕顺时针, 取 -sin (与 PDF 视觉一致) */
    xf.eM11 = (FLOAT)cs;
    xf.eM12 = (FLOAT)-sn;
    xf.eM21 = (FLOAT)sn;
    xf.eM22 = (FLOAT)cs;

    /* 与写入 PDF 使用完全相同的布局公式 */
    currentParams(&pp);
    nm = pdfWatermarkMarks(&pp, ew, eh, mx, my, 256);
    for (k2 = 0; k2 < nm; k2++) {
        /* 页面坐标(原点左下) -> 屏幕坐标(原点左上) */
        xf.eDx = (FLOAT)(px + mx[k2] * scale);
        xf.eDy = (FLOAT)(py + (eh - my[k2]) * scale);
        SetWorldTransform(dc, &xf);
        TextOutW(dc, 0, 0, g_text, (int)wcslen(g_text));
    }
    {
        XFORM ident;
        ident.eM11 = 1; ident.eM12 = 0; ident.eM21 = 0; ident.eM22 = 1;
        ident.eDx = 0; ident.eDy = 0;
        SetWorldTransform(dc, &ident);
    }
    SelectObject(dc, oldFont);
    DeleteObject(font);
}

/* ------------------------------------------------------------------ */
static void setTrack(HWND hWnd, int id, int lo, int hi, int val)
{
    HWND tb = GetDlgItem(hWnd, id);
    if (!tb) return;
    SendMessage(tb, TBM_SETRANGE, TRUE, MAKELPARAM(lo, hi));
    SendMessage(tb, TBM_SETPOS, TRUE, val);
}

static void updateLabels(HWND hWnd)
{
    wchar_t buf[64];
    /* 注意: wsprintf 不支持 %f, 浮点须先取整或用 swprintf */
    swprintf(buf, 64, L"%d px", (int)(g_fontSize + 0.5));
    SetDlgItemTextW(hWnd, IDC_ST_FONT, buf);
    swprintf(buf, 64, L"%d \x5EA6", (int)(g_angle + (g_angle < 0 ? -0.5 : 0.5)));   /* 度 */
    SetDlgItemTextW(hWnd, IDC_ST_ANGLE, buf);
    wsprintfW(buf, L"%d", g_hboxes);
    SetDlgItemTextW(hWnd, IDC_ST_HBOX, buf);
    wsprintfW(buf, L"%d", g_vboxes);
    SetDlgItemTextW(hWnd, IDC_ST_VBOX, buf);
    wsprintfW(buf, L"%d %%", g_opacity);
    SetDlgItemTextW(hWnd, IDC_ST_OPACITY, buf);
    wsprintfW(buf, L"旋转: %d\xB0", g_rotate);
    SetDlgItemTextW(hWnd, IDC_ST_ROTATE, buf);
}

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        static const wchar_t *CLS = L"STATIC";
        static const wchar_t *BTN = L"BUTTON";
        static const wchar_t *EDT = L"EDIT";
        static const wchar_t *TRK = TRACKBAR_CLASSW;
        HFONT hFont = CreateFontW(S(-14), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                                  DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                                  L"Microsoft YaHei");
        #define MK(cls, txt, id, x, y, w, h, style) \
            do { HWND _c = CreateWindowExW(0, cls, txt, WS_CHILD | WS_VISIBLE | style, \
                            x, y, w, h, hWnd, (HMENU)(INT_PTR)id, g_hInst, NULL); \
                 if (_c) SendMessageW(_c, WM_SETFONT, (WPARAM)hFont, TRUE); } while (0)

        MK(BTN, L"\x6253\x5F00 PDF", IDC_OPEN, S(12), S(12), S(100), S(32), BS_PUSHBUTTON);
        MK(BTN, L"\x4FDD\x5B58 PDF", IDC_SAVE, S(122), S(12), S(100), S(32), BS_PUSHBUTTON);

        MK(CLS, L"\x6C34\x5370\x6587\x5B57:", 0, S(240), S(18), S(70), S(20), 0);
        MK(EDT, g_text, IDC_TEXT, S(312), S(14), S(240), S(28),
           WS_BORDER | ES_AUTOHSCROLL);

        MK(BTN, L"\x6587\x5B57\x989C\x8272...", IDC_COLOR, S(566), S(12), S(100), S(32), BS_PUSHBUTTON);

        MK(CLS, L"\x5B57\x53F7:", 0, S(684), S(18), S(40), S(20), 0);
        MK(TRK, NULL, IDC_TB_FONT, S(726), S(16), S(160), S(22), 0);
        MK(CLS, L"12 pt", IDC_ST_FONT, S(892), S(18), S(60), S(20), 0);

        MK(CLS, L"\x503E\x659C\x89D2\x5EA6:", 0, S(12), S(58), S(70), S(20), 0);
        MK(TRK, NULL, IDC_TB_ANGLE, S(84), S(56), S(180), S(22), 0);
        MK(CLS, L"45 \x5EA6", IDC_ST_ANGLE, S(270), S(58), S(60), S(20), 0);

        MK(CLS, L"\x6C34\x5E73\x91CD\x590D:", 0, S(344), S(58), S(70), S(20), 0);
        MK(TRK, NULL, IDC_TB_HBOX, S(416), S(56), S(140), S(22), 0);
        MK(CLS, L"3", IDC_ST_HBOX, S(562), S(58), S(30), S(20), 0);

        MK(CLS, L"\x5782\x76F4\x91CD\x590D:", 0, S(606), S(58), S(70), S(20), 0);
        MK(TRK, NULL, IDC_TB_VBOX, S(678), S(56), S(140), S(22), 0);
        MK(CLS, L"6", IDC_ST_VBOX, S(824), S(58), S(30), S(20), 0);

        MK(CLS, L"\x900F\x660E\x5EA6:", 0, S(12), S(94), S(60), S(20), 0);
        MK(TRK, NULL, IDC_TB_OPACITY, S(74), S(92), S(160), S(22), 0);
        MK(CLS, L"10 %", IDC_ST_OPACITY, S(240), S(94), S(50), S(20), 0);

        MK(BTN, L"\x4EC5\x5C45\x4E2D\x4E00\x4E2A", IDC_CB_SINGLE, S(452), S(92), S(120), S(24), BS_AUTOCHECKBOX);
        MK(BTN, L"旋转 90°", IDC_BTN_ROTATE, S(584), S(92), S(104), S(24), BS_PUSHBUTTON);
        MK(CLS, L"旋转: 0°", IDC_ST_ROTATE, S(700), S(94), S(120), S(20), 0);

        MK(CLS,
           L"\x6253\x5F00 PDF \x540E\x53EF\x5B9E\x65F6\x9884\x89C8\x6C34\x5370\x5E03\x5C40; "
           L"\x4FDD\x5B58\x65F6\x6C34\x5370\x8986\x76D6\x5728\x539F\x5185\x5BB9\x4E4B\x4E0A\x3002",
           IDC_ST_INFO, S(12), S(92 + 30), S(900), S(20), 0);

        setTrack(hWnd, IDC_TB_FONT,    FONT_MIN_PX, FONT_MAX_PX, (int)g_fontSize);
        setTrack(hWnd, IDC_TB_ANGLE,    0, 180, (int)g_angle + 90);
        setTrack(hWnd, IDC_TB_HBOX,     1,  10, g_hboxes);
        setTrack(hWnd, IDC_TB_VBOX,     1,  20, g_vboxes);
        setTrack(hWnd, IDC_TB_OPACITY,  1, 100, g_opacity);
        updateLabels(hWnd);
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_OPEN: doOpen(); return 0;
        case IDC_SAVE: doSave(); return 0;
        case IDC_COLOR: doChooseColor(); return 0;
            invalidatePreview();
            return 0;
        case IDC_CB_SINGLE:
            g_single = (int)SendDlgItemMessageW(hWnd, IDC_CB_SINGLE, BM_GETCHECK, 0, 0);
            invalidatePreview();
            return 0;
        case IDC_BTN_ROTATE:
            g_rotate = (g_rotate + 90) % 360;
            updateLabels(hWnd);
            rebuildView();
            InvalidateRect(hWnd, NULL, TRUE);
            return 0;
        case IDC_TEXT:
            if (HIWORD(wp) == EN_CHANGE)
                SetTimer(hWnd, 1, 150, NULL);   /* 防抖 */
            return 0;
        case IDCANCEL: DestroyWindow(hWnd); return 0;
        }
        break;

    case WM_TIMER:
        if (wp == 1) {
            KillTimer(hWnd, 1);
            GetDlgItemTextW(hWnd, IDC_TEXT, g_text, 255);
            invalidatePreview();
        }
        return 0;

    case WM_HSCROLL: {
        HWND tb = (HWND)lp;
        int id = GetDlgCtrlID(tb);
        int pos = (int)SendMessage(tb, TBM_GETPOS, 0, 0);
        int event = LOWORD(wp);
        switch (id) {
        case IDC_TB_FONT:    g_fontSize = pos; break;
        case IDC_TB_ANGLE:   g_angle    = pos - 90; break;
        case IDC_TB_HBOX:    g_hboxes   = pos; break;
        case IDC_TB_VBOX:    g_vboxes   = pos; break;
        case IDC_TB_OPACITY: g_opacity  = pos; break;
        default: return 0;
        }
        updateLabels(g_hWnd);
        if (event == TB_THUMBTRACK) return 0;   /* 拖动中只更新数字 */
        invalidatePreview();
        return 0;
    }

    case WM_ERASEBKGND: {
        HDC dc = (HDC)wp;
        RECT rc; GetClientRect(hWnd, &rc);
        HBRUSH br = CreateSolidBrush(RGB(240, 240, 240));
        FillRect(dc, &rc, br);
        DeleteObject(br);
        return 1;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hWnd, &ps);

        /* 双缓冲绘制预览区 */
        RECT area; calcPreviewArea(&area);
        int aw = area.right - area.left, ah = area.bottom - area.top;
        if (aw > 0 && ah > 0) {
            HDC mem = CreateCompatibleDC(dc);
            BITMAPINFO dib;
            void *surfBits = NULL;
            HBITMAP bm, oldBm;

            memset(&dib, 0, sizeof(dib));
            dib.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            dib.bmiHeader.biWidth = aw;
            dib.bmiHeader.biHeight = -ah;          /* top-down */
            dib.bmiHeader.biPlanes = 1;
            dib.bmiHeader.biBitCount = 32;
            dib.bmiHeader.biCompression = BI_RGB;
            bm = CreateDIBSection(dc, &dib, DIB_RGB_COLORS, &surfBits, NULL, 0);
            if (!bm) { DeleteDC(mem); EndPaint(hWnd, &ps); return 0; }
            oldBm = (HBITMAP)SelectObject(mem, bm);

            HBRUSH bg = CreateSolidBrush(RGB(240, 240, 240));
            RECT marea = { 0, 0, aw, ah };
            FillRect(mem, &marea, bg);
            DeleteObject(bg);

            if (g_doc) {
                int dx, dy, dw, dh;
                calcPreview(&dx, &dy, &dw, &dh);

                /* 页面阴影边框 + 白底 */
                {
                    RECT pr = { dx - area.left, dy - area.top,
                                dx - area.left + dw, dy - area.top + dh };
                    HBRUSH wb = CreateSolidBrush(RGB(255, 255, 255));
                    HPEN pen = CreatePen(PS_SOLID, 1, RGB(160, 160, 160));
                    HPEN oldPen = (HPEN)SelectObject(mem, pen);
                    HBRUSH oldBr = (HBRUSH)SelectObject(mem, wb);
                    Rectangle(mem, pr.left, pr.top, pr.right, pr.bottom);
                    SelectObject(mem, oldPen);
                    SelectObject(mem, oldBr);
                    DeleteObject(pen);
                    DeleteObject(wb);
                }
                /* 页面原内容 (已按 g_rotate 旋转的底图) */
                if (g_viewBmp && dw > 0 && dh > 0) {
                    HDC bdc = CreateCompatibleDC(mem);
                    HBITMAP obm = (HBITMAP)SelectObject(bdc, g_viewBmp);
                    SetStretchBltMode(mem, COLORONCOLOR);
                    StretchBlt(mem, dx - area.left, dy - area.top, dw, dh,
                               bdc, 0, 0, g_viewBmpW, g_viewBmpH, SRCCOPY);
                    SelectObject(bdc, obm);
                    DeleteDC(bdc);
                }
                /*
                 * 水印: 先画到白色遮罩, 再按 颜色 + 透明度 合成到预览面.
                 * 这样预览与保存后的 PDF 一样是真正半透明的, 能看到下面的内容,
                 * 而不是把颜色调淡后不透明地盖住。
                 */
                {
                    void *maskBits = NULL;
                    HBITMAP maskBm = CreateDIBSection(dc, &dib, DIB_RGB_COLORS,
                                                      &maskBits, NULL, 0);
                    if (maskBm && maskBits) {
                        HDC mdc = CreateCompatibleDC(dc);
                        HBITMAP omb = (HBITMAP)SelectObject(mdc, maskBm);
                        HBRUSH black = CreateSolidBrush(RGB(0, 0, 0));
                        RECT mr = { 0, 0, aw, ah };
                        FillRect(mdc, &mr, black);
                        DeleteObject(black);
                        drawPreviewWatermark(mdc, dx - area.left, dy - area.top, dw, dh);
                        {
                            unsigned char *sp = (unsigned char *)surfBits;
                            unsigned char *mp = (unsigned char *)maskBits;
                            int cr = GetRValue(g_color), cg = GetGValue(g_color), cb = GetBValue(g_color);
                            int alpha255 = g_opacity * 255 / 100;
                            long i, n = (long)aw * ah;
                            for (i = 0; i < n; i++) {
                                int cov = mp[i * 4 + 2];        /* 遮罩 R 通道 = 覆盖率 */
                                int a;
                                if (!cov) continue;
                                a = cov * alpha255 / 255;
                                if (a <= 0) continue;
                                sp[i*4+2] = (unsigned char)(sp[i*4+2] + (cr - sp[i*4+2]) * a / 255);
                                sp[i*4+1] = (unsigned char)(sp[i*4+1] + (cg - sp[i*4+1]) * a / 255);
                                sp[i*4+0] = (unsigned char)(sp[i*4+0] + (cb - sp[i*4+0]) * a / 255);
                            }
                        }
                        SelectObject(mdc, omb);
                        DeleteObject(maskBm);
                        DeleteDC(mdc);
                    }
                }
            } else {
                HFONT pf = CreateFontW(S(-14), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                                       DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY,
                                       0, L"Microsoft YaHei");
                HFONT oldPf = (HFONT)SelectObject(mem, pf);
                SetTextColor(mem, RGB(120, 120, 120));
                SetBkMode(mem, TRANSPARENT);
                RECT tr = { 0, ah / 2 - 20, aw, ah / 2 + 20 };
                DrawTextW(mem, L"点击【打开PDF】选择一张PDF",
                          -1, &tr, DT_CENTER | DT_SINGLELINE);
                SelectObject(mem, oldPf);
                DeleteObject(pf);
       
            }

            BitBlt(dc, area.left, area.top, aw, ah, mem, 0, 0, SRCCOPY);
            SelectObject(mem, oldBm);
            DeleteObject(bm);
            DeleteDC(mem);
        }
        EndPaint(hWnd, &ps);
        return 0;
    }

    case WM_SIZE:
        InvalidateRect(hWnd, NULL, TRUE);
        return 0;

    case WM_DESTROY:
        releasePageBmp();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wp, lp);
}

/* ------------------------------------------------------------------ */
/* 命令行模式 (参数风格与 python 版类似)                                */
/* ------------------------------------------------------------------ */
static void cliPrint(const wchar_t *s)
{
    DWORD w;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s,
              (DWORD)(wcslen(s) * 2), &w, NULL);
}

static int runCli(int argc, wchar_t **argv)
{
    WmParams p;
    wchar_t *mode = L"grid";
    wchar_t *text = NULL, *infile = NULL, *outfile = NULL;
    int i, rc;
    int rotate = 0;                 /* 顺时针旋转角度 (0/90/180/270) */
    wchar_t err[160] = L"";
    PdfDoc *d;
    HANDLE fh;
    DWORD size, got;
    uint8_t *buf;

    /* 附加父控制台以便 printf 可见 */
    AttachConsole(ATTACH_PARENT_PROCESS);

    p.mode = WM_MODE_GRID;
    p.size = PX2PT(FONT_DEF_PX); p.angle = 45; p.opacity = 0.2;
    p.r = GetRValue(g_color) / 255.0;
    p.g = GetGValue(g_color) / 255.0;
    p.b = GetBValue(g_color) / 255.0;
    p.hboxes = 3; p.vboxes = 2;
    p.ix = 0.5; p.iy = 0.5;

    for (i = 1; i < argc; i++) {
        wchar_t *a = argv[i];
        if (!wcscmp(a, L"grid") || !wcscmp(a, L"insert")) { mode = a; continue; }
        if (!wcscmp(a, L"-V") || !wcscmp(a, L"--version")) {
            cliPrint(L"pdfwatermark " PW_VERSION L" " PW_BUILD_DATE L"\n");
            return 0;
        }
        if (!wcscmp(a, L"-o") || !wcscmp(a, L"--opacity")) {
            if (++i < argc) p.opacity = _wtof(argv[i]);
            continue;
        }
        if (!wcscmp(a, L"-a") || !wcscmp(a, L"--angle")) {
            if (++i < argc) p.angle = _wtof(argv[i]);
            continue;
        }
        if (!wcscmp(a, L"-tc") || !wcscmp(a, L"--text-color")) {
            if (++i < argc) {
                const wchar_t *h = argv[i];
                if (*h == L'#') h++;
                g_color = (COLORREF)wcstoul(h, NULL, 16);
                p.r = GetRValue(g_color) / 255.0;
                p.g = GetGValue(g_color) / 255.0;
                p.b = GetBValue(g_color) / 255.0;
            }
            continue;
        }
        if (!wcscmp(a, L"-ts") || !wcscmp(a, L"--text-size")) {
            if (++i < argc) p.size = PX2PT(_wtof(argv[i]));
            continue;
        }
        if (!wcscmp(a, L"-h") || !wcscmp(a, L"--horizontal-boxes")) {
            if (++i < argc) p.hboxes = _wtoi(argv[i]);
            continue;
        }
        if (!wcscmp(a, L"-v") || !wcscmp(a, L"--vertical-boxes")) {
            if (++i < argc) p.vboxes = _wtoi(argv[i]);
            continue;
        }
        if (!wcscmp(a, L"-x") || !wcscmp(a, L"--x")) {
            if (++i < argc) p.ix = _wtof(argv[i]);
            continue;
        }
        if (!wcscmp(a, L"-y") || !wcscmp(a, L"--y")) {
            if (++i < argc) p.iy = _wtof(argv[i]);
            continue;
        }
        if (!wcscmp(a, L"-s") || !wcscmp(a, L"--save")) {
            if (++i < argc) outfile = argv[i];
            continue;
        }
        if (!wcscmp(a, L"-t") || !wcscmp(a, L"--text")) {
            if (++i < argc) text = argv[i];
            continue;
        }
        if (!wcscmp(a, L"-r") || !wcscmp(a, L"--rotate")) {
            if (++i < argc) rotate = _wtoi(argv[i]);
            continue;
        }
        if (!wcscmp(a, L"-h") && !text) { /* help */ }
        /* 位置参数: 先文本后输入文件; .pdf 结尾视为输入文件 */
        if (wcsstr(a, L".pdf") || wcsstr(a, L".PDF")) {
            if (!infile) infile = a; else if (!outfile) outfile = a;
        } else if (!text) {
            text = a;
        } else if (!infile) {
            infile = a;
        }
    }

    if (!infile || !text) {
        cliPrint(
            L"用法: pdfwatermark <grid|insert> <水印文字> <输入.pdf> [选项]\n"
            L"  -s, --save <文件>          输出文件 (默认 原名_水印.pdf)\n"
            L"  -o, --opacity <0..1>       透明度 (默认 0.2)\n"
            L"  -a, --angle <度>           倾斜角度 (默认 45)\n"
            L"  -tc, --text-color <#rrggbb> 文字颜色 (默认 #FF0000)\n"
            L"  -ts, --text-size <px>      字号像素 (默认 50, 最大 200)\n"
            L"  -h, --horizontal-boxes <n> 水平重复数 (默认 3)\n"
            L"  -v, --vertical-boxes <n>   垂直重复数 (默认 2)\n"
            L"  -x, --x <0..1> / -y, --y <0..1>  insert 模式位置\n"
            L"  -r, --rotate <0|90|180|270>  顺时针旋转页面并保存\n"
            L"  -V, --version              显示版本\n");
        return 2;
    }
    if (!outfile) {
        static wchar_t defOut[MAX_PATH];
        wchar_t *e;
        wcscpy(defOut, infile);
        e = wcsrchr(defOut, L'.');
        if (e && _wcsicmp(e, L".pdf") == 0) *e = 0;
        wcscat(defOut, L"_水印.pdf");
        outfile = defOut;
    }
    if (!wcscmp(mode, L"insert")) p.mode = WM_MODE_SINGLE;

    fh = CreateFileW(infile, GENERIC_READ, FILE_SHARE_READ, NULL,
                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (fh == INVALID_HANDLE_VALUE) {
        cliPrint(L"错误: 无法打开输入文件。\n");
        return 1;
    }
    size = GetFileSize(fh, NULL);
    buf = (uint8_t *)malloc(size);
    if (!buf || !ReadFile(fh, buf, size, &got, NULL) || got != size) {
        CloseHandle(fh);
        free(buf);
        cliPrint(L"错误: 读取文件失败。\n");
        return 1;
    }
    CloseHandle(fh);

    d = pdfLoad(buf, size, err, 160);
    free(buf);
    if (!d) {
        cliPrint(L"错误: ");
        cliPrint(err);
        cliPrint(L"\n");
        return 1;
    }
    p.text = text;
    pdfRotateAll(d, rotate);
    rc = pdfApplyWatermark(d, &p, err, 160);
    if (rc == 0) rc = pdfWrite(d, outfile, err, 160);
    pdfFree(d);
    if (rc != 0) {
        cliPrint(L"错误: ");
        cliPrint(err);
        cliPrint(L"\n");
        return 1;
    }
    cliPrint(L"已保存: ");
    cliPrint(outfile);
    cliPrint(L"\n");
    return 0;
}

/* ------------------------------------------------------------------ */
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev,
                    LPWSTR cmdLine, int nShow)
{
    INITCOMMONCONTROLSEX icc;
    WNDCLASSEXW wc;
    MSG msg;

    g_hInst = hInst;

    /* 命令行模式: 仅当第一个参数是 grid / insert 时 */
    {
        int argc = 0;
        wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argc > 1 && argv) {
            /* CLI 模式: 第一个参数为 grid/insert, 或任何 - 开头的选项
             * (如 -V/--version). 否则视为"打开文件", 进 GUI. */
            if (_wcsicmp(argv[1], L"grid") == 0 ||
                _wcsicmp(argv[1], L"insert") == 0 ||
                argv[1][0] == L'-') {
                int r = runCli(argc, argv);
                LocalFree(argv);
                return r;
            }
            LocalFree(argv);
        }
    }

    CoInitialize(NULL);
    {
        /* DPI 感知 */
        HMODULE u32 = GetModuleHandleW(L"user32.dll");
        typedef BOOL (WINAPI *SetCtxFn)(HANDLE);
        SetCtxFn f = u32 ? (SetCtxFn)GetProcAddress(u32, "SetProcessDpiAwarenessContext") : NULL;
        if (f)
            f((HANDLE)-4);
        else {
            typedef BOOL (WINAPI *SetAwareFn)(void);
            SetAwareFn g = u32 ? (SetAwareFn)GetProcAddress(u32, "SetProcessDPIAware") : NULL;
            if (g) g();
        }
        HDC sdc = GetDC(NULL);
        g_dpi = GetDeviceCaps(sdc, LOGPIXELSX);
        ReleaseDC(NULL, sdc);
        if (g_dpi < 96) g_dpi = 96;
    }

    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);

    /* 初始化 MuPDF 并定位内置字体目录 (exe 所在目录下的 mupdf_fonts) */
    {
        wchar_t exePath[MAX_PATH];
        char fontDir[MAX_PATH * 2];
        wchar_t *slash;
        GetModuleFileNameW(NULL, exePath, MAX_PATH);
        slash = wcsrchr(exePath, L'\\');
        if (slash) *slash = 0;
        wcscat(exePath, L"\\mupdf_fonts");
        if (WideCharToMultiByte(CP_UTF8, 0, exePath, -1, fontDir,
                                sizeof(fontDir), NULL, NULL) == 0)
            fontDir[0] = 0;
        pwMuInit(fontDir[0] ? fontDir : NULL);
    }

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = L"PdfWatermarkApp";
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);

    g_hWnd = CreateWindowW(L"PdfWatermarkApp",
        L"PDF \x6C34\x5370\x5DE5\x5177 v" PW_VERSION L" " PW_BUILD_DATE,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, S(1000), S(760),
        NULL, NULL, hInst, NULL);
    ShowWindow(g_hWnd, nShow);

    /* 命令行直接打开 PDF */
    if (cmdLine && *cmdLine) {
        wchar_t path[MAX_PATH];
        if (*cmdLine == L'"') {
            wchar_t *end = wcschr(cmdLine + 1, L'"');
            if (end) { wcsncpy(path, cmdLine + 1, end - cmdLine - 1);
                       path[end - cmdLine - 1] = 0; }
            else wcscpy(path, cmdLine + 1);
        } else wcscpy(path, cmdLine);
        if (path[0] && openPdf(path) == 0) {
            updateInfo();
            InvalidateRect(g_hWnd, NULL, TRUE);
        }
    }

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(g_hWnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (g_doc) pdfFree(g_doc);
    CoUninitialize();
    return 0;
}
