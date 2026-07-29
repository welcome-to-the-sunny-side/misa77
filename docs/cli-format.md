The `misa` command-line tool produces compressed `.misa77` containers in a format that constitutes a few headers, followed by the raw compression stream (see [`format.md`](format.md))

## Compressed file (`.misa77`)

A `.misa77` file is a 6-byte container header followed by the raw compression stream:

```text
[4 bytes  magic = "MSA7" (0x4D 0x53 0x41 0x37)]
[1 byte   file format version]
[1 byte   flags]
[raw compression stream ...]
```

- `magic`: identifies the file and lets the tool reject input that is trivially non-misa77. Note that the internal decompress primitive does not perform any validation on the raw compressed stream, so this check only exists to gracefully handle cases where someone accidentally tries to decompress a non-`.misa77` file with `misa`.
- `version`: it is the container version ID. A build only decodes containers whose version it recognizes (currently `1` and `2`), anything else is rejected. The version describes the payload's stream format: `1` = light (levels -1..3), `2` = heavy (level 4). Older builds that only know version `1` therefore reject heavy files gracefully instead of misdecoding them. Readers may, but need not, cross-check the container version against the stream's own flags byte (the library's decompressor routes by the stream, not the container).
- `flags`: currently `0`, to be used in the future.