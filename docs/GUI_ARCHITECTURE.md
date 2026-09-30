# GUI ARCHITECTURE

**Document ID:** GUI-001 · **Status:** PHASE 1

Target: a professional tuner beside a motorcycle on a dyno at 02:00. Fast, dense, dark,
keyboard-first, never blocking, and incapable of hiding a dangerous action behind a click.

---

## 1. Layering (spec §121, enforced)

```
  UI layer          widgets, views, editors, dashboards       (PyQt6)
      ▲  reads state, sends intents
  Application state  projects, connection, selections, undo stack, session model
      ▲
  Calibration model  typed schema, transactions, versions, validation
  Engine model       tuner's independent 1–4 cylinder engine model (analysis)
  Data model         telemetry/log/diagnostic typed frames
      ▲
  Protocol           framing, message types, session, auth, updates
      ▲
  Transport          USB CDC / CAN / CAN-FD / wireless / simulator / HIL
```

**Hard rule:** the UI package may not import `transport` or any protocol-implementation
detail beyond the data model. A CI import-check enforces this.

---

## 2. Thread model (spec §120 — never block the UI thread)

```
GUI thread (PyQt6)          — widgets, painting, input only. No I/O, no parsing,
                              no computation over log arrays longer than a screenful.
   │  signals/slots only
   ├── TelemetryWorker (QThread)   — transport I/O, frame decode, ring buffer,
   │                                never touches widgets
   ├── LogWorker       (QThread)   — log file read/decode/index, chunked
   ├── AnalysisWorker  (QThread)   — dyno/drag/track/performance computation
   ├── AutotuneWorker  (QThread)   — statistical accumulation
   ├── FirmwareWorker  (QThread)   — update, progress, verification
   └── MapWorker      (QThread)    — smoothing/interpolation/surface rebuild
```

Rules:
- Workers never create widgets. They emit signals; the GUI thread marshals.
- Long computations are chunked with a **yield** so a cancel is honoured within a bounded
  time; no worker may run unbounded.
- High-rate data goes through lock-free ring buffers (single producer / single consumer),
  never through queued signals at frame rate — queued signals at 1 kHz are themselves a
  performance bug.
- The GUI stays responsive during telemetry, logging, map editing, playback, autotune, dyno
  analysis and firmware update simultaneously. This is an acceptance test, not an aspiration.

---

## 3. Application structure

```
tuner/
  app/         entry point, dependency wiring, argument parsing, single-instance guard
  core/        application state, commands, undo stack, session, event bus, units,
               formatting, keyboard map, theme tokens, capability model
  ui/          main window, workspace registry, command palette, status bar, dialogs,
               confirmation flow, wizard, layout persistence
  widgets/     reusable: numeric field with bounds, labelled value, table, tree,
               log console, event list, fault explanation card
  dashboards/  dashboard, race, dyno, drag, track (shared widget grid + persistence)
  maps/        2D table, heatmap, 3D surface, contour, live cell trace, compare,
               smoothing, ramp, scale, arithmetic, undo/redo
  calibration/ schema loader, generated descriptors, editors per type, transactions,
               version control, diff/merge
  telemetry/   live data tree, channel registry, gauges, real-time plots
  logging/     log viewer, cursor synchronisation, overlays, annotations, markers,
               playback controller, trigger scope
  autotune/    VE autotune engine, ignition analyser, confidence model
  dyno/ drag/ track/ performance/   analysers, metric definitions
  diagnostics/ DTC, freeze frame, sensor health, output health, ECU health, EOL report
  protocol/    generated frame codecs, session, capability, auth
  transport/   usb_cdc, can, canfd, simulator, hil, loopback
  simulator/   plant model client
  hil/         HIL rig control
  firmware_update/ image verification, progress, recovery UX
  projects/    project store (SQLite + files), engine/hardware/engine-profile binding
  ai/          advisory assistant (optional, §84/§115) — proposes, never applies
  tests/
```

---

## 4. Design system (spec §119, §116)

