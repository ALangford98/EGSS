# Procedural courses: what there is, and how far it could go

2026-10-17. A scoping note, not a design to build from yet. The owner asked
for procedural courses to be "scoped out a little" alongside the cannon run.

## What exists now

`TestEnv/src/RunCourse.h` builds every course, hand-made or not, out of the
same parts:

- **Pieces.** A course is a list of pieces: start, sprint, vaults, ramp and
  drop, climb, crates, cover, pillars, corner, finish. Each piece is a
  stretch of track in its own frame (along it, across it). It lays out
  its obstacles, places its cannons, and declares where it commits the
  runner. A no-fire zone covers the run-up and landing of a vault line,
  the climb, the pillars and the drop.
- **Fairness by construction.** Cannons stand off the open stretches. The
  committing stretches are either out of their arc or inside a no-fire
  zone. On top of that, `Gunnery` holds any shot that would be unfair at
  the moment it would fire (see the cannon run's notes in
  `TestEnv/src/Gunnery.h`).
- **A generator.** `RunCourse::Generate(seed)` draws 8 to 11 pieces by
  weight, using a xorshift stream seeded from the course number. It
  follows these rules:
  - never the same piece twice running;
  - never two committing pieces back to back, so there is open ground
    with cannons between them;
  - no corner first or last;
  - a corner turns whichever way keeps its footprint clear of what is
    already laid. If both ways are taken it is redrawn; if the course
    boxes itself in, the finish comes early.
- **Determinism.** The same seed gives the same course every time, on any
  machine. A time on "course 4711" means the same thing to everyone, so a
  daily seed or a shared leaderboard needs no course data to be sent.
- **A runner for testing.** `RunCourse::Waypoints(layout)` gives the
  track's centre line. The temporary autopilot used in this session
  steered along it, jumped on the parkour hint and dodged on threats. It
  finished the hand-made course in 58 s with 2 hits, and is what checked
  the generated courses (results in the changelog for 2026-10-17).

## Where it falls short

1. **No difficulty model.** Weights are flat. A course can open with three
   cannon pieces and end with pillars. There is no notion of a curve
   (gentle start, hard middle, breather, finale), nor of a target time.
2. **No validation in the loop.** The generator checks footprints only. It
   cannot know whether a piece can be run into from the one before, for
   example a climb whose run-up is a corner's plaza. Each piece has been
   run, but not every pair.
3. **Flat world.** Every piece starts and ends at ground level. A ramp and
   drop goes up and comes down within one piece, so there are no rooftop
   runs, no stretches of platforms, and no choice of high and low routes.
4. **One line.** There are no branches. Every runner takes the same path,
   so the only skill is execution, not route-finding.
5. **Cannons are per piece.** No cannon covers several pieces, and nothing
   accounts for one's arc reaching into a neighbour's no-fire zone.
   `Gunnery` still holds those shots, so it is fair, but the course does
   not plan for it.
6. **Scenery is trees.** Every course looks alike apart from its layout.

## How far it could go: a ladder

Each rung stands on the one below. Rough sizes are in sessions of the kind
this one was.

1. **A difficulty curve (small).** Give each piece a difficulty (its moves'
   precision, its cannon count) and give the generator a target profile,
   for example ramp up for 40%, a breather, peak at 80%, an easy finish.
   Draw by weight times fit to the profile. Add a par time from the sum of
   each piece's measured time.
2. **Validation by running it (small to medium).** Promote the autopilot
   from a test into `RunCourse::Pilot`, with no cannons: the ghost runner.
   Reject a generated course that the ghost cannot finish within par times
   1.5, and draw again. This turns "valid footprint" into "runnable", and
   it is how piece-pair problems (2 above) get found without enumerating
   pairs. The ghost is also an attract mode and a "beat the ghost" race.
3. **Height (medium).** Give pieces an entry and an exit height, with
   connectors such as stairs, ramps, ledges and drops. New piece kinds
   would include a platform run, a gap jump between platforms with a fall
   below as the penalty, a wall run across a gap, and a hang traverse. The
   humanoid already does all of these in the parkour yard.
4. **Branches (medium to large).** A piece may fork into two routes that
   rejoin, typically a fast exposed route and a slow covered one, so that
   choosing where to be shot at becomes part of the run. The path check
   becomes a small graph and the gates become per-branch.
5. **Cannon placement as its own pass (medium).** Once the track is laid,
   place cannons over the whole course. Maximise how much open track is
   covered, subject to the arcs not reaching any no-fire zone, with a
   minimum spacing and a budget from the difficulty curve. The fairness
   rules then hold by construction instead of mostly.
6. **Themes (medium).** The same layout with a different palette, props and
   lighting: a yard, a rooftop city, a quarry. Prefab models already give
   the building blocks (crates, barriers, trusses, light panels).

## What it would take to make a game of it

- **Seeds as content.** A daily course for everyone (the date as the seed),
  plus "course 4711" to share. Best times kept per seed, which the run
  already does in memory, persisted to the project folder.
- **Ghosts.** Record the input of a best run (`--record` already does
  this, step-exact) and replay it as a translucent second runner. Input
  replay is deterministic, so a ghost is a recording, not a stream of
  poses.
- **Export.** The cannons and the course builder live in `TestEnv`. To ship
  the run as a game, `Gunnery` would become a runtime system driven by a
  component, as `LauncherSystem` is, and the course would be built at load
  from a seed component. That is the same step the crash test's launcher
  took.

## Recommendation

Rungs 1 and 2 together: a difficulty curve validated by the ghost runner.
They are small, they make every seed worth playing, and the ghost gives the
game a second mode for free. Height (3) is the biggest single gain in how
it plays, and the humanoid already has every move it needs.
