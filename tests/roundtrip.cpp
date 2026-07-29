// misa77 - A codec optimized for decompression throughput
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Shreyas Ghildiyal <nonadhocproblems@gmail.com>

#include "misa77/experimental.h"
#include "misa77/misa77.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

// Format constants the test deliberately targets. These mirror src/format.h;
// the test only uses the public API, so they are duplicated here on purpose (if
// the format changes, the interesting boundary points below move with them).
namespace fmt
{
    constexpr size_t small_lim = 32;     // <= this is stored raw
    constexpr size_t hashtab_lag = 32;   // min match distance the encoder emits
    constexpr size_t dis_lim = 1u << 16; // 65536; emitted dis must fit a uint16_t
    constexpr size_t vector_width = 32;  // cyccpy's unconditional store width
    constexpr uint8_t heavy_mask = 0x01; // flags byte (offset 7): bit clear = light, set = heavy
} // namespace fmt

namespace
{
    struct Stats
    {
        int total = 0;
        int passed = 0;
    };

    // A canary byte written into the slack past each buffer's logical end. After
    // a compress/decompress we re-check it: any change means the codec wrote
    // out of bounds (the over-write class the old test could not see).
    constexpr uint8_t kCanary = 0xCC;
    constexpr size_t kMargin = 64;

    bool canary_intact(const std::vector<uint8_t>& buf, size_t logical_end)
    {
        for (size_t i = logical_end; i < buf.size(); ++i)
            if (buf[i] != kCanary)
                return false;
        return true;
    }

