#!/usr/bin/env python3
"""Board generator: circuit -> placement -> (route) -> board + DRC.

    ./.venv/bin/python build.py --board xlat-test [--route] [--passes N]

Output goes to out/<board>/. Only the translator test board exists so far; the channel
board gets added to BOARDS when its circuit is ready."""
import argparse
import subprocess
from pathlib import Path

from board.kicad_pcb import pin1_marks, write_board
from board.netlist import check
from board.route import route
from board.xlat_test import SILK_PIN1, SILK_TEXT, build_xlat_test, place_xlat_test

HERE = Path(__file__).resolve().parent
KICAD_CLI = "/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli"


def _xlat_silk(c, pl):
    x0, y0, x1, y1 = pl.outline
    texts = [(t, (x0 + x1) / 2, y0 + 1.3 + 1.2 * i, 1.0) for i, t in enumerate(SILK_TEXT)]
    return texts + list(pin1_marks(c, pl, SILK_PIN1))


# name -> (build circuit, place, layers, silk function, board file stem)
BOARDS = {
    "xlat-test": (build_xlat_test, place_xlat_test, 2, _xlat_silk, "xlat-test"),
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--board", choices=sorted(BOARDS), required=True)
    ap.add_argument("--route", action="store_true")
    ap.add_argument("--passes", type=int, default=100)
    a = ap.parse_args()
    build, place, layers, silk, stem = BOARDS[a.board]
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
    write_board(c, p, board, tracks, vias, layers=layers, silk=silk(c, p))
    subprocess.run([KICAD_CLI, "pcb", "drc", "--refill-zones", "--save-board", "--format", "json",
                    "--severity-all", "-o", str(out / "drc.json"), str(board)], check=True)
    print(f"wrote {board}")


if __name__ == "__main__":
    main()
