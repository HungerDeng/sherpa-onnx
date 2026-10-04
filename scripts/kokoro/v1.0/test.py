#!/usr/bin/env python3
# Copyright    2025  Xiaomi Corp.        (authors: Fangjun Kuang)
"""Exercise the Kokoro SDK with phonemes and spans returned by misaki-rs."""

import json
import math
from pathlib import Path

import numpy as np
import sherpa_onnx


FIXTURE_DIR = Path(__file__).resolve().parents[1] / "fixtures"


def load_fixture(name):
    output = json.loads((FIXTURE_DIR / f"misaki-{name}.json").read_text())
    return name, output["phonemes"], tuple(
        span["phonemes"] for span in output["spans"]
    )


FIXTURES = (load_fixture("price"), load_fixture("ai"))


def make_input(phonemes, span_phonemes):
    return sherpa_onnx.PhonemeInput(
        phonemes=phonemes,
        spans=[sherpa_onnx.PhonemeSpan(phonemes=p) for p in span_phonemes],
    )


def check_audio(tts, name, phonemes, span_phonemes, sid):
    audio = tts.generate_from_phonemes(
        make_input(phonemes, span_phonemes), sid=sid, speed=1.0
    )
    assert audio.sample_rate == 24000, (name, audio.sample_rate)
    assert len(audio.samples) > 0, name
    assert np.isfinite(audio.samples).all(), name

    alignments = audio.span_alignments
    assert alignments is not None, name
    assert len(alignments) == len(span_phonemes), (
        name,
        len(alignments),
        len(span_phonemes),
    )
    duration = len(audio.samples) / audio.sample_rate
    previous_end = 0.0
    for index, (alignment, supplied) in enumerate(zip(alignments, span_phonemes)):
        assert alignment.original_phonemes == supplied, (name, index, alignment)
        assert alignment.inferred_phonemes == supplied, (name, index, alignment)
        assert math.isfinite(alignment.start_ts), (name, index, alignment)
        assert math.isfinite(alignment.end_ts), (name, index, alignment)
        assert previous_end <= alignment.start_ts <= alignment.end_ts, (
            name,
            index,
            alignment,
        )
        assert alignment.end_ts <= duration + 1 / audio.sample_rate, (
            name,
            index,
            alignment,
            duration,
        )
        previous_end = alignment.end_ts

    print(f"{name}: {duration:.2f}s, {len(alignments)} aligned spans")
    return audio


def make_tts(model):
    config = sherpa_onnx.OfflineTtsConfig(
        model=sherpa_onnx.OfflineTtsModelConfig(
            kokoro=sherpa_onnx.OfflineTtsKokoroModelConfig(
                model=str(model), voices="./voices.bin", tokens="./tokens.txt"
            ),
            num_threads=2,
        )
    )
    assert config.validate(), model
    return sherpa_onnx.OfflineTts(config)


def main():
    # v1.0's bf_alice is speaker 20. The fourth span is the multiword price,
    # and punctuation has its own spans in both misaki-rs fixtures.
    sid = 20
    assert len(FIXTURES[0][2]) == 9
    assert len(FIXTURES[1][2]) == 11
    assert " " in FIXTURES[0][2][3]
    assert FIXTURES[0][2][4] == "."

    for model in (Path("kokoro.onnx"), Path("kokoro.int8.onnx")):
        assert model.is_file(), model
        tts = make_tts(model)
        for name, phonemes, spans in FIXTURES:
            check_audio(tts, f"{model}:{name}", phonemes, spans, sid)

        if model.name == "kokoro.onnx":
            # 510 style rows allow at most 509 payload tokens per inference.
            phonemes = " ".join([FIXTURES[0][1]] * 8)
            spans = FIXTURES[0][2] * 8
            assert len(phonemes) > 509
            check_audio(tts, f"{model}:rebatching", phonemes, spans, sid)

            # An unsupported phoneme keeps its caller-supplied alignment row.
            supplied = FIXTURES[0][1] + " 🧪"
            supplied_spans = FIXTURES[0][2] + ("🧪",)
            audio = tts.generate_from_phonemes(
                make_input(supplied, supplied_spans), sid=sid, speed=1.0
            )
            assert len(audio.samples) > 0
            assert len(audio.span_alignments) == len(supplied_spans)
            omitted = audio.span_alignments[-1]
            assert omitted.original_phonemes == "🧪"
            assert omitted.inferred_phonemes == ""
            assert omitted.start_ts == omitted.end_ts == -1.0


if __name__ == "__main__":
    main()
