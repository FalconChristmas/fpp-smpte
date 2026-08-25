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

### MultiSync must be enabled

In player mode the timecode is generated from FPP's sequence and media sync callbacks, and FPP only
fires those when **MultiSync is enabled**. With MultiSync off the device opens and the encoder runs
but no LTC frames are ever produced. There is no need for any remotes to be configured — the
setting just has to be on.

### PipeWire source mode

Ticking **Output as PipeWire Source** publishes the LTC as a routable `Audio/Source` node
(`fpp_smpte_ltc`) instead of writing to a device, so it can be mixed onto a shared multi-channel
card, sent to AES67, and so on.

Routing it requires **Media Backend → PipeWire (Advanced)**: the node is patched in from
*Input/Output Setup → Audio/Video → Input Mixing (Mix Buses)*, by adding a member of type
**PipeWire Source**. Simple PipeWire does not expose that page, so on the default backend the node
is published but there is nowhere to route it.

The LTC node is **mono**. Mixing it into a stereo (or wider) input group needs FPP 10 with the
channel-mapping fix from [FalconChristmas/fpp#2754](https://github.com/FalconChristmas/fpp/issues/2754);
older builds mix a mono source in as silence.

## License

GPLv2 — see [LICENSE](LICENSE).
