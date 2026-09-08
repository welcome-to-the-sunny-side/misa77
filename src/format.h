// misa77 - A codec optimized for decompression throughput
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Shreyas Ghildiyal <nonadhocproblems@gmail.com>

#pragma once

#include <cstdint>

namespace misa77
{
    // (bit at this pos = 0) => light format, otherwise heavy
    inline constexpr uint8_t format_bit = 0;

    // (bit at this pos = 1) => compact format. Mutually exclusive with `format_bit`; both clear
    // means light. Used by levels in [config::compact_lb, config::max_level].
    inline constexpr uint8_t compact_bit = 2;
    static_assert(compact_bit != format_bit);

    // Width in bytes of several unconditional operations we perform through isa libs in both
    // formats.
    inline constexpr uint64_t vector_width = 32;

    // light format, used by levels in [config::min_level, config::heavy_lb)
    namespace light
    {
        // (`raw file size <= small_lim`) => small mode, we just write the raw bytes.
        inline constexpr uint32_t small_lim = 32;

        // When we find a match, we check `lookahead` positions ahead for a potentially better
        // match.
        inline constexpr uint64_t lookahead = 2;

        // The distance in bytes by which the start of the active hashtable window lags the pointer.
        // Don't set this too high, as there were a lot of matches for dis in [32, 512].
        inline constexpr uint64_t hashtab_lag = 32;
        static_assert(hashtab_lag >= vector_width);

        // If we're asked to copy a match after having processed `c` bytes of raw data, then the
        // match will begin in `[c - dis_lim - hashtab_lag, c - hashtab_lag)`.
        inline constexpr uint64_t dis_lim = (uint32_t(1) << 16);

        // We have exactly 2 bytes to indicate distance.
        static_assert(dis_lim <= (1 << 16));

        // The two distance bytes hold `dis - min_dis`, so the window is exactly [min_dis, max_dis].
        inline constexpr uint32_t min_dis = uint32_t(hashtab_lag + 1);
        inline constexpr uint32_t max_dis = uint32_t(hashtab_lag + dis_lim);
        static_assert(max_dis - min_dis == dis_lim - 1);
        static_assert(dis_lim - 1 <= UINT16_MAX);

        inline constexpr uint32_t min_match_len = 4;

        // `min_match_len >= 4` is needed for the compression bound to hold.
        static_assert(min_match_len >= 4);

        inline constexpr uint32_t max_match_len = 32;

        // One `cyccpy` call should deal with the entire match.
        static_assert(max_match_len <= vector_width);

        // Every length in [min_match_len, max_match_len] should be exactly representable.
        static_assert(max_match_len - min_match_len + 1 <= (uint32_t(1) << 5) - 1);

        inline constexpr uint64_t lit_lim = 7;
        static_assert(lit_lim == (uint64_t(1) << 3) - 1);

        // Lowerbound for the number of literals in the final raw suffix.
        // This MUST be >= `vector_width` because:
        // 1. During decompression, `cyccpy` performs unconditional writes in the destination
        // buffer, and we're using this suffix as padding that we can safely overwrite!!!
        // 2. During decompression, we perform unconditional reads from the source buffer, and this
        // literal suffix acts as padding that we can safely "over-read" into!!!
        inline constexpr uint32_t literal_suffix = 32;
        static_assert(literal_suffix >= vector_width);

        // Need enough source bytes to be able to guarantee that we can produce the appropriate raw
        // literal suffix.
        static_assert(literal_suffix <= small_lim + 1);

        // Unconditionally copied number of literal bytes in decompressor.
        inline constexpr uint64_t dec_literal_copy = 16;
        static_assert(literal_suffix >= dec_literal_copy);
    } // namespace light

    // heavy format, used by levels in [config::heavy_lb, config::max_level]
    namespace heavy
    {
        // a lot of things ahead have an analogue in the light format

        inline constexpr uint32_t small_lim = 64;

        // lut_t.len_of[64] contains a (match code) -> (match len) mapping that is clustered close
        // for smaller values of match code/len, and gets increasingly diffuse as match code
        // increases. The tables inside are constexpr evaluated.
        struct lut_t
        {
            uint8_t len_of[64];      // code -> (length (0 for code 0))
            uint8_t code_floor[256]; // length -> (code of largest codebook length <= it)
            uint8_t len_floor[256];  // length -> (that codebook length itself)

            // constexpr black magic thanks to Fable
            constexpr lut_t() : len_of(), code_floor(), len_floor()
            {
                auto len_at = [](uint32_t c) -> uint32_t
                {
                    return c == 0    ? 0
                           : c <= 29 ? c + 3
                           : c <= 45 ? 34 + 2 * (c - 30)
                           : c <= 61 ? 68 + 4 * (c - 46)
                           : c == 62 ? 160
                           : c == 63 ? 192
                                     : 0;
                };
                for (uint32_t c = 0; c < 64; ++c)
                    len_of[c] = uint8_t(len_at(c));

                // Build floor tables: for every length l in [0,255], the largest codebook
                // length <= l and its code (0 for l < 4).
                uint32_t cur_code = 0;
                uint32_t cur_len = 0;
                uint32_t c = 1;
                for (uint32_t l = 0; l < 256; ++l)
                {
                    while (c < 64 and len_at(c) <= l)
                    {
                        cur_code = c;
                        cur_len = len_at(c);
                        ++c;
                    }
                    code_floor[l] = uint8_t(cur_code);
                    len_floor[l] = uint8_t(cur_len);
                }
            }
        };
        inline constexpr lut_t LUT;

