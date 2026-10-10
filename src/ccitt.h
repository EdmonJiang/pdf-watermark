/*
 * ccitt.h - CCITT Group 3/4 (T.4/T.6) 传真解码 (纯 C)
 *
 * 用于渲染扫描件 PDF 中的 /CCITTFaxDecode 图像。
 * 输出为 1 位位图: 每行 Columns 位, MSB 在左, 行按字节对齐, 黑像素 = 1。
 */
#ifndef CCITT_H
#define CCITT_H

#include <stddef.h>
#include <stdint.h>

/*
 * 解码 CCITT 数据。
 *   in/inLen : 压缩数据
 *   K        : DecodeParms /K  (<0 = Group 4, >0 = Group 3 1D)
 *   columns  : 每行像素数 (/Columns)
 *   rows     : 行数 (/Height)
 *   outBytes : 输出缓冲区字节数
 * 返回 malloc 的位图 (调用方 free), 失败返回 NULL。
 */
uint8_t *ccittDecode(const uint8_t *in, size_t inLen, int K, int columns,
                     int rows, int *outBytes);

#endif
