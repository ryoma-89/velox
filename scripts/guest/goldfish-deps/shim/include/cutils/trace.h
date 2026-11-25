// Velox: gfxstream のゲストのトレースは既定で無効（Tracing.cpp は no-op）。ヘッダーのみ用意する。
#pragma once
#include <stdint.h>
#define ATRACE_TAG_GRAPHICS (1 << 1)
#define ATRACE_TAG_NEVER 0
static inline void atrace_begin(uint64_t tag, const char* name) { (void)tag; (void)name; }
static inline void atrace_end(uint64_t tag) { (void)tag; }
static inline uint64_t atrace_is_tag_enabled(uint64_t tag) { (void)tag; return 0; }
#define ATRACE_BEGIN(name) ((void)0)
#define ATRACE_END() ((void)0)
#define ATRACE_CALL() ((void)0)
#define ATRACE_NAME(name) ((void)0)
#define ATRACE_INT(name, value) ((void)0)
