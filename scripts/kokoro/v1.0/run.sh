#!/usr/bin/env bash
# Copyright    2025  Xiaomi Corp.        (authors: Fangjun Kuang)

set -ex

if [ ! -d Kokoro-82M ]; then
  git clone https://huggingface.co/hexgrad/Kokoro-82M
fi

# https://huggingface.co/hexgrad/Kokoro-82M/tree/main/voices
#
# af -> American female
# am -> American male
# bf -> British female
# bm -> British male

has_pred_dur() {
  python3 - "$1" <<'PY'
import sys

import onnx

model = onnx.load(sys.argv[1], load_external_data=False)
outputs = {output.name: output for output in model.graph.output}
duration = outputs.get("pred_dur")
valid = (
    "audio" in outputs
    and duration is not None
    and duration.type.tensor_type.elem_type == onnx.TensorProto.INT64
)
sys.exit(0 if valid else 1)
PY
}

if [ ! -f ./kokoro.onnx ] || ! has_pred_dur ./kokoro.onnx; then
  python3 ./export_onnx.py
  rm -f ./.add-meta-data.done ./kokoro.int8.onnx
fi

if [ ! -f ./.add-meta-data.done ] || [ ./kokoro.onnx -nt ./.add-meta-data.done ]; then
  python3 ./add_meta_data.py
  touch ./.add-meta-data.done
fi

if [ ! -f ./kokoro.int8.onnx ] ||
   [ ./kokoro.onnx -nt ./kokoro.int8.onnx ] ||
   ! has_pred_dur ./kokoro.int8.onnx; then
  python3 ./dynamic_quantization.py
fi

if [ ! -f ./tokens.txt ]; then
  ./generate_tokens.py
fi

if [ ! -f ./voices.bin ]; then
  ./generate_voices_bin.py
fi

./test.py
ls -lh
