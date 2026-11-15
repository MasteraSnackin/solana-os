# Solana OS C apps

From Solana OS 2.2.0 an App Store app can be written in C. The store compiles it to
WebAssembly and to the badge's native code, and the badge runs it in a sandbox next to
the Lua apps. A C app has exactly the [Lua API](LUA_API.md): the same modules, the same
permissions, the same limits (only the CPU limit is its own: "Limits"), reached through
`badge.h`, a header of typed C functions
(`ui_label()`, `timer_every()`, `http_get()`, ...). This file is the C reference and the
contract between the SDK, the store and the badge. The web shows it at `/apps/docs/c`;
the SDK (headers, examples, the build script) is in `sdk/c/` and `tools/cc/` of
[solana-os](https://github.com/solana-foundation/solana-os), and as a zip on the site.

What a function *does* (what a label looks like, when a message arrives, what an error
means) is in the Lua reference; this page says how to call it from C and what is
different. Where they disagree on behaviour, the Lua reference is right and this page
has a bug.

## C or Lua?

Choose C when the app computes a lot per frame: a game that draws every pixel itself, a
simulation, an emulator, a decoder, number crunching. A C app draws a whole frame into
its own memory and puts it on the screen in one call (`ui_canvas_blit`), and once the
store has installed it, its loops run as the badge's own machine code. Choose C, too, to
bring existing C code to the badge (a parser, a codec, game logic).

Choose Lua for everything else: it is shorter to write, the store shows reviewers less
code, and most badge apps spend their time waiting for keys and the network, where C
gains nothing. Each call into the API from C goes through the same Lua host a Lua app
uses, so an app made mostly of API calls (labels, lists, `http`) is no faster in C. On a
badge the cheapest API call took 4 µs from native code and 7 µs interpreted, and setting a
label's text 176 µs and 248 µs: the time is the API's, not the language's.

C is fast only in its native form, which only a store install brings (see "What runs
where"). Everywhere else (the emulator, Try in emulator, Run on badge) the badge
interprets the WebAssembly, and there C ran *slower* than the same work in Lua.

| | Lua app | C app |
|---|---|---|
| Source | `main.lua` + `.lua` modules | `main.c` + `.c`/`.h` files |
| API | the Lua modules | the same, through `badge.h` |
| Runs as | Lua bytecode, from flash | native code (`app.aot`) in place from flash, or the WebAssembly in WAMR's interpreter |
| Its own memory | Lua tables and strings | a WebAssembly linear memory: `malloc`, arrays, structs (≤ 4 MB) |
| Drawing | `ui.canvas` calls | the same, plus whole RGB565 frames with `ui_canvas_blit` |
| Badges | Solana OS 0.5.0+ | Solana OS 2.2.0+ |

## The package

An app is a folder, as for Lua ([LUA_API.md "The package"](LUA_API.md#the-package)):

```
starfield/
  main.c                compiled with every other .c file
  app.json              the manifest, with "lang": "c"
  lib/maze.c            compiled too
  lib/maze.h            #include "lib/maze.h"
  art/ship.png          data: ui_load("art/ship.png")
  levels/1.json         data: fs_read(), fs_json_push()
```

- `main.c` (≤ 64 KB) and `app.json`, whose `"lang": "c"` makes it a C app. Without
  `lang` the store takes the folder for a Lua app.
- Bundled files under the Lua limits: at most 64 besides `main.c` and `app.json`, each
  ≤ 256 KB, ≤ 1 MB together, with the same path rules (lowercase letters, digits, `_`,
  `-` and `.`, `/` between folders; ≤ 64 characters, ≤ 4 levels; no name starting or
  ending with `.`). `app.wasm` and `app.aot` at the top of the folder are taken: the
  badge keeps an app's code under those names, so no app, C or Lua, may bundle a file
  called that (the store refuses it; `bin/app.wasm` is fine).
- Every bundled `.c` file is compiled with `main.c` into one module; `.h` files are for
  `#include` (paths from the file that includes them, or from the app's folder). Every
  other file is data the app reads at run time with `fs_*`, `ui_load()`, `ui_image()`
  and `ui_anim()`.
- The badge never gets the C source. The store installs `/sdcard/hh/apps/<id>/`:
  `app.json`, `app.wasm` and, when the badge can run it, `app.aot`, the app's `storage`
  in `data.json`, and the data files under `files/`. Source and headers stay in the store.

The manifest is the Lua one ([LUA_API.md](LUA_API.md#the-package)) with `"lang": "c"`:

```json
{
  "id": "starfield-c",
  "name": "Starfield",
  "version": "1.0.0",
  "author": "@you",
  "summary": "Fly through the stars and catch the green ones.",
  "icon": "play",
  "accent": "9945FF",
  "category": "games",
  "lang": "c",
  "permissions": ["led"]
}
```

The store adds what it compiled to the copy a badge installs (`code`, server/API.md "C
apps"):

```json
"code": {
  "wasm": {"sha256": "<64 hex digits>", "size": 4300},
  "aot": {"target": "rv32imac-xip-wamr2.4.5-2", "sha256": "<64 hex digits>", "size": 9892,
          "sig": "<the store's Ed25519 signature, base64>"}
}
```

`aot` is there only when the store has a native form for the badge's current target (see
"Security" for `sig`).

**C apps need Solana OS 2.2.0 or later.** The store offers them only to badges that run
it, and says so in the app's details on older ones, as it does for `ble` apps and 1.0.0.

## How it is compiled

The store builds every version you publish with one pinned toolchain, and refuses a
version that doesn't build, naming the file and line as the compiler does
(`lib/maze.c:12:3: error: use of undeclared identifier 'wall'`). The same flags and
checks are in the SDK, so a local build matches the store's.

1. **C to WebAssembly.** clang from [wasi-sdk](https://github.com/WebAssembly/wasi-sdk)
   24.0 (clang 18.1.2) compiles `main.c` and the other `.c` files and links them with
   wasi-libc into one module, `app.wasm`: a *reactor* (no `main()`; the badge calls
   `_initialize` once, then `on_start`), `-O2`, unused code removed, no debug
   information.
2. **The checks.** The module may import only the functions of `badge_abi.h` and the
   few WASI calls below; must export `on_start`; must have one memory whose maximum is
   at most 4 MB; may use only the WebAssembly features the badge runs (below); and must
   be at most 1.5 MB. A module that fails one is refused, by the store when it builds
   and by the badge when it loads.
3. **WebAssembly to native code.** `wamrc` from [WAMR](https://github.com/bytecodealliance/wasm-micro-runtime)
   2.4.5 compiles `app.wasm` to RISC-V code for the badge's ESP32-C5, `app.aot`: built
   to run in place from flash (no relocations in the code), with every memory access
   bounds-checked. The store keeps it only if it fits a flash slot (at most 1 MB minus
   4 KB, 1 044 480 bytes) and is clean to run in place; otherwise the version is published
   with `app.wasm` alone and badges interpret it. The store signs each `app.aot` it serves.

The flags are data files in `sdk/c/toolchain/`, which the store reads too:

| File | Holds |
|---|---|
| `versions` | `ABI=1`, `WASI_SDK=24.0`, `WAMR=2.4.5`, `AOT_TARGET=rv32imac-xip-wamr2.4.5-2` |
| `cflags` | the compiler's flags, one per line |
| `ldflags` | the linker's (through clang) |
| `wamrcflags` | wamrc's: RV32IMAC, `-O3` with `--size-level=3`, run in place (`--xip`), bounds checks on, no SIMD or reference types, and the helpers the badge's WAMR provides for a CPU without an FPU (software floating point, 64-bit division, count trailing zeros) |
| `imports` | the functions a module may import, `module.name` per line |

`AOT_TARGET` names the native code those flags make. A badge runs an `app.aot` only if
its target is exactly the badge's own; any change to `wamrcflags` or to WAMR gets a new
target, and the store rebuilds the native forms. (`-1` named a helper wrongly, so wamrc
ignored it and code using `printf` or `qsort` wasn't clean to run in place; `-2` fixes
that.) The native code also assumes the signatures of the badge's WASI imports, so
changing one of those needs a new target too.

**WebAssembly features.** The badge's WAMR is a small embedded build: it runs the
WebAssembly MVP plus **bulk memory** (`memory.copy`, `memory.fill`), **sign extension**,
**non-trapping float-to-int** and **mutable globals**, and nothing else: no SIMD, threads
or atomics, reference types, multi-value, exceptions, tail calls, extended constants,
memory64 or several memories. The cflags turn exactly those four on and the rest off,
and the check decodes every instruction of the module, so an `__attribute__((target("simd128")))`
or inline assembly is refused too. wasi-sdk 24 is the newest release whose wasi-libc stays
inside that set: from 25 on, its libc and compiler-rt are built with reference types,
whose longer `call_indirect` encoding this WAMR build refuses.

**Memory.** 32 KB of stack for C's locals, placed first in memory so an overflow traps
instead of corrupting data. The memory starts as small as the app's data and stack need
(64 KB for most of the examples, 128 KB for `starfield` with its 34 KB frame buffer) and
grows as `malloc` asks, up to 4 MB, where `malloc` returns `NULL` (see "Limits" for the
badge's own cap).

### Building locally

`tools/cc/fetch.sh` downloads the pinned toolchain (wasi-sdk and wamrc, each checked
against a SHA-256 in the script) into `tools/cc/.toolchain/` (macOS on Apple Silicon,
where wamrc runs under Rosetta 2, and Linux x86_64). `tools/cc/badge-cc` builds an app
folder the store's way:

```sh
sh tools/cc/fetch.sh                                  # once, ~220 MB
tools/cc/badge-cc sdk/c/examples/hello -o hello.wasm  # compile, link, check
tools/cc/badge-cc myapp -o app.wasm --aot app.aot     # and the native code, to check it builds
tools/cc/badge-cc --check app.wasm --report           # the checks alone; imports, exports, features
```

It prints the compiler's messages with paths from the app's folder, warns about what
the store would refuse in the package (too many files, bad paths, no `"lang": "c"`), and
exits 1 when the build or a check fails. The `app.aot` it makes is only a check that
wamrc accepts the module: a badge takes native code from the store alone (see
"Security").

## What runs where

| Where the app runs | What runs | Limit per callback |
|---|---|---|
| A badge, installed from the App Store (listed or unlisted) | `app.aot`, native, in place from flash | 200 ms of native time |
| A badge, installed from the store without a native form (none for its target, or one that doesn't verify) | `app.wasm` in WAMR's interpreter | 2 000 000 WebAssembly instructions |
| A badge, from Run on badge or the serial console | `app.wasm`, interpreted | 2 000 000 instructions |
| The emulator, Try in emulator | `app.wasm`, interpreted | 2 000 000 instructions |

A store install always writes `app.wasm`, and adds `app.aot` when its target is the
badge's own, it fits a flash slot and the store's signature verifies. At launch the badge
copies the native form into a 1 MB slot of its flash (unless it is there already) and runs
it from there, as it does with a Lua app's compiled code: no native code is ever copied
into RAM. At every launch it checks the signature again and that the slot holds exactly
the signed file, and runs `app.wasm` if anything fails ("Security"). The app behaves the
same either way (the same API, messages and memory cap); only the speed and the CPU limit
differ.

Measured on a prototype badge (ESP32-C5 rev 1.2, Solana OS 2.2.0, USB power, Wi-Fi off):

| | Native (`app.aot`) | Interpreted (`app.wasm`) |
|---|---|---|
| Signature check, every launch | 57 to 76 ms | |
| Copy into a flash slot, first launch only | 34 to 58 ms for 1 to 4 KB, 290 to 380 ms for 46 to 76 KB | |
| Later launches: map the slot and hash it (46 KB) | 3.6 to 3.8 ms | |
| `lua run` until `hello` runs | about 460 ms the first time, about 120 ms after | about 90 ms |
| The starfield example: drawing one frame | 7 ms | 33 ms |
| The starfield example: frames a second | 19.2 to 19.8 (the screen is the limit) | 12.6 to 13.2 |

Earlier, on the same badge (Solana OS 2.1.0 with a test build of WAMR 2.4.5), with a
synthetic test module (clang `-O2`, no libc), times in ms:

| Benchmark | C compiled into the firmware | Native (`app.aot`) | Interpreted (`app.wasm`) | Lua |
|---|---|---|---|---|
| Mandelbrot 320 × 240, fixed point, 32 iterations | 27.4 | 36.8 | 3 111 | 1 292 |
| Mandelbrot with floats (software floating point) | 903 | 1 169 | 3 691 | |
| Sieve of 64 K, 4 times (in PSRAM) | 645 | 642 | 3 298 | 2 650 |
| SHA-256 of 16 KB | 5.8 | 11.4 | 410 | |

The native column used wamrc with run-in-place flags like those in `wamrcflags`, the interpreted one the interpreter the
badge runs (WAMR's classic interpreter, with instruction counting). The native form ran at
1.0 to 2.0 times the time of C built into the firmware. On integer work it was 36 to 85
times faster than the interpreter; where the time went to software floating point or to
PSRAM, 3 to 5 times. Interpreted, the same work took longer than in Lua.

On 2.2.0 the interpreter ran a plain loop at about 4 500 WebAssembly instructions a
millisecond, so its 2 000 000 a callback are about 0.45 s of such a loop. Code heavier in
memory accesses and calls runs fewer a millisecond (the benchmarks above ran 2 170 to
2 865 on the 2.1.0 test build), so it takes longer to reach the count. The
emulator runs the same interpreter and counts the same instructions: `lua mem` there prints
the most a callback of your app used ("most wasm instructions in one callback"), which is
what a badge would count.

For an author that means: test the logic and the UI in the emulator or with Run on badge,
interpreted; to see how fast the app really is, install it from the store (an unlisted
app is enough), and check with `lua mem` on the badge that it ran `app.aot (native)`.

## Lifecycle

The badge loads the module, calls `_initialize` (wasi-libc's set-up: static
constructors), then these exports when they are defined. Only `on_start` is required;
`badge_abi.h` declares them all with their export names, so defining one is enough.

```c
void on_start(void);                      // once, at launch: build the UI here
int32_t on_key(int32_t key, int32_t repeat); // KEY_UP ... KEY_BACK; return 1 if handled
void on_tick(int32_t ms);                 // every 30 ms; ms since the last tick
void on_mesh(void);                       // a message from this app on another badge ("mesh")
int32_t on_meet(void);                    // another badge asks for a meet ("mesh")
void on_exit(void);                       // the app is closing (HOME, CANCEL, sys_exit)
```

They are the Lua globals of the same names ([LUA_API.md "Lifecycle"](LUA_API.md#lifecycle)),
with the same timing and rules:

- `on_key(key, repeat)`: `key` is `KEY_UP`, `KEY_DOWN`, `KEY_LEFT`, `KEY_RIGHT`,
  `KEY_SELECT` or `KEY_BACK` (CANCEL), `repeat` 1 for an auto-repeat. Return 0 for
  `KEY_BACK` to exit. HOME always returns to the launcher and never reaches the app. A
  focused list, `ui_confirm` or `ui_input` takes the keys it uses first.
- `on_tick(ms)`: move things by `ms`, never by counting ticks. For several keys at once
  read `sys_keys()` here (`KEY_HELD(sys_keys(), KEY_LEFT)`).
- `on_mesh()`: the sender's badge id and the message are its arguments on the stack:
  `badge_arg(1)` and `badge_arg(2)` (see "Values on the stack").
- `on_meet()`: `badge_arg(1)` is who asks, `badge_arg(2)` their data. To sign, return
  `mesh_meet_accept("{\"caught\":1}")`; return 0 to refuse.
- `on_exit()`: runs when the app closes normally (HOME, CANCEL, `sys_exit()`). It is
  not called after the app stopped on an error (its memory can't be trusted then) or
  after `exit()`, which ends the app at once.

The screen below the status bar is `UI_W` × `UI_H` (320 × 218) pixels, (0, 0) at the
top-left. A runtime error (a trap: an out-of-bounds access, a division by zero,
`unreachable`, `abort()`), `badge_fail()`, a mistake in using the ABI, running out of
memory or out of time stops the app with the error screen ("When an app stops"); CANCEL
returns to the launcher. An API call the badge refuses doesn't stop it: the wrapper
returns `BADGE_ERR` and `badge_error()` says why.

Callbacks never run at the same time: one export or callback at a time, each to the end.
There are no threads.

### Limits

Everything in [LUA_API.md "Limits"](LUA_API.md#limits) holds for a C app too (UI objects,
label text, images, timers, storage, files, reads, mesh, Bluetooth, HTTP, RPC, WebSocket,
crypto), plus:

| | |
|---|---|
| Memory | One cap, the Lua app's: 6 MB in PSRAM. It covers the app's linear memory (≤ 4 MB: its data, its 32 KB stack and `malloc`), WAMR's copy of the module (interpreted; native code stays in flash), its instance and its own 32 KB stack, and the Lua state behind the API (labels' text, loaded images, tables you get). `sys_memory()` reports the sum. When the cap would be passed by growing the linear memory, the growth is refused: `malloc` returns `NULL` and the app runs on. Anything else past it stops the app with "Out of memory". Measured in the emulator: the SDK's examples run with 121 to 221 KB in all |
| The badge's memory | Measured on a badge (Solana OS 2.2.0, Wi-Fi off): 125.4 KB of SRAM free for `malloc` and 8.36 MB of PSRAM at the launcher; with `hello` running, 110.2 KB and 8.24 MB native, 112.0 KB and 8.22 MB interpreted. Twenty native launches and exits left the free heap (157 KB) and its largest block (96 KB) as they were |
| CPU, interpreted | 2 000 000 WebAssembly instructions per callback (about 0.45 s of a plain loop on a badge, "What runs where"). A callback's nested calls into the app share it (a list's `on_select` called from inside `on_key`, a C function called through `sos_call`), and so do `_initialize` and `on_start` at launch. `memset`, `memcpy` and `memmove` (WebAssembly's `memory.fill`, `memory.copy` and `memory.init`) cost one instruction per 4 bytes, so a callback moves at most 8 MB with them: on a badge `memset` writes about 12 KB a millisecond and `memmove` about 8 KB, while the interpreter runs 2 200 to 4 500 plain instructions, so 4 bytes take about an instruction's time. Past it: "App took too long". Measured: stuck apps stopped at exactly 2 000 000, with no watchdog warning; a loop of 3.75 MB `memset`s 0.96 s after launch and of `memmove`s 1.27 s (both counting 0.3 s to grow the memory first), of 64 KB `memory.init`s 1.0 s |
| CPU, native | 200 ms of native time per callback, shared the same way. The app's own code counts, and so do `memset`, `memcpy` and `memmove` (the badge runs them in 4 KB pieces and stops between two); time inside the badge's API functions and while the linear memory grows doesn't (the API's work is bounded by the host's steps, below). Past it: "App took too long". Measured: a plain loop stopped after 204 to 210 ms of native time, a loop calling an API function after 204 to 210 ms, a spinning callback nested inside an API call after 203 to 207 ms, a loop spending its time in the software floating-point helpers after 207 to 295 ms, a loop of 3.75 MB `memset`s or `memmove`s after 200 to 201 ms, of `memory.init`s after 207 ms |
| Host work | What the API functions do for the app counts against the host's 200 000 steps a callback, as for a Lua app, in both forms: reading files, `crypto`, drawing on a canvas, `ui_canvas_blit`, and the calls themselves. Every call into the badge costs 1 step and `sos_call` 4 more (a Lua call's own instructions); what one copies costs 1 step per 4 bytes (strings in and out: `sos_push_str`, `sos_to_str`, the keys of `sos_get` and `sos_set`); console output (`printf` and `badge_print()`, or Lua's `print` reached through the API) 32 steps a byte, since the console is a 115 200-baud serial line that writes about 11 bytes a millisecond, so a callback prints about 6 KB; `arc4random()` and `getentropy()` 6 steps a byte. Past it: "App took too long". Measured on a badge: a loop pushing 2.5 MB strings stopped at the first one (nothing copied), loops of 1 KB `fd_write`s or `sos_log`s after 6 KB of output, a 64 KB `print` before any of it, and the SDK's API probe (92 checks and their output in one `on_start`) used 119 886 steps |
| One callback, whatever it does | 3 s of wall-clock time, in both forms. The limits above stop an app long before; this is the backstop for anything they miss. Past it: "App took too long". Measured with those limits raised in a test build: a plain native loop, a native `memset` loop, native and interpreted loops calling the badge, and an interpreted `memset` loop each stopped 3.0 to 3.3 s after the callback began |
| Stack | 32 KB of the linear memory for C's locals. Interpreted, the function calls themselves use WAMR's 32 KB stack (in the app's heap); native, the badge's 12 KB task stack, shared with the badge's code that calls the app, so deep recursion stops sooner there. Measured with `hello` running: 7.2 KB of the task stack left native, 7.0 KB interpreted; at the deepest nesting of callbacks tested, 2.4 KB native, 3.8 KB interpreted |
| Module | `app.wasm` ≤ 1.5 MB. The native form was 2.2 to 3.0 times the size of the module for the SDK's examples (`hello`: 21 KB of WebAssembly, 46 KB native); it must fit a 1 MB flash slot, or the store publishes `app.wasm` alone |
| The stack frame | 64 values per frame (see "The raw ABI"), 1024 kept values (`sos_ref`) at a time |
| Callbacks waiting | 64 in `badge.h` (timers, server calls, lists, sockets, ...); past that a call fails with "badge.h: more than 64 callbacks waiting" |
| Numbers | C has 64-bit integers and doubles in its own memory; values crossing into the API are 32-bit integers and 32-bit floats, as in Lua. The badge's CPU has no floating-point unit: floats work, slowly (the Mandelbrot rows of "What runs where"); keep hot loops in integers |

### When an app stops

The error screen's title and message, as the serial console also prints them
(`[lua:<id>] <title>: <message>`):

| What happened | Title | Message |
|---|---|---|
| A load or store outside the linear memory (a bad pointer, C's stack overflowing its 32 KB) | App error | The app read or wrote outside its memory (out of bounds memory access) |
| Integer division by zero | App error | The app divided by zero (integer divide by zero) |
| `INT_MIN / -1` | App error | Integer overflow in a division (INT_MIN / -1) (integer overflow) |
| A float too big for the integer it is converted to | App error | A float too big for an integer (invalid conversion to integer) |
| Recursion too deep for WAMR's stack (interpreted) | App error | The app ran out of stack (recursion too deep) (wasm operand stack overflow) |
| The badge task's stack running out (callbacks nested through `sos_call`; native code's own recursion) | App error | Callbacks nested too deeply (native stack overflow) |
| A function pointer that points nowhere (0, garbage) | App error | The app called a bad function pointer (uninitialized element), or (undefined element) past the end of the function table |
| A function pointer of the wrong type | App error | The app called a function pointer of the wrong type (indirect call type mismatch) |
| `abort()`, `__builtin_trap()`, a failed `assert` | App error | The app crashed (unreachable code: abort(), __builtin_trap()) (unreachable) |
| `badge_fail("...")` | App error | your text ("The app stopped" for an empty one) |
| An index outside the frame | App error | sos_to_int: index 5 is outside the frame (1 values) |
| A pointer or length outside the linear memory, given to the badge | App error | sos_push_str: the pointer 0x7ffffff0 (100 bytes) is outside the app's memory |
| More than 64 values in a frame | App error | sos_push_int: more than 64 values in a frame |
| A value given to `sos_push_fn` that isn't a C callback | App error | sos_push_fn: no function there (function 12345) |
| A kept value that isn't one | App error | sos_unref: 77 is not a kept value |
| An import the badge doesn't have | App error | The app imports env.launch_rockets, which the badge doesn't have |
| Built for a newer ABI | App error | This app needs a newer Solana OS (its C ABI is 2, this badge's 1). Update in Settings > Software update. |
| No `on_start`, or an export of the wrong type | App error | The app has no on_start / The app's on_key has the wrong type (see badge_abi.h) |
| A start function (the SDK never makes one; WAMR would run it with no limit while it loads the module) | App error | The app has a start function, which the badge doesn't run (rebuild it with the SDK) |
| An export WAMR would run by itself at load, `__wasm_call_ctors` or `__post_instantiate` (the SDK's constructors run from `_initialize`) | App error | The app exports __wasm_call_ctors, which the badge doesn't run (rebuild it with the SDK) |
| Native code put on the card as `app.wasm` | App error | app.wasm is native code, not WebAssembly: only the store's signed app.aot runs natively |
| An `app.wasm` that isn't WebAssembly at all | App error | app.wasm isn't WebAssembly |
| The 6 MB cap | Out of memory | The app needs more than 6 MB. |
| The badge's own memory running short first | Out of memory | The badge is low on memory. |
| Too many interpreted instructions (bulk memory operations included) | App took too long | Over 2000000 wasm instructions in one callback |
| Too much native time | App took too long | Native code ran over 200 ms in one callback |
| Too much host work: API calls, copies, console output | App took too long | Over 200000 host steps in one callback (copies, output, fs, crypto, drawing) |
| A callback past 3 s, whatever it was doing | App took too long | The app ran over 3 s in one callback |
| The native form can't run (no store key on the badge yet, a signature that doesn't verify, a changed `app.aot`) and there is no `app.wasm` | App error | The app can't start: the store's signature doesn't verify, and it has no app.wasm. Install it again from the App Store |

On a badge (Solana OS 2.2.0), every trap above, a bad pointer, an index outside the frame,
the 6 MB cap, an unknown import, a newer ABI, a start function, native code as `app.wasm`
and each kind of stuck code in "Limits" ended on its error screen with no restart, in both
forms. When the native form can't run and `app.wasm` is
there, nothing shows: the app runs interpreted, and the console logs why
(`native code not used (...)`).

The messages name the function (`sos_to_int`, `sos_push_str`, ...) of the raw ABI that
`badge.h` called for you. When the linear memory can't grow (the 4 MB maximum, or the
6 MB cap) nothing stops: `memory.grow` answers -1 and `malloc` returns `NULL`. `exit()`
isn't an error: the app just closes.

## Standard library

wasi-libc, the C library of wasi-sdk (musl-based), is linked into the app, so ordinary C
works: `string.h`, `stdlib.h` (`malloc`, `qsort`, `strtol`, `rand`), `stdio.h`
formatting (`snprintf`, `sscanf`), `math.h`, `ctype.h`, `time.h`, 64-bit arithmetic. The
badge provides only these WASI calls underneath it:

| Call | On the badge |
|---|---|
| `fd_write` | `stdout` and `stderr` go to the serial console a line at a time, like Lua's `print` (`[<app id>] ...`, control characters as `?`); the console shows them, Run on badge and Try in emulator show them below the app. One call takes at most 4 buffers and 1 KB (stdio writes the rest with the next), and output costs host steps ("Limits") |
| `clock_time_get` | `time()` and `clock_gettime(CLOCK_REALTIME)` are the server's clock. Until the badge has it, `clock_gettime(CLOCK_REALTIME)` fails and `time()` returns 0, as `sys_time()` does; the monotonic clock counts from boot |
| `random_get` | `getentropy()`, `arc4random()`: the badge's hardware random numbers, at most 4 KB a call (more fails with `EINVAL`; `getentropy()` asks for 256 bytes at most) |
| `proc_exit` | `exit()` ends the app at once, without `on_exit` |
| `fd_close`, `fd_seek`, `fd_fdstat_get`, `environ_*`, `args_*` | answered, doing nothing useful (stdio asks them at start); no environment, no arguments |

So: **no files** (`fopen` and friends need WASI calls the badge doesn't provide, and the
module is refused: read the app's files with `fs_read()`), **no sockets** (`http_*`,
`ws_*`), **no threads**, **no sleeping** (`sleep()`, `nanosleep()`: use `timer_after()`),
no `getenv`, no signals. `printf` works and adds about 17 KB to the module; `badge_print()`
logs a line without it. Formatting is slow interpreted: one `snprintf` with two numbers
took about 3 700 WebAssembly instructions. `rand()` is the same sequence every launch
unless you seed it (`srand(sys_millis())`, or `arc4random()`). `main()` is never called.

## The API: badge.h

```c
#include "badge.h"   // includes badge_abi.h, stdint.h, stddef.h, string.h
```

Every function is one call of the Lua API and behaves as the Lua reference says; the
headings below link there. `badge.h` is a header only (every function `static inline`),
so there is nothing to link, and the linker keeps only what the app calls.

### Conventions

- **Names.** `module_function()` for `module.function` (`ui.label` → `ui_label`,
  `mesh.send` → `mesh_send`), and methods as `type_method()` (`l:text()` →
  `ui_label_text(l, ...)`).
- **Indices count from 0** in C: LEDs 0..7, list rows, results (`ui_list_selected()`).
  The raw ABI (`sos_geti`, `badge_arg`) keeps the stack's 1-based indices.
- **Status.** Functions that act return 0, or `BADGE_ERR` (-1) when the API refused or
  failed; `badge_error()` then holds the reason (`"too fast: 4 messages per second"`,
  `"not a mesh channel"`, or a Lua error's text, which counts from 1 as Lua does:
  `led_set(8, ...)` says `"bad argument #1 to '?' (LEDs are 1..8)"`) until the next
  failure. Readers return the value, with a documented default when there is none.
- **Objects** are handles: `ui_obj` (labels, rects, bars, gradients, lists, QR codes,
  images, canvases, animations), `ui_img` (a `ui_load()` result), `ble_link`,
  `ws_socket`. All are `badge_obj { int32_t ref; }`; a constructor returns one with
  `.ref == 0` on failure. `ui_obj_remove(&o)` removes an object and frees its handle;
  `badge_obj_free(&o)` frees a handle without removing anything (a loaded image, a link
  or socket you're done with). Handles you never free are freed when the app exits.
- **Strings out** go into a buffer you pass, with its size: at most `cap - 1` bytes and a
  NUL. The function returns the full length, so a result `>= cap` says it was cut, and
  `BADGE_ERR` when there is no string (`badge_id()` before the first hello). Binary data
  (`fs_read`, Bluetooth values) comes the same way, length first.
- **Tables in** are JSON text: `mesh_send("*", "{\"ping\":1}")`, `storage_set_json()`,
  `http_post_json()`, `items_grant()`. The badge decodes it (`json.decode`), so a table
  arrives as Lua would send it. Or build one on the stack: `badge_new_table()`,
  `badge_set_int("x", 3)`, ..., then `mesh_send_top("*")`.
- **Tables out** stay on the stack as a `badge_val`, valid until the callback that got
  them returns. Documented fields come in structs (`mesh_peer`, `http_response`,
  `ble_dev`, `profile_t`, ...) or through typed helpers; anything else through
  `badge_field_int(v, "name", default)`, `badge_field_str()`, `badge_push_field()`, or
  as JSON text with `json_encode()`.
- **Callbacks** are a function pointer and a `void *user` you choose, handed back on
  every call. Server calls, dialogs and timers that fire once free their slot when they
  run; `timer_every()` keeps it until `timer_cancel()`; a list's `on_select` until
  `ui_obj_remove()`; a socket's and a link's until their close event; a served
  Bluetooth service and a scan until the app exits or replaces them.
- **Colours** are `0xRRGGBB` or a theme colour that follows Dark/Light: `COL_TEXT`,
  `COL_DIM`, `COL_BG`, `COL_PANEL`, `COL_BORDER`, `COL_PURPLE`, `COL_GREEN`, `COL_PINK`,
  `COL_LPURPLE`, `COL_BLUE`, `COL_AQUA`, `COL_LIME`, `COL_YELLOW`, `COL_ORANGE`. In option
  structs 0 means "the default"; use `COL_BLACK` there for black. `COL_NONE` is "no
  colour" (see-through). `RGB(r, g, b)` builds `0xRRGGBB`; `RGB565(r, g, b)` and
  `RGB565_HEX(0xRRGGBB)` build a pixel for `ui_canvas_blit`.
- **Option structs** may be `NULL` for all defaults, and zeroed fields are defaults, so
  designated initializers read well:
  `ui_label(0, 40, "Hi", &(ui_label_opts){.size = 32, .color = COL_GREEN})`.

Every wrapper leaves the stack as it found it, except those named `*_push`, which leave
one value on top for you to read and pop.

### `ui` ([Lua](LUA_API.md#ui))

```c
int ui_bg(uint32_t color);
ui_obj ui_label(int x, int y, const char *text, const ui_label_opts *o);
//   ui_label_opts { int size; uint32_t color; int mono; int w; int align; }
//   size 12 | 14 | 20 | 32; align UI_LEFT | UI_CENTER | UI_RIGHT (needs w)
int ui_label_text(ui_obj l, const char *text);
int ui_label_color(ui_obj l, uint32_t color);
ui_obj ui_rect(int x, int y, int w, int h, const ui_rect_opts *o);
//   ui_rect_opts { uint32_t color; int radius; uint32_t border; }  border: a 1 px border's colour
int ui_rect_size(ui_obj r, int w, int h);
int ui_rect_color(ui_obj r, uint32_t color);
ui_obj ui_bar(int x, int y, int w, int h, int value);      // progress 0..100, the Solana gradient
int ui_bar_set(ui_obj b, int value);
ui_obj ui_gradient(int x, int y, int w, int h);

int ui_obj_pos(ui_obj o, int x, int y);                    // any object
int ui_obj_show(ui_obj o, int show);
int ui_obj_remove(ui_obj *o);                              // and frees the handle
void badge_obj_free(badge_obj *o);

ui_obj ui_list(int x, int y, int w, int h, const char *const *items, int n,
               ui_select_fn on_select, void *user);        // void on_select(void *user, int index)
ui_obj ui_list_rows(int x, int y, int w, int h, const ui_row *rows, int n,
                    ui_select_fn on_select, void *user);   // ui_row { title, sub, right }
int ui_list_items(ui_obj list, const char *const *items, int n);
int ui_list_set_rows(ui_obj list, const ui_row *rows, int n);
int ui_list_selected(ui_obj list);                         // the row, from 0, or -1
int ui_list_select(ui_obj list, int index);

ui_obj ui_qr(int x, int y, int size, const char *text);    // 25..218 px, text <= 256 bytes
int ui_qr_text(ui_obj q, const char *text);
int ui_hint(const char *text);                             // pinned to the bottom (16 px)
int ui_toast(const char *text);
int ui_confirm(const char *text, ui_confirm_fn cb, void *user);    // void cb(void *user, int yes)
int ui_input(const char *prompt, int maxlen, ui_input_fn cb, void *user); // text NULL: cancelled
int ui_clear(void);                                        // every object; free handles you keep
```

#### Images ([Lua](LUA_API.md#images))

```c
ui_img ui_load(const char *path);                          // a bundled PNG or GIF, loaded once
ui_img ui_load_rows(const char *const *rows, int n, const ui_pal *pal, int npal); // pixel art
//   ui_pal { char ch; uint32_t color; }  adds or changes a character (COL_NONE: see-through)
int ui_img_size(ui_img img, int *w, int *h);
ui_obj ui_image(int x, int y, const char *path, int scale);           // scale 1..16 (0: 1)
ui_obj ui_image_of(int x, int y, ui_img img, int scale);              // views share the pixels
ui_obj ui_image_rows(int x, int y, const char *const *rows, int n, const ui_pal *pal, int npal, int scale);
int ui_image_set(ui_obj o, const char *path);
int ui_image_set_img(ui_obj o, ui_img img);
int ui_image_scale(ui_obj o, int scale);                   // images and animations

ui_obj ui_anim(int x, int y, const char *path, const ui_anim_opts *o); // a bundled GIF, playing
//   ui_anim_opts { int scale; int loop; }  loop: UI_LOOP_FILE (what the GIF says),
//   UI_LOOP_FOREVER, UI_LOOP_ONCE, or a number of times
int ui_anim_play(ui_obj a);
int ui_anim_stop(ui_obj a);
int ui_anim_playing(ui_obj a);
```

#### Canvas and blit

```c
ui_obj ui_canvas(int x, int y, int w, int h, int scale, uint32_t color); // color 0: see-through
int ui_canvas_clear(ui_obj c, uint32_t color);             // COL_NONE: see-through
int ui_canvas_pixel(ui_obj c, int x, int y, uint32_t color);
int ui_canvas_rect(ui_obj c, int x, int y, int w, int h, uint32_t color);  // filled
int ui_canvas_line(ui_obj c, int x1, int y1, int x2, int y2, uint32_t color);
int ui_canvas_image(ui_obj c, ui_img img, int x, int y);   // see-through pixels leave it as it was
int ui_canvas_image_path(ui_obj c, const char *path, int x, int y);
int32_t ui_canvas_get(ui_obj c, int x, int y);             // 0xRRGGBB, or -1
int ui_canvas_blit(ui_obj c, const uint16_t *px, int x, int y, int w, int h);
```

`ui_canvas_blit` is why games are written in C: it copies `w` × `h` RGB565 pixels (row by
row, little-endian, `RGB565()`) from the app's memory onto the canvas at (`x`, `y`),
clipped, in one call (`sos_blit` underneath, which skips the stack), at most 4096 pixels
a side. Draw the whole frame into your own buffer in `on_tick`, then blit it. A canvas
costs 2 bytes a pixel of the app's heap whatever its scale, so a 160 × 108 canvas shown
at 2× fills the screen for 34 KB (and a 34 KB buffer of your own). The canvas redraws on
screen after the callback. A blit costs the host 1 step per 16 pixels written (1 080 of
the 200 000 for 160 × 108). Filling a full 320 × 216 canvas took 16.4 to 16.7 ms on a
badge, from native code and interpreted alike: the copy is the badge's own code.

The pixel value `0x0001` is the canvas's own mark for see-through, so a blit writes it as
`0x0000` (black): a blit never makes a pixel see-through. `ui_canvas_get` gives the
colour back from RGB565, each channel's top bits repeated (`0x9945FF` reads `0x9C45FF`,
`0xFF0000` stays `0xFF0000`).

```c
static uint16_t frame[160 * 108];
static ui_obj canvas;

void on_start(void) { canvas = ui_canvas(0, 0, 160, 108, 2, COL_BLACK); }

void on_tick(int32_t ms)
{
    static uint32_t t;
    t += (uint32_t)ms;
    for (int y = 0; y < 108; y++)
        for (int x = 0; x < 160; x++)
            frame[y * 160 + x] = RGB565(x + t / 8, y * 2, (x ^ y) + t / 16);
    ui_canvas_blit(canvas, frame, 0, 0, 160, 108);
}
```

Interpreted, this `on_tick` is about 478 000 WebAssembly instructions (counted in the
emulator): more than 0.1 s a frame on a badge even at a plain loop's pace ("What runs
where"), against a tick every 30 ms. An app that computes every pixel needs its native
form. One that draws less holds up interpreted: the `starfield` example (220 stars into a
160 × 108 frame) drew a frame in 7 ms native and 33 ms interpreted, and ran at 19.4 and
12.6 to 13.2 frames a second.

### `led` (permission `led`, [Lua](LUA_API.md#led-permission-led))

```c
#define LED_COUNT 8                        // 0..5 across the top, 6..7 on the right edge
int led_set(int i, int r, int g, int b);   // 0..255 each; the badge scales to its cap
int led_hex(int i, uint32_t hex);
int led_fill(int r, int g, int b);
int led_off(void);
```

### `timer` ([Lua](LUA_API.md#timer))

```c
int32_t timer_after(int32_t ms, timer_fn fn, void *user);  // void fn(void *user); the id (> 0) or BADGE_ERR
int32_t timer_every(int32_t ms, timer_fn fn, void *user);
void timer_cancel(int32_t id);
```

8 timers at a time; a delay under 20 ms is taken as 20 ms. Anywhere `badge.h` takes a
callback, `NULL` is allowed: the call still happens, and nothing runs when it answers.

### `sys`, `badge`, `json` ([Lua](LUA_API.md#sys-badge-json))

```c
uint32_t sys_millis(void);                 // ms since the app started (no stack: fast)
uint32_t sys_keys(void);                   // keys held now: KEY_HELD(sys_keys(), KEY_UP) (fast)
int32_t sys_time(void);                    // unix seconds from the server's clock, 0 if never synced
int32_t sys_battery(void);                 // percent, -1 on USB or unknown
int sys_memory(int32_t *used, int32_t *max);   // the whole app: Lua state and WAMR's share ("Limits")
void sys_exit(void);
int32_t badge_id(char *buf, uint32_t cap);     // "7K2Q9M"; BADGE_ERR before the first hello
int32_t badge_owner(char *buf, uint32_t cap);  // "@toly"; BADGE_ERR while unclaimed
int badge_online(void);
int json_push(const char *text);           // json.decode, pushed (see "Values on the stack")
int32_t json_encode(badge_val v, char *buf, uint32_t cap);
const char *key_name(int32_t key);         // "up", ... "back"
void badge_print(const char *line);        // a console line, as Lua's print, without printf (<= 1024 bytes)
void badge_fail(const char *msg);          // stop with this on the error screen
```

### `storage` (always allowed, [Lua](LUA_API.md#storage-always-allowed))

```c
int32_t storage_get_int(const char *key, int32_t dflt);
int32_t storage_get_str(const char *key, char *buf, uint32_t cap);  // BADGE_ERR when unset
int32_t storage_get_json(const char *key, char *buf, uint32_t cap); // any value as JSON ("null" when unset)
badge_val storage_push(const char *key);   // the value itself, pushed: pop it
int storage_set_int(const char *key, int32_t v);
int storage_set_bool(const char *key, int v);
int storage_set_str(const char *key, const char *v);
int storage_set_json(const char *key, const char *json);   // tables, lists
int storage_remove(const char *key);
```

16 KB per app, kept on the card across launches and removed with the app. Keep what
lives only in C memory in sync: save in `on_exit` or as it changes.

### `fs` (always allowed, [Lua](LUA_API.md#fs-and-require-always-allowed))

```c
int fs_exists(const char *path);
int32_t fs_size(const char *path);         // -1 when there's no such file
int32_t fs_read(const char *path, void *buf, uint32_t cap);         // the whole file (<= 256 KB)
int32_t fs_read_at(const char *path, int32_t offset, int32_t len, void *buf, uint32_t cap);
int32_t fs_list(const char *path, void (*fn)(void *user, const char *name), void *user);
int32_t fs_lines(const char *path, void (*fn)(void *user, const char *line, uint32_t len), void *user);
int fs_json_push(const char *path);        // the decoded file, pushed
```

The app's bundled data files, read-only (`.c` and `.h` files aren't among them: the
badge has no source). `fs_read` returns the file's length: give a buffer one byte
longer to get all of it (binary is fine; the NUL after it is extra). `fs_list` and
`fs_lines` call `fn` for each entry or line before they return. Reading counts toward the
host's 200 000 steps a callback as in Lua (500 a file opened, 1 per 4 bytes). A missing
file is `0` from `fs_exists`, `-1` from `fs_size` and `BADGE_ERR` from the others.

### `wifi` (permission `wifi`, [Lua](LUA_API.md#wifi-permission-wifi))

```c
int wifi_status(void);                     // WIFI_OFF, WIFI_CONNECTING, WIFI_ON, WIFI_FAILED
int32_t wifi_ssid(char *buf, uint32_t cap);
int wifi_on(void);
int wifi_off(void);
```

### `mesh` (permission `mesh`, [Lua](LUA_API.md#mesh-permission-mesh))

```c
int mesh_nearby(mesh_peer *out, int max);  // how many there are (may be > max)
//   mesh_peer { char id[8], name[32], kind[8]; int32_t level, rssi; int bridged, app; }
int mesh_send(const char *to, const char *json);   // to: a badge id, or "*" / NULL for all
int mesh_send_top(const char *to);         // the table on top of the stack (popped)
int mesh_bridge(void);
int mesh_pause(void);
int mesh_resume(void);
int mesh_info(mesh_info_t *out);
//   mesh_info_t { int32_t channel, event; char band[4]; int online, paused, scanning; }
int mesh_channel(int n);                   // 0: back to the event's channel
int mesh_scan(const mesh_scan_opts *o, mesh_scan_fn cb, void *user);
//   mesh_scan_opts { const int *channels; int nchannels; const char *band; int dwell; }
//   void cb(void *user, badge_val channels, int count), read during the callback with:
int mesh_scan_channel(badge_val channels, int i, mesh_channel_t *out);
//   mesh_channel_t { int32_t channel, badges, beacons, app, npeers; char band[4]; }
int mesh_scan_peer(badge_val channels, int i, int j, mesh_peer *out);

int mesh_meet(const char *id, const char *data_json, mesh_meet_fn cb, void *user);
//   void cb(void *user, const char *peer, badge_val their_data, const char *err)
int32_t mesh_meet_accept(const char *data_json);   // in on_meet(): return mesh_meet_accept(...)
int mesh_meets(int32_t after, badge_result_fn cb, void *user);  // {items = {...}, next}
```

Messages are JSON objects of ≤ 200 bytes, 4 a second. `on_mesh()` receives them:

```c
void on_mesh(void)
{
    char from[8];
    badge_str(badge_arg(1), from, sizeof from);
    int32_t score = badge_field_int(badge_arg(2), "score", -1);   // check: anything can arrive
    if (score >= 0) { /* ... */ }
}
```

Meets, bridges, channels and scans work as the Lua reference says ("Meets", "Bridged
badges", "Choosing a channel"); the [Connectivity guide](CONNECTIVITY.md) says when to use
which.

### `ble` (permission `ble`, Solana OS 1.0.0, [Lua](LUA_API.md#ble-permission-ble-solana-os-100))

```c
int ble_on(void);                          // BADGE_ERR: "not enough memory", "unavailable"
int ble_off(void);
int ble_state(void);                       // 1 on, 0 off
int32_t ble_address(char *buf, uint32_t cap);
int ble_advertise(const ble_adv *a);       // NULL stops
//   ble_adv { const char *name, *service; const void *data; uint32_t data_len; int connectable, interval; }
int ble_scan(const ble_scan_opts *o, ble_scan_fn cb, void *user);   // void cb(void *user, const ble_dev *dev)
//   ble_scan_opts { const char *service, *name; int active, seconds, duty; }
//   ble_dev { char addr[18]; int32_t rssi; char name[30]; char services[4][37]; int nservices;
//             int32_t company; uint8_t data[32]; int data_len; int connectable, random; }
int ble_scan_stop(void);

ble_link ble_connect(const ble_dev *dev, ble_event_fn cb, void *user);
//   void cb(void *user, const ble_event *ev): BLE_EV_OPEN, BLE_EV_NOTIFY (chr, data, len),
//   BLE_EV_CLOSE (reason), always the last
int ble_link_read(ble_link l, const char *service, const char *chr, ble_read_fn cb, void *user);
//   void cb(void *user, const uint8_t *data, uint32_t len, const char *err)
int ble_link_write(ble_link l, const char *service, const char *chr, const void *data, uint32_t len);
int ble_link_write_cb(ble_link l, const char *service, const char *chr, const void *data, uint32_t len,
                      ble_done_fn cb, void *user);         // void cb(void *user, const char *err)
int ble_link_subscribe(ble_link l, const char *service, const char *chr, int on);
int ble_link_close(ble_link l);
int ble_link_state(ble_link l);            // BLE_CONNECTING, BLE_OPEN, BLE_CLOSED

int ble_serve(const ble_service *svc, ble_event_fn cb, void *user);
//   ble_service { const char *service; const ble_char *chars; int nchars; }
//   ble_char { const char *uuid; int read, write, notify; const void *value; uint32_t value_len; }
//   events: BLE_EV_OPEN (addr), BLE_EV_WRITE (chr, data, len), BLE_EV_SUBSCRIBE (chr, on), BLE_EV_CLOSE
int ble_set(const char *chr, const void *data, uint32_t len);
int ble_notify(const char *chr, const void *data, uint32_t len);
int ble_drop(void);
```

Strings in an event (`chr`, `addr`, `reason`) and its data are valid until the callback
returns: copy what you keep. Everything a peer sends is untrusted.

### `lamps` (permission `lamps`, [Lua](LUA_API.md#lamps-permission-lamps))

```c
int lamps_balance(lamps_fn cb, void *user);
int lamps_spend(int32_t amount, const char *sku, lamps_fn cb, void *user);
int lamps_transfer(const char *badge_id, int32_t amount, const char *memo, lamps_fn cb, void *user);
int lamps_deposit(const char *address, int32_t amount, const char *memo, lamps_fn cb, void *user);
int lamps_award(int32_t amount, const char *reason, lamps_fn cb, void *user);
//   void cb(void *user, const lamps_result *r, const char *err)
//   lamps_result { int32_t balance, awarded; int needs_x; char id[24]; }
```

Spending, transfers and deposits show the badge's own confirmation, which no app can
answer. `lamps_award` pays nothing until the app has an award allowance
([LUA_API.md "Develop and publish"](LUA_API.md#develop-and-publish)).

### `items` (permission `items`, [Lua](LUA_API.md#items-permission-items))

```c
int items_list(badge_result_fn cb, void *user);    // result: a list of items
int items_all(badge_result_fn cb, void *user);
int items_grant(const char *kind, const char *name, const char *data_json, badge_result_fn cb, void *user);
int items_update(const char *id, const char *data_json, badge_result_fn cb, void *user);
int items_remove(const char *id, badge_result_fn cb, void *user);
void item_info(badge_val item, item_t *out);       // item_t { char id[40], kind[32], name[48]; int tradable; }
//   its data: badge_field_json(item, "data", buf, cap)
```

`badge_result_fn` is `void cb(void *user, badge_val result, const char *err)`: `err` is
`NULL` and `result` the answer, or `err` the reason (`"offline"`, `"not linked"`,
`"denied"`, `"busy"`, `"socket open"`, ...). Walk a list with `badge_len(result)` and
`badge_push_index(result, i)` (pop each).

### `profile` (permission `profile`, [Lua](LUA_API.md#profile-permission-profile))

```c
int profile_me(profile_fn cb, void *user);
int profile_get(const char *badge_id, profile_fn cb, void *user);
//   void cb(void *user, const profile_t *p, const char *err)
//   profile_t { char name[48], x[32], x_name[64], solana[48]; int32_t level, lamps, pages; char badge[8]; }
```

Empty strings where Lua has `nil` (no X, no linked or public Solana address).

### `quest` (permission `quest`, [Lua](LUA_API.md#quest-permission-quest))

```c
int quest_report(const char *event, const char *data_json, badge_result_fn cb, void *user);
int quest_active(badge_result_fn cb, void *user);  // result: a list
void quest_info(badge_val q, quest_t *out);        // quest_t { char id[40], title[64], step[64]; }
```

### `http` (permission `http`, [Lua](LUA_API.md#http-permission-http-solana-os-046))

```c
int http_get(const char *url, http_fn cb, void *user);
int http_get_headers(const char *url, const http_header *h, int n, http_fn cb, void *user);
int http_post(const char *url, const char *body, http_fn cb, void *user);       // text
int http_post_json(const char *url, const char *json, http_fn cb, void *user);  // sent as JSON
int http_request(const http_req *q, http_fn cb, void *user);
//   http_req { method, url, headers, nheaders, body, body_len, json }
//   void cb(void *user, const http_response *res, const char *err)
//   http_response { int32_t status; char type[64]; int truncated; badge_val body, location; }
int32_t http_body(const http_response *res, char *buf, uint32_t cap);
```

`err` is set when there was no answer (`"offline"`, `"host not allowed"`, `"timeout"`,
...); any answer, a 404 included, comes as `res` with its `status`. Only the hosts in
`app.json`; the badge server makes the request and signs who asked (see the Lua
reference for what your server receives).

### `ws` (permission `http`, Solana OS 0.5.1, [Lua](LUA_API.md#ws-permission-http-solana-os-051))

```c
ws_socket ws_open(const char *url, const char *const *protocols, int nprotocols, ws_fn cb, void *user);
//   void cb(void *user, const ws_event *ev): WS_EV_OPEN, WS_EV_MESSAGE (data, len), WS_EV_CLOSE
//   (reason, code: the host's close code or -1), always the last
int ws_send(ws_socket w, const char *text);
int ws_send_bytes(ws_socket w, const void *data, uint32_t len);
int ws_send_json(ws_socket w, const char *json);
int ws_close(ws_socket w);
int ws_state(ws_socket w);                 // WS_CONNECTING, WS_OPEN, WS_CLOSED
```

### `rpc` (permission `rpc`, Solana OS 0.5.2, [Lua](LUA_API.md#rpc-permission-rpc-solana-os-052))

```c
int rpc_call(const char *method, const char *params_json, const char *cluster,
             badge_result_fn cb, void *user);       // cluster NULL: mainnet, or "devnet"
```

`params_json` is the params array as JSON (`"[\"Gh9Zw...\", {\"commitment\":\"confirmed\"}]"`)
or `NULL`. Integers beyond 32 bits come back as decimal strings (`"5000000000"`): read
them with `badge_field_str()` and `strtoll()`, which C has. Never put a keypair on a badge
(see the Lua reference's warning).

### `crypto` and `solana` (always allowed, Solana OS 2.1.0, [Lua](LUA_API.md#crypto-and-solana-always-allowed-solana-os-210))

```c
int crypto_sha256(const void *data, uint32_t len, uint8_t out[32]);
int crypto_on_curve(const uint8_t bytes[32]);
int crypto_verify(const uint8_t key[32], const void *msg, uint32_t len, const uint8_t sig[64]);
int32_t solana_encode(const void *bytes, uint32_t len, char *out, uint32_t cap);  // base58, <= 128 bytes
int32_t solana_decode(const char *text, uint8_t *out, uint32_t cap);   // how many bytes, or BADGE_ERR
int solana_pda(const badge_bytes *seeds, int nseeds, const uint8_t program[32],
               uint8_t out[32], int *bump);         // badge_bytes { const void *p; uint32_t len; }
```

They cost the host steps the Lua reference lists. Plain C can do SHA-256 or base58
itself too, in its own memory; the badge's versions are its own code, much faster than
the interpreter (the SHA-256 row of "What runs where").

## Values on the stack

Between C and the Lua API values travel on a stack (see "The raw ABI"). Most apps never
see it, except for tables that callbacks receive and for fields beyond the typed
structs:

```c
badge_val badge_arg(int32_t n);            // a callback's argument n (from 1)
badge_val badge_top(void);                 // the top value
void badge_pop(int32_t n);
int32_t badge_type(badge_val v);           // SOS_NIL, SOS_BOOL, SOS_INT, SOS_NUM, SOS_STR, SOS_TABLE, ...
int32_t badge_int(badge_val v);  float badge_num(badge_val v);  int badge_bool(badge_val v);
int32_t badge_str(badge_val v, char *buf, uint32_t cap);
uint32_t badge_len(badge_val v);           // a string's bytes, a list's length
int32_t badge_field_type(badge_val t, const char *key);
int32_t badge_field_int(badge_val t, const char *key, int32_t dflt);
float badge_field_num(badge_val t, const char *key, float dflt);
int badge_field_bool(badge_val t, const char *key);
int32_t badge_field_str(badge_val t, const char *key, char *buf, uint32_t cap);
int32_t badge_field_json(badge_val t, const char *key, char *buf, uint32_t cap);
badge_val badge_push_field(badge_val t, const char *key);  // pushes t[key]: pop it
badge_val badge_push_index(badge_val t, int32_t i);        // pushes t[i + 1]: pop it
void badge_new_table(void);                // then badge_set_int/num/bool/str/bytes(key, ...)
```

A `badge_val` is a position in the current frame: valid until the function that got it
returns, and only while what is above it hasn't been popped. Nested tables:

```c
static void got_meets(void *user, badge_val res, const char *err)
{
    if (err) { ui_toast(err); return; }
    badge_val items = badge_push_field(res, "items");
    for (uint32_t i = 0; i < badge_len(items); i++) {
        badge_val m = badge_push_index(items, (int32_t)i);
        char peer[8];
        badge_field_str(m, "peer", peer, sizeof peer);
        /* ... */
        badge_pop(1);                       // m
    }
    badge_pop(1);                           // items
}
```

## The raw ABI

`badge.h` is built on `badge_abi.h`, the ABI itself (`SOS_ABI 1`): the functions a module
imports from module `"badge"` and the exports the badge calls. It is a small, checked
version of Lua's C API over the app's Lua state, so a new Lua function is reachable from C
the day it ships, before `badge.h` has a wrapper:

```c
// mesh.scan's "band" option, the raw way: mesh.scan({band = "5"}, cb)
sos_global("mesh.scan", 9);                // the function
sos_new_table();
sos_push_str("5", 1);
sos_set(-2, "band", 4);                    // {band = "5"}
sos_push_fn(my_callback, my_data);         // int32_t my_callback(void *ud, int32_t nargs)
if (sos_call(2, 0) != 0) {                 // failed: the error's text is on top
    char err[100];
    sos_to_str(-1, err, sizeof err);
    sos_pop(1);
}
```

| Group | Functions |
|---|---|
| The frame | `sos_top`, `sos_pop`, `sos_push_value`, `sos_remove` |
| Push | `sos_push_nil`, `sos_push_bool`, `sos_push_int`, `sos_push_num` (a 32-bit float), `sos_push_str` (bytes, copied), `sos_new_table`, `sos_push_fn` (a C function as a value), and `badge.h`'s `sos_push_cstr` |
| Read | `sos_type`, `sos_to_bool`, `sos_to_int`, `sos_to_num`, `sos_to_str` (into a buffer; returns the full length), `sos_len` |
| Tables and objects | `sos_get` (a field, or an object's method), `sos_set`, `sos_geti`, `sos_seti` (from 1), `sos_next` |
| The API | `sos_global` (a dotted name: `"ui.label"`, `"ui.W"`), `sos_call` |
| Kept values | `sos_ref` (pops and keeps; 1024 at a time), `sos_unref`, `sos_push_ref` |
| No stack | `sos_blit`, `sos_keys`, `sos_millis`, `sos_log`, `sos_fail`, `sos_host_abi` |

- **The frame.** Each call into the app has its own frame: `on_start` starts empty, a
  callback starts with its arguments at 1..nargs. Indices are 1-based from the bottom or
  negative from the top (-1 the top); an index outside the frame stops the app (only
  `sos_type` answers `SOS_NONE` for one). At most 64 values a frame.
- **Calling.** Push the function (`sos_global`, or an object's method with `sos_get`),
  then its arguments (for a method the object first, as `self`), then
  `sos_call(nargs, nresults)`: 0 with `nresults` values pushed (`SOS_MULTRET`: all;
  count with `sos_top()`), or non-zero with one value, the error's text, which you pop.
  The non-zero value is the host's error code: 2 for an ordinary error (what an API
  function raises). Out of memory, running out of time and a trap in a nested callback
  always stop the app instead, whatever the caller wanted.
- **Names.** `sos_global` takes a dotted name of 1 to 96 characters; longer stops the
  app. A name that doesn't exist pushes nil.
- **C callbacks.** `sos_push_fn(fn, ud)` makes a function value that calls
  `int32_t fn(void *ud, int32_t nargs)` with its arguments at 1..nargs; it returns how
  many results it left on top. The value lives as long as the badge holds it. `fn` must
  be a function of the app with exactly that type; 0 or anything else stops the app.
- **Straight to the badge.** `sos_log` writes at most 1024 bytes a line (the rest is
  cut); `sos_blit` takes at most 4096 pixels a side and writes `0x0001` as `0x0000`
  ("Canvas and blit"); `sos_fail` stops the app with its text; `sos_keys` and
  `sos_millis` cost no stack.
- **The exports** are declared in `badge_abi.h` with their export names (`on_start`,
  `on_key`, `on_tick`, `on_exit`, `on_mesh`, `on_meet`), so defining one exports it and
  leaving one out is fine. `on_meet` signs by pushing a table and returning 1
  (`mesh_meet_accept` does both). `badge_abi()` (weak, defined in the header) tells the
  badge which ABI the app was built for; a badge refuses a newer one than its own.
- **One budget a callback.** Every call into the app made while a callback runs (a
  function value of `sos_push_fn` that an API function calls, a list's selection inside
  `on_key`) shares that callback's budget ("Limits"); at launch `_initialize` and
  `on_start` share one.

## Example

A tap counter that keeps its count, flashes the LEDs and pays 1 !LAMP per 100 taps (the
[Lua example](LUA_API.md#example), in C; `app.json`: `"lang": "c"`, `"permissions":
["led", "lamps"]`):

```c
#include <stdio.h>

#include "badge.h"

static ui_obj count_label;
static int32_t taps;

static void show(void)
{
    char text[16];
    snprintf(text, sizeof text, "%ld", (long)taps);
    ui_label_text(count_label, text);
}

static void lights_off(void *user)
{
    (void)user;
    led_off();
}

static void awarded(void *user, const lamps_result *r, const char *err)
{
    (void)user;
    if (err) return;
    if (r->awarded > 0) ui_toast("+1 !LAMP");
    else if (r->needs_x) ui_toast("Link X to earn !LAMPs");
}

void on_start(void)
{
    taps = storage_get_int("taps", 0);
    ui_label(0, 60, "Taps", &(ui_label_opts){.size = 20, .w = UI_W, .align = UI_CENTER});
    count_label = ui_label(0, 100, "", &(ui_label_opts){.size = 32, .color = COL_GREEN, .w = UI_W, .align = UI_CENTER});
    ui_hint("SELECT tap   CANCEL exit");
    show();
}

int32_t on_key(int32_t key, int32_t repeat)
{
    if (key != KEY_SELECT || repeat) return key != KEY_BACK;
    taps++;
    show();
    led_fill(0x14, 0xF1, 0x95);
    timer_after(80, lights_off, NULL);
    if (taps % 100 == 0) lamps_award(1, "100 taps", awarded, NULL);
    return 1;
}

void on_exit(void)
{
    storage_set_int("taps", taps);
}
```

More in `sdk/c/examples/`: `hello` (labels, keys, a timer), `starfield` (a game drawn a
frame at a time with `ui_canvas_blit` and `sys_keys`), `tally` (storage), `sol-price`
(`http` with a callback), `ping` (`mesh_send` and `on_mesh`) and `sketch` (a canvas and a
bundled PNG). `sh sdk/c/examples/build.sh` builds them all.

## Develop and publish

As for Lua ([LUA_API.md "Develop and publish"](LUA_API.md#develop-and-publish)): the same
store, permissions, test runs, code check, review, allowances and limits. What differs:

- **Write** the app in the editor on `/apps/submit` with the language set to C, or drop
  a folder or .zip with `main.c` and `app.json`. The submit page compiles it on the
  server and shows the compiler's errors with their file and line.
- **Try in emulator** compiles the draft on the server and runs `app.wasm` in the
  emulator's WAMR interpreter, with every data file on the emulated card. `printf` and
  errors show below it.
- **Run on badge** compiles the draft on the server and copies `app.wasm` and the data
  files to a real badge over USB (Web Serial), which needs Solana OS 2.2.0. A draft
  runs in the interpreter there: only the store's install brings native code, so a
  published app that computes a lot runs much faster than its test run ("What runs where").
- **Locally**, build with `badge-cc` and copy the module over the serial console, which
  takes `app.wasm` like any app file (LUA_API.md "Console"):
  `lua recv <id> app.wasm <bytes> <sha256>` (and `app.json`, data files), then
  `lua run <id>`. The console never takes `app.aot`, and any console write to an
  installed app deletes its `app.aot`, which would no longer match.
- **`lua mem`** after a run says which form ran (`app.wasm (interpreted)` or
  `app.aot (native)`), the app's heap peak, and the most instructions (interpreted) or
  native time a callback used, against the limit; native, also the flash slot, whether
  this launch copied the code in, and how long the signature check and the slot took.
- **`lua aot <id>`** says why an installed app runs `app.aot` or `app.wasm`, without
  launching it: this build's target and the manifest's, whether the store's key is
  cached, whether the signature verifies, `app.aot`'s size against a flash slot, and what
  runs (`runs: app.wasm, interpreted: the store's key isn't on this badge yet`).
  `lua aotkey` fetches the store's key, with Wi-Fi on. When a launch finds no key or a
  signature the cached key doesn't verify (the store may have changed its key), the app
  runs `app.wasm` and, if the badge is online, it asks the server for the key again in the
  background, once per boot, so the next launch can verify; `lua aot <id>` then says so
  (`key refresh: refreshed: the server sent a new key 12 s ago`).
- **Speed.** To measure the real thing, publish the app unlisted, install it from the
  store on a badge and read `lua mem` there.
- **Publish** submits the source: the store compiles it itself, so the code check and the
  reviewers read the code that runs. Each version is compiled once with the toolchain in
  `versions`; when the native target changes, the store builds the native form again.

## Security

- **The sandbox.** A C app is a WebAssembly module: it reaches only its own linear
  memory, and the badge only through the imports listed in `toolchain/imports`, every
  one of which checks its indices, pointers and lengths. A module importing anything
  else doesn't load. Everything an app can do goes through the same Lua API, permissions
  and limits as a Lua app's, and the same confirmations (spending, transfers) that no app
  can answer.
- **WebAssembly from anyone.** The interpreter enforces the sandbox whoever compiled the
  module, so a badge runs an `app.wasm` from anywhere: the store, Run on badge, the
  console.
- **Native code only from the store, signed.** `app.aot` is machine code for the badge's
  CPU, and the sandbox then rests on the compiler that made it: the store's `wamrc`, from
  the module the store itself built and checked, with bounds checks on every memory
  access. So the badge runs an `app.aot` only when the store's installer wrote it (the
  console, Run on badge and quest downloads never write one, and any console write to
  the app deletes it), its target is the badge's own, its SHA-256 and size match the
  manifest's `code.aot`, and the manifest's `sig` is the store's Ed25519 signature of
  `sos-aot1|<target>|<sha256 in hex>`, checked against the store's public key (`GET
  /api/device/aotkey`, which the badge caches) at install and again at every launch,
  before the file is mapped; then the badge hashes the flash slot through its mapping, so
  it runs exactly the signed bytes. A file edited on the card doesn't run: it no longer
  matches what was signed, and the badge runs `app.wasm` instead. Each file must also be
  what its name says before WAMR's loader sees it (which would take either format from
  its first bytes): `app.wasm` WebAssembly, `app.aot` native code. Measured: with no key
  cached, a wrong key or a changed `app.aot`, the app ran from `app.wasm`; native code
  copied onto the card as `app.wasm` was refused, with no restart.
- **Your code is readable.** Anyone who installs a C app can read its `app.wasm` (and
  decompile it): an API key in it is no secret. Keep keys on your server and call it
  with `http`, as in Lua.
- **Input is hostile.** Mesh messages, Bluetooth values, socket messages and HTTP bodies
  can be anything. Check lengths before copying, use the buffer sizes `badge.h` asks
  for, and never trust a field's presence: the `badge_field_*` helpers return your
  default when it is missing or of another type.
