# Game asset corpus, Intel

lzbench, single thread, Intel i7-14650HX (P-core, turbo off). Ratio is the compressed size as a percentage of the original.

The corpus is one 263,301,120-byte tar of shipped game data as an engine sees it after unwrapping the game's own archive containers: block-compressed and uncompressed textures, mesh and animation buffers, compiled levels, cooked engine packages, and text, localisation, bytecode and table data. It is drawn from fourteen PC games on twelve engines. Its composition was fixed by data type, engine and game before any file was selected, without knowledge of how any compressor performs on it. It is built from commercial games, so it can't be distributed.

| Data type | Share of bytes |
| --- | --- |
| Block-compressed textures | 32.5 % |
| Geometry | 15.2 % |
| Levels, worlds, scenes | 13.9 % |
| Engine packages, serialized objects | 13.0 % |
| Other raster data | 9.9 % |
| Text, localisation, tables, bytecode | 9.4 % |
| Animation | 6.1 % |

The block tables use lzbench's `-b` option: the tar is cut into fixed-size blocks, and each block is compressed and decompressed independently.

## Whole file

| Compressor name         | Compression| Decompress.| Compr. size | Ratio |
| ---------------         | -----------| -----------| ----------- | ----- |
| misa77 0.8.0 --1        |   320 MB/s |  4167 MB/s |   150705323 | 57.24 |
| misa77 0.8.0 -0         |   224 MB/s |  4503 MB/s |   146655971 | 55.70 |
| misa77 0.8.0 -1         |  54.2 MB/s |  5351 MB/s |   141927907 | 53.90 |
| misa77 0.8.0 -2         |  35.2 MB/s |  4401 MB/s |   135244018 | 51.36 |
| misa77 0.8.0 -3         |  10.9 MB/s |  4459 MB/s |   129910358 | 49.34 |
| misa77 0.8.0 -4         |  1.91 MB/s |  5228 MB/s |   116372402 | 44.20 |
| misa77 0.8.0 -5         |  0.98 MB/s |  2214 MB/s |    86010625 | 32.67 |
| lz4 1.11.0              |   533 MB/s |  2941 MB/s |   142823813 | 54.24 |
| lz4hc 1.11.0 -9         |  23.7 MB/s |  2721 MB/s |   122165338 | 46.40 |
| lz4hc 1.11.0 -12        |  6.70 MB/s |  2805 MB/s |   121732829 | 46.23 |
| zstd 1.5.7 -1           |   360 MB/s |   789 MB/s |   114616340 | 43.53 |
| zstd 1.5.7 -11          |  34.4 MB/s |   773 MB/s |   100651951 | 38.23 |
| zstd 1.5.7 -19          |  3.51 MB/s |   595 MB/s |    91747266 | 34.84 |
| zstd 1.5.7 -22          |  2.21 MB/s |   582 MB/s |    91643641 | 34.81 |
| zlib 1.4 -9             |  4.27 MB/s |   182 MB/s |   106990390 | 40.63 |
| libdeflate 1.26 -12     |  4.17 MB/s |   465 MB/s |   103400315 | 39.27 |
| xz 5.8.4 -9             |  2.45 MB/s |  52.7 MB/s |    80312924 | 30.50 |
| lzav 7.2 -1             |   501 MB/s |  2228 MB/s |   132868680 | 50.46 |
| lzav 7.2 -2             |  83.2 MB/s |  1906 MB/s |   121761787 | 46.24 |
| zxc 1.0.0 -3            |   184 MB/s |  4012 MB/s |   154902064 | 58.83 |
| zxc 1.0.0 -5            |  57.8 MB/s |  3346 MB/s |   132312231 | 50.25 |
| zxc 1.0.0 -7            |  7.24 MB/s |  1717 MB/s |   117229590 | 44.52 |

## 256 KiB blocks (`-b256`)

