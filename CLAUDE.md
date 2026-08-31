# EGSS

A game engine written from scratch in C++17, following the shape of TheCherno's
Hazel series but diverging where it made sense. `EGSS/` is the engine, built as
a shared library; `TestEnv/` is a sandbox app that links against it and holds
seventeen demos.

This is built to be **understood**, not to ship a game. A working black box is
worth less here than a mechanism that can be followed. Explain the load-bearing
idea; skip the ceremony.

## Reading this repo without drowning in it

**Load context in tiers, and expand only on a trigger.** This section used to
say "read `docs/HANDOVER.md` before anything substantial". Obeying that
literally cost ~252k tokens across the handover and the README, and still
answered "what are we working on" **wrong** — because the expensive documents
were the stale ones and `git log` went unread. Cheap and live beats large and
cached.

**Tier 0 — always.** This file and `memory/MEMORY.md`. ~2.9k tokens, automatic.

**Tier 1 — every session, before anything else.** `git log --oneline -15`,
`git status --short`, and `docs/STATE.md`. **~1k tokens, and it answers "what
are we working on" by itself.** Nothing above this tier is needed for that
question. Where `docs/STATE.md` and git disagree, **git wins**.

**Tier 2 — one area, on a named trigger:**

| Trigger | Read | Cost |
| --- | --- | --- |
| About to edit a file | `grep -n` its symbols against the traps body | ~1k |
| First time in an area | The trap index in `docs/HANDOVER.md`, then that area's `docs/ENGINE.md` section | ~4k + ~2k |
| "Why is it like this?" | `grep -n '^### ' README.md \| head -30`, then `sed` out the one entry | ~1.5k |
| Picking the next task | `README.md` "Still outstanding" only | ~9k |
| A measurement disagrees with arithmetic | The traps body in full | 31k |

**Tier 3 — never read whole.** The README changelog is ~170k tokens; the traps
body is ~31k over 254 entries. Both are newest-first and both are
meant to be **grepped**. Reading either end to end is the failure this ladder
exists to prevent.

**Say which trigger fired when you expand a tier**, so the cost is visible and
the ladder can be corrected when it is wrong.

## Git

**Never commit to `main`, and never push.** The owner owns both, commits their
own work between sessions and sometimes while a reply is being written. Assume
any git state you did not just observe is stale — check `git log` and
`git status` rather than trusting an earlier message.

**Work directly in the local checkout, not an isolated worktree branch.**
Earlier this used a `worktree-<name>` branch for handover, reviewed with
`git log -p main..worktree-<name>` and merged in later — that's gone. The
owner wants to run, test, and push from the same tree a session edits, with
no merge step between. Do not call `EnterWorktree`.

Commit in logical units and let the messages carry the reasoning, the same way
the changelog does. Do not fabricate granular history — if a change was
genuinely developed as one thing, it is one commit.

Never `git stash` in a shared checkout, and never `reset --hard` anywhere
without checking which repository the command will actually run in — a `cd` in
an earlier command persists.

## Build

```sh
./egss.py build            # debug; always regenerates project files first
./egss.py build release
./egss.py build all        # every config
./egss.py run
./egss.py clean
./egss.py gen              # project files only
./egss.py build --no-gen   # skip regeneration

./egss.py sanitize         # ASan + UBSan, then every demo under it
./egss.py sanitize release # the same at -O2, where UB actually bites
./egss.py build --sanitize # just the instrumented build
./egss.py windows          # bridge KWin's window list to the wallpaper

# Flags for TestEnv go after a bare --
./egss.py run -- --demo Breakout --record run.rec
```

**`TestEnv/` in the repo root is source, not a binary.** The executable is at
`bin/<Config>-linux-x86_64/TestEnv/TestEnv` and must run from that directory —
assets load by path relative to it, and screenshots, recordings, `imgui.ini` and
`profile.json` all land beside it. `./egss.py run` handles that for you.

It regenerates every time **on purpose**: premake expands file globs at
generation time, so a new `.cpp` is invisible to the build until it does. That
costs 0.21 s and removes the most confusing failure in the project — code that
compiles in the editor and never links.

**Verify all three configs before calling something done.** Release has caught
things Debug did not.

Assets load by path relative to the executable; premake copies `TestEnv/assets`
next to the binary after every link.

`vendor/bin/premake/premake5` is gitignored, and the `EGSS/vendor/*` submodules
need `git submodule update --init --recursive`. A fresh clone or a new worktree
has neither, and the build fails confusingly on both.

## How work is verified

The habit that has repeatedly paid off:

> **Compute the expected value by hand, then compare. And suspect the
> measurement before the code.**

Screenshots prove a thing renders. They do not prove it is *right*. Nearly every
real bug in this project was found by a number disagreeing with arithmetic.

The strongest checks compare against **a formula the code knows nothing about** —
re-deriving the implementation's own arithmetic proves only that it was copied
twice. Past examples: RT60 against Sabine/Eyring, mean free path against
`π·Area/Perimeter`, a reverb tail measured back with Schroeder integration,
momentum conservation across a contact impulse, `(2/3) g sin θ` for a rolling
disc.

