#!/usr/bin/env python3
"""Rebuild the hakuX 1.2.0-xclipse-V13 test APK from the official 1.2.0 APK.

What it changes (nothing else):
  * classes.dex: NativeCoreActivity.onCreate starts with
      Os.setenv("HAKUX_HLE_CAPS",   CAPS,   true)
      Os.setenv("HAKUX_HLE_QUIRKS", QUIRKS, true)
    (in a try/catch), and the "1.2.0" version strings say VERSION_NAME
    (smali_V13.diff is the resulting smali diff);
  * AndroidManifest.xml: versionCode 24, versionName VERSION_NAME;
  * lib/arm64-v8a/libhakuxcore.so: patch_libhakuxcore.py.
Then zipalign and apksigner with YOUR key (the developer's key is not
available, so the result must replace an uninstalled official app).

usage: rebuild_apk.py hakuX-1.2.0.apk out.apk
env:   SMALI_JAR, BAKSMALI_JAR, BUILD_TOOLS (dir with zipalign, apksigner),
       KEYSTORE, KS_PASS, KEY_ALIAS (passed to apksigner, never stored)
"""
from __future__ import annotations

import io
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import patch_libhakuxcore  # noqa: E402

VERSION_CODE = 24
VERSION_NAME = "1.2.0-xclipse-V13"
CAPS = "-bc,-fbfetch,-bindless,-vids,-ahb,-eds1,-eds2"
QUIRKS = "+ds_same_load,+z_eq_w,+no_push_desc,+fresh_clear"
CORE_SO = "lib/arm64-v8a/libhakuxcore.so"


def run(cmd):
    print("+", " ".join(str(c) for c in cmd))
    subprocess.check_call(cmd)


def replace_axml_utf16_string(man: bytes, old: str, new: str) -> bytes:
    man = bytearray(man)
    assert man[0:2] == b"\x03\x00"
    off = 8
    typ, hs, sz = struct.unpack_from("<HHI", man, off)
    assert typ == 0x0001
    sc, stc, flags, ss, sts = struct.unpack_from("<IIIII", man, off + 8)
    assert (flags & (1 << 8)) == 0
    assert stc == 0
    base = off + 8 + 20
    offsets = [struct.unpack_from("<I", man, base + 4 * i)[0] for i in range(sc)]
    str_data_base = off + ss
    pool_end = off + sz

    idx = None
    for i, o in enumerate(offsets):
        p = str_data_base + o
        n = struct.unpack_from("<H", man, p)[0]
        s = man[p + 2 : p + 2 + 2 * n].decode("utf-16le")
        if s == old:
            idx = i
            break
    if idx is None:
        raise ValueError(old)

    old_start = str_data_base + offsets[idx]
    old_end = str_data_base + offsets[idx + 1] if idx + 1 < sc else pool_end

    new_enc = struct.pack("<H", len(new)) + new.encode("utf-16le") + b"\x00\x00"
    old_span = old_end - old_start
    delta = len(new_enc) - old_span
    pad = (-delta) % 4
    new_enc = new_enc + (b"\x00" * pad)
    delta = len(new_enc) - old_span
    assert delta % 4 == 0

    out = bytearray()
    out += man[:old_start]
    out += new_enc
    out += man[old_end:]

    for j in range(idx + 1, sc):
        struct.pack_into("<I", out, base + 4 * j, offsets[j] + delta)
    struct.pack_into("<I", out, off + 4, sz + delta)
    struct.pack_into("<I", out, 4, struct.unpack_from("<I", out, 4)[0] + delta)
    return bytes(out)


def patch_manifest(man: bytes, version_code: int, version_name: str) -> bytes:
    out = bytearray(replace_axml_utf16_string(man, "1.2.0", version_name))
    needle = bytes([0x12, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x08, 0x00, 0x00, 0x10])
    i = out.find(needle)
    if i < 0:
        raise SystemExit("versionCode attr not found")
    struct.pack_into("<I", out, i + 12, version_code)
    return bytes(out)


