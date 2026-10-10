/*
 * ccitt.c - CCITT Group 3/4 (T.4/T.6) 传真解码 (纯 C, 无依赖)
 *
 * 参考 ITU-T T.4 / T.6。
 * 输出: 1 位位图, 每行 Columns 位, MSB 在左, 行按字节对齐, 黑 = 1。
 */
#include <stdlib.h>
#include <string.h>
#include "ccitt.h"

/* ------------------------------------------------------------------ */
/* 位读取器 (MSB first)                                                */
/* ------------------------------------------------------------------ */
typedef struct { const uint8_t *p; size_t nbits; size_t pos; } Bits;

static int bget(Bits *b)
{
    if (b->pos >= b->nbits) return -1;
    {
        int v = (b->p[b->pos >> 3] >> (7 - (b->pos & 7))) & 1;
        b->pos++;
        return v;
    }
}

/* ------------------------------------------------------------------ */
/* 游程码表 (T.4)                                                      */
/* ------------------------------------------------------------------ */
typedef struct { int bits; int len; int run; } Code;

static const Code W_TERM[] = {
    {0x35,8,0},{0x07,6,1},{0x07,4,2},{0x08,4,3},{0x0B,4,4},{0x0C,4,5},
    {0x0E,4,6},{0x0F,4,7},{0x13,5,8},{0x14,5,9},{0x07,5,10},{0x08,5,11},
    {0x08,6,12},{0x03,6,13},{0x34,6,14},{0x35,6,15},{0x2A,6,16},{0x2B,6,17},
    {0x27,7,18},{0x0C,7,19},{0x08,7,20},{0x17,7,21},{0x03,7,22},{0x04,7,23},
    {0x28,7,24},{0x2B,7,25},{0x13,7,26},{0x24,7,27},{0x18,7,28},{0x02,8,29},
    {0x03,8,30},{0x1A,8,31},{0x1B,8,32},{0x12,8,33},{0x13,8,34},{0x14,8,35},
    {0x15,8,36},{0x16,8,37},{0x17,8,38},{0x28,8,39},{0x29,8,40},{0x2A,8,41},
    {0x2B,8,42},{0x2C,8,43},{0x2D,8,44},{0x04,8,45},{0x05,8,46},{0x0A,8,47},
    {0x0B,8,48},{0x52,8,49},{0x53,8,50},{0x54,8,51},{0x55,8,52},{0x24,8,53},
    {0x25,8,54},{0x58,8,55},{0x59,8,56},{0x5A,8,57},{0x5B,8,58},{0x4A,8,59},
    {0x4B,8,60},{0x32,8,61},{0x33,8,62},{0x34,8,63}
};
static const Code W_MAKE[] = {
    {0x1B,5,64},{0x12,5,128},{0x17,6,192},{0x37,7,256},{0x36,8,320},
    {0x37,8,384},{0x64,8,448},{0x65,8,512},{0x68,8,576},{0x67,8,640},
    {0xCC,9,704},{0xCD,9,768},{0xD2,9,832},{0xD3,9,896},{0xD4,9,960},
    {0xD5,9,1024},{0xD6,9,1088},{0xD7,9,1152},{0xD8,9,1216},{0xD9,9,1280},
    {0xDA,9,1344},{0xDB,9,1408},{0x98,9,1472},{0x99,9,1536},{0x9A,9,1600},
    {0x9B,9,1664},{0xCC,9,1728},{0xCD,9,1792},{0xD2,9,1856},{0xD3,9,1920},
    {0xD4,9,1984},{0xD5,9,2048},{0xD6,9,2112},{0xD7,9,2176},{0xD8,9,2240},
    {0xD9,9,2304},{0xDA,9,2368},{0xDB,9,2432}
};
static const Code B_TERM[] = {
    {0x37,10,0},{0x02,3,1},{0x03,2,2},{0x02,2,3},{0x03,3,4},{0x03,4,5},
    {0x02,4,6},{0x03,5,7},{0x05,6,8},{0x04,6,9},{0x04,7,10},{0x05,7,11},
    {0x07,7,12},{0x04,8,13},{0x07,8,14},{0x18,9,15},{0x17,10,16},{0x18,10,17},
    {0x08,10,18},{0x67,11,19},{0x68,11,20},{0x6C,11,21},{0x37,11,22},{0x28,11,23},
    {0x17,11,24},{0x18,11,25},{0xCA,12,26},{0xCB,12,27},{0xCC,12,28},{0xCD,12,29},
    {0x68,12,30},{0x69,12,31},{0x6A,12,32},{0x6B,12,33},{0xD2,12,34},{0xD3,12,35},
    {0xD4,12,36},{0xD5,12,37},{0xD6,12,38},{0xD7,12,39},{0x6C,12,40},{0x6D,12,41},
    {0xDA,12,42},{0xDB,12,43},{0x54,12,44},{0x55,12,45},{0x56,12,46},{0x57,12,47},
    {0x64,12,48},{0x65,12,49},{0x52,12,50},{0x53,12,51},{0x24,12,52},{0x25,12,53},
    {0x58,12,54},{0x59,12,55},{0x5A,12,56},{0x5B,12,57},{0x4A,12,58},{0x4B,12,59},
    {0x32,12,60},{0x33,12,61},{0x34,12,62},{0x35,12,63}
};
static const Code B_MAKE[] = {
    {0x0F,10,64},{0xC8,12,128},{0xC9,12,192},{0x5C,12,256},{0x5D,12,320},
    {0x5E,12,384},{0x5F,12,448},{0x9A,13,512},{0x9B,13,576},{0x9C,13,640},
    {0x9D,13,704},{0x9E,13,768},{0x9F,13,832},{0xAA,13,896},{0xAB,13,960},
    {0xAC,13,1024},{0xAD,13,1088},{0xAE,13,1152},{0xAF,13,1216},{0xB2,13,1280},
    {0xB3,13,1344},{0xB4,13,1408},{0xB5,13,1472},{0xB6,13,1536},{0xB7,13,1600},
    {0xB8,13,1664},{0xB9,13,1728},{0xBA,13,1792},{0xBB,13,1856},{0xBC,13,1920},
    {0xBD,13,1984},{0xBE,13,2048},{0xBF,13,2112},{0xC0,13,2176},{0xC1,13,2240},
    {0xC2,13,2304},{0xC3,13,2368},{0xC4,13,2432}
};

