import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch, FancyArrow

RED, BLK, BLU, GRY = "#c00000", "#111111", "#1f4fd8", "#666"
fig, ax = plt.subplots(figsize=(17, 10.5), dpi=110)
ax.set_xlim(0, 17); ax.set_ylim(-1.0, 11.2); ax.axis("off")

def box(x, top, w, title, lines):
    h = 0.72 + 0.44 * len(lines) + 0.22
    y = top - h
    ax.add_patch(FancyBboxPatch((x, y), w, h, boxstyle="round,pad=0.10,rounding_size=0.16",
                                fc="white", ec="#111", lw=2.0, zorder=3))
    ax.text(x + 0.28, top - 0.52, title, fontsize=13, fontweight="bold", zorder=4)
    for i, ln in enumerate(lines):
        ax.text(x + 0.38, top - 1.08 - i * 0.44, ln, fontsize=11, zorder=4)
    return y

def wire(pts, c, lw=2.3, ls="-"):
    ax.plot([p[0] for p in pts], [p[1] for p in pts], color=c, lw=lw, ls=ls,
            solid_capstyle="round", solid_joinstyle="round", zorder=1)

def arrow(x, y, dx, dy, c):
    ax.add_patch(FancyArrow(x, y, dx, dy, width=0.015, head_width=0.15, head_length=0.2,
                            length_includes_head=True, color=c, zorder=2))

def lab(x, y, t, c, fs=10.5, rot=0, ha="left"):
    ax.text(x, y, t, color=c, fontsize=fs, rotation=rot, ha=ha, va="center", zorder=5,
            bbox=dict(fc="white", ec="none", pad=1.0))

ax.text(0.2, 10.8, "SKULL EYES — WIRING", fontsize=20, fontweight="bold")
ax.text(0.2, 10.42, "Seeed XIAO ESP32-S3  •  2× GC9A01 1.28\" round display  •  optional Ai-Thinker RD-03D radar",
        fontsize=11.5, color="#444")

box(0.3, 9.8, 3.4, "5 V SUPPLY", ["USB-C, or 5 V 1 A", "eyes ~0.13 A", "radar ~0.09 A"])
box(4.6, 9.8, 5.0, "XIAO ESP32-S3", [
    "D1  → LEFT display CS", "D2  → RIGHT display CS", "D3  → both DC",
    "D4  → both RST", "D8  → both SCL (SPI clock)", "D10 → both SDA (MOSI)",
    "D7  ← radar TX (via 47 Ω)", "5V / GND → everything else"])
wire([(3.7, 9.1), (4.6, 9.1)], RED); arrow(4.3, 9.1, 0.3, 0, RED)

box(10.9, 9.8, 5.8, "LEFT GC9A01", ["VCC ← 5 V", "GND ← common ground", "SCL ← D8", "SDA ← D10",
                                    "DC  ← D3", "CS  ← D1", "RST ← D4"])
box(10.9, 5.1, 5.8, "RIGHT GC9A01", ["VCC ← 5 V", "GND ← common ground", "SCL ← D8", "SDA ← D10",
                                     "DC  ← D3", "CS  ← D2", "RST ← D4"])
wire([(9.6, 8.6), (10.2, 8.6), (10.2, 8.2), (10.9, 8.2)], BLU, 2.2); arrow(10.6, 8.2, 0.3, 0, BLU)
wire([(10.2, 8.6), (10.2, 3.9), (10.9, 3.9)], BLU, 2.2); arrow(10.6, 3.9, 0.3, 0, BLU)
lab(10.35, 9.05, "SCK / MOSI / DC / RST / CS-L", BLU, 10)
lab(8.6, 3.55, "SCK / MOSI / DC / RST / CS-R", BLU, 10)

box(4.6, 4.2, 5.0, "RD-03D RADAR (optional)", [
    "5V  ← 5 V", "GND ← common ground", "TX  → 47 Ω → D7", "RX  — leave open",
    "MOUNT PORTRAIT (tall, not wide)"])
wire([(7.1, 4.2), (7.1, 5.0)], BLU, 2.2); arrow(7.1, 4.8, 0, 0.2, BLU)
lab(7.25, 4.6, "radar TX → 47 Ω → D7", BLU)

wire([(1.0, 8.4), (1.0, 0.2), (16.3, 0.2)], BLK, 2.6)
for x in (6.0, 11.4, 14.6):
    wire([(x, 0.2), (x, 0.5)], BLK, 2.0)
lab(1.3, 0.5, "COMMON GROUND — supply −, XIAO GND, both displays, radar", BLK, 11)

notes = [
 "1.  Both displays share SCL, SDA, DC and RST; only CS differs, which is how the XIAO talks to one at a time.",
 "2.  The radar's wide 120° fan is across its SHORT edge — mount the module standing up, or people vanish as they step aside.",
 "3.  47 Ω in series with the radar's TX keeps the signal clean over a longer cable; the radar's RX is unused for now.",
 "4.  Wire the displays by the labels printed on them, not by assumed pin order.",
 "5.  Settings that trip people up: Tools > PSRAM > OPI PSRAM, USB Mode > Hardware CDC and JTAG.",
]
ax.text(0.3, -0.15, "\n".join(notes), fontsize=10.4, va="top", linespacing=1.6)
plt.savefig("/mnt/user-data/outputs/skull_eyes_wiring.png", facecolor="white", bbox_inches="tight")
print("ok")
