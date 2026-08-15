#!/usr/bin/env python3
"""Generate a compact Pinyin candidate file for the K230 launcher."""

from __future__ import annotations

import argparse
from collections import defaultdict
from pathlib import Path
import re


PINYIN_RE = re.compile(r"^[a-züv ]+$")


def compact_key(pinyin: str) -> str:
    return pinyin.replace(" ", "").replace("ü", "v")


def load_rime_dict(path: Path) -> dict[str, dict[str, int]]:
    entries: dict[str, dict[str, int]] = defaultdict(dict)
    in_data = False

    with path.open("r", encoding="utf-8") as fp:
        for raw in fp:
            line = raw.strip()
            if not line:
                continue
            if line == "...":
                in_data = True
                continue
            if not in_data or line.startswith("#"):
                continue

            parts = line.split("\t")
            if len(parts) < 2:
                continue
            word = parts[0].strip()
            pinyin = parts[1].strip().lower()
            if not word or not PINYIN_RE.match(pinyin):
                continue
            try:
                weight = int(parts[2]) if len(parts) >= 3 else 0
            except ValueError:
                weight = 0

            key = compact_key(pinyin)
            if not key:
                continue
            old = entries[key].get(word)
            if old is None or weight > old:
                entries[key][word] = weight

    return entries


def write_compact(entries: dict[str, dict[str, int]], output: Path,
                  max_candidates: int) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("w", encoding="utf-8", newline="\n") as fp:
        fp.write("# K230 phone UI compact Pinyin candidates\n")
        fp.write("# Source: rime-pinyin-simp/pinyin_simp.dict.yaml\n")
        fp.write("# License: Apache-2.0\n")
        fp.write("# Format: compact_pinyin<TAB>candidate candidate ...\n")
        for key in sorted(entries):
            ranked = sorted(entries[key].items(),
                            key=lambda item: (-item[1], len(item[0]), item[0]))
            words = [word for word, _weight in ranked[:max_candidates]]
            if words:
                fp.write(f"{key}\t{' '.join(words)}\n")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path,
                        help="Rime pinyin_simp.dict.yaml source")
    parser.add_argument("output", type=Path,
                        help="Generated compact dictionary")
    parser.add_argument("--max-candidates", type=int, default=54,
                        help="Candidates kept for each compact pinyin key")
    args = parser.parse_args()

    entries = load_rime_dict(args.source)
    write_compact(entries, args.output, args.max_candidates)
    total = sum(len(v) for v in entries.values())
    print(f"generated {len(entries)} keys, {total} source candidates -> {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
