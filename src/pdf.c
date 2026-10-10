/*
 * pdf.c - 迷你 PDF 解析 / 重写引擎 (纯 C, 无第三方库)
 *
 * 功能: 解析 PDF (经典 xref 表 + PDF1.5 交叉引用流 + ObjStm 对象流),
 *       给每个页面追加水印内容流, 全量重写保存为新文件。
 * 编译: gcc -O2 -c pdf.c
 */
#include "pdf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <ctype.h>
#include <windows.h>
#include <gdiplus.h>
#include "ccitt.h"
#include "mupdf_wm.h"

/* ================================================================== */
/* 动态字节缓冲                                                        */
/* ================================================================== */
typedef struct { uint8_t *b; size_t n, cap; } Buf;

static void bufPut(Buf *v, const void *p, size_t n)
{
    if (n == 0) return;
    if (v->n + n > v->cap) {
        size_t c = v->cap ? v->cap : 256;
        while (c < v->n + n) c *= 2;
        v->b = (uint8_t *)realloc(v->b, c);
        v->cap = c;
    }
    memcpy(v->b + v->n, p, n);
    v->n += n;
}
static void bufByte(Buf *v, int c) { uint8_t b = (uint8_t)c; bufPut(v, &b, 1); }
static void bufStr(Buf *v, const char *s) { bufPut(v, s, strlen(s)); }
static void bufFmt(Buf *v, const char *fmt, ...);

/* ================================================================== */
/* Inflate (DEFLATE 解压, 自实现)                                       */
/* ================================================================== */
typedef struct {
    const uint8_t *in; size_t pos, len;
    uint32_t bitbuf; int bitcnt;
} Bits;

static int bitsGet(Bits *z, int need)
{
    uint32_t v = z->bitbuf;
    while (z->bitcnt < need) {
        if (z->pos >= z->len) return -1;
        v |= (uint32_t)z->in[z->pos++] << z->bitcnt;
        z->bitcnt += 8;
    }
    z->bitbuf = v >> need;
    z->bitcnt -= need;
    return (int)(v & ((1u << need) - 1));
}

typedef struct { uint16_t count[16]; uint16_t symbol[288]; } Huff;

static int huffBuild(Huff *h, const uint8_t *lens, int n)
{
    int i, maxv = 0;
    uint16_t offs[16];
    memset(h->count, 0, sizeof(h->count));
    for (i = 0; i < n; i++) {
        if (lens[i] > 15) return -1;
        h->count[lens[i]]++;
        if (lens[i] > maxv) maxv = lens[i];
    }
    h->count[0] = 0;
    offs[0] = offs[1] = 0;
    for (i = 1; i < 15; i++) offs[i + 1] = offs[i] + h->count[i];
    for (i = 0; i < n; i++)
        if (lens[i]) h->symbol[offs[lens[i]]++] = (uint16_t)i;
    return maxv;
}

static int huffDecode(Bits *z, const Huff *h)
{
    int code = 0, first = 0, index = 0, len;
    for (len = 1; len <= 15; len++) {
        int b = bitsGet(z, 1);
        if (b < 0) return -1;
        code |= b;
        int cnt = h->count[len];
        if (code - first < cnt)
            return h->symbol[index + (code - first)];
        index += cnt;
        first = (first + cnt) << 1;
        code <<= 1;
    }
    return -1;
}

static const uint16_t LEN_BASE[29] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,
    35,43,51,59,67,83,99,115,131,163,195,227,258 };
static const uint8_t LEN_EXTRA[29] = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,
    3,3,3,3,4,4,4,4,5,5,5,5,0 };
static const uint16_t DIST_BASE[30] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,
    257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
static const uint8_t DIST_EXTRA[30] = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,
    7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

static int inflateBlock(Bits *z, const Huff *lc, const Huff *dc, Buf *out)
{
    for (;;) {
        int sym = huffDecode(z, lc);
        if (sym < 0) return -1;
        if (sym < 256) {
            bufByte(out, sym);
        } else if (sym == 256) {
            return 0;
        } else {
            int len, dist, ds, i;
            sym -= 257;
            if (sym >= 29) return -1;
            len = LEN_BASE[sym] + bitsGet(z, LEN_EXTRA[sym]);
            ds = huffDecode(z, dc);
            if (ds < 0 || ds >= 30) return -1;
            dist = DIST_BASE[ds] + bitsGet(z, DIST_EXTRA[ds]);
            if (dist <= 0 || dist > (int)out->n) return -1;
            for (i = 0; i < len; i++)
                bufPut(out, out->b + out->n - dist, 1);
        }
    }
}

