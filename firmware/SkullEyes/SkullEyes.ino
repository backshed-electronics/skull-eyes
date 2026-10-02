/*
  SkullEyes.ino  (Rev AM)

  Two animated eyes on two 1.28" GC9A01 round displays, with optional RD-03D
  radar tracking, a Wi-Fi settings page and over-the-air updates.

  BOARD: Seeed XIAO ESP32-S3
    Tools > Board      : XIAO_ESP32S3
    Tools > PSRAM      : OPI PSRAM
    Tools > USB Mode   : Hardware CDC and JTAG
    Tools > Upload Mode: UART0 / Hardware CDC
    Nothing to change under Partition Scheme: the default gives 3 MB of app
    space and this build uses about 1.4 MB.

    Wiring is listed by D-number below, as printed on the board.

  WIRING (unchanged since Verified Rev B)
    D8   -> both displays SCL (SPI clock)
    D10  -> both displays SDA (SPI data)
    D3   -> both displays DC
    D1   -> LEFT display CS
    D2   -> RIGHT display CS
    D4   -> both displays RST
    D7   <- RD-03D radar TX through 47 ohm (radar RX left open)
    D5   -> optional screen power switch (see README)

  HOW IT DRAWS
    The eye is built in layers - a fixed dark socket, an iris that slides
    inside it, a live slit pupil with a hot glow, and curved lids. Each eye is
    composed 24 rows at a time and streamed to its display in one continuous
    transfer, so the screen never shows a half-drawn or blanked frame.


  Without the radar the eyes run a set of idle behaviors (wandering glances,
  side-eye, scanning, glaring, sleepy droop, eye rolls, double-takes, staring).
  When an RD-03D is connected, tracking switches on automatically as soon as it
  reports a person.

  Libraries: Adafruit GFX Library, Adafruit GC9A01A (Adafruit BusIO comes with GFX).
*/

// All settings and logic live in skull_app.cpp (settings are near the top).

void skullSetup();
void skullLoop();

void setup() { skullSetup(); }
void loop()  { skullLoop(); }
