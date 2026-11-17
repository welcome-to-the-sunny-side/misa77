A lot of constants will be referenced ahead. Refer to [`src/format.h`](../src/format.h) for more information about them.

Assumptions:

- The size of the raw file is `<= 1e15` bytes.
- Everything is little-endian.

The format is as follows:

- The first 7 bytes contain an unsigned 56-bit integer which is equal to the size of the uncompressed data in bytes. We denote this size by `n`. 
- The next (8th) byte contains `flags`.
- The low `format_bits` bits of `flags` hold the format ID, which dictates what the following format is. ID `format_light` (0) is "light", and ID `format_heavy` (1) is "heavy".

## Light

Constants referenced ahead are from namespace `light` in [`src/format.h`](../src/format.h).

Every bit of `flags` other than the format ID is unused and must be 0. A light stream can therefore only have `flags` = `0x00`.

### Small mode

If `n <= small_lim`, the stream is the 8-byte header followed by the `n` original bytes, and nothing else.

If `n > small_lim`, the header is followed by the layout below.

### Layout

After the header, we have a count followed by three disjoint regions, laid out back-to-back. They take the following form:

```text
[u64 literal_suffix_cnt]                     8 bytes
[control stream]                             control part of each sequence, in sequence order
[literal stream]                             literal part of each sequence, in REVERSE sequence order
[literal suffix: literal_suffix_cnt bytes]   the last literal_suffix_cnt original bytes, raw
```

The control stream grows from left to right and the literal stream grows from right to left (starting right before the literal suffix).

A stream is decoded in the following manner:

1. Process the sequences in order (see "Sequences" below).
2. After the last sequence is processed, append the `literal_suffix_cnt` bytes of the literal suffix to the output in order.

For a valid misa77 stream, the sequences will have output `n - literal_suffix_cnt` bytes, and we will have output `n` bytes after the literal suffix.

### Sequences

Each sequence has a "control part" in the control stream and a "literal part" in the literal stream. The control part takes the following form:

```text
[token byte]             top 3 bits = l', bottom 5 bits = m'
[2 offset bytes]         unsigned 16-bit integer v
[extra length bytes]     present only if l' = lit_lim (see "Literal length" below)
```

We process each sequence in the following manner:

- Derive the literal length `l` from `l'` and the extra length bytes.
- Append the `l` bytes of the sequence's literal part to the output buffer. Within a literal part, the bytes are in the order in which they are output.
- Set `m = m' + min_match_len - 1` and `d = min_dis + v`.
- Copy `m` bytes from `d` positions behind in the output buffer, to the output buffer.

A few notes:

- Every sequence has a match, and `min_match_len <= m <= max_match_len`.
- A sequence's literal part can be empty.

#### Literal length

- If `l' < lit_lim`, then `l = l'`.
- Otherwise, we read extra length bytes until we encounter the first byte with value `< 255`, and `l = lit_lim + 255 * (number of 255-valued bytes) + (value of the final byte)`.

#### Distance validity

Let `p` be the output position after a sequence's literals. The stream is valid only if `d <= p`.
The two offset bytes can express any `d` in [`min_dis`, `max_dis`] = [33, 65568], and there is no other window limit in the format.

#### Trailing literal suffix

We must have `literal_suffix_cnt >= literal_suffix`, ie. every sequence must end at or before output position `n - literal_suffix`. This allows for unconditional vector-register-width over-writes and over-reads.

## Heavy

Constants referenced ahead are from namespace `heavy` in [`src/format.h`](../src/format.h) and [`src/codebook.h`](../src/codebook.h).
    
Bits 2-3 of `flags` indicate the "codebook" used. ID 0 is for table `codebook_a` from `codebook.h`, and 1 for `codebook_s`. Bits 4-7 are currently unused and must be 0. A heavy stream can therefore only have `flags` = `0x01` or `0x05`. 

### Small mode

If `n <= small_lim`, the stream is the 8-byte header followed by the `n` original bytes, and nothing else.

If `n > small_lim`, the header is followed by one or more blocks.

### Blocks

After the header, we have disjoint "blocks" laid out back-to-back. Each block has a header, which gives the size of each of the three disjoint streams that follow it. It takes the following form:

```text
[u24 tok_cnt][u24 off_cnt][u24 lit_cnt]    block header, 9 bytes (block_header_size)
[cmd stream: tok_cnt bytes]                one command byte per token
[off stream: off_cnt bytes]                offset bytes, in token order
[lit stream: lit_cnt bytes]                literal bytes, in token order, then trailing literals
```

A block is decoded in the following manner:

1. Process the `tok_cnt` command bytes in order (see "Tokens" below).
2. After the last token is processed, every remaining byte in the literal stream is a **trailing literal**. These are appended to the output in order.

Overall, we decode each block in order, and for a valid misa77 stream, we will have output `n` bytes by the time the last block finishes decoding.

### Tokens

Let `T` be the table selected by the relevant bits in `flags`. A command byte, interpreted as an unsigned 8-bit integer, is an index into `T`. Each entry in `T` gives us a tuple `(kind, lit, len)`.

There are five kinds of tokens:

| kind | offset bytes | `lit` | `len` | distance of the match |
|---|---|---|---|---|
| L | 0 | `>= 1` | 0 (no match) | N/A (the repeat distance is unchanged) |
| R | 0 | any | `>= 1` | the current repeat distance |
| S | 1 | any | `>= 1` | `min_dis + v`, `v` in [0, 255], so `d` in [33, 288] |
| N | 2 | any | `>= 1` | `min_dis + v`, `v` in [0, 65535], so `d` in [33, 65568] |
| F | 3 | any | `>= 1` | `min_dis + v`, `v` in [0, 2^24 - 1], so `d` in [33, 2^24 + 32] |

We process each command `c` in the following manner (while simultaneously maintaining a "repeat distance" `d`, which has no value at the start of the stream):

- Retrieve `T[c] = (kind, lit, len)`.
- Append the next `lit` bytes from the literal stream to the output buffer.
- If `len > 0`, then:
    - If `kind` is not `R`:
        - Load the next `offset_bytes(kind)` bytes of the offset stream as an integer `v`.
        - Set `d = min_dis + v`.
    - Copy `len` bytes from `d` positions behind in the output buffer, to the output buffer.

A few notes:

- Every token must output at least one byte into the output buffer.
- As of now, both `A` and `S` tables have power-of-two `lit`s for `L` commands, so incompressible data can be stored with little overhead.

#### Repeat distance

- There is one repeat distance per stream, and *not* per block, so modifications to it by S/N/F tokens in a block carry over to the next.  
- An `S`, `N` or `F` token must occur before the first `R` token (so the repeat distance is well defined).

#### Distance validity

For a token with a match, let `p` be the output position after its literals. The stream is valid only if `d <= p`.
There is no other window limit in the format (apart from the implicit limits imposed by the tables and the maximum input size). Any distance that the token's kind can express is valid if it satisfies `d <= p`.

#### Trailing literal suffix

Every token must end at or before output position `n - literal_suffix`. This allows for unconditional vector-register-width over-writes and over-reads.