/*
 * T.6 2D 码 (T.4 表 4): 前缀 + 附加位
 *   V0  1
 *   VR1 011
 *   VR2 0011
 *   VR3 00011
 *   VL1 010
 *   VL2 00010
 *   VL3 000010   <- 注意 T.6 中 VL3 为 0000010
 *   H   001   (水平模式)
 * 为清晰起见用 (bits,len) 直接匹配, 见下表。
 */
typedef struct { int bits; int len; int kind; int delta; } Code2D;
/* kind: 0=V(n), 1=H, 2=P */
static const Code2D C2D[] = {
    {0x1,   1, 0,  0},   /* V0  = 1        */
    {0x3,   3, 0,  1},   /* VR1 = 011      */
    {0x3,   4, 0,  2},   /* VR2 = 0011     */
    {0x3,   5, 0,  3},   /* VR3 = 00011    */
    {0x2,   3, 0, -1},   /* VL1 = 010      */
    {0x2,   4, 0, -2},   /* VL2 = 00010    */
    {0x2,   5, 0, -3},   /* VL3 = 000010   */
    {0x1,   3, 1,  0},   /* H   = 001      */
    {0x1,   6, 2,  0}    /* P   = 000001   (Group3 2D 用) */
};


/* 在游程码表中匹配一个码; 返回 run, -2 = 匹配到 make-up, -1 = 失败 */
static int matchRun(Bits *b, const Code *t, int n)
{
    int v = 0, len;
    for (len = 1; len <= 14; len++) {
        int bit = bget(b);
        int i;
        if (bit < 0) return -1;
        v = (v << 1) | bit;
        for (i = 0; i < n; i++)
            if (t[i].len == len && t[i].bits == v)
                return t[i].run;
    }
    return -1;
}

/* 读一个完整游程 (含 make-up) */
static int readRun(Bits *b, int white)
{
    const Code *term = white ? W_TERM : B_TERM;
    const Code *make = white ? W_MAKE : B_MAKE;
    int nterm = white ? (int)(sizeof(W_TERM)/sizeof(W_TERM[0]))
                      : (int)(sizeof(B_TERM)/sizeof(B_TERM[0]));
    int nmake = white ? (int)(sizeof(W_MAKE)/sizeof(W_MAKE[0]))
                      : (int)(sizeof(B_MAKE)/sizeof(B_MAKE[0]));
    int total = 0, guard = 0;
    for (;;) {
        int r;
        if (++guard > 64) return -1;
        r = matchRun(b, term, nterm);
        if (r >= 0) return total + r;
        r = matchRun(b, make, nmake);
        if (r < 0) return -1;
        total += r;
    }
}

static void setPix(uint8_t *row, int x, int black)
{
    if (black) row[x >> 3] |= (uint8_t)(0x80 >> (x & 7));
}
static int getPix(const uint8_t *row, int x)
{
    return (row[x >> 3] >> (7 - (x & 7))) & 1;
}