def inject_setenv(smali_text: str, caps: str, quirks: str | None) -> str:
    # baksmali 3.x emits .registers N (total); older used .locals.
    m = re.search(
        r"(\.method public onCreate\(Landroid/os/Bundle;\)V\r?\n)"
        r"(\s*\.(?:locals|registers) (\d+)\r?\n)",
        smali_text,
    )
    if not m:
        raise SystemExit("onCreate not found")
    directive = ".registers" if ".registers" in m.group(2) else ".locals"
    count = int(m.group(3))
    # Need v0,v1,v2 free at method entry. .registers already includes p0/p1.
    new_count = max(count, 5 if directive == ".registers" else 3)

    lines = [
        f"    {directive} {new_count}",
        "",
        "    .line 10001",
        "    :try_start_xclipse",
        '    const-string v0, "HAKUX_HLE_CAPS"',
        "",
        f'    const-string v1, "{caps}"',
        "",
        "    const/4 v2, 0x1",
        "",
        "    invoke-static {v0, v1, v2}, Landroid/system/Os;->setenv(Ljava/lang/String;Ljava/lang/String;Z)V",
        "",
    ]
    if quirks:
        lines += [
            '    const-string v0, "HAKUX_HLE_QUIRKS"',
            "",
            f'    const-string v1, "{quirks}"',
            "",
            "    const/4 v2, 0x1",
            "",
            "    invoke-static {v0, v1, v2}, Landroid/system/Os;->setenv(Ljava/lang/String;Ljava/lang/String;Z)V",
            "",
        ]
    lines += [
        "    :try_end_xclipse",
        "    .catch Ljava/lang/Exception; {:try_start_xclipse .. :try_end_xclipse} :catch_xclipse",
        "",
        "    goto :xclipse_done",
        "",
        "    :catch_xclipse",
        "    move-exception v0",
        "",
        "    :xclipse_done",
        "",
    ]
    insertion = "\n".join(lines) + "\n"
    out = smali_text[: m.start(2)] + insertion + smali_text[m.end(2) :]
    if "Landroid/system/Os;->setenv" not in out:
        raise SystemExit("setenv not injected")
    if "invoke-super {p0, p1}, Lorg/libsdl/app/SDLActivity;->onCreate" not in out:
        raise SystemExit("invoke-super lost")
    return out


def patch_version_strings(smali_dir: Path, version_name: str, version_code: int):
    replacements = {
        '"1.2.0"': f'"{version_name}"',
        '"hakuX 1.2.0 (8)\\nsession started "': f'"hakuX {version_name} ({version_code})\\nsession started "',
        '"=== hakuX 1.2.0: the app, pid "': f'"=== hakuX {version_name}: the app, pid "',
        '"hakuX/1.2.0 (Android)"': f'"hakuX/{version_name} (Android)"',
    }
    for path in smali_dir.rglob("*.smali"):
        text = path.read_text(encoding="utf-8")
        orig = text
        for a, b in replacements.items():
            text = text.replace(a, b)
        if text != orig:
            path.write_text(text, encoding="utf-8")


def main(orig_apk: str, out_apk: str):
    env = os.environ
    smali, baksmali = env["SMALI_JAR"], env["BAKSMALI_JAR"]
    bt = Path(env["BUILD_TOOLS"])
    tmp = Path(tempfile.mkdtemp(prefix="hakux_v13_"))
    with zipfile.ZipFile(orig_apk) as z:
        infos = z.infolist()
        datas = {i.filename: z.read(i.filename) for i in infos}
    (tmp / "classes.dex").write_bytes(datas["classes.dex"])
    run(["java", "-jar", baksmali, "d", str(tmp / "classes.dex"), "-o", str(tmp / "smali")])
    nca = tmp / "smali" / "com" / "rfandango" / "haku_x" / "NativeCoreActivity.smali"
    nca.write_text(inject_setenv(nca.read_text(encoding="utf-8"), CAPS, QUIRKS), encoding="utf-8")
    patch_version_strings(tmp / "smali", VERSION_NAME, VERSION_CODE)
    run(["java", "-jar", smali, "a", str(tmp / "smali"), "--api", "28", "-o", str(tmp / "new.dex")])
    datas["classes.dex"] = (tmp / "new.dex").read_bytes()
    datas["AndroidManifest.xml"] = patch_manifest(datas["AndroidManifest.xml"], VERSION_CODE,
                                                  VERSION_NAME)
    (tmp / "stock.so").write_bytes(datas[CORE_SO])
    patch_libhakuxcore.main(str(tmp / "stock.so"), str(tmp / "v13.so"))
    datas[CORE_SO] = (tmp / "v13.so").read_bytes()

    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as zout:
        for info in infos:
            n = info.filename
            if n.startswith("META-INF/") and n.endswith((".SF", ".RSA", ".DSA", ".EC", "MANIFEST.MF")):
                continue                # the old signature
            ni = zipfile.ZipInfo(filename=n, date_time=info.date_time)
            ni.compress_type = info.compress_type
            ni.external_attr = info.external_attr
            ni.create_system = info.create_system
            zout.writestr(ni, datas[n], compress_type=info.compress_type)
    (tmp / "unsigned.apk").write_bytes(buf.getvalue())
    run([str(bt / "zipalign"), "-f", "-p", "4", str(tmp / "unsigned.apk"), str(tmp / "aligned.apk")])
    run([str(bt / "apksigner"), "sign", "--ks", env["KEYSTORE"], "--ks-pass", "env:KS_PASS",
         "--ks-key-alias", env["KEY_ALIAS"], "--key-pass", "env:KS_PASS",
         "--v1-signing-enabled", "true", "--v2-signing-enabled", "true",
         "--v3-signing-enabled", "true", "--out", out_apk, str(tmp / "aligned.apk")])
    shutil.rmtree(tmp)
    print("OK", out_apk)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