static int inflateRaw(const uint8_t *in, size_t inLen, Buf *out)
{
    Bits z;
    memset(&z, 0, sizeof(z));
    z.in = in; z.len = inLen;
    for (;;) {
        int last = bitsGet(&z, 1);
        int type = bitsGet(&z, 2);
        if (last < 0 || type < 0) return -1;
        if (type == 0) {
            z.bitbuf = 0; z.bitcnt = 0;
            if (z.pos + 4 > z.len) return -1;
            size_t l = z.in[z.pos] | ((size_t)z.in[z.pos + 1] << 8);
            z.pos += 4;
            if (z.pos + l > z.len) return -1;
            bufPut(out, z.in + z.pos, l);
            z.pos += l;
        } else if (type == 1) {
            static Huff lc, dc;
            static int built = 0;
            uint8_t lens[288];
            int i;
            if (!built) {
                for (i = 0; i < 144; i++) lens[i] = 8;
                for (; i < 256; i++) lens[i] = 9;
                for (; i < 280; i++) lens[i] = 7;
                for (; i < 288; i++) lens[i] = 8;
                huffBuild(&lc, lens, 288);
                for (i = 0; i < 30; i++) lens[i] = 5;
                huffBuild(&dc, lens, 30);
                built = 1;
            }
            if (inflateBlock(&z, &lc, &dc, out) != 0) return -1;
        } else if (type == 2) {
            static Huff lc, dc;
            static const int order[19] = { 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
            uint8_t lens[320], clens[19];
            Huff cl;
            int hlit = bitsGet(&z, 5) + 257;
            int hdist = bitsGet(&z, 5) + 1;
            int hclen = bitsGet(&z, 4) + 4;
            int i, prev = 16;
            if (hlit > 288 || hdist > 30) return -1;
            memset(clens, 0, sizeof(clens));
            for (i = 0; i < hclen; i++) {
                int b = bitsGet(&z, 3);
                if (b < 0) return -1;
                clens[order[i]] = (uint8_t)b;
            }
            if (huffBuild(&cl, clens, 19) < 0) return -1;
            memset(lens, 0, sizeof(lens));
            i = 0;
            while (i < hlit + hdist) {
                int s = huffDecode(&z, &cl);
                if (s < 0) return -1;
                if (s < 16) { lens[i++] = (uint8_t)s; prev = s; }
                else if (s == 16) {
                    int r = 3 + bitsGet(&z, 2);
                    if (i == 0) return -1;
                    while (r-- > 0 && i < hlit + hdist) lens[i++] = (uint8_t)prev;
                } else if (s == 17) {
                    int r = 3 + bitsGet(&z, 3);
                    prev = 0;
                    while (r-- > 0 && i < hlit + hdist) lens[i++] = 0;
                } else {
                    int r = 11 + bitsGet(&z, 7);
                    prev = 0;
                    while (r-- > 0 && i < hlit + hdist) lens[i++] = 0;
                }
            }
            huffBuild(&lc, lens, hlit);
            huffBuild(&dc, lens + hlit, hdist);
            if (inflateBlock(&z, &lc, &dc, out) != 0) return -1;
        } else return -1;
        if (last) break;
    }
    return 0;
}

/* zlib 包装检测 (CMF/FLG) */
static int flateDecode(const uint8_t *in, size_t inLen, Buf *out)
{
    if (inLen >= 2 && (in[0] & 0x0F) == 8 && (((in[0] << 8) | in[1]) % 31) == 0)
        return inflateRaw(in + 2, inLen - 2, out);
    return inflateRaw(in, inLen, out);
}


/* PNG 行预测器还原 (xref stream 常用 Predictor 12) */
static int applyPredictor(Buf *d, int columns, int predictor)
{
    size_t rowLen, pos = 0, outPos = 0, rows, r, i;
    uint8_t *dst, *prevRow = NULL;
    if (predictor < 10 || columns <= 0 || d->n == 0) return 0;
    rowLen = (size_t)columns;
    rows = d->n / (rowLen + 1);
    dst = (uint8_t *)malloc(d->n);
    if (!dst) return -1;
    for (r = 0; r < rows; r++) {
        int ft = d->b[pos];
        const uint8_t *row = d->b + pos + 1;
        uint8_t *cur = dst + outPos;
        for (i = 0; i < rowLen; i++) {
            int a = i >= 1 ? cur[i - 1] : 0;
            int b = prevRow ? prevRow[i] : 0;
            int c = (prevRow && i >= 1) ? prevRow[i - 1] : 0;
            int v = row[i];
            switch (ft) {
            case 0: break;
            case 1: v += a; break;
            case 2: v += b; break;
            case 3: v += (a + b) / 2; break;
            case 4: {
                int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
                v += (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
                break; }
            default: free(dst); return -1;
            }
            cur[i] = (uint8_t)(v & 0xFF);
        }
        outPos += rowLen;
        pos += rowLen + 1;
        prevRow = cur;
    }
    memcpy(d->b, dst, outPos);
    d->n = outPos;
    free(dst);
    return 0;
}



/* ================================================================== */
/* PDF 对象模型                                                        */
/* ================================================================== */
enum { PV_NULL, PV_BOOL, PV_INT, PV_REAL, PV_NAME, PV_STRING,
       PV_ARRAY, PV_DICT, PV_REF };

typedef struct Value Value;
static Value *resolveRef(PdfDoc *d, Value *v);
typedef struct { char *key; Value *val; } KV;
struct Value {
    int type;
    union {
        int b;
        long long i;
        double r;
        char *name;
        struct { uint8_t *b; size_t n; } str;
        struct { Value **v; int n; } arr;
        struct { KV *kv; int n; } dict;
        struct { int num, gen; } ref;
    } u;
};

static Value *vNew(int type)
{
    Value *v = (Value *)calloc(1, sizeof(Value));
    v->type = type;
    return v;
}
static void vFree(Value *v)
{
    int i;
    if (!v) return;
    switch (v->type) {
    case PV_NAME: free(v->u.name); break;
    case PV_STRING: free(v->u.str.b); break;
    case PV_ARRAY:
        for (i = 0; i < v->u.arr.n; i++) vFree(v->u.arr.v[i]);
        free(v->u.arr.v);
        break;
    case PV_DICT:
        for (i = 0; i < v->u.dict.n; i++) {
            free(v->u.dict.kv[i].key);
            vFree(v->u.dict.kv[i].val);
        }
        free(v->u.dict.kv);
        break;
    default: break;
    }
    free(v);
}
static Value *vCopy(Value *v)
{
    int i;
    Value *n;
    if (!v) return NULL;
    n = vNew(v->type);
    switch (v->type) {
    case PV_BOOL: n->u.b = v->u.b; break;
    case PV_INT: n->u.i = v->u.i; break;
    case PV_REAL: n->u.r = v->u.r; break;
    case PV_REF: n->u.ref = v->u.ref; break;
    case PV_NAME: n->u.name = _strdup(v->u.name); break;
    case PV_STRING:
        n->u.str.b = (uint8_t *)malloc(v->u.str.n ? v->u.str.n : 1);
        memcpy(n->u.str.b, v->u.str.b, v->u.str.n);
        n->u.str.n = v->u.str.n;
        break;
    case PV_ARRAY:
        for (i = 0; i < v->u.arr.n; i++) {
            n->u.arr.v = (Value **)realloc(n->u.arr.v,
                       (size_t)(n->u.arr.n + 1) * sizeof(Value *));
            n->u.arr.v[n->u.arr.n++] = vCopy(v->u.arr.v[i]);
        }
        break;
    case PV_DICT:
        for (i = 0; i < v->u.dict.n; i++) {
            n->u.dict.kv = (KV *)realloc(n->u.dict.kv,
                     (size_t)(n->u.dict.n + 1) * sizeof(KV));
            n->u.dict.kv[n->u.dict.n].key = _strdup(v->u.dict.kv[i].key);
            n->u.dict.kv[n->u.dict.n].val = vCopy(v->u.dict.kv[i].val);
            n->u.dict.n++;
        }
        break;
    default: break;
    }
    return n;
}

static Value *dictGet(Value *d, const char *key)
{
    int i;
    if (!d || d->type != PV_DICT) return NULL;
    for (i = 0; i < d->u.dict.n; i++)
        if (strcmp(d->u.dict.kv[i].key, key) == 0)
            return d->u.dict.kv[i].val;
    return NULL;
}
static void dictSet(Value *d, const char *key, Value *val)
{
    int i;
    for (i = 0; i < d->u.dict.n; i++)
        if (strcmp(d->u.dict.kv[i].key, key) == 0) {
            vFree(d->u.dict.kv[i].val);
            d->u.dict.kv[i].val = val;
            return;
        }
    d->u.dict.kv = (KV *)realloc(d->u.dict.kv,
               (size_t)(d->u.dict.n + 1) * sizeof(KV));
    d->u.dict.kv[d->u.dict.n].key = _strdup(key);
    d->u.dict.kv[d->u.dict.n].val = val;
    d->u.dict.n++;
}
static void arrAdd(Value *a, Value *v)
{
    a->u.arr.v = (Value **)realloc(a->u.arr.v,
               (size_t)(a->u.arr.n + 1) * sizeof(Value *));
    a->u.arr.v[a->u.arr.n++] = v;
}

typedef struct {
    int num, gen;
    int used;          /* xref 中登记 */
    int inObjStm;      /* 来自对象流 */
    int skip;          /* 写出时跳过 */
    Value *val;        /* 原始解析值 */
    Value *modVal;     /* 修改后的值 (写出用) */
    const uint8_t *rawStart, *rawEnd;      /* 原始字节范围 */
    const uint8_t *rawStream; size_t rawStreamLen;  /* 原始流(未解码) */
    uint8_t *newStream; size_t newStreamLen;        /* 新对象附带的流 */
} Obj;

typedef struct { int num, gen, type; long long off; int stm, idx; } XEnt;

/* MuPDF 水印字体文件 (NULL/空 表示自动查找系统黑体) */
static const char *g_wmFontFile = NULL;

struct PdfDoc {
    uint8_t *data; size_t len;
    Obj **objs; int objCap;
    int maxObjNum;
    XEnt *xents; int nxent, xentCap;
    int seenCap; unsigned char *seen;
    int rootNum, rootGen;
    int *pages; int npages;
    double *pw, *ph;
    int *prot;              /* 每页 /Rotate (0/90/180/270) */
    int encrypted;
};

static Obj *objEnsure(PdfDoc *d, int num, int gen)
{
    if (num <= 0) return NULL;
    if (num >= d->objCap) {
        int c = d->objCap ? d->objCap : 64;
        while (c <= num) c *= 2;
        d->objs = (Obj **)realloc(d->objs, (size_t)c * sizeof(Obj *));
        memset(d->objs + d->objCap, 0, (size_t)(c - d->objCap) * sizeof(Obj *));
        d->objCap = c;
    }
    if (!d->objs[num]) {
        Obj *o = (Obj *)calloc(1, sizeof(Obj));
        o->num = num;
        d->objs[num] = o;
    }
    if (num > d->maxObjNum) d->maxObjNum = num;
    if (gen > d->objs[num]->gen) d->objs[num]->gen = gen;
    return d->objs[num];
}

static void addXent(PdfDoc *d, int num, int gen, int type,
                    long long off, int stm, int idx)
{
    if (num <= 0) return;
    if (num >= d->seenCap) {
        int c = d->seenCap ? d->seenCap : 256;
        while (c <= num) c *= 2;
        d->seen = (unsigned char *)realloc(d->seen, (size_t)c);
        memset(d->seen + d->seenCap, 0, (size_t)(c - d->seenCap));
        d->seenCap = c;
    }
    if (d->seen[num]) return;      /* 新的 xref 段优先 */
    d->seen[num] = 1;
    if (d->nxent == d->xentCap) {
        d->xentCap = d->xentCap ? d->xentCap * 2 : 256;
        d->xents = (XEnt *)realloc(d->xents, (size_t)d->xentCap * sizeof(XEnt));
    }
    d->xents[d->nxent].num = num; d->xents[d->nxent].gen = gen;
    d->xents[d->nxent].type = type; d->xents[d->nxent].off = off;
    d->xents[d->nxent].stm = stm; d->xents[d->nxent].idx = idx;
    d->nxent++;
}

/* ================================================================== */
/* 词法 / 语法分析                                                     */
/* ================================================================== */
typedef struct { const uint8_t *p, *end; } Cur;

static int isDelim(int c)
{
    return isspace(c) || c=='(' || c==')' || c=='<' || c=='>' ||
           c=='[' || c==']' || c=='{' || c=='}' || c=='/' || c=='%';
}
static void skipWs(Cur *c)
{
    for (;;) {
        while (c->p < c->end && isspace(*c->p)) c->p++;
        if (c->p < c->end && *c->p == '%') {
            while (c->p < c->end && *c->p != '\n' && *c->p != '\r') c->p++;
            continue;
        }
        break;
    }
}
static int kwAt(Cur *c, const char *kw)
{
    size_t n = strlen(kw);
    skipWs(c);
    if ((size_t)(c->end - c->p) < n) return 0;
    if (memcmp(c->p, kw, n) != 0) return 0;
    if ((size_t)(c->end - c->p) > n && !isDelim(c->p[n])) return 0;
    c->p += n;
    return 1;
}
static int hexv(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}
static int tryIntAt(Cur *c, long long *out)
{
    const uint8_t *save = c->p;
    const uint8_t *p;
    int neg = 0;
    long long v = 0;
    const uint8_t *ds;
    skipWs(c);
    p = c->p;
    if (p < c->end && (*p == '+' || *p == '-')) { neg = (*p == '-'); p++; }
    ds = p;
    while (p < c->end && isdigit(*p)) { v = v * 10 + (*p - '0'); p++; }
    if (p == ds || (p < c->end && *p == '.')) { c->p = save; return 0; }
    c->p = p;
    *out = neg ? -v : v;
    return 1;
}

static Value *parseValue(Cur *c, int depth);

static char *parseName(Cur *c)
{
    Buf b;
    memset(&b, 0, sizeof(b));
    c->p++;   /* '/' */
    while (c->p < c->end && !isDelim(*c->p)) {
        if (*c->p == '#' && (size_t)(c->end - c->p) >= 3 &&
            isxdigit(c->p[1]) && isxdigit(c->p[2])) {
            bufByte(&b, hexv(c->p[1]) * 16 + hexv(c->p[2]));
            c->p += 3;
        } else {
            bufByte(&b, *c->p);
            c->p++;
        }
    }
    bufByte(&b, 0);
    return (char *)b.b;
}

static Value *parseLitString(Cur *c)
{
    Buf b;
    int nest = 1;
    memset(&b, 0, sizeof(b));
    c->p++;   /* '(' */
    while (c->p < c->end && nest > 0) {
        int ch = *c->p++;
        if (ch == '(') { nest++; bufByte(&b, ch); }
        else if (ch == ')') { nest--; if (nest) bufByte(&b, ch); }
        else if (ch == 92 && c->p < c->end) {
            int e = *c->p++;
            switch (e) {
            case 'n': bufByte(&b, '\n'); break;
            case 'r': bufByte(&b, '\r'); break;
            case 't': bufByte(&b, '\t'); break;
            case 'b': bufByte(&b, 8); break;
            case 'f': bufByte(&b, 12); break;
            case '\r': if (c->p < c->end && *c->p == '\n') c->p++; break;
            case '\n': break;
            default:
                if (e >= '0' && e <= '7') {
                    int v = e - '0', cnt = 1;
                    while (cnt < 3 && c->p < c->end && *c->p >= '0' && *c->p <= '7')
                        v = v * 8 + (*c->p++ - '0'), cnt++;
                    bufByte(&b, v);
                } else bufByte(&b, e);
            }
        } else bufByte(&b, ch);
    }
    Value *v = vNew(PV_STRING);
    v->u.str.b = b.b; v->u.str.n = b.n;
    return v;
}

static Value *parseHexStr(Cur *c)
{
    Buf b;
    int hi = -1;
    memset(&b, 0, sizeof(b));
    c->p++;   /* '<' */
    while (c->p < c->end && *c->p != '>') {
        int ch = *c->p++;
        if (!isxdigit(ch)) continue;
        if (hi < 0) hi = hexv(ch);
        else { bufByte(&b, hi * 16 + hexv(ch)); hi = -1; }
    }
    if (hi >= 0) bufByte(&b, hi * 16);
    if (c->p < c->end) c->p++;   /* '>' */
    Value *v = vNew(PV_STRING);
    v->u.str.b = b.b; v->u.str.n = b.n;
    return v;
}

static Value *parseNumberOrRef(Cur *c)
{
    const uint8_t *start = c->p;
    const uint8_t *p = c->p;
    int isReal = 0;
    char tmp[64];
    size_t n;
    Value *v;
    if (*p == '+' || *p == '-') p++;
    while (p < c->end && (isdigit(*p) || *p == '.')) { if (*p == '.') isReal = 1; p++; }
    if (p < c->end && (*p == 'e' || *p == 'E')) {
        const uint8_t *q = p + 1;
        if (q < c->end && (*q == '+' || *q == '-')) q++;
        if (q < c->end && isdigit(*q)) {
            isReal = 1; p = q;
            while (p < c->end && isdigit(*p)) p++;
        } else p++;   /* 吞掉孤立的 e/E (畸形数字) */
    }
    n = (size_t)(p - start);
    c->p = p;
    if (n == 0 || n >= sizeof(tmp)) return vNew(PV_NULL);
    memcpy(tmp, start, n);
    tmp[n] = 0;

    if (!isReal) {
        /* 尝试引用 "num gen R" */
        Cur t = { c->p, c->end };
        long long gen;
        skipWs(&t);
        if (tryIntAt(&t, &gen)) {
            skipWs(&t);
            if (t.p < t.end && *t.p == 'R' &&
                (t.end - t.p == 1 || isDelim(t.p[1]))) {
                v = vNew(PV_REF);
                v->u.ref.num = (int)strtoll(tmp, NULL, 10);
                v->u.ref.gen = (int)gen;
                c->p = t.p + 1;
                return v;
            }
        }
        v = vNew(PV_INT);
        v->u.i = strtoll(tmp, NULL, 10);
        return v;
    } else {
        v = vNew(PV_REAL);
        v->u.r = strtod(tmp, NULL);
        return v;
    }
}

static Value *parseValue(Cur *c, int depth)
{
    Value *v;
    int ch;
    skipWs(c);
    if (c->p >= c->end || depth > 64) return vNew(PV_NULL);
    ch = *c->p;
    if (ch == '/') {
        v = vNew(PV_NAME);
        v->u.name = parseName(c);
        return v;
    }
    if (ch == '(') return parseLitString(c);
    if (ch == '<') {
        if (c->end - c->p > 1 && c->p[1] == '<') {
            c->p += 2;
            v = vNew(PV_DICT);
            for (;;) {
                char *key;
                const uint8_t *before;
                skipWs(c);
                if (c->p >= c->end) break;
                if (c->p + 1 < c->end && c->p[0] == '>' && c->p[1] == '>') {
                    c->p += 2;
                    break;
                }
                if (*c->p != '/') { c->p++; continue; }   /* 容错 */
                before = c->p;
                key = parseName(c);
                v->u.dict.kv = (KV *)realloc(v->u.dict.kv,
                         (size_t)(v->u.dict.n + 1) * sizeof(KV));
                v->u.dict.kv[v->u.dict.n].key = key;
                v->u.dict.kv[v->u.dict.n].val = parseValue(c, depth + 1);
                v->u.dict.n++;
                if (c->p == before) c->p++;               /* 防死循环 */
            }
            return v;
        }
        return parseHexStr(c);
    }
    if (ch == '[') {
        c->p++;
        v = vNew(PV_ARRAY);
        for (;;) {
            const uint8_t *before;
            skipWs(c);
            if (c->p >= c->end) break;
            if (*c->p == ']') { c->p++; break; }
            before = c->p;
            arrAdd(v, parseValue(c, depth + 1));
            if (c->p == before) c->p++;   /* 防死循环 */
        }
        return v;
    }
    if (ch == ']' || ch == '>' || ch == ')' || ch == '{' || ch == '}') {
        c->p++;
        return vNew(PV_NULL);
    }
    if (isdigit(ch) || ch == '+' || ch == '-' || ch == '.')
        return parseNumberOrRef(c);
    if (kwAt(c, "true"))  { v = vNew(PV_BOOL); v->u.b = 1; return v; }
    if (kwAt(c, "false")) { v = vNew(PV_BOOL); v->u.b = 0; return v; }
    if (kwAt(c, "null"))  return vNew(PV_NULL);
    /* 未知 token: 跳过 */
    while (c->p < c->end && !isDelim(*c->p)) c->p++;
    return vNew(PV_NULL);
}

/* ================================================================== */
/* 对象加载                                                            */
/* ================================================================== */
static const uint8_t *findBytes(const uint8_t *hay, size_t hlen, const char *needle)
{
    size_t nl = strlen(needle), i;
    if (nl == 0 || hlen < nl) return NULL;
    for (i = 0; i + nl <= hlen; i++)
        if (hay[i] == needle[0] && memcmp(hay + i, needle, nl) == 0)
            return hay + i;
    return NULL;
}

/* 解析一个直接对象 (num gen obj ... endobj); 返回 0 成功 */
/* 偏移容错: 在 off 附近寻找 "wantNum 0 obj" (pypdf 风格的宽容读取) */
static long long findObjNear(const uint8_t *data, size_t len, long long off,
                             int wantNum)
{
    char pat[32];
    int pl = sprintf(pat, "%d 0 obj", wantNum);
    long long lo = off > 64 ? off - 64 : 0;
    long long hi = off + 2048 < (long long)len ? off + 2048 : (long long)len;
    const uint8_t *p = data + lo;
    const uint8_t *stop = data + hi;
    for (;;) {
        p = findBytes(p, (size_t)(stop - p), pat);
        if (!p) return -1;
        if (p > data && !isspace((unsigned char)p[-1])) { p += pl; continue; }
        if (p + pl < data + len && !isDelim(p[pl])) { p += pl; continue; }
        return (long long)(p - data);
    }
}

static int loadObjAt(PdfDoc *d, long long off, int wantNum)
{
    Cur c = { d->data + off, d->data + d->len };
    const uint8_t *afterDict;
    Value *v;
    Value *lv;
    Obj *o;
    long long num, gen;
    const uint8_t *streamData = NULL;
    size_t streamLen = 0;

    skipWs(&c);
    if (!tryIntAt(&c, &num) || !tryIntAt(&c, &gen) || !kwAt(&c, "obj")) {
        if (wantNum > 0) {
            long long alt = findObjNear(d->data, d->len, off, wantNum);
            if (alt < 0) return -1;
            off = alt;
            c.p = d->data + off;
            skipWs(&c);
            if (!tryIntAt(&c, &num) || !tryIntAt(&c, &gen) || !kwAt(&c, "obj"))
                return -1;
        } else
            return -1;
    }
    if (wantNum > 0 && num != wantNum) return -1;
    if (num <= 0 || num > 0x7FFFFFF0) return -1;
    const uint8_t *valStart = c.p;   /* "obj" 之后, 值开始 */
    v = parseValue(&c, 0);
    afterDict = c.p;

    if (v->type == PV_DICT) {
        Cur t = { afterDict, c.end };
        skipWs(&t);
        if (kwAt(&t, "stream")) {
            const uint8_t *es;
            long long len = -1;
            if (t.p < t.end && *t.p == '\r') t.p++;
            if (t.p < t.end && *t.p == '\n') t.p++;
            streamData = t.p;
            lv = dictGet(v, "Length");
            if (lv && lv->type == PV_INT) len = lv->u.i;
            es = findBytes(streamData, (size_t)(c.end - streamData), "endstream");
            if (len >= 0 && streamData + len <= c.end &&
                (!es || (size_t)len <= (size_t)(es - streamData)))
                streamLen = (size_t)len;
            else if (es)
                streamLen = (size_t)(es - streamData);
            else
                streamLen = (size_t)(c.end - streamData);
            if (es) {
                const uint8_t *eo = findBytes(es, (size_t)(c.end - es), "endobj");
                c.p = eo ? eo + 6 : c.end;
            } else c.p = c.end;
        }
    }
    if (!streamData) {
        const uint8_t *eo = findBytes(c.p, (size_t)(c.end - c.p), "endobj");
        c.p = eo ? eo + 6 : c.end;
    }

    o = objEnsure(d, (int)num, (int)gen);
    o->used = 1;
    o->val = v;
    /* raw 范围只覆盖对象值本身 (不含 "N G obj" 头与 "endobj") */
    o->rawStart = valStart;
    if (streamData && streamLen >= 0) {
        const uint8_t *es = findBytes(streamData + streamLen,
            (size_t)(c.end - streamData - streamLen), "endstream");
        o->rawEnd = es ? es + 9 : c.p;
    } else
        o->rawEnd = afterDict;
    o->rawStream = streamData;
    o->rawStreamLen = streamLen;
    return 0;
}

/* 解码对象流 ObjStm: 把其中包含的对象解析出来 */
static int loadObjStm(PdfDoc *d, Obj *o)
{
    Value *dict = o->val;
    Value *nv = dictGet(dict, "N");
    Value *fv = dictGet(dict, "First");
    Buf dec;
    Cur c;
    int i, n, first;

    memset(&dec, 0, sizeof(dec));
    if (!nv || !fv || nv->type != PV_INT || fv->type != PV_INT) return -1;
    if (flateDecode(o->rawStream, o->rawStreamLen, &dec) != 0) {
        free(dec.b);
        return -1;
    }
    n = (int)nv->u.i;
    first = (int)fv->u.i;
    if (n <= 0 || n > 100000 || first < 0 || first >= (int)dec.n) {
        free(dec.b);
        return -1;
    }
    c.p = dec.b; c.end = dec.b + dec.n;
    for (i = 0; i < n; i++) {
        long long num, off;
        Value *v;
        Obj *no;
        skipWs(&c);
        if (!tryIntAt(&c, &num) || !tryIntAt(&c, &off)) break;
        if (num <= 0 || num > 0x7FFFFFF0) break;
        {
            Cur sc = { dec.b + first + off, dec.b + dec.n };
            v = parseValue(&sc, 0);
        }
        no = objEnsure(d, (int)num, o->gen);
        no->used = 1;
        no->inObjStm = 1;
        no->val = v;
    }
    o->skip = 1;   /* ObjStm 容器不再写出 */
    free(dec.b);
    return 0;
}

/* 解码流数据 (仅支持 FlateDecode / 无过滤) */
static int decodeStream(PdfDoc *d, Value *dict, const uint8_t *raw, size_t rawLen, Buf *out)
{
    Value *f = dictGet(dict, "Filter");
    int flate = 0;
    memset(out, 0, sizeof(*out));
    if (f) {
        if (f->type == PV_NAME && strcmp(f->u.name, "FlateDecode") == 0)
            flate = 1;
        else if (f->type == PV_ARRAY) {
            int i;
            for (i = 0; i < f->u.arr.n; i++) {
                Value *e = f->u.arr.v[i];
                if (e->type == PV_NAME && strcmp(e->u.name, "FlateDecode") == 0)
                    flate = 1;
                else if (e->type == PV_NAME) {
                    /* 不支持的过滤 (如 LCrypt) */
                    return -2;
                }
            }
        } else if (f->type == PV_NULL) flate = 0;
        else flate = 0;
    }
    if (flate) {
        if (flateDecode(raw, rawLen, out) != 0) { free(out->b); return -1; }
        /* predictor */
        {
            Value *dp = dictGet(dict, "DecodeParms");
            if (!dp && f && f->type == PV_ARRAY) {
                Value *a = dictGet(dict, "DecodeParms");
                if (a && a->type == PV_ARRAY && a->u.arr.n > 0) dp = a->u.arr.v[0];
            }
            if (dp && dp->type == PV_DICT) {
                Value *pv = dictGet(dp, "Predictor");
                Value *cv = dictGet(dp, "Columns");
                int pred = (pv && pv->type == PV_INT) ? (int)pv->u.i : 1;
                int cols = (cv && cv->type == PV_INT) ? (int)cv->u.i : 1;
                if (pred >= 10) {
                    if (applyPredictor(out, cols, pred) != 0) { free(out->b); return -1; }
                }
            }
        }
        return 0;
    }
    bufPut(out, raw, rawLen);
    return 0;
}

/* ================================================================== */
/* xref 解析                                                           */
/* ================================================================== */

/* 经典 xref 表: 返回 trailer 字典 (可能为 NULL) 和 /Prev */
static Value *parseClassicXref(PdfDoc *d, long long off, long long *prevOut)
{
    Cur c = { d->data + off, d->data + d->len };
    Value *trailer = NULL;
    *prevOut = -1;
    skipWs(&c);
    if (!kwAt(&c, "xref")) return NULL;
    for (;;) {
        long long start, count, i;
        skipWs(&c);
        if (c.p >= c.end) break;
        if (kwAt(&c, "trailer")) {
            trailer = parseValue(&c, 0);
            break;
        }
        if (!tryIntAt(&c, &start) || !tryIntAt(&c, &count)) break;
        if (count < 0 || count > 0x1000000) break;
        for (i = 0; i < count; i++) {
            long long f1, f2;
            int typ;
            /* 每条记录 20 字节: %10d %5d [nf]  但容错解析 */
            skipWs(&c);
            if (c.end - c.p < 18) break;
            {
                const uint8_t *p = c.p;
                int k;
                long long a = 0, b = 0;
                while (p < c.end && *p == ' ') p++;
                for (k = 0; k < 10 && p < c.end && isdigit(*p); k++) a = a * 10 + (*p++ - '0');
                while (p < c.end && *p == ' ') p++;
                for (k = 0; k < 5 && p < c.end && isdigit(*p); k++) b = b * 10 + (*p++ - '0');
                while (p < c.end && *p == ' ') p++;
                if (p >= c.end) break;
                typ = *p++;
                c.p = p;
                /* 跳过行尾剩余 (\r\n / \n / 空格) */
                while (p < c.end && *p != '\n' && *p != '\r' && k < 2) {
                    if (*p != ' ') break;
                    p++;
                }
                c.p = p;
                f1 = a; f2 = b;
            }
            if (typ == 'n')
                addXent(d, (int)(start + i), (int)f2, 1, f1, 0, 0);
            else
                addXent(d, (int)(start + i), (int)f2, 0, 0, 0, 0);
        }
    }
    if (trailer) {
        Value *pv = dictGet(trailer, "Prev");
        if (pv && pv->type == PV_INT) *prevOut = pv->u.i;
    }
    return trailer;
}

/* 交叉引用流 (PDF1.5): 返回其字典 (含 /Root /Prev /Size) */
static Value *parseXrefStream(PdfDoc *d, long long off, long long *prevOut)
{
    Cur c = { d->data + off, d->data + d->len };
    long long num, gen;
    Value *dict;
    Obj *o;
    Value *wv, *iv, *sz;
    int w0, w1, w2;
    Buf dec;
    size_t pos = 0;
    int iRows = 0, iSub;
    long long idxStore[2] = { 0, 0 };

    *prevOut = -1;
    skipWs(&c);
    if (!tryIntAt(&c, &num) || !tryIntAt(&c, &gen) || !kwAt(&c, "obj")) return NULL;
    dict = parseValue(&c, 0);
    if (dict->type != PV_DICT) { vFree(dict); return NULL; }

    /* 定位流数据 */
    {
        Cur t = { c.p, c.end };
        skipWs(&t);
        if (!kwAt(&t, "stream")) { vFree(dict); return NULL; }
        if (t.p < t.end && *t.p == '\r') t.p++;
        if (t.p < t.end && *t.p == '\n') t.p++;
        c.p = t.p;
    }
    if (loadObjAt(d, off, -1) != 0) { vFree(dict); return NULL; }
    o = objEnsure(d, (int)num, (int)gen);
    if (!o->rawStream) { vFree(dict); return NULL; }

    if (decodeStream(d, dict, o->rawStream, o->rawStreamLen, &dec) != 0) {
        vFree(dict);
        return NULL;
    }

    wv = dictGet(dict, "W");
    sz = dictGet(dict, "Size");
    iv = dictGet(dict, "Index");
    if (!wv || wv->type != PV_ARRAY || wv->u.arr.n < 1) {
        free(dec.b); vFree(dict); return NULL;
    }
    w0 = (wv->u.arr.n > 0 && wv->u.arr.v[0]->type == PV_INT) ? (int)wv->u.arr.v[0]->u.i : 1;
    w1 = (wv->u.arr.n > 1 && wv->u.arr.v[1]->type == PV_INT) ? (int)wv->u.arr.v[1]->u.i : 0;
    w2 = (wv->u.arr.n > 2 && wv->u.arr.v[2]->type == PV_INT) ? (int)wv->u.arr.v[2]->u.i : 0;

    /* 子段列表 */
    if (iv && iv->type == PV_ARRAY && iv->u.arr.n >= 2) {
        iRows = iv->u.arr.n / 2;
    } else if (sz && sz->type == PV_INT) {
        idxStore[0] = 0; idxStore[1] = sz->u.i;
        iv = NULL;
        iRows = 1;
    } else { free(dec.b); vFree(dict); return NULL; }

    for (iSub = 0; iSub < iRows; iSub++) {
        long long start, count, r;
        if (iv) {
            if (iv->u.arr.v[2*iSub]->type != PV_INT ||
                iv->u.arr.v[2*iSub+1]->type != PV_INT) break;
            start = iv->u.arr.v[2*iSub]->u.i;
            count = iv->u.arr.v[2*iSub+1]->u.i;
        } else {
            start = idxStore[0]; count = idxStore[1];
        }
        if (count < 0 || count > 0x1000000) break;
        for (r = 0; r < count; r++) {
            long long f[3] = { 1, 0, 0 };
            int k;
            for (k = 0; k < 3; k++) {
                int w = k == 0 ? w0 : (k == 1 ? w1 : w2);
                if (w == 0) { f[k] = (k == 0) ? 1 : 0; continue; }
                if (pos + (size_t)w > dec.n) { free(dec.b); vFree(dict); return NULL; }
                f[k] = 0;
                for (; w > 0; w--) f[k] = (f[k] << 8) | dec.b[pos++];
            }
            if (f[0] == 1)
                addXent(d, (int)(start + r), (int)f[2], 1, f[1], 0, 0);
            else if (f[0] == 2)
                addXent(d, (int)(start + r), 0, 2, 0, (int)f[1], (int)f[2]);
            else
                addXent(d, (int)(start + r), 0, 0, 0, 0, 0);
        }
    }
    free(dec.b);

    {
        Value *pv = dictGet(dict, "Prev");
        if (pv && pv->type == PV_INT) *prevOut = pv->u.i;
    }
    return dict;
}

/* ================================================================== */
/* 页面树遍历                                                          */
/* ================================================================== */
static Obj *resolve(PdfDoc *d, Value *v)
{
    if (!v) return NULL;
    if (v->type == PV_REF) {
        if (v->u.ref.num > 0 && v->u.ref.num < d->objCap)
            return d->objs[v->u.ref.num];
        return NULL;
    }
    /* 内联对象: 借用一个伪对象不可行, 调用方处理 */
    return NULL;
}

static void getMediaBox(PdfDoc *d, Value *dict, Value *inherited, double *w, double *h)
{
    Value *mb = dictGet(dict, "MediaBox");
    Obj *o;
    *w = 612; *h = 792;
    if (!mb && inherited) mb = inherited;
    if (mb && mb->type == PV_REF) {
        o = resolve(d, mb);
        if (o && o->val) mb = o->val;
    }
    if (mb && mb->type == PV_ARRAY && mb->u.arr.n >= 4) {
        double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        int i;
        for (i = 0; i < 4; i++) {
            Value *e = mb->u.arr.v[i];
            double v = 0;
            if (e->type == PV_INT) v = (double)e->u.i;
            else if (e->type == PV_REAL) v = e->u.r;
            if (i == 0) x0 = v; else if (i == 1) y0 = v;
            else if (i == 2) x1 = v; else y1 = v;
        }
        if (x1 > x0 && y1 > y0) { *w = x1 - x0; *h = y1 - y0; }
    }
}

static void addPage(PdfDoc *d, int num, double w, double h, int rot)
{
    d->pages = (int *)realloc(d->pages, (size_t)(d->npages + 1) * sizeof(int));
    d->pw = (double *)realloc(d->pw, (size_t)(d->npages + 1) * sizeof(double));
    d->ph = (double *)realloc(d->ph, (size_t)(d->npages + 1) * sizeof(double));
    d->prot = (int *)realloc(d->prot, (size_t)(d->npages + 1) * sizeof(int));
    d->pages[d->npages] = num;
    d->pw[d->npages] = w;
    d->ph[d->npages] = h;
    d->prot[d->npages] = ((rot % 360) + 360) % 360;
    d->npages++;
}

/* 读取 /Rotate (整数, 继承) */
static int getRotate(Value *dict, int inherited)
{
    Value *r = dictGet(dict, "Rotate");
    int v = inherited;
    if (r && (r->type == PV_INT || r->type == PV_REAL))
        v = (r->type == PV_INT) ? (int)r->u.i : (int)r->u.r;
    return v;
}
static void findPagesX(PdfDoc *d, int num, Value *inheritedMB, int inheritedRot,
                       int depth);
static void findPages(PdfDoc *d, int num, Value *inheritedMB, int depth)
{
    findPagesX(d, num, inheritedMB, 0, depth);
}
static void findPagesX(PdfDoc *d, int num, Value *inheritedMB, int inheritedRot,
                       int depth)
{
    Obj *o;
    Value *dict;
    if (depth > 64 || d->npages > 100000) return;
    if (num <= 0 || num >= d->objCap) return;
    o = d->objs[num];
    if (!o || !o->val || o->val->type != PV_DICT) return;
    dict = o->val;

    {
        Value *mb = dictGet(dict, "MediaBox");
        if (mb) inheritedMB = mb;
    }
    inheritedRot = getRotate(dict, inheritedRot);

    {
        Value *type = dictGet(dict, "Type");
        Value *kids = dictGet(dict, "Kids");
        int isPages = 0;
        if (type && type->type == PV_NAME && strcmp(type->u.name, "Pages") == 0)
            isPages = 1;
        if (!isPages && !kids &&
            !(type && type->type == PV_NAME && strcmp(type->u.name, "Page") == 0)) {
            /* 无 Type 无 Kids: 若像页面则按页面处理 */
            if (!dictGet(dict, "Contents")) return;
        }
        if (kids && kids->type == PV_ARRAY) {
            int i;
            double w, h;
            getMediaBox(d, dict, inheritedMB, &w, &h);
            for (i = 0; i < kids->u.arr.n; i++) {
                Value *k = kids->u.arr.v[i];
                if (k->type == PV_REF)
                    findPagesX(d, k->u.ref.num, inheritedMB, inheritedRot, depth + 1);
            }
            return;
        }
        /* 叶子页面 */
        {
            double w, h;
            getMediaBox(d, dict, inheritedMB, &w, &h);
            addPage(d, num, w, h, inheritedRot);
        }
    }
}

/* ================================================================== */
/* 载入 / 释放                                                         */
/* ================================================================== */
PdfDoc *pdfLoad(const uint8_t *fileData, size_t fileLen, wchar_t *err, size_t errMax)
{
    PdfDoc *d;
    const uint8_t *sx;
    long long off;
    int i;

    if (err) err[0] = 0;
    if (fileLen < 64 || memcmp(fileData, "%PDF-", 5) != 0) {
        if (err) wcscpy(err, L"文件不是 PDF 格式 (缺少 %PDF- 头)。");
        return NULL;
    }
    d = (PdfDoc *)calloc(1, sizeof(PdfDoc));
    d->data = (uint8_t *)malloc(fileLen);
    memcpy(d->data, fileData, fileLen);
    d->len = fileLen;

    /* 找最后一个 startxref */
    sx = NULL;
    {
        const uint8_t *from = fileData;
        for (;;) {
            const uint8_t *p = findBytes(from, fileLen - (size_t)(from - fileData), "startxref");
            if (!p) break;
            sx = p;
            from = p + 9;
        }
    }
    if (!sx) {
        if (err) wcscpy(err, L"未找到 startxref, 文件可能损坏。");
        pdfFree(d);
        return NULL;
    }
    {
        Cur c = { sx + 9, fileData + fileLen };
        long long v;
        skipWs(&c);
        if (!tryIntAt(&c, &v)) {
            if (err) wcscpy(err, L"startxref 偏移解析失败。");
            pdfFree(d);
            return NULL;
        }
        off = v;
    }

    /* 沿 /Prev 链解析所有 xref 段 */
    {
        int guard = 0;
        while (off >= 0 && off < (long long)fileLen && guard++ < 512) {
            long long prev = -1;
            Value *trailer = NULL;
            Cur t = { d->data + off, d->data + d->len };
            skipWs(&t);
            if (t.end - t.p >= 4 && memcmp(t.p, "xref", 4) == 0)
                trailer = parseClassicXref(d, off, &prev);
            else
                trailer = parseXrefStream(d, off, &prev);
            if (!trailer) break;
            if (!d->rootNum) {
                Value *r = dictGet(trailer, "Root");
                if (r && r->type == PV_REF) {
                    d->rootNum = r->u.ref.num;
                    d->rootGen = r->u.ref.gen;
                }
                if (dictGet(trailer, "Encrypt")) d->encrypted = 1;
            }
            vFree(trailer);
            off = prev;
        }
    }

    if (!d->rootNum) {
        /* 回退: startxref 偏移无效 (文件损坏/被改过) 时,
           扫描文件中最后一个 "xref" 关键字重试 */
        const uint8_t *lastXref = NULL;
        const uint8_t *from = d->data;
        for (;;) {
            const uint8_t *p = findBytes(from, d->len - (size_t)(from - d->data), "xref");
            if (!p) break;
            if (p > d->data && isspace(p[-1])) lastXref = p;
            from = p + 4;
        }
        if (lastXref) {
            long long prev2 = -1;
            Value *trailer = parseClassicXref(d, (long long)(lastXref - d->data), &prev2);
            if (trailer) {
                Value *r = dictGet(trailer, "Root");
                if (r && r->type == PV_REF) {
                    d->rootNum = r->u.ref.num;
                    d->rootGen = r->u.ref.gen;
                }
                vFree(trailer);
            }
        }
    }

    if (d->encrypted) {
        if (err) wcscpy(err, L"该 PDF 已加密, 不支持。");
        pdfFree(d);
        return NULL;
    }
    if (!d->rootNum) {
        if (err) wcscpy(err, L"未找到文档根对象 (Root)。");
        pdfFree(d);
        return NULL;
    }

    /* 加载全部直接对象 */
    for (i = 0; i < d->nxent; i++) {
        XEnt *e = &d->xents[i];
        if (e->type == 1 && e->off >= 0 && e->off < (long long)fileLen)
            loadObjAt(d, e->off, e->num);
    }
    /* 展开对象流 ObjStm */
    for (i = 1; i < d->objCap; i++) {
        Obj *o = d->objs[i];
        if (o && o->val && o->val->type == PV_DICT && o->rawStream) {
            Value *t = dictGet(o->val, "Type");
            if (t && t->type == PV_NAME && strcmp(t->u.name, "ObjStm") == 0)
                loadObjStm(d, o);
        }
    }

    /* 页面树 */
    {
        Obj *cat = objEnsure(d, d->rootNum, d->rootGen);
        if (cat && cat->val && cat->val->type == PV_DICT) {
            Value *pg = dictGet(cat->val, "Pages");
            if (pg && pg->type == PV_REF)
                findPages(d, pg->u.ref.num, NULL, 0);
        }
    }
    if (d->npages == 0) {
        if (err) wcscpy(err, L"未找到任何页面。");
        pdfFree(d);
        return NULL;
    }
    return d;
}

void pdfFree(PdfDoc *d)
{
    int i;
    if (!d) return;
    if (d->objs) {
        for (i = 0; i < d->objCap; i++) {
            Obj *o = d->objs[i];
            if (!o) continue;
            vFree(o->val);
            vFree(o->modVal);
            free(o->newStream);
            free(o);
        }
        free(d->objs);
    }
    free(d->data);
    free(d->xents);
    free(d->seen);
    free(d->pages);
    free(d->pw);
    free(d->ph);
    free(d->prot);
    free(d);
}

int pdfPageCount(PdfDoc *d) { return d->npages; }
/* 返回页面"显示尺寸" (考虑 /Rotate; 90/270 时宽高互换) */
void pdfGetPage(PdfDoc *d, int idx, double *w, double *h)
{
    if (idx < 0 || idx >= d->npages) { *w = 612; *h = 792; return; }
    if (d->prot[idx] == 90 || d->prot[idx] == 270) {
        *w = d->ph[idx]; *h = d->pw[idx];
    } else {
        *w = d->pw[idx]; *h = d->ph[idx];
    }
}

/* ================================================================== */
/* 水印                                                                */
/* ================================================================== */

/* Helvetica 标准字体宽度 (/1000), ASCII 32..126 */
static const int HELV_W[95] = {
    278,278,355,556,556,889,667,191,333,333,389,584,278,333,278,278,
    556,556,556,556,556,556,556,556,556,556,278,278,584,584,584,556,
    1015,667,667,722,722,667,611,778,722,278,500,667,611,833,722,778,
    667,778,722,667,611,722,667,667,611,667,667,278,278,278,469,556,
    333,556,556,500,556,556,278,556,556,222,222,500,222,833,556,556,
    556,556,333,500,278,556,500,722,500,500,500,334,260,334,584 };

static int isAsciiOnly(const wchar_t *t)
{
    while (*t) if (*t++ > 126) return 0;
    return 1;
}

/* 转成 PDF 字面串字节 (ASCII->WinAnsi, 中文->GBK), 并计算文本宽度(/1000) */
static int encodeText(const wchar_t *t, int cjk, Buf *out, double *width1000)
{
    Buf gbk;
    size_t i;
    int w = 0;
    memset(&gbk, 0, sizeof(gbk));
    if (cjk) {
        int n = WideCharToMultiByte(936, 0, t, -1, NULL, 0, NULL, NULL);
        if (n <= 0) return -1;
        gbk.b = (uint8_t *)malloc((size_t)n);
        WideCharToMultiByte(936, 0, t, -1, (char *)gbk.b, n, NULL, NULL);
        gbk.n = (size_t)n - 1;   /* 去掉结尾 \0 */
        for (i = 0; i < gbk.n; ) {
            uint8_t b = gbk.b[i];
            if (b >= 0x81 && i + 1 < gbk.n) { i += 2; w += 1000; }
            else {
                if (b >= 32 && b <= 126) w += HELV_W[b - 32];
                i++;
            }
        }
        for (i = 0; i < gbk.n; i++) {
            uint8_t b = gbk.b[i];
            if (b == '(' || b == ')' || b == 92) bufByte(out, 92);
            bufByte(out, b);
        }
        free(gbk.b);
    } else {
        for (i = 0; t[i]; i++) {
            wchar_t ch = t[i];
            if (ch >= 32 && ch <= 126) w += HELV_W[ch - 32];
            if (ch == '(' || ch == ')' || ch == 92) bufByte(out, 92);
            bufByte(out, (int)ch);
        }
    }
    *width1000 = w;
    return 0;
}

static int newObjNum(PdfDoc *d)
{
    return d->maxObjNum + 1;   /* objEnsure 会更新 maxObjNum */
}

/* ================================================================== */
/* 并入 MuPDF 生成的"嵌入字体"覆盖层                                    */
/* ================================================================== */

/*
 * 把覆盖层 PDF (单页, 由 pwWmBuild 生成) 的内容流 + 字体资源并入目标文档.
 * 这是为了让导出的水印带上**已嵌入子集的 CJK 字体**, 从而与预览一致,
 * 不受阅读器字体替换影响.
 *
 * 实现: 递归深拷贝覆盖层对象子树, 对象号重映射.
 *   - contentNum: 输出覆盖层第 0 页内容流的对象号
 *   - 返回: 新建的字体资源字典 (可直接用作 /Resources/Font), 失败返回 NULL
 */

static Value *ovCopyVal(PdfDoc *dst, PdfDoc *src, int *map, Value *v, int depth);
static int ovCopyRef(PdfDoc *dst, PdfDoc *src, int *map, int num, int depth);

/* 深拷贝一个直接值; REF 交给 ovCopyRef 处理对象分配 */
static Value *ovCopyVal(PdfDoc *dst, PdfDoc *src, int *map, Value *v, int depth)
{
    Value *n;
    int i;
    if (!v) return NULL;
    if (depth > 32) return vNew(PV_NULL);
    switch (v->type) {
    case PV_REF: {
        int dn = ovCopyRef(dst, src, map, v->u.ref.num, depth + 1);
        n = vNew(PV_REF);
        n->u.ref.num = dn;
        n->u.ref.gen = 0;
        return n;
    }
    case PV_ARRAY:
        n = vNew(PV_ARRAY);
        for (i = 0; i < v->u.arr.n; i++)
            arrAdd(n, ovCopyVal(dst, src, map, v->u.arr.v[i], depth + 1));
        return n;
    case PV_DICT:
        n = vNew(PV_DICT);
        for (i = 0; i < v->u.dict.n; i++)
            dictSet(n, v->u.dict.kv[i].key,
                    ovCopyVal(dst, src, map, v->u.dict.kv[i].val, depth + 1));
        return n;
    default:
        return vCopy(v);
    }
}

/* 把一个覆盖层对象拷到目标文档, 返回目标对象号 (0 表示失败) */
static int ovCopyRef(PdfDoc *dst, PdfDoc *src, int *map, int num, int depth)
{
    Obj *so, *do_;
    Value *sv;
    if (num <= 0 || num > src->maxObjNum || depth > 32) return 0;
    if (map[num]) return map[num];
    so = src->objs[num];
    if (!so) return 0;
    sv = so->modVal ? so->modVal : so->val;
    {
        int dn = newObjNum(dst);
        map[num] = dn;
        do_ = objEnsure(dst, dn, 0);
        if (sv) do_->modVal = ovCopyVal(dst, src, map, sv, depth + 1);
        else do_->modVal = vNew(PV_NULL);
        /* 流数据 */
        if (so->newStream) {
            do_->newStream = (uint8_t *)malloc(so->newStreamLen ? so->newStreamLen : 1);
            memcpy(do_->newStream, so->newStream, so->newStreamLen);
            do_->newStreamLen = so->newStreamLen;
        } else if (so->rawStream) {
            do_->newStream = (uint8_t *)malloc(so->rawStreamLen ? so->rawStreamLen : 1);
            memcpy(do_->newStream, so->rawStream, so->rawStreamLen);
            do_->newStreamLen = so->rawStreamLen;
        }
        return dn;
    }
}

static Value *overlayImport(PdfDoc *d, const uint8_t *ovData, size_t ovLen,
                            int *contentNum, wchar_t *err, size_t errMax)
{
    PdfDoc *src;
    int *map;
    Value *fonts = NULL;
    Obj *pg;
    Value *pd, *res, *fnt, *cont;
    int pnum, cnum;

    src = pdfLoad(ovData, ovLen, err, errMax);
    if (!src) return NULL;
    map = (int *)calloc((size_t)src->maxObjNum + 2, sizeof(int));
    if (!map) { pdfFree(src); return NULL; }

    fonts = vNew(PV_DICT);
    pnum = src->pages[0];
    pg = src->objs[pnum];
    pd = pg->modVal ? pg->modVal : pg->val;
    res = resolveRef(src, dictGet(pd, "Resources"));
    fnt = res ? dictGet(res, "Font") : NULL;
    cont = dictGet(pd, "Contents");

    /* 字体资源: /F0 -> 拷贝后的字体对象 */
    if (fnt && fnt->type == PV_DICT) {
        int i;
        for (i = 0; i < fnt->u.dict.n; i++) {
            Value *fv = fnt->u.dict.kv[i].val;
            Value *nv = ovCopyVal(d, src, map, fv, 0);
            dictSet(fonts, fnt->u.dict.kv[i].key, nv);
        }
    }
    /* 内容流 */
    cnum = 0;
    if (cont && cont->type == PV_REF)
        cnum = ovCopyRef(d, src, map, cont->u.ref.num, 0);
    else if (cont && cont->type == PV_ARRAY && cont->u.arr.n > 0 &&
             cont->u.arr.v[0]->type == PV_REF)
        cnum = ovCopyRef(d, src, map, cont->u.arr.v[0]->u.ref.num, 0);

    free(map);
    pdfFree(src);
    if (!cnum) { vFree(fonts); return NULL; }

    /*
     * 用 q /GS gs ... Q 把覆盖层内容包起来:
     *   - 透明度统一由外层 ExtGState /GS 控制 (覆盖层内不再自带 alpha);
     *   - q/Q 保证图形状态不泄漏到页面其它内容.
     */
    {
        Obj *co = d->objs[cnum];
        const uint8_t *body = co && co->newStream ? co->newStream : NULL;
        size_t blen = co && co->newStream ? co->newStreamLen : 0;
        static const char pre[] = "q\n/GS gs\n";
        static const char post[] = "\nQ\n";
        size_t total = sizeof(pre) - 1 + blen + sizeof(post) - 1;
        uint8_t *nb = (uint8_t *)malloc(total ? total : 1);
        if (!nb) { vFree(fonts); return NULL; }
        memcpy(nb, pre, sizeof(pre) - 1);
        if (body && blen) memcpy(nb + sizeof(pre) - 1, body, blen);
        memcpy(nb + sizeof(pre) - 1 + blen, post, sizeof(post) - 1);
        free(co->newStream);
        co->newStream = nb;
        co->newStreamLen = total;
        if (co->modVal && co->modVal->type == PV_DICT)
            dictSet(co->modVal, "Length",
                    ({ Value *x = vNew(PV_INT); x->u.i = (long long)total; x; }));
    }
    *contentNum = cnum;
    return fonts;
}

/* 创建字体对象, 返回对象号; cjk 决定 Type0/GBK 或 Helvetica */
static int makeFontObj(PdfDoc *d, int cjk)
{
    int i, num;
    Obj *o;
    Value *f;
    /* 复用已有水印字体 (重复保存不膨胀) */
    for (i = 1; i < d->objCap; i++) {
        Obj *q = d->objs[i];
        Value *qf = q ? (q->modVal ? q->modVal : q->val) : NULL;
        if (qf && qf->type == PV_DICT && dictGet(qf, "PWWMFont")) {
            int qCjk = dictGet(qf, "DescendantFonts") != NULL;
            if (qCjk == cjk) return i;
        }
    }
    num = newObjNum(d);
    o = objEnsure(d, num, 0);
    f = vNew(PV_DICT);
    dictSet(f, "Type", ({ Value *x = vNew(PV_NAME); x->u.name = _strdup("Font"); x; }));
    dictSet(f, "PWWMFont", ({ Value *x = vNew(PV_BOOL); x->u.b = 1; x; }));
    if (!cjk) {
        dictSet(f, "Subtype", ({ Value *x = vNew(PV_NAME); x->u.name = _strdup("Type1"); x; }));
        dictSet(f, "BaseFont", ({ Value *x = vNew(PV_NAME); x->u.name = _strdup("Helvetica"); x; }));
        dictSet(f, "Encoding", ({ Value *x = vNew(PV_NAME); x->u.name = _strdup("WinAnsiEncoding"); x; }));
    } else {
        Value *cid, *csi, *fd;
        dictSet(f, "Subtype", ({ Value *x = vNew(PV_NAME); x->u.name = _strdup("Type0"); x; }));
        dictSet(f, "BaseFont", ({ Value *x = vNew(PV_NAME); x->u.name = _strdup("STSong-Light"); x; }));
        dictSet(f, "Encoding", ({ Value *x = vNew(PV_NAME); x->u.name = _strdup("GBK-EUC-H"); x; }));
        cid = vNew(PV_DICT);
        dictSet(cid, "Type", ({ Value *x = vNew(PV_NAME); x->u.name = _strdup("Font"); x; }));
        dictSet(cid, "Subtype", ({ Value *x = vNew(PV_NAME); x->u.name = _strdup("CIDFontType0"); x; }));
        dictSet(cid, "BaseFont", ({ Value *x = vNew(PV_NAME); x->u.name = _strdup("STSong-Light"); x; }));
        csi = vNew(PV_DICT);
        dictSet(csi, "Registry", ({ Value *x = vNew(PV_STRING); x->u.str.b = (uint8_t *)_strdup("Adobe"); x->u.str.n = 5; x; }));
        dictSet(csi, "Ordering", ({ Value *x = vNew(PV_STRING); x->u.str.b = (uint8_t *)_strdup("GB1"); x->u.str.n = 3; x; }));
        dictSet(csi, "Supplement", ({ Value *x = vNew(PV_INT); x->u.i = 2; x; }));
        dictSet(cid, "CIDSystemInfo", csi);
        fd = vNew(PV_DICT);
        dictSet(fd, "Type", ({ Value *x = vNew(PV_NAME); x->u.name = _strdup("FontDescriptor"); x; }));
        dictSet(fd, "FontName", ({ Value *x = vNew(PV_NAME); x->u.name = _strdup("STSong-Light"); x; }));
        dictSet(fd, "Flags", ({ Value *x = vNew(PV_INT); x->u.i = 4; x; }));
        {
            Value *bb = vNew(PV_ARRAY);
            arrAdd(bb, ({ Value *x = vNew(PV_INT); x->u.i = -25; x; }));
            arrAdd(bb, ({ Value *x = vNew(PV_INT); x->u.i = -254; x; }));
            arrAdd(bb, ({ Value *x = vNew(PV_INT); x->u.i = 781; x; }));
            arrAdd(bb, ({ Value *x = vNew(PV_INT); x->u.i = 775; x; }));
            dictSet(fd, "FontBBox", bb);
        }
        dictSet(fd, "ItalicAngle", vNew(PV_INT));
        dictSet(fd, "Ascent", ({ Value *x = vNew(PV_INT); x->u.i = 750; x; }));
        dictSet(fd, "Descent", ({ Value *x = vNew(PV_INT); x->u.i = -250; x; }));
        dictSet(fd, "CapHeight", ({ Value *x = vNew(PV_INT); x->u.i = 750; x; }));
        dictSet(fd, "StemV", ({ Value *x = vNew(PV_INT); x->u.i = 56; x; }));
        dictSet(cid, "FontDescriptor", fd);
        {
            Value *arr = vNew(PV_ARRAY);
            arrAdd(arr, cid);
            dictSet(f, "DescendantFonts", arr);
        }
    }
    o->modVal = f;
    return num;
}

/* 创建 ExtGState (透明度) 对象 */
static int makeGsObj(PdfDoc *d, double opacity)
{
    int i, num;
    Obj *o;
    Value *f;
    /* 复用已有水印 ExtGState (透明度变化时原地更新) */
    for (i = 1; i < d->objCap; i++) {
        Obj *q = d->objs[i];
        if (!q) continue;
        Value *qf = q->modVal ? q->modVal : q->val;
        if (qf && qf->type == PV_DICT && dictGet(qf, "PWWMGS")) {
            if (!q->modVal) q->modVal = vCopy(q->val);
            dictSet(q->modVal, "CA", ({ Value *x = vNew(PV_REAL); x->u.r = opacity; x; }));
            dictSet(q->modVal, "ca", ({ Value *x = vNew(PV_REAL); x->u.r = opacity; x; }));
            return i;
        }
    }
    num = newObjNum(d);
    o = objEnsure(d, num, 0);
    f = vNew(PV_DICT);
    dictSet(f, "Type", ({ Value *x = vNew(PV_NAME); x->u.name = _strdup("ExtGState"); x; }));
    dictSet(f, "PWWMGS", ({ Value *x = vNew(PV_BOOL); x->u.b = 1; x; }));
    dictSet(f, "CA", ({ Value *x = vNew(PV_REAL); x->u.r = opacity; x; }));
    dictSet(f, "ca", ({ Value *x = vNew(PV_REAL); x->u.r = opacity; x; }));
    o->modVal = f;
    return num;
}

/*
 * 计算水印位置 (页面坐标 pt, 原点左下); GUI 预览与写 PDF 共用同一套公式.
 *
 * 网格模式把水印放在每个单元格的中心, 因此最外圈离页边至少有半格,
 * 不会切边; 标记点始终落在 [0,pw] x [0,ph] 内.
 */
int pdfWatermarkMarks(const WmParams *p, double pw, double ph,
                      double *x, double *y, int max)
{
    int n = 0;
    if (p->mode == WM_MODE_SINGLE) {
        if (max > 0) {
            double cx = p->ix * pw, cy = p->iy * ph;
            if (cx < 0) cx = 0;
            if (cx > pw) cx = pw;
            if (cy < 0) cy = 0;
            if (cy > ph) cy = ph;
            x[0] = cx; y[0] = cy; n = 1;
        }
    } else {
        int hb = p->hboxes > 0 ? p->hboxes : 1;
        int vb = p->vboxes > 0 ? p->vboxes : 1;
        double cellW = pw / hb, cellH = ph / vb;
        int ix, iy;
        /* 水印放在每个单元格的中心, 因此最外圈离页边半格, 不会切边 */
        for (iy = 0; iy < vb; iy++) {
            for (ix = 0; ix < hb; ix++) {
                if (n >= max) return n;
                x[n] = (ix + 0.5) * cellW;
                y[n] = (iy + 0.5) * cellH;
                n++;
            }
        }
    }
    return n;
}

/* 创建一个内容为给定字节的内容流对象, 返回对象号 */
static int makeRawStreamObj(PdfDoc *d, const char *content)
{
    int num = newObjNum(d);
    Obj *o = objEnsure(d, num, 0);
    Value *dict = vNew(PV_DICT);
    size_t n = strlen(content);
    o->newStream = (uint8_t *)malloc(n ? n : 1);
    memcpy(o->newStream, content, n);
    o->newStreamLen = n;
    dictSet(dict, "Length", ({ Value *x = vNew(PV_INT); x->u.i = (long long)n; x; }));
    o->modVal = dict;
    return num;
}

/* 创建水印内容流对象, 返回对象号 */

/*
 * 生成水印内容流. rot 为页面 /Rotate (0/90/180/270).
 *
 * /Rotate 表示页面在显示时旋转的角度. 水印应当在"显示方向"上正立,
 * 因此:
 *   - 网格布局用"显示后的尺寸" (90/270 时宽高互换);
 *   - 内容流开头加一个旋转矩阵, 把显示坐标映射回页面坐标系.
 */
/*
 * 当 ovData 非空时, 水印内容流与字体资源直接取自 MuPDF 生成的覆盖层
 * (字体已嵌入子集, 保证跨阅读器一致); outFonts 返回覆盖层的字体资源字典.
 * 否则回退到内置的"手工拼内容流 + 标准 CJK 字体"路径.
 */
static int makeContentObj(PdfDoc *d, const WmParams *p, int fontNum, int gsNum,
                          double pw, double ph, int rot, const char *encText,
                          double width1000,
                          const uint8_t *ovData, size_t ovLen,
                          Value **outFonts)
{
    if (outFonts) *outFonts = NULL;
    if (ovData && ovLen) {
        int cnum = 0;
        wchar_t e2[128];
        Value *fonts = overlayImport(d, ovData, ovLen, &cnum, e2, 128);
        if (fonts && cnum) {
            Obj *o2 = d->objs[cnum];
            Value *dd = vNew(PV_DICT);
            /* 标记为水印, 便于重复保存时剥离 */
            dictSet(dd, "PWWM", ({ Value *x = vNew(PV_BOOL); x->u.b = 1; x; }));
            if (o2 && o2->modVal && o2->modVal->type == PV_DICT)
                dictSet(o2->modVal, "PWWM",
                        ({ Value *x = vNew(PV_BOOL); x->u.b = 1; x; }));
            else if (o2) o2->modVal = dd;
            /*
             * 覆盖层是按"显示尺寸"生成的 (rot=90/270 时是横版), 而页面
             * MediaBox 仍是原始的竖版. 这里在最前面插入与手工路径相同的
             * 旋转矩阵, 把覆盖层的显示坐标映射回页面坐标系.
             * 注意 overlayImport 已经加过外层 q 与 /GS gs, 所以要把旋转矩阵
             * 插到那个 q 之后.
             */
            if (rot == 90 || rot == 180 || rot == 270) {
                char rc[96];
                /*
                 * 页面 MediaBox 为 pw x ph (未旋转), 覆盖层按显示尺寸生成.
                 * 下面把"显示坐标"映射回"页面坐标":
                 *   rot=90 : (dx,dy) -> (dy, ph-dx)  => 0 -1 1 0 0 ph
                 *   rot=270: (dx,dy) -> (pw-dy, dx)  => 0 1 -1 0 pw 0
                 *   rot=180: (dx,dy) -> (pw-dx, ph-dy)
                 */
                /*
                 * 覆盖层是在"显示坐标"(y 向下, DW x DH) 里画的.
                 * 这里把它映射回页面坐标 (y 向上, pw x ph):
                 *   rot=90 : 显示左上 -> 页面左上,   x 轴向右=页面向下
                 *   rot=180: 显示左上 -> 页面右下
                 *   rot=270: 显示左上 -> 页面左下
                 */
                /*
                 * 覆盖层内容是按"显示尺寸"(y 向下) 生成的; 这里映射回页面坐标.
                 * /Rotate 90 (顺时针显示) 时, 页面左下角成为显示左上角:
                 * 显示 (dx,dy) -> 页面 (dy, dx)
                 * /Rotate 270 时相反: 页面左上角成为显示左上角:
                 * 显示 (dx,dy) -> 页面 (pw-dy, ph-dx)
                 * /Rotate 180: 显示 (dx,dy) -> 页面 (pw-dx, ph-dy)
                 */
                if (rot == 90)
                    sprintf(rc, "0 1 -1 0 %.6f 0 cm\n", pw);
                else if (rot == 180)
                    sprintf(rc, "-1 0 0 -1 %.6f %.6f cm\n", pw, ph);
                else
                    sprintf(rc, "0 -1 1 0 0 %.6f cm\n", ph);
                if (o2 && o2->newStream) {
                    static const char pre[] = "q\n/GS gs\n";
                    size_t plen = sizeof(pre) - 1;
                    size_t rlen = strlen(rc);
                    size_t total = plen + rlen + o2->newStreamLen - plen;
                    uint8_t *nb;
                    if (o2->newStreamLen >= plen &&
                        memcmp(o2->newStream, pre, plen) == 0) {
                        total = plen + rlen + (o2->newStreamLen - plen);
                        nb = (uint8_t *)malloc(total ? total : 1);
                        if (nb) {
                            memcpy(nb, o2->newStream, plen);
                            memcpy(nb + plen, rc, rlen);
                            memcpy(nb + plen + rlen, o2->newStream + plen,
                                   o2->newStreamLen - plen);
                            free(o2->newStream);
                            o2->newStream = nb;
                            o2->newStreamLen = total;
                            if (o2->modVal && o2->modVal->type == PV_DICT)
                                dictSet(o2->modVal, "Length",
                                        ({ Value *x = vNew(PV_INT); x->u.i = (long long)total; x; }));
                        }
                    }
                }
            }
            if (outFonts) *outFonts = fonts;
            else vFree(fonts);
            return cnum;
        }
        if (fonts) vFree(fonts);
    }
    /* 显示尺寸 (旋转 90/270 时宽高互换) */
    double dw = (rot == 90 || rot == 270) ? ph : pw;
    double dh = (rot == 90 || rot == 270) ? pw : ph;
    int num = newObjNum(d);
    Obj *o = objEnsure(d, num, 0);
    Value *dict = vNew(PV_DICT);
    Buf cs;
    double ang = p->angle * M_PI / 180.0;
    dictSet(dict, "PWWM", ({ Value *x = vNew(PV_BOOL); x->u.b = 1; x; }));
    double csA = cos(ang), snA = sin(ang);
    double tw = width1000 / 1000.0 * p->size;    /* 文本宽度 pt */
    int i;

    memset(&cs, 0, sizeof(cs));
    /*
     * 关键: 先把 CTM 复位成单位矩阵.
     * 原页面内容流可能以未配对的 cm 结尾 (例如 test4.pdf 开头就是
     * "0.75 0 0 -0.75 0 841.92 cm" 且没有 q 包裹), 而 /Contents 数组的
     * 各段是拼接执行、共享图形状态的. 不复位的话水印会继承那个翻转矩阵,
     * 导致文字上下镜像.
     */
    bufStr(&cs, "q\n");
    /* 把 /Rotate 转成内容流里的 cm (显示坐标 -> 页面坐标) */
    if (rot == 90)
        bufFmt(&cs, "0 -1 1 0 0 %.6f cm\n", ph);
    else if (rot == 180)
        bufFmt(&cs, "-1 0 0 -1 %.6f %.6f cm\n", pw, ph);
    else if (rot == 270)
        bufFmt(&cs, "0 1 -1 0 %.6f 0 cm\n", pw);
    else
        bufStr(&cs, "1 0 0 1 0 0 cm\n");
    bufStr(&cs, "/GS gs\n");
    bufStr(&cs, "BT\n");
    bufFmt(&cs, "/Fw %.4g Tf\n", p->size);
    bufFmt(&cs, "%.4f %.4f %.4f rg\n", p->r, p->g, p->b);

    {
        double mx[256], my[256];
        int nm = pdfWatermarkMarks(p, dw, dh, mx, my, 256);
        /*
         * 文本沿旋转后的基线方向延伸, 纵向/横向跨度 = tw*|cos| / tw*|sin|,
         * 还有大致一个行高的字高. 计算包围盒后把起点夹到页面内,
         * 否则在窄页 (如 A5 发票) 上水印会被挤出页面外.
         */
        double dxT = tw * csA, dyT = tw * snA;   /* 基线向量 */
        double pad = p->size * 0.5;              /* 字高/行距余量 */
        double xlo = (dxT < 0 ? -dxT : 0) + pad;
        double xhi = (dxT > 0 ? dw - dxT : dw) - pad;
        double ylo = (dyT < 0 ? -dyT : 0) + pad;
        double yhi = (dyT > 0 ? dh - dyT : dh) - pad;
        if (xlo > xhi) xlo = xhi = (dw - tw * fabs(csA)) * 0.5;
        if (ylo > yhi) ylo = yhi = (dh - tw * fabs(snA)) * 0.5;
        for (i = 0; i < nm; i++) {
            double tx = mx[i] - tw / 2 * csA;
            double ty = my[i] - tw / 2 * snA;
            if (tx < xlo) tx = xlo;
            if (tx > xhi) tx = xhi;
            if (ty < ylo) ty = ylo;
            if (ty > yhi) ty = yhi;
            /* Tm 必须给出完整 6 个分量 (a b c d e f), 否则后续水印会丢旋转/位移 */
            bufFmt(&cs, "%.6f %.6f %.6f %.6f %.6f %.6f Tm\n",
                   csA, snA, -snA, csA, tx, ty);
            bufFmt(&cs, "(%s) Tj\n", encText);
        }
    }
    bufStr(&cs, "ET\nQ\n");

    dictSet(dict, "Length", ({ Value *x = vNew(PV_INT); x->u.i = (long long)cs.n; x; }));
    o->modVal = dict;
    o->newStream = cs.b;
    o->newStreamLen = cs.n;
    return num;
}

/* 把页面 /Resources 合并进水印字体与 ExtGState; 返回 0 成功 */
static int patchResources(PdfDoc *d, Value *pageDict, int fontNum, int gsNum,
                             Value *overlayFonts)
{
    Value *res = dictGet(pageDict, "Resources");
    Obj *ro = NULL;
    Value *rd = NULL;

    if (res && res->type == PV_REF) {
        ro = resolve(d, res);
        if (ro && !ro->modVal && ro->val) ro->modVal = vCopy(ro->val);
        rd = ro ? ro->modVal : NULL;
    } else if (res && res->type == PV_DICT) {
        /* 内联 Resources: 直接改页面副本 (pageDict 已是 modVal) */
        rd = res;
    }
    if (!rd) {
        /* 无 Resources (可能从父节点继承): 新建一个, 同时保留继承内容
           —— 为安全起见, 把父节点 Resources 复制到页面上再合并 */
        rd = vNew(PV_DICT);
        dictSet(pageDict, "Resources", rd);
    }
    {
        Value *fonts = dictGet(rd, "Font");
        if (fonts && fonts->type == PV_REF) {
            Obj *fo = resolve(d, fonts);
            if (fo && !fo->modVal && fo->val) fo->modVal = vCopy(fo->val);
            fonts = fo ? fo->modVal : NULL;
        }
        if (!fonts || fonts->type != PV_DICT) {
            fonts = vNew(PV_DICT);
            dictSet(rd, "Font", fonts);
        }
        {
            Value *fn = vNew(PV_REF);
            fn->u.ref.num = fontNum; fn->u.ref.gen = 0;
            dictSet(fonts, "Fw", fn);
        }
        /* 并入覆盖层带来的字体 (已嵌入子集) */
        if (overlayFonts && overlayFonts->type == PV_DICT) {
            int j;
            for (j = 0; j < overlayFonts->u.dict.n; j++) {
                Value *fv = vCopy(overlayFonts->u.dict.kv[j].val);
                dictSet(fonts, overlayFonts->u.dict.kv[j].key, fv);
            }
        }
    }
    {
        Value *gs = dictGet(rd, "ExtGState");
        if (gs && gs->type == PV_REF) {
            Obj *go = resolve(d, gs);
            if (go && !go->modVal && go->val) go->modVal = vCopy(go->val);
            gs = go ? go->modVal : NULL;
        }
        if (!gs || gs->type != PV_DICT) {
            gs = vNew(PV_DICT);
            dictSet(rd, "ExtGState", gs);
        }
        {
            Value *gn = vNew(PV_REF);
            gn->u.ref.num = gsNum; gn->u.ref.gen = 0;
            dictSet(gs, "GS", gn);
        }
    }
    return 0;
}

/* 统计文档中本工具写入的水印内容流对象数 */
int pdfCountWatermarks(PdfDoc *d)
{
    int i, n = 0;
    if (!d) return 0;
    for (i = 1; i < d->objCap; i++) {
        Obj *o = d->objs[i];
        Value *mv = o ? (o->modVal ? o->modVal : o->val) : NULL;
        if (mv && mv->type == PV_DICT &&
            dictGet(mv, "PWWM") && dictGet(mv, "PWWM")->u.b)
            n++;
    }
    return n;
}

/* 判断对象是否为本工具添加的水印内容流 */
static int objIsWatermark(PdfDoc *d, int num)
{
    Obj *o;
    Value *mv;
    if (num <= 0 || num >= d->objCap) return 0;
    o = d->objs[num];
    if (!o) return 0;
    mv = o->modVal ? o->modVal : o->val;
    return mv && mv->type == PV_DICT &&
           dictGet(mv, "PWWM") && dictGet(mv, "PWWM")->u.b;
}

int pdfApplyWatermark(PdfDoc *d, const WmParams *p, wchar_t *err, size_t errMax)
{
    int cjk, fontNum, gsNum, i;
    Buf enc;
    double width1000 = 0;

    if (err) err[0] = 0;
    if (!p->text || !*p->text) {
        if (err) wcscpy(err, L"水印文字为空。");
        return -1;
    }
    cjk = !isAsciiOnly(p->text);
    memset(&enc, 0, sizeof(enc));
    if (encodeText(p->text, cjk, &enc, &width1000) != 0) {
        if (err) wcscpy(err, L"水印文字无法编码 (请尝试常用文字)。");
        return -1;
    }
    enc.b = (uint8_t *)realloc(enc.b, enc.n + 1);
    enc.b[enc.n] = 0;

    fontNum = makeFontObj(d, cjk);
    gsNum = makeGsObj(d, p->opacity);

    for (i = 0; i < d->npages; i++) {
        int pnum = d->pages[i];
        Obj *po = objEnsure(d, pnum, 0);
        Value *pd;
        Value *contents, *newRef;
        int cnum;
        pwWatermark ovWm;
        Value *ovFonts;
        double tw2;

        if (!po->val || po->val->type != PV_DICT) {
            if (err) wcscpy(err, L"页面对象解析失败。");
            free(enc.b);
            return -1;
        }
        if (!po->modVal) po->modVal = vCopy(po->val);
        pd = po->modVal;

        /* 剥离旧水印内容流 (重复保存不叠加) */
        {
            Value *ct = dictGet(pd, "Contents");
            if (ct && (ct->type == PV_REF || ct->type == PV_ARRAY)) {
                Value *clean = vNew(PV_ARRAY);
                int j, kept = 0;
                if (ct->type == PV_REF) {
                    if (objIsWatermark(d, ct->u.ref.num))
                        d->objs[ct->u.ref.num]->skip = 1;  /* 旧水印对象不再写出 */
                    else { arrAdd(clean, vCopy(ct)); kept = 1; }
                } else {
                    for (j = 0; j < ct->u.arr.n; j++) {
                        Value *e = ct->u.arr.v[j];
                        if (e->type == PV_REF && objIsWatermark(d, e->u.ref.num)) {
                            d->objs[e->u.ref.num]->skip = 1;
                            continue;
                        }
                        arrAdd(clean, vCopy(e));
                        kept++;
                    }
                }
                if (kept == 0) {
                    vFree(clean);
                    dictSet(pd, "Contents", vNew(PV_NULL));
                } else if (kept == 1) {
                    Value *only = clean->u.arr.v[0];
                    free(clean->u.arr.v);   /* 只释放容器, 保留元素 */
                    free(clean);
                    dictSet(pd, "Contents", only);
                } else {
                    dictSet(pd, "Contents", clean);
                }
            }
        }

        /* 生成"嵌入字体"水印覆盖层 (UTF-8 文本, 显示尺寸坐标) */
        ovWm.data = NULL; ovWm.len = 0;
        ovFonts = NULL;
        {
            char u8[512];
            WmParams p2 = *p;
            wchar_t wtext[256];
            double mxs[256], mys[256];
            int nmark, k;
            double dw2 = (d->prot[i] == 90 || d->prot[i] == 270) ? d->ph[i] : d->pw[i];
            double dh2 = (d->prot[i] == 90 || d->prot[i] == 270) ? d->pw[i] : d->ph[i];
            double ang2 = p->angle * M_PI / 180.0;
            double csA2 = cos(ang2), snA2 = sin(ang2);
            wcsncpy(wtext, p->text, 255); wtext[255] = 0;
            WideCharToMultiByte(CP_UTF8, 0, wtext, -1, u8, sizeof(u8), NULL, NULL);
            tw2 = width1000 / 1000.0 * p->size;
            nmark = pdfWatermarkMarks(&p2, dw2, dh2, mxs, mys, 256);
            /* 与手工路径一致: 起点做半宽偏移, 并夹取到页面内 */
            {
                double dxT = tw2 * csA2, dyT = tw2 * snA2;
                double pad = p->size * 0.5;
                double xlo = (dxT < 0 ? -dxT : 0) + pad;
                double xhi = (dxT > 0 ? dw2 - dxT : dw2) - pad;
                double ylo = (dyT < 0 ? -dyT : 0) + pad;
                double yhi = (dyT > 0 ? dh2 - dyT : dh2) - pad;
                if (xlo > xhi) xlo = xhi = (dw2 - tw2 * fabs(csA2)) * 0.5;
                if (ylo > yhi) ylo = yhi = (dh2 - tw2 * fabs(snA2)) * 0.5;
                for (k = 0; k < nmark; k++) {
                    double tx = mxs[k] - tw2 / 2 * csA2;
                    double ty = mys[k] - tw2 / 2 * snA2;
                    if (tx < xlo) tx = xlo;
                    if (tx > xhi) tx = xhi;
                    if (ty < ylo) ty = ylo;
                    if (ty > yhi) ty = yhi;
                    mxs[k] = tx; mys[k] = ty;
                }
            }
            pwWmBuild(u8, dw2, dh2, mxs, mys, nmark, p->size, p->angle,
                      p->r, p->g, p->b, p->opacity, g_wmFontFile, &ovWm);
            if (getenv("PWDUMP") && ovWm.data) {
                FILE *df = fopen(getenv("PWDUMP"), "wb");
                if (df) { fwrite(ovWm.data, 1, ovWm.len, df); fclose(df); }
            }
            (void)0;
        }
        cnum = makeContentObj(d, p, fontNum, gsNum, d->pw[i], d->ph[i], d->prot[i],
                              (char *)enc.b, width1000,
                              ovWm.data, ovWm.len, &ovFonts);

        /*
         * /Contents 的顺序:  [q] + 原内容 + [Q] + 水印.
         *
         * 必须把原内容包在 q/Q 里: PDF 的 /Contents 数组各段是拼接执行、
         * 共享图形状态的. 有些生成器 (如 test4.pdf) 在内容流开头写了
         * "0.75 0 0 -0.75 0 841.92 cm" 却没有配套的 q/Q, 该矩阵会一直
         * 残留到我们的水印段, 把水印文字上下镜像.
         * 用一对 q/Q 把原内容包起来即可让图形状态在任何情况下都复位
         * (cm 是右乘, 所以不能靠水印段自己写 "1 0 0 1 0 0 cm" 抵消).
         */
        contents = dictGet(pd, "Contents");
        if (contents) {
            Value *arr = vNew(PV_ARRAY);
            int qnum = makeRawStreamObj(d, "q\n");
            int Qnum = makeRawStreamObj(d, "Q\n");
            Value *rq = vNew(PV_REF), *rQ = vNew(PV_REF);
            rq->u.ref.num = qnum; rq->u.ref.gen = 0;
            rQ->u.ref.num = Qnum; rQ->u.ref.gen = 0;
            arrAdd(arr, rq);
            if (contents->type == PV_ARRAY) {
                int j;
                for (j = 0; j < contents->u.arr.n; j++)
                    arrAdd(arr, vCopy(contents->u.arr.v[j]));
            } else if (contents->type == PV_REF) {
                arrAdd(arr, vCopy(contents));
            } else if (contents->type == PV_DICT) {
                /* 内联流: 搬成独立对象 */
                int onum = newObjNum(d);
                Obj *oo = objEnsure(d, onum, 0);
                Value *r1 = vNew(PV_REF);
                oo->modVal = vCopy(contents);
                if (po->rawStream) {
                    oo->newStream = (uint8_t *)malloc(po->rawStreamLen);
                    memcpy(oo->newStream, po->rawStream, po->rawStreamLen);
                    oo->newStreamLen = po->rawStreamLen;
                    dictSet(oo->modVal, "Length",
                            ({ Value *x = vNew(PV_INT); x->u.i = (long long)po->rawStreamLen; x; }));
                }
                r1->u.ref.num = onum; r1->u.ref.gen = 0;
                arrAdd(arr, r1);
            }
            arrAdd(arr, rQ);
            newRef = vNew(PV_REF);
            newRef->u.ref.num = cnum; newRef->u.ref.gen = 0;
            arrAdd(arr, newRef);
            dictSet(pd, "Contents", arr);
        } else {
            newRef = vNew(PV_REF);
            newRef->u.ref.num = cnum; newRef->u.ref.gen = 0;
            dictSet(pd, "Contents", newRef);
        }
        patchResources(d, pd, fontNum, gsNum, ovFonts);
        if (ovFonts) vFree(ovFonts);
        pwWmFree(&ovWm);
    }
    free(enc.b);
    return 0;
}

/* ================================================================== */
/* 序列化 / 保存                                                       */
/* ================================================================== */
static void bufFmt(Buf *v, const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    tmp[sizeof(tmp) - 1] = 0;
    bufStr(v, tmp);
}

static void serName(Buf *out, const char *s)
{
    bufByte(out, '/');
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c <= 0x20 || strchr("()<>[]{}/%", c))
            bufFmt(out, "#%02X", c);
        else
            bufByte(out, c);
    }
}
static void serStr(Buf *out, const uint8_t *b, size_t n)
{
    size_t i;
    bufByte(out, '<');
    for (i = 0; i < n; i++) bufFmt(out, "%02X", b[i]);
    bufByte(out, '>');
}

