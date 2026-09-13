#!/usr/bin/env python3
"""
pe_dump_analyzer.py — Analyse d'un dump PE64 protégé VMProtect (Destiny 2 / Project Sunrise).

Répond aux questions qui comptent avant de passer à Ghidra / NoVmp :
  1. Le dump est-il complet ?  -> entropie par page, % de pages encore chiffrées par section
  2. L'IAT est-elle résolue ?  -> descripteurs d'import + slots IAT pointant hors image
  3. Où atterrit l'OEP ?       -> suit la chaîne de jmp du stub VMP et dit si la page cible est du code
  4. Y a-t-il une VM ?         -> signatures vm_entry / rolling-key / mutation VMP 3.x
  5. Quoi de neuf ?            -> --compare ancien.bin : pages passées de chiffré -> code

Usage :
    python pe_dump_analyzer.py dump.bin
    python pe_dump_analyzer.py dump.bin --json out.json
    python pe_dump_analyzer.py dump.bin --compare ancien_dump.bin
    python pe_dump_analyzer.py dump.bin --layout file      # dump Scylla (raw offsets) au lieu de layout mémoire
"""
from __future__ import annotations

import argparse
import json
import math
import re
import struct
import sys
from collections import Counter

try:
    from capstone import Cs, CS_ARCH_X86, CS_MODE_64
    from capstone.x86 import X86_OP_IMM
except ImportError:
    sys.exit("pip install capstone")
try:
    import numpy as np          # optionnel : entropie par page ~50x plus rapide
except ImportError:
    np = None

PAGE = 0x1000
ENC_THRESHOLD = 7.8     # au-dessus : chiffré / compressé
CODE_LO, CODE_HI = 5.0, 7.6   # code x64 (mutation VMP monte jusqu'à ~7.4)

# ---------------------------------------------------------------- utilitaires

def entropy(data: bytes) -> float:
    if not data:
        return 0.0
    n = len(data)
    return -sum((c / n) * math.log2(c / n) for c in Counter(data).values())


def page_entropies(data: bytes) -> list[float]:
    """Entropie de chaque page de 4 Ko (vectorisé si numpy est présent)."""
    n = len(data) // PAGE
    if n == 0:
        return []
    if np is None:
        return [entropy(data[i * PAGE:(i + 1) * PAGE]) for i in range(n)]
    arr = np.frombuffer(data, dtype=np.uint8, count=n * PAGE).reshape(n, PAGE).astype(np.int32)
    counts = np.bincount((arr + 256 * np.arange(n)[:, None]).ravel(), minlength=256 * n).reshape(n, 256)
    p = counts / PAGE
    with np.errstate(divide="ignore", invalid="ignore"):
        h = -np.where(p > 0, p * np.log2(p), 0).sum(axis=1)
    return h.tolist()


def classify(h: float) -> str:
    if h > ENC_THRESHOLD:
        return "encrypted"
    if CODE_LO <= h <= CODE_HI:
        return "code"
    if h < 1.0:
        return "zero"
    return "other"


# ---------------------------------------------------------------- PE

