#!/usr/bin/env python3
"""Compare ValkeyModule_* API declarations in valkeymodule.h with VM_* implementations in module.c.

Wasm type-checks indirect calls, so a return-type or parameter-type mismatch that
x86-64 tolerates becomes a trap ("null function or function signature mismatch").
Usage: api-sig-check.py <src dir>
"""
import re
import sys

src = sys.argv[1]
hdr = open(f"{src}/valkeymodule.h").read()
impl = open(f"{src}/module.c").read()


def norm(sig):
    sig = re.sub(r"/\*.*?\*/", " ", sig, flags=re.S)
    sig = re.sub(r"\s+", " ", sig).strip()
    sig = re.sub(r"\s*\*\s*", "*", sig)  # normalise pointer spacing
    sig = re.sub(r"\s*,\s*", ",", sig)
    sig = re.sub(r"\(\s*", "(", sig)
    sig = re.sub(r"\s*\)", ")", sig)
    return sig


def params(sig):
    inner = sig[sig.index("(") + 1 : sig.rindex(")")]
    if inner.strip() in ("", "void"):
        return []
    out = []
    for p in inner.split(","):
        p = p.strip()
        # drop parameter name (last identifier) unless it's a bare type like 'int'
        toks = re.findall(r"[A-Za-z_][A-Za-z0-9_]*|\*|\[\]|\.\.\.", p)
        if len(toks) >= 2 and toks[-1] not in ("*", "[]", "...") and toks[-2] not in ("struct", "enum", "const", "unsigned", "long"):
            toks = toks[:-1]
        out.append("".join(t if t == "*" else " " + t for t in toks).strip())
    return out


# header: VALKEYMODULE_API <ret> (*ValkeyModule_<Name>)(<params>) VALKEYMODULE_ATTR;
decls = {}
for m in re.finditer(r"VALKEYMODULE_API\s+(.*?)\(\*ValkeyModule_(\w+)\)\s*\((.*?)\)\s*VALKEYMODULE_ATTR", hdr, re.S):
    ret, name, ps = m.group(1), m.group(2), m.group(3)
    decls[name] = (norm(ret), params(norm(f"({ps})")))

# impl: <ret> VM_<Name>(<params>) {   (skip comments, 'static', prototypes)
impls = {}
for m in re.finditer(r"^([A-Za-z_][\w \*]*?)\s*\**\s*VM_(\w+)\s*\((.*?)\)\s*\{", impl, re.S | re.M):
    ret, name, ps = m.group(1), m.group(2), m.group(3)
    full = impl[m.start() : m.end()]
    retm = re.match(r"^(.*?)VM_", norm(full))
    impls[name] = (norm(retm.group(1)), params(norm(f"({ps})")))

bad = 0
for name, (dret, dps) in sorted(decls.items()):
    if name not in impls:
        continue
    iret, ips = impls[name]
    if dret.replace("const ", "") != iret.replace("const ", "") or [p.replace("const ", "") for p in dps] != [p.replace("const ", "") for p in ips]:
        bad += 1
        print(f"MISMATCH {name}\n  header: {dret} ({', '.join(dps)})\n  impl:   {iret} ({', '.join(ips)})")
print(f"\n{len(decls)} declared, {len(impls)} implemented, {bad} mismatched")
