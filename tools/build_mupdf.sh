#!/bin/sh
# build_mupdf.sh - 用 SumatraPDF 里 vendored 的第三方库构建 MuPDF 静态库 (libmupdf.a)
#
# 目标: 只构建 PDF 渲染所需的最小集合, 供 pdfwatermark 预览使用.
# 依赖源码来自:
#   sumatrapdf/ext/mupdf        (MuPDF 1.28.5 完整源码)
#   sumatrapdf/ext/a-*          (amalgamated 第三方库)
#   sumatrapdf/ext/libjpeg-turbo (完整 libjpeg-turbo 源码树)
set -e

TC="D:/SOFT/Embarcadero_Dev-Cpp_6.3_TDM-GCC_9.2_Portable/TDM-GCC-64/bin"
GCC="$TC/gcc.exe"
AR="$TC/ar.exe"

SUM="D:/Dev/pdf-watermark/sumatrapdf"
MU="$SUM/ext/mupdf"
EXT="$SUM/ext"
OUT="D:/Dev/pdf-watermark/mupdf_build"
OBJ="$OUT/obj"
ERR="$OUT/err"

rm -rf "$OBJ" "$ERR"
mkdir -p "$OBJ" "$ERR"

DEFS="-DFZ_ENABLE_PDF=1 -DFZ_ENABLE_SVG=1 -DFZ_ENABLE_XPS=1 \
-DFZ_ENABLE_JS=0 -DFZ_ENABLE_MD=0 -DFZ_ENABLE_BARCODE=0 \
-DFZ_ENABLE_HYPHEN=0 -DFZ_ENABLE_BROTLI=1 -DFZ_ENABLE_HTML=0 \
-DFZ_ENABLE_OCR=0 -DFZ_ENABLE_DOCX_OUTPUT=0 -DFZ_ENABLE_EPUB=0 \
-DFZ_ENABLE_MOBI=0 -DFZ_ENABLE_HTML_ENGINE=0 -DFZ_ENABLE_ARCHIVE=0 \
-DFZ_ENABLE_FB2=0 -DFZ_ENABLE_TXT=0 -DFZ_ENABLE_OFFICE=0 \
-DHAVE_LIBCRYPTO=0 -DTOFU_CJK_LANG -DFT_ERR_PREFIX=FT_Err_ \
-DUSE_JPIP -DOPJ_EXPORTS -DHAVE_LCMS2MT=1 -DOPJ_STATIC -DSHARE_JPEG"

INC="-I$OUT -I$MU/include -I$MU/source/fitz -I$MU/source/pdf \
-I$EXT/a-freetype/include -I$MU/scripts/freetype -I$EXT/a-brotli \
-I$EXT/a-harfbuzz -I$EXT/a-lcms2 -I$EXT/a-jbig2dec -I$EXT/a-openjpeg \
-I$EXT/libjpeg-turbo/src -I$EXT/a-zlib -I$EXT/a-extract \
-I$SUM/src/shared/mupdf"

CFLAGS="-O2 -w -DNDEBUG $DEFS $INC"
FT_DEFS='-DFT2_BUILD_LIBRARY -DFT_CONFIG_MODULES_H="slimftmodules.h" -DFT_CONFIG_OPTIONS_H="slimftoptions.h"'

pass=0; fail=0

cc() {  # cc <outname> <cfile> [extra-defs]
    if "$GCC" $CFLAGS $3 -c "$2" -o "$OBJ/$1.o" 2>"$ERR/$1.txt"; then
        pass=$((pass+1))
    else
        fail=$((fail+1))
        printf '  FAIL %s: %s\n' "$1" "$(grep -m1 -i 'fatal error\|error:' "$ERR/$1.txt" | head -c 160)"
    fi
}

echo "=== thirdparty (amalgamated) ==="
cc tp_zlib        "$EXT/a-zlib/zlib.c"
cc tp_freetype    "$EXT/a-freetype/freetype.c" "$FT_DEFS"
cc tp_jbig2dec    "$EXT/a-jbig2dec/jbig2dec.c"
cc tp_lcms2       "$EXT/a-lcms2/lcms2.c"
cc tp_openjpeg    "$EXT/a-openjpeg/openjpeg.c"
cc tp_brotli      "$EXT/a-brotli/brotli.c"
echo "  thirdparty: pass=$pass fail=$fail"

echo "=== libjpeg-turbo (DCTDecode 需要) ==="
JSRC="$EXT/libjpeg-turbo/src"
for f in jcapimin jcarith jccoefct jccolor jcdctmgr jchuff jcicc jcinit          jcmainct jcomapi jcparam jcphuff jcprepct jcsample jctrans jcmarker          jcmaster jdapimin jdarith jdcoefct jdcolor jddctmgr jdhuff jdicc          jdinput jdmarker jdmaster jdmerge jdphuff jdpostct jdsample          jerror jfdctflt jfdctfst jfdctint jidctflt jidctfst jidctint          jidctred jmemmgr jmemnobs jquant1 jquant2 jutils jdmainct \n         jdatadst jdatasrc jctrans jdtrans jcmarker jcmaster jdmaster \n         jcmainct jdmainct jcparam jcarith jdarith jchuff jdhuff \n         jdapistd jcapistd jddctmgr jdcolor jdpostct jdsample jdinput \n         jdmarker jdmaster jdmerge jdphuff jdsample jidctint jfdctint \n         jquant1 jquant2 jcmaster jdcoefct jcphuff \n         jcdiffct jddiffct jclhuff jdlhuff jclossls jdlossls \n         jpeg_nbits jaricom; do
    [ -f "$JSRC/$f.c" ] && cc "jpg_$f" "$JSRC/$f.c"
done
cc jxl_stub "$OUT/jxl_stub.c"
# jpeg_nbits_table 在源文件里是 HIDDEN 的, 用导出版本替换
cc jpeg_nbits_pub "$OUT/jpeg_nbits.c" "-I$EXT/libjpeg-turbo/src -DINCLUDE_JPEG_NBITS_TABLE=1"
cc jsimd_none "$OUT/jsimd_none.c"
cc noto_sumatra "$SUM/src/shared/mupdf/noto_sumatra.c"

echo "=== mupdf fitz ==="
for f in "$MU"/source/fitz/*.c; do
    b=$(basename "$f" .c)
    case "$b" in
        # 需要可选依赖 / 非 PDF 渲染功能, 跳过
        html-*|memento|deskew|skew|load-jxl|jmemcust|noto)
            continue ;;
    esac
    cc "fitz_$b" "$f"
done

echo "=== mupdf pdf ==="
for f in "$MU"/source/pdf/*.c; do
    b=$(basename "$f" .c)
    cc "pdf_$b" "$f"
done

echo "=== mupdf svg/xps (可选) ==="
for d in svg xps cbz; do
    [ -d "$MU/source/$d" ] || continue
    for f in "$MU"/source/$d/*.c; do
        [ -f "$f" ] || continue
        b=$(basename "$f" .c)
        cc "${d}_$b" "$f"
    done
done

echo "  total: pass=$pass fail=$fail"
echo "=== archiving ==="
rm -f "$OUT/libmupdf.a"
"$AR" rcs "$OUT/libmupdf.a" "$OBJ"/*.o
echo "OK: $OUT/libmupdf.a ($(ls "$OBJ"/*.o 2>/dev/null | wc -l) objects)"
