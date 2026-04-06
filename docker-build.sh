#!/usr/bin/env bash
# Convenience wrapper around the LinkFPGA Docker build environment.
#
# Usage:
#   ./docker-build.sh                           # default: full build
#   ./docker-build.sh --num-tdm-ports 4 --num-physical-tdm-ports 2
#   ./docker-build.sh --shell                   # interactive shell instead
#
# The image is built once and cached. The current source tree is mounted
# read-write into /src so artifacts land back on the host.

set -euo pipefail

IMAGE=linkfpga-builder
HERE="$(cd "$(dirname "$0")" && pwd)"

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
  echo ">>> building docker image $IMAGE (one-time)..."
  docker build -t "$IMAGE" "$HERE"
fi

if [[ "${1:-}" == "--shell" ]]; then
  shift
  exec docker run --rm -it -v "$HERE":/src -w /src "$IMAGE" bash "$@"
fi

if [[ "${1:-}" == "--gen-pinout" ]]; then
  shift
  exec docker run --rm -v "$HERE":/src -w /src "$IMAGE" \
      python3 tools/gen_pinout.py "$@"
fi

exec docker run --rm -v "$HERE":/src -w /src "$IMAGE" \
    python3 build.py --build "$@"
