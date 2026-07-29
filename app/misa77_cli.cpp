// misa77 - A codec optimized for decompression throughput
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Shreyas Ghildiyal <nonadhocproblems@gmail.com>
//
// misa77 command-line tool.
//
// This CLI was built with the help of the following entities (as I'm too lazy):
// - Claude Opus 4.8
// - Claude Fable 5
//
// Two file-based subcommands over the misa77 library codecs:
//   compress    FILE          -> FILE.misa77
//   decompress  FILE.misa77   -> FILE
//
// Compressed format: [4 byte magic "MSA7"][1 byte version][1 byte flags][raw compression stream]
// Container version 1 = light-format payload, 2 = heavy-format payload (see docs/cli-format.md).

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <misa77/misa77.h>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>

namespace
{
    namespace fs = std::filesystem;

    constexpr std::string_view VERSION_STR = MISA77_VERSION_STR;

    constexpr char MAGIC[4] = {'M', 'S', 'A', '7'};
    // The container version is the compatibility boundary: v1 wraps a light-format
    // payload (readable by every misa build), v2 wraps a heavy-format payload
    // (0.4.0+). A build rejects versions it does not know, so older tools fail
    // gracefully on newer files instead of feeding an unknown stream to their decoder.
    constexpr uint8_t VERSION_LIGHT = 1;
    constexpr uint8_t VERSION_HEAVY = 2;
    constexpr size_t HEADER_SIZE = 6; // 4 magic + 1 version + 1 flags

    [[noreturn]] void die(const std::string& msg)
    {
        std::cerr << "misa: error: " << msg << '\n';
        std::exit(1);
    }

    // ---- --verbose reporting ----

    using clk = std::chrono::steady_clock;

    double seconds_since(clk::time_point t0)
    {
        return std::chrono::duration<double>(clk::now() - t0).count();
    }

    std::string with_commas(uint64_t v)
    {
        std::string s = std::to_string(v);
        for (int i = int(s.size()) - 3; i > 0; i -= 3)
            s.insert(size_t(i), ",");
        return s;
    }

    std::string fixed(double v, int prec)
    {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.*f", prec, v);
        return buf;
    }

    // "misa: IN -> OUT" + sizes/ratio + codec/total timing. `speed_bytes` is the side the
    // MB/s convention rates: input for compression, output for decompression. The codec
    // line is separate from `total_s` because the wall is typically I/O-dominated.
    void report(const std::string& in_path,
                const std::string& out_path,
                uint64_t in_bytes,
                uint64_t out_bytes,
                uint64_t speed_bytes,
                double codec_s,
                double total_s)
    {
        const double ratio = double(std::max(in_bytes, out_bytes)) /
                             double(std::max<uint64_t>(std::min(in_bytes, out_bytes), 1));
        const double mbps = double(speed_bytes) / 1e6 / std::max(codec_s, 1e-9);
        std::cerr << "misa: " << in_path << " -> " << out_path << '\n'
                  << "misa: " << with_commas(in_bytes) << " -> " << with_commas(out_bytes)
                  << " bytes (ratio " << fixed(ratio, 3) << ")\n"
                  << "misa: codec " << fixed(codec_s, 3) << " s (" << fixed(mbps, 1)
                  << " MB/s), total " << fixed(total_s, 2) << " s\n";
    }

    // A memory-mapped file, with two uses:
    //   Mapping in(path):  map an existing file read-only
    //   Mapping out(path, bytes):  create `path` sized to `bytes`, mapped read-write
    // When the final length is only known after writing (compression), call `finish(n)` to shrink
    // the file to `n`.
    class Mapping
    {
    public:
        explicit Mapping(const std::string& path) : path_(path)
        {
            fd_ = open(path.c_str(), O_RDONLY | O_CLOEXEC);
            if (fd_ < 0)
                die("cannot open '" + path + "': " + std::strerror(errno));
            struct stat st{};
            if (fstat(fd_, &st) != 0)
                die("cannot stat '" + path + "': " + std::strerror(errno));
            size_ = static_cast<uint64_t>(st.st_size);
            map(PROT_READ, MAP_PRIVATE);
        }

        Mapping(const std::string& path, uint64_t bytes) : path_(path), size_(bytes)
        {
            fd_ = open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
            if (fd_ < 0)
                die("cannot create '" + path + "': " + std::strerror(errno));
            if (ftruncate(fd_, static_cast<off_t>(size_)) != 0)
                die("cannot size '" + path + "': " + std::strerror(errno));
            map(PROT_READ | PROT_WRITE, MAP_SHARED);
        }

        ~Mapping()
        {
            if (base_ != MAP_FAILED)
                munmap(base_, size_);
            if (fd_ >= 0)
                close(fd_);
        }

        Mapping(const Mapping&) = delete;
        Mapping& operator=(const Mapping&) = delete;

