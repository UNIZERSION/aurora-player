#pragma once
#include <stdlib.h>
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_SPIRAM 4
#define MALLOC_CAP_DMA 8
static inline void *heap_caps_malloc(size_t n,unsigned cap){(void)cap;return malloc(n);}
static inline void *heap_caps_calloc(size_t n,size_t bytes,unsigned cap){(void)cap;return calloc(n,bytes);}
