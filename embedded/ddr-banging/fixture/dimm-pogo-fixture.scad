// DDR3 240-pin UDIMM pogo-pin fixture (SINT-100)
//
// Two jaws clamp the bottom edge of a desktop DDR3 DIMM. P75 pogo pins in
// staggered rows press on the gold pads. The front jaw carries a floor the
// DIMM stands on, with a key rib that drops into the notch; the back jaw
// bolts to the floor's end face with M3 screws that pass beneath the DIMM.
// A PCB on each jaw's outer face holds the pin tails (soldered) and breaks
// them out to headers; see pcb/gen_pcb.py. The same bolts clamp the PCBs.
//
// Geometry from the Micron MT16JTF 240-pin DDR3 UDIMM drawing (JEDEC MO-269):
//   board 133.35 long, 1.27 thick; pad pitch 1.0, pad width 0.80;
//   pin 1 at 5.18 from the left edge; key notch centre 54.68 from the left
//   edge; pins 48 -> 49 are 5.0 apart across the notch; pin 1 to pin 120 is
//   123.0. Back pin (120 + n) sits directly behind front pin n.
//
// Coordinates: X along the DIMM from its pin-1 end, Y through the board
// (front face at -board_t/2), Z up from the DIMM's bottom edge.
//
// Print on the Mars 5 Ultra: tough/ABS-like resin, pin holes along Z (lay
// each jaw on its back face). Print the hole test piece first and set
// hole_d from it.
//
// Render a part:  openscad -D 'part="front"' -o front.stl dimm-pogo-fixture.scad

part = "assembly";   // assembly | front | back | test | gauge

// --- Things to measure / tune ---------------------------------------------
hole_d          = 1.07;  // press fit for the P75 barrel (test piece, 2026-10-23: 1.07)
plunger_exposed = 5.5;   // P75-B1 tip to barrel front, at rest (MEASURE on arrival)
stroke          = 2.65;  // P75 full travel
compression     = 1.3;   // working compression on the pad (~half stroke)
row_z           = [0.75, 2.03];  // pin rows above the bottom edge (odd, even pins); 1.28 apart is the PCB-pad minimum
pad_top         = 2.4;   // top of the gold pads above the edge (measured on the Samsung M391B5673FH0)
clear_above     = 4.4;   // jaw face height before it steps back to clear SMD parts
holes           = "needed";  // "needed" (our signals only) or "all" 240

// --- DIMM -----------------------------------------------------------------
board_len  = 133.35;
board_t    = 1.27;
pitch      = 1.0;
pin1_x     = 5.18;
notch_x    = 54.68;
notch_gap  = 4.0;    // extra spacing after pin 48
rib_w      = 1.2;    // notch is ~1.5 wide (0.76 R)
rib_h      = 2.0;
end_clr    = 0.15;   // board length is 133.20-133.50
stop_w     = 2.5;    // end-stop thickness along X
stop_h     = 6;      // end-stop height above the floor

// --- Fixture --------------------------------------------------------------
c       = 0.6;       // air gap between DIMM face and jaw face
W       = 8;         // jaw thickness (pin barrel bore length)
floor_h = 7;         // floor depth below the DIMM edge
jaw_top = 7;         // overall jaw height above the edge (stepped back above clear_above)
step_back = 2.5;     // how far the jaw steps back above clear_above
x0      = -(end_clr + stop_w + 0.5);        // jaw extends this far beyond each DIMM end
jaw_len = board_len - 2 * x0;
bolt_x  = [25, 42, 102, 118];  // in the gaps between pin clusters, clear of PCB tracks
bolt_z  = -floor_h / 2;
m3      = 3.4;
nut_af  = 5.7;
nut_h   = 2.6;
$fn     = 24;

// Pins the project needs (rank 0, byte lane 0, SPD, power). See ../pogo-pins.md.
front_needed = [
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11,       // VREFDQ, DQ0-3, DQS0/#, VSS
    50, 51, 52, 54, 55, 56, 57, 58, 59, 60,  // CKE0, BA2, A11, A7, A5, A4, VDD
    61, 62, 65, 66, 67, 70, 71, 73, 74,      // A2, VREFCA, A10, BA0, WE#, CAS#, VDD
    76, 77, 80, 83,                          // S1#, ODT1 (tied off), VSS
    117, 118, 119, 120                       // SA0, SCL, SA2, VTT
];
back_needed = [
    121, 122, 123, 124, 125, 127, 128, 129, 130,  // DQ4-7, DM0, VSS
    168, 169, 170, 173, 174, 175, 176, 177, 178,  // RESET#, CKE1, A12, A9, A8, A6, VDD
    179, 180, 181, 182, 183, 184, 185, 186,       // A3, A1, CK0/#, VDD
    188, 190, 192, 193, 195, 196, 199, 202,       // A0, BA1, RAS#, S0#, ODT0, A13, VSS
    236, 237, 238, 240                            // VDDSPD, SA1, SDA, VTT
];

function pad_x(n) = let(i = n > 120 ? n - 120 : n)
    pin1_x + (i - 1) * pitch + (i >= 49 ? notch_gap : 0);
function pad_z(n) = let(i = n > 120 ? n - 120 : n) row_z[(i - 1) % 2];
function front_pins() = holes == "all" ? [for (i = [1:120]) i] : front_needed;
function back_pins()  = holes == "all" ? [for (i = [121:240]) i] : back_needed;

// Inner faces
yf = -board_t / 2 - c;   // front jaw inner face
yb =  board_t / 2 + c;   // back jaw inner face

