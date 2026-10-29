#!/usr/bin/env python3
"""Savestate versioning: keeps native/state/lock.h, the section schemas
(native/state/schema/<SECTION>.v<N>.txt) and native/state/CHANGELOG.md in
step with the field tables (the .def files in native/state).

The core's build checks each section's tables against lock.h's layout hash
(native/state/def.h); this tool computes the same hash from the .def files.

  statelock.py                      check: every section matches its lock and schema
  statelock.py --bump SEC "what"    the section's next version (a change of layout or meaning)
  statelock.py --bump SEC "what" --breaks
                                    ... that older states of it can't load (min rises)
  statelock.py --init               lock new sections at version 1
  statelock.py --sizes              NC_REC sizes from the core's compiler (compile_commands.json)
  statelock.py --since REF          section versions changed since a git ref (release notes)

Rules for a bump without --breaks (older states keep loading):
  * a new field has since == the new version;
  * a dropped field stays listed with until == the new version;
  * an existing field keeps its type and count.
"""

import argparse
import datetime
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
STATE = os.path.join(REPO, "native", "state")
LOCK = os.path.join(STATE, "lock.h")
SCHEMA = os.path.join(STATE, "schema")
CHANGELOG = os.path.join(STATE, "CHANGELOG.md")
CONTAINER = 1

TYPES = {  # name: (NcFieldType, element size) as state.h
    "U8": (1, 1), "U16": (2, 2), "U32": (3, 4), "U64": (4, 8),
    "I8": (5, 1), "I16": (6, 2), "I32": (7, 4), "I64": (8, 8),
    "BOOL": (9, 1), "F32": (10, 4), "F64": (11, 8),
}
FT_REC = 12
M32 = 0xFFFFFFFF


def fh(t, e, n, s, u, l):
    """state.h's NC_FH."""
    v = t
    for x in (e, n, s, u, l):
        v = (v * 131 + x) & M32
    return (v * 2654435761) & M32


class Field:
    def __init__(self, name, kind, ty, count, since, until):
        self.name, self.kind, self.ty = name, kind, ty   # kind: F or R (ty: a record)
        self.count, self.since, self.until = count, since, until

    def schema(self):
        s = f"  {self.name} {'rec ' if self.kind == 'R' else ''}{self.ty} x{self.count} since {self.since}"
        return s + (f" until {self.until}" if self.until else "")


class Section:
    def __init__(self, name, path):
        self.name, self.path = name, path
        self.records = []        # [(type, size, [Field])]

    def layouts(self):
        lay = {}
        for t, _, fields in self.records:
            h = 0
            for f in fields:
                if f.kind == "R":
                    if f.ty not in lay:
                        raise SystemExit(f"{self.path}: {t}.{f.name}: record {f.ty} isn't defined above it")
                    h = (h + fh(FT_REC, lay[f.ty], f.count, f.since, f.until, len(f.name) + 1)) & M32
                else:
                    code, size = TYPES[f.ty]
                    h = (h + fh(code, size, f.count, f.since, f.until, len(f.name) + 1)) & M32
            lay[t] = h & 0x7FFFFFFF
        return lay

    def hash(self):
        return sum(self.layouts().values()) & 0x7FFFFFFF

    def schema(self):
        out = [f"section {self.name}"]
        for t, _, fields in self.records:
            out.append(f"record {t}")
            out += [f.schema() for f in fields]
        return "\n".join(out) + "\n"


NUM = r"\s*(\d+)\s*"
RX = {
    "SECTION": re.compile(r"^\s*NC_SECTION\(\s*(\w+)\s*\)"),
    "REC": re.compile(r"^\s*NC_REC\(\s*(\w+)\s*,\s*(?:NC_ABI_SIZE\(\s*(\d+)\s*,\s*(\d+)\s*\)|(\d+))\s*\)"),
    "F": re.compile(r"^\s*NC_F\(\s*(\w+)\s*,\s*([\w.\[\]]+)\s*,\s*(\w+)\s*," + NUM + "," + NUM + "," + NUM + r"\)"),
    "R": re.compile(r"^\s*NC_R\(\s*(\w+)\s*,\s*([\w.\[\]]+)\s*,\s*(\w+)\s*," + NUM + "," + NUM + "," + NUM + r"\)"),
    "SKIP": re.compile(r"^\s*NC_SKIP\(\s*(\w+)\s*,\s*([\w.\[\]]+)\s*\)"),
    "END": re.compile(r"^\s*NC_END\(\s*(\w+)\s*\)"),
}


