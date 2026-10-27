# xclipse/: how the tested 1.2.0-xclipse-V13 APK was made

The APK that was tested on the device was **not** built from source. It is
the official hakuX 1.2.0 APK (`com.rfandango.haku_x`, versionCode 8,
sha256 `535ca8e86074607181bad7c14a1abd3e41bb806749da878fc0dd01c1638f94c4`)
with three changes, then re-signed with a different key:

| File | Change |
|---|---|
| `classes.dex` | `NativeCoreActivity.onCreate` first calls `Os.setenv("HAKUX_HLE_CAPS", "-bc,-fbfetch,-bindless,-vids,-ahb,-eds1,-eds2", true)` and `Os.setenv("HAKUX_HLE_QUIRKS", "+ds_same_load,+z_eq_w,+no_push_desc,+fresh_clear", true)` (inside a try/catch, before `invoke-super SDLActivity.onCreate`); the `"1.2.0"` version strings read `1.2.0-xclipse-V13`. Full smali diff: `smali_V13.diff`. |
| `AndroidManifest.xml` | versionCode 24, versionName `1.2.0-xclipse-V13` |
| `lib/arm64-v8a/libhakuxcore.so` | 6 byte patches, `patch_libhakuxcore.py` (input sha256 `b0e8ba72…b9`, output sha256 `597e4180…5a`) |

## The native byte patches (file offsets in the stock 1.2.0 `libhakuxcore.so`)

`.text` file offset equals its virtual address (0x10f280) in this build.

| Offset | Before | After | Meaning | Fix |
|---|---|---|---|---|
| 0x2202C | `uniform` | `buffer ` | GLSL `layout(set = 1, binding = 0, std140) uniform VshConst` → `buffer VshConst` (shader/vsh.c) | 2 |
| 0x66342 | `uniform` | `buffer ` | GLSL `… uniform FfConst` → `buffer FfConst` (shader/ff_gen.c) | 2 |
| 0x8F4DC | `08 00 00 00` | `09 00 00 00` | set-1 `VkDescriptorSetLayoutBinding.descriptorType`: UNIFORM_BUFFER_DYNAMIC → STORAGE_BUFFER_DYNAMIC | 2 |
| 0x864C | `08 00 00 00` | `09 00 00 00` | set-1 `VkWriteDescriptorSet.descriptorType` template, same change | 2 |
| 0x318AE8 | `08 01 80 d2` (`mov x8,#8`) | `28 01 80 d2` (`mov x8,#9`) | set-1 `VkDescriptorPoolSize.type`, same change | 2 |
| 0x31E144 | `73 07 00 b4` (`cbz x19, 0x31e230`) | `3b 00 00 14` (`b 0x31e230`) | inlined `vat_format()` (vk/draw.c), `HLE_VAT_S32K`: always take the SSCALED table (R16…_SSCALED, kind 0) instead of the vertex-program SINT table (R16…_SINT, kind 1) | 3 |

Fix 1 needs no native patch: it is only the `HAKUX_HLE_CAPS` environment
variable that `hw/xbox/hle/vk/vkdev.c` already reads.

The arena buffer already has `VK_BUFFER_USAGE_STORAGE_BUFFER_BIT`
(vk/guest_ram.c `hle_vk_arena_init`) and its windows are 256-byte aligned,
so the same dynamic offsets are valid for a storage buffer.

## Reproducing the APK

```sh
python3 xclipse/patch_libhakuxcore.py libhakuxcore.so libhakuxcore_v13.so   # native part only

SMALI_JAR=smali.jar BAKSMALI_JAR=baksmali.jar BUILD_TOOLS=$ANDROID_HOME/build-tools/<ver> \
KEYSTORE=my.jks KS_PASS=... KEY_ALIAS=... \
python3 xclipse/rebuild_apk.py hakuX-1.2.0.apk hakuX-1.2.0-xclipse-V13.apk
```

`rebuild_apk.py` was checked against the shipped V13 APK: every entry is
byte-identical except `classes.dex`, which disassembles to identical smali
(only the dex layout differs), and the signature (your key).
