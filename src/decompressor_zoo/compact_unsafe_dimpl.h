// misa77 - A codec optimized for decompression throughput
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Shreyas Ghildiyal <nonadhocproblems@gmail.com>

#pragma once

#include "decompressor_zoo/compact_match_copy.h"
#include "format.h"
#include "util.h"

#include <cstdint>
#include <cstring>

namespace misa77
{
    // Returns number of bytes written to `dst`, and 0 on failure.
    // `isa_lib` is ISA-dependent.
    // PRECONDITION: `src`/`src_size` correspond to a valid compact stream.
    template <class isa_lib>
    uint64_t compact_unsafe_dimpl(const uint8_t* __restrict src,
                                  uint64_t src_size,
                                  uint8_t* __restrict dst,
                                  uint64_t dst_cap)
    {
        using namespace compact;

        const uint64_t original_size = loadu8(src) & 0x00FF'FFFF'FFFF'FFFF;

        if (dst_cap < original_size)
            return 0;

        // Small source
        if (original_size <= small_lim)
        {
            if (original_size > 0)
                memcpy(dst, src + uint64_t(8), original_size);
            return original_size;
        }

        const uint64_t literal_suffix_cnt = loadu8(src + 8);
        const uint8_t* control = src + 16;
        const uint8_t* literals = src + src_size - literal_suffix_cnt;
        uint8_t* out = dst;

        while (control < literals)
        {
            const uint8_t tag = control[0];
            uint64_t lit_len;
            uint64_t match_len;
            uint32_t dis;

            if (tag & 0x80)
            {
                // Long token
                const uint32_t mcode = (tag >> 2) & 0x1F;
                lit_len = tag & 0x03;
                dis = uint32_t(loadu2(control + 1)) + 1;
                control += 3;

                if (mcode <= len_ext_code - 1)
                {
                    match_len = mcode + min_match_len;
                }
                else
                {
                    uint64_t v = 0;
                    while (true)
                    {
                        const uint8_t b = *control++;
                        v += b;
                        if (b != 255)
                            break;
                    }
                    match_len = len_ext_base + v;
                }
            }
            else
            {
                // Short token
                match_len = ((tag >> 4) & 0x07) + min_match_len;
                lit_len = (tag >> 2) & 0x03;
                dis = (uint32_t(tag & 0x03) << 8) | control[1];
                control += 2;
                dis += 1;
            }

            if (lit_len == lit_lim)
            {
                uint64_t v = 0;
                while (true)
                {
                    const uint8_t b = *control++;
                    v += b;
                    if (b != 255)
                        break;
                }
                lit_len = lit_lim + v;
            }

            literals -= lit_len;
            if (lit_len > dec_literal_copy) [[unlikely]]
            {
                isa_lib::copy32(out, literals);

                if (lit_len > vector_width) [[unlikely]]
                    memcpy(out + vector_width, literals + vector_width, lit_len - vector_width);
            }
            else
            {
                // Unconditional copy; the suffix slack in both buffers absorbs the overshoot.
                memcpy(out, literals, dec_literal_copy);
            }
            out += lit_len;

            compact_copy_match<isa_lib>(out, dis, match_len);
            out += match_len;
        }

        if (uint64_t(out - dst) != original_size - literal_suffix_cnt)
            return 0;

        // Literal suffix at the end
        memcpy(out, src + src_size - literal_suffix_cnt, literal_suffix_cnt);
        out += literal_suffix_cnt;

        return uint64_t(out - dst);
    }
} // namespace misa77