static void serVal(Buf *out, Value *v)
{
    int i;
    if (!v) { bufStr(out, "null"); return; }
    switch (v->type) {
    case PV_NULL: bufStr(out, "null"); break;
    case PV_BOOL: bufStr(out, v->u.b ? "true" : "false"); break;
    case PV_INT: bufFmt(out, "%lld", v->u.i); break;
    case PV_REAL:
        bufFmt(out, "%.10g", v->u.r);
        break;
    case PV_NAME: serName(out, v->u.name); break;
    case PV_STRING: serStr(out, v->u.str.b, v->u.str.n); break;
    case PV_REF: bufFmt(out, "%d %d R", v->u.ref.num, v->u.ref.gen); break;
    case PV_ARRAY:
        bufByte(out, '[');
        for (i = 0; i < v->u.arr.n; i++) {
            if (i) bufByte(out, ' ');
            serVal(out, v->u.arr.v[i]);
        }
        bufStr(out, "]");
        break;
    case PV_DICT:
        bufStr(out, "<<");
        for (i = 0; i < v->u.dict.n; i++) {
            bufByte(out, ' ');
            serName(out, v->u.dict.kv[i].key);
            bufByte(out, ' ');
            serVal(out, v->u.dict.kv[i].val);
        }
        bufStr(out, " >>");
        break;
    default: bufStr(out, "null"); break;
    }
}

