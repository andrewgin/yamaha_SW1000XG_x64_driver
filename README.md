# Yamaha SW1000XG driver for 64-bit Windows 10/11

[![Donate](https://img.shields.io/badge/Donate-Stripe-635BFF?logo=stripe&logoColor=white)](https://buy.stripe.com/9B66oI90I7M2doE52senS00)

The **Yamaha SW1000XG** is a late-1990s PCI sound card. It combines a full XG
wavetable synthesizer (the same engine as the MU100R) with six stereo wave
outputs, line-level recording and a MIDI interface. Yamaha's last driver was for
32-bit Windows 2000/XP, so on any modern PC the card does nothing.

This project is a new driver that aims to run the card on **64-bit Windows 10
and 11**. It was written by reverse-engineering Yamaha's original driver and
then building a modern one from the notes, without copying any of Yamaha's
code.

> **Status: early, experimental.** The driver starts the card and plays its XG
> synthesizer from Windows MIDI software on Windows 10. Wave playback,
> recording and the mixer are not written yet. The driver is test-signed, so it
> only loads with Windows in test mode, and it can crash the PC. See
> [What works](#what-works) below.

## What is working now

Once it is installed, the card appears in Windows as "Yamaha SW1000XG (x64)",
with these MIDI devices:

- **Synth:** "SW1000 #1 Synthesizer" to "SW1000 #3 Synthesizer", the XG
  synthesizer's MIDI ports, as on XP. Programs such as MIDI-OX can play
  through them.
- **MIDI:** "SW1000 MIDI OUT" and "SW1000 MIDI IN", the card's external
  sockets (built, not yet tested; see below).

## What works

Everything marked "works" was checked on a real SW1000XG in a Windows 10 PC, in
a series of test rounds.

| Area | State |
|---|---|
| Card detection, memory and register access | works |
| DSP microcode load and start-up | works |
| Interrupts | works (MIDI sources so far) |
| XG synth through ports #1 and #2 | works, from MIDI-OX |
| Synth port #3 | listed, but its notes don't reach the synth (being investigated) |
| External MIDI IN/OUT sockets | built; next test round |
| Wave playback and recording | not started |
| Volume and mute (mixer) | not started |
| Sleep and resume | not started |
| ASIO | not planned for the first version |

## What you need

- A Yamaha SW1000XG (PCI ID `VEN_1073&DEV_1000`) in a PC with a PCI slot,
  running 64-bit Windows 10 or 11. The DS2416 and other Yamaha DSP cards are
  not supported.
- **Your own copy of Yamaha's original Windows 2000/XP driver**,
  `Yamaha_SW10000XG_Win2000-XP.zip` (it holds `yswds.sys` v1.01.0014.1). The
  card's DSP needs about 9 KB of Yamaha microcode to start. This project
  can't redistribute it, so an installer script extracts it from your copy,
  checks every piece against known hashes, and stores it in the registry. The
  driver refuses to start without it.
- Secure Boot off and Windows test mode on, until the driver is properly
  signed.

The step-by-step guide, including how to back out if anything goes wrong, is
[port/TESTING.md](port/TESTING.md). Read its warnings first: this is an
unfinished kernel driver.

## How it is being made

1. **Reverse engineering.** Yamaha's `yswds.sys` was studied with static
   analysis only. Usefully, the binary still
   carries its own symbol table, so most functions have Yamaha's original
   names.
2. **A clean-room port.** The new driver in [port/](port/) is written from
   the static analysis, not from Yamaha's code. It is a standard PortCls audio
   driver built with the current Windows Driver Kit. It fixes problems found in
   the original, such as unchecked allocations, a double mapping of the card's
   memory, and a MIDI write that could stall for five seconds when its queue
   was full.
3. **Testing on real hardware.** The driver is tested with a real card as we progress through the features.

## Repository layout

| Path | What it holds |
|---|---|
| [re/](re/) | the findings, one file per section; [symbols.csv](re/symbols.csv) (named addresses), [structs.h](re/structs.h) (recovered data structures) |
| [port/](port/) | the new driver |
| [port/BUILD.md](port/BUILD.md) | building, signing, and installing the microcode |
| [port/TESTING.md](port/TESTING.md) | the guide for testers with the card |
| [port/src/](port/src/), [port/inc/](port/inc/) | driver source |
| [port/inf/](port/inf/) | the install file |
| [port/install/](port/install/) | PowerShell scripts: install, uninstall, microcode, and the hardware tests |
| [tools/](tools/) | Python helpers: reading the original's symbol table, extracting and checking the microcode |

## Building

Build on Windows with Visual Studio 2022 and the Windows Driver Kit (which can
come from NuGet), Release x64:

    nuget restore port\packages.config -PackagesDirectory port\packages
    msbuild port\ysw1000xg.vcxproj /p:Configuration=Release /p:Platform=x64 /p:SignMode=TestSign

The details, including CI and how test builds are published, are in
[port/BUILD.md](port/BUILD.md).

## Supporting the project

If the driver brings your card back to life, a tip helps with the project's AI development costs. Your donation and contribution is very much appreciated!

## Legal

- This is an independent project, not affiliated with or endorsed by Yamaha.
  "Yamaha" and "SW1000XG" are Yamaha's trademarks.
- The repository holds **no Yamaha code or data**: no binaries, no microcode, no
  decompiled code. The notes describe what the original driver does. The new
  driver is written from them.
- The microcode comes from the user's own copy of Yamaha's driver, at install
  time. Please don't share the Yamaha download or the extracted microcode.
