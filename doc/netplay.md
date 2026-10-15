# Rollback netplay

This fork of mGBA runs Game Boy Advance games for online play with rollback netcode. Rollback netcode lets each player's computer run the game without waiting for the other players' inputs: it predicts them, and corrects the game when they arrive. Two kinds of multiplayer are covered:

- **Link-cable games**, where every player has their own GBA and two to four GBAs talk over a link cable. Each player's computer emulates every player's GBA, plus the cable between them. Only the players' button presses travel over the network.
- **Shared-GBA games**, where several players control one GBA. Usually this means a game patched to read extra players' keys from memory, such as the p2join patch for *Super Street Fighter II Turbo Revival*.

The netplay core consists of a deterministic machine of linked GBAs inside the emulator library, game profiles, and the connection to GekkoNet, a rollback netcode library. A headless test program runs it. mGBA-qt's netplay window plays it with every player on one computer, or online: players find each other through a Nakama game server, in the window itself or in the Nakama Matchmaker launcher, and then connect to each other directly. This document covers the core; [netplay-client.md](netplay-client.md) covers the window, the online service and what remains to be built. The core is a port of gbanet, an earlier prototype of the same design written in Go on mGBA 0.10 with the GGPO-Go rollback library. Everything below was built and tested on Linux only.

## Contents

| Path | What it is |
|---|---|
| `include/mgba/internal/gba/sio/linkmachine.h`, `src/gba/sio/linkmachine.c` | The link machine: 1 to 4 GBAs, the link cable between them, whole-machine savestates and checksums |
| `include/mgba/feature/netplay.h`, `src/feature/netplay/` | Profiles and their identity, the GekkoNet session around a machine, and a UDP network adapter for GekkoNet that can add latency, jitter and packet loss for tests |
| `src/third-party/gekkonet/` | GekkoNet (BSD 2-Clause license) with its serialization header, zpp_bits (MIT license) |
| `src/platform/test/netplay-main.c` | `mgba-netplay-test`, the test program |
| `src/platform/qt/netplay/` | mGBA-qt's netplay window, the thread that runs a session for it, and `mgba-netplay-online-test`, a headless player of online matches ([netplay-client.md](netplay-client.md)) |
| `src/third-party/netplay-online/` | netplay-online (MIT license): the Nakama client, the direct connections between players and the launcher's handoff, shared with the Nakama Matchmaker launcher |
| `tools/netplay/profiles/` | Example profiles |
| `tools/netplay/testrom/` | `linkstress.gba`, a test program that keeps the link cable busy, with its source and build script |
| `tools/netplay/run_test.sh` | The test suite |
| `tools/netplay/run_online_test.sh` | The online test, against a Nakama server ([netplay-client.md](netplay-client.md)) |

## How it works

### Rollback

Every player's computer runs its own copy of the game. Every frame, each computer runs the game with all players' inputs. When another player's input for a frame has not arrived yet, the computer predicts it by repeating that player's last input. When the real input arrives and differs, the computer loads a savestate from before that frame and runs the frames since then again with the correct inputs; it *re-simulates* them. This needs a deterministic game: the same inputs from the same state must lead to the same state on every computer.

GekkoNet exchanges the inputs, predicts, decides when to roll back, and keeps the peers' frames in step. It drives the game through events: save the state, load a state, or advance one frame with given inputs. With every saved state, the game hands GekkoNet a checksum of that state. Peers exchange the checksums of frames whose inputs are confirmed, and a mismatch means that their games have diverged, which is called a *desync*.

Two of GekkoNet's session types are used:

| Session | Use |
|---|---|
| Game session | One per player in an online match |
| Stress session | A test that needs no network. Every frame, it loads the state from 7 frames earlier, re-simulates the 6 frames since then, and compares each frame's checksum with the one from the first time that frame ran. Any non-determinism shows up as a desync. |

GekkoNet also has spectator and replay sessions, which nothing uses yet.

### Why every computer emulates every GBA