/* ================================================================== */
/* 预览渲染: 解释页面内容流, 用 GDI 画到 32 位 DIB                        */
/* 支持: 文本、直线/曲线/矩形填充描边、JPEG/Flate 图像、Form XObject      */
/* ================================================================== */

/* 取流对象解码后数据 (malloc 缓冲, 调用方 free) */
/* ASCIIHexDecode: 十六进制文本 -> 字节 */
static int asciiHexDecode(const uint8_t *in, size_t inLen, Buf *out)
{
    int hi = -1;
    size_t i;
    for (i = 0; i < inLen; i++) {
        int c = in[i], v;
        if (c == '>') break;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else continue;
        if (hi < 0) hi = v;
        else { uint8_t b = (uint8_t)((hi << 4) | v); bufPut(out, &b, 1); hi = -1; }
    }
    if (hi >= 0) { uint8_t b = (uint8_t)(hi << 4); bufPut(out, &b, 1); }
    return 0;
}

/* ASCII85Decode: base85 文本 -> 字节 */
static int ascii85Decode(const uint8_t *in, size_t inLen, Buf *out)
{
    uint32_t t = 0;
    int n = 0;
    size_t i;
    for (i = 0; i < inLen; i++) {
        int c = in[i];
        if (c == '~' ) break;
        if (c == 'z' && n == 0) {
            static const uint8_t z[4] = { 0, 0, 0, 0 };
            bufPut(out, z, 4);
            continue;
        }
        if (c < '!' || c > 'u') continue;
        t = t * 85u + (uint32_t)(c - '!');
        if (++n == 5) {
            uint8_t b[4];
            b[0] = (uint8_t)(t >> 24); b[1] = (uint8_t)(t >> 16);
            b[2] = (uint8_t)(t >> 8); b[3] = (uint8_t)t;
            bufPut(out, b, 4);
            t = 0; n = 0;
        }
    }
    if (n > 0) {
        uint8_t b[4];
        int k;
        for (k = n; k < 5; k++) t = t * 85u + 84u;
        b[0] = (uint8_t)(t >> 24); b[1] = (uint8_t)(t >> 16);
        b[2] = (uint8_t)(t >> 8); b[3] = (uint8_t)t;
        bufPut(out, b, (size_t)(n - 1));
    }
    return 0;
}

