# Introduction

This directory is for kokoro v1.1-zh.

See also https://huggingface.co/hexgrad/Kokoro-82M-v1.1-zh

`run.sh` exports `kokoro.onnx` with `audio` and per-token `pred_dur` outputs.
Existing ONNX and quantized files without `pred_dur` are regenerated. It
generates `tokens.txt` and `voices.bin`, with no lexicon or G2P step.

Run `uv sync` in this directory to install the export dependencies from
`pyproject.toml`. This is a script-only project. `test.py` and
`generate_samples.py` also need Python bindings built from this sherpa-onnx
checkout; the published wheel does not yet provide
`generate_from_phonemes`.

Obtain `G2pOutput` in the application through misaki-rs. Pass its aggregate
`phonemes` and each `span.phonemes` to
`sherpa_onnx.PhonemeInput(phonemes=..., spans=[sherpa_onnx.PhonemeSpan(phonemes=...), ...])`,
then call `tts.generate_from_phonemes(input, sid=0, speed=1.0)`. Kokoro's
`audio.span_alignments` has one entry per supplied span with the original and
inferred phonemes and start/end timestamps in seconds. If a span has no
supported tokens, its inferred phonemes are empty and its timestamps are
`-1`.

`test.py` exercises both supplied misaki-rs examples with the full and
quantized models, plus token rebatching across the model's input limit.
