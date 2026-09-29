# ABS Player: an Audiobookshelf client for the ESP32-S3 round display

A small, self-contained audiobook player that streams from an
[Audiobookshelf](https://www.audiobookshelf.org/) server over Wi-Fi and plays through the board's
built-in speaker. It browses your library on a 1.85" round touchscreen, resumes where you left
off, and syncs listening progress back to the server, so you can move between this and the
Audiobookshelf apps.

> **Disclaimer:** this project was created with [Claude Code](https://claude.com/claude-code),
> Anthropic's AI coding agent. Claude worked directly against the hardware from the terminal,
> using ESP-IDF to build, flash and read the serial logs, and used those logs to debug the board
> bring-up, audio pipeline and UI. The code has only been tested on the one board described below;
> review it before relying on it.

## The idea

Audiobookshelf is a self-hosted server for audiobooks. Its clients are phone and web apps, but
a tiny dedicated player (a round screen with a speaker, sitting on a desk or bedside table) is
nicer for "just carry on with my book". This firmware turns an inexpensive dev board into that:

- **Home:** "Continue Listening" and "Recently Added" shelves of cover art. Swipe left/right
  within a shelf, up/down between shelves.
- **Library:** every book as an A-Z list, a cover carousel, or grouped by author, with an A-Z
  scrub ring on the right edge.
- **Now Playing:** cover art backdrop, a chapter-progress ring you can drag to scrub, ±30 s,
  previous/next chapter and volume. When nothing is loaded it offers your most recent book to
  resume.

## Screenshots

Captured on the device itself (see [Capturing screenshots](#capturing-screenshots)).

| Home | Library | Now Playing |
| :---: | :---: | :---: |
| <img src="docs/media/home_continue.png" width="240"> | <img src="docs/media/library_list.png" width="240"> | <img src="docs/media/player_resume.png" width="240"> |
| <img src="docs/media/home_recent.png" width="240"> | <img src="docs/media/library_covers.png" width="240"> | <img src="docs/media/library_authors.png" width="240"> |

| Browsing covers | Resuming a book |
| :---: | :---: |
| <img src="docs/media/carousel.gif" width="300"> | <img src="docs/media/playing.gif" width="300"> |

<img src="docs/media/library_scrub.png" width="200" align="right">

Dragging the ring on the Library page's right edge jumps through the list A-Z, with the current
letter shown large.
<br clear="right">

## Hardware

**[Waveshare ESP32-S3-Touch-LCD-1.85C](https://docs.waveshare.com/ESP32-S3-Touch-LCD-1.85C)**, **V1 revision**:

| Part | Details |
| --- | --- |
| SoC | ESP32-S3R8, dual-core 240 MHz, Wi-Fi |
| Memory | 8 MB octal PSRAM, 16 MB flash |
| Display | 1.85" round 360×360 IPS, ST77916 over QSPI |
| Touch | CST816T capacitive (I2C) |
| Audio | PCM5101 I2S DAC + NS8002 amplifier, onboard speaker |
| IO expander | TCA9554 (LCD/touch resets, amp enable) |

Pin assignments used (see `main/board.c`):

| Function | GPIO |
| --- | --- |
| I2C SDA / SCL (touch, expander, RTC) | 11 / 10 |
| LCD QSPI SCK, D0–D3, CS | 40, 46, 45, 42, 41, 21 |
| LCD backlight (PWM) | 5 |
| Touch interrupt | 4 |
| I2S BCK / LRCK / DOUT (to PCM5101) | 48 / 38 / 47 |
| LCD reset, touch reset | expander pins EXIO2, EXIO1 |

**Board revisions matter.** Waveshare ships two audio variants of this board. V2 has an ES8311
codec and ES7210 microphone ADC on I2C, and Waveshare's demo code targets it. V1, which this
firmware targets, has a PCM5101 DAC with no control bus, so volume is applied in software. If
your I2C scan shows devices at 0x18/0x40 you have V2, and `board_audio_*` in `main/board.c`
needs porting to the ES8311.

## How playback works

Most audiobooks in a typical library are large single-file `.m4b` (AAC) files, often 20–40
hours long. Their MP4 index tables run to megabytes, which makes seeking into them directly on a
microcontroller impractical. Instead the player uses Audiobookshelf's HLS transcode mode:

1. `POST /api/items/:id/play` advertises only `audio/mpeg` with `forceTranscode`, so the server
   serves every book, whatever its format, as HLS: 6-second MPEG-TS segments named
   `output-N.ts`, containing AAC (or MP3 for MP3 sources, stream-copied).
2. Segment N always starts at N×6 s, even after the server restarts its transcoder for a seek.
   This was verified: a segment is byte-identical whichever way it was produced. So the playlist
   is never downloaded: to play from time *t*, the player fetches segments from ⌊t/6⌋ and
   drops the first (t mod 6) seconds of decoded audio.
3. A **fetch task** streams segments over a keep-alive HTTPS connection into a 512 KB PSRAM
   buffer (about a minute of audio). A **decode task** feeds it through `esp_audio_codec`'s TS
   demuxer and AAC/MP3 decoder to I2S. A **control task** handles commands, the playback
   session, and progress sync (every 20 s, and on pause, seek and close).

Cover art is downloaded already resized by the server, decoded with the ESP32-S3's ROM JPEG
decoder, and held in an LRU cache in PSRAM.

## Project layout

```
main/
  main.c           startup: display, Wi-Fi, library load, refresh loop
  board.c/.h       hardware bring-up: I2C, expander, QSPI LCD, touch, backlight, I2S audio
  wifi.c/.h        station mode using secrets.h
  abs_api.c/.h     Audiobookshelf REST client (library, progress, sessions, covers, HLS segments)
  player.c/.h      streaming player: fetch / decode / control tasks
  cover.c/.h       cover download, JPEG decode and PSRAM LRU cache
  carousel.c/.h    reusable virtual cover carousel (3 card objects, any number of books)
  ui.c, ui_priv.h  UI shell: dock, pages, shared helpers, derived book lists
  ui_home.c        Home shelves
  ui_library.c     Library list / covers / authors + A-Z ring
  ui_player.c      Now Playing
  lv_mem_psram.c   LVGL allocator that keeps all UI objects in PSRAM
  secrets.h.example
```

## Building and flashing

Requires [ESP-IDF](https://docs.espressif.com/projects/esp-idf/) **v5.5** (developed on 5.5.2).
Managed components (LVGL 9.3, esp_lvgl_port, ST77916/CST816S drivers, esp_audio_codec, esp_jpeg)
are fetched automatically on the first build.

1. Create your secrets file (it is git-ignored):

   ```sh
   cp main/secrets.h.example main/secrets.h
   ```

   and fill in:

   | Define | Value |
   | --- | --- |
   | `WIFI_SSID`, `WIFI_PASSWORD` | your 2.4 GHz network |
   | `ABS_SERVER` | server host name only, e.g. `abs.example.com` (HTTPS is assumed) |
   | `ABS_TOKEN` | an Audiobookshelf API key (Settings → API Keys) |

2. Build and flash:

   ```sh
   . $IDF_PATH/export.sh
   idf.py set-target esp32s3     # first time only
   idf.py -p /dev/tty.usbmodem1101 flash monitor
   ```

   On macOS, if `export.sh` picks a Python that doesn't have the IDF virtualenv, point it at
   the right one first, e.g.
   `export IDF_PYTHON_ENV_PATH=~/.espressif/python_env/idf5.5_py3.14_env`.

The console is on the board's native USB (USB-Serial/JTAG).

## Notes and lessons from bring-up

- **Internal RAM is the real constraint, not PSRAM.** Wi-Fi, TLS, DMA buffers and task stacks all
  compete for ~512 KB of on-chip SRAM. LVGL uses a custom allocator (`lv_mem_psram.c`) so every UI
  object lives in PSRAM. That took free internal RAM from ~14 KB to ~41 KB. Covers, the audio
  buffer, JSON parsing and the cover loader's stack are also in PSRAM.
- **Touch:** the CST816T powers up with continuous swipe tracking disabled (MotionMask `0xEC` =
  0), which breaks gestures. `board.c` sets it to `0x06`.
- **Display:** panels that report ID `00 02 7F 7F` need Waveshare's alternate ST77916 init
  table, which is selected automatically.
- **Touch zones:** the progress and A-Z rings claim every touch from their inner edge (~166 px
  from centre) outwards, so all controls are kept inside radius ~150–160 px (see `ui_priv.h`).
- **Server:** some reverse proxies reject default HTTP user agents, so the client sends its own.
- **Debugging aids:** define `PLAYER_STATUS_LOG` (e.g. with `target_compile_definitions` in
  `main/CMakeLists.txt`) to log pipeline stats every second: input rate, decode errors, I2S timing,
  underruns, a loudness envelope and free heap. `PLAYER_NO_SYNC` disables progress sync for tests.

## Capturing screenshots

The screenshots and GIFs above come from the device itself. Build with `UI_CAPTURE` (and
`PLAYER_NO_SYNC`, so the demo playback doesn't move your real progress) by adding this to
`main/CMakeLists.txt`:

```cmake
target_compile_definitions(${COMPONENT_LIB} PRIVATE UI_CAPTURE PLAYER_NO_SYNC)
```

After the library loads, `main/ui_capture.c` runs a scripted tour of the UI and streams each frame
over the console as base64 RGB565. LVGL's clock is replaced by a virtual one that only advances when
the script says so, so animation frames land at exact moments even though each frame takes a
second or two to send. Record the console to a file, then convert it:

```sh
python3 tools/capture_to_media.py serial.log docs/media   # needs ffmpeg
```

Frames named `<name>_NNN` become `<name>.gif` (using each frame's hold time); the rest become PNGs
masked to the round panel. Note that the capture shows your own library's titles and covers.

## Known limitations

- Fonts cover basic Latin only, so accented characters in titles don't render.
- One cover format (JPEG with unusual 1×2 chroma subsampling) isn't supported by the ROM decoder
  and falls back to a title card.
- No screen timeout or sleep yet; the first library load and first cover take a few seconds
  (TLS handshakes). An SD-card cache for covers and the library is a planned improvement.
- Only the first book library on the server is shown; podcasts aren't supported.