**Tokens** (single source, `core/theme.py`):

```
background      #0B0D10   (near-black, low blue cast)
surface         #12161B
surface_raised  #1A2027
border         #232A33
text_primary    #E6EAF0
text_secondary  #8A94A3
text_disabled   #4A525E
accent          #00C2D6   (cyan — interactive/normal telemetry)
accent_pressed  #009AA9
healthy         #35C46A   (green ONLY for a confirmed healthy state)
warning         #F2A33C   (orange)
critical        #E5484D   (red — warnings and destructive confirmations only)
race            #C46BFF   (mode accents, sparingly)
```

Rules:
- **No gradients in data areas.** No drop shadows on panels. No rounded-everything.
- 4 px spacing base; 8/12/16/24 for structure. Dense by default.
- Typography: a single UI family; tabular numerals for all values; a monospace family
  for hex/raw. Critical values (RPM, lambda, ignition) are large.
- Colour carries **meaning only**: cyan = live/normal, green = confirmed healthy,
  orange = warning, red = critical/destructive. No decorative colour.
- No animation during tuning except a 100 ms value-transition on gauges; anything slower
  is noise at 2 AM.

---

## 5. Keyboard-first operation (spec §118)

Every action has a shortcut; the map editor is fully operable without a mouse.

| Context | Keys |
|---|---|
| Global | `Ctrl+P` command palette · `Ctrl+W` workspace switcher · `Ctrl+L` toggle log dock · `Ctrl+S` save · `Ctrl+F` find · `F1` contextual help |
| Map cell nav | arrows · `Home/End/PageUp/PageDown` · `Shift+arrows` extend selection |
| Value edit | `↑/↓` ±step · `Shift+↑/↓` ×5 · `Ctrl+↑/↓` ×0.1% · type to enter a value |
| Map tools | `Ctrl+Z`/`Ctrl+Y` undo/redo · `Ctrl+C/V/X` copy/paste/fill · `Ctrl+B` bias to current cell |
| Logging | `Ctrl+R` start/stop · `Ctrl+M` marker |
| Dashboards | `F2…F9` presets · `Ctrl+Shift+D` duplicate dashboard |
| Safety | `Ctrl+E` engine arm (opens confirmation) · `Esc` cancels any pending confirmation |

A **command palette** exposes every action with its shortcut, so the keyboard model is
discoverable rather than folklore.

---

## 6. Dangerous-action UX (spec §117)

A single `confirm_destructive()` mechanism, used by every dangerous path:

```
┌──────────────────────────────────────────────────────────┐
│ ⚠  WRITE CALIBRATION TO ECU                            │
├──────────────────────────────────────────────────────────┤
│ Project     : GT650_TRACK                                 │
│ Items       : 14 scalars, 2 tables (1 024 cells)         │
│ Safety items: 3 changed  ← requires ENGINEER role        │
│ Target      : ECU SN 000123, slot A, running engine      │
│                                                           │
│ Consequences:                                             │
│  • ignition and injection values change on the next      │
│    cycle — do not perform this with the engine running   │
│  • 3 safety items will be lowered                        │
│  • Current tune is preserved as v12 and can be restored  │
│                                                           │
│ [ ] I understand and accept                    [ WRITE ]  │
└──────────────────────────────────────────────────────────┘
```

Covered actions: firmware update, calibration write, ignition output test, injector output
test, engine-control arm, factory reset, safety-limit engineering mode, and mode change
while running. The default button is **never** the destructive one.

---

## 7. Map editor (spec §70–72) — the flagship feature

