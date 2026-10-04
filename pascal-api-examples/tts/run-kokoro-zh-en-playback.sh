#!/usr/bin/env bash

set -ex

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )
SHERPA_ONNX_DIR=$(cd $SCRIPT_DIR/../.. && pwd)

echo "SHERPA_ONNX_DIR: $SHERPA_ONNX_DIR"

if [[ ! -f ../../build/install/lib/libsherpa-onnx-c-api.dylib  && ! -f ../../build/install/lib/libsherpa-onnx-c-api.so && ! -f ../../build/install/lib/sherpa-onnx-c-api.dll ]]; then
  mkdir -p ../../build
  pushd ../../build
  cmake \
    -DCMAKE_INSTALL_PREFIX=./install \
    -DSHERPA_ONNX_ENABLE_PYTHON=OFF \
    -DSHERPA_ONNX_ENABLE_TESTS=OFF \
    -DSHERPA_ONNX_ENABLE_CHECK=OFF \
    -DBUILD_SHARED_LIBS=ON \
    -DSHERPA_ONNX_ENABLE_PORTAUDIO=OFF \
    ..

  cmake --build . --target install --config Release
  popd
fi

if [ ! -f ./kokoro-multi-lang-v1_0/model.onnx ]; then
  echo 'Export Kokoro v1.0 with pred_dur using scripts/kokoro/v1.0/run.sh and copy model.onnx, voices.bin, and tokens.txt into pascal-api-examples/tts/kokoro-multi-lang-v1_0.' >&2
  exit 1
fi

fpc \
  -dSHERPA_ONNX_USE_SHARED_LIBS \
  -Fu$SHERPA_ONNX_DIR/sherpa-onnx/pascal-api \
  -Fl$SHERPA_ONNX_DIR/build/install/lib \
  -Fl/usr/local/Cellar/portaudio/19.7.0/lib \
  ./kokoro-zh-en-playback.pas

# Please see ../portaudio-test/README.md
# for how to install portaudio on macOS

export LD_LIBRARY_PATH=$SHERPA_ONNX_DIR/build/install/lib:$LD_LIBRARY_PATH
export DYLD_LIBRARY_PATH=$SHERPA_ONNX_DIR/build/install/lib:$DYLD_LIBRARY_PATH

./kokoro-zh-en-playback
