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
