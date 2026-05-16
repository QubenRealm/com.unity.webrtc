#!/bin/bash -eu

export LIBWEBRTC_DOWNLOAD_URL=https://github.com/Unity-Technologies/com.unity.webrtc/releases/download/M116-20250805/webrtc-android.zip
export SOLUTION_DIR=$(pwd)/Plugin~
export PLUGIN_DIR=$(pwd)/Runtime/Plugins/Android

# [realmview fork] Portable AAR-append helper. Git for Windows ships unzip but
# NOT zip, so the original `zip -g libwebrtc.aar jni/<abi>/libwebrtc.so` invocation
# below fails on Windows dev boxes. We try info-zip first (Linux/macOS CI path
# unchanged) and fall back to Python's stdlib zipfile module (Windows dev path),
# which behaves identically for our purpose: append a single file to an existing
# AAR with deflate compression preserved.
aar_append() {
    local aar="$1"
    local file="$2"
    if command -v zip >/dev/null 2>&1; then
        zip -g "$aar" "$file"
    elif command -v python >/dev/null 2>&1; then
        python -c "import zipfile, sys; z = zipfile.ZipFile(sys.argv[1], 'a', zipfile.ZIP_DEFLATED); z.write(sys.argv[2]); z.close()" "$aar" "$file"
    elif command -v python3 >/dev/null 2>&1; then
        python3 -c "import zipfile, sys; z = zipfile.ZipFile(sys.argv[1], 'a', zipfile.ZIP_DEFLATED); z.write(sys.argv[2]); z.close()" "$aar" "$file"
    else
        echo "ERROR: aar_append needs 'zip' (info-zip) OR 'python'/'python3' on PATH." >&2
        exit 1
    fi
}

BUILD_TYPE="${1:-release}"
if [ "$BUILD_TYPE" = "debug" ]; then
  CMAKE_BUILD_TYPE="Debug"
else
  CMAKE_BUILD_TYPE="Release"
fi

# Resolve Android NDK location robustly across shells/CI.
# Priority:
#   1) explicit ANDROID_NDK
#   2) ANDROID_NDK_ROOT
#   3) ANDROID_NDK_HOME
#   4) auto-detect latest Unity-bundled NDK on Windows/Git Bash
ANDROID_NDK_RESOLVED="${ANDROID_NDK:-${ANDROID_NDK_ROOT:-${ANDROID_NDK_HOME:-}}}"

if [ -z "$ANDROID_NDK_RESOLVED" ]; then
  # Git Bash path style (/c/...) and WSL path style (/mnt/c/...).
  # Pick the lexicographically highest editor folder, which works for installed
  # versions like 2021.3.x, 2022.3.x, 6000.0.x.
  UNITY_NDK_CANDIDATES_GITBASH=(/c/Program\ Files/Unity/Hub/Editor/*/Editor/Data/PlaybackEngines/AndroidPlayer/NDK)
  UNITY_NDK_CANDIDATES_WSL=(/mnt/c/Program\ Files/Unity/Hub/Editor/*/Editor/Data/PlaybackEngines/AndroidPlayer/NDK)

  for ndk_path in "${UNITY_NDK_CANDIDATES_GITBASH[@]}"; do
    [ -d "$ndk_path" ] || continue
    ANDROID_NDK_RESOLVED="$ndk_path"
  done
  for ndk_path in "${UNITY_NDK_CANDIDATES_WSL[@]}"; do
    [ -d "$ndk_path" ] || continue
    ANDROID_NDK_RESOLVED="$ndk_path"
  done

  if [ -n "$ANDROID_NDK_RESOLVED" ]; then
    echo "Auto-detected Unity NDK: $ANDROID_NDK_RESOLVED"
  fi
fi

if [ -z "$ANDROID_NDK_RESOLVED" ]; then
  echo "ERROR: Android NDK path is not set and Unity bundled NDK was not auto-detected." >&2
  echo "Set one of: ANDROID_NDK, ANDROID_NDK_ROOT, ANDROID_NDK_HOME." >&2
  echo "Example (Unity bundled NDK):" >&2
  echo "  export ANDROID_NDK=\"C:/Program Files/Unity/Hub/Editor/<version>/Editor/Data/PlaybackEngines/AndroidPlayer/NDK\"" >&2
  exit 1