// Pin holes run along Y through the jaw thickness.
module pin_hole(n, y_face, dir) {
    translate([pad_x(n), y_face, pad_z(n)])
        rotate([dir > 0 ? -90 : 90, 0, 0])
            cylinder(d = hole_d, h = W + 2, center = false);
}

module jaw_body(y_face, dir) {
    // dir = -1 for the front jaw (extends towards -Y), +1 for the back
    y_out = y_face + dir * W;
    hull_y0 = min(y_face, y_out);
    difference() {
        union() {
            // lower part, full thickness up to clear_above
            translate([x0, hull_y0, -floor_h]) cube([jaw_len, W, floor_h + clear_above]);
            // upper part, stepped back away from the DIMM
            translate([x0, dir < 0 ? hull_y0 : y_face + step_back, clear_above])
                cube([jaw_len, W - step_back, jaw_top - clear_above]);
        }
        // pad-row markers: a shallow V on the top at every 10th pin, and the notch
        for (i = [1:120]) if (i % 10 == 0 || i == 1 || i == 48 || i == 49)
            translate([pad_x(i), dir < 0 ? hull_y0 + (W - step_back) / 2 : y_face + step_back + (W - step_back) / 2, jaw_top])
                rotate([0, 45, 0]) cube([0.6, W, 0.6], center = true);
    }
}

module front_jaw() {
    difference() {
        union() {
            jaw_body(yf, -1);
            // floor under the DIMM, out to the back jaw's face
            translate([x0, yf, -floor_h]) cube([jaw_len, yb - yf, floor_h]);
            // key rib into the notch, rounded on top so the stick drops onto it
            translate([notch_x, yf, 0]) hull() {
                translate([-rib_w / 2, 0, 0]) cube([rib_w, yb - yf, 0.01]);
                translate([0, 0, rib_h - rib_w / 2]) rotate([-90, 0, 0]) cylinder(d = rib_w, h = yb - yf);
            }
            // end stops: mark where the stick goes and stop it sliding
            for (x = [-end_clr - stop_w, board_len + end_clr])
                translate([x, yf, 0]) cube([stop_w, yb - yf, stop_h]);
        }
        for (n = front_pins()) pin_hole(n, yf + 1, -1);
        // M3 through the floor and jaw; the bolts also clamp both PCBs (M3x25, nut on the front PCB)
        for (x = bolt_x)
            translate([x, yb + 1, bolt_z]) rotate([90, 0, 0]) cylinder(d = m3, h = W + (yb - yf) + 2);
        label("FRONT  pins 1-120", yf - W);
    }
}

module back_jaw() {
    difference() {
        jaw_body(yb, 1);
        for (n = back_pins()) pin_hole(n, yb - 1, 1);
        for (x = bolt_x)
            translate([x, yb - 1, bolt_z]) rotate([-90, 0, 0]) cylinder(d = m3, h = W + 2);
        label("BACK  pins 121-240", yb + W);
    }
}

module label(t, y) {
    translate([board_len / 2, y + (y < 0 ? -0.01 : 0.01), -floor_h / 2 - 2.6])
        rotate([90, 0, y < 0 ? 0 : 180])
            linear_extrude(0.8, center = true)
                text(t, size = 2.2, halign = "center", font = "Liberation Sans:style=Bold");
}

// Hole-size test strip: 0.95 .. 1.10 in 0.01 steps, labelled every 0.05
module test_piece() {
    sizes = [for (i = [0:15]) 0.95 + i * 0.01];
    difference() {
        cube([len(sizes) * 2.5 + 3, 9, W]);
        for (k = [0:len(sizes) - 1]) {
            translate([2.75 + k * 2.5, 6, -1]) cylinder(d = sizes[k], h = W + 2);
            if (k % 5 == 0)
                translate([2.75 + k * 2.5, 1.2, W - 0.6]) linear_extrude(1)
                    text(str(round(sizes[k] * 100)), size = 1.8, halign = "center");
        }
    }
}

// Insertion gauge: a strip as thick as the tip should stand proud of the jaw face.
// Lay it on the inner face beside a pin and press the pin in until its tip is flush.
module gauge() {
    p = compression + c;  // tip protrusion at rest
    difference() {
        cube([40, 8, p]);
        translate([20, 4, p - 0.4]) linear_extrude(1)
            text(str("tip ", p, " mm"), size = 2.5, halign = "center", valign = "center");
    }
}

module dimm_ghost() {
    %difference() {
        translate([0, -board_t / 2, 0]) cube([board_len, board_t, 30.5]);
        translate([notch_x - 0.8, -board_t, -0.1]) cube([1.6, 2 * board_t, 3]);
    }
}

if (part == "front")    front_jaw();
if (part == "back")     back_jaw();
if (part == "test")     test_piece();
if (part == "gauge")    gauge();
if (part == "assembly") { front_jaw(); color("SteelBlue") back_jaw(); dimm_ghost(); }

// Sanity checks
web = sqrt(pitch * pitch + pow(row_z[1] - row_z[0], 2)) - hole_d;
echo(str("Diagonal web between staggered holes: ", web, " mm; same-row web: ", 2 * pitch - hole_d, " mm"));
echo(str("Pins: front ", len(front_pins()), ", back ", len(back_pins())));
assert(row_z[1] + 0.3 <= pad_top, "Upper pin row lands above the pads - lower row_z or measure pad_top");
assert(web >= 0.5, "Staggered-hole web too thin for resin");

// Cross-section through pins 3 and 4 (preview only)
if (part == "section")
    intersection() {
        union() { front_jaw(); back_jaw(); }
        translate([pad_x(3) - 0.2, -30, -30]) cube([pad_x(4) - pad_x(3) + 0.4, 60, 60]);
    }
