#!/usr/bin/env python3
"""Reproduce the native part of the hakuX 1.2.0-xclipse-V13 test build.

Input : lib/arm64-v8a/libhakuxcore.so from the official hakuX 1.2.0 APK
        (versionCode 8, sha256 below).
Output: the patched libhakuxcore.so that ships in 1.2.0-xclipse-V13.

These byte patches are what was tested on the device (Galaxy S24 FE,
Xclipse 940). The source-level equivalents are the commits on this branch
(see XCLIPSE_FIXES.md); the source form does what these do, gated to
Samsung Xclipse, and adds `readonly` to the storage-buffer blocks.

usage: patch_libhakuxcore.py <stock libhakuxcore.so> <out .so>
"""
import hashlib
import struct
import sys

STOCK_SHA256 = "b0e8ba726487e385d6282adce606da19dd6c605a31f1244fa3de4b360b0e00b9"
V13_SHA256 = "597e4180f51b9c656939070843d45514ddce41a10033d86a79684833d331435a"


def repl(buf, off, old, new):
    assert len(old) == len(new)
    assert bytes(buf[off:off + len(old)]) == old, f"unexpected bytes at {off:#x}"
    buf[off:off + len(new)] = new


def u32(buf, off, old, new):
    assert struct.unpack_from("<I", buf, off)[0] == old, f"unexpected word at {off:#x}"
    struct.pack_into("<I", buf, off, new)


def main(src, dst):
    raw = bytearray(open(src, "rb").read())
    if hashlib.sha256(raw).hexdigest() != STOCK_SHA256:
        sys.exit("input is not the stock hakuX 1.2.0 libhakuxcore.so")

    # Fix 2: vertex constants as a storage buffer instead of a uniform buffer.
    # GLSL text in .rodata (shader/vsh.c head(), shader/ff_gen.c hle_ff_vs_glsl):
    #   "... std140) uniform VshConst ..." -> "... std140) buffer  VshConst ..."
    #   "... std140) uniform FfConst ..."  -> "... std140) buffer  FfConst ..."
    repl(raw, 0x2202C, b"uniform", b"buffer ")
    repl(raw, 0x66342, b"uniform", b"buffer ")
    # set-1 VkDescriptorSetLayoutBinding.descriptorType (vk/draw.c hle_draw_init):
    #   VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC (8) -> STORAGE_BUFFER_DYNAMIC (9)
    u32(raw, 0x8F4DC, 8, 9)
    # set-1 VkWriteDescriptorSet.descriptorType template: 8 -> 9
    u32(raw, 0x864C, 8, 9)
    # set-1 VkDescriptorPoolSize.type: mov x8, #8 -> mov x8, #9
    u32(raw, 0x318AE8, 0xD2800108, 0xD2800128)

    # Fix 3: vat_format (vk/draw.c, inlined): for vertex programs, S32K
    # attributes take the SSCALED table (R16.._SSCALED, kind 0) instead of
    # the SINT one (R16.._SINT, kind 1).  cbz x19 (prog == 0) -> b (always).
    u32(raw, 0x31E144, 0xB4000773, 0x1400003B)

    if hashlib.sha256(raw).hexdigest() != V13_SHA256:
        sys.exit("result does not match the V13 library")
    open(dst, "wb").write(raw)
    print("wrote", dst)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
