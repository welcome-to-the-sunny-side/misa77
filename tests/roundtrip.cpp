// misa77 - A codec optimized for decompression throughput
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Shreyas Ghildiyal <nonadhocproblems@gmail.com>

#include "codebook.h"
#include "compress_dispatch.h"
#include "decompress_dispatch.h"
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
    constexpr size_t small_lim = 32;      // <= this is stored raw
    constexpr size_t hashtab_lag = 32;    // min match distance the encoder emits
    constexpr size_t dis_lim = 1u << 16;  // 65536; emitted dis must fit a uint16_t
    constexpr size_t vector_width = 32;   // cyccpy's unconditional store width
    constexpr uint8_t format_mask = 0x03; // flags byte (offset 7): low 2 bits = format, 0 = light

    // false while the new level 4 is being ported: heavy lanes are skipped
    constexpr bool heavy_ported = true;

    // heavy (level 4) stream layout, restated here so the test does not inherit a format.h slip
    constexpr uint8_t heavy_flags_a = 0x01;
    constexpr uint8_t heavy_flags_s = 0x05;
    constexpr uint64_t heavy_min_dis = 33;
    constexpr size_t heavy_literal_suffix = 32;
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
        const bool stream_heavy = (compressed[7] & fmt::format_mask) != 0;
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
        {
            std::vector<uint8_t> outs(dcap + kMargin, kCanary);
            const uint64_t rss = misa77::decompress(compressed.data(), csz, outs.data(), dcap);
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
        const int8_t top_level =
            fmt::heavy_ported ? misa77::config::max_level : int8_t(misa77::config::heavy_lb - 1);
        for (int8_t level = misa77::config::min_level; level <= top_level; ++level)
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

    // Assembles a valid heavy stream token by token, together with the output it must decode to.
    // Covers codes and shapes the level-4 compressor may never emit.
    struct heavy_builder
    {
        const misa77::heavy::entry* cb;
        uint8_t flags;
        std::vector<uint8_t> cmd, off, lit, body, out;
        uint64_t rep = 0;
        uint32_t seed = 12345;

        heavy_builder(const misa77::heavy::entry* cb_, uint8_t flags_) : cb(cb_), flags(flags_) {}

        uint8_t rnd()
        {
            seed = seed * 1664525u + 1013904223u;
            return uint8_t(seed >> 24);
        }

        void literals(uint32_t n)
        {
            for (uint32_t i = 0; i < n; ++i)
            {
                const uint8_t b = rnd();
                lit.push_back(b);
                out.push_back(b);
            }
        }

        // `dis` is only read for S / N / F; R reuses the last one. Returns false if the codebook
        // has no such code.
        bool tok(misa77::heavy::kind k, uint32_t nlit, uint32_t len, uint64_t dis = 0)
        {
            int c = -1;
            for (int i = 0; i < int(misa77::heavy::codebook_size); ++i)
                if (cb[i].k == k and cb[i].lit == nlit and cb[i].len == len)
                    c = i;
            if (c < 0)
                return false;
            cmd.push_back(uint8_t(c));
            literals(nlit);
            const uint32_t ob = misa77::heavy::offset_bytes(k);
            if (ob > 0)
            {
                const uint64_t v = dis - fmt::heavy_min_dis;
                for (uint32_t i = 0; i < ob; ++i)
                    off.push_back(uint8_t(v >> (8 * i)));
                rep = dis;
            }
            for (uint32_t i = 0; i < len; ++i)
            {
                const uint8_t b = out[out.size() - rep];
                out.push_back(b);
            }
            return true;
        }

        void end_block(uint32_t trailing)
        {
            literals(trailing);
            for (const size_t cnt : {cmd.size(), off.size(), lit.size()})
                for (int i = 0; i < 3; ++i)
                    body.push_back(uint8_t(cnt >> (8 * i)));
            body.insert(body.end(), cmd.begin(), cmd.end());
            body.insert(body.end(), off.begin(), off.end());
            body.insert(body.end(), lit.begin(), lit.end());
            cmd.clear(), off.clear(), lit.clear();
        }

        std::vector<uint8_t> stream() const
        {
            std::vector<uint8_t> s(8);
            const uint64_t header = uint64_t(out.size()) | (uint64_t(flags) << 56);
            for (int i = 0; i < 8; ++i)
                s[size_t(i)] = uint8_t(header >> (8 * i));
            s.insert(s.end(), body.begin(), body.end());
            return s;
        }
    };

    void check_heavy(const heavy_builder& b, bool built, const char* name, Stats& stats)
    {
        ++stats.total;
        if (not built)
        {
            std::fprintf(stderr, "[heavy-handbuilt/%s] FAIL: codebook lacks a code\n", name);
            return;
        }
        const std::vector<uint8_t> s = b.stream();
        const size_t n = b.out.size();
        std::vector<uint8_t> d(n + kMargin, kCanary);
        const uint64_t r = misa77::decompress(s.data(), s.size(), d.data(), n);
        if (r != n or not std::equal(b.out.begin(), b.out.end(), d.begin()) or
            not canary_intact(d, n))
        {
            std::fprintf(stderr,
                         "[heavy-handbuilt/%s] FAIL: returned %llu for n = %zu\n",
                         name,
                         static_cast<unsigned long long>(r),
                         n);
            return;
        }
        std::printf("[heavy-handbuilt/%s] OK (%zu bytes)\n", name, n);
        ++stats.passed;

        // the safe decoder agrees on the valid stream
        ++stats.total;
        std::fill(d.begin(), d.end(), kCanary);
        const uint64_t rs = misa77::decompress(s.data(), s.size(), d.data(), n);
        if (rs != n or not std::equal(b.out.begin(), b.out.end(), d.begin()) or
            not canary_intact(d, n))
        {
            std::fprintf(stderr,
                         "[heavy-handbuilt-safe/%s] FAIL: returned %llu for n = %zu\n",
                         name,
                         static_cast<unsigned long long>(rs),
                         n);
            return;
        }
        ++stats.passed;

        // Corrupted and truncated copies: the safe decoder may accept or reject, but must stay
        // inside both buffers. `m` is sized exactly so a sanitizer build sees any over-read.
        ++stats.total;
        uint32_t seed = 987654321u ^ uint32_t(n);
        const auto rnd = [&seed]() -> uint32_t
        {
            seed = seed * 1664525u + 1013904223u;
            return seed >> 8;
        };
        const int trials = n < 4096 ? 20000 : 1500;
        for (int trial = 0; trial < trials; ++trial)
        {
            std::vector<uint8_t> m = s;
            if (trial % 4 == 3)
                m.resize(rnd() % s.size());
            const int flips = m.empty() ? 0 : 1 + int(rnd() % 3);
            for (int i = 0; i < flips; ++i)
            {
                // bias towards the headers and the command stream
                const size_t pos =
                    (rnd() % 2) ? rnd() % std::min<size_t>(m.size(), 64) : rnd() % m.size();
                m[pos] = uint8_t(rnd());
            }
            std::fill(d.begin(), d.end(), kCanary);
            const uint64_t rm = misa77::decompress(m.data(), m.size(), d.data(), n);
            if (rm > n or not canary_intact(d, n))
            {
                std::fprintf(stderr,
                             "[heavy-mutate/%s] FAIL: trial %d returned %llu, n = %zu\n",
                             name,
                             trial,
                             static_cast<unsigned long long>(rm),
                             n);
                return;
            }
        }
        std::printf("[heavy-mutate/%s] OK (%d trials)\n", name, trials);
        ++stats.passed;
    }

    void run_heavy_handbuilt(Stats& stats)
    {
        using misa77::heavy::kind;
        for (const bool small_table : {false, true})
        {
            const misa77::heavy::entry* cb =
                small_table ? misa77::heavy::codebook_s : misa77::heavy::codebook_a;
            const uint8_t flags = small_table ? fmt::heavy_flags_s : fmt::heavy_flags_a;
            const char* tag = small_table ? "S" : "A";
            char name[64];

            {
                heavy_builder b(cb, flags);
                b.end_block(100);
                std::snprintf(name, sizeof name, "%s/literal-only", tag);
                check_heavy(b, true, name, stats);
            }
            {
                // every kind, big literal and big repeat codes, first match at the minimum
                // distance, exactly `literal_suffix` trailing literals
                heavy_builder b(cb, flags);
                bool ok = b.tok(kind::L, 64, 0) and b.tok(kind::N, 0, 10, 40) and
                          b.tok(kind::S, 0, 5, fmt::heavy_min_dis) and b.tok(kind::R, 0, 16) and
                          b.tok(kind::L, 4, 0) and b.tok(kind::N, 1, 5, 60) and
                          b.tok(kind::R, 0, 64) and b.tok(kind::L, 32768, 0) and
                          b.tok(kind::N, 0, 32, 30000) and b.tok(kind::S, 0, 4, 288);
                if (not small_table)
                    ok = ok and b.tok(kind::F, 0, 6, 32000);
                ok = ok and b.tok(kind::R, 0, 65536);
                b.end_block(uint32_t(fmt::heavy_literal_suffix));
                std::snprintf(name, sizeof name, "%s/one-block", tag);
                check_heavy(b, ok, name, stats);
            }
            {
                // the repeat distance carries across blocks; a block with no trailing literals
                heavy_builder b(cb, flags);
                bool ok = b.tok(kind::L, 64, 0) and b.tok(kind::N, 0, 10, 40);
                b.end_block(3);
                ok = ok and b.tok(kind::R, 0, 16) and b.tok(kind::N, 1, 5, 50);
                b.end_block(0);
                ok = ok and b.tok(kind::L, 2, 0) and b.tok(kind::R, 0, 32);
                b.end_block(33);
                std::snprintf(name, sizeof name, "%s/three-blocks", tag);
                check_heavy(b, ok, name, stats);
            }
        }
    }

    // --- Cross-ISA lane ------------------------------------------------------
    // compress() / decompress() only ever run the best path the CPU supports, so
    // the SSE2 and portable code would otherwise never execute here. This calls
    // every ISA entry point the machine can run directly: each one's stream must
    // decode to the input through every one's decoder.
    struct isa_entry
    {
        const char* name;
        uint64_t (*compress)(
            const uint8_t* __restrict, uint64_t, uint8_t* __restrict, uint64_t, misa77::config);
        uint64_t (*decompress)(const uint8_t* __restrict, uint64_t, uint8_t* __restrict, uint64_t);
    };

    std::vector<isa_entry> runnable_isas()
    {
        std::vector<isa_entry> v;
#if defined(__x86_64__)
        if (__builtin_cpu_supports("avx2"))
            v.push_back({"avx2", misa77::compress_avx2, misa77::decompress_avx2});
        v.push_back({"sse2", misa77::compress_sse2, misa77::decompress_sse2});
#elif defined(__aarch64__)
        v.push_back({"neon", misa77::compress_neon, misa77::decompress_neon});
#endif
        v.push_back({"portable", misa77::compress_portable, misa77::decompress_portable});
        return v;
    }

    // Streams are byte-identical across the x86 paths and portable (measured, and
    // pinned here). NEON has never been checked for that, so there it is only
    // reported.
#if defined(__aarch64__)
    constexpr bool isa_identity_required = false;
#else
    constexpr bool isa_identity_required = true;
#endif

    void run_cross_isa_one(const std::vector<uint8_t>& input, const char* name, Stats& stats)
    {
        const std::vector<isa_entry> isas = runnable_isas();
        const int8_t top =
            fmt::heavy_ported ? misa77::config::max_level : int8_t(misa77::config::heavy_lb - 1);

        for (int lvl = misa77::config::min_level; lvl <= top; ++lvl)
        {
            ++stats.total;
            bool ok = true;
            const misa77::config cfg{static_cast<int8_t>(lvl)};
            const uint64_t bound = misa77::compress_bound(input.size(), cfg);

            std::vector<std::vector<uint8_t>> streams;
            for (const isa_entry& e : isas)
            {
                std::vector<uint8_t> c(bound + kMargin, kCanary);
                const uint64_t n = e.compress(input.data(), input.size(), c.data(), bound, cfg);
                if (n == 0 or n > bound or not canary_intact(c, bound))
                {
                    std::fprintf(
                        stderr,
                        "[cross-isa %s L%d] FAIL: %s compress returned %llu (bound %llu)\n",
                        name,
                        lvl,
                        e.name,
                        static_cast<unsigned long long>(n),
                        static_cast<unsigned long long>(bound));
                    ok = false;
                }
                c.resize(n <= bound ? n : 0);
                streams.push_back(std::move(c));
            }

            for (size_t i = 1; i < streams.size(); ++i)
                if (streams[i] != streams[0])
                {
                    std::fprintf(stderr,
                                 "[cross-isa %s L%d] %s: %s and %s streams differ\n",
                                 name,
                                 lvl,
                                 isa_identity_required ? "FAIL" : "note",
                                 isas[0].name,
                                 isas[i].name);
                    ok = ok and not isa_identity_required;
                }

            for (size_t i = 0; i < streams.size() and ok; ++i)
                for (const isa_entry& e : isas)
                    for (const bool safe : {true}) // 0.8.0: safe is the only mode
                    {
                        std::vector<uint8_t> d(input.size() + kMargin, kCanary);
                        const uint64_t r = e.decompress(
                            streams[i].data(), streams[i].size(), d.data(), input.size());
                        if (r != input.size() or not canary_intact(d, input.size()) or
                            not std::equal(input.begin(), input.end(), d.begin()))
                        {
                            std::fprintf(stderr,
                                         "[cross-isa %s L%d] FAIL: %s stream through the %s %s "
                                         "decoder returned %llu\n",
                                         name,
                                         lvl,
                                         isas[i].name,
                                         e.name,
                                         safe ? "safe" : "unsafe",
                                         static_cast<unsigned long long>(r));
                            ok = false;
                        }
                    }

            if (ok)
                ++stats.passed;
        }
    }
} // namespace

