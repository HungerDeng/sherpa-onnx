#!/usr/bin/env bash
set -e

if [ ! -f ./kokoro-multi-lang-v1_0/model.onnx ]; then
  echo "Export Kokoro v1.0 with pred_dur using scripts/kokoro/v1.0/run.sh, then place model.onnx, voices.bin, and tokens.txt in ./kokoro-multi-lang-v1_0." >&2
  exit 1
fi

dotnet run
