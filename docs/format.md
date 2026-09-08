A lot of constants will be referenced ahead. Refer to [`src/format.h`](../src/format.h) for more information about them.

Assumptions:

- The size of the raw file is `<= 1e15` bytes.
- Everything is little-endian.

The format is as follows:

- The first 7 bytes contain an unsigned 56-bit integer which is equal to the size of the uncompressed data in bytes.
- The next (8th) byte contains flags.
- The bit at position `format_bit` dictates what the following format is. If it's 0, then the format is "light", otherwise, it's "heavy".

## Light

Constants referenced ahead are from namespace `light` in [`src/format.h`](../src/format.h).

- If `src_size <= small_lim` bytes, we simply write the raw source.
- Otherwise:
    - The next 8 bytes contain another unsigned 64-bit integer `literal_suffix_cnt`, which is equal to the length of the suffix of source (in bytes) that we're *not* going to compress (ie. we will append these raw bytes to the end of the compressed format). It's guaranteed that `literal_suffix_cnt >= literal_suffix` (constant from [`src/format.h`](../src/format.h)).
    - The bytes that follow correspond to a sequence of disjoint "blocks".
    - Each block contains four pieces of data: 
        - Literal length.
        - Raw literals.
        - Match length.
        - Offset to the position behind us from where the match begins.
    - The decompressor will read these disjoint blocks from left to right, and perform two operations for each block:
        - Append the sequence of raw literals to the bytes already written in the decompression buffer.
        - Append the specified match to the bytes already written in the decompression buffer.
    - Each block is stored in the following format:
        - A "control sub-block", which contains the literal length, match length, and match offset for the block. The byte-level decomposition is `[1 token byte][2 match offset bytes][variable number of extra literal length bytes]`.
            - The token byte is divided into two parts : `[top 3 bits = l'][bottom 5 bits = m']`.
            - The match length is defined as `m = m' + min_match_len - 1`. It's guaranteed that `min_match_len <= m <= max_match_len`.
            - The 2 match offset bytes contain a 16-bit unsigned integer `d'`, with the actual distance to the match being defined as `d' + hashtab_lag + 1`. 
            - The literal length `l` is derived through the following algorithm:
                - If `l' < 7`, then `l = l'`.
                - Otherwise, we read extra literal length bytes until we encounter the first byte with value `< 255`. We define `l = l' + 255 * (number of 255-value bytes) + (value of final byte)` (the value of a byte is just its interpretation as an unsigned 8 bit integer here).
            - Note that each "control sub-block" always contains the token byte and 2 match offset bytes.
        - A "raw literal sub-block", which just contains the raw literal bytes to be copied by the decompressor when processing this block. This sub-block contains literal bytes in the order in which they will be copied by the decompressor.
    - Now, the two sub-blocks for a block aren't stored contiguously. Instead, we have two separate streams for control sub-blocks and literal sub-blocks. To be precise:
        - The control sub-block stream grows from left to right, and the literal sub-block stream grows from right to left.
        - The complete block stream takes the following form: `[control sub-block for block 1][control sub-block for block 2]...[control sub-block for block n][literal sub-block for block n]...[literal sub-block for block 2][literal sub-block for block 1]`. 
    - At the end, we append the previously mentioned suffix of raw bytes. It is guaranteed that the number of literal bytes in this suffix are >= `literal_suffix`.
- Therefore, when `src_size > small_lim`, the complete bitstream has the following form:

```text
[src_size (8 bytes)]
[literal_suffix_cnt (8 bytes)]
[control sub-block for block 1 (>= 3 bytes)]
[control sub-block for block 2 (>= 3 bytes)]
.
.
.
[control sub-block for block n ( >= 3 bytes)]
[literal sub-block for block n (>= 0 bytes)]
.
.
.
[literal sub-block for block 2 (>= 0 bytes)]
[literal sub-block for block 1 (>= 0 bytes)]
[raw literal suffix bytes (>= literal_suffix bytes)]
```

Some constraints:

- If copying a match involves copying range `[x, x + n)` to `[y, y + n)`, then we must have `hashtab_lag + 1 <= y - x <= hashtab_lag + dis_lim`.

## Heavy

Constants referenced ahead are from namespace `heavy` in [`src/format.h`](../src/format.h).

The bit at position `cond_bit` in the 8-th byte is a decoder performance hint. It never affects the decoded bytes, and a decoder may ignore it. If set, long matches are rare in this stream: copy 32 bytes unconditionally, and the rest only when `match length > 32`. If clear, long matches are common: copy 64 bytes unconditionally (with a tail loop for matches past 64).