def parse(path):
    sec, cur = None, None
    for no, line in enumerate(open(path, encoding="utf-8"), 1):
        line = line.split("//")[0]
        if not line.strip() or line.lstrip().startswith(("/*", "*")):
            continue
        for kind, rx in RX.items():
            m = rx.match(line)
            if m:
                break
        else:
            raise SystemExit(f"{path}:{no}: not a field table line: {line.strip()}")
        g = m.groups()
        if kind == "SECTION":
            sec = Section(g[0], path)
        elif sec is None:
            raise SystemExit(f"{path}:{no}: NC_SECTION first")
        elif kind == "REC":
            cur = (g[0], int(g[1] or g[3]), [])
            sec.records.append(cur)
        elif kind in ("F", "R"):
            if not cur or cur[0] != g[0]:
                raise SystemExit(f"{path}:{no}: {g[0]}.{g[1]} outside its NC_REC")
            if kind == "F" and g[2] not in TYPES:
                raise SystemExit(f"{path}:{no}: type {g[2]} (one of {', '.join(TYPES)})")
            cur[2].append(Field(g[1], kind, g[2], int(g[3]), int(g[4]), int(g[5])))
        elif kind == "END":
            cur = None
    if sec is None:
        raise SystemExit(f"{path}: no NC_SECTION")
    return sec


def sections():
    out = {}
    for fn in sorted(os.listdir(STATE)):
        if fn.endswith(".def"):
            s = parse(os.path.join(STATE, fn))
            if s.name in out:
                raise SystemExit(f"section {s.name} in two .def files")
            out[s.name] = s
    return out


def read_lock():
    lock = {}
    if not os.path.exists(LOCK):
        return lock
    for line in open(LOCK, encoding="utf-8"):
        m = re.match(r"#define NC_STATE_(V|MIN|H)_(\w+)\s+(0x[0-9a-fA-F]+|\d+)u?", line)
        if m:
            lock.setdefault(m.group(2), {})[m.group(1)] = int(m.group(3), 0)
    return lock


def write_lock(lock):
    lines = ["/* Generated by tools/native/statelock.py from the .def files in native/state: don't edit.",
             " * Per section: the version this core writes (V), the oldest it reads (MIN),",
             " * and its field tables' layout hash (H, checked by native/state/def.h). */",
             "#ifndef NATIVE_STATE_LOCK_H", "#define NATIVE_STATE_LOCK_H", "",
             f"#define NC_STATE_CONTAINER {CONTAINER}", ""]
    for name in sorted(lock):
        e = lock[name]
        lines += [f"#define NC_STATE_V_{name} {e['V']}", f"#define NC_STATE_MIN_{name} {e['MIN']}",
                  f"#define NC_STATE_H_{name} 0x{e['H']:08x}u"]
    lines += ["", "#endif", ""]
    with open(LOCK, "w", encoding="utf-8", newline="\n") as fp:
        fp.write("\n".join(lines))


def schema_path(name, v):
    return os.path.join(SCHEMA, f"{name}.v{v}.txt")


def changelog(entry):
    head = "# Savestate changelog\n\nOne entry per section version (tools/native/statelock.py).\n"
    text = open(CHANGELOG, encoding="utf-8").read() if os.path.exists(CHANGELOG) else head
    with open(CHANGELOG, "w", encoding="utf-8", newline="\n") as fp:
        fp.write(text.rstrip("\n") + "\n\n" + entry + "\n")


def git_head():
    try:
        return subprocess.check_output(["git", "-C", REPO, "rev-parse", "--short", "HEAD"], text=True).strip()
    except (OSError, subprocess.CalledProcessError):
        return "?"


