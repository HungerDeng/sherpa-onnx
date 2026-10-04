# Introduction

The v1.0 and v1.1-zh Kokoro exports use precomputed phonemes. The frontend
calls misaki-rs, then passes only the aggregate phonemes and each span's
phonemes to sherpa-onnx. Text normalization, G2P, and language selection are
outside the Kokoro SDK path.

```json
{
  "phonemes": "ˌeɪˈaɪ ɪz sˌoʊ ˈɔːsʌm.",
  "spans": [
    {"phonemes": "ˌeɪˈaɪ"},
    {"phonemes": "ɪz"},
    {"phonemes": "sˌoʊ"},
    {"phonemes": "ˈɔːsʌm"},
    {"phonemes": "."}
  ]
}
```

The resulting `GeneratedAudio` still contains `samples` and `sample_rate`.
Kokoro also fills `span_alignments` with one entry for every supplied span:

```cpp
struct GeneratedAudio {
  std::vector<float> samples;
  int32_t sample_rate;
  std::optional<std::vector<SpanAlignment>> span_alignments;
};

struct SpanAlignment {
  std::string original_phonemes;
  std::string inferred_phonemes;
  float start_ts;  // seconds in the returned waveform
  float end_ts;    // seconds in the returned waveform
};
```

Other offline TTS models leave `span_alignments` unset. Kokoro reconstructs
`inferred_phonemes` from token IDs sent to the model; unsupported symbols are
omitted. A span with no inferred tokens retains its input phonemes and has
`-1` timestamps. Long input is split at model token limits while preserving
span order and timing in the combined waveform.

The exports in [v1.0](v1.0/README.md) and [v1.1-zh](v1.1-zh/README.md)
include the model's `pred_dur` output for timestamp calculation. Their
`test.py` scripts use the two supplied misaki-rs examples in [fixtures](fixtures).
Older ONNX files without `pred_dur` must be re-exported before use.

To synthesize one of the supplied misaki-rs inputs with the CLI, first run
`scripts/kokoro/v1.0/run.sh` from its directory to export the model and
generate `voices.bin` and `tokens.txt`. Then, from the repository root:

```bash
./build/bin/sherpa-onnx-offline-tts \
  --kokoro-model=./scripts/kokoro/v1.0/kokoro.onnx \
  --kokoro-voices=./scripts/kokoro/v1.0/voices.bin \
  --kokoro-tokens=./scripts/kokoro/v1.0/tokens.txt \
  --kokoro-input-json=./scripts/kokoro/fixtures/misaki-price.json \
  --sid=20 \
  --output-filename=./generated-kokoro-price.wav
```

The CLI writes the WAV and prints its sample rate and one alignment row per
input span. `misaki-ai.json` can be used with the same command.

The v0.19 scripts are outside this refactor.
