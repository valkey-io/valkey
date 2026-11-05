#!/usr/bin/env python3
"""Convert TLC's state-graph dump of spec/Core_xcheck.cfg (or, with --ack,
spec/CoreAck_xcheck.cfg) into data for lean/XCheck.

Usage: python3 xcheck/dot2lean.py [--ack] [dot] [outdir]
  default:  xcheck/core_xcheck.dot    -> lean/XCheck/data      (lake exe xcheck)
  --ack:    xcheck/coreack_xcheck.dot -> lean/XCheck/data_ack  (lake exe xcheck_ack)

Writes (all generated, git-ignored):
  states.txt  one line per state: the numbers of the State encoding below
  labels.txt  one line per state: TLC's original state text with all whitespace removed
              (Lean re-renders each decoded State in TLC syntax and compares, so the
              decoding is tested against TLC's own text, independently of this script)
  edges.txt   one line per TLC transition: "src dst Label(args)"
  init.txt    the index of TLC's initial state

Node n1/n2/n3 = Fin 3 values 0/1/2. Sets of nodes are bitmasks (bit i = node i).
State encoding (must match XCheck/Data.lean `decode`):
  log n (n=0..2): len, entries...
  cfg: ver, prim, eligMask
  known n: ver, prim, eligMask        (n=0..2)
  sync n: mask                        (n=0..2)
  acked n r: nat                      (n=0..2, r=0..2)
  commit n, recon n, term n           (3 each)
  hist: len, entries...
With --ack, two more fields follow (must match XCheck/Ack.lean `decodeA`):
  held: count, then (node, o, t) per reply, in TLC's printed order
  okd: count, then (o, v) per acknowledged pair, in TLC's printed order
and labels.txt has the "/\\ var = value" conjuncts sorted by variable name, because
TLC's variable order in this dump is not fixed (Lean renders them in that order too).
"""
import os
import re
import sys

NODES = ["n1", "n2", "n3"]
VARS = {"log", "cfg", "known", "sync", "acked", "commit", "recon", "term", "hist"}
ACK = False


class P:
    """Recursive-descent parser for the TLC value syntax used in the dump."""

    def __init__(self, s):
        self.s, self.i = s, 0

    def ws(self):
        while self.i < len(self.s) and self.s[self.i].isspace():
            self.i += 1

    def eat(self, t):
        self.ws()
        if not self.s.startswith(t, self.i):
            raise ValueError(f"expected {t!r} at {self.i}: {self.s[self.i:self.i+40]!r}")
        self.i += len(t)

    def peek(self, t):
        self.ws()
        return self.s.startswith(t, self.i)

    def ident(self):
        self.ws()
        m = re.compile(r"[A-Za-z_][A-Za-z0-9_]*").match(self.s, self.i)
        if not m:
            raise ValueError(f"ident at {self.i}: {self.s[self.i:self.i+40]!r}")
        self.i = m.end()
        return m.group()

    def value(self):
        self.ws()
        c = self.s[self.i]
        if c.isdigit():
            m = re.compile(r"\d+").match(self.s, self.i)
            self.i = m.end()
            return int(m.group())
        if self.peek("<<"):
            self.eat("<<")
            items = []
            if not self.peek(">>"):
                items.append(self.value())
                while self.peek(","):
                    self.eat(",")
                    items.append(self.value())
            self.eat(">>")
            return ("seq", items)
        if c == "{":
            self.eat("{")
            items = []
            if not self.peek("}"):
                items.append(self.value())
                while self.peek(","):
                    self.eat(",")
                    items.append(self.value())
            self.eat("}")
            return ("set", items)
        if c == "[":
            self.eat("[")
            rec = {}
            while True:
                k = self.ident()
                self.eat("|->")
                rec[k] = self.value()
                if self.peek(","):
                    self.eat(",")
                    continue
                break
            self.eat("]")
            return ("rec", rec)
        if c == "(":
            self.eat("(")
            fn = {}
            while True:
                k = self.value()
                self.eat(":>")
                fn[k] = self.value()
                if self.peek("@@"):
                    self.eat("@@")
                    continue
                break
            self.eat(")")
            return ("fn", fn)
        name = self.ident()
        if name not in NODES:
            raise ValueError(f"unknown model value {name}")
        return ("node", NODES.index(name))


def node(v):
    assert v[0] == "node", v
    return v[1]


def mask(v):
    assert v[0] == "set", v
    m = 0
    for x in v[1]:
        m |= 1 << node(x)
    return m


def nat(v):
    assert isinstance(v, int), v
    return v


