// misa77 - A codec optimized for decompression throughput
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Shreyas Ghildiyal <nonadhocproblems@gmail.com>

#pragma once

#include "format.h"
#include "util.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace misa77
{
    // Copies `len` bytes from `out - dis` to `out` for the compact format. Unlike the light
    // format's `cyccpy`, `dis` may be smaller than `vector_width` (overlapping match) and `len`
    // may exceed `vector_width`.
    //
    // PRECONDITION: `dis >= 1` and `dis <= out - dst_base` (the source is already written), and
    // there are at least `max(len, vector_width)` writable bytes at `out`.
    template <class isa_lib>
    [[gnu::always_inline]]
    inline void compact_copy_match(uint8_t* out, uint32_t dis, uint64_t len)
    {
        if (dis >= vector_width and len <= vector_width)
        {
            // Alias-free single vector copy (writes a little past `len`, which the caller's
            // suffix slack absorbs).
            isa_lib::cyccpy(out - dis, dis);
        }
        else if (dis >= len)
        {
            // Non-overlapping.
            memcpy(out, out - dis, len);
        }
        else if (dis == 1)
        {
            memset(out, out[-1], len);
        }
        else
        {
            // Overlapping with period `dis >= 2`. Grow a correct `dis`-periodic window by
            // doubling, then copy it forward in ever-larger chunks. Every source range
            // [out - dis, out - dis + take) lies inside already-written output.
            uint64_t off = 0;
            uint64_t chunk = dis;
            while (off < len)
            {
                const uint64_t take = std::min(chunk, len - off);
                memcpy(out + off, out - dis, take);
                off += take;
                if (chunk < (uint64_t(1) << 20))
                    chunk <<= 1;
            }
        }
    }
} // namespace misa77
