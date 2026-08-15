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


def load_emoji_dict(paths: list[Path]) -> dict[str, list[str]]:
    entries: dict[str, list[str]] = {}

    for path in paths:
        if not path:
            continue
        with path.open("r", encoding="utf-8") as fp:
            for raw in fp:
                line = raw.strip()
                if not line or line.startswith("#"):
                    continue
                parts = line.split("\t", 1)
                if len(parts) != 2:
                    continue
                key = parts[0].strip()
                if not key:
                    continue
                values = parts[1].strip().split()
                emojis: list[str] = []
                for value in values:
                    if value == key:
                        continue
                    if value not in emojis:
                        emojis.append(value)
                if not emojis:
                    continue
                bucket = entries.setdefault(key, [])
                for emoji in emojis:
                    if emoji not in bucket:
                        bucket.append(emoji)

    return entries


def write_compact(entries: dict[str, dict[str, int]], output: Path,
                  max_candidates: int, emoji_entries: dict[str, list[str]],
                  max_emoji_per_word: int, max_emoji_per_key: int) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("w", encoding="utf-8", newline="\n") as fp:
        fp.write("# K230 phone UI compact Pinyin candidates\n")
        fp.write("# Source: rime-pinyin-simp/pinyin_simp.dict.yaml\n")
        fp.write("# Emoji source: rime-emoji/opencc/emoji_word.txt, emoji_category.txt\n")
        fp.write("# License: Apache-2.0 for pinyin data, LGPL-3.0 for rime-emoji data\n")
        fp.write("# Format: compact_pinyin<TAB>candidate candidate ...\n")
        for key in sorted(entries):
            ranked = sorted(entries[key].items(),
                            key=lambda item: (-item[1], len(item[0]), item[0]))
            candidates: list[str] = []
            emoji_count = 0
            for word, _weight in ranked[:max_candidates]:
                if word not in candidates:
                    candidates.append(word)
                if emoji_count >= max_emoji_per_key:
                    continue
                for emoji in emoji_entries.get(word, [])[:max_emoji_per_word]:
                    if emoji not in candidates:
                        candidates.append(emoji)
                        emoji_count += 1
                        if emoji_count >= max_emoji_per_key:
                            break
            if candidates:
                fp.write(f"{key}\t{' '.join(candidates)}\n")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path,
                        help="Rime pinyin_simp.dict.yaml source")
    parser.add_argument("output", type=Path,
                        help="Generated compact dictionary")
    parser.add_argument("--max-candidates", type=int, default=54,
                        help="Candidates kept for each compact pinyin key")
    parser.add_argument("--emoji", type=Path, action="append", default=[],
                        help="rime-emoji OpenCC text dictionary")
    parser.add_argument("--max-emoji-per-word", type=int, default=3,
                        help="Emoji candidates added after a matching word")
    parser.add_argument("--max-emoji-per-key", type=int, default=18,
                        help="Total emoji candidates added for one pinyin key")
    args = parser.parse_args()

    entries = load_rime_dict(args.source)
    emoji_entries = load_emoji_dict(args.emoji)
    write_compact(entries, args.output, args.max_candidates, emoji_entries,
                  args.max_emoji_per_word, args.max_emoji_per_key)
    total = sum(len(v) for v in entries.values())
    emoji_total = sum(len(v) for v in emoji_entries.values())
    print(f"generated {len(entries)} keys, {total} source candidates, "
          f"{len(emoji_entries)} emoji keys, {emoji_total} emoji candidates "
          f"-> {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
