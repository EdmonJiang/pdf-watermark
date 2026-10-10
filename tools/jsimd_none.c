/*
 * jsimd_none.c - libjpeg-turbo 非 SIMD 后端 (最小实现)
 *
 * 本构建不编译汇编 SIMD. 这里只提供被引用的 jsimd_* 符号.
 * 所有 "can_*" 返回 0, 使 libjpeg 走标量路径 (在 jdmaster/jcmaster 里判定).
 * 参数一律用 void* 以避免与头文件类型定义冲突.
 */

#define JBOOLEAN_DUMMY 0

int jsimd_can_rgb_ycc(void) { return 0; }
int jsimd_can_rgb_gray(void) { return 0; }
int jsimd_can_ycc_rgb(void) { return 0; }
int jsimd_can_ycc_rgb565(void) { return 0; }
int jsimd_c_can_null_convert(void) { return 0; }
int jsimd_can_h2v2_downsample(void) { return 0; }
int jsimd_can_h2v1_downsample(void) { return 0; }
int jsimd_can_h2v2_smooth_downsample(void) { return 0; }
int jsimd_can_h2v2_upsample(void) { return 0; }
int jsimd_can_h2v1_upsample(void) { return 0; }
int jsimd_can_int_upsample(void) { return 0; }
int jsimd_can_h2v2_fancy_upsample(void) { return 0; }
int jsimd_can_h2v1_fancy_upsample(void) { return 0; }
int jsimd_can_h1v2_fancy_upsample(void) { return 0; }
int jsimd_can_h2v2_merged_upsample(void) { return 0; }
int jsimd_can_h2v1_merged_upsample(void) { return 0; }
int jsimd_can_huff_encode_one_block(void) { return 0; }
int jsimd_can_encode_mcu_AC_first_prepare(void) { return 0; }
int jsimd_can_encode_mcu_AC_refine_prepare(void) { return 0; }
int jsimd_can_convsamp(void) { return 0; }
int jsimd_can_convsamp_float(void) { return 0; }
int jsimd_can_fdct_islow(void) { return 0; }
int jsimd_can_fdct_ifast(void) { return 0; }
int jsimd_can_fdct_float(void) { return 0; }
int jsimd_can_quantize(void) { return 0; }
int jsimd_can_quantize_float(void) { return 0; }
int jsimd_can_idct_islow(void) { return 0; }
int jsimd_can_idct_ifast(void) { return 0; }
int jsimd_can_idct_float(void) { return 0; }
int jsimd_can_idct_4x4(void) { return 0; }
int jsimd_can_idct_2x2(void) { return 0; }
int jsimd_can_idct_6x6(void) { return 0; }
int jsimd_can_idct_12x12(void) { return 0; }

/* 下面这些在 can_* 返回 0 时不会被调用, 但需要符号存在 */
void jsimd_rgb_ycc_convert(void) {}
void jsimd_rgb_gray_convert(void) {}
void jsimd_ycc_rgb_convert(void) {}
void jsimd_ycc_rgb565_convert(void) {}
void jsimd_c_null_convert(void) {}
void jsimd_h2v2_downsample(void) {}
void jsimd_h2v1_downsample(void) {}
void jsimd_h2v2_smooth_downsample(void) {}
void jsimd_h2v2_upsample(void) {}
void jsimd_h2v1_upsample(void) {}
void jsimd_int_upsample(void) {}
void jsimd_h2v2_fancy_upsample(void) {}
void jsimd_h2v1_fancy_upsample(void) {}
void jsimd_h1v2_fancy_upsample(void) {}
void jsimd_h2v2_merged_upsample(void) {}
void jsimd_h2v1_merged_upsample(void) {}
void *jsimd_huff_encode_one_block(void) { return 0; }
void jsimd_encode_mcu_AC_first_prepare(void) {}
int jsimd_encode_mcu_AC_refine_prepare(void) { return 0; }
void jsimd_convsamp(void) {}
void jsimd_convsamp_float(void) {}
void jsimd_fdct_islow(void) {}
void jsimd_fdct_ifast(void) {}
void jsimd_fdct_float(void) {}
void jsimd_quantize(void) {}
void jsimd_quantize_float(void) {}
void jsimd_idct_islow(void) {}
void jsimd_idct_ifast(void) {}
void jsimd_idct_float(void) {}
void jsimd_idct_4x4(void) {}
void jsimd_idct_2x2(void) {}
void jsimd_idct_6x6(void) {}
void jsimd_idct_12x12(void) {}