**Capture frames from inside the engine, never from outside it.** `import`
hangs against XWayland and `xdotool` matches stale windows; two sessions were
lost to that before the in-engine path existed. Now:

```sh
./TestEnv --demo Physics --capture shots/a.png --capture-frame 240
./TestEnv --demo Physics --lockstep --hide-ui --capture shots/a.png --capture-step 240
```

**A capture or a replay opens no window.** `--capture` and `--play` mean nobody
is watching, and a window that maps takes the keyboard off whatever the machine
is actually being used for — a one-second capture still lands in the middle of a
sentence. Add `--show-window` to watch one happen, or `--hide-window` to silence
any other run. A hidden window renders identically: the back buffer belongs to
the driver, not the compositor, and captures taken both ways are byte-identical
across four demos.

The second form is **bit-reproducible** — two runs produce byte-identical PNGs,
verified. Three things are load-bearing and each was measured:

- `--capture-step`, not `--capture-frame`. Frames get skipped while the window
  is being mapped, so the same frame number is not the same simulation state
  twice.
- `--lockstep` runs exactly one fixed step per frame instead of feeding the
  accumulator wall-clock time. Without it, how much has been simulated by a
  given moment depends on how fast the machine ran.
- `--hide-ui` drops the panels, which print frame times in milliseconds and so
  differ every run no matter how deterministic the simulation is.

That makes a captured frame a **regression test**: run it, hash the pixels,
compare. `Framebuffer::ReadPixelRGBA` is still the right tool for checking one
colour at one point — use it when the question is "what value is this pixel",
and capture when the question is "does this scene look right".

**Recorded input replays exactly**, which is how a test gets input without a
person:

```sh
./TestEnv --demo Breakout --record run.rec    # play it, then quit
./TestEnv --play run.rec --hide-ui --capture a.png --capture-step 200
```

Input is sampled per *fixed step*, so the frame rate it was recorded at does not
matter, and the file names its own scene. All six demos are step-deterministic;
keep them that way — **anything that moves belongs in `OnFixedUpdate`, not
`OnUpdate`**. Three demos violated that and could not reproduce themselves run
to run, let alone under replay.

**Panel state is recorded too, for parameters a demo registers.** One line per
slider that reaches the simulation:

```cpp
RegisterParam("Gravity", &m_Gravity);   // in the demo's constructor
```

Sampled per fixed step beside the input and written back on playback, so a
session recorded while the sliders moved replays as itself. Unregistered knobs
are unchanged — that is the point, since a colour or a camera angle changes the
picture rather than the run. If a replay diverges, ask first whether the knob
that moved is registered.

### The self-test pattern

Tests are **temporary**, live in `TestEnv/src/`, and are deleted once they have
done their job. There is no test framework and the project does not want one;
the value is in the measuring, not in keeping the harness.

```cpp
// TEMPORARY -- delete after verifying X.
#pragma once
#include <Egss.h>

namespace XTest {
    inline int g_Pass = 0, g_Fail = 0;
    inline void Check(bool ok, const std::string& what) {
        ok ? g_Pass++ : g_Fail++;
        EGSS_TRACE("  [{0}] {1}", ok ? "ok " : "FAIL", what);
    }
    inline void Run() { /* ... */ }
}
```

Call it from the `TestEnv` constructor in `TestApp.cpp`, run the app for a few
seconds, read the log, then remove both the header and the call. **Grep for
`TEMPORARY` before declaring finished.**

Two specifics: `AudioEngine::RenderForTest` runs the mixer with no device (it
must not run while the device is live — `Shutdown()`, test, `Init()`), and
GL-dependent checks need a live context, so they run inside a demo's update
rather than at construction.

### When a measurement disagrees

Check the measurement first. A constant offset across every reading, a value
that is exactly zero, or a suspiciously round ratio are all more likely to be
the test than the code. Report failures **with the output** — several of the
most useful moments in this project were a test failing and the reason being
more interesting than the fix.

## Conventions

**Comments explain *why*, and are worth the space.** Record the reasoning and
the wrong turn, not a narration of the code. `// Bumping the generation is what
makes every outstanding handle stale` earns its line; `// increment generation`
does not. Match the surrounding density.

**The README changelog is part of the work**, not an afterthought. Entries
record what was built, what broke, and what the measurement said.

**Adding a demo** is one line in `TestEnv/src/DemoRegistry.h`. Demos are
header-only; `DemoLayer` holds the is-this-demo-active guard in `final` `Layer`
overrides, so demos override the `OnDemo*` hooks. `TestEnv/src/Demo.h` holds
`g_ActiveDemo`, the demo shown at startup. Self-registration via static
initialisers was deliberately rejected: forgetting an include would produce no
demo and no compile error.

**Don't build speculative scaffolding.** "A component with no system" is the
phrase used to decline it. Write the thing that reads the data first.

## Working with the owner

- They read the code. Don't over-explain what is already visible in it.
- They ask for specific things and mean the scope they asked for. "Keep going"
  means take the next roadmap item and say which one you picked and why.
- They spot real problems. Take the report seriously and check rather than
  reassure.
