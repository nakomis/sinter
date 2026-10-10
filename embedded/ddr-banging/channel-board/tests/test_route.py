import json
import subprocess
from pathlib import Path

from board.dsn import write_dsn
from board.ses import read_ses
from board.xlat_test import build_xlat_test, place_xlat_test

DATA = Path(__file__).parent / "data"
ROOT = Path(__file__).resolve().parent.parent


def test_read_ses_converts_units_and_flips_y():
    tracks, vias = read_ses(DATA / "mini.ses")
    assert len(tracks) == 2 and len(vias) == 1
    t = tracks[0]
    assert (t.net, t.layer, t.width) == ("GND", "F.Cu", 0.15)
    assert (t.x1, t.y1, t.x2, t.y2) == (100.0, 100.0, 110.0, 100.0)
    assert (vias[0].x, vias[0].y) == (110.0, 105.0)


def test_dsn_two_layer_has_no_planes(tmp_path):
    c = build_xlat_test()
    write_dsn(c, place_xlat_test(c), tmp_path / "a.dsn", layers=2)
    text = (tmp_path / "a.dsn").read_text()
    assert "(plane" not in text and "In1.Cu" not in text
    assert "(layer F.Cu (type signal))" in text and "(layer B.Cu (type signal))" in text
    assert text.count("(net ") >= len(c.nets)
    assert "(class gnd GND" in text and "(clearance 210)" in text
    assert "(class power GND" not in text


def test_dsn_four_layer_has_planes(tmp_path):
    c = build_xlat_test()
    write_dsn(c, place_xlat_test(c), tmp_path / "a.dsn", layers=4)
    text = (tmp_path / "a.dsn").read_text()
    assert "(plane GND (polygon In1.Cu" in text and "Via[0-3]_600:300_um" in text


def test_full_route_is_drc_clean():
    """Integration: run build.py --board xlat-test --route and check its DRC output (slow)."""
    subprocess.run(["./.venv/bin/python", "build.py", "--board", "xlat-test", "--route"],
                   check=True, cwd=ROOT)
    drc = json.loads((ROOT / "out" / "xlat-test" / "drc.json").read_text())
    assert drc["unconnected_items"] == []
    assert [v for v in drc["violations"] if v["severity"] == "error"] == []
