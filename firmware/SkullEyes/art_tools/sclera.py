"""White of a human eye: shaded sclera with blood vessels.

Vessels are drawn as branching polylines with PIL, then blurred, rather than as
a radial function - real ones gather at the inner and outer corners, wander, and
split, which a starburst never looks like.
"""
import numpy as np
from PIL import Image, ImageDraw, ImageFilter
from render import N, X, Y, sample, smooth, NOISE_A, NOISE_B

def _draw_vein(d, x, y, ang, length, width, rng, iris_px, half, depth=0):
    """Walk a vessel inward, wandering, splitting now and then."""
    steps = max(3, int(length / 6))
    for _ in range(steps):
        nx = x + np.cos(ang) * 6
        ny = y + np.sin(ang) * 6
        if np.hypot(nx - half, ny - half) < iris_px * 1.02:      # stop at the iris
            return
        if np.hypot(nx - half, ny - half) > half:                 # or at the edge
            return
        d.line((x, y, nx, ny), fill=int(255 * min(1.0, width / 3.0)), width=max(1, int(width)))
        x, y = nx, ny
        ang += rng.normal(0, 0.22)
        width *= 0.97
        if depth < 2 and rng.random() < 0.06:
            _draw_vein(d, x, y, ang + rng.choice([-1, 1]) * rng.uniform(0.4, 0.9),
                       length * 0.5, width * 0.7, rng, iris_px, half, depth + 1)

def sclera(iris_r, seed=11):
    half = N / 2
    iris_px = iris_r * half
    rng = np.random.default_rng(seed)

    vein_img = Image.new("L", (N, N), 0)
    d = ImageDraw.Draw(vein_img)
    for _ in range(34):
        # start out near the rim, clustered at the left and right corners
        side = rng.choice([0.0, np.pi])
        a = side + rng.normal(0, 0.55)
        r0 = half * rng.uniform(0.80, 1.0)
        x, y = half + np.cos(a) * r0, half + np.sin(a) * r0
        inward = np.arctan2(half - y, half - x) + rng.normal(0, 0.35)
        _draw_vein(d, x, y, inward, half * rng.uniform(0.25, 0.7),
                   rng.uniform(1.6, 3.4), rng, iris_px, half)
    veins = np.asarray(vein_img.filter(ImageFilter.GaussianBlur(1.2)), np.float32) / 255.0

    r = np.sqrt(X * X + Y * Y)
    base = np.array([0.93, 0.91, 0.88], np.float32) * np.ones(X.shape + (3,), np.float32)
    base *= (1 - 0.30 * smooth(0.55, 1.05, r))[..., None]          # vignette into the socket
    base *= (1 - 0.20 * np.clip(-Y, 0, 1))[..., None]              # shadow under the upper lid
    base *= (0.95 + 0.10 * sample(NOISE_B, (X + 1) * 90, (Y + 1) * 90))[..., None]

    red = np.array([0.72, 0.11, 0.09], np.float32)
    col = base * (1 - 0.80 * veins)[..., None] + red * (0.70 * veins)[..., None]

    # pink wash in the corners, and the shadow the iris casts on the white
    corner = np.clip(np.abs(X) - 0.35, 0, 1) * (1 - smooth(0.75, 1.0, r))
    wash = corner * (0.35 + 0.4 * sample(NOISE_A, X * 50, Y * 50))
    col = col * (1 - 0.40 * wash)[..., None] + np.array([0.82, 0.46, 0.42]) * (0.40 * wash)[..., None]
    # No shadow ring baked in around the iris: the iris moves and the sclera
    # does not, so anything drawn at the iris's resting place is left behind.
    col *= (r < 1.0)[..., None]
    return np.clip(col, 0, 1)
