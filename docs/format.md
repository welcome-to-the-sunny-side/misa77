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
    - The bytes that follow correspond to a series of disjoint "sequences".
    - Each sequence contains four pieces of data: 
        - Literal length.
        - Raw literals.
        - Match length.
        - Offset to the position behind us from where the match begins.
    - The decompressor will read these disjoint sequences from left to right, and perform two operations for each sequence:
        - Append the run of raw literals to the bytes already written in the decompression buffer.
        - Append the specified match to the bytes already written in the decompression buffer.
    - Each sequence is stored in the following format:
        - A "control part", which contains the literal length, match length, and match offset for the sequence. The byte-level decomposition is `[1 token byte][2 match offset bytes][variable number of extra literal length bytes]`.
            - The token byte is divided into two parts : `[top 3 bits = l'][bottom 5 bits = m']`.
            - The match length is defined as `m = m' + min_match_len - 1`. It's guaranteed that `min_match_len <= m <= max_match_len`.
            - The 2 match offset bytes contain a 16-bit unsigned integer `d'`, with the actual distance to the match being defined as `d' + hashtab_lag + 1`. 
            - The literal length `l` is derived through the following algorithm:
                - If `l' < 7`, then `l = l'`.
                - Otherwise, we read extra literal length bytes until we encounter the first byte with value `< 255`. We define `l = l' + 255 * (number of 255-value bytes) + (value of final byte)` (the value of a byte is just its interpretation as an unsigned 8 bit integer here).
            - Note that each "control part" always contains the token byte and 2 match offset bytes.
        - A "raw literal part", which just contains the raw literal bytes to be copied by the decompressor when processing this sequence. This part contains literal bytes in the order in which they will be copied by the decompressor.
    - Now, the two parts for a sequence aren't stored contiguously. Instead, we have two separate streams for control parts and literal parts. To be precise:
        - The control part stream grows from left to right, and the literal part stream grows from right to left.
        - The complete sequence stream takes the following form: `[control part for sequence 1][control part for sequence 2]...[control part for sequence n][literal part for sequence n]...[literal part for sequence 2][literal part for sequence 1]`. 
    - At the end, we append the previously mentioned suffix of raw bytes. It is guaranteed that the number of literal bytes in this suffix are >= `literal_suffix`.
- Therefore, when `src_size > small_lim`, the complete bitstream has the following form:

```text
[src_size (8 bytes)]
[literal_suffix_cnt (8 bytes)]
[control part for sequence 1 (>= 3 bytes)]
[control part for sequence 2 (>= 3 bytes)]
.
.
.
[control part for sequence n ( >= 3 bytes)]
[literal part for sequence n (>= 0 bytes)]
.
.
.
[literal part for sequence 2 (>= 0 bytes)]
[literal part for sequence 1 (>= 0 bytes)]
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
    - The bytes that follow correspond to a series of disjoint "sequences".
    - Each sequence contains four pieces of data: 
        - Literal length.
        - Raw literals.
        - Match length.
        - Offset to the position behind us from where the match begins.
    - The decompressor will read these disjoint sequences from left to right, and perform two operations for each sequence:
        - Append the run of raw literals to the bytes already written in the decompression buffer.
        - Append the specified match to the bytes already written in the decompression buffer.
    - Each sequence is stored in the following format:
        - A "control part" spanning 4 bytes, which contains the literal length, match length, and match offset for the sequence. The 4 bytes form a little-endian unsigned 32-bit integer, decomposed as: bits 0-19 (lowest) = match offset, bits 20-25 = match length code, bits 26-31 (highest) = literal length.
            - Let `m` be the unsigned 6-bit integer derived from the match length bits and `l'` be the unsigned 6-bit integer derived from the literal length bits.
            - The match length is defined as `LUT.len_of[m]`, where `LUT` is a lookup table defined in [`src/format.h`](../src/format.h). `m = 0` is reserved and never appears in a valid stream.
            - The 20 match offset bits contain a 20-bit unsigned integer `d'`, with the actual distance to the match being defined as `d' + hashtab_lag + 1`. 
            - The literal length `l` is derived through the following algorithm:
                - If `l' < lit_lim`, then `l = l'`.
                - Otherwise, we read extra literal length bytes until we encounter the first byte with value `< 255`. We define `l = l' + 255 * (number of 255-value bytes) + (value of final byte)` (the value of a byte is just its interpretation as an unsigned 8 bit integer here).
            - Note that each "control part" always contains the 4 bytes mentioned above.
        - A "raw literal part", which just contains the raw literal bytes to be copied by the decompressor when processing this sequence. This part contains literal bytes in the order in which they will be copied by the decompressor.
    - Now, the two parts for a sequence aren't stored contiguously. Instead, we have two separate streams for control parts and literal parts. To be precise:
        - The control part stream grows from left to right, and the literal part stream grows from right to left.
        - The complete sequence stream takes the following form: `[control part for sequence 1][control part for sequence 2]...[control part for sequence n][literal part for sequence n]...[literal part for sequence 2][literal part for sequence 1]`. 
    - At the end, we append the previously mentioned suffix of raw bytes. It is guaranteed that the number of literal bytes in this suffix are >= `literal_suffix`.
- Therefore, when `src_size > small_lim`, the complete bitstream has the following form:

```text
[src_size (8 bytes)]
[literal_suffix_cnt (8 bytes)]
[control part for sequence 1 (>= 4 bytes)]
[control part for sequence 2 (>= 4 bytes)]
.
.
.
[control part for sequence n ( >= 4 bytes)]
[literal part for sequence n (>= 0 bytes)]
.
.
.
[literal part for sequence 2 (>= 0 bytes)]
[literal part for sequence 1 (>= 0 bytes)]
[raw literal suffix bytes (>= literal_suffix bytes)]
```

Some constraints:

- If copying a match involves copying range `[x, x + n)` to `[y, y + n)`, then we must have `hashtab_lag + 1 <= y - x <= hashtab_lag + ndis`.