def seq(v):
    assert v[0] == "seq", v
    return [len(v[1])] + [nat(x) for x in v[1]]


def fn(v):
    assert v[0] == "fn" and sorted(node(k) for k in v[1]) == [0, 1, 2], v
    return [v[1][("node", i)] for i in range(3)]


def cfg(v):
    assert v[0] == "rec" and set(v[1]) == {"ver", "prim", "elig"}, v
    r = v[1]
    return [nat(r["ver"]), node(r["prim"]), mask(r["elig"])]


def held(v):
    assert v[0] == "set", v
    out = [len(v[1])]
    for r in v[1]:
        assert r[0] == "rec" and set(r[1]) == {"n", "o", "t"}, r
        out += [node(r[1]["n"]), nat(r[1]["o"]), nat(r[1]["t"])]
    return out


def okd(v):
    assert v[0] == "set", v
    out = [len(v[1])]
    for x in v[1]:
        assert x[0] == "seq" and len(x[1]) == 2, x
        out += [nat(x[1][0]), nat(x[1][1])]
    return out


def norm_label(text):
    """Whitespace removed; with --ack also the conjuncts sorted by variable name."""
    t = "".join(text.split())
    if not ACK:
        return t
    parts = t.split("/\\")[1:]
    parts.sort(key=lambda c: c.split("=", 1)[0])
    return "".join("/\\" + c for c in parts)


def parse_state(text):
    vals = {}
    for part in text.split("/\\")[1:]:
        p = P(part)
        name = p.ident()
        p.eat("=")
        vals[name] = p.value()
        p.ws()
        assert p.i == len(p.s), (name, p.s[p.i:])
    assert set(vals) == VARS | ({"held", "okd"} if ACK else set()), vals.keys()
    out = []
    for x in fn(vals["log"]):
        out += seq(x)
    out += cfg(vals["cfg"])
    for x in fn(vals["known"]):
        out += cfg(x)
    out += [mask(x) for x in fn(vals["sync"])]
    for x in fn(vals["acked"]):
        out += [nat(y) for y in fn(x)]
    for v in ("commit", "recon", "term"):
        out += [nat(x) for x in fn(vals[v])]
    out += seq(vals["hist"])
    if ACK:
        out += held(vals["held"]) + okd(vals["okd"])
    return out


NODE_RE = re.compile(r'^(-?\d+) \[label="((?:[^"\\]|\\.)*)"(.*)\];?$')
EDGE_RE = re.compile(r'^(-?\d+) -> (-?\d+) \[label="([^"]*)"')


def unescape(s):
    return s.replace("\\n", "\n").replace("\\\\", "\\")


def main():
    global ACK
    args = sys.argv[1:]
    if args and args[0] == "--ack":
        ACK, args = True, args[1:]
    dot = args[0] if args else ("xcheck/coreack_xcheck.dot" if ACK else "xcheck/core_xcheck.dot")
    outd = args[1] if len(args) > 1 else ("lean/XCheck/data_ack" if ACK else "lean/XCheck/data")
    os.makedirs(outd, exist_ok=True)
    idx, states, labels, edges, init = {}, [], [], [], []
    with open(dot) as f:
        for line in f:
            line = line.rstrip("\n")
            m = EDGE_RE.match(line)
            if m:
                edges.append((m.group(1), m.group(2), m.group(3)))
                continue
            m = NODE_RE.match(line)
            if m:
                fid, text = m.group(1), unescape(m.group(2))
                if fid in idx:
                    raise ValueError(f"duplicate node {fid}")
                idx[fid] = len(states)
                states.append(parse_state(text))
                labels.append(norm_label(text))
                if "style = filled" in m.group(3):
                    init.append(idx[fid])
    assert len(init) == 1, init
    enc = {}
    for i, st in enumerate(states):
        key = tuple(st)
        if key in enc:
            raise ValueError(f"states {enc[key]} and {i} have the same value")
        enc[key] = i
    with open(os.path.join(outd, "states.txt"), "w") as f:
        for st in states:
            f.write(" ".join(map(str, st)) + "\n")
    with open(os.path.join(outd, "labels.txt"), "w") as f:
        for lb in labels:
            f.write(lb + "\n")
    with open(os.path.join(outd, "edges.txt"), "w") as f:
        for a, b, lb in edges:
            f.write(f"{idx[a]} {idx[b]} {lb}\n")
    with open(os.path.join(outd, "init.txt"), "w") as f:
        f.write(f"{init[0]}\n")
    print(f"{len(states)} states, {len(edges)} edges, init {init[0]} -> {outd}")


if __name__ == "__main__":
    main()