    // Run one round-trip: misa77 compresses, then misa77 decompresses, with
    // bounds + canary + header checks on top of the byte-for-byte comparison.
    // The decompress destination is sized EXACTLY (decompressed_buffer_bound),
    // so the trailing canary catches any overshoot past the real output end.
    template <class CompressFn>
    bool run_one_with(const std::vector<uint8_t>& input,
                      const char* name,
                      Stats& stats,
                      misa77::config cfg,
                      CompressFn&& do_compress)
    {
        ++stats.total;

        // --- Compress ---------------------------------------------------------
        const uint64_t cbound = misa77::compress_bound(input.size(), cfg);
        std::vector<uint8_t> compressed(cbound + kMargin, kCanary);
        const uint64_t csz = do_compress(input.data(), input.size(), compressed.data(), cbound);
        if (csz == 0)
        {
            std::fprintf(stderr, "[%s] FAIL: compress returned 0\n", name);
            return false;
        }
        if (csz > cbound)
        {
            std::fprintf(stderr,
                         "[%s] FAIL: compress wrote %llu > compress_bound %llu\n",
                         name,
                         static_cast<unsigned long long>(csz),
                         static_cast<unsigned long long>(cbound));
            return false;
        }
        if (!canary_intact(compressed, cbound))
        {
            std::fprintf(stderr, "[%s] FAIL: compress overran compress_bound\n", name);
            return false;
        }

        // --- Header sanity ----------------------------------------------------
        if (misa77::decompressed_size(compressed.data()) != input.size())
        {
            std::fprintf(stderr, "[%s] FAIL: decompressed_size header wrong\n", name);
            return false;
        }

        // --- Format routing ---------------------------------------------------
        // The stream's flags byte must sit on the level's side of heavy_lb: light
        // below it, heavy at or above. Levels < heavy_lb being light is what
        // entitles them to the safe-accept branch below.
        const bool stream_heavy = (compressed[7] & fmt::heavy_mask) != 0;
        if (stream_heavy != (cfg.level >= misa77::config::heavy_lb))
        {
            std::fprintf(stderr,
                         "[%s] FAIL: stream format (%s) does not match level %d\n",
                         name,
                         stream_heavy ? "heavy" : "light",
                         int(cfg.level));
            return false;
        }

        // --- Decompress into an exactly-sized buffer --------------------------
        const uint64_t dcap = misa77::decompressed_buffer_bound(input.size());
        std::vector<uint8_t> out(dcap + kMargin, kCanary);
        const uint64_t rsz = misa77::decompress(compressed.data(), csz, out.data(), dcap);
        if (rsz != input.size())
        {
            std::fprintf(stderr,
                         "[%s] FAIL: roundtrip wrong size (got %llu, want %zu)\n",
                         name,
                         static_cast<unsigned long long>(rsz),
                         input.size());
            return false;
        }
        if (!canary_intact(out, dcap))
        {
            std::fprintf(
                stderr, "[%s] FAIL: decode wrote past the exact-size dst (overshoot bug)\n", name);
            return false;
        }
        if (input.size() > 0 && std::memcmp(out.data(), input.data(), input.size()) != 0)
        {
            size_t at = 0;
            while (at < input.size() && out[at] == input[at])
                ++at;
            std::fprintf(stderr, "[%s] FAIL: content mismatch at byte %zu\n", name, at);
            return false;
        }

        // --- Safe decoder: same stream, same bytes, through the checked path ---
        // Heavy streams have no safe decoder yet (deliberately deferred until the format
        // freezes); the documented contract is that safe mode REJECTS them with 0. Pin that.
        if (cfg.level >= misa77::config::heavy_lb)
        {
            std::vector<uint8_t> outs(dcap + kMargin, kCanary);
            const uint64_t rss = misa77::decompress(
                compressed.data(), csz, outs.data(), dcap, misa77::dconfig(true));
            if (rss != 0)
            {
                std::fprintf(stderr,
                             "[%s] FAIL: safe decode of a heavy stream returned %llu, want 0 "
                             "(unsupported must reject)\n",
                             name,
                             static_cast<unsigned long long>(rss));
                return false;
            }
            if (!canary_intact(outs, dcap) or outs != std::vector<uint8_t>(dcap + kMargin, kCanary))
            {
                std::fprintf(
                    stderr, "[%s] FAIL: safe decode of a heavy stream wrote to dst\n", name);
                return false;
            }
        }
        else
        {
            std::vector<uint8_t> outs(dcap + kMargin, kCanary);
            const uint64_t rss = misa77::decompress(
                compressed.data(), csz, outs.data(), dcap, misa77::dconfig(true));
            if (rss != input.size())
            {
                std::fprintf(stderr,
                             "[%s] FAIL: safe roundtrip wrong size (got %llu, want %zu)\n",
                             name,
                             static_cast<unsigned long long>(rss),
                             input.size());
                return false;
            }
            if (!canary_intact(outs, dcap))
            {
                std::fprintf(
                    stderr, "[%s] FAIL: safe decode wrote past the exact-size dst\n", name);
                return false;
            }
            if (input.size() > 0 && std::memcmp(outs.data(), input.data(), input.size()) != 0)
            {
                std::fprintf(stderr, "[%s] FAIL: safe decode content mismatch\n", name);
                return false;
            }
        }

        const double ratio =
            input.size() > 0 ? static_cast<double>(csz) / static_cast<double>(input.size()) : 0.0;
        std::printf("[%s] OK  %zu -> %llu bytes  (ratio %.3f)\n",
                    name,
                    input.size(),
                    static_cast<unsigned long long>(csz),
                    ratio);
        ++stats.passed;
        return true;
    }

    // The runtime-dispatched compress(), swept over every supported level so new
    // zoo modes are covered without touching the call sites below.
    bool run_one(const std::vector<uint8_t>& input, const char* name, Stats& stats)
    {
        bool ok = true;
        for (int8_t level = misa77::config::min_level; level <= misa77::config::max_level; ++level)
        {
            char lname[96];
            std::snprintf(lname, sizeof(lname), "%s@L%d", name, int(level));
            ok = run_one_with(input,
                              lname,
                              stats,
                              misa77::config(level),
                              [level](const uint8_t* s, uint64_t ss, uint8_t* d, uint64_t dc)
                              { return misa77::compress(s, ss, d, dc, misa77::config(level)); }) and
                 ok;
        }
        return ok;
    }

