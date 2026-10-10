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
