#!/usr/bin/env bash

set -ex

model_dir=../../scripts/kokoro/v1.0
for file in kokoro.onnx voices.bin tokens.txt; do
  if [ ! -f "$model_dir/$file" ]; then
    echo "Export Kokoro v1.0 with $model_dir/run.sh first (requires pred_dur): $model_dir/$file is missing" >&2
    exit 1
  fi
done

dart pub get

dart run \
  ./bin/kokoro-en.dart \
  --model "$model_dir/kokoro.onnx" \
  --voices "$model_dir/voices.bin" \
  --tokens "$model_dir/tokens.txt" \
  --sid 9 \
  --speed 1.0 \
  --output-wav kokoro-en-9.wav

ls -lh *.wav
