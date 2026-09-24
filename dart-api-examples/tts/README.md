# Introduction

This folder contains examples for text to speech with Dart API.

| File | Description|
|------|------------|
|[./bin/piper.dart](./bin/piper.dart)| Use a Piper tts model for text to speech. See [./run-piper.sh](./run-piper.sh)|
|[./bin/coqui.dart](./bin/coqui.dart)| Use a Coqui tts model for text to speech. See [./run-coqui.sh](./run-coqui.sh)|
|[./bin/zh.dart](./bin/zh.dart)| Use a Chinese VITS tts model for text to speech. See [./run-zh.sh](./run-zh.sh)|
|[./bin/zipvoice-zh-en.dart](./bin/zipvoice-zh-en.dart)| Use a ZipVoice Chinese/English zero-shot TTS model. See [./run-zipvoice-zh-en.sh](./run-zipvoice-zh-en.sh)|
|[./bin/kokoro-en.dart](./bin/kokoro-en.dart)| Generate Kokoro v1.0 audio from misaki-rs phonemes. See [./run-kokoro-en.sh](./run-kokoro-en.sh)|
|[./bin/kokoro-zh-en.dart](./bin/kokoro-zh-en.dart)| Generate Kokoro v1.1-zh audio from misaki-rs phonemes. See [./run-kokoro-zh-en.sh](./run-kokoro-zh-en.sh)|

This example package uses the Flutter/Dart binding in this checkout. Build its
native library from the same checkout before running. The Kokoro launch scripts
use ONNX files exported by `scripts/kokoro/v1.0/run.sh` and
`scripts/kokoro/v1.1-zh/run.sh`, which include
the `pred_dur` output needed for span timestamps. Both examples contain one of
the supplied misaki-rs phoneme fixtures. Pass `--g2p-output output.json` to a
Dart example to use a full `G2pOutput` JSON file; it forwards only `phonemes`
and `spans[].phonemes` to the SDK. The waveform samples, sample rate, and span
alignments are all available on the result.
