#!/usr/bin/env bash
# Fetch footprints + symbols for every LCSC part into footprints/lcsc.*
set -euo pipefail
cd "$(dirname "$0")"
PARTS=(
  C39843328 # RP2354B
  # DIMM (C5164821) omitted: EasyEDA returns 404, footprint is generated in Task 4
  C148481   # XLAT
  C34771    # VTTREG
  C133803   # EXPANDER
  C485077   # DEC138
  C6186     # LDO33
  C116592   # BUCK
  C408345   # L_BUCK
  C42411119 # L_CORE
  C9002     # XTAL12
  C165948   # USBC
  C318884   # SW
  C2297     # LED_G
  C2286     # LED_R
  C160403   # JST_SWD
  C53207574 # HDR_2X25
  C55829516 # HDR_2X4
  C20610291 # HDR_1X24
  C1525     # C100N
  C52923    # C1U
  C23733    # C4U7
  C19702    # C10U
  C45783    # C22U
  C1548     # C15P
  C25100    # R27
  C25105    # R33
  C17168    # R0
  C11702    # R1K
  C25867    # R1K5
  C4109     # R2K
  C25900    # R4K7
  C25905    # R5K1
  C25744    # R10K
)
# easyeda.com's CloudFront returns 403 for easyeda2kicad's default User-Agent, so run it
# through a one-line wrapper that sends a neutral UA instead.
FAILED=()
for c in "${PARTS[@]}"; do
  # resumable: skip parts whose symbol is already in the library
  grep -q "\"$c\"" footprints/lcsc.kicad_sym 2>/dev/null && continue
  sleep 8   # be polite: easyeda.com rate-limits bursts with a 403
  .venv/bin/python -c '
import sys
from easyeda2kicad.easyeda.easyeda_api import EasyedaApi
_init = EasyedaApi.__init__
def init(self):
    _init(self)
    self.headers["User-Agent"] = "channel-board-fetch"
EasyedaApi.__init__ = init
from easyeda2kicad.__main__ import main
sys.argv[0] = "easyeda2kicad"
sys.exit(main())
' --symbol --footprint --lcsc_id="$c" --output footprints/lcsc --overwrite || FAILED+=("$c")
done
if ((${#FAILED[@]})); then echo "FAILED: ${FAILED[*]}" >&2; exit 1; fi
