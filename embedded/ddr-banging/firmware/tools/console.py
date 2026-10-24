# Send console commands to the ddr-banging firmware and print the replies.
#   python3 console.py /dev/cu.usbmodemXXXX tck spd selftest   (needs pyserial)

import sys, time, serial
s = serial.Serial(sys.argv[1], 115200, timeout=0.2)
time.sleep(0.5); s.read(100000)
for cmd in sys.argv[2:]:
    s.write((cmd + "\r\n").encode())
    out, quiet = b"", 0
    t0 = time.time()
    while quiet < 10 and time.time() - t0 < 30:
        d = s.read(4096)
        if d: out += d; quiet = 0
        else: quiet += 1
    print(f"===== {cmd}\n{out.decode(errors='replace')}")
