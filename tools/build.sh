#!/bin/sh
# Build workaround: `pebble build` passes the system Python's sys.path to the
# SDK's (older) venv Python, which breaks waf when the two versions differ.
# This runs waf the same way pebble-tool does, minus the leaked paths.
set -e
SDK="$HOME/.pebble-sdk/SDKs/current"
VENV="$(readlink -f "$SDK/.venv")"
cd "$(dirname "$0")/.."
PATH="$SDK/toolchain/bin:$SDK/toolchain/arm-none-eabi/bin:$PATH" \
NODE_PATH="$SDK/node_modules" NOCLIMB=1 \
  env -u PYTHONPATH -u PYTHONHOME "$VENV/bin/python" "$SDK/sdk-core/pebble/waf" configure build "$@"
