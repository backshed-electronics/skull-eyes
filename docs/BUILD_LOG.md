# Build notes

Things learned the hard way, in case they save someone else the time.

## Mount the radar portrait
The RD-03D covers ±60° across its wide axis and ±30° across the narrow one. Mounted landscape you
get a tall, narrow beam: people slide out of view as they step aside, and whatever sits dead ahead
dominates. Rotating it 90° fixes tracking that otherwise looks broken.

## Indoors, most radar targets are furniture
Appliances, cabinets and walls are strong reflectors and often nearer than the person. The
firmware only picks up targets that are *moving*, keeps watching them for a while once they stop,
and learns the positions of fixed echoes so it can ignore them.

## The radar's first range step is junk
Range resolution is 0.75 m, and that first step reports the prop itself and nearby wiring as
targets. The "ignore closer than" setting throws that step away.

## Drawing speed is about where the art lives
Reading artwork from flash costs roughly ten times what internal RAM does, and PSRAM sits in
between — fine for streaming, poor for scattered reads. The art is palettised (8-bit indices plus
a 256-colour table) specifically so it fits in internal RAM alongside Wi-Fi. On the ESP32-S3,
check the start-up line says the art is **internal**, not PSRAM.

## Frame rate is capped by the display link
Pushing both 240×240 screens takes about 46 ms at a 40 MHz clock, so ~21 fps is the ceiling
regardless of processor. 80 MHz doubles it, if the wiring is short and tidy.

## The eyes must move further than you think
Tracking that is technically correct can be invisible: a person 18° off centre once moved the iris
12 pixels out of 240. Map the radar's angle onto a much smaller full-travel angle, and give the
eyes real travel, or nobody will notice it working.

## Windows COM port assignment
If uploads fail with "Write timeout" while the board enumerates and resets normally, check which
COM number is actually the board (`mode` in a Command Prompt, with and without it plugged in). A
stale or reserved port will accept a connection and then swallow everything sent to it.

## Saved settings outrank new defaults
Settings live in the chip's flash. Changing a default in the source does nothing on a board that
already has a value saved — including the hotspot password.
