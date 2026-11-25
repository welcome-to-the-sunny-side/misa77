A lot of constants will be referenced ahead. Refer to [`src/format.h`](../src/format.h) for more information about them.

Assumptions:

- The size of the raw file is `<= 1e15` bytes.
- Everything is little-endian.

The format is as follows:

- The first 7 bytes contain an unsigned 56-bit integer which is equal to the size of the uncompressed data in bytes. We denote this size by `n`. 
- The next (8th) byte contains `flags`.
- The low `format_bits` bits of `flags` hold the format ID, which dictates what the following format is.

| ID | constant | format | levels | valid `flags` |
|---|---|---|---|---|
| 0 | `format_light` | [Light](#light) | -1 to 3 | `0x00` |
| 1 | `format_heavy` | [Heavy](#heavy) | 4 | `0x01`, `0x05` |
| 2 | `format_entropy` | [Entropy](#entropy) | 5 | `0x02` |

ID 3 is unused, and a stream with it is invalid.

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

## Entropy

TLDR:
- The format uses a modified version of the heavy format's codebook structure (including repeat codes).
- On top, we add some combination of Huffman encoding on split streams, adaptive delta encoding, difference encoding for literals, etc. 

Constants referenced ahead are from namespace `entropy` in [`src/format.h`](../src/format.h) and [`src/codebook.h`](../src/codebook.h).

Every bit of `flags` other than the format ID is unused and must be 0. An entropy stream can therefore only have `flags` = `0x02`.

### Small mode

If `n <= small_lim`, the stream is the 8-byte header followed by the `n` original bytes, and nothing else.

If `n > small_lim`, the header is followed by the layout below.

### Layout

```text
[blocks]                          one or more blocks, back-to-back
[raw prefix: prefix_size bytes]   the first prefix_size original bytes, raw
[raw suffix: suffix_size bytes]   the last suffix_size original bytes, raw
```

The blocks produce output positions `[prefix_size, n - suffix_size)`. A decoder always knows the first `prefix_size` and last `suffix_size` original bytes by reading the last `prefix_size + suffix_size` bytes of the compressed payload.

Block boundaries are determined by making "cuts" at multiples of `block_size` on the original input. A block's original bytes then correspond to the original bytes between the corresponding adjacent cuts.

A few notes:

- The first block starts at `prefix_size` instead of 0.
- The last block ends at `n - suffix_size` (exclusive).
- One can observe that every block except the first and the last necessarily has `block_size` bytes (when decompressed).
- Given the index of a block and the size of the original data that was compressed, we can determine the size of the block in $O(1)$. Denote this length by `len`.

### Blocks

The first byte of a block is its `mode`, which must be one of:

| `mode` | name | meaning |
|---|---|---|
| 0 | `mode_vanilla` | coded block |
| 1 | `mode_sub` | coded block, literals are differences (see "SUB") |
| 2 | `mode_delta` | coded block, some cells are differences (see "DELTA") |
| 3 | `mode_sub_delta` | both SUB and DELTA are applied |
| 4 | `mode_stored` | stored block |

A "stored" block is the mode byte followed by the block's `len` original bytes.

A coded block takes the following form:

```text
[u8 mode]
[u24 cmd_cnt][u24 lit_cnt]           number of commands, number of literal bytes
[u24 o1_cnt][u24 o2_cnt][u24 o3_cnt] number of offsets stored in 1, 2 and 3 bytes
[u16 coded]                          bit s set <=> stream s is Huffman coded (else raw)
[strides]                            mode has mode_delta => one byte per cell (see "DELTA")
[u24 size per coded stream]          in stream order, only for streams whose bit in `coded` is set
[the ten streams]                    in stream order
```

The part up to and including `coded` takes `block_header_size` (18) bytes. Let `off_cnt = o1_cnt + o2_cnt + o3_cnt` and `wide_cnt = o2_cnt + o3_cnt`. The ten streams, and the number of bytes each of them holds (once decoded), are:

| # | stream | bytes | content |
|---|---|---|---|
| 0 | `s_cmd` | `cmd_cnt` | one command byte per command |
| 1 | `s_lit` | `lit_cnt` | literal bytes, in command order |
| 2 | `s_o1` | `o1_cnt` | the 1-byte offsets |
| 3 | `s_o2l` | `o2_cnt` | low byte of each 2-byte offset |
| 4 | `s_o2h` | `o2_cnt` | high byte of each 2-byte offset |
| 5 | `s_o3l` | `o3_cnt` | low byte of each 3-byte offset |
| 6 | `s_o3m` | `o3_cnt` | middle byte of each 3-byte offset |
| 7 | `s_o3h` | `o3_cnt` | high byte of each 3-byte offset |
| 8 | `s_f1` | `ceil(off_cnt / 8)` | one bit per offset: set => 2 or 3 bytes |
| 9 | `s_f2` | `ceil(wide_cnt / 8)` | one bit per 2/3-byte offset: set => 3 bytes |

Streams 0-7 should be fairly self-evident. For streams 8 and 9, see "Deriving offset byte count" ahead.

A raw stream is just, well, stored raw. Coded streams are described ahead in "Huffman coded streams".

A coded block is valid only if all of the following hold:

- `1 <= cmd_cnt <= len`, `lit_cnt <= len`, `off_cnt <= cmd_cnt` and `min_new_tok_match_len * off_cnt <= len`.
- Bits 10-15 of `coded` are 0, and a stream which holds 0 bytes is not coded.
- `s_f1` has exactly `wide_cnt` set bits, `s_f2` has exactly `o3_cnt` set bits, and the bits past the last flag in each are 0.
- The commands output exactly `len` bytes, and use exactly `lit_cnt` literal bytes and `off_cnt` offsets.

After the last block, exactly `prefix_size + suffix_size` bytes of the stream remain.

### Commands

A command byte, interpreted as an unsigned 8-bit integer, is an index into the table `codebook`. Each entry gives us a tuple `(kind, lit, match_len)`. There are two kinds:

| kind | `lit` | `match_len` | distance of the match |
|---|---|---|---|
| R | `<= max_lit` | `<= max_match_len` | the current repeat distance |
| N | `<= max_lit` | `>= min_new_tok_match_len` | the next offset |

We process each command `c` in the following manner (while simultaneously maintaining a "repeat distance" `d`):

- Retrieve `codebook[c] = (kind, lit, match_len)`.
- Append the next `lit` bytes of the literal stream to the output buffer. In a block with `mode_sub`, each of these is first added to the output byte `d` positions behind it (see "SUB").
- If `kind` is `N`, set `d` to the distance of the next offset (see "Offsets").
- Copy `match_len` bytes from `d` positions behind in the output buffer, to the output buffer.

A few notes:

- Every command outputs at least one byte.
- A command with only literals is an `R` command with `match_len = 0`, and a long match is continued with `R` commands with `lit = 0`.
- Commands never cross a block boundary.

#### Repeat distance

- There is one repeat distance per stream, and *not* per block. It is initialised as `initial_dis`, and modifications to it by `N` commands in a block persist as we go to blocks ahead.
- A stored block doesn't affect the repeat distance.

#### Distance validity

For a command with `match_len > 0`, let `p` be the output position after its literals. The stream is valid only if match distance `d` satisfies `d <= p` (besides lying in `[min_dis, max_dis]` of course).

### Offsets

An offset is an unsigned integer `v` stored in 1, 2 or 3 bytes, and means the distance `min_dis + v`:

| bytes | `v` | distance | streams |
|---|---|---|---|
| 1 | [0, 255] | [32, 287] | `s_o1` |
| 2 | [0, 65535] | [32, 65567] | `s_o2l`, `s_o2h` |
| 3 | [0, 2^24 - 1] | [32, 2^24 + 31] | `s_o3l`, `s_o3m`, `s_o3h` |

Offsets are stored in little-endian order.

#### Deriving offset byte count

Note that the number of offset bytes for an offset is determined by the flag streams.

- A flag stream is a sequence of bits, where bit `j` of the stream is bit `j % 8` of byte `j / 8` (so each byte is filled starting from its least significant bit).
- Number the block's offsets `0, 1, ...` in command order. Bit `j` of `s_f1` is clear if offset `j` takes 1 byte, and set if it takes 2 or 3.
- Number the offsets whose `s_f1` bit is set `0, 1, ...` in command order. Bit `w` of `s_f2` is clear if the `w`-th of them takes 2 bytes, and set if it takes 3.

The `k`-th 1-byte offset is byte `k` of `s_o1`. The `k`-th 2-byte offset has its low byte at byte `k` of `s_o2l` and its high byte at byte `k` of `s_o2h`, and likewise for 3-byte offsets.

### SUB

In a block with `mode_sub` set in `mode`, a literal byte in the literal stream is not the original byte `x`, but `x - y` (mod 256), where `y` is the output byte `d` positions behind it and `d` is the repeat distance at the start of the command (before this command, potentially being `N` type, changes it). The decoder adds `y` back when it decodes this command (note that it has access to the exact repeat distance the encoder encodes the byte with).

### DELTA

A block with `mode_delta` set in `mode` has "DELTA" enabled. What is "DELTA"? Read ahead.

Like we cut raw input bytes to get blocks, we now cut them at every multiple of `cell_size`.

A "cell" of a block is just the bytes between two adjacent cuts. 

A few notes:
- It's guaranteed that `block_size` is a multiple of `cell_size`, so each cell is completely contained within a single block.
- Just like with blocks, the raw prefix and suffix are excluded from cells, so the first and last cells (of the first and last block respectively) can be shorter than `cell_size`. 

Now, the block has one "stride" byte per cell, in cell order. A stride must be `<= max_stride`.

In a cell with stride `s > 0`, we do the following:

- The encoder sets `x[i] = x[i] - x[i - s]` (mod 256, where the RHS uses the original `x` bytes) for every position `i` of the cell.
- We then perform the rest of the encoding steps (LZ, Huffman etc).
- The decoder will first reverse LZ, Huffman, etc.
- It will then finally set `x[i] = x[i] + x[i - s]` (mod 256) in increasing order of `i` for the cell.

A cell with stride 0 is left as it is.

Since `x[i - s]` may lie in an earlier cell or an earlier block (or even in the raw prefix), this enforces the following requirements on any decoder:
- When block `i` is being decoded, block `i - 1` must be fully decoded.
- When cell `j` is being delta decoded, cell `j - 1` must have been fully delta decoded. 

A block with cells therefore looks like the following:

```text
[stride 0][stride 1]...[stride k - 1]    one byte per cell, in cell order (see "Blocks" for where these sit)

bs         c              c + cell_size                       be
|  cell 0  |    cell 1    |    cell 2    |  ...  | cell k - 1 |
```

Here, `[bs, be)` are the block's original positions, `c` is the first multiple of `cell_size` after `bs`, and `k` is the number of cells of the block.

Note that DELTA interacts with matches and SUB literals (as we first process commands (and simultaneously undo SUB), and only then undo DELTA). Therefore, inside a DELTA block, match and sub literal bytes are going to be dealing with the block's own bytes before the DELTA restore, and every earlier block's bytes *after* the DELTA restore (all earlier blocks must have been fully decoded by this time).

### Huffman coded streams

A Huffman coded stream which holds `m` (original) bytes takes the following form:

```text
[table]
[bit string 0][bit string 1]...[bit string payload_split - 1]
[u16 size of bit string 0][u16 size of bit string 1]...[u16 size of bit string payload_split - 2]
```

The bit string sizes are in bytes and rounded up. The last bit string takes whatever remains between the table and the sizes.

#### Table

The table gives a code length in `[0, max_code_len]` for each of the 256 byte values, where 0 means that the byte value doesn't occur. Its first byte determines the layout of the table:

- `table_flat` (0): 128 bytes follow, holding one 4-bit length (interpreted as a u4) per byte value. Byte value `a` is in byte `a / 2`, in the low 4 bits if `a` is even and the high 4 bits if odd.
- `table_sparse` (1): a 32-byte bitset follows (bit `a % 8` of byte `a / 8` is set if byte value `a` has a length), and then one 4-bit length per set bit, in increasing order of byte value, packed as above and padded to a whole byte.

#### Codes

We do not "transmit" codes (i.e. we do not store them in the compressed format). Instead, codes are unambiguously determined from a given set of code lengths using the following algorithm:

```text
c = 0
for l = 1 to max_code_len:
    for a = 0 to 255:
        if length[a] == l:
            code[a] = c        (an l-bit integer)
            c = c + 1
    c = 2 * c
```

So codes are handed out in increasing order of (length, byte value).

A few notes:

- The encoder's job is therefore to determine optimal code-lengths for each symbol (`sum(2^(-codelength))` must be 1, or else there must only be one symbol with a non-zero codelength, which must be 1), and transmit it to the decoder through the format. It of course also uses the derived codes to produce Huffman-coded bitstrings.
- The decoder receives these code-lengths (along with the bytes they belong to), unambiguously reconstructs the codes, and then Huffman decodes the bitstrings.

We number the bits of a bit string as follows: the first byte holds bits 0-7, the second byte holds bits 8-15, and so on. Within a byte, we go from the least significant bit to the most significant bit.

Codes are laid out on these bits one after the other, with no gaps. The first code starts at bit 0, and the most significant bit of each code comes first. If the last byte isn't full, its remaining bits are padding.

For example, with codes `A = 0`, `B = 10`, `C = 110` and `D = 111`, a bit string holding `B, C, A, D` is stored as the two bytes `0xCD 0x01`:

```text
bits in order:    1 0   1 1 0   0   1 1 1
bit number:       0 1   2 3 4   5   6 7 8

byte 0 (bits 0-7):
    bit:    7 6 5 4 3 2 1 0
    value:  1 1 0 0 1 1 0 1    = 0xCD
byte 1 (bit 8, then 7 padding bits) = 0x01
```

#### Bit strings

The stream's bytes are coded from last to first, so the byte at position `m - 1 - j` is the `j`-th symbol. We distribute symbols across bitstrings in a round-robin manner (so decodes can be done in parallel). Symbol `j` is in bit string `j % payload_split`. Within a bit string, codes are stored in decreasing order of the positions of their corresponding bytes in the original stream.

A coded stream is valid only if every bit string is used up exactly when we decode it, ie. the last code of each bit string must end inside the bit string's last byte (a partly used last byte counts as used).

### Compressed size bound

Any block can be a stored block, which takes `len + 1` bytes. If the compressed bytes of a block are larger than they originally were, we just store the block raw. Therefore, any input of `n` bytes has a valid stream of at most `compressed_size_bound(n)` = `n + header_size + 1 + n / block_size` bytes.
