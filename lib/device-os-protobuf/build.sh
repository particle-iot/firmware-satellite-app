#!/bin/bash

set -e

PROJECT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
LIB_DIR="$PROJECT_DIR/lib/device-os-protobuf"
PROTO_DIR="$LIB_DIR/device-os-protobuf"
DEST_DIR="$LIB_DIR/src"

NANOPB_DIR="$PROJECT_DIR/lib/nanopb/nanopb"
NANOPB_PLUGIN_DIR="$NANOPB_DIR/generator/protoc-gen-nanopb"

gen_proto() {
  echo "Compiling $1"
  protoc -I"$NANOPB_DIR/generator/proto" \
         -I"$PROTO_DIR" \
         -I"$(dirname "$1")" \
         --plugin="protoc-gen-nanopb=$NANOPB_PLUGIN_DIR" \
         --nanopb_out="${DEST_DIR}" "$1"
}

# Create a virtual environment
python3 -m venv "$LIB_DIR/.venv"
source "$LIB_DIR/.venv/bin/activate"

# Install dependencies
pip3 install protobuf

mkdir -p $DEST_DIR

# Compile protocol definitions
gen_proto "${PROTO_DIR}/cloud/describe.proto"
gen_proto "${PROTO_DIR}/cloud/cloud_new.proto"
