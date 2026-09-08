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
    // Guaranteed to terminate without OOB access for arbitrary input (safe mode).
    template <class isa_lib>
    uint64_t compact_safe_dimpl(const uint8_t* __restrict src,
                                uint64_t src_size,
                                uint8_t* __restrict dst,
                                uint64_t dst_cap)
    {
        using namespace compact;

        if (src_size < 8) [[unlikely]]
            return 0;

        const uint64_t original_size = loadu8(src) & 0x00FF'FFFF'FFFF'FFFF;

        if (dst_cap < original_size)
            return 0;

        // Small source
        if (original_size <= small_lim)
        {
            if (src_size < 8 + original_size) [[unlikely]]
                return 0;
            if (original_size > 0)
                memcpy(dst, src + uint64_t(8), original_size);
            return original_size;
        }

        if (src_size < 16) [[unlikely]]
            return 0;

        const uint64_t literal_suffix_cnt = loadu8(src + 8);

        if (literal_suffix_cnt < literal_suffix or literal_suffix_cnt > src_size - 16 or
            literal_suffix_cnt > original_size) [[unlikely]]
            return 0;

        const uint8_t* control = src + 16;
        const uint8_t* literals = src + src_size - literal_suffix_cnt;
        uint8_t* out = dst;

        const uint64_t loop_limit = original_size - literal_suffix_cnt;

        while (control < literals)
        {
            const uint8_t tag = control[0];
            uint64_t lit_len;
            uint64_t match_len;
            uint32_t dis;

            if (tag & 0x80)
            {
                // Long token
                if (control + 3 > literals) [[unlikely]]
                    return 0;
                const uint32_t mcode = (tag >> 2) & 0x1F;
                lit_len = tag & 0x03;
                dis = uint32_t(loadu2(control + 1)) + 1;
                control += 3;

                if (mcode <= len_ext_code - 1)
                {
                    match_len = mcode + min_match_len;
                }
                else if (mcode == len_ext_code)
                {
                    uint64_t v = 0;
                    while (true)
                    {
                        if (control >= literals) [[unlikely]]
                            return 0;
                        const uint8_t b = *control++;
                        v += b;
                        if (b != 255)
                            break;
                    }
                    match_len = len_ext_base + v;
                    if (match_len > max_long_match_len) [[unlikely]]
                        return 0;
                }
                else
                {
                    // 30/31 are reserved and never emitted
                    return 0;
                }
            }
            else
            {
                // Short token
                if (control + 2 > literals) [[unlikely]]
                    return 0;
                match_len = ((tag >> 4) & 0x07) + min_match_len;
                lit_len = (tag >> 2) & 0x03;
                dis = (uint32_t(tag & 0x03) << 8) | control[1];
                control += 2;
                dis += 1;
                if (dis > short_max_dis) [[unlikely]]
                    return 0;
            }

            if (lit_len == lit_lim)
            {
                uint64_t v = 0;
                while (true)
                {
                    if (control >= literals) [[unlikely]]
                        return 0;
                    const uint8_t b = *control++;
                    v += b;
                    if (b != 255)
                        break;
                }
                lit_len = lit_lim + v;
            }

            // Source under-read guard for the literals.
            if (lit_len > uint64_t(literals - src)) [[unlikely]]
                return 0;

            // Destination bounds: literals + match must fit before the raw suffix.
            const uint64_t written = uint64_t(out - dst);
            if (lit_len > loop_limit - written) [[unlikely]]
                return 0;
            if (match_len > loop_limit - written - lit_len) [[unlikely]]
                return 0;

            // Match source must be already-written output.
            if (dis > written + lit_len) [[unlikely]]
                return 0;

            literals -= lit_len;
            if (lit_len)
                memcpy(out, literals, lit_len);
            out += lit_len;

            compact_copy_match<isa_lib>(out, dis, match_len);
            out += match_len;
        }

        if (out != dst + loop_limit) [[unlikely]]
            return 0;

        memcpy(out, src + src_size - literal_suffix_cnt, literal_suffix_cnt);
        return original_size;
    }
} // namespace misa77