/* 判断某个过滤器名是否是"图像编解码器"(交给 GDI+ / 专用解码器处理) */
static int isImageCodec(const char *n)
{
    return n && (strcmp(n, "DCTDecode") == 0 || strcmp(n, "DCT") == 0 ||
                 strcmp(n, "JPXDecode") == 0 || strcmp(n, "CCITTFaxDecode") == 0 ||
                 strcmp(n, "CCF") == 0 || strcmp(n, "JBIG2Decode") == 0);
}

/* 取过滤器名 (可能来自数组第 idx 项) */
static const char *filterNameAt(Value *f, int idx)
{
    if (!f) return NULL;
    if (f->type == PV_NAME) return idx == 0 ? f->u.name : NULL;
    if (f->type == PV_ARRAY && idx < f->u.arr.n &&
        f->u.arr.v[idx]->type == PV_NAME)
        return f->u.arr.v[idx]->u.name;
    return NULL;
}

/* 用 DecodeParms 对 Flate 解压结果做 PNG predictor 还原 */
static int flateDecodePredictor(const uint8_t *in, size_t inLen, Value *dp, Buf *out)
{
    Value *pv = dictGet(dp, "Predictor");
    Value *cv = dictGet(dp, "Columns");
    Value *cl = dictGet(dp, "Colors");
    Value *bp = dictGet(dp, "BitsPerComponent");
    int pred = (pv && pv->type == PV_INT) ? (int)pv->u.i : 1;
    int cols = (cv && cv->type == PV_INT) ? (int)cv->u.i : 1;
    int colors = (cl && cl->type == PV_INT) ? (int)cl->u.i : 1;
    int bpc = (bp && bp->type == PV_INT) ? (int)bp->u.i : 8;
    int rowLen;
    if (flateDecode(in, inLen, out) != 0) return -1;
    if (pred < 10) return 0;
    rowLen = (colors * bpc * cols + 7) / 8;   /* 每行字节数 */
    if (rowLen <= 0) return -1;
    return applyPredictor(out, rowLen, pred);
}
/*
 * 解码流为图像/内容数据.
 * 依次应用 Flate/ASCIIHex/ASCII85/LZW 等"压缩类"过滤器;
 * 遇到图像编解码器 (DCT/CCITT/JPX/JBIG2) 时停止, 由调用方处理.
 * effFilter 返回停止处的编码器名 (无则 NULL).
 */