- If `src_size <= small_lim` bytes, we simply write the raw source.
- Otherwise:
    - The next 8 bytes contain another unsigned 64-bit integer `literal_suffix_cnt`, which is equal to the length of the suffix of source (in bytes) that we're *not* going to compress (ie. we will append these raw bytes to the end of the compressed format). It's guaranteed that `literal_suffix_cnt >= literal_suffix` (constant from [`src/format.h`](../src/format.h)).
    - The bytes that follow correspond to a sequence of disjoint "blocks".
    - Each block contains four pieces of data: 
        - Literal length.
        - Raw literals.
        - Match length.
        - Offset to the position behind us from where the match begins.
    - The decompressor will read these disjoint blocks from left to right, and perform two operations for each block:
        - Append the sequence of raw literals to the bytes already written in the decompression buffer.
        - Append the specified match to the bytes already written in the decompression buffer.
    - Each block is stored in the following format:
        - A "control sub-block" spanning 4 bytes, which contains the literal length, match length, and match offset for the block. The 4 bytes form a little-endian unsigned 32-bit integer, decomposed as: bits 0-19 (lowest) = match offset, bits 20-25 = match length code, bits 26-31 (highest) = literal length.
            - Let `m` be the unsigned 6-bit integer derived from the match length bits and `l'` be the unsigned 6-bit integer derived from the literal length bits.
            - The match length is defined as `LUT.len_of[m]`, where `LUT` is a lookup table defined in [`src/format.h`](../src/format.h). `m = 0` is reserved and never appears in a valid stream.
            - The 20 match offset bits contain a 20-bit unsigned integer `d'`, with the actual distance to the match being defined as `d' + hashtab_lag + 1`. 
            - The literal length `l` is derived through the following algorithm:
                - If `l' < lit_lim`, then `l = l'`.
                - Otherwise, we read extra literal length bytes until we encounter the first byte with value `< 255`. We define `l = l' + 255 * (number of 255-value bytes) + (value of final byte)` (the value of a byte is just its interpretation as an unsigned 8 bit integer here).
            - Note that each "control sub-block" always contains the 4 bytes mentioned above.
        - A "raw literal sub-block", which just contains the raw literal bytes to be copied by the decompressor when processing this block. This sub-block contains literal bytes in the order in which they will be copied by the decompressor.
    - Now, the two sub-blocks for a block aren't stored contiguously. Instead, we have two separate streams for control sub-blocks and literal sub-blocks. To be precise:
        - The control sub-block stream grows from left to right, and the literal sub-block stream grows from right to left.
        - The complete block stream takes the following form: `[control sub-block for block 1][control sub-block for block 2]...[control sub-block for block n][literal sub-block for block n]...[literal sub-block for block 2][literal sub-block for block 1]`. 
    - At the end, we append the previously mentioned suffix of raw bytes. It is guaranteed that the number of literal bytes in this suffix are >= `literal_suffix`.
- Therefore, when `src_size > small_lim`, the complete bitstream has the following form:

```text
[src_size (8 bytes)]
[literal_suffix_cnt (8 bytes)]
[control sub-block for block 1 (>= 4 bytes)]
[control sub-block for block 2 (>= 4 bytes)]
.
.
.
[control sub-block for block n ( >= 4 bytes)]
[literal sub-block for block n (>= 0 bytes)]
.
.
.
[literal sub-block for block 2 (>= 0 bytes)]
[literal sub-block for block 1 (>= 0 bytes)]
[raw literal suffix bytes (>= literal_suffix bytes)]
```

Some constraints:

- If copying a match involves copying range `[x, x + n)` to `[y, y + n)`, then we must have `hashtab_lag + 1 <= y - x <= hashtab_lag + ndis`.

## Compact

Constants referenced ahead are from namespace `compact` in [`src/format.h`](../src/format.h).

The compact format targets compiled code and binaries: it allows short distances (down to 1) and
long matches, at the cost of a slightly less uniform decode loop than the light format.

- If `src_size <= small_lim` bytes, we simply write the raw source (after the 8-byte header).
- Otherwise:
    - The next 8 bytes contain `literal_suffix_cnt`, exactly as in the light format.
    - The remaining bytes are the two streams (control forward, literals backward) followed by the
      raw literal suffix, exactly as in the light format. Only the token grammar differs.
    - Each token is either a **short** (2-byte) or **long** (3-byte) control sub-block, chosen by
      the top bit of the first byte:
        - Short: `0[L:3][l:2][D:2]` then one byte `d`.
            - `L = match_len - 4`, so `match_len` is 4..11.
            - `l` is the literal length (0..2 direct; 3 means an escape).
            - `D:d` is `dis - 1` as a 10-bit value, so `dis` is 1..1024.
        - Long: `1[M:5][l:2]` then two little-endian bytes `d`.
            - `M = match_len - 4` for `match_len` in 4..32. `M = 29` means the match length is
              escaped; 30 and 31 are reserved and must be rejected by a safe decoder.
            - `l` is the literal length (same 2-bit encoding as the short form).
            - `d` is `dis - 1` as a 16-bit value, so `dis` is 1..65536.
    - Literal-length escape (`l = 3`): read bytes until the first value `< 255`; the literal length
      is `3 + 255 * (number of 255-value bytes) + (final byte)`.
    - Match-length escape (`M = 29`): read bytes until the first value `< 255`; the match length is
      `33 + 255 * (number of 255-value bytes) + (final byte)`.
    - When both escapes are present, the **match-length** escape is read first, then the
      **literal-length** escape.

Some constraints:

- If copying a match involves copying range `[x, x + n)` to `[y, y + n)`, then we must have
  `min_dis <= y - x <= max_dis`, i.e. `1 <= y - x <= 65536`. Matches may overlap their source
  (`y - x < n`), in which case the copy repeats the `y - x` byte pattern.
- The decoder must not assume `dis >= vector_width`; overlapping matches are legal.
