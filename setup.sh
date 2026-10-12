#!/usr/bin/env bash
# Installs everything needed to build and flash this firmware. The Pico SDK,
# Arm toolchain and picotool go into .pico-sdk/ in this repo, using the same
# layout as Raspberry Pi's Pico VS Code extension. Safe to re-run: installed
# parts are skipped.
set -euo pipefail

SDK_VERSION=2.3.1
TOOLCHAIN_VERSION=14.2.rel1
TOOLCHAIN_DIR_NAME=14_2_Rel1
PICOTOOL_VERSION=2.3.0

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEPS_DIR="$REPO_DIR/.pico-sdk"
SDK_DIR="$DEPS_DIR/sdk/$SDK_VERSION"
TOOLCHAIN_DIR="$DEPS_DIR/toolchain/$TOOLCHAIN_DIR_NAME"
PICOTOOL_DIR="$DEPS_DIR/picotool/$PICOTOOL_VERSION"

case "$(uname -s)-$(uname -m)" in
    Darwin-arm64)  TOOLCHAIN_HOST=darwin-arm64 ;;
    Darwin-x86_64) TOOLCHAIN_HOST=darwin-x86_64 ;;
    Linux-x86_64)  TOOLCHAIN_HOST=x86_64 ;;
    Linux-aarch64) TOOLCHAIN_HOST=aarch64 ;;
    *) echo "Unsupported host: $(uname -sm)" >&2; exit 1 ;;
esac

echo "==> Host tools"
if [ "$(uname -s)" = Darwin ]; then
    if ! command -v brew >/dev/null; then
        echo "Homebrew is required (https://brew.sh)" >&2
        exit 1
    fi
    missing=()
    for formula in cmake ninja libusb pkgconf; do
        brew list --formula "$formula" >/dev/null 2>&1 || missing+=("$formula")
    done
    if [ ${#missing[@]} -gt 0 ]; then
        brew install "${missing[@]}"
    fi
else
    missing=()
    for cmd in git curl cmake ninja pkg-config c++; do
        command -v "$cmd" >/dev/null || missing+=("$cmd")
    done
    pkg-config --exists libusb-1.0 2>/dev/null || missing+=("libusb-1.0 headers")
    if [ ${#missing[@]} -gt 0 ]; then
        echo "Missing: ${missing[*]}" >&2
        echo "On Debian/Ubuntu: sudo apt install git curl cmake ninja-build pkg-config build-essential libusb-1.0-0-dev" >&2
        exit 1
    fi
fi

echo "==> FreeRTOS submodule"
git -C "$REPO_DIR" submodule update --init --recursive

if [ ! -f "$SDK_DIR/pico_sdk_init.cmake" ]; then
    echo "==> Pico SDK $SDK_VERSION"
    mkdir -p "$(dirname "$SDK_DIR")"
    rm -rf "$SDK_DIR.tmp"
    git clone --depth 1 --branch "$SDK_VERSION" https://github.com/raspberrypi/pico-sdk.git "$SDK_DIR.tmp"
    git -C "$SDK_DIR.tmp" submodule update --init --depth 1
    mv "$SDK_DIR.tmp" "$SDK_DIR"
fi

if [ ! -x "$TOOLCHAIN_DIR/bin/arm-none-eabi-gcc" ]; then
    echo "==> Arm GNU Toolchain $TOOLCHAIN_VERSION ($TOOLCHAIN_HOST)"
    mkdir -p "$TOOLCHAIN_DIR.tmp"
    curl -fL# "https://developer.arm.com/-/media/Files/downloads/gnu/$TOOLCHAIN_VERSION/binrel/arm-gnu-toolchain-$TOOLCHAIN_VERSION-$TOOLCHAIN_HOST-arm-none-eabi.tar.xz" \
        | tar -xJ -C "$TOOLCHAIN_DIR.tmp" --strip-components 1
    mv "$TOOLCHAIN_DIR.tmp" "$TOOLCHAIN_DIR"
fi

if [ ! -f "$PICOTOOL_DIR/picotool/picotoolConfig.cmake" ]; then
    echo "==> picotool $PICOTOOL_VERSION"
    work="$(mktemp -d)"
    git clone --depth 1 --branch "$PICOTOOL_VERSION" https://github.com/raspberrypi/picotool.git "$work/src"
    # picotool records its install prefix, so install straight to the final location
    rm -rf "$PICOTOOL_DIR"
    PICO_SDK_PATH="$SDK_DIR" PICO_TOOLCHAIN_PATH="$TOOLCHAIN_DIR" cmake -S "$work/src" -B "$work/build" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DPICOTOOL_FLAT_INSTALL=1 -DCMAKE_INSTALL_PREFIX="$PICOTOOL_DIR"
    cmake --build "$work/build"
    cmake --install "$work/build"
    rm -rf "$work"
fi

echo
echo "Dependencies are installed in $DEPS_DIR"
echo "Build and flash with the VS Code tasks, or from a shell:"
echo "  cmake -S . -B build-dual -G Ninja -DPICO_BOARD=pico -DDUAL_PROBE_PICO=ON \\"
echo "    -DPICO_SDK_PATH=$SDK_DIR -DPICO_TOOLCHAIN_PATH=$TOOLCHAIN_DIR \\"
echo "    -Dpicotool_DIR=$PICOTOOL_DIR/picotool"
echo "  cmake --build build-dual"
echo "  $PICOTOOL_DIR/picotool/picotool load -v -x build-dual/debugprobe_dual_on_pico.uf2 -f --vid 0x2e8a --pid 0x000c"
