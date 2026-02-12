Heatshrink WASM build (for web UI upload compression)

This builds a small encoder-only WASM module for the web UI. It exports:
- hs_alloc
- hs_free
- hs_encode
- memory (WASM linear memory)

Suggested build command (Emscripten):

emcc heatshrink_wasm.c \
  ../../third_party/heatshrink/heatshrink_encoder.c \
  -I../../third_party/heatshrink \
  -O3 \
  -s EXPORTED_FUNCTIONS='[_hs_alloc,_hs_free,_hs_encode]' \
  -s EXPORTED_RUNTIME_METHODS='[]' \
  -s ALLOW_MEMORY_GROWTH=1 \
  -s MODULARIZE=0 \
  -s ENVIRONMENT=web \
  -o heatshrink.wasm

Copy the resulting heatshrink.wasm to:
- spiffs/heatshrink.wasm
