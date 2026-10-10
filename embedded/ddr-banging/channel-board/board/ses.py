"""Read a Specctra SES (Freerouting's output) back into Tracks and Vias."""
from pathlib import Path

from board.kicad_pcb import Track, Via
from board.sexpr import find, find_all, parse


def read_ses(path, min_width=0.0):
    """Tracks and vias from the SES. Freerouting narrows the few-micron escape stubs at fine-pitch
    pads below the class width; 'min_width' lifts any such track to the design-rule minimum (DRC
    then confirms that clearances still hold)."""
    # '(string_quote ")' would open a string in our reader; the clause is not needed here.
    tree = parse(Path(path).read_text().replace('(string_quote ")', "(string_quote)"))
    routes = find(tree, "routes")
    res = find(routes, "resolution")
    scale = res[2] * 1000.0 if res[1] == "um" else None      # SES units per mm
    if scale is None:
        raise ValueError(f"unsupported SES unit {res[1]}")
    tracks, vias = [], []
    for net in find_all(find(routes, "network_out"), "net"):
        name = str(net[1])
        for wire in find_all(net, "wire"):
            for p in find_all(wire, "path"):
                layer, width, pts = str(p[1]), p[2] / scale, p[3:]
                xy = [(pts[i] / scale, -pts[i + 1] / scale) for i in range(0, len(pts), 2)]
                for (ax, ay), (bx, by) in zip(xy, xy[1:]):
                    tracks.append(Track(name, layer, ax, ay, bx, by, round(max(width, min_width), 4)))
        for v in find_all(net, "via"):
            vias.append(Via(name, v[2] / scale, -v[3] / scale))
    return tracks, vias
