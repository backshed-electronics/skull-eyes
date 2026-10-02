# Skull Eyes (Rev T)

Two animated fire-cracked skull eyes on two 1.28" GC9A01 round displays, driven by a
Seeed XIAO ESP32-C3. Radar tracking with an Ai-Thinker RD-03D switches on by itself
the moment the radar reports a person, with no code change needed when it arrives.

**Wiring is unchanged from Verified Rev B.** Nothing to re-solder.

## What's in the folder

| File | What it is |
|---|---|
| `SkullEyes.ino` | Tiny starter file the Arduino tools look for |
| `skull_app.cpp` | All behavior, radar and display code. **Settings are at the top.** |
| `eye_render.h` | The layered eye drawing engine |
| `skull_web.cpp` | Wi-Fi, settings page, on/off schedule, firmware updates over Wi-Fi |
| `eye_art.h` | The artwork for all eye styles (generated — don't edit by hand) |
| `art_tools/` | Python scripts that generate `eye_art.h`, including the eyelid art (only needed to change the art) |
| `previews/` | Pictures of the art layers |

## Building

Libraries: **Adafruit GC9A01A** and **Adafruit GFX Library** (BusIO installs with GFX).
Board: **XIAO_ESP32C3** (esp32 by Espressif, version 3.x). Leave *USB CDC On Boot* = Enabled (the default).

**Board: Seeed XIAO ESP32-S3.** In the Arduino IDE set:

| Tools menu | Setting |
|---|---|
| Board | XIAO_ESP32S3 |
| PSRAM | OPI PSRAM |
| USB Mode | Hardware CDC and JTAG |
| Upload Mode | UART0 / Hardware CDC |
| Partition Scheme | leave alone (default gives 3 MB app; this build uses ~1.4 MB) |

The first flash must be over USB. After that, updates can go over Wi-Fi from the settings page.

## What it does without the radar

The eyes cycle through idle "moods", picked at random:

- **Wander**: slow, deliberate looks to a spot, then a long intense hold
- **Side-eye**: suspicious look to one side, one eye narrower than the other
- **Scan**: slow sweep across, like searching
- **Glare**: heavy squint dead ahead, razor-thin pupil, fire flares up
- **Squint**: slowly narrows its eyes while locked on one spot; sometimes one eye narrows more (skeptical), then slowly relaxes
- **Sleepy**: lids sink, eyes drift down, slow blinks, then it snaps awake
- **Stare**: unblinking dead-centre stare while the pupil slowly narrows
- **Eye roll**
- **Double-take**: glance, look back, then snap back with a startle

Running all the time: slow blinks every 4.5–11 seconds (occasionally a double), slow pupil "breathing",
and a candle-like flicker in the glow around the pupil.

## What it does with the radar

- Someone appears → startle (pupil snaps thin, eyes open wide), then the eyes follow them.
- The eyes follow smoothly; bigger moves are a firmer turn, never a twitch.
- Closer people → eyes look slightly down and go a little cross-eyed.
- Someone stands still for 2.5 seconds → it slowly narrows its eyes at them.
- Closer than 1.2 m → glaring squint.
- Person leaves → eyes stare where they vanished, then do a search sweep, then go back to idle.

Radar hookup: RD-03D 5V → 5 V rail, GND → common ground, TX → 47 Ω → D7. RX left open.

## Settings you might need (top of `skull_app.cpp`)

| Symptom | Change |
|---|---|
| Eye movement too slow / too fast | `MOVE_SPEED` (1.0 default; 1.5 = 50% faster, 0.7 = slower) |
| Blinks too slow / too fast | `BLINK_SPEED` (1.0 default; lower = slower) |
| Blinks too often / too rare | `BLINK_EVERY_MIN_S` and `BLINK_EVERY_MAX_S` |
| Squints close too slowly / quickly | `SQUINT_TAU_S` (bigger = slower) |
| Displays black or garbled after updating | `USE_DMA` to `false` (slower older drawing path) |
| Colors look negative / blue | `DISPLAY_INVERT` to `false` |
| One eye is upside down | that eye's `LEFT_ROTATION` / `RIGHT_ROTATION` to `2` |
| Brows look worried instead of angry | `SWAP_BROW_SLANT` to `true` |
| Screen power switch wired | `POWER_SWITCH_INSTALLED` to `true` (and `POWER_SWITCH_ON_HIGH` to match the module) |
| Want the old plain black lids | `LID_ARTWORK` to `false` |
| Want more or less anger | `BROW_ANGER` (0 = none, 0.35 default, 0.6 furious) |
| Eyes lock onto something that isn't there | raise "Ignore closer than" on the settings page |
| With radar: eyes look away from you | `RADAR_X_INVERT` to `true` |
| Glare distance | `RADAR_CLOSE_MM` |

## Serial Monitor (115200 baud)

Every 5 seconds it prints frames per second, then per frame: time spent drawing,
time spent waiting on the SPI link to the displays, and everything else, plus how many
radar packets arrived. At start-up it prints whether DMA drawing is active. Mood changes and radar pickups are printed as they happen.

- `radar frames` stays at 0 with the radar connected → check TX → D7, the 47 Ω resistor, 5 V and ground.
- Expect roughly 18–20 fps with DMA drawing (Rev E measured 8.6 fps). The eye movement is designed around quick jumps and holds,
  which reads as natural at this rate.

## Look: eye style and screen alignment (Rev I)

On the settings page, the **Look** card has:

- **Eye style**: *Classic Pumpkin* (warm fire) or *Ghostly Skeleton* (cold blue-white, bone lids,
  steadier glow, more staring and fewer antics). Changing style restarts it, which takes
  about 10 seconds, because it reloads the artwork into memory.
- **Brightness / Paleness / Warm-cool / Glow**: colour trim sliders. They re-tint the artwork
  held in memory, so changes show up straight away with no restart and no reflash. Brightness
  and Glow tame screens that look blown out; Paleness washes the colour toward white; Warm-cool
  shifts it toward orange or blue. They can dim, wash out and shift the existing colours, but
  they can't turn blue into green — that still means regenerating the art.
- **Eye travel**: how far the iris slides from centre at a full side-glance, in pixels.
  20 is a gentle look, **40 (default)** is a hard glance, 55 is extreme. The radar's full
  ±60° of coverage maps onto this range, so a person at the edge of what the radar can see
  gets the eyes turned as far as they go.
- **Tilt**: rotates everything on one screen, in degrees, for a screen glued in crooked.
  The iris, pupil, lids and lid shadows all turn together.
- **Left eye / Right eye nudge**: moves the whole eye inside its screen, in pixels, to line the two
  up when the screens ended up mounted at slightly different heights. About **190 pixels per inch**:
  1/8 in ≈ 24, 3/16 in ≈ 35, 1/4 in ≈ 47. A screen glued low needs a negative "down" value, which
  pushes its artwork up to match the other eye. Nudges apply immediately, no restart.

Nudging trades a little of the eye's dark surround for alignment: the artwork shifts inside the
round glass, so a thin black crescent appears on the opposite edge. In a recessed socket it doesn't
show.

## Wi-Fi settings page (Rev H)

**First time:** power up the pumpkin, then on your phone join the Wi-Fi network
**PumpkinEyes** (password **pumpkin123**). The settings page should pop up; if not, open
http://192.168.4.1. Enter your home Wi-Fi name and password and tap *Save & restart*.

**After that:** on any phone or PC on your home Wi-Fi, open **http://skull.local**
(or the IP address shown in the Serial Monitor / your router). Bookmark it.

The page has:
- **Status**: eyes on/off and why, the time, Wi-Fi, frame rate, current mood
- **Mode**: *Follow schedule*, *Always on*, *Always off*
- **Schedule**: days of the week, an Evening on-time and a Morning on-time (each can be
  switched off; either can run past midnight), and your time zone
- **Wi-Fi**: home network and the hotspot password
- **Firmware update**: upload a new `.bin` without a cable

The time comes from the internet over your home Wi-Fi, including daylight saving.
Until a schedule is saved it stays in *Always on*. If the clock isn't known yet
(e.g. right after power-up, before Wi-Fi connects) the eyes stay on.

The PumpkinEyes hotspot only runs when it can't reach your home Wi-Fi
(none saved, wrong password, or out of range for 45 seconds). Once home Wi-Fi connects,
the hotspot turns off to save power.

### Updating firmware over Wi-Fi
1. Build the `.bin` on your PC (command at the bottom of this file). It lands in
   `build\SkullEyes.ino.bin`.
2. Open http://skull.local → *Firmware update* → choose that file → *Upload firmware*.
3. The eyes freeze for about 20–40 seconds while it uploads, then it restarts on the new code.
   If anything goes wrong the old firmware keeps running.

## Power switch (optional, recommended on battery)

Without a switch, off-times are black screens: the screen backlights stay lit (a faint grey glow
in the dark) and the radar keeps running. With a switch, off-times cut power to the screens and
radar, and only the XIAO stays awake (at low speed) so the settings page still works.

Wire it in the **+5 V** feed to the screens and radar, never in the ground:

```
5 V rail ──► switch ──► both screens VCC + RD-03D 5V
XIAO D5 (GPIO7) ──► switch control input
XIAO keeps its own 5 V feed (through the Schottky diode), unswitched
All grounds stay connected
```

Either part works:
- **Relay module** (5 V coil, 3.3 V-compatible trigger, use the **NO** contact). Simple and
  familiar, but the coil draws about 70 mA the whole time the eyes are on, and it clicks.
- **High-side MOSFET switch** (P-channel MOSFET with a small driver transistor). Silent and
  draws almost nothing. Avoid the common cheap "MOSFET trigger" boards: most of them switch
  the ground side, which is the wrong side here.

Then set `POWER_SWITCH_INSTALLED = true` at the top of `skull_app.cpp`. If your module turns on
with a LOW input, also set `POWER_SWITCH_ON_HIGH = false`. At an off-time the XIAO stops driving
the display wires (so nothing leaks into the unpowered screens) and switches them off; at the next
on-time it restarts itself to power the screens up and set them up cleanly, which takes a few seconds.

## Changes in Rev Y (ignore the furniture)
- **"Ignore things that never move"** on the settings page, on by default. The radar happily
  reports appliances, walls and cabinets as targets, and the eyes then track a fridge instead of
  a person. Anything that stays within 25 cm of itself for 6 seconds is treated as part of the
  room and skipped until it moves 40 cm. Someone standing perfectly still is dropped too.
- The diagnostics line counts these as `still`.

## Changes in Rev AM
- Added `USER_GUIDE.md`: the settings page explained control by control, with the wiring diagram,
  an eye-style board and the eyelid sets, plus a troubleshooting table.
- Added `docs/` with the diagram and reference images; the wiring diagram is redrawn for the
  ESP32-S3 build (the connections themselves are unchanged since Rev B).

## Changes in Rev AL (eyelids are their own choice)
- Eyelids are no longer tied to the eye style. **Ember rind**, **pale bone**, **dried flesh** or
  **none** (plain darkness), chosen on the settings page. Default is whatever the style ships with.
- The Bloodshot Human style now ships with **dried flesh** rather than fresh skin: a bare skull has
  no skin, and pink lids read as wrong before anyone works out why.
- With lids set to none, a blink reads as the eye sinking back into the socket.
- Changing the lid set restarts the prop, as changing the style does.

## Changes in Rev AK (human eye)
- **Bloodshot Human** style: white sclera with branching blood vessels that gather at the corners,
  a brown iris about half the size of the beast styles', a round pupil, and skin-toned eyelids.
- Iris size, pupil shape and pupil size are now **per style** rather than fixed, so a human iris
  can sit small inside a sclera while the beast eyes still fill the glass.
- The pupil setting gained an **"as the style intends"** option, which is the default: slit for
  the beast styles, round for the human one. Either can still be forced.

## Changes in Rev AJ (round pupil, renamed)
- **Pupil shape** on the settings page: vertical slit (reptile, cat) or round (human, zombie,
  demon). The round pupil dilates and contracts with the same controls, and the glow follows it.
- The project is now **Skull Eyes**: sketch `SkullEyes.ino`, hotspot **SkullEyes** (password
  `skulleyes`), settings page at **http://skull.local**.
- Saved settings survive the rename - they are still stored under the old internal name on
  purpose, so colours, schedule and radar tuning carry over.

## Changes in Rev AI (faster drawing)
- **Round-screen clipping.** The displays are circular, so roughly a fifth of every square frame
  was spent drawing corners nobody can see. Each row is now clipped to the glass, and the corners
  are blacked out with a single memset.
- **One walk per row instead of two.** The socket used to be expanded into a scratch buffer and
  then read back; it is now read straight into place as each segment is drawn.
- **The pupil pass only covers the band around the pupil** rather than the whole iris.
- **Byte-swapping two pixels at a time** with 32-bit operations instead of one at a time.
- Rendering output is unchanged - verified against the previous build's test renders.

## Changes in Rev AH (display clock)
- **Display clock** on the settings page: 40, 60 or 80 MHz, applied after a restart. Sending both
  eyes takes about 46 ms at 40 MHz, 31 at 60, 23 at 80 - which is the hard ceiling on frame rate
  whatever the processor does. Higher clocks need short, tidy wiring; speckles mean come back down.
- Start-up prints the clock in use and the frame rate ceiling it implies.

## Changes in Rev AG (internal RAM, not PSRAM)
- The art now goes to **internal RAM** first and only falls back to PSRAM. PSRAM is external and
  slow for the scattered reads this drawing does; with the art there, frames cost about the same
  as they did on the C3 despite the faster chip.
- Start-up and `[stats]` now say where the art actually landed: internal, PSRAM or flash.
- Drawing strips are back to 24 rows (11.5 KB each), freeing another 15 KB of internal RAM.
- The reserve left free dropped from 120 KB to 64 KB, since fast art matters more than headroom.
- The foreshorten / tilt / tissue path switches itself off while art is in PSRAM.

## Changes in Rev AF (S3 only)
- The ESP32-C3 support is gone. One board, one set of settings, no conditionals: all the art in
  RAM, the fancier drawing on by default, 40-row strips, and Wi-Fi on its own core.
- Building for anything other than the S3 now stops with a clear message instead of misbehaving.

## Changes in Rev AE (ESP32-S3 support)
Built for the S3 but **not yet run on one** - expect to revise once there are real numbers.
- Detects the S3 at build time. On it: all the art goes in RAM (120 KB left free), the drawing
  strips are 40 rows instead of 24, and the foreshortening / tilt / stretched tissue default
  back on.
- **Wi-Fi and the settings page run on core 0**, drawing on core 1. On the C3 that work cost
  about 13 ms of every frame.
- Art that can't fit in internal RAM now tries PSRAM before falling back to flash.
- **No partition change needed on the S3**: its default gives 3 MB of app space (this build
  uses 1.4 MB). The C3 still needs Minimal SPIFFS.
- Build/upload on the S3:
  `arduino-cli compile --fqbn esp32:esp32:XIAO_ESP32S3:PSRAM=opi --upload -p COM3 SkullEyes`
  In the IDE, remember Tools → PSRAM → OPI PSRAM.

## Changes in Rev AD
- Header comments corrected: the sketch covers both the XIAO ESP32-C3 and the ESP32-S3, and
  the wiring is listed by D-number since the GPIO numbers behind them differ between the two.

## Changes in Rev AC (radar map)
- **"What the radar sees"** on the settings page: a live top-down map, four updates a second.
  Radar at the bottom centre, range rings every metre, its 120-degree field of view, the
  "ignore closer than" arc, learned furniture as grey crosses, and every target it reports -
  green for the one being followed, amber for seen but not chosen, dark for rejected. A blue
  line shows where the eyes are pointing.

## Changes in Rev AB (tracking lag)
- **Tracking response** slider (1-10, default 8). It sets the smoothing between the radar and
  the eyes: at 1 they glide and lag about half a second behind, at 10 they go straight to where
  the radar says, in lock step. High settings also show the radar's own jitter.
- Previously the smoothing was fixed at roughly the "3" setting, which is where the lag came from.

## Changes in Rev AA (sleep until someone moves)
- **"Sleep with eyes shut until someone moves"** on the settings page, with "stay awake for N s
  after they leave" (default 12 s). The lids close, and when the radar finds someone the eyes
  snap open, startle, and lock straight onto them.
- While fully shut the frame isn't redrawn at all, so it idles cool and wakes instantly.

## Changes in Rev Z (learn the room)
- **"The room"** card on the settings page. Walk out of the radar's view, press **Learn the room
  (20 s)**, and stay away: everything still visible is furniture, and its position is remembered
  and ignored from then on. Saved to flash, so it survives a power cut. **Forget** clears it.
- It also learns quietly: anything that sits in one spot for 30 seconds is added by itself.
- Someone clearly walking (15 cm/s or more) is still seen even inside a learned spot, so people
  passing the fridge aren't invisible.
- The diagnostics line counts targets rejected as known clutter.

## Changes in Rev Y (follow people, not furniture)
- The radar reports up to three targets, and indoors most of them are appliances, cabinets and
  walls. A target now has to be **moving** to be picked up; once someone is being followed they
  can stand still and keep the eyes' attention for a set time before it lets go.
- Two settings on the page: "Only follow things that move" (on by default) and
  "Keep watching for N s after they stop" (8 s).
- The diagnostics line counts targets rejected as not-moving.

## Changes in Rev X (see the movement)
- **Test sweep** button on the settings page: ignores the radar and swings both eyes their full
  travel for 60 seconds. If that isn't obvious from where you stand, the problem is how far the
  eyes move, not the tracking.
- Livelier defaults for new installs: travel 52 px (was 40), full look at 22 deg off centre
  (was 30). Existing installs keep their saved values - change them on the page.

## Changes in Rev W (eyelids in RAM)
- The eyelid art is palettised too (23 KB instead of 46 KB) and now gets RAM before the socket.
  The lid code looks up a different depth for every pixel, which is the worst possible pattern
  for flash; the socket is read straight through and suffers far less from staying there.
- Colour trim works on the lid palette as well, so it stays instant.

## Changes in Rev V (palette art, visible tracking)
- The iris art is now 256 colours plus a palette: 58 KB instead of 115 KB, so it fits in RAM
  alongside Wi-Fi. Measured colour error is well under one step of the display's own colour
  depth, so it looks the same.
- Colour trim now adjusts the 256-entry palette instead of every pixel, so it is instant and
  works even when the art is in flash.
- **"Full look at ... degrees off centre"** on the settings page, default **30**. Previously
  the eyes only reached full travel at 60 deg, so someone walking across a room moved them
  barely a dozen pixels. The radar line now also prints the iris offset in pixels.

## Changes in Rev T (stop starving the heap)
- 140 KB of RAM is now left free. Rev S handed so much to the artwork that the display driver
  and Wi-Fi ran out mid-frame, which made the eyes blink out and come back.
- The iris art therefore usually stays in flash. On the plain drawing path each row is read
  straight through, so this costs only a few milliseconds a frame.
- Foreshortening / tilt / stretched tissue now default to **off**: they sample the art in a
  slanted pattern that is ten times slower from flash. The page setting only takes effect when
  start-up reports the iris art is in RAM.
- Every `[stats]` line now reports free RAM, with a warning under 25 KB.

## Changes in Rev S (display errors, build time)
- Fixed `Failed to allocate priv TX buffer` / `spi transmit (polling) param failed`. The window
  command sent before each frame lived in flash, so the SPI driver allocated and freed a
  temporary DMA buffer twice per eye per frame. It now lives in RAM.
- More heap is left free (90 KB) and the art is claimed in order of how often each piece is
  read: iris, socket, distance table, lids, tissue. Whatever doesn't fit stays in flash.
- The start-up benchmark no longer keeps five frame structures in RAM (about 9 KB back).
- **Build time (fixed in S2 — the first version wouldn't compile with it set to 1):** `SINGLE_STYLE_ONLY` at the top of `skull_app.cpp`. Set it to 1 to leave the
  Ghostly Skeleton artwork out: measured here, a clean build went from 98 s to 39 s.

## Changes in Rev R (radar distance units)
- "Radar distances are in millimetres / centimetres" on the settings page. Some RD-03D modules
  report position in centimetres, which makes every distance read ten times too small: people
  at 3 m look like clutter at 30 cm, and the near cutoff then throws away real targets.
  Stand a known distance away, read the value, and pick the one that matches.
- The diagnostics line shows distances in metres and states the scale in use.

## Changes in Rev Q (measure, don't guess)
- At start-up the board times each drawing path on itself and prints the results as `[bench]`
  lines: eyes centred, looking aside, plus foreshortening, plus stretched tissue, plus tilt.
  It also states whether the iris art ended up in RAM or flash.
- Start-up messages no longer get dropped by the non-blocking serial port; it goes
  non-blocking only once set-up has finished.
- "Foreshorten and stretch when looking aside" is a tick box on the settings page, so the
  slower drawing path can be switched off and compared live.

## Changes in Rev P (near cutoff)
- "Ignore closer than" on the settings page, default **750 mm**. The radar's nearest range step
  is 750 mm wide and reports the prop itself and nearby wiring as if they were people; this
  discards that whole step. Adjustable from the page, no reflash.
- The diagnostics line shows the cutoff in use.

## Changes in Rev O (memory fragmentation fix)
- The iris art is copied into RAM in ten blocks of 24 rows instead of one 115 KB lump, so a
  fragmented heap can still take it. Each row points at RAM where it fit and flash where it
  didn't, and start-up reports both free memory and the largest single free block.
- Artwork is now claimed before the display DMA buffers as well as before Wi-Fi.

## Changes in Rev N (safety net)
- If the iris art can't fit in RAM, tilt, foreshortening and the stretched tissue switch off
  automatically and a warning prints at start-up. Those effects sample the art in a slanted
  pattern that is fine from RAM and roughly ten times slower from flash.

## Changes in Rev M (speed fix)
- The artwork is now copied into RAM **before** Wi-Fi starts, keeping 80 KB free for Wi-Fi.
  Previously Wi-Fi went first and the iris — read for every pixel of every frame — was left in
  slow flash, which cost about 90 ms per frame. The start-up line now reports how much of the
  art made it into RAM, e.g. "art in RAM: 217 KB of 217 KB".
- The turned-eye drawing path (squash, tilt, stretched tissue) no longer does 64-bit maths per
  pixel; each row is set up once and the inner loop is plain 32-bit adds.
- A small side-glance now uses the fast drawing path instead of the general one.
- The raw radar frame prints as a single line so the USB serial can't chop it up.

## Changes in Rev L (radar diagnostics)
- Every 5 seconds the Serial Monitor prints what the radar is actually reporting: byte and frame
  counts, how many targets were seen and used, why any were rejected, the last target's raw
  numbers, and one raw frame in hex. Set `RADAR_DEBUG = false` in `skull_app.cpp` to quieten it.
- "Flip radar left/right" is now a tick box on the settings page instead of a reflash.
- The settings page shows live radar status.
- The iris art now gets priority when copying art into RAM, so Wi-Fi can't crowd out the one piece
  that matters most for frame rate.

## Changes in Rev K (colour trim)
- Brightness, paleness, warm/cool and glow sliders on the Look card, applied live.
- The trim is applied to the copy of the art in memory, so it costs nothing per frame; retinting
  takes a few milliseconds when you move a slider.

## Changes in Rev J (wide gaze, tilt, stretched tissue)
- Eye travel is now a setting and defaults to twice the old range; the radar's ±60° maps to it.
- At a full side-glance the iris foreshortens, the way a real eye does when it turns, and the
  tissue behind the eye stretches into visible fibres on the side it is pulling away from.
- Per-eye tilt correction on the settings page.
- More texture on the eyelids: folds, creases and pores instead of plain grain.
- `GAZE_SQUASH` and `TISSUE_PULL` at the top of `skull_app.cpp` set how strong the last two are;
  set either to 0 to turn it off.

## Changes in Rev I (skeleton style and alignment)
- Second eye style, Ghostly Skeleton: cold blue-white iris with fine cracks, pale bone eyelids,
  a slow cold pulse instead of candle flicker, and colder behavior (more staring, no eye-rolls).
- Per-eye nudge on the settings page, for screens mounted slightly off from each other.
- Both styles live in the firmware; switching is a menu choice, not a reflash.

## Changes in Rev H (Wi-Fi and schedule)
- Settings page over Wi-Fi: schedule, manual on/off, Wi-Fi setup, firmware updates.
- Internet time with daylight saving; settings survive power-off.
- Off-times: black and asleep screens, or true power-off with the optional switch.
- Needs the Minimal SPIFFS partition scheme (see Building).

## Changes in Rev G (eyelids)
- Blinks, squints and the sleepy droop now close real eyelids instead of black: burnt
  pumpkin-rind lids with a grain that follows the curve of the lid, a crease on the upper
  lid, a dark lash line with a thin fire-lit rim, and a soft shadow cast onto the eye.
- When the eye closes fully, the two lids meet at a seam with both lash lines showing.
- `LID_ARTWORK = false` at the top of `skull_app.cpp` goes back to plain black lids.
- Eye movement, blink and squint timing are unchanged from Rev E.

## Changes in Rev F (speed)
- Eye art is copied from flash into RAM at start-up. Reading it from flash every frame
  was taking about 46 ms per frame; that was the biggest slowdown.
- Frames are now sent by DMA: the SPI hardware sends one strip in the background while
  the chip draws the next one, instead of the chip stopping to wait.
- The Serial Monitor can no longer stall the animation.
- If DMA set-up ever fails, it falls back to the Rev E drawing method automatically.

## Changes in Rev E
- Eye movements are slow, eased glides with long holds instead of quick jumps.
- Blinks are about half a second, less frequent, and double blinks are rarer.
- New Squint mood; squints are much deeper and the lower lid now rises too.
- With radar: standing still in front of it earns a slow squint.

## Programming safety (same as Rev B)

If the displays are on the external 5 V rail, don't run the XIAO from USB alone with that rail
switched off and the display signal wires connected. Either power the 5 V rail while programming
or unplug the displays.

## Command lines

First flash over USB (change COM3 if needed):

```
arduino-cli compile --fqbn esp32:esp32:XIAO_ESP32C3:PartitionScheme=min_spiffs --upload -p COM3 SkullEyes
```

Build a `.bin` for a Wi-Fi update (file appears in `build\`):

```
arduino-cli compile --fqbn esp32:esp32:XIAO_ESP32C3:PartitionScheme=min_spiffs --output-dir build SkullEyes
```