Linked games exchange data over the cable every frame, often many times per frame. *Super Street Fighter II Turbo Revival* makes about nine transfers per frame in its V.S. mode, each exchanging a 16-bit word between the GBAs. During a race, *Mario Kart: Super Circuit* sends each GBA's button state once per frame, and every GBA computes the whole race from all players' buttons. Each exchange needs all GBAs at the same point in emulated time; mGBA's own link play between windows synchronizes the GBAs every 4,096 CPU cycles, about 69 times per frame. Over the internet, every exchange would wait for a network round trip. The link machine avoids that: every computer runs all the GBAs and the cable locally, so cable traffic never leaves the computer. Only each player's keys travel over the network, once per frame, which is all rollback needs.

### The cable

The emulated cable is the official GBA link cable (the multiplay cable). In this document and in profiles, GBAs are numbered from 1; the C API counts from 0. GBA 1 sits at the cable's small plug, the other GBAs at the large plugs. The behavior follows GBATEK, the standard technical reference for the GBA hardware, and mGBA's own link emulation:

- **Multiplayer mode:** GBA 1 is the parent (multiplayer ID 0), and GBAs 2 to 4 are children with IDs 1 to 3. The "all ready" line is high while every GBA is in multiplayer mode. Only the parent starts transfers, and only while every GBA is ready. A transfer delivers every GBA's 16-bit word to all of them, with 0xFFFF in place of missing GBAs. Transfer times come from mGBA's table, for example 5,755 cycles for two GBAs at 115,200 baud.
- **Normal mode:** data moves one way along the cable, because the small plug's input line is grounded. Each GBA receives the outgoing bits of the GBA before it, and GBA 1 receives zeros. A GBA takes part in a transfer when it drives the clock, or when it is armed with an external clock in the same 8-bit or 32-bit width. A transfer takes 8 or 64 cycles per bit, for the 2 MHz or 256 kHz clock.

UART mode, the JOY bus and general-purpose mode are not connected between GBAs.

### The machine

The link machine runs all its GBAs on one thread. Each machine frame is 280,896 CPU cycles, one GBA video frame, and starts at the beginning of GBA 1's vertical blank (the pause between two video frames). Frames are cut into slices of 2,000 cycles, and in every slice the GBAs run one after another to the same cycle count.

When a GBA starts a serial transfer, the machine stops that GBA right after the register write. It then runs the GBAs that have not reached that cycle up to it, and takes each GBA's outgoing data as it is at that cycle. Completion, with the received data and the serial interrupt, is scheduled on every participating GBA at the start cycle plus the transfer time. The order of events depends only on emulated cycles, never on the host's threads or timing, so the machine is deterministic.

The machine attaches to each GBA as its serial driver, through the same `GBASIODriver` interface that mGBA's own link drivers use (lockstep between windows, and the connection to the Dolphin GameCube emulator). It completes transfers itself instead of through the core's completion event, which mGBA savestates do not hold.

There is one approximation. In Normal mode, a GBA other than GBA 1 can drive the transfer clock. The GBAs before it in the running order have then already run past the start cycle. They contribute their data as it is when the transfer is processed, at most one slice later, and receive their completion at once if its time has passed. The machine counts these deliveries as "late"; none occurred in the tests below.

### Savestates and checksums

A machine savestate holds each GBA's mGBA savestate, plus what mGBA savestates (in this 0.11 development version) leave out or restore differently from how the game left it:

