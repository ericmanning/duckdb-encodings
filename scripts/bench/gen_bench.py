#!/usr/bin/env python3
"""Generate CSV files for decode benchmarking.

Usage: gen_bench.py <kind> <output.csv> [target_megabytes]

Kinds:
  latin1        mostly-ASCII latin-1 (~0.1% of rows carry one high byte)
  latin1_heavy  French/German-style latin-1 prose (~11% high bytes)
  cp1251        Cyrillic windows-1251 (mostly high bytes)
  sjis          Shift-JIS mixing Japanese and ASCII words
"""
import sys

ASCII_WORDS = [
    "alpha", "bravo", "charlie", "delta", "echo", "foxtrot", "golf", "hotel",
    "india", "juliet", "kilo", "lima", "mike", "november", "oscar", "papa",
    "quebec", "romeo", "sierra", "tango", "uniform", "victor", "whiskey",
]
ACCENTED = ["caf\xe9", "na\xefve", "M\xfcller", "Fran\xe7ois", "Z\xfcrich"]
HEAVY_WORDS = [
    "le", "café", "était", "très", "élevé", "Müller", "Straße", "über", "garçon", "naïve", "the", "and",
    "données", "fichier", "größe", "für", "déjà", "où", "value", "record", "nombre", "année", "système",
]
CYRILLIC_WORDS = ["привет", "мир", "данные", "Москва", "кодировка", "файл", "строка", "значение"]
SJIS_WORDS = ["東京", "データ", "日本語", "文字", "テスト", "ファイル", "alpha", "bravo", "charlie", "delta", "echo"]


def latin1_row(i):
    name = ACCENTED[i % len(ACCENTED)] if i % 1000 == 999 else ASCII_WORDS[i % len(ASCII_WORDS)]
    desc = "-".join(ASCII_WORDS[(i + j) % len(ASCII_WORDS)] for j in range(5))
    return name, desc


def words_row(words, desc_len, stride):
    def row(i):
        desc = " ".join(words[(i * stride + j) % len(words)] for j in range(desc_len))
        return words[i % len(words)], desc
    return row


KINDS = {
    "latin1": ("latin-1", latin1_row),
    "latin1_heavy": ("latin-1", words_row(HEAVY_WORDS, 8, 7)),
    "cp1251": ("cp1251", words_row(CYRILLIC_WORDS, 5, 1)),
    "sjis": ("shift_jis", words_row(SJIS_WORDS, 5, 1)),
}


def main():
    if len(sys.argv) not in (3, 4) or sys.argv[1] not in KINDS:
        sys.exit(__doc__)
    kind, out = sys.argv[1], sys.argv[2]
    target = int(sys.argv[3]) * 1_000_000 if len(sys.argv) == 4 else 300_000_000
    encoding, make_row = KINDS[kind]
    written = 0
    rows = 0
    with open(out, "w", encoding=encoding, newline="") as f:
        f.write("id,name,description,value\n")
        buf = []
        while written < target:
            name, desc = make_row(rows)
            row = f"{rows},{name},{desc},{rows * 37 % 100000}\n"
            buf.append(row)
            written += len(row.encode(encoding))
            rows += 1
            if len(buf) >= 50000:
                f.write("".join(buf))
                buf = []
        f.write("".join(buf))
    print(f"rows={rows} bytes~{written}")


if __name__ == "__main__":
    main()