class PE:
    def __init__(self, data: bytes, layout: str = "auto"):
        self.data = data
        if data[:2] != b"MZ":
            raise ValueError("pas de signature MZ")
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        if data[pe:pe + 4] != b"PE\0\0":
            raise ValueError("pas de signature PE")
        nsec = struct.unpack_from("<H", data, pe + 6)[0]
        self.timestamp = struct.unpack_from("<I", data, pe + 8)[0]
        optsz = struct.unpack_from("<H", data, pe + 20)[0]
        opt = pe + 24
        if struct.unpack_from("<H", data, opt)[0] != 0x20B:
            raise ValueError("pas un PE32+ (x64)")
        self.entry_rva = struct.unpack_from("<I", data, opt + 16)[0]
        self.image_base = struct.unpack_from("<Q", data, opt + 24)[0]
        self.image_size = struct.unpack_from("<I", data, opt + 56)[0]
        dd = opt + 112
        self.dirs = {}
        for i, name in enumerate(["export", "import", "resource", "exception", "security",
                                  "basereloc", "debug", "arch", "globalptr", "tls",
                                  "loadcfg", "boundimport", "iat", "delayimport", "clr"]):
            self.dirs[name] = struct.unpack_from("<II", data, dd + 8 * i)
        self.sections = []
        secs = opt + optsz
        for i in range(nsec):
            o = secs + 40 * i
            name = data[o:o + 8].split(b"\0")[0].decode("ascii", "replace")
            vs, va, rs, ro = struct.unpack_from("<IIII", data, o + 8)
            ch = struct.unpack_from("<I", data, o + 36)[0]
            self.sections.append(dict(name=name, va=va, vsize=vs, rsize=rs, roff=ro, chars=ch))

        # layout mémoire (dump brut : offset fichier == RVA) ou fichier (Scylla / exe disque)
        if layout == "auto":
            mem_like = all(s["roff"] == s["va"] for s in self.sections if s["rsize"]) \
                or len(data) >= self.image_size - PAGE
            layout = "mem" if mem_like else "file"
        self.layout = layout

    # RVA -> offset fichier
    def off(self, rva: int) -> int | None:
        if self.layout == "mem":
            return rva if rva < len(self.data) else None
        for s in self.sections:
            if s["va"] <= rva < s["va"] + max(s["vsize"], s["rsize"]):
                o = s["roff"] + (rva - s["va"])
                return o if o < len(self.data) else None
        return rva if rva < len(self.data) else None  # headers

    def read(self, rva: int, n: int) -> bytes:
        o = self.off(rva)
        return self.data[o:o + n] if o is not None else b""

    def section_of(self, rva: int) -> str:
        for s in self.sections:
            if s["va"] <= rva < s["va"] + s["vsize"]:
                return s["name"]
        return "?"

    def qword(self, rva: int) -> int:
        b = self.read(rva, 8)
        return struct.unpack("<Q", b)[0] if len(b) == 8 else 0

    def cstr(self, rva: int, n: int = 64) -> str:
        return self.read(rva, n).split(b"\0")[0].decode("ascii", "replace")


# ---------------------------------------------------------------- analyses