        inline constexpr uint32_t min_match_len = 4;
        inline constexpr uint32_t max_match_len = 192;
        static_assert(LUT.len_of[0] == 0);
        static_assert(LUT.len_of[1] == min_match_len);
        static_assert(LUT.len_of[63] == max_match_len);

        inline constexpr uint32_t literal_suffix = 64;

        // 1 MB window
        inline constexpr uint32_t win_bits = 20;
        inline constexpr uint64_t ndis = uint64_t(1) << win_bits;
        inline constexpr uint64_t hashtab_lag = 32;
        inline constexpr uint32_t min_dis = hashtab_lag + 1;
        inline constexpr uint32_t max_dis = uint32_t(min_dis + ndis - 1);

        inline constexpr uint64_t dec_literal_copy = 16;
        inline constexpr uint64_t lit_lim = 63;
        inline constexpr uint64_t dis_mask = (uint32_t(1) << win_bits) - 1;

        // The decoder's `cyccpy` chunks require `dis >= vector_width` to be alias-free
        static_assert(min_dis > vector_width);

        // `lit_lim` goes beyond the 6-bit literal-run field
        static_assert(lit_lim == (uint64_t(1) << 6) - 1);
        static_assert(dis_mask == ndis - 1);

        // position of condition bit in the flag byte
        // this bit indicates whether the decompressor should perform unconditional 64 byte copies
        // or not
        inline constexpr uint8_t cond_bit = 1;
        static_assert(format_bit != cond_bit);

        // here, unconditional decoder does two 32 byte copies, so we must have enough space at the
        // end
        static_assert(literal_suffix >= 2 * vector_width);
    } // namespace heavy

    // compact format, used by levels in [config::compact_lb, config::max_level]
    //
    // The grammar is LZ4-inspired (short distances, variable token size) but keeps the light
    // format's two-stream layout and unconditional-copy decode shape. Two token forms:
    //
    //   short (2 bytes): 0 LLL ll DD | dddddddd
    //       L = match_len - 4 (3 bits, so 4..11), l = literal_len (2 bits, 0..2 direct, 3 = escape),
    //       D|d = dis - 1 (10 bits, so dis 1..1024)
    //   long  (3 bytes): 1 MMMMM ll | dddddddd dddddddd
    //       M = match_len - 4 (5 bits, 0..28 -> 4..32; 29 = length extension, 30/31 reserved),
    //       l = literal_len (same 2-bit encoding), d = dis - 1 (16 bits, so dis 1..65536)
    //
    // Literal and match length escapes are 255-stuffed and use `lit_lim` / `len_ext_base` as the
    // base, exactly like the light format's literal-length escape.
    namespace compact
    {
        inline constexpr uint32_t small_lim = 32;

        inline constexpr uint32_t min_match_len = 4;

        // Dense (exactly representable) match lengths.
        inline constexpr uint32_t max_match_len = 32;

        // Longest match the length extension can encode.
        inline constexpr uint32_t max_long_match_len = 65536;

        // The short token's coverage. Matches in here at a near distance cost 2 bytes.
        inline constexpr uint32_t short_max_match_len = 11;
        inline constexpr uint32_t short_max_dis = 1024;
        static_assert(short_max_match_len <= max_match_len);
        static_assert(short_max_dis == (uint32_t(1) << 10));

        // Distances are stored as (dis - 1).
        inline constexpr uint32_t min_dis = 1;
        inline constexpr uint32_t max_dis = uint32_t(1) << 16;
        static_assert(max_dis <= (uint32_t(1) << 16));

        // Literal-length field is 2 bits in both token forms; the all-ones value is the escape.
        inline constexpr uint64_t lit_lim = 3;
        static_assert(lit_lim == (uint64_t(1) << 2) - 1);

        // Long-token match-length code: 0..28 => len 4..32, 29 => extension, 30/31 reserved.
        inline constexpr uint32_t len_ext_code = 29;
        static_assert(max_match_len - min_match_len == len_ext_code - 1);

        // First length that needs the extension.
        inline constexpr uint32_t len_ext_base = max_match_len + 1;

        // Same decode-side padding contract as the light format: the trailing literals absorb
        // unconditional over-reads/over-writes and the source buffer provides over-read slack.
        inline constexpr uint32_t literal_suffix = 32;
        inline constexpr uint64_t dec_literal_copy = 16;
        static_assert(literal_suffix >= vector_width);
        static_assert(literal_suffix >= dec_literal_copy);
        static_assert(max_long_match_len <= uint32_t(1) << 20);
    } // namespace compact
} // namespace misa77
