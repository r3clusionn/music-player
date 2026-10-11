# music-player

A music player for Linux in the spirit of foobar2000: a dense, fast window of playlists in columns you define, a library tree, gapless playback and ReplayGain, and nothing else running. Written in C with GTK3, libmpv for playback and ffmpeg for tags and cover art. It is made for one laptop (Ryzen AI 9 HX 475, Gentoo with dwm, PipeWire started on demand) and built to cost almost nothing while it plays: no new system packages, no timers while paused, and a large audio buffer so the processor sleeps between writes.

**Status:** planned, phase 5 of 7 of [the plan](https://github.com/r3clusionn/media-player/blob/main/docs/PLAN.md#5-music-player-music-player). No code yet. Screenshots will be captures of the real program once it runs.

## Features

- **foobar2000-style layout:** a library tree (artist, album, track or by folder), playlist tabs, a cover art panel and a properties panel, in splitters you can rearrange.
- **Columns you define** with format strings: `%artist%`, `%album%`, `%tracknumber%`, `%title%`, `%length%`, `%codec%`, `%bitrate%` and `$if(...)`. Click a header to sort, drag rows to reorder.
- **Playback:** gapless, ReplayGain by track or album with preamp and clipping prevention, playback orders (repeat, shuffle tracks, shuffle albums), a queue, stop after current, a 10-band equalizer.
- **Formats:** everything ffmpeg plays (FLAC, ALAC, Opus, MP3, AAC, WavPack, APE, DSD), `.cue` sheets and `.m3u` playlists.
- **Search as you type** across the whole library, from a SQLite full-text index.
- **Bit-perfect option:** asks PipeWire to follow each file's sample rate.
- **Control from anywhere:** `music-player-ctl play|pause|toggle|next|prev|stop|vol+|vol-` over a UNIX socket, for window-manager keys. No D-Bus needed.

## How to install

Requires gcc, meson, ninja, pkgconf, GTK 3, libmpv (mpv 0.41), ffmpeg (libavformat, libavcodec) and SQLite.

```sh
git clone https://github.com/r3clusionn/music-player
cd music-player
meson setup build --buildtype=release -Db_lto=true
ninja -C build
```

## How to use

To be written with the program.

## How it works

- **Playback** is libmpv with video turned off (`vid=no`): gapless audio, ReplayGain and the equalizer are mpv's own.
- **Tags and cover art** are read with libavformat; covers are decoded with ffmpeg's JPEG and PNG decoders (the system's gdk-pixbuf has no JPEG loader) and scaled once to the panel's size.
- **The library** is a SQLite database with an FTS5 index; a rescan compares file modification times, and folders are watched only while the window is open.
- **Power:** no wakeups while paused; while playing, only mpv's audio thread runs and the position display updates only when the window is visible.

## Measurements

None yet. Targets: under 30 MB with a 20,000-track library, and under 0.5 W above system idle while playing.

## Verification

None yet. Planned: gapless playback checked on a gapless album, ReplayGain values against the files' tags, scan time and memory with a 20,000-track library.

## Limits

- Linux with X11 only; built for one machine (`-march=native`).
- No streaming services; local files only.

## License

MIT (see `LICENSE`).
