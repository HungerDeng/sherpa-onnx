#!/usr/bin/env python3
# Copyright    2025  Xiaomi Corp.        (authors: Fangjun Kuang)
"""
Generate samples for
https://k2-fsa.github.io/sherpa/onnx/tts/all/
"""

from pathlib import Path

import sherpa_onnx
import soundfile as sf

from generate_voices_bin import speaker2id

config = sherpa_onnx.OfflineTtsConfig(
    model=sherpa_onnx.OfflineTtsModelConfig(
        kokoro=sherpa_onnx.OfflineTtsKokoroModelConfig(
            model="./kokoro.onnx",
            voices="./voices.bin",
            tokens="./tokens.txt",
        ),
        num_threads=2,
        debug=True,
    ),
)

if not config.validate():
    raise ValueError("Please check your config")

tts = sherpa_onnx.OfflineTts(config)
# Phonemes and spans from a misaki-rs G2pOutput for
# "AI is so awesome. I can't live without it."
phoneme_input = sherpa_onnx.PhonemeInput(
    phonemes="ˌeɪˈaɪ ɪz sˌoʊ ˈɔːsʌm. aɪ kˈænt lˈɪv wɪðˈaʊt ɪt.",
    spans=[
        sherpa_onnx.PhonemeSpan(phonemes=p)
        for p in (
            "ˌeɪˈaɪ", "ɪz", "sˌoʊ", "ˈɔːsʌm", ".", "aɪ",
            "kˈænt", "lˈɪv", "wɪðˈaʊt", "ɪt", ".",
        )
    ],
)
output_dir = Path("./hf/kokoro/v1.1-zh/mp3")
output_dir.mkdir(parents=True, exist_ok=True)

for s, i in speaker2id.items():
    print(s, i, len(speaker2id))
    audio = tts.generate_from_phonemes(phoneme_input, sid=i, speed=1.0)

    sf.write(
        output_dir / f"{i}-{s}.mp3",
        audio.samples,
        samplerate=audio.sample_rate,
    )
