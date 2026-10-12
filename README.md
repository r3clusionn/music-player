# music-player

A music player for Linux in the spirit of foobar2000: a dense, fast window of playlists in columns you define, a library tree, gapless playback and ReplayGain, and nothing else running. Written in C with GTK3, libmpv for playback, ffmpeg for tags and cover art, and SQLite for the library. It is made for one laptop (Ryzen AI 9 HX 475, Gentoo with dwm, PipeWire started on demand) and built to cost almost nothing while it plays: no new system packages, nothing at all running while paused, and big audio buffers so the processor sleeps between writes.

**Status:** working, version 1.0, used on the target laptop. Gapless playback and ReplayGain are verified sample by sample, and the library is tested with 20,000 tracks (see Verification). There is no real music collection on the test machine, so the screenshot shows the generated test albums.

![The music-player window at 2880x1800 in its dark theme: a menu bar and a row of transport buttons, a seek bar, the time 0:01 / 0:12, the playback order and a volume slider; on the left the library tree (All music, 15 tracks, by album artist), the album's cover art and the properties of the playing track; on the right a playlist tab with the columns Artist/album, Track no, Title / track artist and Duration, listing five Sine Ensemble tones, three cue sheet tracks, two Opus tracks, three gapless parts and an AAC track, with "Tone 3 (329.63 Hz)" in bold and marked as playing; the status bar reads FLAC · 44.1 kHz · 16 bit · mono · 131 kbps](docs/images/music-player.png)

## Features

- **foobar2000-style layout:** the library tree, playlist tabs, a cover art panel and a properties panel, in splitters whose positions are kept.
- **Columns you define** with foobar2000's title formatting: `%artist%`, `%album artist%`, `%tracknumber%`, `%title%`, `%length%`, `%codec%`, `%bitrate%`, `[...]` blocks, `$if(...)`, `$num(...)` and more. Click a header to sort, right-click it to show or hide columns, and drag rows to reorder.
- **Playback:**
  - gapless, also between the tracks of a cue sheet;
  - ReplayGain by track or album, with preamp and clipping prevention (Opus R128 gains included);
  - playback orders: default, repeat playlist, repeat track, shuffle tracks, shuffle albums, random;
  - a playback queue, stop after current, Previous that restarts the track after 3 seconds;
  - a 10-band equalizer.
- **Formats:** everything ffmpeg plays (FLAC, ALAC, Opus, Vorbis, MP3, AAC, WavPack, APE, WAV, AIFF, DSD), `.cue` sheets and `.m3u`/`.m3u8` playlists.
- **Library:** folders you choose, scanned on a thread; only new or changed files are read again. Views by album artist, artist, genre, year or folder. Search as you type over titles, artists, albums, genres, dates and paths.
- **Sample-rate following:** optionally lets PipeWire switch to each file's sample rate instead of resampling.
- **Control from anywhere:** `music-player-ctl` over a UNIX socket for window-manager keys and scripts. No D-Bus needed. Starting `music-player FILES` while it runs hands the files to the open window.

## How to install

Requires gcc, meson, ninja, pkgconf, GTK 3, libmpv (mpv 0.41), ffmpeg (libavformat, libavcodec, libswscale), SQLite with FTS5 and libX11. Linux with X11 only.

```sh
git clone https://github.com/r3clusionn/music-player
cd music-player
meson setup build --prefix=$HOME/.local
ninja -C build install
```

This installs `music-player` (242 KB) and `music-player-ctl` (16 KB) to `~/.local/bin`.

## How to use

```sh
music-player                      # open the window
music-player ~/Music/Album        # play a folder, file, cue sheet or playlist
music-player --add-folder ~/Music # add a library folder (Preferences does the same)
```

Double-click a library node to play it, or right-click it to send it to a playlist or the queue. In a playlist, Enter or a double-click plays, Delete removes and Q queues. Tabs are renamed with a double-click (or F2) and closed with a middle click.

| Key | Action |
|---|---|
| Space | Play / pause |
| Left / Right | Seek 5 s back / forward |
| Ctrl+Left / Ctrl+Right | Previous / next |
| Ctrl+. | Stop |
| Ctrl+F | Search the library |
| Ctrl+J | Show the playing track |
| Ctrl+E | Equalizer |
| Ctrl+P | Preferences |
| Ctrl+O / Ctrl+Shift+O | Add files / a folder to the playlist |
| Ctrl+N / Ctrl+W | New / close playlist |
| Ctrl+L / Ctrl+S | Load / save a playlist as .m3u8 |
| F11 | Fullscreen |
| Media keys | Play, pause, stop, next, previous |

`music-player-ctl` takes one command and prints the answer:

| Command | What it does |
|---|---|
| `play`, `pause`, `toggle`, `stop`, `next`, `prev` | Playback |
| `vol+`, `vol-`, `vol N` | Volume by 5%, or to N% |
| `seek+`, `seek-`, `seek N` | 5 s back or forward, or to N seconds |
| `order NAME` | `default`, `repeat`, `repeat-track`, `shuffle`, `shuffle-albums`, `random` |
| `stop-after` | Toggle stop after the current track |
| `status` | `playing`/`paused`/`stopped`, artist, title, album, position, length (tab-separated) |
| `open PATH`, `add PATH` | Replace or add to the current playlist |
| `search TEXT` | Search the library |
| `show`, `fullscreen`, `rescan`, `quit` | Window and library |

Settings live in `~/.config/music-player/config.ini`; the library and playlists live in `~/.local/share/music-player/library.db`.

## How it works

- **Playback** is libmpv with video off. mpv's playlist always holds the playing track and the next one, which is asked from the app when a track starts. That makes changes gapless, and a change of order, queue or stop-after takes effect at once. ReplayGain and the equalizer (a chain of ffmpeg `equalizer` filters, with headroom for boosts) are mpv's own.
- **mpv starts on the first play.** An open window that has played nothing runs no mpv threads. While paused, nothing in the program wakes up.
- **The position display** updates once a second, on the second, only while playing and while the window can be seen (not on another dwm tag, minimized or fully covered).
- **Buffers:** a one-second mpv audio buffer and a 200 ms PipeWire buffer. That halves PipeWire's wakeups for this stream compared with its default period.
- **Tags and cover art** are read with libavformat. Covers are embedded pictures or `cover`/`folder`/`front` images in the folder. ffmpeg decodes them (gdk-pixbuf here has no JPEG loader), scales them once to the panel's size, and they are drawn at the screen's 2x scale.
- **The library** is a SQLite database (WAL) with an FTS5 index kept in sync by triggers. The scanner walks the folders on a thread, reads only files whose size or time changed with half the cores, and writes in batches of 500. A cue sheet replaces the file it describes in the library. Tags shared by many tracks (artist, album, genre, date, codec) are stored once in memory.
- **Playlists** are a custom GTK tree model over an array of track pointers, with fixed-height rows. Cells are formatted only when drawn, so 20,000 rows cost one pointer each.
- **Audio on demand:** the first play runs `audio ensure`, which starts PipeWire on this laptop. If PipeWire is restarted during playback, audio comes back by itself.
- **No icon theme** is installed here, so the transport icons are drawn with cairo in the theme's text color.

## Measurements

On the target laptop, on battery, backlight 70%. Power is the battery's own reading over 30 s, for the whole laptop. Wakeups count all of the program's threads.

| | |
|---|---|
| Laptop idle, music-player closed | 4.1 W |
| Window open, not playing | 3.9 W (no change), 0 wakeups/s, 0% CPU |
| Playing FLAC | 4.6 W (+0.5 W), 57 wakeups/s, 1.2% of one core |
| Paused | 4.0 W, 0 wakeups/s |
| Memory: window open, empty library | 36 MB PSS |
| Memory: 20,000-track library | 40 MB PSS |
| Memory: 20,000-track library and a 20,000-track playlist | 48 MB PSS |
| Memory added while playing (mpv) | about 16 MB |
| Start to ready, with the 20,000-track playlist | 0.18 s |
| First scan of 20,000 files | 1.39 s |
| Rescan of 20,000 unchanged files | 0.12 s |
| A 20,000-file folder into a playlist | 0.42 s |
| Search over 20,000 tracks | 1–12 ms |

The plan's target was 30 MB. GTK3 itself (its libraries, theme and fonts) is most of the 36 MB, so it isn't reachable with GTK.

## Verification

- `meson test -C build` after `tests/make-testdata.sh build/testdata`, which generates FLAC, Opus, AAC and WAV files, a cue sheet and covers with ffmpeg:
  - **format:** 29 title-format cases and a duration check against foobar2000's behavior (missing fields, `[...]` blocks, `$if`, `$if2`, `$num`, track artist only when it differs, file-name fallback).
  - **library:** a scan finds 15 tracks. It checks tags, track/total numbers, codec, rate and bit depth, ReplayGain values, and Opus R128 converted to ReplayGain (−1280 → 0.00 dB). Cue tracks get their ranges and tags (performer per track), and the cue sheet's image file is hidden. It also checks embedded and folder covers, prefix search, an unchanged rescan reading nothing, a touched file read again, and playlist order surviving a reopen.
  - **playback:** the three parts of a 30-second tone, queued like the app does, come out of mpv (writing raw PCM) identical to the unsplit original: 1,323,000 of 1,323,000 frames, no gap or overlap. A track tagged −5.21 dB plays 5.18 dB quieter with track gain on (16-bit rounding).
- **In the window,** driven with `music-player-ctl` and screenshots of the real window (`tests/xshot.c`):
  - transitions, next and previous, cue tracks 1 → 2 → 3;
  - shuffle plays all 15 tracks once before repeating, shuffle albums moves album by album;
  - repeat track; stop after current stops and clears itself;
  - search, and every library view (genre, year, folder) expanded;
  - the preferences, columns and equalizer dialogs.
- Wakeups and CPU per thread were measured idle, playing and paused. They found and fixed a cover-panel resize loop that kept the program busy while paused.
- **Not verified:** a real music collection (none on this machine); DSD, APE and WavPack files (ffmpeg here can't encode them for tests; playback is ffmpeg's and mpv's own).

## Limits

- Linux with X11 only; built for one machine (`-march=native`).
- Local files only: no streaming services, no tag editing, no scrobbling yet.
- The equalizer and ReplayGain change the samples; for bit-perfect output keep the volume at 100% and turn both off.
- Following the sample rate writes a PipeWire drop-in (`~/.config/pipewire/pipewire.conf.d/60-music-player-rates.conf`) and takes effect the next time audio starts.

## License

MIT (see `LICENSE`).
