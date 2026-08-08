# fpp-smpte

Generate and follow SMPTE Linear Time Code (LTC) on an audio channel, for
[Falcon Player (FPP)](https://github.com/FalconChristmas/fpp).

Lets FPP act as the timecode master for other show systems, or slave an FPP remote to timecode
coming from something else.

## How it works

- **Player mode** — FPP generates LTC from the position of the running playlist and outputs it on
  the selected sound device.
- **Remote mode** — FPP listens for LTC on the selected audio capture device and syncs a local
  playlist to it.

## Features

- Standard timecode frame rates, independently of what FPP itself is outputting.
- **Hour Field Is Playlist Index** — when off, the timecode is elapsed time since the start of the
  playlist. When on, the hour field carries the playlist index and the minutes/seconds/frames give
  the position within that item.
- Optional PipeWire source-node output in addition to SDL, compiled in automatically when the
  PipeWire development headers are present.

## Installation

Install from **Content Setup → Plugins** in the FPP web UI, then restart FPPD.

The build needs `libltc` and SDL3; `apt-get install -y libltc-dev` is run automatically by the
Makefile if the headers aren't already present.

## Configuration

**Input/Output Setup → SMPTE** in the FPP web UI. Select the audio output device (player mode) or
capture device (remote mode), the timecode format, and whether the hour field carries the playlist
index.

## License

GPLv2 — see [LICENSE](LICENSE).