        // Unmap and shrink the file to its real length `n` (<= the reserved size). Call once,
        // after writing, on a writable mapping whose final size wasn't known up front.
        void finish(uint64_t n)
        {
            if (base_ != MAP_FAILED)
            {
                munmap(base_, size_);
                base_ = MAP_FAILED;
            }
            if (ftruncate(fd_, static_cast<off_t>(n)) != 0)
                die("cannot resize '" + path_ + "': " + std::strerror(errno));
            size_ = n;
        }

        uint8_t* data()
        {
            return base_ == MAP_FAILED ? nullptr : static_cast<uint8_t*>(base_);
        }
        const uint8_t* data() const
        {
            return base_ == MAP_FAILED ? nullptr : static_cast<const uint8_t*>(base_);
        }
        uint64_t size() const
        {
            return size_;
        }

    private:
        void map(int prot, int flags)
        {
            if (size_ == 0)
                return; // mmap rejects a zero length; leave the mapping empty (data() == nullptr)
            base_ = mmap(nullptr, size_, prot, flags, fd_, 0);
            if (base_ == MAP_FAILED)
                die("cannot map '" + path_ + "': " + std::strerror(errno));
        }

        std::string path_;
        int fd_ = -1;
        void* base_ = MAP_FAILED;
        uint64_t size_ = 0;
    };

    bool prompt_overwrite(const std::string& path)
    {
        std::cerr << "misa: '" << path << "' exists; overwrite? [y/N] ";
        std::string line;
        if (!std::getline(std::cin, line))
            return false;
        return line == "y" || line == "Y" || line == "yes";
    }

    void ensure_overwritable(const std::string& path, bool force)
    {
        if (fs::exists(path) && !force && !prompt_overwrite(path))
            die("not overwriting '" + path + "'");
    }

    // Refuse to write output onto the very file we are reading: creating the output truncates
    // it, which would destroy the input mid-operation. (Only reachable via an explicit -o; the
    // derived default names never collide with the input.)
    void reject_self_overwrite(const std::string& input, const std::string& output)
    {
        std::error_code ec;
        if (fs::exists(output) && fs::equivalent(input, output, ec) && !ec)
            die("input and output are the same file ('" + output + "')");
    }

    // ---- codec actions ----

    // Returns the final output-file size; `codec_s` gets the library-call time alone.
    uint64_t compress_to(const uint8_t* in,
                         uint64_t n,
                         const std::string& outpath,
                         bool force,
                         misa77::config cfg,
                         double& codec_s)
    {
        ensure_overwritable(outpath, force);
        const uint64_t cap = HEADER_SIZE + misa77::compress_bound(n, cfg);
        Mapping out(outpath, cap); // reserve the worst case, shrink to fit below

        uint8_t* buf = out.data();
        std::memcpy(buf, MAGIC, 4);
        const bool heavy = cfg.level >= misa77::config::heavy_lb;
        buf[4] = heavy ? VERSION_HEAVY : VERSION_LIGHT;
        buf[5] = 0;

        const auto t0 = clk::now();
        const uint64_t csz = misa77::compress(in, n, buf + HEADER_SIZE, cap - HEADER_SIZE, cfg);
        codec_s = seconds_since(t0);
        if (csz == 0)
        {
            unlink(outpath.c_str());
            die("compression failed");
        }
        out.finish(HEADER_SIZE + csz);
        return HEADER_SIZE + csz;
    }

    // Returns the decompressed size; `codec_s` gets the library-call time alone.
    uint64_t decompress_to(
        const uint8_t* in, uint64_t n, const std::string& outpath, bool force, double& codec_s)
    {
        if (n < HEADER_SIZE + 8)
            die("input is too small to be a misa77 file");
        if (std::memcmp(in, MAGIC, 4) != 0)
            die("not a misa77 file (bad magic)");
        if (in[4] != VERSION_LIGHT and in[4] != VERSION_HEAVY)
            die("unsupported misa77 version " + std::to_string(int(in[4])) +
                " (this build reads v" + std::to_string(int(VERSION_LIGHT)) + "-v" +
                std::to_string(int(VERSION_HEAVY)) + ")");
        if (in[5] != 0)
            die("unsupported misa77 flags: " + std::to_string(int(in[5])));

        const uint8_t* payload = in + HEADER_SIZE;
        const uint64_t payload_size = n - HEADER_SIZE;
        const uint64_t orig = misa77::decompressed_size(payload);

        // Mapping the output at exactly `orig` relies on the decoder writing no padding past the
        // logical end; guard that invariant.
        if (misa77::decompressed_buffer_bound(orig) != orig)
            die("internal error: decode buffer bound is not exact");

        ensure_overwritable(outpath, force);
        Mapping out(outpath, orig); // exact size, no finish() needed
        const auto t0 = clk::now();
        const uint64_t r = misa77::decompress(payload, payload_size, out.data(), orig);
        codec_s = seconds_since(t0);
        if (r != orig)
        {
            unlink(outpath.c_str());
            die("decompression failed (corrupt or truncated stream)");
        }
        return orig;
    }

    // ---- args ----