fi

# [realmview fork] Resolve a cmake executable. Three runtime environments:
#   - Linux/macOS CI:      cmake is on PATH normally.
#   - Git Bash on Windows: cmake may or may not be on PATH (depends on whether
#                          a CMake install is registered system-wide).
#   - WSL bash on Windows: native ELF cmake is rarely installed; the Unity-
#                          bundled Windows cmake.exe is available under
#                          /mnt/c/... but WSL does NOT auto-resolve `cmake`
#                          to `cmake.exe` (unlike interactive interop), so
#                          a bare `cmake .` invocation in the script fails.
#
# We resolve once into an absolute CMAKE variable and rewrite later invocations
# below to use "$CMAKE" instead of `cmake`. Falling back silently to the
# baseline AAR (no libwebrtc.so) is the silent-failure mode that bit us on
# 2026-05-15 -- symptom on device was `DllNotFoundException: Unable to load
# DLL 'webrtc'`.
CMAKE=""
if command -v cmake >/dev/null 2>&1; then
  CMAKE="cmake"
else
  ANDROID_PLAYER_ROOT="$(dirname "$ANDROID_NDK_RESOLVED")"
  CMAKE_CANDIDATE=""
  if [ -d "$ANDROID_PLAYER_ROOT/SDK/cmake" ]; then
    for cmake_dir in "$ANDROID_PLAYER_ROOT/SDK/cmake"/*/bin; do
      if [ -x "$cmake_dir/cmake" ]; then
        CMAKE_CANDIDATE="$cmake_dir/cmake"
      elif [ -f "$cmake_dir/cmake.exe" ]; then
        # WSL can execute Windows .exe via interop, but only when invoked
        # explicitly with the .exe suffix. We rely on that here.
        CMAKE_CANDIDATE="$cmake_dir/cmake.exe"
      fi
    done
  fi
  if [ -n "$CMAKE_CANDIDATE" ]; then
    CMAKE="$CMAKE_CANDIDATE"
    echo "Auto-detected Unity-bundled cmake: $CMAKE"
    # Also expose it on PATH so any sub-tool that shells out to bare `cmake`
    # (e.g. CMake's own Ninja generator regenerating build files) finds it.
    export PATH="$(dirname "$CMAKE"):$PATH"
  else
    echo "ERROR: cmake is not on PATH and Unity-bundled cmake was not found under" >&2
    echo "       $ANDROID_PLAYER_ROOT/SDK/cmake" >&2
    echo "       Install cmake or set PATH to include one before running this script." >&2
    exit 1
  fi
fi

# Download LibWebRTC only once; reuse local cache on subsequent runs.
if [ ! -f "webrtc.zip" ]; then
  echo "Downloading libwebrtc archive..."
  curl -L "$LIBWEBRTC_DOWNLOAD_URL" > webrtc.zip
else
  echo "Using cached webrtc.zip"
fi

if [ ! -f "$SOLUTION_DIR/webrtc/lib/libwebrtc.aar" ]; then
  echo "Extracting libwebrtc archive..."
  mkdir -p "$SOLUTION_DIR/webrtc"
  unzip -o -d "$SOLUTION_DIR/webrtc" webrtc.zip
else
  echo "Using cached extracted libwebrtc at $SOLUTION_DIR/webrtc"
fi

cp -f "$SOLUTION_DIR/webrtc/lib/libwebrtc.aar" "$PLUGIN_DIR"

# If debug build, download android-binaries that contains Vulkan validation layer
if [ "$BUILD_TYPE" = "debug" ]; then
  if [ ! -d "$SOLUTION_DIR/android-binaries" ]; then
    wget -q --show-progress https://github.com/KhronosGroup/Vulkan-ValidationLayers/releases/download/vulkan-sdk-1.4.321.0/android-binaries-1.4.321.0.zip
    unzip -d "$(pwd)" android-binaries-1.4.321.0.zip
    mv "$(pwd)/android-binaries-1.4.321.0" $SOLUTION_DIR/android-binaries
  fi
fi

# Build UnityRenderStreaming Plugin
# Force Ninja to put long compile/link arguments into response files, regardless
# of platform default. This avoids the Windows 8191-char `cmd.exe` command-line
# cap which the libwebrtc link hits because of ~150 `-Wl,--undefined=Java_*`
# flags pulled in by the JNI export retention list (observed 2026-05-15:
# "The command line is too long. ninja: build stopped: subcommand failed.").
#
# Ninja reads this as an ENVIRONMENT variable, not a CMake -D define -- passing
# it as -D CMAKE_NINJA_FORCE_RESPONSE_FILE=ON triggers a "Manually-specified
# variables were not used by the project" warning and silently doesn't work.
export CMAKE_NINJA_FORCE_RESPONSE_FILE=1

cd "$SOLUTION_DIR"
for ARCH_ABI in "arm64-v8a" "x86_64"
do
  echo ""
  echo "===================================================================="
  echo "[build_plugin_android] Building libwebrtc.so for ABI: $ARCH_ABI"
  echo "===================================================================="
  "$CMAKE" . \
    -B build \
    -D CMAKE_SYSTEM_NAME=Android \
    -D CMAKE_ANDROID_API_MIN=24 \
    -D CMAKE_ANDROID_API=24 \
    -D CMAKE_ANDROID_ARCH_ABI=$ARCH_ABI \
    -D CMAKE_ANDROID_NDK="$ANDROID_NDK_RESOLVED" \
    -D CMAKE_BUILD_TYPE=$CMAKE_BUILD_TYPE \
    -D CMAKE_ANDROID_STL_TYPE=c++_static

  "$CMAKE" \
    --build build \
    --target WebRTCPlugin

  # libwebrtc.so move into libwebrtc.aar.
  # Validate the build actually produced the .so before continuing -- otherwise
  # the for-loop would happily skip ABIs and we'd ship a broken aar (silent
  # failure mode observed 2026-05-15 when cmake hit an incremental no-op).
  pushd $PLUGIN_DIR
  if [ ! -f "libwebrtc.so" ]; then
    echo "ERROR: cmake did not produce libwebrtc.so for $ARCH_ABI in $PLUGIN_DIR" >&2
    echo "       Check the cmake build output above. Aborting before aar mutation." >&2
    exit 1
  fi
  mkdir -p jni/$ARCH_ABI
  mv libwebrtc.so jni/$ARCH_ABI
  aar_append libwebrtc.aar jni/$ARCH_ABI/libwebrtc.so

  # Post-condition: verify the aar now contains the .so we just appended.
  # Catches the case where aar_append exits 0 but writes nothing.
  if ! python -c "import zipfile,sys; z=zipfile.ZipFile(sys.argv[1]); sys.exit(0 if sys.argv[2] in z.namelist() else 1)" libwebrtc.aar "jni/$ARCH_ABI/libwebrtc.so"; then
    echo "ERROR: libwebrtc.aar does not contain jni/$ARCH_ABI/libwebrtc.so after append" >&2
    echo "       The build looks successful but the aar is broken. Aborting." >&2
    exit 1
  fi
  echo "[build_plugin_android] Verified jni/$ARCH_ABI/libwebrtc.so present in libwebrtc.aar"

  # If debug build, add Vulkan validation layer
  if [ "$BUILD_TYPE" = "debug" ]; then
    cp $SOLUTION_DIR/android-binaries/$ARCH_ABI/libVkLayer_khronos_validation.so jni/$ARCH_ABI
    aar_append libwebrtc.aar jni/$ARCH_ABI/libVkLayer_khronos_validation.so
  fi
  rm -r jni
  popd
  rm -rf build
done

# Final report so the operator can confirm what landed in the aar without
# rummaging through it manually.
echo ""
echo "===================================================================="
echo "[build_plugin_android] Final libwebrtc.aar .so entries:"
python -c "import zipfile,sys; [print('  '+n) for n in zipfile.ZipFile(sys.argv[1]).namelist() if n.endswith('.so')]" "$PLUGIN_DIR/libwebrtc.aar"
echo "===================================================================="