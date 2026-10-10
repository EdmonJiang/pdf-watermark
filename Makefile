# Makefile for pdf-watermark (Windows / MinGW-w64)
#
# 依赖:
#   1) libmupdf.a   由 tools/build_mupdf.sh 生成
#   2) mupdf_fonts/ 内置字体 (见 README)
#
# 用法:
#   make            构建 pdfwatermark.exe
#   make clean      清理

CC      ?= gcc
MUPDF_DIR ?= ../sumatrapdf/ext/mupdf
NOTO_C    ?= ../sumatrapdf/src/shared/mupdf/noto_sumatra.c
MUPDF_LIB ?= ../mupdf_build/libmupdf.a

SRC = src/pdfwatermark.c src/pdf.c src/ccitt.c src/mupdf_render.c src/mupdf_wm.c
OBJ = $(SRC:.c=.o)

CFLAGS  = -O2 -mwindows -municode -Wall -I. -Isrc \
          -I$(MUPDF_DIR)/include -I$(MUPDF_DIR)/../src/shared/mupdf
LDFLAGS = -mwindows -municode
LIBS    = $(MUPDF_LIB) $(NOTO_C) \
          -lcomctl32 -lgdi32 -lgdiplus -lole32 -lcomdlg32 -lshell32 \
          -lwindowscodecs -luuid -lws2_32 -luser32 -lm

TARGET = pdfwatermark.exe

all: $(TARGET)

$(TARGET): $(SRC) $(MUPDF_LIB)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(NOTO_C) $(LIBS) $(LDFLAGS)

clean:
	rm -f $(TARGET) $(OBJ)

.PHONY: all clean
