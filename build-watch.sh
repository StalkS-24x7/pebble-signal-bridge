#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

docker run --rm \
  -v "$SCRIPT_DIR/watch-app:/pebble" \
  rebble/pebble-sdk \
  bash -c "cd /pebble && pebble build"

echo ""
echo "Built: watch-app/build/pebble.pbw"
