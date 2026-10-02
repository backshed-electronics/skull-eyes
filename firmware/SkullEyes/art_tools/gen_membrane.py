"""Stretchy tissue behind the eye.

Stored in iris-local coordinates (120x120, each texel = 2x2 screen px, centred
on the iris), so when the iris slides the texture slides with it: the streaks
read as skin being dragged. Sampled only in the socket ring on the side the eye
is pulling away from, and added on top of the socket.
"""
import numpy as np
from render import sample, smooth, NOISE_A, NOISE_B, FIBER

SIZE = 120                      # texels; covers 240x240 screen px iris-local

g = (np.arange(SIZE) * 2 + 1) - 120.0
X, Y = np.meshgrid(g, g)
R = np.sqrt(X * X + Y * Y)
TH = np.arctan2(Y, X)

MEMBRANE_PALETTES = {
    "Classic Pumpkin":   dict(fibre=(.55, .20, .04), tension=(1.0, .55, .12), web=(.30, .10, .02)),
    "Bloodshot Human":   dict(fibre=(.62, .26, .24), tension=(.95, .55, .50), web=(.40, .20, .18)),
    "Ghostly Skeleton":  dict(fibre=(.42, .46, .55), tension=(.70, .84, 1.0), web=(.22, .26, .34)),
}

def membrane(style, IRIS_PX=103.2):
    p = MEMBRANE_PALETTES[style]
    # long streaks running outward from the iris: high frequency around the
    # circle, low frequency along the radius
    streak = sample(FIBER, (TH / (2 * np.pi)) * 512 * 3.0, R * 0.22)
    streak = np.clip((streak - .42) * 2.6, 0, 1)
    fine = sample(NOISE_A, (TH / (2 * np.pi)) * 512 * 8, R * 0.7)
    web = sample(NOISE_B, X * 0.9 + 30, Y * 0.9 + 70)

    # strongest right at the iris edge, fading out into the socket
    near = np.exp(-np.clip(R - IRIS_PX, 0, None) / 34.0)
    inner = smooth(IRIS_PX - 12, IRIS_PX + 2, R)          # nothing under the iris
    amount = near * inner

    col = (np.array(p["fibre"]) * (0.35 + 0.9 * streak)[..., None]
           + np.array(p["web"]) * (0.25 + 0.5 * web)[..., None] * 0.6
           + np.array(p["fibre"]) * (0.25 * fine)[..., None])
    # taut highlight hugging the iris edge, like tissue under tension
    ridge = np.exp(-((R - (IRIS_PX + 3)) / 6.5) ** 2)
    col = col * amount[..., None] + np.array(p["tension"]) * (0.55 * ridge * inner)[..., None]
    return np.clip(col * 0.62, 0, 1)

def to565(rgb):
    q = np.clip(np.round(rgb * [31, 63, 31]), 0, [31, 63, 31]).astype(np.uint32)
    return (q[..., 0] << 11) | (q[..., 1] << 5) | q[..., 2]
