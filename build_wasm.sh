#!/usr/bin/env bash
# Build "Engine Baru" (C++ -> WebAssembly) with Emscripten and embed it into the HTML.
# Needs the Emscripten SDK (emcc) on PATH:  https://emscripten.org/docs/getting_started/downloads.html
set -euo pipefail
cd "$(dirname "$0")"

emcc px_engine.cpp -O3 -std=c++17 -fno-exceptions -fno-rtti --no-entry \
  -sSTANDALONE_WASM=1 \
  -sFILESYSTEM=0 \
  -sALLOW_MEMORY_GROWTH=0 -sINITIAL_MEMORY=67108864 -sSTACK_SIZE=1048576 \
  -sERROR_ON_UNDEFINED_SYMBOLS=0 \
  -sEXPORTED_FUNCTIONS=_px_init,_px_io,_px_search,_px_perft,_px_clear \
  -o px_engine.wasm

python3 inject_wasm.py chesspx43_engine.html px_engine.wasm chesspx43_final.html
echo "done: chesspx43_final.html"