/* 参考行中, 从 x 起找第一个颜色相对 a0 颜色发生转变的位置 (b1) */
static int nextChange(const uint8_t *ref, int columns, int x, int refColor)
{
    int i;
    if (x < 0) x = 0;
    for (i = x; i < columns; i++)
        if (getPix(ref, i) != refColor) return i;
    return columns;
}
/* 找 b2: b1 之后下一个转变位置 (颜色相同段的结束) */
static int nextChange2(const uint8_t *ref, int columns, int b1, int color)
{
    int i;
    int c = (b1 < columns) ? getPix(ref, b1) : color;
    for (i = b1 + 1; i < columns; i++)
        if (getPix(ref, i) != c) return i;
    return columns;
}

/* ------------------------------------------------------------------ */
uint8_t *ccittDecode(const uint8_t *in, size_t inLen, int K, int columns,
                     int rows, int *outBytes)
{
    size_t stride;
    uint8_t *out;
    Bits br;
    int r;

    if (outBytes) *outBytes = 0;
    if (!in || inLen == 0 || columns <= 0 || rows <= 0) return NULL;
    if (columns > 100000 || rows > 100000) return NULL;

    stride = ((size_t)columns + 7) / 8;
    out = (uint8_t *)calloc(stride * (size_t)rows, 1);
    if (!out) return NULL;
    br.p = in; br.nbits = inLen * 8; br.pos = 0;

    if (K > 0) {
        /* ---------- Group 3 (1D) ---------- */
        for (r = 0; r < rows; r++) {
            int x = 0, white = 1;
            uint8_t *row = out + (size_t)r * stride;
            while (x < columns) {
                int run = readRun(&br, white);
                int k;
                if (run < 0) break;
                if (!white)
                    for (k = 0; k < run && x + k < columns; k++)
                        setPix(row, x + k, 1);
                x += run;
                white = !white;
                if (run == 0) break;
            }
        }
    } else {
        /* ---------- Group 4 (T.6, 2D) ---------- */
        uint8_t *ref = (uint8_t *)calloc(stride, 1);   /* 初始参考行: 全白 */
        if (!ref) { free(out); return NULL; }
        for (r = 0; r < rows; r++) {
            uint8_t *row = out + (size_t)r * stride;
            int a0 = 0;
            int color = 0;            /* 当前游程颜色: 0=白, 1=黑 */
            int guard = 0;
            while (a0 < columns) {
                int v = 0, len, code = -1, kind = 0, delta = 0;
                if (++guard > columns + 128) break;
                for (len = 1; len <= 7; len++) {
                    int bit = bget(&br);
                    int i;
                    if (bit < 0) { code = -1; break; }
                    v = (v << 1) | bit;
                    for (i = 0; i < (int)(sizeof(C2D)/sizeof(C2D[0])); i++) {
                        if (C2D[i].len == len && C2D[i].bits == v) {
                            kind = C2D[i].kind;
                            delta = C2D[i].delta;
                            code = i;
                            break;
                        }
                    }
                    if (code >= 0) break;
                }
                if (code < 0) break;   /* EOF 或错误: 结束本页 */

                if (kind == 1) {
                    /* 水平模式: 依次读两个游程 (当前色, 反色) */
                    int run1 = readRun(&br, color == 0);
                    int run2 = readRun(&br, color != 0);
                    int x, end1, end2;
                    if (run1 < 0 || run2 < 0) { a0 = columns; break; }
                    end1 = a0 + run1;
                    end2 = end1 + run2;
                    if (end1 > columns) end1 = columns;
                    if (end2 > columns) end2 = columns;
                    if (color) {
                        for (x = a0; x < end1; x++) setPix(row, x, 1);
                    } else {
                        for (x = end1; x < end2; x++) setPix(row, x, 1);
                    }
                    a0 = end2;
                    /* 颜色不变 */
                } else if (kind == 2) {
                    /* P 模式 (pass): 跳过参考行的下一个同色段 */
                    int b1 = nextChange(ref, columns, a0, getPix(ref, a0));
                    int b2 = nextChange2(ref, columns, b1, getPix(ref, b1));
                    a0 = b2;
                } else {
                    /* V(n): a1 = b1 + delta */
                    int b1 = nextChange(ref, columns, a0, color);
                    int a1 = b1 + delta;
                    int x;
                    if (a1 < a0) a1 = a0;
                    if (a1 > columns) a1 = columns;
                    if (color)
                        for (x = a0; x < a1; x++) setPix(row, x, 1);
                    a0 = a1;
                    color ^= 1;
                }
            }
            memcpy(ref, row, stride);
        }
        free(ref);
    }

    if (outBytes) *outBytes = (int)(stride * (size_t)rows);
    return out;
}
