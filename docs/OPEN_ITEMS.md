
## SIM-001 - The plant model cannot hold a stable idle

| Field | Value |
|---|---|
| Status | OPEN - blocks scenario assertions that need a settled part-throttle mixture |
| Found by | software-in-the-loop simulator, `firmware/sim` |
| Affects | `sim_lean_condition_produces_a_cut`, and any scenario asserting fuel flow at load |

The virtual engine in `firmware/sim/rc_sim.c` is a behavioural model, not a validated one.
Its charge model draws from absolute manifold pressure and its throttle restriction is a
single calibrated fraction, with no residual-charge or trapped-volume term. At part
throttle the resulting mixture sits outside the ECU's own safety band, so the ECU cuts
fuel - correctly, given what it is being told - and the fuel-flow assertions in those two
scenarios cannot be evaluated.

This is a limitation of the MODEL, not a defect in the ECU. The fix is to add a
trapped-charge term and calibrate the friction and charge limits against real
instrumentation, which needs the bench. It is deliberately not worked around by weakening
the assertions: the two scenarios assert what they can and say so.

**What this does and does not block.** It does not block the unit suite (285 104 checks,
0 failures), the fuzzer, or the twelve firmware defects the simulator found and that were
fixed. It does block claiming that closed-loop convergence has been demonstrated in a
closed loop, which is why the direction test exists instead of a convergence test.

## Schema precision: the 1-byte VE table cannot express a fraction

`fuel.ve_table` is stored as one unsigned byte per cell with `scale = 1.0` and a range of
20..200. That gives whole numbers only: a tuner asking for 72.5 is stored as 72, and reads
back as 72. Nothing in the flash path loses the value - the value was never there.

This is not a bug in the transfer, and the round trip is exact for every representable
value. It is a resolution decision in the schema, and it was found by an end-to-end test
rather than by inspection, which is the argument for running those tests. Two questions an
owner has to answer before a track tune:

- Is whole-percent VE resolution acceptable? It is roughly 0.5% of stoich, so on a
  lambda-closed setup it is immaterial, but on an open-loop or wide-open run it is visible
  in the trace.
- Does any other 1-byte table have the same problem where it matters more? `boost_target`
  and the retard tables are the ones to check.

Resolution: UNVERIFIED. Not changed, because widening the cell type moves the image layout
and therefore every offset, the descriptor, and the parity test - a change to make
deliberately, with the schema version bumped, not as a side effect of a test failing.

## Tuner: what the GUI does and does not prove

The PyQt6 application is written and driven by 16 tests against the real window, the real
widgets and a real ECU. Those tests click the actual buttons through the actual signals.
They are worth running, and they are still not a substitute for a person using it: no test
checks whether a number is *legible* on a trackside screen in daylight, or whether the
ordering of the editor's 537 items matches how someone thinks about an engine.

Deliberately out of scope for now, in the order I would do them:

- **No datalog export.** Live data is shown; it cannot be written to a file. A track session
  currently produces no evidence. This is the single biggest gap in the tuner.
- **No autotune.** `docs/AUTOTUNE_SPEC.md` describes it; nothing implements it.
- **No per-cylinder table editing on a live map axis.** The table grid is a grid, not a
  plotted map, so shaping a fuel table by RPM/load is done by numbers rather than by shape.
- **No saved tunes.** The editor holds one image in memory. There is no file format, no
  slot management, and no "which tune is on the ECU" list.
- **No units/axis configuration for tables.** Column headers are cell indices. A real tuner
  labels them with the engine's RPM and MAP breakpoints.

## Known: the 1-byte VE table

Recorded above in this file. Worth repeating here because it will otherwise be rediscovered
as a bug: `fuel.ve_table` is one unsigned byte per cell with scale 1.0, so it expresses whole
numbers only. A request for 72.5 is stored as 72. Changing the cell width moves the image
layout and therefore every offset, the descriptor and the parity test - a deliberate change
with a schema version bump, not a side effect.

## Tuner thread model, recorded because it is easy to break

The link lives on one thread. Every request from the interface is a queued signal, and
`EcuClient` refuses concurrent use with a named error rather than interleaving reads.

Three mistakes were made and fixed during development, and each presented as something else
entirely:

1. Calling a worker's slot directly from the window, which ran it on the *interface* thread
   and froze the window on the first read. Looked like a hang.
2. A `QTimer` stopped from the wrong thread, which Qt reports as a warning and otherwise
   ignores - leaving a timer reading a link after the window closed. Looked like a leaked
   thread.
3. `timeout or self.default` in a transport, where a legitimate zero timeout became the
   one-second default and a 50 ms poll blocked for a second. Looked like a stalled link.

The rule that prevents all three: the interface thread only ever *asks*, and only ever
receives results it can display.

## The boost rule and a naturally aspirated engine

`boost_table_within_limit` requires the boost target table's **declared range** (0..200 kPa)
to sit inside `safety.boost_max_kpa`. A naturally aspirated profile wants to say "101.3 kPa,
that is all there is", and cannot satisfy that rule without claiming a ceiling the engine
cannot reach.

The builder's answer is to raise `boost_max_kpa` to the table's range and say so in the build
report. That is honest but unsatisfying: the number in the calibration is now a statement
about the schema rather than about the engine.

**This is a schema problem, not a builder problem.** The right fix is one of:

- skip the rule when `boost_closed_loop_enabled` is false, on the grounds that a table nobody
  is allowed to use cannot be outside a limit;
- give the boost table a per-vehicle declared range, which means moving the range out of the
  schema and into the calibration, which is a larger change than it sounds;
- split "the boost table's range" from "the maximum boost the hardware can make", so the rule
  tests the second against the engine's capability rather than the first.

Not changed, because each option moves the image layout or weakens a safety rule, and that
should be a deliberate decision with the schema version bumped.

## The trigger period window is a two-part relationship, not three rpm points

An early version of the builder derived `min_period_us` as the tooth period at maximum rpm,
which is a reasonable-looking derivation and the wrong one. The shipped defaults settle the
semantics: `overspeed_period_us` is exactly twice `min_period_us`, which only makes sense if
the two are the overspeed threshold and a noise floor below it.

A period window belongs to a specific crank wheel. Copied from another engine it either
declares every tooth an overspeed at the limit or accepts a stall indefinitely, and both look
like "the trigger is not quite right" rather than "these two numbers came from a different
wheel". `tuner/raccoon/build.py` derives all three from the tooth count; nothing is typed.

## The ignition envelope and the lambda window must CONTAIN their tables

`ignition_table_within_envelope` compares the ignition table's declared range against the hard
envelope, not the cells currently in it. The intent is right: a table that can express an
angle the envelope will silently clamp lets a tuner type a number that is quietly altered.

The consequence for a profile is that narrowing the envelope below the table's range is
refused by the ECU, so the builder WIDENS the envelope and reports it. The alternative -
rejecting the profile - would make a safer profile impossible to build, which is a bad way
for a safety mechanism to fail. The report says exactly what was widened and why.
