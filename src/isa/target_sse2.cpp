// misa77 - A codec optimized for decompression throughput
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Shreyas Ghildiyal <nonadhocproblems@gmail.com>

// Baseline for x86-64 (SSE2 support is guaranteed in the ISA).

#include "compress_dispatch.h"
#include "compressor_zoo/heavy_optimal_cimpl.h"
#include "compressor_zoo/light_loose_cimpl.h"
#include "compressor_zoo/light_medium_cimpl.h"
#include "compressor_zoo/light_optimal_cimpl.h"
#include "decompress_dispatch.h"
#include "decompressor_zoo/heavy_safe_dimpl.h"
#include "decompressor_zoo/heavy_unsafe_dimpl.h"
#include "decompressor_zoo/safe_dimpl.h"
#include "decompressor_zoo/unsafe_dimpl.h"
#include "isa/lib_sse2.h"

#include <cstdint>

namespace misa77
{
    uint64_t compress_sse2(const uint8_t* __restrict src,
                           uint64_t src_size,
                           uint8_t* __restrict dst,
                           uint64_t dst_cap,
                           config cfg)
    {
        if (cfg.level == 0)
            return light_loose_cimpl<lib_sse2>(src, src_size, dst, dst_cap);
        else if (cfg.level == 1)
            return light_medium_cimpl<lib_sse2>(src, src_size, dst, dst_cap);
        else if (cfg.level == 2)
            return light_optimal_cimpl<lib_sse2>(src, src_size, dst, dst_cap);
        else if (cfg.level == 3)
            return heavy_optimal_cimpl<lib_sse2>(src, src_size, dst, dst_cap);
        return 0;
    }

    uint64_t decompress_sse2(const uint8_t* __restrict src,
                             uint64_t src_size,
                             uint8_t* __restrict dst,
                             uint64_t dst_cap,
                             dconfig dcfg)
    {
        if ((uint64_t(1) << format_bit) & src[7])
        {
            // heavy
            if (dcfg.safe)
                return heavy_safe_dimpl<lib_sse2>(src, src_size, dst, dst_cap);
            return heavy_unsafe_dimpl<lib_sse2>(src, src_size, dst, dst_cap);
        }

        // light
        if (dcfg.safe)
            return safe_dimpl<lib_sse2>(src, src_size, dst, dst_cap);
        return unsafe_dimpl<lib_sse2>(src, src_size, dst, dst_cap);
    }

} // namespace misa77
