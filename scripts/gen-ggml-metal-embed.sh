#!/bin/sh
# Generates ggml-metal-embed.s (same idea as ggml/src/ggml-metal/CMakeLists.txt with GGML_METAL_EMBED_LIBRARY).
# Comments in English per project convention.
set -e
METAL_DIR="${SRCROOT}/ThirdParty/whisper.cpp/ggml/src/ggml-metal"
COMMON="${SRCROOT}/ThirdParty/whisper.cpp/ggml/src/ggml-common.h"
IMPL="${METAL_DIR}/ggml-metal-impl.h"
SRC="${METAL_DIR}/ggml-metal.metal"
OUT_DIR="${SRCROOT}/ThirdParty/whisper.cpp/build/generated"
OUT_ASM="${OUT_DIR}/ggml-metal-embed.s"
TMP1="${OUT_DIR}/ggml-metal-embed.tmp.metal"
MERGED="${OUT_DIR}/ggml-metal-embed.metal"

mkdir -p "${OUT_DIR}"

sed -e "/__embed_ggml-common.h__/r ${COMMON}" -e "/__embed_ggml-common.h__/d" < "${SRC}" > "${TMP1}"
sed -e '/#include "ggml-metal-impl.h"/r '"${IMPL}" -e '/#include "ggml-metal-impl.h"/d' < "${TMP1}" > "${MERGED}"

{
  echo '.section __DATA,__ggml_metallib'
  echo '.globl _ggml_metallib_start'
  echo '_ggml_metallib_start:'
  echo ".incbin \"${MERGED}\""
  echo '.globl _ggml_metallib_end'
  echo '_ggml_metallib_end:'
} > "${OUT_ASM}"