    misa77::config parse_level(std::string_view s)
    {
        int lvl = 0;
        const auto res = std::from_chars(s.data(), s.data() + s.size(), lvl);
        if (res.ec != std::errc() || res.ptr != s.data() + s.size() ||
            lvl < misa77::config::min_level || lvl > misa77::config::max_level)
            die("invalid --level value '" + std::string(s) + "' (want an integer in [" +
                std::to_string(int(misa77::config::min_level)) + ", " +
                std::to_string(int(misa77::config::max_level)) + "])");
        return misa77::config(static_cast<int8_t>(lvl));
    }

    [[noreturn]] void usage(int code)
    {
        std::ostream& os = code == 0 ? std::cout : std::cerr;
        os << "misa77 " << VERSION_STR
           << " : a fast-decompression LZ77-style compressor\n\n"
              "USAGE\n"
              "  misa compress    [OPTIONS] FILE     compress FILE -> FILE.misa77\n"
              "  misa decompress  [OPTIONS] FILE     decompress FILE (a .misa77) -> FILE\n"
              "  misa help | misa version\n\n"
              "COMMON OPTIONS\n"
              "  -o, --output PATH   output path (default derived from FILE)\n"
              "  -f, --force         overwrite the output without asking\n"
              "  -v, --verbose       report sizes, ratio and timing (to stderr)\n\n"
              "COMPRESS OPTIONS\n"
              "  -l, --level N       compression level, "
           << int(misa77::config::min_level) << ".." << int(misa77::config::max_level)
           << "                 [default " << int(misa77::config::default_level)
           << "]\n"
              "                      -1 = fastest compression, 0 = fast compression,\n"
              "                      1 = faster decompression, 2 = better ratio,\n"
              "                      3 = best ratio (slow compression, good decode speed),\n"
              "                      4 = large-window format: wins on big inputs\n"
              "                          (slow compression, good decode speed)\n";
        std::exit(code);
    }

    int run(int argc, char** argv)
    {
        if (argc < 2)
            usage(1);

        const std::string_view cmd = argv[1];
        if (cmd == "help" || cmd == "-h" || cmd == "--help")
            usage(0);
        if (cmd == "version" || cmd == "--version")
        {
            std::cout << "misa77 " << VERSION_STR << '\n';
            return 0;
        }

        enum class Cmd
        {
            Compress,
            Decompress
        } which;
        if (cmd == "compress" || cmd == "c")
            which = Cmd::Compress;
        else if (cmd == "decompress" || cmd == "d" || cmd == "x")
            which = Cmd::Decompress;
        else
            die("unknown command '" + std::string(cmd) + "' (try: misa help)");

        std::string input, output;
        bool have_input = false, have_output = false, force = false, verbose = false;
        misa77::config cfg;
        bool have_level = false;

        for (int i = 2; i < argc; ++i)
        {
            const std::string_view s = argv[i];
            auto next = [&](std::string_view name) -> std::string
            {
                if (i + 1 >= argc)
                    die("missing argument for " + std::string(name));
                return argv[++i];
            };
            if (s == "-o" || s == "--output")
                output = next(s), have_output = true;
            else if (s == "-f" || s == "--force")
                force = true;
            else if (s == "-v" || s == "--verbose")
                verbose = true;
            else if (s == "--level" || s == "-l")
                cfg = parse_level(next(s)), have_level = true;
            else if (!s.empty() && s[0] == '-')
                die("unknown option '" + std::string(s) + "'");
            else if (have_input)
                die("multiple input files given");
            else
                input = std::string(s), have_input = true;
        }

        if (!have_input)
            die("missing input file (try: misa help)");

        // Flags that only mean something on one codec path are errors elsewhere: silently
        // ignoring an option the user typed hides a misunderstanding.
        if (have_level && which != Cmd::Compress)
            die("--level applies only to 'misa compress'");

        const auto t0 = clk::now();
        if (which == Cmd::Compress)
        {
            const Mapping in(input);
            const std::string outpath = have_output ? output : input + ".misa77";
            reject_self_overwrite(input, outpath);
            double codec_s = 0;
            const uint64_t out_bytes =
                compress_to(in.data(), in.size(), outpath, force, cfg, codec_s);
            if (verbose)
                report(input, outpath, in.size(), out_bytes, in.size(), codec_s, seconds_since(t0));
        }
        else // Decompress
        {
            const Mapping in(input);
            std::string outpath;
            if (have_output)
                outpath = output;
            else if (input.ends_with(".misa77"))
                outpath = input.substr(0, input.size() - 7);
            else
                die("cannot derive an output name from '" + input + "'; use -o");
            reject_self_overwrite(input, outpath);
            double codec_s = 0;
            const uint64_t out_bytes = decompress_to(in.data(), in.size(), outpath, force, codec_s);
            if (verbose)
                report(input, outpath, in.size(), out_bytes, out_bytes, codec_s, seconds_since(t0));
        }
        return 0;
    }

} // namespace

int main(int argc, char** argv)
{
    try
    {
        return run(argc, argv);
    }
    catch (const std::exception& e)
    {
        std::cerr << "misa: error: " << e.what() << '\n';
        return 1;
    }
}
