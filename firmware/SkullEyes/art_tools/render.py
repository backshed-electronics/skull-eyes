import numpy as np
from scipy.ndimage import gaussian_filter, map_coordinates
from PIL import Image, ImageDraw, ImageFont

SS = 2                 # supersample factor
N = 240 * SS           # render size
R = N / 2              # display radius (render px)
yy, xx = np.mgrid[0:N, 0:N].astype(np.float32) + 0.5
X = (xx - R) / R       # -1..1
Y = (yy - R) / R

rng = np.random.default_rng(7)

def tile_noise(size, sigma, seed):
    r = np.random.default_rng(seed)
    a = gaussian_filter(r.random((size, size)).astype(np.float32), sigma, mode="wrap")
    a -= a.min(); a /= a.max()
    return a

def fbm_tex(size, seed, octaves=(24, 10, 4, 1.6), weights=(0.45, 0.3, 0.17, 0.08)):
    t = sum(w * tile_noise(size, s, seed + i) for i, (s, w) in enumerate(zip(octaves, weights)))
    t -= t.min(); t /= t.max()
    return t

NOISE_A = fbm_tex(512, 11)
NOISE_B = fbm_tex(512, 23, octaves=(6, 3, 1.5, 0.8))
FIBER = fbm_tex(512, 37, octaves=(3, 1.5, 0.8, 0.5), weights=(0.3, 0.3, 0.25, 0.15))

def sample(tex, u, v):
    return map_coordinates(tex, [v.ravel(), u.ravel()], order=1, mode="grid-wrap").reshape(u.shape)

def smooth(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0, 1)
    return t * t * (3 - 2 * t)

def ramp(stops, t):
    ts = [s[0] for s in stops]
    out = np.zeros(t.shape + (3,), np.float32)
    for c in range(3):
        out[..., c] = np.interp(t, ts, [s[1][c] for s in stops])
    return out

# iris_r: iris radius as a fraction of the display radius. Beast eyes nearly
# fill the glass; a human iris is small inside a white sclera.
STYLES = {
    "Bloodshot Human": dict(
        iris_r=0.46, sclera=True, pupil="round", round_default=True,
        stops=[(0, (.42, .30, .14)), (.25, (.55, .40, .18)), (.55, (.42, .29, .12)),
               (.85, (.26, .17, .07)), (1, (.07, .05, .02))],
        socket=(.90, .88, .86), hot=(.35, .25, .12), cracks=.30, cells=240,
        rim=(.55, .35, .18)),
    "Classic Pumpkin": dict(
        stops=[(0, (1, 1, .85)), (.22, (1, .86, .32)), (.5, (1, .52, .06)),
               (.82, (.78, .2, .01)), (1, (.32, .05, 0))],
        socket=(.22, .05, .0), hot=(1, .95, .6), cracks=.9, pupil="slit", rim=(1, .45, .05)),
    "Reptile": dict(
        stops=[(0, (.95, 1, .6)), (.25, (.78, .95, .22)), (.55, (.4, .72, .06)),
               (.85, (.14, .36, .02)), (1, (.04, .12, 0))],
        socket=(.04, .12, .02), hot=(.9, 1, .5), cracks=.55, cells=240, pupil="slit", rim=(.5, .9, .1)),
    "Ghostly Skeleton": dict(
        stops=[(0, (.92, .98, 1.0)), (.22, (.72, .86, .98)), (.5, (.45, .62, .85)),
               (.82, (.18, .26, .48)), (1, (.05, .07, .16))],
        socket=(.04, .05, .12), hot=(.85, .95, 1), cracks=.55, cells=80, pupil="slit",
        rim=(.55, .72, 1.0)),
    "Demon": dict(
        stops=[(0, (1, .9, .55)), (.3, (1, .5, .08)), (.6, (.88, .14, .02)),
               (.88, (.45, .02, 0)), (1, (.16, 0, 0))],
        socket=(.2, .01, 0), hot=(1, .8, .3), cracks=.8, pupil="round", rim=(1, .25, .02)),
}

from scipy.spatial import cKDTree
def _jgrid(n, seed=5):
    k = int(round(np.sqrt(n)))
    g = (np.arange(k) + 0.5) / k * 2.6 - 1.3
    gx, gy = np.meshgrid(g, g)
    j = np.random.default_rng(seed).uniform(-0.38, 0.38, (k * k, 2)) * (2.6 / k)
    return np.stack([gx.ravel(), gy.ravel()], 1) + j

_trees = {n: cKDTree(_jgrid(n)) for n in (80, 240)}

def crack_dist(u, v, cells=80):
    q = np.stack([u.ravel(), v.ravel()], 1)
    d, _ = _trees[cells].query(q, k=2)
    return ((d[:, 1] - d[:, 0]) * 0.5).reshape(u.shape)

IRIS_R = 0.86          # default; styles can override with iris_r          # iris radius as fraction of display radius
MAX_DX, MAX_DY = 0.17, 0.10