int main(int argc, char** argv)
{
    Stats stats;

    run_heavy_handbuilt(stats);

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
            const uint64_t r = misa77::decompress(s, n, d.data(), cap);
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

        // Every flags byte with a non-light format id other than the two valid heavy ones is
        // rejected untouched. A light stream relabelled as heavy is garbage: unsafe must not
        // see it, safe must stay in bounds.
        if (not fmt::heavy_ported)
        {
            ++stats.total;
            bool ok = true;
            const std::vector<uint8_t> clean(raw.size() + kMargin, kCanary);
            std::vector<uint8_t> d = clean;
            for (int flags = 1; flags < 256 and ok; ++flags)
            {
                if ((flags & fmt::format_mask) == 0)
                    continue;
                cs[7] = uint8_t(flags);
                for (const bool safe : {true}) // 0.8.0: safe is the only mode
                {
                    const bool heavy_flags =
                        flags == fmt::heavy_flags_a or flags == fmt::heavy_flags_s;
                    if (heavy_flags and not safe)
                        continue;
                    const uint64_t r = misa77::decompress(cs.data(), csz, d.data(), raw.size());
                    if (heavy_flags)
                    {
                        if (r > raw.size() or not canary_intact(d, raw.size()))
                        {
                            std::fprintf(stderr, "[flags-gate] FAIL: flags 0x%02X\n", flags);
                            ok = false;
                        }
                        d = clean;
                        continue;
                    }
                    if (r != 0 or d != clean)
                    {
                        std::fprintf(stderr,
                                     "[flags-gate] FAIL: flags 0x%02X %s decode returned %llu\n",
                                     flags,
                                     safe ? "safe" : "unsafe",
                                     static_cast<unsigned long long>(r));
                        ok = false;
                        d = clean;
                    }
                }
            }
            cs[7] = 0;
            if (ok)
            {
                std::printf("[flags-gate] OK\n");
                ++stats.passed;
            }
        }

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
                const uint64_t r = misa77::decompress(cs.data(), csz, d.data(), raw.size());
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

    // --- Every ISA entry point this machine can run --------------------------
    // Sizes straddle small mode, the level-4 table switch, a level-4 block and
    // its sort segment.
    run_cross_isa_one({}, "empty", stats);
    run_cross_isa_one({0x42}, "1-byte", stats);
    run_cross_isa_one(random_vec(fmt::small_lim, 21), "small_lim-rand", stats);
    run_cross_isa_one(random_vec(fmt::small_lim + 1, 21), "small_lim+1-rand", stats);
    run_cross_isa_one(std::vector<uint8_t>(300, 'X'), "300-Xs", stats);
    run_cross_isa_one(lz_friendly(50'000, 22), "50k-lz", stats);
    run_cross_isa_one(lz_friendly(300'000, 23), "300k-lz", stats);
    run_cross_isa_one(lz_friendly(2'500'000, 24), "2.5M-lz", stats);
    run_cross_isa_one(random_vec(200'000, 25), "200k-prng", stats);
    run_cross_isa_one(std::vector<uint8_t>(1'000'000, 'A'), "1M-As", stats);
    run_cross_isa_one(force_distance(40), "dist=40", stats);
    run_cross_isa_one(force_distance(fmt::dis_lim + 32), "dist=dis_lim+32", stats);
    run_cross_isa_one(tiled(random_vec(4096, 26), 300), "tiled@4096-1.2M", stats);

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
