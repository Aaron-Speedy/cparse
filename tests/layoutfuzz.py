#!/usr/bin/env python3
"""Differential struct-layout fuzzer: random structs with bit-fields,
packed/aligned attributes and #pragma pack, checked against the system C
compiler through cparse --typecheck (which asserts size, alignment and
member offsets). Usage: layoutfuzz.py [count] [seed]
Environment: MS=1 adds ms_struct, PLAIN=1 drops attributes, CC picks the
compiler ("gcc -m32", x86_64-w64-mingw32-gcc) and CPARSEFLAGS adds cparse
options ("--target ilp32")."""
import random, subprocess, sys, os, tempfile

TYPES = ["char", "signed char", "unsigned char", "short", "unsigned short",
         "int", "unsigned", "long", "unsigned long", "long long",
         "unsigned long long", "_Bool", "float", "double", "long double",
         "void *", "char[3]", "short[5]", "__int128"]
INTS = {"char": 8, "signed char": 8, "unsigned char": 8, "short": 16,
        "unsigned short": 16, "int": 32, "unsigned": 32, "long": 64,
        "unsigned long": 64, "long long": 64, "unsigned long long": 64,
        "_Bool": 1}

def member(r, i):
    t = r.choice(TYPES)
    attrs = ""
    if PLAIN: pass
    elif r.random() < 0.15:
        attrs += " __attribute__((aligned(%d)))" % r.choice([1, 2, 4, 8, 16, 32])
    if not PLAIN and r.random() < 0.1:
        attrs += " __attribute__((packed))"
    if t in INTS and r.random() < 0.5:
        w = r.randint(0, INTS[t])
        if w == 0 or r.random() < 0.1:
            return "%s :%d;" % (t, w) if w == 0 else "%s :%d;" % (t, w)
        return "%s m%d :%d%s;" % (t, i, w, attrs)
    if t.endswith("]"):
        base, n = t[:-1].split("[")
        return "%s m%d[%s]%s;" % (base, i, n, attrs)
    return "%s m%d%s;" % (t, i, attrs)

def struct(r, n):
    kind = "union" if r.random() < 0.15 else "struct"
    body = " ".join(member(r, i) for i in range(r.randint(1, 7)))
    if r.random() < 0.2:
        inner = " ".join(member(r, 100+i) for i in range(r.randint(1, 3)))
        body += " %s { %s } m99;" % (r.choice(["struct", "union"]), inner)
    sattr = ""
    if not PLAIN and r.random() < 0.25: sattr += " __attribute__((packed))"
    if not PLAIN and r.random() < 0.15: sattr += " __attribute__((aligned(%d)))" % r.choice([1, 2, 4, 8, 16])
    if MS and r.random() < 0.7: sattr += " __attribute__((ms_struct))"
    text = "%s%s S%d { %s };" % (kind, sattr, n, body)
    if not PLAIN and r.random() < 0.2:
        text = "#pragma pack(push, %d)\n%s\n#pragma pack(pop)" % (r.choice([1, 2, 4, 8]), text)
    return text

# the target model decides which types exist and how wide long is
FLAGS = os.environ.get("CPARSEFLAGS", "")
if "ilp32" in FLAGS:
    TYPES.remove("__int128")
if "ilp32" in FLAGS or "llp64" in FLAGS:
    INTS["long"] = INTS["unsigned long"] = 32

MS = os.environ.get("MS") == "1"
PLAIN = os.environ.get("PLAIN") == "1"

def main():
    count = int(sys.argv[1]) if len(sys.argv) > 1 else 200
    seed = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    r = random.Random(seed)
    here = os.path.dirname(os.path.abspath(__file__))
    cparse = os.path.join(here, "..", "cparse")
    src = "\n".join(struct(r, i) for i in range(count)) + "\n"
    with tempfile.TemporaryDirectory() as d:
        f = os.path.join(d, "l.c")
        open(f, "w").write(src)
        cc = os.environ.get("CC", "gcc").split()
        flags = os.environ.get("CPARSEFLAGS", "").split()
        tc = subprocess.run([cparse, "--cc", " ".join(cc)] + flags + ["--typecheck", f],
                            capture_output=True, text=True)
        t = os.path.join(d, "t.c")
        open(t, "w").write(tc.stdout)
        g = subprocess.run(cc + ["-fsyntax-only", "-w", "-fmax-errors=100000", t],
                           capture_output=True, text=True)
        fails = [l for l in g.stderr.splitlines() if "error:" in l]
        names = sorted({l.split('"')[1].split()[-1].split(".")[0] for l in fails if '"' in l})
        print("%d structs, %d failed assertions in %d structs" % (count, len(fails), len(names)))
        lines = src.splitlines()
        for nm in names[:8]:
            for i, l in enumerate(lines):
                if (" %s {" % nm) in l:
                    pre = lines[i-1] if i and lines[i-1].startswith("#pragma") else ""
                    print("  " + (pre + " " if pre else "") + l)
        for l in fails[:8]:
            print("  " + l.split("error: ")[-1])
    sys.exit(1 if fails else 0)

main()