def check(secs, lock, quiet=False):
    bad = []
    for name, s in secs.items():
        e = lock.get(name)
        if not e:
            bad.append(f"{name}: not locked yet (statelock.py --init)")
            continue
        sp = schema_path(name, e["V"])
        old = open(sp, encoding="utf-8").read() if os.path.exists(sp) else None
        if s.hash() != e["H"] or old != s.schema():
            bad.append(f"{name}: its fields changed since version {e['V']}: "
                       f"statelock.py --bump {name} \"what changed\" (--breaks if old states can't load)")
    for name in lock:
        if name not in secs:
            bad.append(f"{name}: locked but no .def has it")
    for b in bad:
        print(b, file=sys.stderr)
    if not bad and not quiet:
        print(f"{len(secs)} sections match their lock")
    return not bad


def fields_of(schema_text):
    """record -> {field: (spec without since/until, since, until)} from a schema file"""
    recs, cur = {}, None
    for line in schema_text.splitlines():
        if line.startswith("record "):
            cur = recs.setdefault(line.split()[1], {})
        elif line.startswith("  ") and cur is not None:
            p = line.split()
            until = int(p[p.index("until") + 1]) if "until" in p else 0
            since = int(p[p.index("since") + 1])
            cur[p[0]] = (" ".join(p[1:p.index("since")]), since, until)
    return recs


def bump(secs, lock, name, what, breaks):
    s = secs.get(name)
    e = lock.get(name)
    if not s or not e:
        raise SystemExit(f"{name}: no such locked section")
    v = e["V"] + 1
    old_text = open(schema_path(name, e["V"]), encoding="utf-8").read()
    if not breaks:
        old, new = fields_of(old_text), fields_of(s.schema())
        errs = []
        for rec, fl in new.items():
            for f, (spec, since, until) in fl.items():
                o = old.get(rec, {}).get(f)
                if o is None and since != v:
                    errs.append(f"{rec}.{f} is new: since {v}")
                elif o and o[0] != spec:
                    errs.append(f"{rec}.{f} changed type or count: --breaks, or a new field")
                elif o and o[2] != until and until != v:
                    errs.append(f"{rec}.{f} dropped: until {v}")
        for rec, fl in old.items():
            for f, (spec, since, until) in fl.items():
                if f not in new.get(rec, {}) and not until and rec in new:
                    errs.append(f"{rec}.{f} removed: keep it listed with until {v} (or --breaks)")
        if errs:
            raise SystemExit("can't bump " + name + " without --breaks:\n  " + "\n  ".join(errs))
    e["V"] = v
    if breaks:
        e["MIN"] = v
    e["H"] = s.hash()
    os.makedirs(SCHEMA, exist_ok=True)
    with open(schema_path(name, v), "w", encoding="utf-8", newline="\n") as fp:
        fp.write(s.schema())
    write_lock(lock)
    effect = f"breaks: states with {name} before v{v} no longer load" if breaks else \
        f"upgraded: v{e['MIN']}..v{v - 1} states load"
    changelog(f"## {name} v{v} ({datetime.date.today().isoformat()}, after {git_head()})\n\n"
              f"- {what}\n- {effect}")
    print(f"{name}: version {v} (min {e['MIN']})")


def init(secs, lock):
    added = []
    for name, s in secs.items():
        if name in lock:
            continue
        lock[name] = {"V": 1, "MIN": 1, "H": s.hash()}
        os.makedirs(SCHEMA, exist_ok=True)
        with open(schema_path(name, 1), "w", encoding="utf-8", newline="\n") as fp:
            fp.write(s.schema())
        added.append(name)
    if added:
        write_lock(lock)
        changelog(f"## {', '.join(added)} v1 ({datetime.date.today().isoformat()}, after {git_head()})\n\n"
                  f"- first version")
    print(f"locked: {', '.join(added) or 'nothing new'}")


