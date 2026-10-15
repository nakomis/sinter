"""DDR3 240-pin UDIMM pin assignments (JEDEC names), 1-240 -> signal.

Source: Micron MT9JSF12872AZ / MT9JSF25672AZ datasheet (240-pin DDR3 UDIMM ECC),
Table 6 "Pin Assignments":
https://mm.digikey.com/Volume0/opasdata/d220001/medias/docus/1300/MT9JSF12872AZ_MT9JSF25672AZ.pdf
That module is 1Rx8, so Micron prints NC for pins other modules use; those take
their JEDEC UDIMM names (CK1/CK1# 63-64, S1# 76, ODT1 77, S2# 79, CKE1 169,
S3# 198; A14/A15 appear as NF/A14, NF/A15 and keep A14/A15). Pins left NC are
ERR_OUT#/PAR_IN/EVENT#/TDQS# and the two pins either side of the key (48, 49).
Pins 1-120 are the front contacts, 121-240 the back; pin 120+n is behind pin n.
"""
import re

UDIMM_PINS = {
    1: "VREFDQ", 2: "VSS", 3: "DQ0", 4: "DQ1", 5: "VSS", 6: "DQS0#", 7: "DQS0", 8: "VSS", 9: "DQ2", 10: "DQ3",
    11: "VSS", 12: "DQ8", 13: "DQ9", 14: "VSS", 15: "DQS1#", 16: "DQS1", 17: "VSS", 18: "DQ10", 19: "DQ11", 20: "VSS",
    21: "DQ16", 22: "DQ17", 23: "VSS", 24: "DQS2#", 25: "DQS2", 26: "VSS", 27: "DQ18", 28: "DQ19", 29: "VSS", 30: "DQ24",
    31: "DQ25", 32: "VSS", 33: "DQS3#", 34: "DQS3", 35: "VSS", 36: "DQ26", 37: "DQ27", 38: "VSS", 39: "CB0", 40: "CB1",
    41: "VSS", 42: "DQS8#", 43: "DQS8", 44: "VSS", 45: "CB2", 46: "CB3", 47: "VSS", 48: "NC", 49: "NC", 50: "CKE0",
    51: "VDD", 52: "BA2", 53: "NC", 54: "VDD", 55: "A11", 56: "A7", 57: "VDD", 58: "A5", 59: "A4", 60: "VDD",
    61: "A2", 62: "VDD", 63: "CK1", 64: "CK1#", 65: "VDD", 66: "VDD", 67: "VREFCA", 68: "NC", 69: "VDD", 70: "A10",
    71: "BA0", 72: "VDD", 73: "WE#", 74: "CAS#", 75: "VDD", 76: "S1#", 77: "ODT1", 78: "VDD", 79: "S2#", 80: "VSS",
    81: "DQ32", 82: "DQ33", 83: "VSS", 84: "DQS4#", 85: "DQS4", 86: "VSS", 87: "DQ34", 88: "DQ35", 89: "VSS", 90: "DQ40",
    91: "DQ41", 92: "VSS", 93: "DQS5#", 94: "DQS5", 95: "VSS", 96: "DQ42", 97: "DQ43", 98: "VSS", 99: "DQ48", 100: "DQ49",
    101: "VSS", 102: "DQS6#", 103: "DQS6", 104: "VSS", 105: "DQ50", 106: "DQ51", 107: "VSS", 108: "DQ56", 109: "DQ57", 110: "VSS",
    111: "DQS7#", 112: "DQS7", 113: "VSS", 114: "DQ58", 115: "DQ59", 116: "VSS", 117: "SA0", 118: "SCL", 119: "SA2", 120: "VTT",
    121: "VSS", 122: "DQ4", 123: "DQ5", 124: "VSS", 125: "DM0", 126: "NC", 127: "VSS", 128: "DQ6", 129: "DQ7", 130: "VSS",
    131: "DQ12", 132: "DQ13", 133: "VSS", 134: "DM1", 135: "NC", 136: "VSS", 137: "DQ14", 138: "DQ15", 139: "VSS", 140: "DQ20",
    141: "DQ21", 142: "VSS", 143: "DM2", 144: "NC", 145: "VSS", 146: "DQ22", 147: "DQ23", 148: "VSS", 149: "DQ28", 150: "DQ29",
    151: "VSS", 152: "DM3", 153: "NC", 154: "VSS", 155: "DQ30", 156: "DQ31", 157: "VSS", 158: "CB4", 159: "CB5", 160: "VSS",
    161: "DM8", 162: "NC", 163: "VSS", 164: "CB6", 165: "CB7", 166: "VSS", 167: "NC", 168: "RESET#", 169: "CKE1", 170: "VDD",
    171: "A15", 172: "A14", 173: "VDD", 174: "A12", 175: "A9", 176: "VDD", 177: "A8", 178: "A6", 179: "VDD", 180: "A3",
    181: "A1", 182: "VDD", 183: "VDD", 184: "CK0", 185: "CK0#", 186: "VDD", 187: "NC", 188: "A0", 189: "VDD", 190: "BA1",
    191: "VDD", 192: "RAS#", 193: "S0#", 194: "VDD", 195: "ODT0", 196: "A13", 197: "VDD", 198: "S3#", 199: "VSS", 200: "DQ36",
    201: "DQ37", 202: "VSS", 203: "DM4", 204: "NC", 205: "VSS", 206: "DQ38", 207: "DQ39", 208: "VSS", 209: "DQ44", 210: "DQ45",
    211: "VSS", 212: "DM5", 213: "NC", 214: "VSS", 215: "DQ46", 216: "DQ47", 217: "VSS", 218: "DQ52", 219: "DQ53", 220: "VSS",
    221: "DM6", 222: "NC", 223: "VSS", 224: "DQ54", 225: "DQ55", 226: "VSS", 227: "DQ60", 228: "DQ61", 229: "VSS", 230: "DM7",
    231: "NC", 232: "VSS", 233: "DQ62", 234: "DQ63", 235: "VSS", 236: "VDDSPD", 237: "SA1", 238: "SDA", 239: "VSS", 240: "VTT",
}


def lane_of(signal):
    m = re.fullmatch(r"(DQ|DQS|DM)(\d+)#?", signal)
    if m:
        n = int(m.group(2))
        return n // 8 if m.group(1) == "DQ" else n
    if re.fullmatch(r"CB\d", signal):
        return 8
    return None