- the multiplayer send register, SIOMLT_SEND, which mGBA savestates do not hold;
- the live values of the serial control registers SIOCNT and RCNT, which mGBA restores by writing them through the register write path;
- the save memory contents, so that a game that writes its save during a rolled-back stretch is restored (mGBA savestates hold the save chip's state but not its contents);
- the link's transfers in flight, with their data and completion times.

A state takes about 516 KiB per GBA: 1,056,920 bytes for two GBAs.

The checksum covers each GBA's work RAM (288 KiB) and the link state. A divergence elsewhere, for example in video memory or CPU registers, is detected once it reaches work RAM. The checksum is a 64-bit hash with the structure of xxHash64, folded to 32 bits; it takes about a quarter of the time of zlib's CRC32 over the same data. GekkoNet asks for a checksum with every saved frame, re-simulated frames included, so its cost counts in every rollback.

### Drawing

Re-simulated frames are not drawn, because drawing has no effect on emulation. For the same reason, a client can draw only the GBAs whose pictures it shows (`GBALinkMachineSetDrawMask()`), such as its own player's GBA in a fighting game, where every GBA shows the same fight; the other GBAs are still emulated in full. The machine skips drawing through mGBA's frame-skip counter, which savestates do not hold. In the peer tests below, each peer re-simulates different frames, which it leaves undrawn, and all peers stay identical.

A GBA's picture (`GBALinkMachineFramebuffer()`) is 240×160, or larger for a ROM hack that declares widescreen margins ([widescreen.md](widescreen.md)). `GBALinkMachineVideoSize()` returns each GBA's size, which can differ between the GBAs of one machine and stays the same for a whole session; clients take it from the machine instead of assuming 240×160.

### Clocks, BIOS and settings

Cartridge clocks start at a fixed time (2001-01-01 by default) and advance with emulated time only, so games with a real-time clock stay deterministic. Every GBA uses mGBA's built-in BIOS replacement and default settings: the machine reads neither the user's configuration nor a BIOS file, since either would have to be identical on every peer. The one setting that the machine changes is widescreen, which it turns on for every GBA; a ROM's declaration alone then decides the margins, the same on every peer. Save data stays in memory; the machine never writes save files.

## Profiles

A profile is an INI file that describes one netplay setup: the GBAs with their ROMs and saves, whether they are linked, where each player's keys go, and optionally scripted keys from power-on. Every peer must load an equivalent profile.

```ini
[profile]
name = ssf2tr-vs
gbas = 2
link = 1
rom = ${SSF2TR_ROM}
save = ${SSF2TR_SAVE}

[player.1]
gba = 1
input = keypad

[player.2]
gba = 2
input = keypad

[intro]
1 = 600
2 = 3: START,START
```

| Section and key | Meaning |
|---|---|
| `[profile]` `name` | Name shown in messages |
| `[profile]` `gbas` | Number of GBAs, 1 to 4 |
| `[profile]` `link` | `1` joins the GBAs with the link cable (2 or more GBAs) |
| `[profile]` `rom`, `rom.N` | ROM of every GBA, or of GBA N. `${VAR}` reads an environment variable; relative paths start from the profile's folder. |
| `[profile]` `save`, `save.N` | Optional save file of every GBA, or of GBA N, read once at the start |
| `[profile]` `rtc` | Optional start time of cartridge clocks: `2001-01-01T00:00:00Z` (UTC only) or milliseconds since 1970 |
| `[profile]` `screens` | Which pictures a client shows by default: `own` (the player's own GBA; the other GBAs run undrawn) or `all`. The netplay window starts with it; the test programs ignore it. |
| `[player.N]` `gba` | The GBA that player N controls |
| `[player.N]` `input` | `keypad` (the default): the player's keys go to that GBA's keypad. `mailbox`: they go into memory, for a game patched to read them. |
| `[player.N]` `mailbox`, `keysOffset`, `markerOffset`, `marker` | For `mailbox` input: every frame, the keys are written to `mailbox + keysOffset`, then `marker`, unless it is 0, to `mailbox + markerOffset` |
| `[intro]` `1`, `2`, ... | Optional scripted keys from power-on, in numbered steps. Each step is `frames` (no keys) or `frames: keys, keys, ...` with one entry per player, keys joined with `+` (`A+RIGHT`) and empty for none. The test programs play them to reach a match; the netplay window starts sessions from power-on without them. |

Players are read from `[player.1]` up to the first missing number; a profile has 1 to 4. Key names are A, B, SELECT, START, RIGHT, LEFT, UP, DOWN, R and L, in any case.

A profile's **identity** is a SHA-1 hash of everything that peers must agree on: the emulator build (project name, version and git commit), every ROM and save file, the link, the clock's start and the player mapping. Peers should only be paired when their identities are equal, and the online service's matchmaker pairs players on it. `mgba-qt --netplay-identity profile.ini` prints it. The identity leaves out the name, the `screens` default and the intro, which change nothing in the machine once a session starts from power-on.

The example profiles:

| Profile | GBAs | Needs |
|---|---|---|
| `linkstress-2p`, `linkstress-4p` | 2 or 4 linked GBAs running `testrom/linkstress.gba` | nothing |
| `wstest-2p` | 2 linked GBAs: GBA 1 runs the widescreen test program ([widescreen.md](widescreen.md)), with a 320×200 picture, and GBA 2 the link test ROM, with 240×160 | nothing |
| `ssf2tr-vs` | 2 linked GBAs in the game's V.S. mode | `SSF2TR_ROM`, `SSF2TR_SAVE` |
| `ssf2tr-hotseat` | 1 GBA with the p2join patch; Player 2 through its mailbox | `SSF2TR_P2JOIN_ROM`, `SSF2TR_SAVE` |
| `ssf2tr-p2pad` | GBA 1 running p2pad, GBA 2 the p2join-patched game, linked | `P2PAD_ROM`, `SSF2TR_P2JOIN_ROM`, `SSF2TR_SAVE` |
| `mksc-3p`, `mksc-4p` | 3 or 4 linked GBAs in a VS race of *Mario Kart: Super Circuit*, each GBA running the game (Multi-Pak link) | `MKSC_ROM` |

`SSF2TR_ROM` is the "[Bug Fix + Original Speeches]" build of *Super Street Fighter II Turbo Revival* (USA), MD5 3e4d486bbdb30748308383c5729c14d3. The p2join patch and p2pad come from the p2join package; p2pad is a small GBA program that sends Player 2's keys over the cable to the patched game. `SSF2TR_SAVE` is the save that the scripted intros were timed with (`tests/data/arcade.sav` in the p2join package); other saves can change the menus' timing.

`MKSC_ROM` is *Mario Kart: Super Circuit* (USA, game code AMKE), MD5 784a036ff1aae709e90167186639b75e. Its profiles use no save file, so the game starts with empty save memory. Other releases of the game may need their own intro timing.

The test ROM `linkstress.gba` alternates every 120 frames between multiplayer mode, with transfers back to back, and Normal 32-bit mode, with one transfer per 4,096 cycles. Every transfer mixes the received data into a running hash, and the next word sent combines that hash with the transfer count and the player's keys. The keys thus flow through the cable, and any difference between two runs spreads into the state. The ROM has no Nintendo logo, so it boots in mGBA but not on a real GBA.

## Building

The CMake option `USE_NETPLAY` (on by default) builds two static libraries, `gekkonet` and `mgba-netplay`; the link machine itself is part of libmgba. GekkoNet needs a C++20 compiler; without one, and on the 3DS, Wii, PS Vita and Switch ports, the option turns itself off. `BUILD_TEST=ON` adds the test program:

```sh
cmake -B build -DBUILD_TEST=ON
cmake --build build --target mgba-netplay-test
```

With the Qt frontend, the option also adds the netplay window to mGBA-qt and builds netplay-online, which uses Qt's Core and Network modules; with `BUILD_TEST=ON`, it adds `mgba-netplay-online-test` ([netplay-client.md](netplay-client.md)).

Builds were tested with GCC 13 on Linux only.

## The test program

```
mgba-netplay-test -m identity|synctest|peer|bench -p profile.ini [options]
```

| Mode | What it does |
|---|---|
| `identity` | Prints the profile's identity |
| `synctest` | One process plays every player in a GekkoNet stress session |
| `peer` | One player per process, over UDP; `-r` lists the other players' addresses |
| `bench` | Measures the cost of a frame with every GBA drawn, with only GBA 1 drawn and with none drawn, and of a save, a load and a checksum |

| Option | Meaning |
|---|---|
| `-f frames` | Session frames after the intro (default 1800) |
| `-s seed` | Seed of the scripted inputs (default 1) |
| `-c frames` | Stress session: rollback distance (default 7) |
| `-P player`, `-o port` | Peer: the local player (from 1) and its UDP port |
| `-r list` | Peer: the other players, for example `2=127.0.0.1:7002,3=10.0.0.5:7003` |
| `-d frames` | Peer: input delay (default 1) |
| `-L ms`, `-J ms`, `-X fraction` | Peer: added latency, random jitter up to the given time, and loss on outgoing packets |
| `-t file` | Writes a trace: one line per frame with the frame number, the checksum and each player's keys |
| `-b frames` | Bench: frames per measurement (default 600) |
| `-w gba:address:n` | Prints n halfwords at an address of a GBA (from 1) after the intro |
| `-S prefix` | Writes each GBA's picture after the intro to `prefix-gbaN.png`, to check where a profile's intro arrives |
| `-B` | Negative control: changes one byte of GBA 1's work RAM after every state load, which GekkoNet must report as desyncs |

Two peers on two computers:

```sh
mgba-netplay-test -m peer -p tools/netplay/profiles/ssf2tr-vs.ini -P 1 -o 7001 -r 2=192.168.1.20:7002
mgba-netplay-test -m peer -p tools/netplay/profiles/ssf2tr-vs.ini -P 2 -o 7002 -r 1=192.168.1.10:7001
```

Every run first plays the profile's intro from power-on. The session then plays scripted inputs: random directions and buttons that are a function of the seed, the player and the frame, never START or SELECT. Each process ends with a summary line that gives rollbacks, re-simulated frames, desyncs, the average time per frame spent in GekkoNet and the machine, link transfers and the final checksum.

`tools/netplay/run_test.sh path/to/mgba-netplay-test` runs the test suite:

1. a stress session of 1,500 frames for every profile whose files are available;
2. the negative control, which must report desyncs;
3. peers on the same computer over UDP, with added latency, jitter and loss, whose traces must match.

It writes its logs to `tools/netplay/_test` (or the folder in `$OUT`) and ends with `ALL CHECKS PASSED` or `CHECKS FAILED`.

## Using the library

A frontend runs a session as follows (error handling left out):

```c
static struct mNetplayProfile profile;   /* large: keep it off the stack */
mNetplayProfileLoad(&profile, path, err, sizeof(err));
mNetplayProfileIdentity(&profile, identity, err, sizeof(err));   /* must equal the peers' */
struct GBALinkMachine* machine = mNetplayProfileOpen(&profile, err, sizeof(err));

GekkoSession* gekko;
gekko_create(&gekko, GekkoGameSession);
GekkoConfig config = {
	.num_players = profile.players,
	.max_input_prediction_window = 8,
	.input_size = NETPLAY_INPUT_SIZE,
	.state_size = GBALinkMachineStateSize(machine),
	.desync_detection = true,
};
gekko_start(gekko, &config);
gekko_net_adapter_set(gekko, adapter);
/* gekko_add_actor() for every player in order: local, or remote with its address;
   gekko_set_local_delay() sets the local player's input delay */

struct mNetplaySession session;
mNetplaySessionInit(&session, machine, &profile, gekko);
/* optional: session.frameCallback runs after every frame, told whether the frame is shown
   (false for frames that GekkoNet re-simulates or runs ahead, which are not drawn) */

/* then, every frame: */
gekko_network_poll(gekko);
/* gekko_session_events(): connections, disconnections, desyncs */
uint8_t input[NETPLAY_INPUT_SIZE];
mNetplayEncodeKeys(keys, input);   /* the local player's GBA keys */
gekko_add_local_input(gekko, localPlayer, input);   /* the local player's actor handle */
int count;
GekkoGameEvent** events = gekko_update_session(gekko, &count);
mNetplaySessionHandleEvents(&session, events, count);
/* show GBALinkMachineFramebuffer(machine, gba) for the GBAs on screen */
```

Frames come 59.73 times per second, the GBA's rate. A peer that runs ahead of the others (`gekko_frames_ahead()` of 1 or more) should slow down a little; the test program then makes its frames 2% longer. `mNetplayUDPAdapterCreate()` gives a plain UDP adapter, one per process; a client that traverses NATs supplies its own `GekkoNetAdapter` on its socket.

## Results

All results come from one 2-core Linux container (Intel Xeon at 2.80 GHz), with GCC 13 and the RelWithDebInfo build type (`-O3`, link-time optimization), and with the game profiles' environment variables set.

**Stress sessions** (1,500 frames after the intro; every frame adds a state load and 6 re-simulated frames):

| Profile | GBAs | Desyncs | Link transfers (intro and session) | Time per frame |
|---|---|---|---|---|
| `linkstress-2p` | 2 | 0 | 86,714 | 6.43 ms |
| `linkstress-4p` | 4 | 0 | 70,836 | 12.70 ms |
| `wstest-2p` | 2 | 0 | none | 7.78 ms |
| `ssf2tr-hotseat` | 1 | 0 | none | 6.85 ms |
| `ssf2tr-p2pad` | 2 | 0 | 4,028 | 9.76 ms |
| `ssf2tr-vs` | 2 | 0 | 44,244 | 12.43 ms |
| `mksc-3p` | 3 | 0 | 6,883 | 17.48 ms |
| `mksc-4p` | 4 | 0 | 6,883 | 26.40 ms |

**Negative control:** with one byte of work RAM changed after every load, the stress session of `linkstress-2p` reported 666 desyncs in 120 frames.

**Peers over UDP on one computer** (each peer emulates every GBA; latency and jitter are added to every packet a peer sends):

| Profile | Peers | Frames | Input delay | Network, each way | Rollbacks per peer | Re-simulated frames per peer | Desyncs |
|---|---|---|---|---|---|---|---|
| `linkstress-2p` | 2 | 1,800 | 1 frame | 50 ms plus up to 30 ms jitter | 293, 294 | 935, 1,247 | 0 |
| `linkstress-4p` | 4 | 900 | 2 frames | 30 ms plus up to 15 ms jitter, 3% loss | 2 to 321 | 2 to 677 | 0 |
| `ssf2tr-vs` | 2 | 1,800 | 1 frame | 50 ms plus up to 30 ms jitter | 291, 293 | 880, 1,237 | 0 |
| `ssf2tr-vs` | 2 | 1,500 | 2 frames | 30 ms plus up to 15 ms jitter, 3% loss | 127, 243 | 132, 381 | 0 |
| `mksc-3p` | 3 | 1,800 | 1 frame | 50 ms plus up to 30 ms jitter | 504 to 580 | 1,965 to 2,131 | 0 |
| `mksc-4p` | 4 | 900 | 2 frames | 30 ms plus up to 15 ms jitter, 3% loss | 240 to 278 | 466 to 725 | 0 |

All peers' traces matched, apart from the last 10 frames of each run, which a peer may have simulated on predicted inputs before it stopped. Rollback counts vary from run to run with timing. In the runs with three or four peers, all processes shared two CPU cores, each of them emulating every GBA, so they could not quite keep up with real time: the four-peer race played 900 frames in about 17.6 seconds instead of 15.

**Costs per frame** (`bench` mode, median of three runs of 600 frames each, after the intro):

| Profile | GBAs | Frame, every GBA drawn | Frame, GBA 1 drawn | Frame, none drawn | Save | Load | Checksum | State size |
|---|---|---|---|---|---|---|---|---|
| `linkstress-2p` | 2 | 0.71 ms | 0.70 ms | 0.72 ms | 0.042 ms | 0.061 ms | 0.047 ms | 1,056,920 bytes |
| `linkstress-4p` | 4 | 1.35 ms | 1.31 ms | 1.33 ms | 0.115 ms | 0.152 ms | 0.100 ms | 2,113,696 bytes |
| `wstest-2p` | 2 | 0.88 ms | 0.74 ms | 0.74 ms | 0.044 ms | 0.065 ms | 0.050 ms | 1,056,920 bytes |
| `ssf2tr-hotseat` | 1 | 1.17 ms | (one GBA only) | 0.74 ms | 0.011 ms | 0.018 ms | 0.023 ms | 528,532 bytes |
| `ssf2tr-p2pad` | 2 | 1.62 ms | 1.08 ms | 1.20 ms | 0.047 ms | 0.072 ms | 0.051 ms | 1,056,920 bytes |
| `ssf2tr-vs` | 2 | 1.90 ms | 1.74 ms | 1.50 ms | 0.050 ms | 0.090 ms | 0.054 ms | 1,056,920 bytes |
| `mksc-3p` | 3 | 2.94 ms | 2.30 ms | 1.91 ms | 0.095 ms | 0.145 ms | 0.071 ms | 1,585,308 bytes |
| `mksc-4p` | 4 | 4.63 ms | 3.64 ms | 3.12 ms | 0.126 ms | 0.160 ms | 0.093 ms | 2,113,696 bytes |

"GBA 1 drawn" is what a client that shows only its own player's screen pays, except in `ssf2tr-p2pad`, where GBA 1 runs p2pad. Single runs of the frame figures varied by up to 54% on this shared machine, so differences smaller than that between columns, such as "GBA 1 drawn" below "none drawn", are noise. The link test ROM draws nothing, so its three frame figures differ only by that variation; [widescreen.md](widescreen.md) gives the cost of drawing margins, measured with longer runs.

A rollback of n frames costs one state load and n re-simulated frames, none of them drawn, each with a save and a checksum, on top of the current frame. One frame of 16.74 ms has room on this machine for 9 re-simulated frames with the two-GBA V.S. profile (8 with the slowest single-run figures), 6 with the three-GBA race and 3 with the four-GBA race, every GBA drawn. Rollbacks get deeper as the network delay between players grows beyond the input delay, because a rollback re-simulates the frames between the one that a late input was meant for and the one in which it arrived. In the two-peer V.S. runs above, rollbacks re-simulated 3 to 4 frames on average with 1 frame of input delay, and 1 to 2 with 2 frames; the three-GBA race re-simulated 3.4 to 4.2 with 1 frame of delay, and the four-GBA race 1.9 to 2.6 with 2. The races thus use much of their room at these network delays, and deeper rollbacks would briefly slow them down on this machine unless the input delay were raised. Faster CPUs raise these limits.

## Limitations

- **A first client.** The netplay window plays matches that Nakama's matchmaker makes, over direct connections without a relay; game channels, challenges, spectating, replays and ranks are not built ([netplay-client.md](netplay-client.md)).
- **Cost grows with the number of GBAs.** Every computer emulates every GBA, and a rollback re-simulates all of them, one after another on one thread. See the costs above.
- **No Single-Pak link play.** The machine needs a ROM for every GBA. In a game's Single-Pak mode, the other GBAs start without a cartridge and receive a program over the cable, through code in the GBA BIOS that mGBA's built-in BIOS replacement does not provide. For games that offer both modes, Multi-Pak play is the one to use, since every computer has the ROM anyway.
- **No accessories on the link port.** Only cables between GBAs are emulated, not the GBA Wireless Adapter, the GameCube link cable or the Mobile Adapter GB. The Mobile Adapter GB connected Japanese games to online services through a mobile phone; *Mario Kart Advance*, the Japanese release of *Mario Kart: Super Circuit*, used it for time-trial rankings and ghosts. Those services answered one player at a time from a server, so they need an emulation of the adapter and a replacement server, such as the REON project's, rather than rollback.
- **Cable model.** It follows GBATEK and mGBA's link emulation, not tests on real hardware. Only multiplayer and Normal modes are connected, with the approximation described under [The machine](#the-machine). Line levels that one GBA reads from another (multiplayer "ready", Normal-mode input) can likewise change up to one slice early or late.
- **One build on every peer.** Emulation is only guaranteed identical within one build, which the identity covers. Determinism between different CPU types (x86-64 and ARM) is expected, because the emulation uses integers only, but it is untested.
- **Fixed sensors.** Cartridge clocks start at a fixed time. Solar, tilt and gyro sensors read nothing.
- **GBA only.** Game Boy and Game Boy Color link play is not covered.
- **Own savestate format.** Machine savestates do not load as mGBA savestates.

## Sources

- GBATEK, GBA serial communication and link cable wiring: https://problemkaputt.de/gbatek.htm
- mGBA (`src/gba/sio.c`, `src/gba/sio/lockstep.c`, `src/gba/io.c`, `src/gba/serialize.c`): https://github.com/mgba-emu/mgba
- GekkoNet, vendored at commit 0d491a4524e10173fc5d2aae22d042d3ae9f2d44: https://github.com/HeatXD/GekkoNet
- xxHash, whose 64-bit variant the checksum's structure follows: https://github.com/Cyan4973/xxHash
- REON, a replacement server for the Mobile Adapter GB: https://github.com/REONTeam/reon
