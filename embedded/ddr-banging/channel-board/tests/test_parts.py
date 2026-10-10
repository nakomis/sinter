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
    assert "QSPI_SS" in p.pins
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
    assert part("HDR_2X25").tht and not part("RP2354B").tht
    assert not part("VTTREG").tht and not part("USBC").tht


def test_dimm_is_tht():
    assert part("DIMM").tht
