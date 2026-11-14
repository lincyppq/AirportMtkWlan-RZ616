#!/bin/bash
# Build without Xcode, with only the Command Line Tools (xcode-select --install).
#
#   ./build.sh            native Wi-Fi kext  -> build/Release/AirportMtkWlan.kext
#   ./build.sh ethernet   Ethernet-style kext -> build/Release/SimpleMtkWlan.kext (+ tools/mtkctl)
#   ./build.sh all        both
#   ./build.sh awdltool   AWDL raw-channel test tool -> tools/awdltool/awdltool (awdl-airdrop branch)
#   ./build.sh awdlcap    AWDL capability switch tool -> tools/awdlcap/awdlcap (awdl-airdrop branch)
#
# CONFIG=Debug ./build.sh for a debug build.
# JOBS=1 ./build.sh limits clang to one compile at a time (16GB, no swap).
# The default is still the builder's os.cpu_count() parallelism.
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

job_args() {
  if [ -n "${JOBS:-}" ]; then
    printf '%s\n' --jobs "$JOBS"
  fi
}

build_native() {
  python3 -I tools/build-xcode-kext.py . AirportMtkWlan/build.json "$CONFIG" \
    "build/$CONFIG/native" --sdk MacKernelSDK $(job_args)
  rm -rf "build/$CONFIG/AirportMtkWlan.kext"
  mv "build/$CONFIG/native/AirportMtkWlan.kext" "build/$CONFIG/"
}

build_awdltool() {
  clang -O2 -Wall -Wextra -Wno-unused-parameter -Itools -Iinclude \
    tools/awdltool/awdltool.c -framework IOKit -framework CoreFoundation \
    -o tools/awdltool/awdltool
  echo "tools/awdltool/awdltool"
}

build_awdlcap() {
  clang -O2 -Wall -Itools tools/awdlcap/awdlcap.c \
    -framework IOKit -framework CoreFoundation -o tools/awdlcap/awdlcap
  echo "tools/awdlcap/awdlcap"
}

build_ethernet() {
  python3 -I tools/build-xcode-kext.py . SimpleMtkWlan "$CONFIG" \
    "build/$CONFIG/ethernet" --sdk MacKernelSDK \
    --define '__PRIVATE_SPI__' $(job_args)
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
  awdltool) build_awdltool; exit 0 ;;
  awdlcap) build_awdlcap; exit 0 ;;
  *) echo "usage: $0 [native|ethernet|all|awdltool|awdlcap]" >&2; exit 2 ;;
esac

for k in build/"$CONFIG"/*.kext; do
  echo "$k"
  shasum -a 256 "$k/Contents/MacOS/"* | sed 's/^/  sha256 /'
done