static int getStreamDataEx(PdfDoc *d, Obj *o, uint8_t **out, size_t *outLen,
                           const char **effFilter)
{
    Value *dict = o->modVal ? o->modVal : o->val;
    const uint8_t *raw;
    size_t rawLen;
    Buf cur;
    int idx = 0;
    Value *f;
    (void)d;
    if (effFilter) *effFilter = NULL;
    if (o->newStream) { raw = o->newStream; rawLen = o->newStreamLen; }
    else if (o->rawStream) { raw = o->rawStream; rawLen = o->rawStreamLen; }
    else return -1;
    f = (dict && dict->type == PV_DICT) ? dictGet(dict, "Filter") : NULL;

    memset(&cur, 0, sizeof(cur));
    bufPut(&cur, raw, rawLen);

    for (;;) {
        const char *fn = filterNameAt(f, idx);
        Buf nx;
        if (!fn) break;
        if (isImageCodec(fn)) {
            if (effFilter) *effFilter = fn;
            break;
        }
        memset(&nx, 0, sizeof(nx));
        if (strcmp(fn, "FlateDecode") == 0 || strcmp(fn, "Fl") == 0) {
            /* PNG predictor 处理 */
            Value *dp = dictGet(dict, "DecodeParms");
            if (dp && dp->type == PV_ARRAY && idx < dp->u.arr.n)
                dp = resolveRef(d, dp->u.arr.v[idx]);
            if (dp && dp->type == PV_DICT &&
                dictGet(dp, "Predictor"))
                flateDecodePredictor(cur.b, cur.n, dp, &nx);
            else if (flateDecode(cur.b, cur.n, &nx) != 0) { free(cur.b); return -1; }
        } else if (strcmp(fn, "ASCIIHexDecode") == 0 || strcmp(fn, "AHx") == 0) {
            asciiHexDecode(cur.b, cur.n, &nx);
        } else if (strcmp(fn, "ASCII85Decode") == 0 || strcmp(fn, "A85") == 0) {
            ascii85Decode(cur.b, cur.n, &nx);
        } else {
            /* 未知压缩器: 原样传递 */
            free(cur.b);
            return -1;
        }
        free(cur.b);
        cur = nx;
        idx++;
    }
    *out = cur.b ? cur.b : (uint8_t *)malloc(1);
    *outLen = cur.n;
    return 0;
}

static int getStreamData(PdfDoc *d, Obj *o, uint8_t **out, size_t *outLen)
{
    return getStreamDataEx(d, o, out, outLen, NULL);
}

static Obj *resolveObj(PdfDoc *d, Value *v)
{
    if (v && v->type == PV_REF &&
        v->u.ref.num > 0 && v->u.ref.num < d->objCap)
        return d->objs[v->u.ref.num];
    return NULL;
}
static Value *resolveRef(PdfDoc *d, Value *v)
{
    Obj *o = resolveObj(d, v);
    if (o) return o->modVal ? o->modVal : o->val;
    return v;
}

/* 二维仿射矩阵乘法 (行向量: out = a * b, 先 b 后 a) */
static void xfMul(XFORM *out, const XFORM *a, const XFORM *b)
{
    XFORM t;
    t.eM11 = a->eM11 * b->eM11 + a->eM12 * b->eM21;
    t.eM12 = a->eM11 * b->eM12 + a->eM12 * b->eM22;
    t.eM21 = a->eM21 * b->eM11 + a->eM22 * b->eM21;
    t.eM22 = a->eM21 * b->eM12 + a->eM22 * b->eM22;
    t.eDx  = a->eDx * b->eM11 + a->eDy * b->eM21 + b->eDx;
    t.eDy  = a->eDx * b->eM12 + a->eDy * b->eM22 + b->eDy;
    *out = t;
}
static void xfSet(XFORM *xf, double a, double b, double c, double dd,
                  double e, double f)
{
    xf->eM11 = (FLOAT)a; xf->eM12 = (FLOAT)b;
    xf->eM21 = (FLOAT)c; xf->eM22 = (FLOAT)dd;
    xf->eDx  = (FLOAT)e; xf->eDy  = (FLOAT)f;
}

typedef struct {
    const char *base;
    int cid, gbk;
} FontInfo;

static int resolveFont(PdfDoc *d, Value *v, FontInfo *fi)
{
    Value *f = resolveRef(d, v);
    memset(fi, 0, sizeof(*fi));
    if (!f || f->type != PV_DICT) return -1;
    {
        Value *bn = dictGet(f, "BaseFont");
        if (bn && bn->type == PV_NAME) fi->base = bn->u.name;
    }
    if (dictGet(f, "DescendantFonts")) fi->cid = 1;
    if (fi->base && (strstr(fi->base, "STSong") || strstr(fi->base, "MSung")))
        fi->gbk = 1;
    return 0;
}

static const wchar_t *fontFaceFor(const char *base, int *bold, int *italic)
{
    *bold = 0; *italic = 0;
    if (base) {
        if (strstr(base, "Bold")) *bold = 1;
        if (strstr(base, "Italic") || strstr(base, "Oblique")) *italic = 1;
        if (strstr(base, "STSong") || strstr(base, "MSung")) return L"SimSun";
        if (strstr(base, "Heisei")) return L"MS Mincho";
        if (strstr(base, "HYGothic")) return L"MS Gothic";
        if (strstr(base, "SimSun")) return L"SimSun";
        if (strstr(base, "Times")) return L"Times New Roman";
        if (strstr(base, "Courier")) return L"Courier New";
        if (strstr(base, "Helvetica") || strstr(base, "Arial")) return L"Arial";
    }
    return L"Arial";
}

/* PDF 字符串 -> UTF-16 (malloc, 调用方 free), 返回字符数 */
static int pdfStrToWide(const uint8_t *s, size_t n, const FontInfo *fi,
                        wchar_t **out)
{
    wchar_t *w;
    int cnt, i;
    if (fi->cid) {
        if (n & 1) n--;
        cnt = (int)(n / 2);
        w = (wchar_t *)malloc(((size_t)cnt + 1) * sizeof(wchar_t));
        if (fi->gbk)
            cnt = MultiByteToWideChar(936, 0, (const char *)s, (int)n, w, cnt);
        else
            for (i = 0; i < cnt; i++)
                w[i] = (wchar_t)(((uint16_t)s[2*i] << 8) | s[2*i+1]);
        if (cnt < 0) cnt = 0;
        w[cnt] = 0;
        *out = w;
        return cnt;
    }
    w = (wchar_t *)malloc((n + 1) * sizeof(wchar_t));
    for (i = 0; i < (int)n; i++) {
        unsigned char c = s[i];
        w[i] = (c >= 0x80) ? (wchar_t)(c + 0xF00) : (wchar_t)c;
    }
    w[n] = 0;
    *out = w;
    return (int)n;
}

typedef struct {
    PdfDoc *d;
    HDC dc;
    XFORM base, ctm, cur;    /* base: PDF->像素; ctm: 用户空间; cur: 合成 */
    XFORM st[64]; int stN;
    COLORREF fc, sc;
    double lw;
    int hasFont; FontInfo fi; double fs;
    XFORM tlm, tm;
    double leading;
    double *pts; int npts, capPts; int open;
    int depth;
    const Value *res;
    int skipInline;          /* BI/ID/EI 内联图像跳过状态 */
} R;

static void pathPt(R *r, double x, double y)
{
    if (r->npts >= r->capPts) {
        r->capPts = r->capPts ? r->capPts * 2 : 32;
        r->pts = (double *)realloc(r->pts, (size_t)r->capPts * 2 * sizeof(double));
    }
    r->pts[r->npts * 2] = x;
    r->pts[r->npts * 2 + 1] = y;
    r->npts++;
}
static void pathReset(R *r) { r->npts = 0; r->open = 0; }
static void pathCubic(R *r, double x1, double y1, double x2, double y2,
                      double x3, double y3)
{
    double x0 = r->npts ? r->pts[(r->npts-1)*2] : 0.0;
    double y0 = r->npts ? r->pts[(r->npts-1)*2+1] : 0.0;
    int i;
    for (i = 1; i <= 8; i++) {
        double t = i / 8.0, u = 1 - t;
        double x = u*u*u*x0 + 3*u*u*t*x1 + 3*u*t*t*x2 + t*t*t*x3;
        double y = u*u*u*y0 + 3*u*u*t*y1 + 3*u*t*t*y2 + t*t*t*y3;
        pathPt(r, x, y);
    }
}