| Capability | Implementation |
|---|---|
| Table view | Custom `QAbstractItemModel`; supports selection, multi-edit, fill, and a per-axis editor |
| Heatmap | Perceptual colour ramp with a legend, min/max markers, and a "flat-cell" highlight |
| 3D surface | `pyqtgraph` `SurfaceItem` with contour projection; rebuild is chunked in a worker |
| Contour | Line contours with labels |
| Live cell trace | Highlighted active cell + the four interpolation corners + the fraction |
| Interpolation | Bilinear, matching the firmware exactly; the GUI shows what the firmware will compute |
| Smoothing | Gaussian / moving-average / constrained, with a preview and an undo point |
| Arithmetic | fill, ramp, scale, add, subtract, multiply, divide, clamp, normalise, bias-to-cell |
| Undo/redo | Command pattern with coalescing for drags; unlimited depth within a session |
| Compare | base vs test: absolute, percentage, changed-cell count, min/max, histogram |
| Clipboard | copy/paste with axis alignment handling, and a **schema-aware** paste that refuses a shape mismatch rather than silently truncating |

A **consistency test** asserts that the GUI's interpolation of a cell equals the firmware's
interpolation of the same cell. A tuner who sees a different number in the GUI than the
ECU uses will stop trusting the product.

---

## 8. Log viewer and playback (spec §75, §76)

- Unlimited channels, loaded lazily from the file index.
- A single synchronised cursor across all plots.
- Zoom/pan per plot and linked; overlays; annotations; event markers.
- Calculated channels (user-defined expressions, evaluated in a worker, bounded, never
  `eval` on untrusted input).
- **Playback**: play/pause/step/rewind/fast-forward with engine-time-accurate advance, and
  it replays dashboard state, the active map cell, and faults — because those are in the
  log (§LOG_FORMAT §5).

---

## 9. Autotune and analysis (spec §73, §74)

- The autotune engine accumulates samples in a worker, rejects invalid regions with a
  **visible, counted** rejection log, and proposes a diff.
- Proposal presentation: `data used · observation · recommendation · confidence ·
  expected effect · risk` (§84, §115). Applying is a separate, explicit action, and
  safety-class items are never in an auto-applicable set.
- The ignition analyser supports timing sweeps, run comparison, and knock correlation, and
  **never advances timing from noisy data** — it proposes, with the evidence, and states
  when the data is insufficient.
- Dyno/drag/track analysers compute only from available channels; a missing channel makes a
  metric show as `unavailable`, never as a guess (§68, §77).

---

## 10. Offline mode (spec §96)

The application is fully usable with no hardware: create projects, edit maps, inspect logs,
analyse dyno runs, configure sensors, run the simulator, and compare tunes. Every feature
has a path that does not require a connection; features that *do* require hardware show
`NOT AVAILABLE — not connected` rather than a dead control.

---

## 11. Capability adaptation (spec §94)

On connect, the ECU's capability frame drives the workspace registry: workspaces for absent
capabilities are hidden or shown with an explicit reason. Layouts persist per project and
per user.

---

## 12. Quality gates for the GUI

| Gate | Criterion |
|---|---|
| Threading | No widget access off the GUI thread (asserted in dev builds) |
| Layering | No `transport` import from `ui` (CI check) |
| Responsiveness | ≥ 30 fps for dashboards and the map editor at the specified data rates while logging and analysing simultaneously (measured) |
| Value integrity | GUI-computed map interpolation == firmware-computed, tested against the same fixtures |
| No unbounded UI work | Every long operation is chunked, cancellable, and shows progress |
| Accessibility | Full keyboard operation, visible focus, contrast ≥ 4.5:1 for text, no colour-only status encoding |
| Dark-theme correctness | No unstyled widget; no default light palette leaking in |

---

## 13. Dependencies

| Package | Use | Licence note |
|---|---|---|
| PyQt6 | UI | LGPL-3.0 → dynamic link, no modification, notices shipped (`THIRD_PARTY_LICENSE_REGISTER.md` TPL-019) |
| pyqtgraph | plots, surfaces, custom items | MIT |
| NumPy | array math, log arrays, interpolation | BSD-3 |
| SciPy | only where justified (smoothing, filters) | BSD-3 |
| pydantic / dataclasses | typed models | MIT |
| SQLite | project store | Public domain |
| pytest, ruff, mypy, bandit | quality gates | MIT / MIT / MIT / Apache-2.0 |

All versions are **locked** in a lock file; the build is reproducible (§100).