| Compressor name         | Compression| Decompress.| Compr. size | Ratio |
| ---------------         | -----------| -----------| ----------- | ----- |
| misa77 0.8.0 --1        |   331 MB/s |  4113 MB/s |   151897710 | 57.69 |
| misa77 0.8.0 -0         |   232 MB/s |  4441 MB/s |   147958474 | 56.19 |
| misa77 0.8.0 -1         |  57.1 MB/s |  5267 MB/s |   143356524 | 54.45 |
| misa77 0.8.0 -2         |  36.6 MB/s |  4303 MB/s |   136732102 | 51.93 |
| misa77 0.8.0 -3         |  10.0 MB/s |  4364 MB/s |   131565676 | 49.97 |
| misa77 0.8.0 -4         |  2.59 MB/s |  5153 MB/s |   121713088 | 46.23 |
| misa77 0.8.0 -5         |  1.08 MB/s |  2151 MB/s |    91164732 | 34.62 |
| lz4 1.11.0              |   539 MB/s |  2947 MB/s |   143456810 | 54.48 |
| lz4hc 1.11.0 -9         |  25.2 MB/s |  2710 MB/s |   123672012 | 46.97 |
| lz4hc 1.11.0 -12        |  7.28 MB/s |  2799 MB/s |   123260887 | 46.81 |
| zstd 1.5.7 -1           |   343 MB/s |   759 MB/s |   115076662 | 43.71 |
| zstd 1.5.7 -11          |  19.4 MB/s |   752 MB/s |   103735709 | 39.40 |
| zstd 1.5.7 -19          |  3.71 MB/s |   553 MB/s |    96663587 | 36.71 |
| zstd 1.5.7 -22          |  2.96 MB/s |   553 MB/s |    96651131 | 36.71 |
| zlib 1.4 -9             |  4.45 MB/s |   180 MB/s |   107525194 | 40.84 |
| libdeflate 1.26 -12     |  4.13 MB/s |   458 MB/s |   104033438 | 39.51 |
| xz 5.8.4 -9             |  2.35 MB/s |  48.3 MB/s |    85225544 | 32.37 |
| lzav 7.2 -1             |   491 MB/s |  2254 MB/s |   138471611 | 52.59 |
| lzav 7.2 -2             |   112 MB/s |  1926 MB/s |   129111210 | 49.04 |
| zxc 1.0.0 -3            |   185 MB/s |  3192 MB/s |   153199994 | 58.18 |
| zxc 1.0.0 -5            |  59.9 MB/s |  2672 MB/s |   132924513 | 50.48 |
| zxc 1.0.0 -7            |  7.30 MB/s |  1448 MB/s |   117520179 | 44.63 |

## 64 KiB blocks (`-b64`)

| Compressor name         | Compression| Decompress.| Compr. size | Ratio |
| ---------------         | -----------| -----------| ----------- | ----- |
| misa77 0.8.0 --1        |   337 MB/s |  4031 MB/s |   155591506 | 59.09 |
| misa77 0.8.0 -0         |   234 MB/s |  4338 MB/s |   152016556 | 57.73 |
| misa77 0.8.0 -1         |  56.7 MB/s |  5089 MB/s |   147655908 | 56.08 |
| misa77 0.8.0 -2         |  35.5 MB/s |  4103 MB/s |   141329601 | 53.68 |
| misa77 0.8.0 -3         |  12.4 MB/s |  4164 MB/s |   136723427 | 51.93 |
| misa77 0.8.0 -4         |  2.79 MB/s |  4919 MB/s |   126321138 | 47.98 |
| misa77 0.8.0 -5         |  1.25 MB/s |  1967 MB/s |    96355195 | 36.60 |
| lz4 1.11.0              |   442 MB/s |  2283 MB/s |   143943797 | 54.67 |
| lz4hc 1.11.0 -9         |  31.5 MB/s |  2692 MB/s |   128308752 | 48.73 |
| lz4hc 1.11.0 -12        |  9.83 MB/s |  2786 MB/s |   127965091 | 48.60 |
| zstd 1.5.7 -1           |   349 MB/s |   726 MB/s |   117956147 | 44.80 |
| zstd 1.5.7 -11          |  23.0 MB/s |   703 MB/s |   107866597 | 40.97 |
| zstd 1.5.7 -19          |  3.42 MB/s |   550 MB/s |   101658046 | 38.61 |
| zstd 1.5.7 -22          |  2.53 MB/s |   550 MB/s |   101633314 | 38.60 |
| zlib 1.4 -9             |  5.12 MB/s |   176 MB/s |   109015280 | 41.40 |
| libdeflate 1.26 -12     |  4.27 MB/s |   439 MB/s |   105794683 | 40.18 |
| xz 5.8.4 -9             |  1.48 MB/s |  45.7 MB/s |    90293872 | 34.29 |
| lzav 7.2 -1             |   460 MB/s |  1919 MB/s |   140685930 | 53.43 |
| lzav 7.2 -2             |   122 MB/s |  1970 MB/s |   135399817 | 51.42 |
| zxc 1.0.0 -3            |   228 MB/s |  3046 MB/s |   153341815 | 58.24 |
| zxc 1.0.0 -5            |  77.7 MB/s |  2564 MB/s |   137049655 | 52.05 |
| zxc 1.0.0 -7            |  7.81 MB/s |  1171 MB/s |   121146254 | 46.01 |
