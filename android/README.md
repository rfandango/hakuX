# Building hakuX

The app (this directory) builds no native code itself: it packs the native
core's libraries from `app/src/main/jniLibs`. Build the core first, copy its
libraries in, then build the APK.

## Toolchain

- Android SDK 36, Build Tools 36.1.0
- NDK 29.0.14206865
- CMake 3.20 or later, Ninja
- JDK 21

## Sources

- The FEX fork ([rfandango/FEX](https://github.com/rfandango/FEX), branch
  `hakuX`) is the submodule `external/fex`, with the FEX submodules it builds
  with:

  ```
  git submodule update --init external/fex
  git -C external/fex submodule update --init External/fmt External/range-v3 \
    External/rpmalloc External/unordered_dense External/xxhash Source/Common/cpp-optparse
  ```

  CMake `HAKUX_FEX_DIR` names another checkout instead.
- The core's third-party sources, outside the repository (CMake `NC_DEPS`):

| Path in `NC_DEPS` | Revision |
|---|---|
| `volk` | 1.4.304 |
| `glslang` | 16.1.0 |
| `spirv_reflect` | vulkan-sdk-1.4.328.0 |
| `vma` | v3.3.0 |
| `SDL3` | release-3.4.18 |
| `adrenotools` | 8fae8ce, with `tools/native/adrenotools-kgsl-context-priority.patch` applied (`git apply`): the KGSL draw context priority hook |
| `root/subprojects/nv2a_vsh_cpu` | 561fe80 |
| `libchdr` (optional: CHD images) | 47d7941 |

## Steps

From the repository root:

```
cmake -S native -B build/native/core -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=<ndk>/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 -DCMAKE_BUILD_TYPE=Release \
  -DNC_DEPS=<native-deps>
ninja -C build/native/core hakuxcore
bash tools/native/app-libs.sh
cd android
./gradlew assembleDebug
```

`app-libs.sh` copies the core, SDL3 and adrenotools' hook libraries into
`app/src/main/jniLibs`, stripped. Run it again after each core build: the
APK packs whatever is there.

A release build (`assembleRelease`) is signed with the keystore that
`key.properties` names (`storeFile`, `storePassword`, `keyAlias`,
`keyPassword`).

## Host tests

The HLE layer's pure modules have host tests (any C compiler with CMake).
Use the dependency directory from the native build:

```
cmake -S tests/hle -B build/hle-tests -G Ninja -DNC_DEPS=<native-deps>
ninja -C build/hle-tests
ctest --test-dir build/hle-tests --output-on-failure
```

## Debug builds

Debug builds also carry the Khronos validation layer, which Android loads
on request (see [app/src/debug/jniLibs/arm64-v8a/README.md](app/src/debug/jniLibs/arm64-v8a/README.md)).
