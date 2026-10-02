"""Eyelid artwork, stored in "lid space": column = screen x / 2 (120 wide),
row = pixels in from the lid's edge (0..95). The chip looks up each covered
pixel by its distance from the lid edge, so the lash line, crease and grain
follow the curve of the lid as it moves."""
import numpy as np
from render import sample, smooth, NOISE_A, NOISE_B, FIBER

DEPTH = 96
xs = (np.arange(120) * 2 + 1).astype(np.float32)
ds = np.arange(DEPTH).astype(np.float32) + 0.5
X, D = np.meshgrid(xs, ds)
U = (X - 120) / 120

# Lid sets, chosen independently of the eye style. A bare skull has no eyelids,
# so "dried" (desiccated tissue) and black lids suit it better than fresh skin.
LID_PALETTES = {
    "Dried flesh": dict(
        up=[(0, (.46, .33, .22)), (14, (.34, .23, .15)), (40, (.19, .12, .08)), (95, (.05, .03, .02))],
        lo=[(0, (.38, .26, .17)), (12, (.26, .17, .11)), (35, (.14, .09, .06)), (95, (.04, .02, .02))],
        ridge=(.26, .16, .09), rim=(.75, .35, .22), crease=True),
    "Classic Pumpkin": dict(
        up=[(0, (.78, .34, .06)), (14, (.58, .22, .04)), (40, (.30, .10, .02)), (95, (.09, .025, 0))],
        lo=[(0, (.60, .24, .04)), (12, (.40, .14, .02)), (35, (.16, .05, .01)), (95, (.06, .015, 0))],
        ridge=(.35, .13, .02), rim=(1.0, .62, .16), crease=True),
    "Bloodshot Human": dict(
        up=[(0, (.72, .56, .50)), (14, (.56, .40, .36)), (40, (.34, .22, .20)), (95, (.10, .06, .05))],
        lo=[(0, (.64, .48, .43)), (12, (.44, .30, .27)), (35, (.22, .14, .12)), (95, (.07, .04, .04))],
        ridge=(.30, .18, .15), rim=(.95, .70, .62), crease=True),
    "Ghostly Skeleton": dict(
        up=[(0, (.62, .64, .70)), (14, (.44, .47, .54)), (40, (.20, .23, .30)), (95, (.05, .06, .09))],
        lo=[(0, (.50, .53, .60)), (12, (.32, .35, .42)), (35, (.13, .15, .20)), (95, (.04, .05, .07))],
        ridge=(.22, .26, .34), rim=(.72, .85, 1.0), crease=False),
}

def ramp(stops, t):
    ts = [s[0] for s in stops]
    return np.stack([np.interp(t, ts, [s[1][c] for s in stops]) for c in range(3)], -1)

def lid(upper, style="Classic Pumpkin"):
    p = LID_PALETTES[style]
    grain = sample(FIBER, X * 0.35 + (0 if upper else 200), D * 2.2)
    grain = np.clip((grain - .5) * 1.8 + .5, 0, 1)
    blot = sample(NOISE_A, X * 0.6 + 50, D * 0.9 + (0 if upper else 300))
    skin = ramp(p["up"] if upper else p["lo"], D)
    fade = 1 - smooth(55, 90, D)
    skin = skin * (1 + fade * (0.45 * grain - 0.30))[..., None] * (1 + fade * (0.4 * blot - 0.2))[..., None]
    skin *= (1 - 0.35 * np.abs(U) ** 2)[..., None]
    if upper and p["crease"]:
        cd = 20 + 4 * U * U
        skin *= (1 - 0.65 * np.exp(-((D - cd) / 2.2) ** 2))[..., None]
        skin += np.array(p["ridge"]) * np.exp(-((D - cd - 5) / 3.0) ** 2)[..., None]
    elif upper:
        # bone: a shallow ridge instead of a fleshy crease
        cd = 24 + 5 * U * U
        skin *= (1 - 0.35 * np.exp(-((D - cd) / 3.5) ** 2))[..., None]
        skin += np.array(p["ridge"]) * 0.6 * np.exp(-((D - cd - 6) / 4.0) ** 2)[..., None]
    # Creases and folds: several soft lines running along the lid, each wobbling
    # a little across its length, plus fine pores. Adds a lot of life when the
    # lid is halfway down.
    wob = sample(NOISE_B, X * 0.8 + (0 if upper else 400), D * 0.05)
    folds = np.zeros_like(D)
    lift = np.zeros_like(D)
    for k, (base, amp, wide) in enumerate([(9, .34, 1.3), (33, .40, 1.8), (47, .32, 1.6),
                                           (61, .28, 2.0), (76, .22, 2.4)]):
        dk = base + 5.0 * (wob - .5) + 2.5 * U * U * (1 if k % 2 else -1)
        folds += amp * np.exp(-((D - dk) / wide) ** 2)
        lift += 0.45 * amp * np.exp(-((D - dk - wide * 1.8) / (wide * 1.3)) ** 2)
    skin *= (1 - np.clip(folds, 0, .85))[..., None]
    skin += skin * lift[..., None]
    pores = sample(NOISE_A, X * 3.4 + 11, D * 4.2 + 5)
    skin *= (0.84 + 0.32 * pores)[..., None]

    rim = np.exp(-(D / 0.9) ** 2)
    lash = smooth(0.8, 1.6, D) * (1 - smooth(4.0, 6.5, D)) if upper else \
           smooth(0.8, 1.6, D) * (1 - smooth(2.5, 4.5, D))
    col = skin * (1 - 0.92 * lash)[..., None]
    col = col * (1 - rim)[..., None] + np.array(p["rim"]) * (0.95 * rim)[..., None]
    return np.clip(col, 0, 1)

def to565(rgb):
    q = np.clip(np.round(rgb * [31, 63, 31]), 0, [31, 63, 31]).astype(np.uint32)
    return (q[..., 0] << 11) | (q[..., 1] << 5) | q[..., 2]
