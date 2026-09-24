#!/usr/bin/env bash

set -ex

model_dir=../../scripts/kokoro/v1.1-zh
for file in kokoro.onnx voices.bin tokens.txt; do
  if [ ! -f "$model_dir/$file" ]; then
    echo "Export Kokoro v1.1-zh with $model_dir/run.sh first (requires pred_dur): $model_dir/$file is missing" >&2
    exit 1
  fi
done

dart pub get

dart run \
  ./bin/kokoro-zh-en.dart \
  --model "$model_dir/kokoro.onnx" \
  --voices "$model_dir/voices.bin" \
  --tokens "$model_dir/tokens.txt" \
  --sid 45 \
  --speed 1.0 \
  --output-wav kokoro-zh-en-45.wav

ls -lh *.wav
