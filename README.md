# vgz2flac

Turns VGM/VGZ chip-music rips (the files on [vgmrips.net](https://vgmrips.net)) into
high-quality FLAC files that play on practically anything: **48 kHz, 24-bit stereo FLAC**,
tagged with the title, game, composer and date from the rip.

On Windows, open `vgz2flac.exe`, drag `.vgz` files or whole folders into the list and click
**Convert**. Each song is written as `<name>.flac` next to the original, or into a folder
you choose.

Also reads `.vgm`, `.s98`, `.dro` and `.gym`.

## Why the output is as good as it gets

A VGM file isn't audio: it's a log of the commands a game sent to its sound chips. The
quality of the FLAC depends entirely on how that log is turned back into sound.

- **Most accurate chip emulation.** Wherever libvgm has a Nuked core (built from photos of
  the real chips' silicon), it's used: Nuked OPN2 for the Mega Drive/Genesis YM2612 (in
  YM2612 mode, with its "ladder effect"), Nuked OPM for the YM2151, Nuked OPLL for the
  YM2413 and Nuked OPL3 for the YM3812/YMF262. Other chips use libvgm's defaults.
- **No cheap resampling.** Players like VGMPlay run every chip at its own sample rate (the
  YM2612 makes 53,267 samples a second, the PSG far more) and squeeze them all to the
  output rate with a simple linear resampler, which softens the top end. vgz2flac renders
  each chip at its exact native rate instead, converts it with SoX's "very high quality"
  resampler, then mixes the chips at the same balance VGMPlay uses.
- **Full precision until the end.** Mixing, the loop fade and the clipping guard happen in
  64-bit floating point. The result is rounded once, to 24 bits.
- **Bit-perfect file.** libFLAC encodes with verification on (it decodes every block and
  compares), and the file only appears once it's complete.
- **Plays almost everywhere.** 48 kHz/24-bit FLAC works on Windows, macOS, Android, iOS
  (Files app, VLC), foobar2000, VLC and most car stereos and music players. The chips' own
  odd rates often don't. If an old device can't do 24-bit, use `--bits 16`.

Looping songs play the loop twice, then fade out over 8 seconds (VGMPlay's curve). Songs
that end on their own play to the end.

The level isn't normalised: the FLAC has the same loudness as the rip in VGMPlay. Only if
the mix would clip is it lowered, just enough to put the loudest peak at -0.1 dBFS (the
report says when that happens).

## Usage

### Windows window app (`vgz2flac.exe`)

- Add songs by dragging files or folders into the list (folders are searched including
  subfolders), with **Add files** / **Add folder**, or by dropping them onto the .exe icon.
- Choose where to save: next to each file, or in one folder.
- Options: loops before the fade, fade length, 24- or 16-bit, replace existing FLACs.
- **Convert** turns into **Cancel** while it runs. Each file shows its progress and result.
  Double-click a finished file to show it in Explorer.

### Command line (`vgz2flac-cli.exe` on Windows, `vgz2flac` on Linux)

```
vgz2flac-cli [options] <file or folder> [more files or folders...]
```

| Option | Meaning |
| --- | --- |
| `-o`, `--out <folder>` | write the FLAC files into this folder instead |
| `--rate <hz>` | output sample rate (default 48000) |
| `--bits <16\|24>` | output bit depth (default 24; 16 adds TPDF dither) |
| `--loops <n>` | times the looping part plays before the fade (default 2) |
| `--fade <seconds>` | fade-out length for looping songs (default 8) |
| `--overwrite` | replace FLAC files that already exist (default: skip them) |
| `--default-cores` | use libvgm's default cores instead of the Nuked ones |
| `--single-pass` | let libvgm mix all chips with its own resampler (for comparison) |

Folders are searched recursively.

For each song the report shows the length, the loudest peak, and every chip with the core
and native rate it was rendered at, plus any warnings from libvgm.

Some arcade/computer rips need extra data files (for example `yrw801.rom` for the OPL4).
Put them next to the song or next to `vgz2flac.exe`.

## Building

The program is C++17 and needs CMake 3.24 or newer and git. While configuring, CMake
downloads the source of [libvgm](https://github.com/ValleyBell/libvgm) (fixed commit),
libFLAC 1.4.3, libsoxr 0.1.3 and zlib 1.3.1 and compiles them into the program, so no
package manager is needed and the result is a single executable.

### Windows (Visual Studio 2022)

From a "Developer PowerShell for VS 2022" (or with CMake on the PATH):

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The programs are `build\Release\vgz2flac.exe` (window) and `vgz2flac-cli.exe` (command
line). They only need the standard Visual C++
runtime, which nearly every Windows PC already has.

### Linux

```sh
sudo apt install cmake g++ git
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

### GitHub

`.github/workflows/build.yml` builds Windows and Linux binaries on every push and uploads
them as workflow artifacts. Pushing a tag such as `v1.0.0` also creates a GitHub release
with both attached.

## Limits

- A VGM records each chip write on a 1/44,100-second grid, so drum samples streamed to a
  chip's DAC are timed to that grid. That's built into the rip, and no renderer can undo it.
- Output is only as accurate as libvgm's cores. The Nuked cores are the most accurate
  available; chips without one use libvgm's defaults.

## Licence

vgz2flac's own code is GPL-2.0-or-later. Binaries also contain libvgm, whose sound cores
come under their own licences (including GPL and LGPL), plus libFLAC (BSD) and libsoxr
(LGPL).
