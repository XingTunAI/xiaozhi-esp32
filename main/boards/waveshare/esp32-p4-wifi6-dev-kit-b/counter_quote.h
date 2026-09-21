#pragma once
#include <stdint.h>

// Complete validated snapshot; amounts are integer fen, weight is milligrams.
struct CounterQuote {
    char name[181] = {};
    char material[49] = {};
    uint32_t weight_mg = 0;
    uint32_t price_fen = 0;
    uint64_t metal_fen = 0;
    uint64_t labor_fen = 0;
    uint64_t discount_fen = 0;
    uint64_t total_fen = 0;
};