static void pathFill(R *r, int even)
{
    POINT *pp;
    int i;
    if (r->npts < 3) return;
    pp = (POINT *)malloc((size_t)r->npts * sizeof(POINT));
    for (i = 0; i < r->npts; i++) {
        pp[i].x = (LONG)(r->pts[i*2] + 0.5);
        pp[i].y = (LONG)(r->pts[i*2+1] + 0.5);
    }
    SetWorldTransform(r->dc, &r->cur);
    SetBkMode(r->dc, TRANSPARENT);
    SetTextColor(r->dc, r->fc);
    if (even) PolyPolygon(r->dc, (const POINT *)pp, &r->npts, 1);
    else Polygon(r->dc, pp, r->npts);
    free(pp);
}
static void pathStroke(R *r)
{
    POINT *pp;
    int i, wdev;
    if (r->npts < 2) return;
    pp = (POINT *)malloc((size_t)r->npts * sizeof(POINT));
    for (i = 0; i < r->npts; i++) {
        pp[i].x = (LONG)(r->pts[i*2] + 0.5);
        pp[i].y = (LONG)(r->pts[i*2+1] + 0.5);
    }
    SetWorldTransform(r->dc, &r->cur);
    SetBkMode(r->dc, TRANSPARENT);
    wdev = (int)(r->lw * (r->cur.eM11 > 0 ? r->cur.eM11 : -r->cur.eM11) + 0.5);
    if (wdev < 1) wdev = 1;
    {
        HPEN pen = CreatePen(PS_SOLID, wdev, r->sc);
        HPEN op = (HPEN)SelectObject(r->dc, pen);
        Polyline(r->dc, pp, r->npts);
        SelectObject(r->dc, op);
        DeleteObject(pen);
    }
    free(pp);
}

/*
 * 绘制一段文本并推进文本矩阵.
 * PDF 语义: 每次 show 之后, 文本矩阵要沿书写方向前进"已绘制宽度".
 * 若不推进, 连续的 Tj 会全部叠在同一位置 (表现为文字拥挤).
 */
static void textShow(R *r, const uint8_t *s, size_t n)
{
    wchar_t *w = NULL;
    int cnt, fh, bold, italic;
    XFORM xf;
    HFONT font, of;
    double ps, ts, ta, ca, sa, ox, oy;
    double advPdf = 0.0;      /* 已绘制宽度 (PDF 文本空间单位) */

    if (!r->hasFont || n == 0) return;
    cnt = pdfStrToWide(s, n, &r->fi, &w);
    if (cnt <= 0) { free(w); return; }

    /* 设备坐标下的文本原点 */
    ox = r->tm.eDx * r->cur.eM11 + r->tm.eDy * r->cur.eM21 + r->cur.eDx;
    oy = r->tm.eDx * r->cur.eM12 + r->tm.eDy * r->cur.eM22 + r->cur.eDy;
    /* 页面缩放 (忽略 y 翻转) */
    ps = sqrt(r->cur.eM11 * r->cur.eM11 + r->cur.eM21 * r->cur.eM21);
    /* 文本矩阵缩放与旋转角 (PDF 用户空间) */
    ts = sqrt(r->tm.eM11 * r->tm.eM11 + r->tm.eM12 * r->tm.eM12);
    if (ts <= 0) ts = 1.0;
    ta = atan2(r->tm.eM12, r->tm.eM11);
    ca = cos(-ta); sa = sin(-ta);

    /*
     * GDI 的 TextOut 在世界变换含负 eM22 (y 翻转) 时会把字形上下镜像,
     * 因此构造无翻转的正定变换: 屏幕 y 向下, PDF 逆时针 = 屏幕顺时针.
     */
    xf.eM11 = (FLOAT)(ca * ps);
    xf.eM12 = (FLOAT)(-sa * ps);
    xf.eM21 = (FLOAT)(sa * ps);
    xf.eM22 = (FLOAT)(ca * ps);
    xf.eDx = (FLOAT)ox;
    xf.eDy = (FLOAT)oy;
    SetGraphicsMode(r->dc, GM_ADVANCED);
    SetWorldTransform(r->dc, &xf);
    fh = (int)(r->fs * ps * ts + 0.5);
    if (fh < 1) fh = 1;

    {
        const wchar_t *face = fontFaceFor(r->fi.base, &bold, &italic);
        font = CreateFontW(-fh, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL,
                           (BOOL)italic, 0, 0, DEFAULT_CHARSET,
                           OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                           face);
    }
    of = (HFONT)SelectObject(r->dc, font);
    SetBkMode(r->dc, TRANSPARENT);
    SetTextColor(r->dc, r->fc);
    SetTextAlign(r->dc, TA_LEFT | TA_TOP);
    TextOutW(r->dc, 0, (int)(-fh * 0.8), w, cnt);

    /* 测量宽度并换算回 PDF 文本空间 (用于推进 tm) */
    {
        SIZE sz;
        if (GetTextExtentPoint32W(r->dc, w, cnt, &sz) && sz.cx > 0) {
            double devPerPdf = ps * ts;
            if (devPerPdf > 1e-6) advPdf = (double)sz.cx / devPerPdf;
        }
    }
    SelectObject(r->dc, of);
    DeleteObject(font);
    free(w);

    /* 沿文本矩阵的 x 轴推进 (tm 与 tlm 同步) */
    if (advPdf != 0.0) {
        double dx = advPdf * r->tm.eM11;
        double dy = advPdf * r->tm.eM12;
        r->tm.eDx += dx;
        r->tm.eDy += dy;
        r->tlm.eDx += dx;
        r->tlm.eDy += dy;
    }
}

/* 仅按 TJ 数值调整推进文本矩阵 (单位: 1/1000 em) */
static void textAdvanceScaled(R *r, double amount1000)
{
    double adv = -amount1000 / 1000.0 * r->fs;
    double dx = adv * r->tm.eM11;
    double dy = adv * r->tm.eM12;
    r->tm.eDx += dx;
    r->tm.eDy += dy;
    r->tlm.eDx += dx;
    r->tlm.eDy += dy;
}

static ULONG_PTR g_gdiToken = 0;
static int ensureGdiplus(void)
{
    if (g_gdiToken) return 1;
    {
        /* 注意: GdiplusStartup 的 output 参数不能为 NULL (否则返回 InvalidParameter) */
        GdiplusStartupInput in;
        GdiplusStartupOutput out;
        in.GdiplusVersion = 1;
        in.DebugEventCallback = NULL;
        in.SuppressBackgroundThread = FALSE;
        in.SuppressExternalCodecs = FALSE;
        if (GdiplusStartup(&g_gdiToken, &in, &out) != Ok) {
            g_gdiToken = 0;
            return 0;
        }
    }
    return g_gdiToken != 0;
}

/* 当前变换下图像单位矩形的设备坐标包围盒 */
static int imgBBox(R *r, int W, int H, int *x0, int *y0, int *x1, int *y1)
{
    double xs[4] = { 0, 1, 1, 0 }, ys[4] = { 0, 0, 1, 1 };   /* image = unit square */
    int i, a0 = 1 << 30, a1 = -(1 << 30), b0 = 1 << 30, b1 = -(1 << 30);
    for (i = 0; i < 4; i++) {
        double px = xs[i] * r->cur.eM11 + ys[i] * r->cur.eM21 + r->cur.eDx;
        double py = xs[i] * r->cur.eM12 + ys[i] * r->cur.eM22 + r->cur.eDy;
        if (px < a0) a0 = (int)px;
        if (px > a1) a1 = (int)px;
        if (py < b0) b0 = (int)py;
        if (py > b1) b1 = (int)py;
    }
    *x0 = a0; *y0 = b0; *x1 = a1; *y1 = b1;
    return (a1 - a0) >= 2 && (b1 - b0) >= 2;
}

static void xobjImage(R *r, Obj *o, Value *xo)
{
    Value *wv = dictGet(xo, "Width"), *hv = dictGet(xo, "Height");
    Value *f = dictGet(xo, "Filter"), *cs = dictGet(xo, "ColorSpace");
    Value *bpc = dictGet(xo, "BitsPerComponent"), *im = dictGet(xo, "ImageMask");
    int W = (wv && (wv->type == PV_INT)) ? (int)wv->u.i : 0;
    int H = (hv && (hv->type == PV_INT)) ? (int)hv->u.i : 0;
    int isJpeg = 0, isFlate = 0;
    int x0, y0, x1, y1;

    if (W < 2 || H < 2 || W > 8192 || H > 8192) return;
    if (!imgBBox(r, W, H, &x0, &y0, &x1, &y1)) return;

    /* 解码过滤器链 (Flate/ASCIIHex/...), 得到有效编码器 */
    {
        uint8_t *probe = NULL; size_t plen = 0;
        const char *eff = NULL;
        if (getStreamDataEx(r->d, o, &probe, &plen, &eff) == 0) {
            if (eff) {
                if (strcmp(eff, "DCTDecode") == 0 || strcmp(eff, "DCT") == 0) isJpeg = 1;
            } else {
                isFlate = 1;    /* 链已全部解压: 得到原始像素 */
            }
            if (probe) free(probe);
        } else {
            isFlate = 1;
        }
        (void)f;
    }


    if (isJpeg && ensureGdiplus()) {
        uint8_t *data = NULL; size_t dlen = 0;
        int gsr = getStreamData(r->d, o, &data, &dlen);
        if (gsr == 0 && dlen > 4) {
            IStream *stm = NULL;
            HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, dlen);
            if (h) {
                memcpy(GlobalLock(h), data, dlen);
                GlobalUnlock(h);
                /* fDeleteOnRelease=TRUE: 流销毁时自动释放 h */
                if (CreateStreamOnHGlobal(h, TRUE, &stm) == S_OK) {
                    GpImage *img = NULL;
                    GdipLoadImageFromStream(stm, &img);
                    if (img) {
                        GpGraphics *g = NULL;
                        /* 画到目标 DC 上 (而非图像自身的 Graphics) */
                        if (GdipCreateFromHDC(r->dc, &g) == Ok && g) {
                            GdipDrawImageRectI(g, img, x0, y0,
                                               x1 - x0, y1 - y0);
                            GdipDeleteGraphics(g);
                        }
                        GdipDisposeImage(img);
                    }
                    stm->lpVtbl->Release(stm);
                }
            }
            free(data);
        }
        return;
    }

    if (im && im->type == PV_BOOL && im->u.b) {
        /* 1 位图像掩码 (含 CCITT 传真) */
        size_t bytes = ((size_t)W + 7) / 8 * (size_t)H;
        uint8_t *data = NULL; size_t dlen = 0;
        const char *eff = NULL;
        int owned = 0;

        if (getStreamDataEx(r->d, o, &data, &dlen, &eff) != 0) {
            if (data) free(data);
            return;
        }
        if (eff && (strcmp(eff, "CCITTFaxDecode") == 0 || strcmp(eff, "CCF") == 0)) {
            Value *dp = dictGet(xo, "DecodeParms");
            int K = -1, cols = W;
            if (dp && dp->type == PV_DICT) {
                Value *kv = dictGet(dp, "K");
                Value *cv = dictGet(dp, "Columns");
                if (kv && kv->type == PV_INT) K = (int)kv->u.i;
                if (cv && cv->type == PV_INT && (int)cv->u.i > 0) cols = (int)cv->u.i;
            }
            {
                int ob = 0;
                uint8_t *fb = ccittDecode(data, dlen, K, cols, H, &ob);
                free(data);
                if (!fb) return;
                data = fb; dlen = (size_t)ob; owned = 1;
            }
        }
        if (dlen < bytes) { free(data); return; }
        {
            BITMAPINFO bi;
            void *bits = NULL;
            HBITMAP hb;
            HDC sdc;
            memset(&bi, 0, sizeof(bi));
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = W;
            bi.bmiHeader.biHeight = -H;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 1;
            bi.bmiHeader.biCompression = BI_RGB;
            /* image mask: 1 -> 用当前填充色 (这里用黑), 0 -> 透明 (画成白) */
            {
                RGBQUAD q0, q1;
                Value *dv = dictGet(xo, "Decode");
                int invert = 0;
                if (dv && dv->type == PV_ARRAY && dv->u.arr.n >= 2) {
                    Value *a = dv->u.arr.v[0];
                    if (a && a->type == PV_INT && a->u.i == 1) invert = 1;
                }
                if (invert) {
                    q0 = (RGBQUAD){ 0, 0, 0, 0 };          /* 0 -> 黑 */
                    q1 = (RGBQUAD){ 255, 255, 255, 0 };    /* 1 -> 白 */
                } else {
                    q0 = (RGBQUAD){ 255, 255, 255, 0 };    /* 0 -> 白 */
                    q1 = (RGBQUAD){ 0, 0, 0, 0 };          /* 1 -> 黑 */
                }
                bi.bmiColors[0] = q0;
                bi.bmiColors[1] = q1;
            }
            hb = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
            if (hb && bits) {
                memcpy(bits, data, bytes);
                sdc = CreateCompatibleDC(NULL);
                SelectObject(sdc, hb);
                SetStretchBltMode(r->dc, COLORONCOLOR);
                StretchBlt(r->dc, x0, y0, x1 - x0, y1 - y0,
                           sdc, 0, 0, W, H, SRCCOPY);
                DeleteDC(sdc);
            }
            if (hb) DeleteObject(hb);
        }
        (void)owned;
        free(data);
        return;
    }

    if (isFlate) {
        int c = 3;
        if (cs && cs->type == PV_NAME) {
            if (strcmp(cs->u.name, "DeviceGray") == 0) c = 1;
            else if (strcmp(cs->u.name, "DeviceCMYK") == 0) c = 4;
        }
        if (bpc && bpc->type == PV_INT && bpc->u.i != 8) return;
        {
            uint8_t *data; size_t dlen;
            BITMAPINFO bi;
            void *bits = NULL;
            HBITMAP hb;
            uint32_t *px;
            size_t need = (size_t)W * H * (size_t)c;
            size_t j;
            if (getStreamData(r->d, o, &data, &dlen) != 0) return;
            memset(&bi, 0, sizeof(bi));
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = W;
            bi.bmiHeader.biHeight = -H;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            hb = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
            if (hb && bits && dlen >= need) {
                px = (uint32_t *)bits;
                for (j = 0; j < (size_t)W * H; j++) {
                    if (c == 1) {
                        int g = data[j];
                        px[j] = 0xFF000000u | ((uint32_t)g << 16) |
                               ((uint32_t)g << 8) | (uint32_t)g;
                    } else if (c == 4) {
                        int cc = data[j*4], mm = data[j*4+1];
                        int yy = data[j*4+2], kk = data[j*4+3];
                        int rr = (255 - kk) * (255 - cc) / 255;
                        int gg = (255 - kk) * (255 - mm) / 255;
                        int bb = (255 - kk) * (255 - yy) / 255;
                        px[j] = 0xFF000000u | ((uint32_t)rr << 16) |
                               ((uint32_t)gg << 8) | (uint32_t)bb;
                    } else {
                        px[j] = 0xFF000000u | ((uint32_t)data[j*3] << 16) |
                               ((uint32_t)data[j*3+1] << 8) |
                               (uint32_t)data[j*3+2];
                    }
                }
            }
            {
                HDC sdc = CreateCompatibleDC(NULL);
                SelectObject(sdc, hb);
                SetStretchBltMode(r->dc, COLORONCOLOR);
                StretchBlt(r->dc, x0, y0, x1 - x0, y1 - y0,
                           sdc, 0, 0, W, H, SRCCOPY);
                DeleteDC(sdc);
            }
            if (hb) DeleteObject(hb);
            free(data);
        }
    }
}

/* 取下一个记号: 值返回 Value*, 关键字写入 kw 并返回 NULL */
static Value *nextTok(Cur *c, char *kw, size_t kwMax)
{
    char ch;
    for (;;) {
        while (c->p < c->end && (unsigned char)*c->p <= 32) c->p++;
        if (c->p < c->end && *c->p == '%') {
            while (c->p < c->end && *c->p != '\n') c->p++;
            continue;
        }
        if (c->p >= c->end) { kw[0] = 0; return NULL; }
        break;
    }
    ch = *c->p;
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')) {
        size_t n = 0;
        while (c->p < c->end &&
               !((unsigned char)*c->p <= 32) && *c->p != '/' &&
               *c->p != '(' && *c->p != ')' && *c->p != '[' &&
               *c->p != ']' && *c->p != '<' && *c->p != '>' && *c->p != '%')
            if (n + 1 < kwMax) kw[n++] = *c->p++;
        kw[n] = 0;
        return NULL;
    }
    return parseValue(c, 0);
}

static void runContent(R *r, const uint8_t *data, size_t len);

static void renderForm(R *r, Value *xo)
{
    Value *m = dictGet(xo, "Matrix"), *cont = dictGet(xo, "Contents");
    Value *res = dictGet(xo, "Resources");
    Buf b;
    XFORM oldCtm, oldCur;
    const Value *oldRes;
    int i;
    if (r->depth >= 8 || !cont) return;
    memset(&b, 0, sizeof(b));
    if (cont->type == PV_REF || cont->type == PV_ARRAY) {
        if (cont->type == PV_REF) {
            Obj *co = resolveObj(r->d, cont);
            uint8_t *sd; size_t sl;
            if (co && getStreamData(r->d, co, &sd, &sl) == 0) {
                bufPut(&b, sd, sl);
                free(sd);
            }
        } else {
            for (i = 0; i < cont->u.arr.n; i++) {
                Obj *co = resolveObj(r->d, cont->u.arr.v[i]);
                uint8_t *sd; size_t sl;
                if (co && getStreamData(r->d, co, &sd, &sl) == 0) {
                    bufPut(&b, sd, sl);
                    free(sd);
                }
            }
        }
    }
    oldCtm = r->ctm; oldCur = r->cur; oldRes = r->res;
    r->depth++;
    if (m && m->type == PV_ARRAY && m->u.arr.n >= 6) {
        XFORM cm;
        xfSet(&cm, m->u.arr.v[0]->u.r, m->u.arr.v[1]->u.r,
              m->u.arr.v[2]->u.r, m->u.arr.v[3]->u.r,
              m->u.arr.v[4]->u.r, m->u.arr.v[5]->u.r);
        xfMul(&r->ctm, &cm, &r->ctm);
        xfMul(&r->cur, &cm, &r->cur);
    }
    r->res = resolveRef(r->d, res);
    if (b.n) runContent(r, b.b, b.n);
    r->ctm = oldCtm; r->cur = oldCur; r->res = oldRes;
    r->depth--;
    free(b.b);
}

