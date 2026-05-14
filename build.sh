#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"

echo "=== ClawStory Build ==="

# Check dependencies
for cmd in cmake g++ pkg-config; do
    if ! command -v $cmd &>/dev/null; then
        echo "Error: $cmd not found. Install with: sudo apt install $cmd"
        exit 1
    fi
done

# Check libcurl
if ! pkg-config --exists libcurl 2>/dev/null; then
    echo "Error: libcurl not found. Install with: sudo apt install libcurl4-openssl-dev"
    exit 1
fi

# Check OpenSSL
if ! pkg-config --exists openssl 2>/dev/null; then
    echo "Error: OpenSSL not found. Install with: sudo apt install libssl-dev"
    exit 1
fi

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j"$(nproc)"

echo ""
echo "Build complete! Run: ./build/clawstory"
