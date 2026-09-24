#!/usr/bin/env bash

set -ex

export CGO_ENABLED=1

if [ ! -f ../../scripts/kokoro/v1.1-zh/kokoro.onnx ]; then
  echo 'Export Kokoro v1.1-zh with pred_dur using scripts/kokoro/v1.1-zh/run.sh.' >&2
  exit 1
fi

go mod tidy
go build

./non-streaming-tts \
  --kokoro-model=../../scripts/kokoro/v1.1-zh/kokoro.onnx \
  --kokoro-voices=../../scripts/kokoro/v1.1-zh/voices.bin \
  --kokoro-tokens=../../scripts/kokoro/v1.1-zh/tokens.txt \
  --g2p-output=../../scripts/kokoro/fixtures/misaki-ai.json \
  --debug=1 \
  --output-filename=./test-kokoro-zh-en.wav
