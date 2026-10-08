# Decode benchmarks

Scripts and results behind this branch's ASCII-run fast path in `src/generated_encoded_functions.cpp`.

## Running

```
python3 scripts/bench/gen_bench.py latin1 /tmp/bench_latin1.csv 300   # kinds: latin1, latin1_heavy, cp1251, sjis
scripts/bench/bench.sh ./build/release/duckdb /tmp/bench_latin1.csv iso-8859_1-1998
```

To time a loadable build, pass the official release CLI matching its DuckDB version and the extension path:

```
scripts/bench/bench.sh ./duckdb-v1.5.6 /tmp/bench_latin1.csv iso-8859_1-1998 path/to/encodings.duckdb_extension
```

Each run prints a row count and hash fingerprints; they must match across builds.

## What the branch changes

Upstream's "Index decode tables" rewrite replaced the per-byte binary search with a per-codec first-byte index. This
branch adds one thing on top: when bytes 0x00-0x7F all decode to themselves and start no multi-byte sequence, runs of
ASCII are found 8 bytes at a time and copied with `memcpy`. Every other byte goes through upstream's path.

An earlier version of this branch also had its own 256-entry direct-lookup table for single-byte codecs. It was
dropped: on latin-1 data it gave no gain over ASCII-run copying on top of upstream's index (it only helps text that is
mostly non-ASCII, e.g. Cyrillic, by ~1.4x).

## Results

Apple Silicon macOS, release builds against DuckDB v1.5.6, 3 runs each, identical fingerprints in every row.

In-tree builds (A = upstream main, B = upstream + direct-lookup table + ASCII runs, C = upstream + ASCII runs, which is
what this branch ships):

| file                                            |    A    |    B    |    C    |
|-------------------------------------------------|--------:|--------:|--------:|
| latin1, 300 MB, 0.1% accented                   | 0.75 s  | 0.124 s | 0.125 s |
| latin1_heavy, 200 MB, 11% high bytes            | 0.56 s  | 0.22 s  | 0.23 s  |
| cp1251, 200 MB, Cyrillic                        | 0.65 s  | 0.39 s  | 0.55 s  |
| sjis, 200 MB (`ignore_errors=true`, see below)  | 1.92 s  | 1.88 s  | 1.97 s  |
| NC voter file, 4.3 GB / 9.2M rows, tab-delimited | 10.1 s  | 1.49 s  | 1.55 s  |

The official v1.5.6 release CLI with the `encodings` extension DuckDB ships for it (built from commit `06295e7`, the
old binary-search decoder) against this branch's CI-built `osx_arm64` extension:

| file                                  | shipped | this branch |
|---------------------------------------|--------:|------------:|
| latin1, 300 MB (`iso-8859_1-1998`)    | 11.1 s  | 0.12 s      |
| latin1_heavy, 200 MB                  | 7.9 s   | 0.24 s      |
| cp1251, 200 MB (`windows-1251-2000`)  | wrong output | 0.53 s |

DuckDB's built-in `encoding='latin-1'` took 15.9 s on the voter file and rejects bytes 0x80-0x9F, so it is not an
alternative for Windows-encoded data.

## Caveats

- The shipped v1.5.6 extension decodes plain ASCII as full-width characters for the `windows-125x-2000` tables (the
  header `id,name` becomes `ｉｄ，ｎａｍｅ`). Upstream fixed this in `cc0dd71`; this branch includes the fix.
- `iso-8859_1-1998` maps 0x80-0x9F to C1 control characters, not Windows-1252's curly quotes, euro sign, dashes, etc.
  Use `windows-1252` for Windows-encoded files; it gets the same ASCII fast path.
- DuckDB v1.5.6's CSV reader can corrupt a multi-byte character that straddles an internal buffer boundary (fixed in
  DuckDB v2.0, duckdb/duckdb#20598). That is why `sjis` needs `ignore_errors=true` above, and why
  `test/sql/decode_tiny_buffer.test` and `test/sql/decode_straddle.test` are skipped on this branch. Single-byte
  encodings are unaffected.