def section_pages(pe: PE) -> list[dict]:
    """Entropie par page de 4 Ko, agrégée par section (exécutables + .vmp*)."""
    out = []
    for s in pe.sections:
        executable = bool(s["chars"] & 0x20000000) or s["name"].startswith(".vmp")
        if not executable or s["vsize"] == 0:
            continue
        seg = pe.read(s["va"], s["vsize"])
        pages = [classify(h) for h in page_entropies(seg)]
        missing = (s["vsize"] // PAGE) - len(pages)
        pages += ["missing"] * missing
        cats = Counter(pages)
        n = len(pages) or 1
        runs = []
        i = 0
        while i < len(pages):
            if pages[i] == "encrypted":
                j = i
                while j < len(pages) and pages[j] == "encrypted":
                    j += 1
                runs.append((s["va"] + i * PAGE, j - i))
                i = j
            else:
                i += 1
        runs.sort(key=lambda r: -r[1])
        out.append(dict(name=s["name"], va=hex(s["va"]), vsize=s["vsize"], pages=n,
                        pct={k: round(100 * v / n, 1) for k, v in cats.items()},
                        encrypted_runs=len(runs),
                        biggest_runs=[(hex(a), c) for a, c in runs[:5]],
                        _pages=pages))
    return out


def imports(pe: PE) -> dict:
    """Descripteurs d'import + état des slots IAT (résolus vers des DLL ou pas)."""
    rva, size = pe.dirs["import"]
    res = dict(descriptors=0, dlls=[], iat_slots=0, resolved=0, zero=0, in_image=0)
    if not rva:
        return res
    d = rva
    while True:
        b = pe.read(d, 20)
        if len(b) < 20:
            break
        ilt, _, _, name, iat = struct.unpack("<IIIII", b)
        if not name and not iat:
            break
        res["descriptors"] += 1
        res["dlls"].append(pe.cstr(name))
        i = 0
        while True:
            v = pe.qword(iat + 8 * i)
            if v == 0:
                if pe.qword(ilt + 8 * i) == 0:
                    break
                res["zero"] += 1
            elif pe.image_base <= v < pe.image_base + pe.image_size:
                res["in_image"] += 1
            elif v & (1 << 63):          # ordinal non résolu
                res["zero"] += 1
            else:
                res["resolved"] += 1
            res["iat_slots"] += 1
            i += 1
            if i > 20000:
                break
        d += 20
    res["iat_state"] = ("résolue (adresses DLL)" if res["resolved"] > res["iat_slots"] * 0.8
                        else "non résolue / partielle")
    return res


def follow_stub(pe: PE, start_rva: int, max_steps: int = 400) -> dict:
    """Suit jmp/call/ret direct depuis l'EP et rapporte où le flux sort du stub VMP.
    S'arrête sur : instruction indéchiffrable, jmp/call indirect, page chiffrée."""
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    pc = start_rva
    stack: list[int] = []
    trace: list[str] = []
    saw_saveall = False
    pushes = 0
    for _ in range(max_steps):
        b = pe.read(pc, 32)
        ins = next(md.disasm(b, pe.image_base + pc), None)
        if ins is None:
            return dict(stop="undecodable", rva=hex(pc), section=pe.section_of(pc),
                        page_class=classify(entropy(pe.read(pc & ~0xFFF, PAGE))),
                        bytes=b[:16].hex(" "), saw_saveall=saw_saveall, steps=len(trace))
        trace.append(f"{ins.address:x} {ins.mnemonic} {ins.op_str}")
        if ins.mnemonic in ("push", "pushfq"):
            pushes += 1
            if pushes >= 15:
                saw_saveall = True
        else:
            pushes = 0
        ops = ins.operands
        if ins.mnemonic == "jmp" and ops and ops[0].type == X86_OP_IMM:
            pc = ops[0].imm - pe.image_base
            page_cls = classify(entropy(pe.read(pc & ~0xFFF, PAGE)))
            if page_cls == "encrypted":
                return dict(stop="jmp into encrypted page", rva=hex(pc), section=pe.section_of(pc),
                            page_class=page_cls, saw_saveall=saw_saveall, steps=len(trace))
            continue
        if ins.mnemonic == "call" and ops and ops[0].type == X86_OP_IMM:
            stack.append(pc + ins.size)
            pc = ops[0].imm - pe.image_base
            continue
        if ins.mnemonic == "ret":
            if not stack:
                return dict(stop="ret without frame", rva=hex(pc), section=pe.section_of(pc),
                            saw_saveall=saw_saveall, steps=len(trace))
            pc = stack.pop()
            continue
        if ins.mnemonic in ("jmp", "call"):
            return dict(stop=f"indirect {ins.mnemonic} {ins.op_str}", rva=hex(pc),
                        section=pe.section_of(pc), saw_saveall=saw_saveall, steps=len(trace))
        pc += ins.size
    return dict(stop="max_steps", rva=hex(pc), section=pe.section_of(pc), saw_saveall=saw_saveall, steps=len(trace))


# signatures VMP 3.x x64
RE_PUSHRUN = re.compile(rb"(?:[\x50-\x57]|\x41[\x50-\x57]|\x9c){14,}")
RE_SAVEALL = re.compile(rb"(?:[\x50-\x57]|\x41[\x50-\x57]|\x9c){14,}\x48\x8d\xa4\x24\x00\xff\xff\xff")
RE_VMENTRY = re.compile(rb"(?:[\x50-\x57]|\x41[\x50-\x57]|\x9c){14,}[\x00-\xff]{0,16}?\x48\x8b\xb4\x24\x90\x00\x00\x00", re.S)
RE_ROLLKEY8 = re.compile(rb"\x8a\x06[\x00-\xff]{0,12}?(?:\x32\xc3|\x30\xd8)[\x00-\xff]{0,20}?(?:\x30\xc3|\x32\xd8)", re.S)
RE_ROLLKEY32 = re.compile(rb"\x8b\x06[\x00-\xff]{0,12}?(?:\x33\xc3|\x31\xd8)[\x00-\xff]{0,20}?(?:\x31\xc3|\x33\xd8)", re.S)
RE_OBF_RET = re.compile(rb"\x48\x8d\x64\x24\x08\x48\x8d\x64\x24\x08\xff\x64\x24\xf8")  # lea rsp,[rsp+8]x2 ; jmp [rsp-8]
RE_OBF_PUSH = re.compile(rb"\x48\x89\x2c\x24\x48\xbd[\x00-\xff]{8}\x48\x87\x2c\x24", re.S)  # mov [rsp],rbp; movabs rbp; xchg


def vmp_signatures(pe: PE) -> dict:
    out = {}
    for s in pe.sections:
        if not (s["chars"] & 0x20000000) and not s["name"].startswith(".vmp"):
            continue
        seg = pe.read(s["va"], s["vsize"])
        if not seg:
            continue
        out[f"{s['name']}@{s['va']:#x}"] = dict(
            save_all_prologues=len(RE_SAVEALL.findall(seg)),
            vm_entry_like=len(RE_VMENTRY.findall(seg)),
            rolling_key_fetch=len(RE_ROLLKEY8.findall(seg)) + len(RE_ROLLKEY32.findall(seg)),
            obf_ret=len(RE_OBF_RET.findall(seg)),
            obf_push=len(RE_OBF_PUSH.findall(seg)),
        )
    return out


def compare(pe: PE, old: PE) -> dict:
    """Pages passées de 'encrypted' à 'code' entre deux dumps (même image)."""
    res = {}
    if pe.image_base != old.image_base:
        res["note"] = "bases différentes (ASLR) : comparaison par RVA, les pages avec pointeurs absolus diffèrent"
    for s in pe.sections:
        if not (s["chars"] & 0x20000000):
            continue
        new = [classify(h) for h in page_entropies(pe.read(s["va"], s["vsize"]))]
        prev = [classify(h) for h in page_entropies(old.read(s["va"], s["vsize"]))]
        dec = sum(1 for a, b in zip(new, prev) if b == "encrypted" and a == "code")
        enc = sum(1 for a in new if a == "encrypted")
        both = [i for i, (a, b) in enumerate(zip(new, prev)) if a == b == "encrypted"]
        ident = sum(1 for i in both if pe.read(s["va"] + i * PAGE, PAGE) == old.read(s["va"] + i * PAGE, PAGE))
        res[s["name"]] = dict(newly_decrypted_pages=dec, still_encrypted_pages=enc,
                              encrypted_in_both=len(both), byte_identical=ident)
    return res


# ---------------------------------------------------------------- main

def main() -> None:
    if hasattr(sys.stdout, "reconfigure"):      # console Windows en cp1252
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dump")
    ap.add_argument("--json")
    ap.add_argument("--compare", help="ancien dump pour diff des pages déchiffrées")
    ap.add_argument("--layout", choices=["auto", "mem", "file"], default="auto")
    args = ap.parse_args()

    data = open(args.dump, "rb").read()
    try:
        pe = PE(data, args.layout)
    except ValueError as e:
        sys.exit(f"PE64 non reconnu : {e}")

    import datetime as dt
    ts = dt.datetime.fromtimestamp(pe.timestamp, dt.timezone.utc).strftime("%Y-%m-%d")
    report = dict(
        file=args.dump, size=len(data), layout=pe.layout,
        image_base=hex(pe.image_base), image_size=hex(pe.image_size),
        entry_point=hex(pe.entry_rva), timestamp=ts,
        vmp_sections=[s["name"] for s in pe.sections if s["name"].startswith(".vmp")],
        sections=[dict(name=s["name"], va=hex(s["va"]), vsize=hex(s["vsize"]), chars=hex(s["chars"]))
                  for s in pe.sections],
    )
    report["pages"] = section_pages(pe)
    report["imports"] = imports(pe)
    report["entry_stub"] = follow_stub(pe, pe.entry_rva)
    report["vmp_signatures"] = vmp_signatures(pe)
    if args.compare:
        report["compare"] = compare(pe, PE(open(args.compare, "rb").read(), args.layout))

    # verdicts
    enc_total = sum(p["pct"].get("encrypted", 0) * p["pages"] / 100 for p in report["pages"])
    sig = report["vmp_signatures"]
    vm_hits = sum(v["vm_entry_like"] + v["rolling_key_fetch"] for v in sig.values())
    mut_hits = sum(v["obf_ret"] + v["obf_push"] + v["save_all_prologues"] for v in sig.values())
    runs_total = sum(p.get("encrypted_runs", 0) for p in report["pages"])
    if enc_total < 50:
        completeness = "probable"
    elif runs_total > 50:
        completeness = ("pages haute entropie dispersées (%d blocs) : bytecode VM / données VMP, "
                        "pas un dump incomplet" % runs_total)
    else:
        completeness = "non : gros blocs chiffrés contigus (sections pas encore déballées)"
    cmp_ = report.get("compare", {})
    ident = sum(v.get("byte_identical", 0) for v in cmp_.values() if isinstance(v, dict))
    both = sum(v.get("encrypted_in_both", 0) for v in cmp_.values() if isinstance(v, dict))
    if both and ident > both * 0.9:
        completeness += " ; %d/%d pages identiques entre les deux dumps -> statique, jamais déchiffré en place" % (ident, both)
    report["verdict"] = dict(
        vmprotect="oui" if report["vmp_sections"] or mut_hits > 50 else "non détecté",
        dump_complet=completeness,
        pages_chiffrees=int(enc_total),
        iat=report["imports"]["iat_state"],
        virtualisation=("signatures VM présentes" if vm_hits else
                        "handlers non signés (mutés) mais %d blocs haute entropie dispersés : VM probable" % runs_total
                        if runs_total > 50 else "aucune signature VM (mutation/packing seul)"),
        mutation_vmp3=mut_hits,
    )

    for p in report["pages"]:
        p.pop("_pages", None)

    if args.json:
        json.dump(report, open(args.json, "w"), indent=2)
        print(f"-> {args.json}")
        return

    print(f"{args.dump}  ({len(data):,} o, layout={pe.layout})")
    print(f"  base {report['image_base']}  taille {report['image_size']}  EP {report['entry_point']}  build {ts}")
    print("\nSections exécutables (entropie par page 4 Ko) :")
    for p in report["pages"]:
        pct = "  ".join(f"{k}={v}%" for k, v in sorted(p["pct"].items()))
        print(f"  {p['name']:8} {p['va']:>11}  {p['pages']:6} pages  {pct}")
        if p.get("encrypted_runs"):
            print(f"           {p['encrypted_runs']} blocs chiffrés, plus gros : {p['biggest_runs']}")
    im = report["imports"]
    print(f"\nImports : {im['descriptors']} DLL, {im['iat_slots']} slots, "
          f"{im['resolved']} résolus, {im['zero']} vides, {im['in_image']} vers l'image  -> {im['iat_state']}")
    print("  " + ", ".join(im["dlls"][:12]) + (" …" if len(im["dlls"]) > 12 else ""))
    st = report["entry_stub"]
    print(f"\nStub d'entrée : {st['steps']} instr., save-all={st['saw_saveall']}, arrêt = {st['stop']} @ {st['rva']} [{st.get('section')}]"
          + (f" page={st['page_class']}" if "page_class" in st else ""))
    print("\nSignatures VMP 3.x :")
    for k, v in sig.items():
        print(f"  {k:18} save-all={v['save_all_prologues']:5} vm_entry={v['vm_entry_like']:3} "
              f"rollkey={v['rolling_key_fetch']:3} obf_ret={v['obf_ret']:5} obf_push={v['obf_push']:5}")
    if "compare" in report:
        print("\nDiff vs ancien dump :")
        for k, v in report["compare"].items():
            if isinstance(v, dict):
                print(f"  {k:8} +{v['newly_decrypted_pages']} déchiffrées, {v['still_encrypted_pages']} chiffrées, "
                      f"{v['byte_identical']}/{v['encrypted_in_both']} identiques octet à octet")
            else:
                print(f"  {v}")
    print("\nVerdict :")
    for k, v in report["verdict"].items():
        print(f"  {k:16} {v}")


if __name__ == "__main__":
    main()
