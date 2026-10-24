# Extend the ddr-banging badge to full bleed for the 63.5 mm round labels:
# fits the orange/blue background per side of the diagonal, smooths it, and
# feathers it out from the badge.  python3 extend_bleed.py ddr-banging-logo.png out.png

import sys
from PIL import Image, ImageDraw, ImageFilter
import numpy as np
from scipy.ndimage import gaussian_filter
src, out = sys.argv[1], sys.argv[2]
a = np.asarray(Image.open(src).convert('RGB')).astype(float)
H, W, _ = a.shape
X0, Y0, X1, Y1, R = 299, 295, 1750, 1750, 170
C = 2036
yy, xx = np.mgrid[0:H, 0:W]
side = (xx + yy) < C
rr = Image.new('L', (W, H), 0)
ImageDraw.Draw(rr).rounded_rectangle((X0+25, Y0+25, X1-25, Y1-25), R, fill=255)
inbadge = np.asarray(rr) > 0
far = np.abs(xx + yy - C) > 25
r, g, b = a[..., 0], a[..., 1], a[..., 2]
orange_px = (r > b + 60) & (r < 245) & (g < 0.6 * r)          # plain orange
blue_px = (b > r + 25) & (b > g + 8) & (b + g + r < 330)       # plain blue, not green PCB
bgm = inbadge & far & ((side & orange_px) | (~side & blue_px))
# erode away a few px around any non-background so halos of the chip glow etc. drop out
bgm = gaussian_filter(bgm.astype(float), 3) > 0.99
def smooth(sidemask, s=40):
    m = (bgm & sidemask).astype(float)
    num = np.stack([gaussian_filter(a[..., k] * m, s) for k in range(3)], -1)
    den = gaussian_filter(m, s)[..., None]
    return num / np.maximum(den, 1e-6), den[..., 0]
so, do = smooth(side); sb, db = smooth(~side)
# carry the smoothed edge colour outwards: clamp each point into the badge
m = 70
cx = np.clip(xx, X0+m, X1-m); cy = np.clip(yy, Y0+m, Y1-m)
fo = so[cy, cx]; fb = sb[cy, cx]
# where the clamped point has no support for its side, fall back to that side's mean
mo = a[bgm & side].mean(0); mb = a[bgm & ~side].mean(0)
fo = np.where((do[cy, cx] > 0.02)[..., None], fo, mo); fb = np.where((db[cy, cx] > 0.02)[..., None], fb, mb)
t = np.clip((C - (xx + yy)) / 3 + 0.5, 0, 1)[..., None]
fill = t * fo + (1 - t) * fb
mk = Image.new('L', (W, H), 0)
ImageDraw.Draw(mk).rounded_rectangle((X0+60, Y0+60, X1-60, Y1-60), R, fill=255)
mk = np.asarray(mk.filter(ImageFilter.GaussianBlur(25))).astype(float)[..., None] / 255
res = mk * a + (1 - mk) * fill
Image.fromarray(np.clip(res, 0, 255).astype(np.uint8)).save(out)