def render_eye(style, gx=0.0, gy=0.0, pupil=1.0, blink=0.0, squint=0.0, left=True, angry=0.35):
    s = STYLES[style]
    # ---- fixed socket layer (does not move) ----
    r_disp = np.sqrt(X * X + Y * Y)
    sock = np.array(s["socket"], np.float32) * (0.55 + 0.9 * sample(NOISE_B, xx * .5, yy * .5)[..., None])
    sock *= (1 - smooth(0.75, 1.0, r_disp))[..., None]

    # ---- moving iris layer ----
    cx, cy = gx * MAX_DX, gy * MAX_DY
    lx, ly = (X - cx) / IRIS_R, (Y - cy) / IRIS_R
    r = np.sqrt(lx * lx + ly * ly)
    th = np.arctan2(ly, lx)
    # radial fibers: sample noise stretched along the radius
    fib = sample(FIBER, (th / (2 * np.pi)) * 512 * 5, r * 25)
    fib = np.clip((fib - .5) * 1.6 + .5, 0, 1)
    fib2 = sample(NOISE_A, (th / (2 * np.pi)) * 512, r * 90)
    t = np.clip(r + (fib - .5) * .18, 0, 1)
    col = ramp(s["stops"], t) * (0.72 + 0.55 * fib)[..., None] * (0.85 + 0.3 * fib2)[..., None]
    # cracks (ridged noise, moves with iris)
    if s["cracks"] > 0:
        wu = lx + (sample(NOISE_B, lx * 60 + 100, ly * 60 + 100) - .5) * .10
        wv = ly + (sample(NOISE_B, lx * 60 + 300, ly * 60 + 300) - .5) * .10
        d = crack_dist(wu, wv, s.get("cells", 80))
        width = 0.010 + 0.012 * sample(NOISE_A, lx * 80, ly * 80)
        crack = (1 - smooth(width * .4, width, d)) * smooth(.30, .50, r) * s["cracks"]
        glowc = np.exp(-d / 0.035) * smooth(.30, .5, r) * s["cracks"]
        col *= (1 + 0.35 * glowc)[..., None]          # hot edges beside cracks
        col *= (1 - 0.92 * crack)[..., None]
    # limbal darkening
    col *= (1 - 0.7 * smooth(.82, 1.0, r))[..., None]

    # pupil
    if s["pupil"] == "slit":
        w = 0.22 * pupil
        h = 0.95
        f = np.abs(lx) - w * np.clip(1 - (ly / h) ** 2, 0, 1)
        f = np.where(np.abs(ly) > h, 1.0, f)
    else:
        f = r - 0.30 * pupil
    edge = 2.5 / (R * IRIS_R)
    pup = 1 - smooth(-edge, edge, f)
    glow = np.exp(-np.clip(f, 0, None) / 0.14) * (1 - pup)
    col += np.array(s["hot"], np.float32) * (0.55 * glow)[..., None]
    col *= (1 - pup)[..., None]

    iris_a = 1 - smooth(1.0 - 2.5 / (R * IRIS_R), 1.0 + 2.5 / (R * IRIS_R), r)
    # outer glow of iris into socket
    halo = np.exp(-np.clip(r - 1, 0, None) / 0.12) * (1 - iris_a) * 0.35
    img = sock + np.array(s["rim"], np.float32) * halo[..., None]
    img = img * (1 - iris_a)[..., None] + col * iris_a[..., None]

    # ---- lids (fixed to display, curved, soft) ----
    u = X if left else -X
    close = max(blink, squint * 0.45)
    top_y = -1.15 + close * 1.15 + 0.10                       # centre of upper lid edge
    upper = top_y + 0.28 * u * u + angry * 0.25 * (u + 1)     # angry: slants toward nose
    bot_y = 1.15 - blink * 1.15 - squint * 0.12
    lower = bot_y - 0.18 * u * u
    e = 3.0 / R
    lid = np.maximum(1 - smooth(upper - e, upper + e, Y), smooth(lower - e, lower + e, Y))
    rimlight = (np.exp(-np.abs(Y - upper) / 0.02) * (Y > upper) +
                np.exp(-np.abs(Y - lower) / 0.02) * (Y < lower)) * 0.25
    img = img * (1 - lid)[..., None] + np.array(s["rim"], np.float32) * rimlight[..., None] * (1 - lid)[..., None]

    # round display mask
    img *= (r_disp < 1.0)[..., None]
    img = np.clip(img, 0, 1)
    im = Image.fromarray((img * 255).astype(np.uint8)).resize((240, 240), Image.LANCZOS)
    return im

def bezel(im, bg=(28, 28, 32)):
    out = Image.new("RGB", (256, 256), bg)
    mask = Image.new("L", (240, 240), 0)
    ImageDraw.Draw(mask).ellipse((0, 0, 239, 239), fill=255)
    out.paste(im, (8, 8), mask)
    ImageDraw.Draw(out).ellipse((5, 5, 250, 250), outline=(70, 70, 78), width=3)
    return out
