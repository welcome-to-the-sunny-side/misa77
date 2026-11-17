The `misa` command-line tool produces compressed `.misa77` containers in a format that constitutes a few headers, followed by the raw compression stream (see [`format.md`](format.md))

## Compressed file (`.misa77`)

A `.misa77` file is a 6-byte container header followed by the raw compression stream:

```text
[4 bytes  magic = "MSA7" (0x4D 0x53 0x41 0x37)]
[1 byte   file format version]
[1 byte   flags]
[raw compression stream ...]
```

- `magic`: identifies the file and lets the tool reject input that is trivially non-misa77. It exists to give a clear error when someone accidentally tries to decompress a non-`.misa77` file with `misa`. The stream itself is decoded in safe mode, so a corrupt or truncated file is rejected with an error instead of being misdecoded.
- `version`: it is the container version ID. A build only decodes containers whose version it recognizes (currently `1` and `3`), anything else is rejected. The version describes the payload's stream format: `1` = light (levels -1..3), `3` = heavy (level 4, 0.7.0 onwards). Version `2` was the heavy format of 0.4.0-0.6.0, which 0.7.0 replaced: current builds reject those files, and older builds reject version `3` files, instead of misdecoding them. Readers may, but need not, cross-check the container version against the stream's own flags byte (the library's decompressor routes by the stream, not the container).
- `flags`: currently `0`, to be used in the future.