def sizes(build):
    """NC_REC sizes: each .c that expands a .def compiled with NC_STATE_PROBE
    (def.h emits nc_probe_size_<Type> = sizeof(Type)), read from its assembly."""
    cc = json.load(open(os.path.join(build, "compile_commands.json"), encoding="utf-8"))
    found = {}
    host_abi = None
    for entry in cc:
        src = entry["file"].replace("\\", "/")
        try:
            text = open(src, encoding="utf-8", errors="replace").read()
        except OSError:
            continue
        if "native/state/def.h" not in text:
            continue
        cmd = entry["command"]
        cmd = re.sub(r"\s-o\s+\S+", " ", cmd)
        cmd = re.sub(r"\s-c\s", " ", cmd)
        cmd = re.sub(r"\s-flto(=\S+)?", " ", cmd)  # assembly, not LTO's IR (HAKUX_LTO)
        cmd += " -DNC_STATE_PROBE -S -o -"
        asm = subprocess.run(cmd, shell=True, cwd=entry["directory"], capture_output=True, text=True)
        if asm.returncode:
            print(asm.stderr[-2000:], file=sys.stderr)
            raise SystemExit(f"probe of {src} failed")
        for m in re.finditer(r"^nc_probe_size_(\w+):[^\n]*\n\s*\.(?:xword|quad|word)\s+(\d+)", asm.stdout, re.M):
            found[m.group(1)] = int(m.group(2))
        abi = re.search(r"^nc_probe_host_abi:[^\n]*\n\s*\.(?:xword|quad|word)\s+([12])", asm.stdout, re.M)
        if abi:
            host_abi = int(abi.group(1))
    changed = 0
    for fn in sorted(os.listdir(STATE)):
        if not fn.endswith(".def"):
            continue
        path = os.path.join(STATE, fn)
        text = open(path, encoding="utf-8").read()

        def fix(m):
            nonlocal changed
            t = m.group(1)
            if t not in found:
                print(f"{fn}: no size for {t} (is its .def expanded by a built source?)", file=sys.stderr)
                return m.group(0)
            if int(m.group(2)) != found[t]:
                changed += 1
            return f"NC_REC({t}, {found[t]})"
        new = re.sub(r"NC_REC\(\s*(\w+)\s*,\s*(\d+)\s*\)", fix, text)
        def fix_abi(m):
            nonlocal changed
            name, android_size, linux_size = m.groups()
            if name not in found or host_abi is None:
                raise SystemExit(f"{fn}: no host ABI size probe for {name}")
            sizes = [int(android_size), int(linux_size)]
            changed += sizes[host_abi - 1] != found[name]
            sizes[host_abi - 1] = found[name]
            return f"NC_REC({name}, NC_ABI_SIZE({sizes[0]}, {sizes[1]}))"
        new = re.sub(r"NC_REC\(\s*(\w+)\s*,\s*NC_ABI_SIZE\(\s*(\d+)\s*,\s*(\d+)\s*\)\s*\)", fix_abi, new)
        if new != text:
            with open(path, "w", encoding="utf-8", newline="\n") as fp:
                fp.write(new)
    print(f"{len(found)} record sizes, {changed} updated")


def since(ref):
    try:
        old_text = subprocess.check_output(["git", "-C", REPO, "show", f"{ref}:native/state/lock.h"],
                                           text=True, stderr=subprocess.DEVNULL)
    except subprocess.CalledProcessError:
        old_text = ""
    old = {}
    for line in old_text.splitlines():
        m = re.match(r"#define NC_STATE_(V|MIN)_(\w+)\s+(\d+)", line)
        if m:
            old.setdefault(m.group(2), {})[m.group(1)] = int(m.group(3))
    now = read_lock()
    broken, changed = [], []
    for name, e in sorted(now.items()):
        o = old.get(name)
        if o and e["MIN"] > o.get("V", 0):
            broken.append(name)
        elif not o or e["V"] != o.get("V"):
            changed.append(f"{name} v{o['V'] if o else 0}->v{e['V']}")
    if broken:
        print(f"Savestates from before {ref} no longer load (changed: {', '.join(broken)}).")
    elif changed:
        print(f"Savestates: compatible ({', '.join(changed)}).")
    else:
        print("Savestates: unchanged.")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bump", nargs=2, metavar=("SECTION", "WHAT"))
    ap.add_argument("--breaks", action="store_true")
    ap.add_argument("--init", action="store_true")
    ap.add_argument("--sizes", action="store_true")
    ap.add_argument("--build", default=os.path.join(REPO, "build", "native", "core"))
    ap.add_argument("--since", metavar="REF")
    a = ap.parse_args()
    if a.sizes:
        sizes(a.build)
        return 0
    if a.since:
        since(a.since)
        return 0
    secs, lock = sections(), read_lock()
    if a.init:
        init(secs, lock)
        return 0
    if a.bump:
        bump(secs, lock, a.bump[0], a.bump[1], a.breaks)
        return 0
    return 0 if check(secs, lock) else 1


if __name__ == "__main__":
    sys.exit(main())
