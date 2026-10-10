#!/usr/bin/env python3
"""Board generator: circuit -> placement -> (route) -> board + DRC.

    ./.venv/bin/python build.py --board xlat-test [--route] [--passes N]

Output goes to out/<board>/. Only the translator test board exists so far; the channel
board gets added to BOARDS when its circuit is ready."""
import argparse
import json
import subprocess
from pathlib import Path

from board.kicad_pcb import write_board
from board.netlist import check
from board.route import route
from board.xlat_test import build_xlat_test, art_xlat, place_xlat_test, silk_xlat

HERE = Path(__file__).resolve().parent
KICAD_CLI = "/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli"


# name -> (build circuit, place, layers, silk function, art function, board file stem)
BOARDS = {
    "xlat-test": (build_xlat_test, place_xlat_test, 2, silk_xlat, art_xlat, "xlat-test"),
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--board", choices=sorted(BOARDS), required=True)
    ap.add_argument("--route", action="store_true")
    ap.add_argument("--passes", type=int, default=100)
    a = ap.parse_args()
    build, place, layers, silk, art, stem = BOARDS[a.board]
    out = HERE / "out" / a.board
    tmp = out / "tmp"
    tmp.mkdir(parents=True, exist_ok=True)
    c = build()
    problems = check(c)
    if problems:
        raise SystemExit("\n".join(problems))
    p = place(c)
    tracks, vias = (), ()
    if a.route:
        tracks, vias = route(c, p, tmp, passes=a.passes, layers=layers)
    board = out / f"{stem}.kicad_pcb"
    write_board(c, p, board, tracks, vias, layers=layers, silk=silk(c, p), art=art(p),
                hide_refs=set(c.instances))   # the silk labels replace the reference texts
    subprocess.run([KICAD_CLI, "pcb", "drc", "--refill-zones", "--save-board", "--format", "json",
                    "--severity-all", "-o", str(out / "drc.json"), str(board)], check=True)
    drc = json.loads((out / "drc.json").read_text())
    bad = [f"unconnected: {' / '.join(i['description'] for i in u['items'])}" for u in drc["unconnected_items"]]
    bad += [f"{v['type']}: {v['description']}" for v in drc["violations"] if v["severity"] == "error"]
    warn = len([v for v in drc["violations"] if v["severity"] != "error"])
    print(f"wrote {board} ({warn} DRC warnings)")
    if bad:      # kicad-cli exits 0 even when DRC fails
        raise SystemExit("DRC failed:\n" + "\n".join(bad))


if __name__ == "__main__":
    main()
