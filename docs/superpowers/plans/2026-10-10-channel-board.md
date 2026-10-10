# Channel Board Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Generate, entirely from Python, a JLCPCB-ready 4-layer PCB (Gerbers, BOM, CPL) that carries a DDR3 DIMM socket, a bare RP2354B, lane-switching translators (lane 0 fitted), power, USB-C and headers.

**Architecture:** A small Python package (`board/`) holds a parts catalogue (footprints and pin names pulled from LCSC/EasyEDA and KiCad's library), a netlist model, the circuit, and a scripted placement. Writers emit a `.kicad_pcb` + `.kicad_pro` and a Specctra `.dsn`; Freerouting 2.5.0 routes headless; a `.ses` reader merges tracks back; `kicad-cli` fills zones, runs DRC and exports fab files.

**Tech Stack:** Python 3.11 (asdf), pytest, easyeda2kicad, KiCad 10.0.0 `kicad-cli`, Freerouting 2.5.0 (`.jar`, Java 25 corretto).

**Spec:** `embedded/ddr-banging/channel-board/DESIGN.md`

## Global Constraints

- Work under `embedded/ddr-banging/channel-board/` on branch `sint-100-ddr-banging`; commit messages `feat(channel-board): … (SINT-101)`; **no `Co-Authored-By` or "Generated with Claude" lines** (Martin's CLAUDE.md).
- British English in all prose, comments and output text.
- `kicad-cli` is `/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli` (10.0.0). No `pcbnew` Python module exists — never import it.
- Freerouting: `freerouting-2.5.0.jar` from GitHub release `v2.5.0` of `freerouting/freerouting`, run with the asdf Java.
- Python: `ASDF_PYTHON_VERSION=3.11.15`; venv at `channel-board/.venv` (git-ignored).
- 4 layers: F.Cu signal, In1.Cu GND plane, In2.Cu +1V5 plane, B.Cu signal. 1.6 mm board.
- Design rules: track ≥ 0.15 mm, clearance ≥ 0.15 mm, vias 0.6 mm pad / 0.3 mm drill; power nets (+5V, +3V3, +1V5, VTT, GND) 0.5 mm tracks.
- **All components on the top side.** Components ≥ 2.5 mm from the board edge except the USB-C receptacle and the DIMM socket's latch overhang.
- Board ≈ 175 × 90 mm; DIMM socket TE **1-1932000-0, LCSC C5164821** (fallback C5171640).
- Lanes 1–7 translators are **DNP** (in the board, excluded from BOM/CPL).
- GPIO map is fixed by DESIGN.md §2 "Pin map" — the table is the contract.
- Long-running commands log full output to `/tmp/channel-board-<step>.log` (tell Martin the path so he can `less +F` it).
- **Never place orders, add to carts or spend money.** Ordering is Martin's.

## Review Focus

1. **Footprint rotation vs JLCPCB's preview** — EasyEDA/KiCad footprints often differ in 0° orientation; a wrong CPL rotation puts the RP2354B or a translator in backwards. Expect: every fitted extended part has an explicit rotation-offset entry, and the CPL rotation equals placement rotation + offset (test in Task 10).
2. **Socket pad numbering vs JEDEC numbering** — if the EasyEDA socket footprint numbers pads differently from JEDEC pin numbers, every DIMM signal is wrong. Expect: pad 1/48/49/120/121 positions match TE drawing C-1932000 sheet 3 (test in Task 4).
3. **Translator side/polarity** — OE and DIR are referenced to VCCA; A must be the 3.3 V (RP2354B) side and command/address DIR must select A→B. Expect: tests asserting VCCA pins on +3V3, VCCB on +1V5, C/A DIR on +3V3, OE# on GND (Task 6).
4. **Missing RP2354B supply pins** — the QFN-80 has many IOVDD/DVDD/VREG/ADC/USB/QSPI supply pins; one floating pin is a dead board. Expect: every power-type pin of every part is on a net (Task 5 check, run in Task 6).
5. **Freerouting leaving nets unrouted** — Freerouting can stop with connections left. Expect: build fails unless DRC reports zero errors *and* zero unconnected items (Task 9).

---

## File structure

```
embedded/ddr-banging/channel-board/
  DESIGN.md                 spec (exists)
  requirements.txt          pytest, easyeda2kicad
  .gitignore                .venv/, out/tmp/, *.ses, *.dsn, freerouting/
  board/
    __init__.py
    sexpr.py                KiCad s-expression reader/writer (Sym vs str)
    kicadlib.py             load .kicad_mod footprints and .kicad_sym symbol pins
    parts.py                parts catalogue: PartDef (LCSC, footprint, pins, fitted, rotation offset)
    jedec.py                DDR3 UDIMM 240-pin pinout table
    netlist.py              Circuit / Net / PartInstance + electrical checks
    circuit.py              build_circuit() — the channel board
    placement.py            place(circuit) → Placement (outline + positions)
    kicad_pcb.py            write .kicad_pcb and .kicad_pro
    dsn.py                  write Specctra .dsn
    ses.py                  read Freerouting .ses → tracks/vias
    fab.py                  BOM/CPL for JLCPCB, gerber zip
  build.py                  end-to-end CLI: circuit → place → route → DRC → fab
  fetch_parts.sh            easyeda2kicad for every LCSC part
  footprints/lcsc.pretty/   fetched footprints (committed)
  footprints/lcsc.kicad_sym fetched symbols (committed)
  tests/
    conftest.py
    test_sexpr.py  test_kicadlib.py  test_parts.py  test_jedec.py
    test_netlist.py  test_circuit.py  test_placement.py
    test_kicad_pcb.py  test_route.py  test_fab.py
  out/                      channel.kicad_pcb/.kicad_pro, gerbers zip, bom.csv, cpl.csv, renders, drc.json
```

---

### Task 1: Scaffold and s-expression reader/writer

**Files:**
- Create: `channel-board/requirements.txt`, `channel-board/.gitignore`, `channel-board/board/__init__.py`, `channel-board/board/sexpr.py`, `channel-board/tests/conftest.py`, `channel-board/tests/test_sexpr.py`

**Interfaces:**
- Produces: `board.sexpr.Sym` (str subclass for bare atoms), `parse(text) -> list`, `dumps(node) -> str`, `find(node, head) -> list|None`, `find_all(node, head) -> list[list]`.

- [ ] **Step 1: Scaffold**

```bash
cd ~/repos/nakomis/sinter/embedded/ddr-banging/channel-board
mkdir -p board tests footprints out
printf 'pytest==8.3.3\neasyeda2kicad==0.8.0\n' > requirements.txt
printf '.venv/\nout/tmp/\n*.dsn\n*.ses\nfreerouting/\n__pycache__/\n' > .gitignore
touch board/__init__.py
ASDF_PYTHON_VERSION=3.11.15 python -m venv .venv
.venv/bin/pip install -r requirements.txt
```
If `easyeda2kicad==0.8.0` does not resolve, run `.venv/bin/pip index versions easyeda2kicad` and pin the newest listed; record it in `requirements.txt`.

`tests/conftest.py`:
```python
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
KICAD_CLI = "/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli"
KICAD_FP = Path("/Applications/KiCad/KiCad.app/Contents/SharedSupport/footprints")
```

- [ ] **Step 2: Write the failing tests** — `tests/test_sexpr.py`

```python
from board.sexpr import Sym, parse, dumps, find, find_all


def test_parse_atoms_strings_and_nesting():
    node = parse('(pad "1" smd rect (at 0 -1.5 90) (layers "F.Cu" F.Paste))')
    assert node[0] == Sym("pad")
    assert node[1] == "1" and not isinstance(node[1], Sym)
    assert node[2] == Sym("smd")
    assert find(node, "at") == [Sym("at"), 0, -1.5, 90]
    assert find(node, "layers")[2] == Sym("F.Paste")


def test_escaped_quotes_round_trip():
    text = '(property "Value" "say \\"hi\\"")'
    assert dumps(parse(text)) == text


def test_numbers_keep_type():
    node = parse("(at 1 2.50 -3)")
    assert node[1] == 1 and isinstance(node[1], int)
    assert node[2] == 2.5 and isinstance(node[2], float)


def test_dumps_quotes_strings_not_symbols():
    assert dumps([Sym("net"), 3, "GND"]) == '(net 3 "GND")'


def test_find_all():
    node = parse('(fp (pad "1") (pad "2") (line))')
    assert [p[1] for p in find_all(node, "pad")] == ["1", "2"]
```

- [ ] **Step 3: Run to see them fail**

Run: `.venv/bin/pytest tests/test_sexpr.py -v` — Expected: FAIL (`ModuleNotFoundError: board.sexpr`).

- [ ] **Step 4: Implement `board/sexpr.py`**

```python
"""Minimal KiCad s-expression reader/writer.

Bare atoms parse to Sym, quoted strings to str, numbers to int/float, so a
round trip keeps KiCad's quoting.
"""
import re

_TOKEN = re.compile(r'\s*(?:(\()|(\))|"((?:[^"\\]|\\.)*)"|([^\s()"]+))', re.S)


class Sym(str):
    """A bare (unquoted) atom."""


def _atom(text):
    try:
        return int(text)
    except ValueError:
        pass
    try:
        return float(text)
    except ValueError:
        return Sym(text)


def parse(text):
    stack, cur = [], None
    for m in _TOKEN.finditer(text):
        lp, rp, qs, at = m.groups()
        if lp:
            new = []
            if cur is not None:
                cur.append(new)
                stack.append(cur)
            cur = new
        elif rp:
            if not stack:
                return cur
            cur = stack.pop()
        elif qs is not None:
            cur.append(re.sub(r'\\(.)', r'\1', qs))
        elif at:
            cur.append(_atom(at))
    raise ValueError("unbalanced s-expression")


def _fmt(v):
    if isinstance(v, list):
        return dumps(v)
    if isinstance(v, Sym):
        return str(v)
    if isinstance(v, str):
        return '"' + v.replace("\\", "\\\\").replace('"', '\\"') + '"'
    if isinstance(v, float):
        s = f"{v:.6f}".rstrip("0").rstrip(".")
        return s if s not in ("-0", "") else "0"
    return str(v)


def dumps(node):
    return "(" + " ".join(_fmt(v) for v in node) + ")"


def find(node, head):
    for v in node:
        if isinstance(v, list) and v and v[0] == head:
            return v
    return None


def find_all(node, head):
    return [v for v in node if isinstance(v, list) and v and v[0] == head]
```

- [ ] **Step 5: Run tests** — `.venv/bin/pytest tests/test_sexpr.py -v` → PASS (5 passed).

- [ ] **Step 6: Commit**

```bash
git add requirements.txt .gitignore board tests
git commit -m "feat(channel-board): scaffold and s-expression reader/writer (SINT-101)"
```

---

### Task 2: Footprint and symbol loaders

**Files:**
- Create: `board/kicadlib.py`, `tests/test_kicadlib.py`

**Interfaces:**
- Consumes: `board.sexpr.parse, find, find_all, Sym`.
- Produces:
  - `Pad(number: str, kind: str, shape: str, x: float, y: float, rot: float, w: float, h: float, drill: float|None, layers: list[str])`
  - `Footprint(name: str, node: list, pads: list[Pad], courtyard: tuple[float,float,float,float])` — courtyard as (xmin, ymin, xmax, ymax) in footprint coordinates; falls back to pad extents + 0.25 mm if no courtyard lines.
  - `load_footprint(path: Path) -> Footprint` — accepts `(footprint …)` and legacy `(module …)`; `node[0]` is normalised to `Sym("footprint")`.
  - `load_symbol_pins(path: Path, symbol: str) -> dict[str, list[str]]` — pin name → list of pin numbers (several pins can share a name, e.g. IOVDD).
  - `load_symbol_pin_types(path, symbol) -> dict[str, str]` — pin number → electrical type (`power_in`, `output`, `bidirectional`, …).

- [ ] **Step 1: Write the failing tests**

```python
from pathlib import Path
from conftest import KICAD_FP
from board.kicadlib import load_footprint, load_symbol_pins, load_symbol_pin_types

R0402 = KICAD_FP / "Resistor_SMD.pretty" / "R_0402_1005Metric.kicad_mod"
SYM = '''(kicad_symbol_lib (symbol "U" (symbol "U_1_1"
  (pin power_in line (at 0 0 0) (length 2.54) (name "VDD") (number "1"))
  (pin power_in line (at 0 0 0) (length 2.54) (name "VDD") (number "4"))
  (pin bidirectional line (at 0 0 0) (length 2.54) (name "IO0") (number "2")))))'''


def test_load_kicad_stdlib_footprint():
    fp = load_footprint(R0402)
    assert fp.node[0] == "footprint"
    assert sorted(p.number for p in fp.pads) == ["1", "2"]
    p1 = next(p for p in fp.pads if p.number == "1")
    assert p1.kind == "smd" and p1.x < 0 and p1.drill is None
    xmin, ymin, xmax, ymax = fp.courtyard
    assert xmin < p1.x < xmax


def test_symbol_pins_group_shared_names(tmp_path):
    f = tmp_path / "t.kicad_sym"
    f.write_text(SYM)
    assert load_symbol_pins(f, "U") == {"VDD": ["1", "4"], "IO0": ["2"]}
    assert load_symbol_pin_types(f, "U")["2"] == "bidirectional"


def test_legacy_module_head_is_normalised(tmp_path):
    f = tmp_path / "m.kicad_mod"
    f.write_text('(module X (layer F.Cu) (pad 1 thru_hole circle (at 0 0) (size 1 1) (drill 0.7) (layers *.Cu *.Mask)))')
    fp = load_footprint(f)
    assert fp.node[0] == "footprint" and fp.pads[0].drill == 0.7
```

- [ ] **Step 2: Run** `.venv/bin/pytest tests/test_kicadlib.py -v` → FAIL (module missing).

- [ ] **Step 3: Implement `board/kicadlib.py`**

```python
from dataclasses import dataclass
from pathlib import Path

from board.sexpr import Sym, find, find_all, parse


@dataclass
class Pad:
    number: str
    kind: str
    shape: str
    x: float
    y: float
    rot: float
    w: float
    h: float
    drill: float | None
    layers: list


@dataclass
class Footprint:
    name: str
    node: list
    pads: list
    courtyard: tuple


def _pad(node):
    at = find(node, "at")
    size = find(node, "size")
    drill = find(node, "drill")
    d = None
    if drill:
        nums = [v for v in drill[1:] if isinstance(v, (int, float))]
        d = float(nums[0]) if nums else None
    return Pad(str(node[1]), str(node[2]), str(node[3]), float(at[1]), float(at[2]),
               float(at[3]) if len(at) > 3 else 0.0, float(size[1]), float(size[2]), d,
               [str(v) for v in find(node, "layers")[1:]])


def _courtyard(node, pads):
    xs, ys = [], []
    for kind in ("fp_line", "fp_rect", "fp_poly", "fp_circle"):
        for g in find_all(node, kind):
            layer = find(g, "layer")
            if not layer or "CrtYd" not in str(layer[1]):
                continue
            for key in ("start", "end", "center"):
                pt = find(g, key)
                if pt:
                    xs.append(float(pt[1])); ys.append(float(pt[2]))
            pts = find(g, "pts")
            for xy in find_all(pts or [], "xy"):
                xs.append(float(xy[1])); ys.append(float(xy[2]))
    if not xs:
        for p in pads:
            xs += [p.x - p.w / 2 - 0.25, p.x + p.w / 2 + 0.25]
            ys += [p.y - p.h / 2 - 0.25, p.y + p.h / 2 + 0.25]
    return (min(xs), min(ys), max(xs), max(ys))


def load_footprint(path):
    node = parse(Path(path).read_text())
    node[0] = Sym("footprint")
    pads = [_pad(p) for p in find_all(node, "pad")]
    return Footprint(str(node[1]), node, pads, _courtyard(node, pads))


def _pins(path, symbol):
    lib = parse(Path(path).read_text())
    for s in find_all(lib, "symbol"):
        if s[1] == symbol:
            stack = [s]
            while stack:
                n = stack.pop()
                for v in n:
                    if isinstance(v, list) and v and v[0] == "pin":
                        yield v
                    elif isinstance(v, list) and v and v[0] == "symbol":
                        stack.append(v)
            return
    raise KeyError(f"symbol {symbol!r} not in {path}")


def load_symbol_pins(path, symbol):
    out = {}
    for pin in _pins(path, symbol):
        out.setdefault(str(find(pin, "name")[1]), []).append(str(find(pin, "number")[1]))
    return out


def load_symbol_pin_types(path, symbol):
    return {str(find(pin, "number")[1]): str(pin[1]) for pin in _pins(path, symbol)}
```

- [ ] **Step 4: Run** `.venv/bin/pytest tests -v` → PASS.

- [ ] **Step 5: Commit** — `git add board/kicadlib.py tests/test_kicadlib.py && git commit -m "feat(channel-board): footprint and symbol loaders (SINT-101)"`

---

### Task 3: Parts — fetch from LCSC and catalogue them

**Files:**
- Create: `fetch_parts.sh`, `footprints/lcsc.pretty/*`, `footprints/lcsc.kicad_sym`, `board/parts.py`, `tests/test_parts.py`

**Interfaces:**
- Consumes: `load_footprint`, `load_symbol_pins`, `load_symbol_pin_types`.
- Produces: `PartDef(key, value, lcsc, footprint: Footprint, pins: dict[str, list[str]], pin_types: dict[str,str], extended: bool, tht: bool, rotation_offset: float)`; `PARTS: dict[str, PartDef]`; `part(key) -> PartDef`.
  Keys used by later tasks: `RP2354B, DIMM, XLAT, DEC138, EXPANDER, VTTREG, LDO33, BUCK, L_BUCK, L_CORE, XTAL12, USBC, SW, LED_G, LED_R, JST_SWD, HDR_2X25, HDR_2X4, C100N, C1U, C4U7, C10U, C22U, C15P, R27, R1K, R820, R5K1, R10K, R33, R0`.

**Part candidates** (MPN → LCSC). Rows marked ✔ were confirmed in this session; the rest must be looked up on `https://jlcpcb.com/parts` (search the MPN, prefer **Basic**, then in-stock Extended) and the C-number written into `fetch_parts.sh`:

| Key | MPN / value | LCSC |
|---|---|---|
| RP2354B | RP2354B (QFN-80) | ✔ C39843328 |
| DIMM | TE 1-1932000-0 | ✔ C5164821 |
| XLAT | SN74AVC16T245DGGR | C148481 |
| VTTREG | TPS51200DRCR | C34771 |
| EXPANDER | PCA9534PWR | C133803 |
| DEC138 | 74LVC138APW,118 (Nexperia) | look up |
| LDO33 | AMS1117-3.3 (SOT-223) | C6186 |
| BUCK | TPS563201DDCR (SOT-23-6, 3 A) | look up |
| L_BUCK | 2.2 µH ≥ 4 A shielded (e.g. 5×5 mm) | look up |
| L_CORE | 3.3 µH per RP2350 hardware design guide (Pico 2: Abracon AOTA-B201610S3R3-101-T) | look up |
| XTAL12 | 12 MHz 3225, e.g. X322512MSB4SI | C9002 |
| USBC | TYPE-C-31-M-12 (16-pin USB 2.0) | C165948 |
| SW | TS-1187A-B-A-B | C318884 |
| LED_G / LED_R | 0603 green / red | C72043 / C2286 |
| JST_SWD | SM03B-SRSS-TB(LF)(SN) | C160403 |
| HDR_2X25 / HDR_2X4 | 2.54 mm male vertical THT | look up |
| C100N / C1U / C4U7 / C10U / C22U / C15P | 0402 100 nF / 0402 1 µF / 0402 4.7 µF / 0603 10 µF / 0805 22 µF / 0402 15 pF | C1525 / look up / look up / C19702 / C45783 / look up |
| R27 / R1K / R820 / R5K1 / R10K / R33 / R0 | 0402 resistors | look up / C11702 / look up / C25905 / C25744 / look up / C17168 |

- [ ] **Step 1: Look up the missing C-numbers** in a browser (Martin, or Claude via Chrome) and fill the table above into `fetch_parts.sh`. **Human checkpoint:** show Martin the table with stock and Basic/Extended for each and wait for "ok" — stock changes daily and the SN74AVC16T245 had only ~490.

- [ ] **Step 2: `fetch_parts.sh`**

```bash
#!/usr/bin/env bash
# Fetch footprints + symbols for every LCSC part into footprints/lcsc.*
set -euo pipefail
cd "$(dirname "$0")"
PARTS=(
  C39843328 C5164821 C148481 C34771 C133803 C6186 C9002 C165948 C318884
  C72043 C2286 C160403 C1525 C19702 C45783 C11702 C25905 C25744 C17168
  # + the C-numbers looked up in Step 1, one per line
)
for c in "${PARTS[@]}"; do
  .venv/bin/easyeda2kicad --symbol --footprint --lcsc_id="$c" --output footprints/lcsc --overwrite
done
```
Run: `bash fetch_parts.sh > /tmp/channel-board-fetch.log 2>&1; tail -5 /tmp/channel-board-fetch.log`
Expected: `footprints/lcsc.pretty/` has one `.kicad_mod` per part, `footprints/lcsc.kicad_sym` exists. Any part that fails to fetch is a wrong C-number — fix and rerun.

- [ ] **Step 3: Write the failing tests** — `tests/test_parts.py`

```python
from board.parts import PARTS, part


def test_every_part_has_footprint_and_lcsc():
    for key, p in PARTS.items():
        assert p.lcsc.startswith("C"), key
        assert p.footprint.pads, key


def test_rp2354b_pins():
    p = part("RP2354B")
    for i in range(48):
        assert f"GPIO{i}" in p.pins, f"GPIO{i}"
    for name in ("IOVDD", "DVDD", "VREG_VIN", "VREG_LX", "VREG_FB", "VREG_AVDD",
                 "USB_DP", "USB_DM", "XIN", "XOUT", "SWCLK", "SWDIO", "RUN",
                 "USB_OTP_VDD", "ADC_AVDD", "QSPI_IOVDD"):
        assert name in p.pins, name
    # QSPI_SS (BOOTSEL) may be absent with in-package flash; Task 6 handles both cases.
    assert len(p.footprint.pads) >= 81          # 80 + exposed pad


def test_translator_pins():
    p = part("XLAT")
    for half in (1, 2):
        assert f"{half}DIR" in p.pins and f"{half}OE#" in p.pins
        for b in range(1, 9):
            assert f"{half}A{b}" in p.pins and f"{half}B{b}" in p.pins
    assert "VCCA" in p.pins and "VCCB" in p.pins


def test_dimm_socket_has_240_numbered_contacts():
    p = part("DIMM")
    nums = {pad.number for pad in p.footprint.pads}
    assert {str(n) for n in range(1, 241)} <= nums
    contacts = [pad for pad in p.footprint.pads if pad.number.isdigit() and 1 <= int(pad.number) <= 240]
    assert all(abs(pad.drill - 0.70) <= 0.08 for pad in contacts)


def test_tht_flags():
    assert part("DIMM").tht and part("HDR_2X25").tht and not part("RP2354B").tht
```

- [ ] **Step 4: Run** → FAIL (no `board.parts`).

- [ ] **Step 5: Implement `board/parts.py`**

EasyEDA pin names vary (`GPIO0` vs `GP0`, `~OE` vs `OE#`). `ALIASES` maps them onto the names above; fill it by printing the fetched names (`.venv/bin/python -c "from board.parts import *; print(sorted(part('XLAT').pins))"`) and adding an alias for each mismatch — never rename by guessing.

```python
from dataclasses import dataclass
from pathlib import Path

from board.kicadlib import Footprint, load_footprint, load_symbol_pin_types, load_symbol_pins

HERE = Path(__file__).resolve().parent.parent
LCSC_FP = HERE / "footprints" / "lcsc.pretty"
LCSC_SYM = HERE / "footprints" / "lcsc.kicad_sym"


@dataclass
class PartDef:
    key: str
    value: str
    lcsc: str
    footprint: Footprint
    pins: dict
    pin_types: dict
    extended: bool
    tht: bool
    rotation_offset: float   # degrees added to placement rotation for JLCPCB CPL


# key: (value, lcsc, footprint file stem, symbol name, extended, rotation_offset)
_TABLE = {
    "RP2354B": ("RP2354B", "C39843328", "QFN-80_L10.0-W10.0-P0.40-TL-EP", "RP2354B", True, 0),
    "DIMM": ("1-1932000-0", "C5164821", "DIMM-TH_1-1932000-0", "1-1932000-0", True, 0),
    # … one row per key in the table above; footprint stem and symbol name are
    # exactly what easyeda2kicad wrote (ls footprints/lcsc.pretty).
}

# fetched-name -> our-name, per key
ALIASES = {
    "XLAT": {},
    "RP2354B": {},
}


def _load(key, row):
    value, lcsc, fp_stem, sym, extended, rot = row
    fp = load_footprint(LCSC_FP / f"{fp_stem}.kicad_mod")
    raw = load_symbol_pins(LCSC_SYM, sym)
    alias = ALIASES.get(key, {})
    pins = {}
    for name, nums in raw.items():
        pins.setdefault(alias.get(name, name), []).extend(nums)
    tht = any(p.kind == "thru_hole" for p in fp.pads)
    return PartDef(key, value, lcsc, fp, pins, load_symbol_pin_types(LCSC_SYM, sym), extended, tht, rot)


PARTS = {k: _load(k, row) for k, row in _TABLE.items()}


def part(key):
    return PARTS[key]
```

Rotation offsets start at 0 for all parts; Task 10 fills them from JLCPCB's preview conventions.

- [ ] **Step 6: Run** `.venv/bin/pytest tests -v` → PASS.

- [ ] **Step 7: Commit** — `git add fetch_parts.sh footprints board/parts.py tests/test_parts.py && git commit -m "feat(channel-board): parts catalogue from LCSC/EasyEDA (SINT-101)"`

---

### Task 4: DDR3 UDIMM pinout, checked against the socket footprint

**Files:**
- Create: `board/jedec.py`, `tests/test_jedec.py`

**Interfaces:**
- Produces: `UDIMM_PINS: dict[int, str]` (1–240 → signal name, JEDEC names: `VSS`, `VDD`, `VREFDQ`, `VREFCA`, `VTT`, `VDDSPD`, `DQ0`…`DQ63`, `CB0`…`CB7`, `DQS0`…`DQS8`, `DQS0#`…`DQS8#`, `DM0`…`DM8`, `A0`…`A15`, `BA0`…`BA2`, `RAS#`, `CAS#`, `WE#`, `S0#`…`S3#`, `CKE0`, `CKE1`, `ODT0`, `ODT1`, `CK0`, `CK0#`, `CK1`, `CK1#`, `RESET#`, `SA0`…`SA2`, `SCL`, `SDA`, `NC`), `lane_of(signal) -> int|None` (DQ/DQS/DM → byte lane 0–7, CB/DQS8/DM8 → 8).

- [ ] **Step 1: Transcribe the table** from the Micron DDR3 UDIMM datasheet pin assignment table (MT16JTF / MT18JSF "240-Pin DDR3 UDIMM Pin Assignments"; fetch `https://www.micron.com/` product PDF or the copy used for `../pogo-pins.md`). Write `UDIMM_PINS` as a literal dict, one line per 10 pins. Pins that are NC on a 2 GB 2Rx8 ECC UDIMM keep their JEDEC name (e.g. `A14`, `S2#`); the circuit leaves them unconnected.

- [ ] **Step 2: Write the tests** — these pin the transcription against the 76 pins already verified in `../pogo-pins.md` and against TE drawing C-1932000 sheet 3 geometry.

```python
from collections import Counter
from board.jedec import UDIMM_PINS, lane_of
from board.parts import part

POGO_VERIFIED = {
    1: "VREFDQ", 3: "DQ0", 4: "DQ1", 6: "DQS0#", 7: "DQS0", 9: "DQ2", 10: "DQ3",
    50: "CKE0", 52: "BA2", 55: "A11", 56: "A7", 58: "A5", 59: "A4", 61: "A2",
    67: "VREFCA", 70: "A10", 71: "BA0", 73: "WE#", 74: "CAS#", 76: "S1#", 77: "ODT1",
    117: "SA0", 118: "SCL", 119: "SA2", 120: "VTT",
    122: "DQ4", 123: "DQ5", 125: "DM0", 128: "DQ6", 129: "DQ7", 168: "RESET#",
    169: "CKE1", 174: "A12", 175: "A9", 177: "A8", 178: "A6", 180: "A3", 181: "A1",
    184: "CK0", 185: "CK0#", 188: "A0", 190: "BA1", 192: "RAS#", 193: "S0#",
    195: "ODT0", 196: "A13", 236: "VDDSPD", 237: "SA1", 238: "SDA", 240: "VTT",
}


def test_complete():
    assert sorted(UDIMM_PINS) == list(range(1, 241))


def test_agrees_with_pogo_verified_pins():
    for pin, sig in POGO_VERIFIED.items():
        assert UDIMM_PINS[pin] == sig, pin
    for pin in (2, 5, 8, 11, 80, 83, 121, 124, 127, 130, 199, 202):
        assert UDIMM_PINS[pin] == "VSS", pin
    for pin in (51, 54, 57, 60, 62, 65, 66, 170, 173, 176, 179, 182, 183, 186):
        assert UDIMM_PINS[pin] == "VDD", pin


def test_each_data_signal_once():
    c = Counter(UDIMM_PINS.values())
    for i in range(64):
        assert c[f"DQ{i}"] == 1
    for i in range(9):
        assert c[f"DQS{i}"] == 1 and c[f"DQS{i}#"] == 1
    for i in range(8):
        assert c[f"CB{i}"] == 1


def test_lane_of():
    assert lane_of("DQ0") == 0 and lane_of("DQ63") == 7 and lane_of("DQS3#") == 3
    assert lane_of("DM5") == 5 and lane_of("CB2") == 8 and lane_of("A0") is None


def _pad(n):
    return next(p for p in part("DIMM").footprint.pads if p.number == str(n))


def test_socket_numbering_matches_te_drawing():
    # C-1932000 sheet 3: pins on 1.00 mm pitch; pin 48 -> 49 spans the key (5.00 mm);
    # pin 1 -> 120 is 123.00 mm; pin 121 sits opposite pin 1.
    assert abs(abs(_pad(2).x - _pad(1).x) - 1.00) < 0.02
    assert abs(abs(_pad(49).x - _pad(48).x) - 5.00) < 0.05
    assert abs(abs(_pad(120).x - _pad(1).x) - 123.00) < 0.05
    assert abs(_pad(121).x - _pad(1).x) < 0.6
    assert abs(_pad(1).y - _pad(2).y) > 1.0          # staggered rows
```
If `test_socket_numbering_matches_te_drawing` fails, compare the EasyEDA footprint against TE drawing sheet 3 (`~/repos/nakomis/sinter/docs/ENG_CD_1932000_H7.pdf`) and **renumber the pads** in a corrected copy `footprints/lcsc.pretty/DIMM-TH_1-1932000-0_JEDEC.kicad_mod` (point `_TABLE["DIMM"]` at it). Never weaken the test.

- [ ] **Step 3: Implement `lane_of`** in `board/jedec.py` beneath the table:

```python
import re


def lane_of(signal):
    m = re.fullmatch(r"(DQ|DQS|DM)(\d+)#?", signal)
    if m:
        n = int(m.group(2))
        return n // 8 if m.group(1) == "DQ" else n
    if re.fullmatch(r"CB\d", signal):
        return 8
    return None
```

- [ ] **Step 4: Run** `.venv/bin/pytest tests/test_jedec.py -v` → PASS.

- [ ] **Step 5: Commit** — `git add board/jedec.py tests/test_jedec.py && git commit -m "feat(channel-board): DDR3 UDIMM pinout, checked against socket footprint (SINT-101)"`

---

### Task 5: Netlist model and electrical checks

**Files:**
- Create: `board/netlist.py`, `tests/test_netlist.py`

**Interfaces:**
- Consumes: `PartDef`.
- Produces:
  - `PartInstance(ref: str, part: PartDef, dnp: bool, value: str)`
  - `Circuit` with `add(key, ref=None, dnp=False, value=None) -> PartInstance` (auto refs by prefix: U, J, R, C, L, Y, SW, D), `connect(net: str, *terminals)` where a terminal is `(inst, pin_name)` (connects **every** pad number of that pin name) or `(inst, pin_name, pad_number)`, `nets: dict[str, set[tuple[str,str]]]` (net → {(ref, pad)}), `instances: dict[str, PartInstance]`, `net_of(ref, pad) -> str|None`.
  - `check(circuit) -> list[str]` — problems: pad on two nets; net with one pad; power_in pin unconnected on a fitted part; two `output` pins on one net; unknown pin name (raises at `connect` time instead).
  - `NC` marker: `circuit.no_connect(inst, pin_name)` records deliberate no-connects; `check` reports unconnected non-power pins only if they are not in this set and not `no_connect`/`passive` typed.

- [ ] **Step 1: Write the failing tests**

```python
import pytest
from board.netlist import Circuit, check


def test_connect_and_lookup():
    c = Circuit()
    r1, r2 = c.add("R10K"), c.add("R10K")
    c.connect("A", (r1, "1"), (r2, "1"))
    c.connect("B", (r1, "2"), (r2, "2"))
    assert r1.ref == "R1" and r2.ref == "R2"
    assert c.net_of("R1", "1") == "A"
    assert check(c) == []


def test_pad_on_two_nets_is_reported():
    c = Circuit()
    r1, r2 = c.add("R10K"), c.add("R10K")
    c.connect("A", (r1, "1"), (r2, "1"))
    c.connect("B", (r1, "1"), (r2, "2"))
    assert any("two nets" in p for p in check(c))


def test_single_pad_net_is_reported():
    c = Circuit()
    r1 = c.add("R10K")
    c.connect("LONELY", (r1, "1"))
    assert any("LONELY" in p for p in check(c))


def test_unknown_pin_raises():
    c = Circuit()
    r1 = c.add("R10K")
    with pytest.raises(KeyError):
        c.connect("A", (r1, "nonsense"))


def test_unconnected_power_pin_reported():
    c = Circuit()
    u = c.add("EXPANDER")
    problems = check(c)
    assert any("power" in p and u.ref in p for p in problems)


def test_dnp_part_power_pins_not_required():
    c = Circuit()
    c.add("XLAT", dnp=True)
    assert not any("power" in p for p in check(c))
```
(Passive parts from EasyEDA name pins `"1"`/`"2"`; adjust the strings if Task 3 printed otherwise.)

- [ ] **Step 2: Run** → FAIL.

- [ ] **Step 3: Implement `board/netlist.py`**

```python
from dataclasses import dataclass, field

from board.parts import part

PREFIX = {"L_BUCK": "L", "L_CORE": "L", "XTAL12": "Y", "SW": "SW", "LED_G": "D", "LED_R": "D",
          "USBC": "J", "JST_SWD": "J", "HDR_2X25": "J", "HDR_2X4": "J", "DIMM": "J"}


def _prefix(key):
    if key in PREFIX:
        return PREFIX[key]
    if key[0] in "RC" and key[1].isdigit():      # R10K, C100N, …
        return key[0]
    return "U"


@dataclass
class PartInstance:
    ref: str
    part: object
    dnp: bool
    value: str


@dataclass
class Circuit:
    instances: dict = field(default_factory=dict)
    nets: dict = field(default_factory=dict)
    _pad_net: dict = field(default_factory=dict)
    _conflicts: list = field(default_factory=list)
    _nc: set = field(default_factory=set)

    def add(self, key, ref=None, dnp=False, value=None):
        p = part(key)
        if ref is None:
            prefix = _prefix(key)
            n = 1 + sum(1 for r in self.instances if r.rstrip("0123456789") == prefix)
            ref = f"{prefix}{n}"
        inst = PartInstance(ref, p, dnp, value or p.value)
        self.instances[ref] = inst
        return inst

    def _pads(self, term):
        inst, name = term[0], term[1]
        if name not in inst.part.pins:
            raise KeyError(f"{inst.ref}: no pin {name!r}")
        return [term[2]] if len(term) > 2 else inst.part.pins[name]

    def connect(self, net, *terms):
        for t in terms:
            for pad in self._pads(t):
                key = (t[0].ref, pad)
                old = self._pad_net.get(key)
                if old and old != net:
                    self._conflicts.append(f"{key[0]}.{key[1]} on two nets: {old}, {net}")
                    continue
                self._pad_net[key] = net
                self.nets.setdefault(net, set()).add(key)

    def no_connect(self, inst, name):
        for pad in self._pads((inst, name)):
            self._nc.add((inst.ref, pad))

    def net_of(self, ref, pad):
        return self._pad_net.get((ref, pad))


def check(c):
    problems = list(c._conflicts)
    for net, pads in c.nets.items():
        if len(pads) < 2:
            problems.append(f"net {net} has only one pad")
        outs = [k for k in pads if c.instances[k[0]].part.pin_types.get(k[1]) == "output"]
        if len(outs) > 1:
            problems.append(f"net {net} has several outputs: {outs}")
    for ref, inst in c.instances.items():
        if inst.dnp:
            continue
        for pad, typ in inst.part.pin_types.items():
            if typ in ("power_in", "power_out") and (ref, pad) not in c._pad_net and (ref, pad) not in c._nc:
                problems.append(f"{ref} pad {pad}: power pin unconnected")
    return problems
```

- [ ] **Step 4: Run** `.venv/bin/pytest tests/test_netlist.py -v` → PASS.

- [ ] **Step 5: Commit** — `"feat(channel-board): netlist model and electrical checks (SINT-101)"`

---

### Task 5b: Translator test board (spike rehearsal) — circuit and placement

Added 2026-10-10 (Martin): a JLCPCB-assembled SN74AVC16T245 breakout so the DIMM spike tests the board's real translator, and so Tasks 8–10 are proven on a cheap order first. **Execution order is now 1–5, 5b, 8–10 (targeting this board), then pause for the spike, then 6–7, re-run 8–10 for the channel board, 11.**

**Files:**
- Create: `board/xlat_test.py`, `tests/test_xlat_test.py`

**Interfaces:**
- Consumes: `Circuit`, `check`, `part()`; `Placement` dataclass from `board/placement.py` (create the module here with `Placement(outline, at)` and `bbox(placement, ref)` exactly as Task 7's interface describes; Task 7 later adds `place()` for the channel board).
- Produces: `build_xlat_test() -> Circuit`; `place_xlat_test(circuit) -> Placement`.

Design (a pre-assembled TSSOP-48 → DIP-48 adapter):
- U1 = `XLAT` (SN74AVC16T245DGGR), fitted.
- J1, J2 = 1×24 2.54 mm male headers (THT, key `HDR_1X24` — look up a JLCPCB C-number via Martin/Chrome, add to parts like Task 3), 15.24 mm (0.6") apart, so the board is DIP-48: **J1 pin n = U1 pin n (1–24), J2 pin k = U1 pin 49−k (48–25)** in standard DIP order (pin 25 opposite pin 24).
- Nets are named after the chip pins (`1DIR`, `1B1`, …, `VCCA`, `VCCB`, `GND`), connecting U1 pin to its header pin.
- Decoupling: per VCCA pin and per VCCB pin, one `C100N` and one `C1U` to GND, placed within 2 mm of that pin.
- Outline about 65 × 20 mm (headers inset 2.5 mm from the long edges), 2 layers: the writer in Task 8 must accept a layer count (`layers=2|4`; 2-layer boards get no inner planes, GND routed as tracks/pour on B.Cu).
- Silkscreen: pin 1 marks on U1 and both headers; "SINT-101 AVC16T245" and the VCCA = 3.3 V / VCCB = 1.5 V reminder.

Tests (`tests/test_xlat_test.py`):
```python
from board.netlist import check
from board.xlat_test import build_xlat_test, place_xlat_test

C = build_xlat_test()


def test_clean():
    assert check(C) == []


def test_dip_mapping():
    for n in range(1, 25):
        assert C.net_of("J1", str(n)) == C.net_of("U1", str(n)), n
    for k in range(1, 25):
        assert C.net_of("J2", str(k)) == C.net_of("U1", str(49 - k)), k


def test_every_supply_pin_decoupled():
    u = C.instances["U1"].part
    for rail in ("VCCA", "VCCB"):
        caps = [r for r, i in C.instances.items() if i.part.key in ("C100N", "C1U")
                and rail in (C.net_of(r, "1"), C.net_of(r, "2"))]
        assert len(caps) >= 2 * len(u.pins[rail]), rail


def test_placement_inside_outline():
    from board.placement import bbox
    P = place_xlat_test(C)
    x0, y0, x1, y1 = P.outline
    for ref in C.instances:
        a = bbox(P, ref)
        assert a[0] >= x0 and a[1] >= y0 and a[2] <= x1 and a[3] <= y1, ref
```

Then Tasks 8–10 run against this board first (`build.py --board xlat-test`), producing `out/xlat-test/` Gerbers/BOM/CPL; **Martin orders it** (human checkpoint), then the plan pauses for the DIMM spike (SINT-100) before Task 6.

---

### Task 6: The circuit

**Files:**
- Create: `board/circuit.py`, `tests/test_circuit.py`

**Interfaces:**
- Consumes: `Circuit`, `check`, `UDIMM_PINS`, `lane_of`.
- Produces: `build_circuit() -> Circuit`; `GPIO_MAP: dict[int, str]` (GPIO → net name, exactly DESIGN.md §2); refs fixed for later tasks: `U1` RP2354B, `J1` DIMM socket, `U2`/`U3` C/A translators, `U10`…`U17` lane 0–7 translators (`U11`–`U17` DNP), `U4` decoder, `U5` expander, `U6` TPS51200, `U7` LDO, `U8` buck, `J2` USB-C, `J3` SWD, `J4` 2×25 header, `J5` ESP32 header.

Net names: `+5V`, `+3V3`, `+1V5`, `+1V1` (RP2354B DVDD), `VTT`, `VREF`, `GND`; DIMM signals use JEDEC names with a `D_` prefix on the 1.5 V side (`D_A0`, `D_DQ0` …, lane k data `D_L{k}_DQ{0..7}`, `D_L{k}_DQS`, `D_L{k}_DQS#`, `D_L{k}_DM`); 3.3 V side GPIO nets are the `GPIO_MAP` names (`DQ0`…`DQ7`, `DQS`, `DQS#`, `DIR_DQ`, `DIR_DQS`, `LANE0`…`LANE2`, `CK`, `CK#`, `CKE`, `CS0#`, `CS1#`, `RAS#`, `CAS#`, `WE#`, `BA0`…`BA2`, `A0`…`A13`, `READY`, `SDA`, `SCL`, `SPI_RX`, `SPI_CS`, `SPI_SCK`, `SPI_TX`, `GPIO15`).

- [ ] **Step 1: Write the failing tests** — `tests/test_circuit.py`

```python
from board.circuit import GPIO_MAP, build_circuit
from board.jedec import UDIMM_PINS, lane_of
from board.netlist import check

C = build_circuit()


def test_no_electrical_problems():
    assert check(C) == []


def test_gpio_map_is_the_spec_table():
    expected = {**{i: f"DQ{i}" for i in range(8)}, 8: "DQS", 9: "DQS#", 10: "DIR_DQ",
                11: "DIR_DQS", 12: "LANE0", 13: "LANE1", 14: "LANE2", 15: "GPIO15",
                16: "CK", 17: "CK#", 18: "CKE", 19: "CS0#", 20: "CS1#", 21: "RAS#",
                22: "CAS#", 23: "WE#", 24: "BA0", 25: "BA1", 26: "BA2",
                **{27 + i: f"A{i}" for i in range(14)},
                41: "READY", 42: "SDA", 43: "SCL",
                44: "SPI_RX", 45: "SPI_CS", 46: "SPI_SCK", 47: "SPI_TX"}
    assert GPIO_MAP == expected
    u1 = C.instances["U1"]
    for gpio, net in GPIO_MAP.items():
        for pad in u1.part.pins[f"GPIO{gpio}"]:
            assert C.net_of("U1", pad) == net, gpio


def test_pio_groups_fit_their_windows():
    data = [g for g, n in GPIO_MAP.items() if n.startswith("DQ")]
    cmd = [g for g, n in GPIO_MAP.items() if n in ("CK", "CK#", "CKE", "CS0#", "CS1#", "RAS#",
                                                   "CAS#", "WE#") or n.startswith(("BA", "A"))]
    assert max(data) <= 31 and min(cmd) >= 16 and max(cmd) <= 47
    assert sorted(cmd) == list(range(min(cmd), max(cmd) + 1))


def _sock(pin):
    return C.net_of("J1", str(pin))


def test_every_dimm_pin_on_its_expected_net():
    for pin, sig in UDIMM_PINS.items():
        net = _sock(pin)
        lane = lane_of(sig)
        if sig == "VSS":
            assert net == "GND", pin
        elif sig == "VDD":
            assert net == "+1V5", pin
        elif sig in ("VREFDQ", "VREFCA"):
            assert net == "VREF", pin
        elif sig == "VTT":
            assert net == "VTT", pin
        elif sig == "VDDSPD":
            assert net == "+3V3", pin
        elif sig in ("SCL", "SDA"):
            assert net == sig, pin
        elif sig in ("SA0", "SA1", "SA2", "ODT0", "ODT1"):
            assert net == "GND", pin
        elif lane is not None and lane < 8:
            suffix = "DM" if sig.startswith("DM") else ("DQS#" if sig.endswith("#") and sig.startswith("DQS")
                      else "DQS" if sig.startswith("DQS") else f"DQ{int(sig[2:]) % 8}")
            assert net == f"D_L{lane}_{suffix}", pin
        elif sig in ("CKE0", "CKE1"):
            assert net == "D_CKE", pin
        elif sig in ("S0#", "S1#", "RAS#", "CAS#", "WE#", "RESET#", "CK0", "CK0#") \
                or sig.startswith(("BA",)) or sig in {f"A{i}" for i in range(14)}:
            assert net == f"D_{sig}", pin
        else:
            assert net is None, (pin, sig)     # NC, ECC lane, CK1, A14/A15, S2#/S3#


def test_translator_sides_and_polarity():
    for ref in ("U2", "U3", *[f"U1{k}" for k in range(8)]):
        u = C.instances[ref].part
        for pad in u.pins["VCCA"]:
            assert C.net_of(ref, pad) == "+3V3", ref
        for pad in u.pins["VCCB"]:
            assert C.net_of(ref, pad) == "+1V5", ref
    for ref in ("U2", "U3"):
        u = C.instances[ref].part
        for half in (1, 2):
            assert C.net_of(ref, u.pins[f"{half}DIR"][0]) == "+3V3"   # A -> B
            assert C.net_of(ref, u.pins[f"{half}OE#"][0]) == "GND"


def test_lane_switching():
    for k in range(8):
        ref = f"U1{k}"
        u = C.instances[ref].part
        assert C.instances[ref].dnp == (k != 0)
        assert C.net_of(ref, u.pins["1OE#"][0]) == f"LANE_SEL{k}#"
        assert C.net_of(ref, u.pins["2OE#"][0]) == f"LANE_SEL{k}#"
        assert C.net_of(ref, u.pins["1DIR"][0]) == "DIR_DQ"
        assert C.net_of(ref, u.pins["2DIR"][0]) == "DIR_DQS"
        assert C.net_of(ref, u.pins["1A1"][0]) == "DQ0"
        assert C.net_of(ref, u.pins["1B1"][0]) == f"D_L{k}_DQ0"
    dec = C.instances["U4"].part
    for k in range(8):
        assert C.net_of("U4", dec.pins[f"Y{k}"][0]) == f"LANE_SEL{k}#"


def test_dm_divider_ratio():
    # each D_Lk_DM is fed from LANE_SELk# through 1k (top) and 820R (bottom) to GND
    for k in range(8):
        tops = [r for r, i in C.instances.items() if i.part.key == "R1K"
                and {C.net_of(r, "1"), C.net_of(r, "2")} == {f"LANE_SEL{k}#", f"D_L{k}_DM"}]
        bots = [r for r, i in C.instances.items() if i.part.key == "R820"
                and {C.net_of(r, "1"), C.net_of(r, "2")} == {f"D_L{k}_DM", "GND"}]
        assert len(tops) == 1 and len(bots) == 1, k


def test_only_lane0_translator_fitted_among_lanes():
    fitted = [r for r, i in C.instances.items() if i.part.key == "XLAT" and not i.dnp]
    assert sorted(fitted) == ["U10", "U2", "U3"]
```

- [ ] **Step 2: Run** → FAIL.

- [ ] **Step 3: Implement `board/circuit.py`.** Build in this order, one helper per block; each block's values come from the stated source.

```python
from board.jedec import UDIMM_PINS, lane_of
from board.netlist import Circuit

GPIO_MAP = {**{i: f"DQ{i}" for i in range(8)}, 8: "DQS", 9: "DQS#", 10: "DIR_DQ",
            11: "DIR_DQS", 12: "LANE0", 13: "LANE1", 14: "LANE2", 15: "GPIO15",
            16: "CK", 17: "CK#", 18: "CKE", 19: "CS0#", 20: "CS1#", 21: "RAS#",
            22: "CAS#", 23: "WE#", 24: "BA0", 25: "BA1", 26: "BA2",
            **{27 + i: f"A{i}" for i in range(14)},
            41: "READY", 42: "SDA", 43: "SCL",
            44: "SPI_RX", 45: "SPI_CS", 46: "SPI_SCK", 47: "SPI_TX"}

CA_SIGNALS = ["CK", "CK#", "CKE", "CS0#", "CS1#", "RAS#", "CAS#", "WE#",
              "BA0", "BA1", "BA2", *[f"A{i}" for i in range(14)]]          # 25
# 3.3 V GPIO net -> 1.5 V DIMM net
CA_DIMM = {"CK": "D_CK0", "CK#": "D_CK0#", "CKE": "D_CKE", "CS0#": "D_S0#", "CS1#": "D_S1#",
           **{s: f"D_{s}" for s in CA_SIGNALS if s not in ("CK", "CK#", "CKE", "CS0#", "CS1#")}}


def decouple(c, net, n, key="C100N"):
    for _ in range(n):
        cap = c.add(key)
        c.connect(net, (cap, "1"))
        c.connect("GND", (cap, "2"))


def resistor(c, key, a, b):
    r = c.add(key)
    c.connect(a, (r, "1"))
    c.connect(b, (r, "2"))
    return r


def power(c):
    """USB-C 5 V in, AMS1117 3.3 V, TPS563201 1.5 V buck, TPS51200 VTT/VREF.
    Buck feedback divider: Vout = 0.768 V × (1 + Rtop/Rbot) (TPS563201 datasheet §8.2);
    pick Rtop/Rbot from E96 so Vout = 1.50 V ± 1 % and add both values to parts."""
    j = c.add("USBC", ref="J2")
    c.connect("+5V", (j, "VBUS"))
    c.connect("GND", (j, "GND"))
    resistor(c, "R5K1", "CC1", "GND"); c.connect("CC1", (j, "CC1"))
    resistor(c, "R5K1", "CC2", "GND"); c.connect("CC2", (j, "CC2"))
    # … LDO U7 (+5V -> +3V3, 22 µF in/out), buck U8 + L_BUCK + feedback divider
    #   (+5V -> +1V5, 22 µF ×2 out), TPS51200 U6 (VLDOIN/VDDQSNS = +1V5, VIN = +3V3,
    #   VO = VTT, REFOUT = VREF, EN = VTT_EN from expander, 10 µF on VO, 100 nF on REFOUT,
    #   per TPS51200 datasheet typical application), power LED on +3V3 via 1k.


def rp2354b(c):
    """RP2354B support per Raspberry Pi 'Hardware design with RP2350', RP2350B minimal design:
    IOVDD/QSPI_IOVDD/USB_OTP_VDD/ADC_AVDD/VREG_VIN on +3V3 (100 nF per pin, 4.7 µF on VREG_VIN),
    VREG_AVDD from +3V3 through 33 Ω with 4.7 µF, VREG_LX -> L_CORE 3.3 µH -> +1V1,
    VREG_FB and DVDD on +1V1 (100 nF per DVDD pin, 4.7 µF bulk), VREG_PGND/GND/EP on GND,
    12 MHz crystal XIN/XOUT with 15 pF ×2 and 1 kΩ series on XOUT,
    USB_DP/USB_DM via 27 Ω to J2 D+/D- (both D+ pins and both D- pins of J2),
    RUN 10 kΩ pull-up + RUN button to GND, QSPI_SS 1 kΩ to BOOTSEL button to GND
    (if the RP2354B symbol has no QSPI_SS pin: BOOTSEL via the documented alternative and
    record it in DESIGN.md open items), SWCLK/SWDIO/GND to J3."""
    u = c.add("RP2354B", ref="U1")
    for gpio, net in GPIO_MAP.items():
        c.connect(net, (u, f"GPIO{gpio}"))
    # … blocks above


def translators(c):
    """U2/U3: C/A, fixed A->B. U10..U17: lanes; U11..U17 DNP."""
    for i, ref in enumerate(("U2", "U3")):
        u = c.add("XLAT", ref=ref)
        c.connect("+3V3", (u, "VCCA"), (u, "1DIR"), (u, "2DIR"))
        c.connect("+1V5", (u, "VCCB"))
        c.connect("GND", (u, "GND"), (u, "1OE#"), (u, "2OE#"))
        decouple(c, "+3V3", 2); decouple(c, "+1V5", 2)
        chans = [(h, b) for h in (1, 2) for b in range(1, 9)]
        sigs = CA_SIGNALS[i * 16:(i + 1) * 16]
        for (h, b), s in zip(chans, sigs):
            c.connect(s, (u, f"{h}A{b}"))
            c.connect(CA_DIMM[s], (u, f"{h}B{b}"))
        for h, b in chans[len(sigs):]:                 # unused inputs tied off
            c.connect("GND", (u, f"{h}A{b}"))
            c.no_connect(u, f"{h}B{b}")
    for k in range(8):
        u = c.add("XLAT", ref=f"U1{k}", dnp=(k != 0))
        c.connect("+3V3", (u, "VCCA")); c.connect("+1V5", (u, "VCCB")); c.connect("GND", (u, "GND"))
        c.connect(f"LANE_SEL{k}#", (u, "1OE#"), (u, "2OE#"))
        c.connect("DIR_DQ", (u, "1DIR")); c.connect("DIR_DQS", (u, "2DIR"))
        for b in range(8):
            c.connect(f"DQ{b}", (u, f"1A{b + 1}"))
            c.connect(f"D_L{k}_DQ{b}", (u, f"1B{b + 1}"))
        c.connect("DQS", (u, "2A1")); c.connect(f"D_L{k}_DQS", (u, "2B1"))
        c.connect("DQS#", (u, "2A2")); c.connect(f"D_L{k}_DQS#", (u, "2B2"))
        for b in range(3, 9):
            c.connect("GND", (u, f"2A{b}"))
            c.no_connect(u, f"2B{b}")
        decouple(c, "+3V3", 2); decouple(c, "+1V5", 2)   # fitted with the lane: mark DNP for k != 0
        resistor(c, "R1K", f"LANE_SEL{k}#", f"D_L{k}_DM")
        resistor(c, "R820", f"D_L{k}_DM", "GND")
    d = c.add("DEC138", ref="U4")
    c.connect("LANE0", (d, "A0")); c.connect("LANE1", (d, "A1")); c.connect("LANE2", (d, "A2"))
    c.connect("+3V3", (d, "VCC"), (d, "E3")); c.connect("GND", (d, "GND"), (d, "E1#"), (d, "E2#"))
    for k in range(8):
        c.connect(f"LANE_SEL{k}#", (d, f"Y{k}"))
    decouple(c, "+3V3", 1)


def dimm(c):
    j = c.add("DIMM", ref="J1")
    for pin, sig in UDIMM_PINS.items():
        lane = lane_of(sig)
        net = None
        if sig == "VSS": net = "GND"
        elif sig == "VDD": net = "+1V5"
        elif sig in ("VREFDQ", "VREFCA"): net = "VREF"
        elif sig == "VTT": net = "VTT"
        elif sig == "VDDSPD": net = "+3V3"
        elif sig in ("SCL", "SDA"): net = sig
        elif sig in ("SA0", "SA1", "SA2", "ODT0", "ODT1"): net = "GND"
        elif sig in ("CKE0", "CKE1"): net = "D_CKE"
        elif lane is not None and lane < 8:
            if sig.startswith("DM"): net = f"D_L{lane}_DM"
            elif sig.startswith("DQS"): net = f"D_L{lane}_DQS#" if sig.endswith("#") else f"D_L{lane}_DQS"
            else: net = f"D_L{lane}_DQ{int(sig[2:]) % 8}"
        elif sig in ("S0#", "S1#", "RAS#", "CAS#", "WE#", "RESET#", "CK0", "CK0#") \
                or sig.startswith("BA") or sig in {f"A{i}" for i in range(14)}:
            net = f"D_{sig}"
        if net:
            c.connect(net, (j, str(pin), str(pin)))
    decouple(c, "+1V5", 12)
    decouple(c, "+1V5", 4, key="C10U")


def io(c):
    """PCA9534 U5 at I2C 0x20 (A0-A2 GND). P0 drives D_RESET# (a 1.5 V CMOS input)
    through a 1k/820R divider, with a 10 kΩ pull-down on D_RESET# so the DIMM is held
    in reset at power-up until firmware sets P0 high.
    P1 = VTT_EN (TPS51200 EN, 3.3 V tolerant per datasheet), P2 = status LED (1k).
    I2C pull-ups: R10K ×2 to +3V3 on SDA and SCL.
    J3 SWD: SWCLK, GND, SWDIO (Raspberry Pi 3-pin JST-SH order: 1 SWCLK, 2 GND, 3 SWDIO).
    J4 2×25: pin n (1-48) = GPIO_MAP[n-1]'s net, pin 49 = +3V3, pin 50 = GND.
    J5 2×4: SPI_SCK, SPI_TX (MOSI from ESP32 = RP2354B SPI_RX: label pins from the ESP32's
    point of view — 1 SCK, 2 MOSI=SPI_RX, 3 MISO=SPI_TX, 4 CS=SPI_CS, 5 READY, 6 +3V3, 7 +5V, 8 GND)."""


def build_circuit():
    c = Circuit()
    power(c)
    rp2354b(c)
    translators(c)
    dimm(c)
    io(c)
    return c
```

Fill the `# …` blocks and docstring-described blocks with real `c.add`/`c.connect` calls following those docstrings; each value cited in a docstring is the source to check. Where a DNP lane needs its decoupling caps and divider resistors DNP too, pass `dnp=(k != 0)` through `decouple`/`resistor` (add a `dnp` parameter to both helpers).

- [ ] **Step 4: Run** `.venv/bin/pytest tests/test_circuit.py -v` → PASS. Then `.venv/bin/python -c "from board.circuit import build_circuit; c=build_circuit(); print(len(c.instances), 'parts', len(c.nets), 'nets')"` and note the counts in the commit message.

- [ ] **Step 5: Commit** — `"feat(channel-board): circuit — power, RP2354B, translators, lane switching, DIMM (SINT-101)"`

---

### Task 7: Placement

**Files:**
- Create: `board/placement.py`, `tests/test_placement.py`

**Interfaces:**
- Consumes: `Circuit`, `PartInstance.part.footprint.courtyard`, `UDIMM_PINS`, `lane_of`.
- Produces: `Placement(outline: tuple[float,float,float,float], at: dict[str, tuple[float, float, float]])` — board outline (x0, y0, x1, y1) in mm, KiCad coordinates (y down); `at[ref] = (x, y, rotation_deg)`; `place(circuit) -> Placement`; `bbox(placement, ref) -> (xmin, ymin, xmax, ymax)` of the rotated courtyard.

Layout (outline 175 × 90 mm, origin (100, 100)):
- J1 (DIMM) centred horizontally, 12 mm from the top edge, rotation 0 (pin 1 at left).
- Lane translators U10–U17: each at x = mean x of its lane's DQ pads on J1, y = J1 row + 14 mm.
- C/A translators U2, U3: x = mean x of the J1 pads carrying their signals, y = J1 row + 14 mm, offset ±12 mm if they collide with a lane translator.
- U4 decoder and DM dividers: row y = J1 + 24 mm, under the lanes.
- U1 RP2354B: board centre x, y = J1 + 42 mm.
- U5, U6 near J1's VTT/SCL end; power (J2 USB-C on the left edge, U7, U8, L_BUCK) at the left; J4 (2×25) along the bottom edge; J5, J3 on the right edge.
- Decoupling caps: within 3 mm of the part whose rail they serve, placed in creation order around it.
- Latch shadow: no parts taller than 6.85 mm within 11.5 mm beyond each end of J1's footprint; nothing at all inside J1's 142 × 7.75 mm keep-out.

- [ ] **Step 1: Write the failing tests**

```python
from itertools import combinations
from board.circuit import build_circuit
from board.placement import bbox, place

C = build_circuit()
P = place(C)
EDGE_EXEMPT = {"J1", "J2"}


def test_everything_placed_inside_outline_with_margin():
    x0, y0, x1, y1 = P.outline
    assert abs((x1 - x0) - 175) < 1 and abs((y1 - y0) - 90) < 1
    for ref in C.instances:
        xa, ya, xb, yb = bbox(P, ref)
        m = 0 if ref in EDGE_EXEMPT else 2.5
        assert xa >= x0 + m and ya >= y0 + m and xb <= x1 - m and yb <= y1 - m, ref


def test_no_courtyard_overlaps():
    refs = list(C.instances)
    for a, b in combinations(refs, 2):
        A, B = bbox(P, a), bbox(P, b)
        overlap = A[0] < B[2] and B[0] < A[2] and A[1] < B[3] and B[1] < A[3]
        assert not overlap, (a, b)


def test_lane_translators_sit_under_their_lanes_in_order():
    xs = [P.at[f"U1{k}"][0] for k in range(8)]
    assert xs == sorted(xs) or xs == sorted(xs, reverse=True)


def test_nothing_in_dimm_keepout():
    jx, jy, _ = P.at["J1"]
    keep = (jx - 71, jy - 3.875, jx + 71, jy + 3.875)
    for ref in C.instances:
        if ref == "J1":
            continue
        A = bbox(P, ref)
        assert not (A[0] < keep[2] and keep[0] < A[2] and A[1] < keep[3] and keep[1] < A[3]), ref
```

- [ ] **Step 2: Run** → FAIL.

- [ ] **Step 3: Implement `board/placement.py`** — `bbox` rotates the four courtyard corners by `rot` (KiCad: positive = counter-clockwise on screen, i.e. `x' = x cos θ + y sin θ`, `y' = −x sin θ + y cos θ` with y down) and translates. `place` follows the layout list above; decoupling caps are placed by a helper `ring(around_ref, caps)` that walks positions on a 1.2 mm grid outward from the part's bbox until each cap's bbox is free. Keep a list of occupied boxes and assert no overlap as you go (raise with the ref on failure, so a crowded area fails loudly).

- [ ] **Step 4: Run** `.venv/bin/pytest tests/test_placement.py -v` → PASS.

- [ ] **Step 5: Commit** — `"feat(channel-board): scripted placement (SINT-101)"`

---

### Task 8: Write `.kicad_pcb` and `.kicad_pro`

**Files:**
- Create: `board/kicad_pcb.py`, `tests/test_kicad_pcb.py`

**Interfaces:**
- Consumes: `Circuit`, `Placement`, `sexpr.dumps`.
- Produces: `write_board(circuit, placement, path: Path, tracks=(), vias=())` writes `channel.kicad_pcb` + `channel.kicad_pro` beside it. `Track(net, layer, x1, y1, x2, y2, width)`, `Via(net, x, y)` dataclasses (defined here, used by Task 9).

Content:
- Header `(kicad_pcb (version 20240108) (generator "sint101_channel_board") (generator_version "8.0")`, `(general (thickness 1.6))`, 4 copper layers `(0 "F.Cu" signal) (1 "In1.Cu" power) (2 "In2.Cu" power) (31 "B.Cu" signal)` plus the user layers list used by `../fixture/pcb/gen_pcb.py`.
- `(net 0 "")` then one `(net i "NAME")` per circuit net, sorted.
- `gr_rect` on Edge.Cuts for the outline.
- Each footprint: deep-copy `inst.part.footprint.node`, set `(at x y rot)` (insert after `layer`), set Reference property text to the ref, add `(attr … dnp)` for DNP parts (`(attr smd dnp)` / `(attr through_hole dnp)`), add `(net i "NAME")` to every pad whose `(ref, number)` has a net, add `(uuid …)` where missing.
- Zones: GND on In1.Cu and +1V5 on In2.Cu covering the outline inset 0.5 mm, `(connect_pads (clearance 0.2))`, `(min_thickness 0.2)`, `(fill yes (thermal_gap 0.3) (thermal_bridge_width 0.4))`.
- Tracks `(segment (start) (end) (width) (layer) (net) (uuid))`, vias `(via (at) (size 0.6) (drill 0.3) (layers "F.Cu" "B.Cu") (net) (uuid))`.
- `.kicad_pro` JSON: `{"meta": {"filename": "channel.kicad_pro", "version": 1}, "board": {"design_settings": {"rules": {"min_clearance": 0.15, "min_track_width": 0.15, "min_via_diameter": 0.5, "min_through_hole_diameter": 0.3, "min_copper_edge_clearance": 0.3}}}, "net_settings": {"classes": [{"name": "Default", "clearance": 0.15, "track_width": 0.15, "via_diameter": 0.6, "via_drill": 0.3}, {"name": "Power", "clearance": 0.2, "track_width": 0.5, "via_diameter": 0.6, "via_drill": 0.3}], "netclass_patterns": [{"netclass": "Power", "pattern": "+*"}, {"netclass": "Power", "pattern": "GND"}, {"netclass": "Power", "pattern": "VTT"}]}}`.

- [ ] **Step 1: Write the failing tests**

```python
import json
import subprocess
from conftest import KICAD_CLI
from board.circuit import build_circuit
from board.kicad_pcb import write_board
from board.placement import place


def test_kicad_parses_unrouted_board(tmp_path):
    c = build_circuit()
    write_board(c, place(c), tmp_path / "channel.kicad_pcb")
    report = tmp_path / "drc.json"
    r = subprocess.run([KICAD_CLI, "pcb", "drc", "--format", "json", "--severity-all",
                        "-o", str(report), str(tmp_path / "channel.kicad_pcb")],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    drc = json.loads(report.read_text())
    assert drc["unconnected_items"], "an unrouted board must report unconnected items"
    bad = [v for v in drc["violations"] if v["type"] in ("courtyards_overlap", "malformed_courtyard",
                                                         "invalid_outline", "lib_footprint_issues")]
    assert bad == []


def test_dnp_flag_written(tmp_path):
    c = build_circuit()
    write_board(c, place(c), tmp_path / "channel.kicad_pcb")
    text = (tmp_path / "channel.kicad_pcb").read_text()
    assert text.count(" dnp)") == sum(1 for i in c.instances.values() if i.dnp)
```

- [ ] **Step 2: Run** → FAIL. **Step 3: Implement** as described. **Step 4: Run** → PASS (kicad-cli takes a few seconds). If kicad-cli reports a parse error, open the board in KiCad 10's PCB Editor to see the line, fix the writer, rerun.

- [ ] **Step 5: Commit** — `"feat(channel-board): kicad_pcb and kicad_pro writer (SINT-101)"`

---

### Task 9: Route with Freerouting

**Files:**
- Create: `board/dsn.py`, `board/ses.py`, `tests/test_route.py`, `tests/data/mini.ses`

**Interfaces:**
- Consumes: `Circuit`, `Placement`, `Track`, `Via`, `write_board`.
- Produces: `write_dsn(circuit, placement, path)`; `read_ses(path) -> tuple[list[Track], list[Via]]`; `route(circuit, placement, workdir: Path, passes=100) -> (tracks, vias)` (writes DSN, runs Freerouting, reads SES).

- [ ] **Step 1: Get Freerouting**

```bash
mkdir -p freerouting
gh release download v2.5.0 --repo freerouting/freerouting --pattern 'freerouting-2.5.0.jar' --dir freerouting
java -jar freerouting/freerouting-2.5.0.jar --help > /tmp/channel-board-freerouting-help.log 2>&1; head -40 /tmp/channel-board-freerouting-help.log
```
Confirm the headless flags in that help output: input DSN (`-de`), output SES (`-do`), max passes (`-mp`), and the flag that disables the GUI (2.x: `--gui.enabled=false`). Use exactly what the help prints.

- [ ] **Step 2: DSN writer (`board/dsn.py`).** Units `(resolution um 10)` `(unit um)`; KiCad mm → DSN µm with **y negated**. Structure: `(layer F.Cu (type signal))`, `(layer In1.Cu (type power))`, `(layer In2.Cu (type power))`, `(layer B.Cu (type signal))`; `(boundary (path pcb 0 x0 y0 x1 y0 x1 y1 x0 y1 x0 y0))`; `(via "Via[0-3]_600:300_um")`; `(rule (width 150) (clearance 150))`; `(plane GND (polygon In1.Cu 0 …outline…))`, `(plane +1V5 (polygon In2.Cu 0 …))`. Placement: one `(component <image> (place <ref> x y front <rot>))` per instance. Library: one `(image <ref>_img (pin <padstack> <padnumber> x y))` per instance (pad coords in footprint frame, y negated, pad rotation folded into padstack shape), padstacks: SMD rect → `(shape (rect F.Cu -w/2 -h/2 w/2 h/2))`; circle → `(shape (circle <layer> d))`; oval → `(shape (path <layer> min(w,h) …))`; THT → shapes on F.Cu and B.Cu; via padstack `Via[0-3]_600:300_um` with circles 600 on F.Cu and B.Cu. Network: `(net <name> (pins <ref>-<pad> …))` for every net; classes `(class default … (rule (width 150) (clearance 150)))` and `(class power +5V +3V3 +1V5 +1V1 VTT GND (rule (width 500) (clearance 200)))`. Wiring empty. Quote names containing `#`, `+` or `-` with `"`. Set `(parser (string_quote ") (space_in_quoted_tokens on))`.

- [ ] **Step 3: SES reader (`board/ses.py`).** Parse with `board.sexpr.parse`; read `(resolution um N)` from `routes`; for each `(net NAME (wire (path LAYER WIDTH x1 y1 x2 y2 …)) (via PADSTACK x y))` emit `Track`s between consecutive points and `Via`s, converting back: mm = value / N / 1000, y negated.

`tests/data/mini.ses`:
```
(session mini (routes (resolution um 10) (parser (string_quote ") (space_in_quoted_tokens on))
  (network_out (net GND
    (wire (path F.Cu 1500 1000000 -1000000 1100000 -1000000 1100000 -1050000))
    (via "Via[0-3]_600:300_um" 1100000 -1050000)))))
```

- [ ] **Step 4: Write the tests**

```python
import json
import subprocess
from pathlib import Path
from conftest import KICAD_CLI
from board.ses import read_ses

DATA = Path(__file__).parent / "data"


def test_read_ses_converts_units_and_flips_y():
    tracks, vias = read_ses(DATA / "mini.ses")
    assert len(tracks) == 2 and len(vias) == 1
    t = tracks[0]
    assert (t.net, t.layer, t.width) == ("GND", "F.Cu", 0.15)
    assert (t.x1, t.y1, t.x2, t.y2) == (100.0, 100.0, 110.0, 100.0)
    assert (vias[0].x, vias[0].y) == (110.0, 105.0)


def test_full_route_is_drc_clean():
    """Integration: run build.py --route and check its DRC output (slow, ~minutes)."""
    out = Path(__file__).resolve().parent.parent / "out"
    subprocess.run(["./.venv/bin/python", "build.py", "--route"], check=True,
                   cwd=out.parent)
    drc = json.loads((out / "drc.json").read_text())
    assert drc["unconnected_items"] == []
    assert [v for v in drc["violations"] if v["severity"] == "error"] == []
```

- [ ] **Step 5: `build.py`** (first version; Task 10 adds fab):

```python
#!/usr/bin/env python3
"""Channel board: circuit -> placement -> (route) -> board + DRC."""
import argparse
import subprocess
from pathlib import Path

from board.circuit import build_circuit
from board.dsn import write_dsn
from board.kicad_pcb import write_board
from board.netlist import check
from board.placement import place
from board.ses import read_ses

HERE = Path(__file__).resolve().parent
OUT = HERE / "out"
TMP = OUT / "tmp"
KICAD_CLI = "/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli"
FREEROUTING = HERE / "freerouting" / "freerouting-2.5.0.jar"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--route", action="store_true")
    ap.add_argument("--passes", type=int, default=100)
    a = ap.parse_args()
    TMP.mkdir(parents=True, exist_ok=True)
    c = build_circuit()
    problems = check(c)
    if problems:
        raise SystemExit("\n".join(problems))
    p = place(c)
    tracks, vias = (), ()
    if a.route:
        write_dsn(c, p, TMP / "channel.dsn")
        with open("/tmp/channel-board-route.log", "w") as log:
            subprocess.run(["java", "-jar", str(FREEROUTING), "-de", str(TMP / "channel.dsn"),
                            "-do", str(TMP / "channel.ses"), "-mp", str(a.passes),
                            "--gui.enabled=false"], check=True, stdout=log, stderr=subprocess.STDOUT)
        tracks, vias = read_ses(TMP / "channel.ses")
    board = OUT / "channel.kicad_pcb"
    write_board(c, p, board, tracks, vias)
    subprocess.run([KICAD_CLI, "pcb", "drc", "--refill-zones", "--save-board", "--format", "json",
                    "--severity-all", "-o", str(OUT / "drc.json"), str(board)], check=True)
    print(f"wrote {board}")


if __name__ == "__main__":
    main()
```
(Adjust the Freerouting flags to what Step 1's help printed.)

- [ ] **Step 6: Run** `.venv/bin/pytest tests/test_route.py -v -k read_ses` → PASS. Then the routing run — tell Martin: `less +F /tmp/channel-board-route.log`:
`.venv/bin/pytest tests/test_route.py -v -k full_route` → PASS.
If unconnected items remain: raise `--passes` to 300; if still failing, widen the board by 10 mm in `placement.py` (update DESIGN.md's size) or move the crowded part (the DRC JSON names the items) — then rerun. Do not relax design rules.

- [ ] **Step 7: Commit** — `git add board/dsn.py board/ses.py build.py tests/test_route.py tests/data && git commit -m "feat(channel-board): Freerouting via DSN/SES, DRC-clean routed board (SINT-101)"`

---

### Task 10: Fabrication outputs for JLCPCB

**Files:**
- Create: `board/fab.py`, `tests/test_fab.py`; Modify: `build.py` (add `--fab`), `board/parts.py` (rotation offsets)

**Interfaces:**
- Consumes: `Circuit`, `Placement`, `PartDef.rotation_offset`, `PartDef.lcsc`.
- Produces: `write_bom(circuit, path)` (columns `Comment,Designator,Footprint,LCSC Part #`, one row per (value, LCSC) group, designators comma-joined, DNP excluded); `write_cpl(circuit, placement, path)` (columns `Designator,Mid X,Mid Y,Layer,Rotation`, mm, `Mid Y` positive-up relative to the outline's bottom-left corner, `Layer` = `Top`, `Rotation` = (placement rotation + `rotation_offset`) mod 360, DNP excluded); `fab(board_path, out_dir)` runs kicad-cli gerbers (`F.Cu,In1.Cu,In2.Cu,B.Cu,F.Mask,B.Mask,F.Silkscreen,B.Silkscreen,Edge.Cuts`, `--subtract-soldermask --no-x2`) + drill (`--format excellon --excellon-separate-th`) into `out/gerbers/` and zips them to `out/channel-gerbers.zip`; renders `out/render-top.png` and `out/render-bottom.png` via `kicad-cli pcb render --side top|bottom`.

- [ ] **Step 1: Write the failing tests**

```python
import csv
from board.circuit import build_circuit
from board.fab import write_bom, write_cpl
from board.parts import PARTS
from board.placement import place

C = build_circuit()
P = place(C)


def test_bom_groups_and_excludes_dnp(tmp_path):
    write_bom(C, tmp_path / "bom.csv")
    rows = list(csv.DictReader(open(tmp_path / "bom.csv")))
    assert set(rows[0]) == {"Comment", "Designator", "Footprint", "LCSC Part #"}
    refs = [r for row in rows for r in row["Designator"].split(",")]
    assert "U11" not in refs and "U10" in refs and "J1" in refs
    assert len(refs) == len(set(refs))
    assert all(row["LCSC Part #"].startswith("C") for row in rows)


def test_cpl_rotation_includes_offset(tmp_path):
    write_cpl(C, P, tmp_path / "cpl.csv")
    rows = {r["Designator"]: r for r in csv.DictReader(open(tmp_path / "cpl.csv"))}
    for ref, inst in C.instances.items():
        if inst.dnp:
            assert ref not in rows
            continue
        want = (P.at[ref][2] + inst.part.rotation_offset) % 360
        assert abs(float(rows[ref]["Rotation"]) - want) < 0.01, ref
        assert rows[ref]["Layer"] == "Top"


def test_every_fitted_extended_part_has_a_checked_offset():
    from board.parts import ROTATION_CHECKED
    for key, p in PARTS.items():
        if p.extended:
            assert key in ROTATION_CHECKED, key
```

- [ ] **Step 2: Run** → FAIL. **Step 3: Implement `board/fab.py`.**

- [ ] **Step 4: Rotation offsets.** In `board/parts.py` add `ROTATION_CHECKED: set[str]`. For each extended part, compare our footprint's pin-1 orientation at rotation 0 with JLCPCB's convention (the part page's EasyEDA footprint, which is what easyeda2kicad fetched — so these are normally 0; KiCad-stdlib footprints, if any, often need 90/180/270). Record the offset in `_TABLE` and add the key to `ROTATION_CHECKED`. The final word is Martin's look at JLCPCB's placement preview at order time (Task 11).

- [ ] **Step 5: Wire `--fab` into `build.py`** (calls `write_bom`, `write_cpl`, `fab`), run `.venv/bin/python build.py --route --fab > /tmp/channel-board-build.log 2>&1` (tell Martin the log path), then `.venv/bin/pytest -v` → all PASS.

- [ ] **Step 6: Commit** — `git add -A board build.py tests out && git commit -m "feat(channel-board): JLCPCB gerbers, BOM, CPL and renders (SINT-101)"`

---

### Task 11: Review package and PR

**Files:**
- Create: `channel-board/README.md`; Modify: `embedded/ddr-banging/README.md` (add `channel-board/` row), `channel-board/DESIGN.md` (resolve open items that the build answered)

- [ ] **Step 1: README** — what the board is, `build.py` usage (`--route`, `--fab`), outputs, log paths, how to reorder (upload `out/channel-gerbers.zip`, `out/bom.csv`, `out/cpl.csv`; 4 layers, 1.6 mm, Economic PCBA if offered for 4-layer else Standard; assemble 2 of 5).

- [ ] **Step 2: Cost estimate** — `bom.csv` × current LCSC prices (from Task 3's table) + assembly fees from `https://jlcpcb.com/help/article/pcb-assembly-price` (joint counts from the board: SMT pads, THT pads) → a short table in the README marked "estimate; JLCPCB's quote is final".

- [ ] **Step 3: Open items** — for each DESIGN.md open item, write what the build settled (e.g. BOOTSEL pin, socket numbering) or leave it open with why.

- [ ] **Step 4: Full test run** — `.venv/bin/pytest -v > /tmp/channel-board-tests.log 2>&1; tail -20 /tmp/channel-board-tests.log` → all PASS.

- [ ] **Step 5: Commit and PR**

```bash
git add -A embedded/ddr-banging
git commit -m "docs(channel-board): README, cost estimate, resolved open items (SINT-101)"
git push -u origin sint-100-ddr-banging
gh pr create --title "feat(ddr-banging): channel board — DDR3 socket + RP2354B (SINT-101)" \
  --body "Generated channel board per embedded/ddr-banging/channel-board/DESIGN.md. Renders in out/. Review before ordering: layout in KiCad, then JLCPCB placement preview (RP2354B, translators, USB-C)."
```

- [ ] **Step 6: Human checkpoint** — Martin opens `out/channel.kicad_pcb` in KiCad 10 and the renders, then (his decision) uploads to JLCPCB, checks the placement preview and orders. Comment the PR link on SINT-101 in Plane.