    // Experimental tuned codec: compress with compress_tuned(param), decompress
    // with the standard decoder (same on-disk format).
    bool run_one_tuned(const std::vector<uint8_t>& input,
                       const char* name,
                       Stats& stats,
                       misa77::experimental::param p)
    {
        // Tuned/experimental codecs emit the light format; any light-level config bounds them.
        return run_one_with(input,
                            name,
                            stats,
                            misa77::config(),
                            [&p](const uint8_t* s, uint64_t ss, uint8_t* d, uint64_t dc)
                            { return misa77::experimental::compress_tuned(s, ss, d, dc, p); });
    }

    std::vector<uint8_t> read_file(const char* path)
    {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f)
            return {};
        const std::streamsize size = f.tellg();
        if (size < 0)
            return {};
        f.seekg(0);
        std::vector<uint8_t> data(static_cast<size_t>(size));
        f.read(reinterpret_cast<char*>(data.data()), size);
        return data;
    }

    // Cheap deterministic PRNG so failures are reproducible.
    uint32_t prng32(uint32_t& state)
    {
        state = state * 1103515245u + 12345u;
        return state;
    }
    uint8_t prng_byte(uint32_t& state)
    {
        return static_cast<uint8_t>(prng32(state) >> 16);
    }

    std::vector<uint8_t> random_vec(size_t n, uint32_t seed)
    {
        std::vector<uint8_t> v(n);
        for (auto& b : v)
            b = prng_byte(seed);
        return v;
    }

    // Build input that forces the encoder to emit a match at distance EXACTLY
    // `dist`: a 4-byte sentinel at offset 0 and again at offset `dist`, with
    // constant 0x00 filler between (which only ever touches the hash slot for
    // "0000", so the sentinel's slot survives and the two are paired) and a
    // non-zero literal tail. This is the lever the old test lacked: dis can be
    // driven right across the 64 KB boundary where the uint16_t field truncates.
    std::vector<uint8_t> force_distance(size_t dist)
    {
        const uint8_t sentinel[4] = {0xDE, 0xAD, 0xBE, 0xEF};
        const size_t tail = 96; // >= literal_suffix trailing literals
        std::vector<uint8_t> v(dist + 4 + tail, 0x00);
        std::memcpy(v.data(), sentinel, 4);
        std::memcpy(v.data() + dist, sentinel, 4);
        // Non-zero tail so the second sentinel's match stops at length 4 (the
        // byte after it differs from the 0x00 that follows the first sentinel).
        uint32_t s = 0xC0FFEEu ^ static_cast<uint32_t>(dist);
        for (size_t i = dist + 4; i < v.size(); ++i)
            v[i] = static_cast<uint8_t>(prng_byte(s) | 1u);
        return v;
    }

    std::vector<uint8_t> tiled(const std::vector<uint8_t>& block, size_t copies)
    {
        std::vector<uint8_t> v;
        v.reserve(block.size() * copies);
        for (size_t i = 0; i < copies; ++i)
            v.insert(v.end(), block.begin(), block.end());
        return v;
    }

    // A realistic LZ-friendly stream: literal runs interleaved with back-copies
    // from random earlier offsets (distances up to ~200 KB, so it spans the
    // encoder's whole window including the boundary, and includes overlapping
    // copies that exercise cyccpy's small-dis path).
    std::vector<uint8_t> lz_friendly(size_t n, uint32_t seed)
    {
        std::vector<uint8_t> v;
        v.reserve(n);
        uint32_t s = seed;
        while (v.size() < n)
        {
            if (v.size() > 70000 && (prng_byte(s) & 3u) != 0u)
            {
                const size_t cap = std::min<size_t>(v.size(), 200000);
                const size_t back = 1 + (prng32(s) % cap);
                const size_t len = 4 + (prng_byte(s) % 200u);
                const size_t from = v.size() - back;
                for (size_t i = 0; i < len && v.size() < n; ++i)
                    v.push_back(v[from + i]); // from+i < size() holds (back >= 1)
            }
            else
            {
                const size_t len = 1 + (prng_byte(s) % 40u);
                for (size_t i = 0; i < len && v.size() < n; ++i)
                    v.push_back(prng_byte(s));
            }
        }
        v.resize(n);
        return v;
    }
} // namespace

