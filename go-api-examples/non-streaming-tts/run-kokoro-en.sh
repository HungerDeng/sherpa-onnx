#!/usr/bin/env bash

set -ex

export CGO_ENABLED=1

if [ ! -f ../../scripts/kokoro/v1.0/kokoro.onnx ]; then
  echo 'Export Kokoro v1.0 with pred_dur using scripts/kokoro/v1.0/run.sh.' >&2
  exit 1
fi

go mod tidy
go build

./non-streaming-tts \
  --kokoro-model=../../scripts/kokoro/v1.0/kokoro.onnx \
  --kokoro-voices=../../scripts/kokoro/v1.0/voices.bin \
  --kokoro-tokens=../../scripts/kokoro/v1.0/tokens.txt \
  --g2p-output=../../scripts/kokoro/fixtures/misaki-price.json \
  --debug=1 \
  --output-filename=./test-kokoro-en.wav
