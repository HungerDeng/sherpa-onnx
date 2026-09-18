#!/usr/bin/env python3
# Copyright    2025  Xiaomi Corp.

import argparse
from collections import Counter
from importlib.resources import files
from pathlib import Path
from typing import Iterable, List, Set, Tuple

from misaki import ja


def read_model_tokens(filename: Path) -> Set[str]:
    tokens = set()
    with filename.open(encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line:
                continue
            token_and_id = line.rsplit(" ", maxsplit=1)
            if len(token_and_id) == 2:
                tokens.add(token_and_id[0])
    return tokens


def is_japanese_character(c: str) -> bool:
    codepoint = ord(c)
    return (
        0x3400 <= codepoint <= 0x4DBF
        or 0x4E00 <= codepoint <= 0x9FFF
        or 0xF900 <= codepoint <= 0xFAFF
        or 0x20000 <= codepoint <= 0x2FA1F
        or 0x3040 <= codepoint <= 0x30FF
        or 0x31F0 <= codepoint <= 0x31FF
        or 0xFF66 <= codepoint <= 0xFF9F
        or codepoint in (0x3005, 0x303B)
    )


def read_japanese_vocabulary(words_filename: Path | None) -> List[str]:
    if words_filename is None:
        words_resource = files("misaki").joinpath("data/ja_words.txt")
        with words_resource.open(encoding="utf-8") as f:
            words = [line.strip() for line in f if line.strip()]
    else:
        with words_filename.open(encoding="utf-8") as f:
            words = [line.strip() for line in f if line.strip()]

    words = sorted(set(words))
    characters = sorted(
        {c for word in words for c in word if is_japanese_character(c)},
        key=ord,
    )
    return characters + [word for word in words if len(word) != 1]


def get_phonemes(word: str, g2p: ja.JAG2P, backend: str) -> str:
    phonemes, tokens = g2p(word)
    if backend != "jtalk":
        return phonemes

    if tokens is None:
        return ""

    result = ""
    for token in tokens:
        token_phonemes = token.phonemes
        if not token_phonemes:
            continue

        # Misaki inserts a separator between unchained Japanese tokens in its
        # combined phoneme result. Reproduce it from MToken metadata without
        # inspecting the pitch track or reconstructing phonemes from moras.
        if (
            result
            and result[-1] in ja.TAILS
            and token._.chain_flag is False
            and token_phonemes[0] not in ja.PUNCT_VALUES
            and not token_phonemes.startswith("ɴ")
        ):
            result += " "
        result += token_phonemes + token.whitespace

    return result.rstrip()


# currently, the entries with unsupported_symbols are not skipped. For example, in the generated cutlet lexicon, you can find these entries:
# ```
# バナヽ b a n a な
# マヽ m a ま
# 途切れ々々々 t o ɡ ʲ i ɾ e <space> 々 々 々
# ```
# However, the generated entries size of cutlet and jtalk are still different, because we skip the entries without phonemes.
#  - Shared vocabulary: 152,154 entries
#  - Cutlet: 1,700 empty pronunciations
#  - JTalk: 1,483 empty pronunciations
#  - Some entries are supported by only one backend: 310 JTalk-only and 93 Cutlet-only, producing the net difference of 217.
#  For example, Cutlet returns no pronunciation for ぁ, 也, and 侵. JTalk returns MTokens without phonemes for examples such as ゎ, 乃, and 乘.
def generate_japanese_lexicon(
    vocabulary: Iterable[str], backend: str, model_tokens: Set[str]
) -> Tuple[List[Tuple[str, str]], Counter[str], int]:
    version = "cutlet" if backend == "cutlet" else "pyopenjtalk"
    g2p = ja.JAG2P(version=version)
    lexicon = []
    unsupported_symbols: Counter[str] = Counter()
    num_skipped_entries = 0

    for index, word in enumerate(vocabulary, start=1):
        try:
            phonemes = get_phonemes(word, g2p, backend)
        except AssertionError as e:
            if backend != "jtalk":
                raise
            # Misaki raised before returning MTokens, so there are no token
            # phonemes to use for this entry.
            num_skipped_entries += 1
            print(f"{backend}: skipped {word!r}: {e}")
            continue
        output_phonemes = []
        for phoneme in phonemes:
            if (
                phoneme == "g"
                and phoneme not in model_tokens
                and "ɡ" in model_tokens
            ):
                # Misaki's JTalk backend uses ASCII g, while Kokoro's token
                # vocabulary uses the IPA Latin small script g.
                phoneme = "ɡ"
            elif phoneme not in model_tokens:
                unsupported_symbols[phoneme] += 1
            output_phonemes.append(phoneme)

        if output_phonemes:
            lexicon.append((word, "".join(output_phonemes)))
        else:
            num_skipped_entries += 1

        if index % 10000 == 0:
            print(f"{backend}: processed {index} entries")

    return lexicon, unsupported_symbols, num_skipped_entries


def print_unsupported_symbols(
    backend: str, unsupported_symbols: Counter[str]
) -> None:
    if not unsupported_symbols:
        print(f"{backend}: no unsupported symbols")
        return

    print(f"{backend}: unsupported symbols:")
    for symbol, count in sorted(
        unsupported_symbols.items(), key=lambda item: (-item[1], ord(item[0]))
    ):
        print(f"  {symbol!r} (U+{ord(symbol):04X}): {count}")


def save(filename: Path, lexicon: Iterable[Tuple[str, str]]) -> None:
    with filename.open("w", encoding="utf-8", newline="\n") as f:
        for word, phones in lexicon:
            tokens = ["<space>" if p == " " else p for p in phones]
            f.write(f"{word} {' '.join(tokens)}\n")


def get_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--tokens", type=Path, default=Path("tokens.txt"))
    parser.add_argument(
        "--words",
        type=Path,
        help="Override Misaki's packaged data/ja_words.txt",
    )
    parser.add_argument("--output-dir", type=Path, default=Path("."))
    parser.add_argument(
        "--backend",
        choices=("all", "cutlet", "jtalk"),
        default="all",
        help="Generate both lexicons or only the selected backend",
    )
    return parser.parse_args()


def main() -> None:
    args = get_args()
    model_tokens = read_model_tokens(args.tokens)
    if not model_tokens:
        raise ValueError(f"No model tokens found in {args.tokens}")
    vocabulary = read_japanese_vocabulary(args.words)
    args.output_dir.mkdir(parents=True, exist_ok=True)

    backends = ("cutlet", "jtalk") if args.backend == "all" else (args.backend,)
    for backend in backends:
        (
            lexicon,
            unsupported_symbols,
            num_skipped_entries,
        ) = generate_japanese_lexicon(
            vocabulary, backend, model_tokens
        )
        filename = args.output_dir / f"lexicon-ja-{backend}.txt"
        save(filename, lexicon)
        print(
            f"Saved {len(lexicon)} entries to {filename}; "
            f"retained {sum(unsupported_symbols.values())} occurrences of "
            f"{len(unsupported_symbols)} unsupported symbols; "
            f"skipped {num_skipped_entries} entries without phonemes"
        )
        print_unsupported_symbols(backend, unsupported_symbols)


if __name__ == "__main__":
    main()
