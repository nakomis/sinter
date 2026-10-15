"""Run Freerouting headless on a circuit + placement."""
import subprocess
from pathlib import Path

from board.dsn import write_dsn
from board.ses import read_ses

MIN_TRACK = 0.15
FREEROUTING = Path(__file__).resolve().parent.parent / "freerouting" / "freerouting-2.5.0.jar"


def route(circuit, placement, workdir, passes=100, layers=4, log="/tmp/channel-board-route.log"):
    """Write workdir/board.dsn, run Freerouting (-de/-do/-mp from its --help; the full output
    goes to 'log'), and return the (tracks, vias) read from workdir/board.ses."""
    workdir = Path(workdir)
    workdir.mkdir(parents=True, exist_ok=True)
    dsn, ses = workdir / "board.dsn", workdir / "board.ses"
    ses.unlink(missing_ok=True)
    write_dsn(circuit, placement, dsn, layers=layers)
    with open(log, "w") as f:
        subprocess.run(["java", "-Djava.awt.headless=true", "-jar", str(FREEROUTING),
                        "-de", str(dsn), "-do", str(ses), "-mp", str(passes)],
                       check=True, stdout=f, stderr=subprocess.STDOUT)
    if not ses.exists():
        raise RuntimeError(f"Freerouting wrote no session file; see {log}")
    report = {}
    tracks, vias = read_ses(ses, min_width=MIN_TRACK, report=report)
    print(f"routed: {len(tracks)} segments, {len(vias)} vias; widened {report['widened']} narrowed "
          f"segments ({report['widened_mm']:.1f} mm) to {MIN_TRACK} mm")
    return tracks, vias