static Value *resGet(R *r, const char *section, const char *name)
{
    Value *v = r->res ? dictGet((Value *)r->res, section) : NULL;
    v = resolveRef(r->d, v);
    return v ? dictGet(v, name) : NULL;
}

/* 内容流解释器 */
static void runContent(R *r, const uint8_t *data, size_t len)
{
    Cur c;
    Value *ops[128];
    int opN = 0;
    char kw[64];
    Value *v;

    c.p = data;
    c.end = data + len;

    while (c.p < c.end) {
        v = nextTok(&c, kw, sizeof(kw));
        if (v) {
            if (opN < 128) ops[opN++] = v;
            else vFree(v);
            continue;
        }
        if (!kw[0]) break;

        if (r->skipInline) {
            if (!strcmp(kw, "EI")) r->skipInline = 0;
            opN = 0;
            continue;
        }

#define OPN(k) ((k) < opN ? (ops[k]->type == PV_REAL ? ops[k]->u.r : (double)ops[k]->u.i) : 0.0)
        if (!strcmp(kw, "q")) {
            if (r->stN < 64) { r->st[r->stN] = r->ctm; r->stN++; }
        } else if (!strcmp(kw, "Q")) {
            if (r->stN > 0) { r->ctm = r->st[--r->stN]; xfMul(&r->cur, &r->ctm, &r->base); }
        } else if (!strcmp(kw, "cm")) {
            XFORM cm;
            xfSet(&cm, OPN(0), OPN(1), OPN(2), OPN(3), OPN(4), OPN(5));
            xfMul(&r->ctm, &cm, &r->ctm);
            xfMul(&r->cur, &cm, &r->cur);
        } else if (!strcmp(kw, "w")) {
            r->lw = OPN(0);
        } else if (!strcmp(kw, "rg")) {
            r->fc = RGB((int)(OPN(0)*255), (int)(OPN(1)*255), (int)(OPN(2)*255));
        } else if (!strcmp(kw, "g")) {
            r->fc = RGB((int)(OPN(0)*255), (int)(OPN(0)*255), (int)(OPN(0)*255));
        } else if (!strcmp(kw, "k")) {
            double c_=OPN(0), m_=OPN(1), y_=OPN(2), k_=OPN(3);
            r->fc = RGB((int)((1-k_)*(1-c_)*255), (int)((1-k_)*(1-m_)*255),
                        (int)((1-k_)*(1-y_)*255));
        } else if (!strcmp(kw, "RG")) {
            r->sc = RGB((int)(OPN(0)*255), (int)(OPN(1)*255), (int)(OPN(2)*255));
        } else if (!strcmp(kw, "G")) {
            r->sc = RGB((int)(OPN(0)*255), (int)(OPN(0)*255), (int)(OPN(0)*255));
        } else if (!strcmp(kw, "K")) {
            double c_=OPN(0), m_=OPN(1), y_=OPN(2), k_=OPN(3);
            r->sc = RGB((int)((1-k_)*(1-c_)*255), (int)((1-k_)*(1-m_)*255),
                        (int)((1-k_)*(1-y_)*255));
        } else if (!strcmp(kw, "gs")) {
            if (opN > 0 && ops[0]->type == PV_NAME) {
                Value *g = resGet(r, "ExtGState", ops[0]->u.name);
                g = resolveRef(r->d, g);
                if (g && g->type == PV_DICT) {
                    Value *ca = dictGet(g, "ca");
                    if (ca && (ca->type == PV_REAL || ca->type == PV_INT)) {
                        double a = ca->type == PV_REAL ? ca->u.r : (double)ca->u.i;
                        if (a < 1.0) {
                            r->fc = RGB((int)(255 + (GetRValue(r->fc) - 255) * a),
                                        (int)(255 + (GetGValue(r->fc) - 255) * a),
                                        (int)(255 + (GetBValue(r->fc) - 255) * a));
                        }
                    }
                }
            }
        } else if (!strcmp(kw, "re")) {
            double x = OPN(0), y = OPN(1), ww = OPN(2), hh = OPN(3);
            pathReset(r);
            pathPt(r, x, y);
            pathPt(r, x + ww, y);
            pathPt(r, x + ww, y + hh);
            pathPt(r, x, y + hh);
        } else if (!strcmp(kw, "m")) {
            pathReset(r);
            pathPt(r, OPN(0), OPN(1));
            r->open = 1;
        } else if (!strcmp(kw, "l")) {
            pathPt(r, OPN(0), OPN(1));
            r->open = 1;
        } else if (!strcmp(kw, "c")) {
            pathCubic(r, OPN(0), OPN(1), OPN(2), OPN(3), OPN(4), OPN(5));
            r->open = 1;
        } else if (!strcmp(kw, "v")) {
            double x0 = r->npts ? r->pts[(r->npts-1)*2] : 0.0;
            double y0 = r->npts ? r->pts[(r->npts-1)*2+1] : 0.0;
            pathCubic(r, x0, y0, OPN(0), OPN(1), OPN(2), OPN(3));
            r->open = 1;
        } else if (!strcmp(kw, "y")) {
            double x1 = r->npts ? r->pts[(r->npts-1)*2] : 0.0;
            double y1 = r->npts ? r->pts[(r->npts-1)*2+1] : 0.0;
            pathCubic(r, OPN(0), OPN(1), x1, y1, OPN(2), OPN(3));
            r->open = 1;
        } else if (!strcmp(kw, "h")) {
            r->open = 0;
        } else if (!strcmp(kw, "f") || !strcmp(kw, "F")) {
            pathFill(r, 0);
            pathReset(r);
        } else if (!strcmp(kw, "f*")) {
            pathFill(r, 1);
            pathReset(r);
        } else if (!strcmp(kw, "S") || !strcmp(kw, "s")) {
            pathStroke(r);
            pathReset(r);
        } else if (!strcmp(kw, "B") || !strcmp(kw, "B*")) {
            pathFill(r, 0);
            pathStroke(r);
            pathReset(r);
        } else if (!strcmp(kw, "n") || !strcmp(kw, "W") || !strcmp(kw, "W*")) {
            pathReset(r);
        } else if (!strcmp(kw, "BI")) {
            r->skipInline = 1;   /* 内联图像: 跳过到 EI */
        } else if (!strcmp(kw, "BT")) {
            xfSet(&r->tlm, 1, 0, 0, 1, 0, 0);
            r->tm = r->tlm;
        } else if (!strcmp(kw, "ET")) {
        } else if (!strcmp(kw, "Tf")) {
            if (opN >= 2 && ops[0]->type == PV_NAME &&
                (ops[1]->type == PV_REAL || ops[1]->type == PV_INT)) {
                Value *f = resGet(r, "Font", ops[0]->u.name);
                r->fs = ops[1]->type == PV_REAL ? ops[1]->u.r : (double)ops[1]->u.i;
                if (resolveFont(r->d, f, &r->fi) == 0) r->hasFont = 1;
                else r->hasFont = 0;
            }
        } else if (!strcmp(kw, "TL")) {
            r->leading = OPN(0);
        } else if (!strcmp(kw, "Tm")) {
            xfSet(&r->tm, OPN(0), OPN(1), OPN(2), OPN(3), OPN(4), OPN(5));
            r->tlm = r->tm;
        } else if (!strcmp(kw, "Td") || !strcmp(kw, "TD")) {
            XFORM m;
            xfSet(&m, 1, 0, 0, 1, OPN(0), OPN(1));
            xfMul(&r->tlm, &m, &r->tlm);
            r->tm = r->tlm;
            if (!strcmp(kw, "TD")) r->leading = -OPN(1);
        } else if (!strcmp(kw, "T*")) {
            XFORM m;
            xfSet(&m, 1, 0, 0, 1, 0, -r->leading);
            xfMul(&r->tlm, &m, &r->tlm);
            r->tm = r->tlm;
        } else if (!strcmp(kw, "Tj") || !strcmp(kw, "'")) {
            if (opN > 0 && ops[opN-1]->type == PV_STRING)
                textShow(r, ops[opN-1]->u.str.b, ops[opN-1]->u.str.n);
        } else if (!strcmp(kw, "\"")) {
            XFORM m;
            xfSet(&m, 1, 0, 0, 1, 0, -r->leading);
            xfMul(&r->tlm, &m, &r->tlm);
            r->tm = r->tlm;
            if (opN > 0 && ops[opN-1]->type == PV_STRING)
                textShow(r, ops[opN-1]->u.str.b, ops[opN-1]->u.str.n);
        } else if (!strcmp(kw, "TJ")) {
            if (opN > 0 && ops[opN-1]->type == PV_ARRAY) {
                int j;
                for (j = 0; j < ops[opN-1]->u.arr.n; j++) {
                    Value *e = ops[opN-1]->u.arr.v[j];
                    if (e->type == PV_STRING)
                        textShow(r, e->u.str.b, e->u.str.n);
                    else if (e->type == PV_INT)
                        textAdvanceScaled(r, (double)e->u.i);
                    else if (e->type == PV_REAL)
                        textAdvanceScaled(r, e->u.r);
                }
            }
        } else if (!strcmp(kw, "Do")) {
            if (opN > 0 && ops[0]->type == PV_NAME) {
                Value *xo = resGet(r, "XObject", ops[0]->u.name);
                Obj *xoObj = NULL;
                if (xo && xo->type == PV_REF) xoObj = resolveObj(r->d, xo);
                else if (xo) xoObj = NULL;
                xo = resolveRef(r->d, xo);
                if (xo && xo->type == PV_DICT) {
                    Value *sub = dictGet(xo, "Subtype");
                    if (sub && sub->type == PV_NAME) {
                        if (strcmp(sub->u.name, "Image") == 0 && xoObj)
                            xobjImage(r, xoObj, xo);
                        else if (strcmp(sub->u.name, "Form") == 0)
                            renderForm(r, xo);
                    }
                }
            }
        }
        /* 其余操作符 (颜色空间/平铺/注释等) 忽略 */
#undef OPN
        opN = 0;
    }
}

int pdfRenderPage(PdfDoc *d, int pageIndex, HBITMAP *bmp, int *w, int *h,
                  int maxDim)
{
    double pw, ph, s;
    int W, H;
    BITMAPINFO bi;
    void *bits = NULL;
    HBITMAP hb;
    HDC dc;
    R r;
    Buf content;
    Obj *po;
    Value *pd, *res;

    if (!d || pageIndex < 0 || pageIndex >= d->npages) return -1;
    if (maxDim < 100) maxDim = 1400;
    pw = d->pw[pageIndex]; ph = d->ph[pageIndex];
    if (pw < 1 || ph < 1) return -1;
    s = (double)maxDim / (pw > ph ? pw : ph);
    W = (int)(pw * s + 0.5);
    H = (int)(ph * s + 0.5);

    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;      /* 顶向下 */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    hb = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!hb || !bits) return -1;
    memset(bits, 0xFF, (size_t)W * H * 4);   /* 白色背景 (FillRect 在此版本 GDI 上会填黑) */
    dc = CreateCompatibleDC(NULL);
    SelectObject(dc, hb);
    SetGraphicsMode(dc, GM_ADVANCED);

    memset(&r, 0, sizeof(r));
    r.d = d;
    r.dc = dc;
    xfSet(&r.base, s, 0, 0, -s, 0, ph * s);
    xfSet(&r.ctm, 1, 0, 0, 1, 0, 0);
    r.cur = r.base;
    r.fc = r.sc = 0;
    r.lw = 1;
    xfSet(&r.tlm, 1, 0, 0, 1, 0, 0);
    r.tm = r.tlm;

    po = (d->pages[pageIndex] > 0 && d->pages[pageIndex] < d->objCap)
         ? d->objs[d->pages[pageIndex]] : NULL;
    pd = po ? (po->modVal ? po->modVal : po->val) : NULL;
    res = pd ? resolveRef(d, dictGet(pd, "Resources")) : NULL;
    r.res = res;

    memset(&content, 0, sizeof(content));
    if (pd) {
        Value *ct = dictGet(pd, "Contents");
        if (ct && ct->type == PV_REF) {
            Obj *co = resolveObj(d, ct);
            uint8_t *sd; size_t sl;
            if (co && getStreamData(d, co, &sd, &sl) == 0) {
                bufPut(&content, sd, sl);
                free(sd);
            }
        } else if (ct && ct->type == PV_ARRAY) {
            int i;
            for (i = 0; i < ct->u.arr.n; i++) {
                Obj *co = resolveObj(d, ct->u.arr.v[i]);
                uint8_t *sd; size_t sl;
                if (co && getStreamData(d, co, &sd, &sl) == 0) {
                    bufPut(&content, sd, sl);
                    free(sd);
                }
            }
        }
    }
    if (content.n) runContent(&r, content.b, content.n);
    free(content.b);
    if (r.pts) free(r.pts);

    /* 32 位 DIB 的 alpha 通道必须为 255, 否则 StretchBlt 按透明处理显示为黑 */
    {
        uint8_t *p = (uint8_t *)bits;
        size_t total = (size_t)W * H;
        size_t j;
        for (j = 3; j < total * 4; j += 4) p[j] = 255;
    }

    DeleteDC(dc);
    *bmp = hb;
    *w = W;
    *h = H;
    return 0;
}

int pdfWrite(PdfDoc *d, const wchar_t *path, wchar_t *err, size_t errMax)
{
    Buf out;
    HANDLE fh;
    DWORD written;
    int i, maxNum;
    long long *offsets;
    char *writtenFlag;
    const char *ver;
    size_t vlen;

    if (err) err[0] = 0;
    memset(&out, 0, sizeof(out));

    /* 版本号沿用原文件 */
    ver = "%PDF-1.5";
    if (d->len > 8 && memcmp(d->data, "%PDF-1.", 7) == 0) {
        static char vbuf[16];
        size_t k = 5;
        while (k < 12 && k < d->len && (isdigit(d->data[k]) || d->data[k] == '.')) k++;
        if (k > 6 && k < sizeof(vbuf)) {
            memcpy(vbuf, d->data, k);
            vbuf[k] = 0;
            ver = vbuf;
        }
    }
    bufStr(&out, ver);
    bufStr(&out, "\n%\xE2\xE3\xCF\xD3\n");

    maxNum = d->maxObjNum;
    offsets = (long long *)calloc((size_t)maxNum + 1, sizeof(long long));
    writtenFlag = (char *)calloc((size_t)maxNum + 1, 1);

    for (i = 1; i <= maxNum; i++) {
        Obj *o = (i < d->objCap) ? d->objs[i] : NULL;
        if (!o || o->skip) continue;
        if (!o->modVal && !o->val && !o->newStream) continue;

        offsets[i] = (long long)out.n;
        writtenFlag[i] = 1;
        bufFmt(&out, "%d %d obj\n", i, o->gen);

        if (o->newStream) {
            /* 新对象: 序列化字典 + 流 */
            serVal(&out, o->modVal ? o->modVal : o->val);
            bufStr(&out, "\nstream\n");
            bufPut(&out, o->newStream, o->newStreamLen);
            bufStr(&out, "\nendstream\n");
        } else if (o->modVal) {
            serVal(&out, o->modVal);
            bufStr(&out, "\n");
        } else if (o->inObjStm) {
            serVal(&out, o->val);
            bufStr(&out, "\n");
        } else if (o->rawStart && o->rawEnd > o->rawStart) {
            bufPut(&out, o->rawStart, (size_t)(o->rawEnd - o->rawStart));
            bufStr(&out, "\n");
        }
        bufStr(&out, "endobj\n");
    }

    /* 经典 xref 表 */
    {
        long long xrefPos = (long long)out.n;
        bufStr(&out, "xref\n");
        bufFmt(&out, "0 %d\n", maxNum + 1);
        bufStr(&out, "0000000000 65535 f \n");
        for (i = 1; i <= maxNum; i++) {
            if (writtenFlag[i])
                bufFmt(&out, "%010lld %05d n \n", offsets[i], d->objs[i]->gen);
            else
                bufStr(&out, "0000000000 65535 f \n");
        }
        bufStr(&out, "trailer\n");
        bufFmt(&out, "<< /Size %d /Root %d %d R >>\n", maxNum + 1,
               d->rootNum, d->rootGen);
        bufFmt(&out, "startxref\n%lld\n%%%%EOF\n", xrefPos);
    }

    free(offsets);
    free(writtenFlag);

    fh = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                      FILE_ATTRIBUTE_NORMAL, NULL);
    if (fh == INVALID_HANDLE_VALUE) {
        if (err) wcscpy(err, L"无法创建输出文件 (可能被占用或无权限)。");
        free(out.b);
        return -1;
    }
    if (!WriteFile(fh, out.b, (DWORD)out.n, &written, NULL) ||
        written != (DWORD)out.n) {
        if (err) wcscpy(err, L"写入文件失败。");
        CloseHandle(fh);
        free(out.b);
        return -1;
    }
    CloseHandle(fh);
    free(out.b);
    (void)vlen;
    return 0;
}