int main(int argc, char** argv)
{
    Stats stats;

    // --- Edge cases / small-mode boundary (<= small_lim is stored raw) ------
    run_one({}, "empty", stats);
    run_one({0x42}, "1-byte", stats);
    run_one(std::vector<uint8_t>(4, 0xAA), "4-bytes-same", stats);
    run_one(std::vector<uint8_t>(fmt::small_lim, 0xAA), "small_lim-same", stats);
    run_one(std::vector<uint8_t>(fmt::small_lim + 1, 0xAA), "small_lim+1-same", stats);
    run_one(std::vector<uint8_t>(fmt::small_lim + 1, 0xAA), "first-tokenized", stats);
    run_one(random_vec(fmt::small_lim, 5), "small_lim-rand", stats);
    run_one(random_vec(fmt::small_lim + 1, 5), "small_lim+1-rand", stats);

    // --- Lengths that exercise the "15 + extras" encoding -------------------
    // Off-by-ones at the nibble boundary (15) and 255-stuffing boundary (270...).
    for (size_t n : {14u, 15u, 16u, 17u, 28u, 29u, 30u, 269u, 270u, 271u, 524u, 525u, 526u})
    {
        std::vector<uint8_t> v(n, 'X');
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%zu-Xs", n);
        run_one(v, buf, stats);
    }

    // --- DISTANCE SWEEP: the coverage the old test lacked -------------------
    // Forces a single match at each distance, walking from below the lag floor,
    // across the whole window, and straight through the 64 KB field boundary
    // (65532..65568) where the uint16_t dis truncates if the encoder over-accepts.
    for (size_t dist : {size_t(16),        size_t(31),        size_t(32),        size_t(33),
                        size_t(100),       size_t(255),       size_t(256),       size_t(1000),
                        size_t(32768),     size_t(60000),     size_t(65000),     size_t(65500),
                        size_t(65530),     size_t(65532),     size_t(65533),     size_t(65534),
                        fmt::dis_lim - 1,  fmt::dis_lim,      fmt::dis_lim + 1,  fmt::dis_lim + 4,
                        fmt::dis_lim + 16, fmt::dis_lim + 31, fmt::dis_lim + 32, fmt::dis_lim + 64,
                        size_t(70000),     size_t(131072)})
    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "dist=%zu", dist);
        run_one(force_distance(dist), buf, stats);
    }

    // --- Highly repetitive: one giant match, heavy match-length 255-stuffing -
    run_one(std::vector<uint8_t>(1'000, 'A'), "1k-As", stats);
    run_one(std::vector<uint8_t>(1'000'000, 'A'), "1M-As", stats);
    run_one(std::vector<uint8_t>(16'000'000, 'A'), "16M-As", stats);

    // --- Pseudo-random: one giant literal run, heavy literal-length stuffing -
    run_one(random_vec(1'000'000, 1), "1M-prng", stats);
    run_one(random_vec(16'000'000, 1), "16M-prng", stats);

    // --- Boundary distances AT SCALE: identical blocks tiled so every block
    //     boundary emits a long match at a fixed distance. The danger stride
    //     (just past the field max) produces thousands of would-be-truncated
    //     matches; the max-valid stride is the control that must compress. -----
    run_one(tiled(random_vec(fmt::dis_lim - 1, 11), 64), "tiled@dis_lim-1", stats);
    run_one(tiled(random_vec(fmt::dis_lim + 4, 64), 64), "tiled@dis_lim+4", stats);
    run_one(tiled(random_vec(4096, 13), 2048), "tiled@4096-8M", stats);

    // --- Realistic mixed big inputs (varied distances incl. overlap) --------
    run_one(lz_friendly(8'000'000, 99), "8M-lz", stats);
    run_one(lz_friendly(16'000'000, 100), "16M-lz", stats);

    // --- Mix of repeated and random ----------------------------------------
    {
        std::vector<uint8_t> mixed(1'000'000);
        uint32_t state = 7;
        for (size_t i = 0; i < mixed.size(); ++i)
            mixed[i] = (i / 64) % 4 == 0 ? prng_byte(state) : 'Z';
        run_one(mixed, "1M-mixed", stats);
    }

    // --- Experimental tuned compressor (misa77::experimental::compress_tuned) --
    // Same on-disk format, so decompression uses the standard decoder. The region
    // DP has several absolute-vs-region-relative index hazards that ONLY surface
    // for region >= 1, so these cases MUST include multi-region (> 64 KB) inputs.
    // Sizes are kept modest because the DP encoder is much slower than compress().
    {
        using misa77::experimental::param;
        auto mk = [](uint32_t size,
                     uint32_t block,
                     uint32_t s47,
                     uint32_t s815,
                     uint32_t l7,
                     uint32_t l17,
                     uint32_t l33)
        {
            param p;
            p.size = size, p.block = block, p.short4_7 = s47, p.short8_15 = s815;
            p.lit7 = l7, p.lit17 = l17, p.lit33 = l33;
            return p;
        };
        param pdef;
        pdef.use_default = true;

        struct Mode
        {
            const char* name;
            param p;
        };
        const std::vector<Mode> modes = {
            {"ratio", mk(1, 0, 0, 0, 0, 0, 0)},    // pure size: greedy-ish longest matches
            {"lg8_b2", mk(1, 2, 0, 0, 8, 32, 64)}, // decode-friendly (from the sweep)
            {"blocky", mk(1, 8, 8, 2, 0, 0, 0)},   // heavy per-block cost: long literal runs
            {"default", pdef},                     // must match compress() via use_default
        };

        // A > 255 literal run in front of a match, to exercise the block-level
        // literal-length 255-stuffing: 400 random bytes (no internal match) then a
        // 64-byte pattern tiled (matches at dis 64 >= the encoder floor).
        std::vector<uint8_t> lit_stuff = random_vec(400, 77);
        {
            const std::vector<uint8_t> pat = random_vec(64, 78);
            for (int i = 0; i < 16; ++i)
                lit_stuff.insert(lit_stuff.end(), pat.begin(), pat.end());
        }

        struct Case
        {
            const char* name;
            std::vector<uint8_t> data;
        };
        std::vector<Case> cases;
        cases.push_back({"33-raw", std::vector<uint8_t>(33, 'A')});
        cases.push_back({"1k-As", std::vector<uint8_t>(1000, 'A')});
        cases.push_back({"lit-stuff", std::move(lit_stuff)});
        cases.push_back({"300k-As", std::vector<uint8_t>(300u * 1024, 'A')});
        cases.push_back({"256k-prng", random_vec(256u * 1024, 3)});
        cases.push_back({"dist=65540", force_distance(65540)});
        cases.push_back({"dist=131072", force_distance(131072)});
        cases.push_back({"tiled@4096-1M", tiled(random_vec(4096, 13), 256)});
        cases.push_back({"1M-lz", lz_friendly(1'000'000, 42)});

        char buf[96];
        for (const auto& m : modes)
            for (const auto& c : cases)
            {
                std::snprintf(buf, sizeof(buf), "tuned[%s]/%s", m.name, c.name);
                run_one_tuned(c.data, buf, stats, m.p);
            }
    }

    // --- Safe decoder: inputs it MUST reject (return 0, no OOB write) -------
    // The heavy adversarial fuzzing lives outside ctest; these are the
    // deterministic guard cases whose rejection the decoder guarantees by spec.
    {
        auto expect_reject = [&stats](const char* name, const uint8_t* s, uint64_t n, uint64_t cap)
        {
            ++stats.total;
            std::vector<uint8_t> d(cap + kMargin, kCanary);
            const uint64_t r = misa77::decompress(s, n, d.data(), cap, misa77::dconfig(true));
            if (r != 0)
            {
                std::fprintf(stderr,
                             "[safe-reject/%s] FAIL: accepted malformed input (returned %llu)\n",
                             name,
                             static_cast<unsigned long long>(r));
                return;
            }
            if (!canary_intact(d, cap))
            {
                std::fprintf(stderr, "[safe-reject/%s] FAIL: wrote past dst_cap\n", name);
                return;
            }
            std::printf("[safe-reject/%s] OK\n", name);
            ++stats.passed;
        };

        const std::vector<uint8_t> raw = lz_friendly(300'000, 7);
        std::vector<uint8_t> cs(misa77::compress_bound(raw.size(), misa77::config()));
        const uint64_t csz = misa77::compress(raw.data(), raw.size(), cs.data(), cs.size());
        cs.resize(csz);

        expect_reject("empty", cs.data(), 0, raw.size());
        expect_reject("7-byte-header", cs.data(), 7, raw.size());
        expect_reject("dst-too-small", cs.data(), csz, raw.size() - 1);

        // Header lies, patched in place and restored.
        auto with_field = [&](int field, uint64_t v, const char* name)
        {
            uint8_t saved[8];
            std::memcpy(saved, cs.data() + 8 * field, 8);
            std::memcpy(cs.data() + 8 * field, &v, 8);
            expect_reject(name, cs.data(), csz, raw.size());
            std::memcpy(cs.data() + 8 * field, saved, 8);
        };
        with_field(0, ~uint64_t(0), "size=2^64-1");
        with_field(1, 0, "suffix=0");
        with_field(1, csz, "suffix=whole-stream");
        with_field(1, ~uint64_t(0), "suffix=2^64-1");

        // Match code 31 decodes to match_len 34 > max_match_len (never emitted by any
        // compressor), so a token can end exactly at the loop limit and overshoot the
        // output cursor past one-past-the-end of dst. The decoder must reject the
        // stream (exit equality check) with all writes in-bounds and the overshoot
        // kept in integer arithmetic. Hand-crafted counterexample (2026-07-18).
        {
            std::vector<uint8_t> evil(89, 0);
            const uint64_t claimed_size = 100, suffix_cnt = 32;
            std::memcpy(evil.data(), &claimed_size, 8);
            std::memcpy(evil.data() + 8, &suffix_cnt, 8);
            evil[16] = 0xFF; // token 1: lit_len field 7, match code 31
            evil[19] = 26;   // extras byte: lit_len = 7 + 26 = 33
            evil[20] = 0x3F; // token 2: lit_len 1, match code 31
            expect_reject("match-code-31", evil.data(), evil.size(), claimed_size);
        }

        // Mutation smoke: single-byte corruptions must never crash or write out
        // of bounds; decoding to garbage or rejecting are both acceptable.
        {
            ++stats.total;
            bool ok = true;
            std::vector<uint8_t> d(raw.size() + kMargin, kCanary);
            for (uint64_t pos = 16; pos < csz && ok; pos += csz / 256 + 1)
            {
                cs[pos] ^= 0x5A;
                const uint64_t r =
                    misa77::decompress(cs.data(), csz, d.data(), raw.size(), misa77::dconfig(true));
                if (r > raw.size() || !canary_intact(d, raw.size()))
                {
                    std::fprintf(stderr,
                                 "[safe-mutation] FAIL at flip offset %llu (returned %llu)\n",
                                 static_cast<unsigned long long>(pos),
                                 static_cast<unsigned long long>(r));
                    ok = false;
                }
                cs[pos] ^= 0x5A;
                std::fill(d.begin(), d.end(), kCanary);
            }
            if (ok)
            {
                std::printf("[safe-mutation] OK\n");
                ++stats.passed;
            }
        }
    }

    // --- Any file paths passed on the command line (e.g. corpora) -----------
    for (int i = 1; i < argc; ++i)
    {
        auto data = read_file(argv[i]);
        if (data.empty())
        {
            std::fprintf(stderr, "[%s] FAIL: could not read file\n", argv[i]);
            ++stats.total;
            continue;
        }
        run_one(data, argv[i], stats);
    }

    std::printf("\n%d/%d tests passed\n", stats.passed, stats.total);
    return stats.passed == stats.total ? 0 : 1;
}
