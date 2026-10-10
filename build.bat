@echo off
rem ============================================================
rem  Build pdfwatermark.exe  (Windows, TDM-GCC 64-bit)
rem ============================================================
rem  Prerequisites
rem    ..\mupdf_build\libmupdf.a   built by tools\build_mupdf.sh
rem    .\mupdf_fonts\              built-in fonts (see README.md)
rem
rem  Override the compiler with:  set CC=D:\path\to\gcc.exe
rem ============================================================
setlocal

if "%CC%"=="" set CC=D:\SOFT\Embarcadero_Dev-Cpp_6.3_TDM-GCC_9.2_Portable\TDM-GCC-64\bin\gcc.exe
if "%MUPDF%"=="" set MUPDF=..\sumatrapdf\ext\mupdf
if "%MUPDF_LIB%"=="" set MUPDF_LIB=..\mupdf_build\libmupdf.a
set NOTO=..\sumatrapdf\src\shared\mupdf\noto_sumatra.c

if not exist "%CC%" (
    echo [ERROR] compiler not found: %CC%
    echo         set CC=^<path to gcc.exe^>
    exit /b 1
)
if not exist "%MUPDF_LIB%" (
    echo [ERROR] MuPDF library not found: %MUPDF_LIB%
    echo         build it first:  sh tools\build_mupdf.sh
    exit /b 1
)

"%CC%" -O2 -mwindows -municode -Wall ^
  -I. -Isrc -I"%MUPDF%\include" -I"..\sumatrapdf\src\shared\mupdf" ^
  -o pdfwatermark.exe ^
  src\pdfwatermark.c src\pdf.c src\ccitt.c src\mupdf_render.c src\mupdf_wm.c ^
  "%NOTO%" "%MUPDF_LIB%" ^
  -lcomctl32 -lgdi32 -lgdiplus -lole32 -lcomdlg32 -lshell32 ^
  -lwindowscodecs -luuid -lws2_32 -luser32 -lm

if errorlevel 1 (
    echo Build FAILED
    exit /b 1
)
echo Build OK: pdfwatermark.exe
endlocal
