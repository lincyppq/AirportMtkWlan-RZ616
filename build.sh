#!/bin/bash
# Build without Xcode, with only the Command Line Tools (xcode-select --install).
#
#   ./build.sh            native Wi-Fi kext  -> build/Release/AirportMtkWlan.kext
#   ./build.sh ethernet   Ethernet-style kext -> build/Release/SimpleMtkWlan.kext (+ tools/mtkctl)
#   ./build.sh all        both
#
# CONFIG=Debug ./build.sh for a debug build.
set -euo pipefail
cd "$(dirname "$0")"
ROOT=$(pwd)
CONFIG=${CONFIG:-Release}
WHAT=${1:-native}

# acidanthera/MacKernelSDK, pinned as in the upstream CI.
SDK_SHA=3f750085caa17ec3a7880f11c11bf4f48cd6a164
if [ ! -d MacKernelSDK/Headers ]; then
  git init -q MacKernelSDK
  git -C MacKernelSDK fetch -q --depth 1 https://github.com/acidanthera/MacKernelSDK.git "$SDK_SHA"
  git -C MacKernelSDK checkout -q --detach FETCH_HEAD
fi

build_native() {
  python3 -I tools/build-xcode-kext.py . AirportMtkWlan/build.json "$CONFIG" \
    "build/$CONFIG/native" --sdk MacKernelSDK
  rm -rf "build/$CONFIG/AirportMtkWlan.kext"
  mv "build/$CONFIG/native/AirportMtkWlan.kext" "build/$CONFIG/"
}

build_ethernet() {
  python3 -I tools/build-xcode-kext.py . SimpleMtkWlan "$CONFIG" \
    "build/$CONFIG/ethernet" --sdk MacKernelSDK \
    --define '__PRIVATE_SPI__'
  rm -rf "build/$CONFIG/SimpleMtkWlan.kext"
  mv "build/$CONFIG/ethernet/SimpleMtkWlan.kext" "build/$CONFIG/"
  clang -O1 -Wall -Wno-deprecated-declarations -Itools/ClientKit \
    tools/mtkctl.c tools/ClientKit/Api.c -framework IOKit -framework CoreFoundation \
    -o tools/mtkctl
}

case "$WHAT" in
  native) build_native ;;
  ethernet) build_ethernet ;;
  all) build_native; build_ethernet ;;
  *) echo "usage: $0 [native|ethernet|all]" >&2; exit 2 ;;
esac

for k in build/"$CONFIG"/*.kext; do
  echo "$k"
  shasum -a 256 "$k/Contents/MacOS/"* | sed 's/^/  sha256 /'
done
