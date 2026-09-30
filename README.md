# RACCOON V2

A 1–4 cylinder standalone motorcycle ECU (STM32H733) with a PyQt6 tuning application,
authenticated calibration loading, and a software-in-the-loop simulator that runs the real
firmware so the whole stack can be exercised with no hardware attached.

**Status: bench-ready software, not track-ready hardware.** Every claim below is about code.
Nothing here has been run on an engine. See *What is not done* before treating any of it as
finished.

---

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

That produces three things:

| Target | What it is |
|---|---|
| `build/rc_tests` | The C unit and property suite, including the C↔Python parity checks |
| `build/rc_sim` | The simulator: scenarios, a fuzzer, and a tuner link server |
| `firmware/generated/*` | Regenerated calibration schema — see *The calibration image* |

The build runs the code generator, so a schema change and a firmware change cannot be made
separately and left inconsistent.

Requirements: CMake ≥ 3.22, a C11 compiler, Python 3.10+, and `arm-none-eabi-gcc` only if
you are cross-compiling for the target. The host build has no embedded-toolchain dependency.

## Run the tuner

```sh
PYTHONPATH=tuner python3 -m raccoon                 # against the simulator
PYTHONPATH=tuner python3 -m raccoon tcp:192.168.1.50:5000
PYTHONPATH=tuner python3 -m raccoon serial:/dev/ttyACM0
```

**The default target is the simulator, deliberately.** A tuner that connected to real
hardware because someone forgot an argument would be alarming, and starting against the
simulator means the interface can be opened and reviewed with no motorcycle present.

Requires `PyQt6`, `pyqtgraph` and `numpy`.

## Run the simulator directly

```sh
./build/rc_sim --cases=10          # scenarios, then 10 fuzz cases
./build/rc_sim --serve-link        # become a tuner server on an arbitrary port
```

`--serve-link` prints two ports on stdout:

```
LINK_PORT 42137
CONTROL_PORT 36611
```

`LINK_PORT` speaks the tuner protocol. `CONTROL_PORT` is a plain-text line channel for the
test rig — `start`, `stop`, `throttle <0..100>` — and it is **not** part of the ECU protocol.
Starting and stopping an engine is something an operator does with a key; putting it on the
same link that refuses a tune change because the engine is running would make that refusal
meaningless.

## Test

```sh
./build/rc_tests                        # 289,445 checks
./build/rc_sim --cases=10                # scenarios + fuzz
python3 -m pytest tests/python -q        # 68 tests: protocol, workflow, profiles, GUI
```

The Python tests run against a compiled `rc_sim` — the real control loop, the real flash
layer, the real validator, the real link service. There is no mock ECU anywhere in the
project, and that is the point: every serious defect found so far lived in the gap between
what the client expected and what the firmware did, and a mock agrees with whatever the
client expects by construction.

Sanitizers:

```sh
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_C_FLAGS="-fsanitize=address,undefined -g"
cmake --build build-asan -j
./build-asan/rc_tests
./build-asan/rc_sim --cases=3
```

---

## The calibration image

One schema generates three things from a single source: the C struct, the C accessors, and
the Python descriptor the tuner reads. **537 items, 4716 bytes, 28 cross-item rules.**

Of those 537, sixteen are `axis1d` items: the RPM and load breakpoints each of the eight 2-D
tables is interpolated along. A table is addressed by cell number, and cell numbers are
meaningless without knowing which rpm each one stands for. Those breakpoints used to be an
even division of the axis range baked into the firmware, which gave an idling engine at
1,000 rpm exactly as much resolution as one at 12,000 rpm - and put the one decision a tuner
most wants to make in the one place nobody tunes. They are now in the calibration, validated
as strictly increasing at activation, and reachable from the tuner.

The image is a C struct with natural alignment, so it contains padding between items and
the fields sum to less than the image. Every item therefore carries its own byte offset, and
the Python side packs each item at that offset individually rather than building one large
struct format. A format string cannot express "skip these three bytes", so every field after
the first padding hole would be written three bytes early — an image the right size and wrong
in every byte, which the ECU rejects with a CRC error pointing at nothing in particular.

`tests/parity` pins this from both sides:

- the tuner's default image is **byte-identical** to the firmware's live calibration;
- the tuner's CRC-32C matches `rc_crc32c` (it is Castagnoli, **not** the CRC-32 in `zlib` or
  `hashlib`);
- the tuner's unlock proof matches the firmware's, byte for byte;
- the live-data wire layout agrees.

Each of those has a teeth check: change one thing and the test fails. That matters, because
a mismatch in any of them produces a well-formed message that is simply wrong — a tuner that
can never unlock, against a log that says nothing about why.

### Writing a tune

```
GUI edit → diff shown → confirm → STAGE → VERIFY → ACTIVATE
```

* **STAGE** writes the inactive slot. The running engine is untouched.
* **VERIFY** makes the ECU read the slot back, compare it byte for byte, and revalidate it.
* **ACTIVATE** is one flag flip, and only runs after the ECU agreed with what was sent.

A failure at any step leaves the running tune exactly as it was, and the interface says so.
A write is refused while the engine is running, and refused while the ECU is locked.

### Unlocking

```
key   = SHA-256("RACCOON-UNLOCK-V1" || salt || code)
proof = HMAC-SHA256(key, salt || le32(CRC32C(running image)))
```

The salt is published in `GET_INFO`. That is why the code has to be in the key: a proof
derived from the salt alone would authenticate anyone who has read the identity block. The
image digest means a captured proof stops working as soon as a different tune is loaded, so
it cannot be saved and replayed into an arbitrary calibration later.

This is a short human-typeable code to stop someone flashing a parked motorcycle. **It is
not protection against an attacker holding the device**, and it should never be described as
if it were. See the security section of the specification for the RSA-3072 signing chain
that does provide that, which is not implemented.

---

## Configuring the ECU to a bike

The engine core contains no application logic, exactly as the specification requires. What
describes a particular motorcycle is data, in `engine_profiles/`:

```sh
engine_profiles/
  gt650.json            the first application, with research provenance on every value
  inline_four.json      a generic four with a 4-tooth cam wheel
  parallel_twin.json    the GT650 class of machine, generic
  single_cylinder.json  a generic single
  custom.json           a TEMPLATE, deliberately unverified so a half-filled copy is refused
```

Every value carries where it came from:

```json
"crank_teeth": {"value": 36, "provenance": "assumed",
                "note": "UNK-GT-001: the OEM trigger wheel has not been counted."}
```

`measured` means a dial read it. `datasheet` means the part's published specification.
`wiring` means continuity or a diagram established it. `assumed` means somebody reasoned it
out. The difference is the whole point: a tune built from a mixture of the two cannot be
reasoned about when the engine misbehaves.

Building one:

```python
from raccoon.vehicle import VehicleProfile
from raccoon import build

profile = VehicleProfile.load("engine_profiles/gt650.json")
image, report = build.build(profile)
print(report.summary())
```

**Derived** from the profile and from arithmetic - geometry including the displacement from
bore × stroke, the trigger period window, the driver currents and dwell ceiling, the strategy
selections, the capability bitfield, the safety envelope.

**Refused**, with every reason at once: a firing order that repeats a cylinder, sequential
injection with no cam wheel, fewer injectors than cylinders, a safety limit above the
maximum rpm, closed loop with no sensor fitted, two throttle sensors with DBW off.

**Not invented**: the fuel and ignition tables. A profile knows an engine has two cylinders;
it does not know what to feed them at 4000 rpm and 60 kPa. The build ships a conservative
starting table and always says so:

```
measure these before running on a track:
  - confirm tooth 1's angle with a timing light or a scope
  - injector flow at 4 A: the fuel model is built on flow in cc per second
  - FUEL TABLE: this build ships the conservative starting table
  - IGNITION TABLE: same
```

The GT650 profile is honest about all of it - 14 of its 45 values are measured or from a
datasheet, and 31 are not, which is the real state of a project that has not been on a dyno.

In the tuner, the **vehicle / project** tab does this and shows the three lists side by side.
Building loads the result straight into the calibration editor, so the path is: describe the
bike, build, edit, write.

`tests/python/test_vehicle_profiles.py` writes profile-built calibrations to a real simulator
and reads them back, so "the firmware accepted it" is a fact rather than the builder agreeing
with itself.

## Layout

```
firmware/
  include/rc/        public headers, one per module
  src/               the implementation
  generated/         calibration schema output - do not edit
  hardware/          HAL, currently virtual only
  sim/               plant, scenarios, fuzzer, link server
tests/
  unit/              C unit and property tests
  parity/            C↔Python agreement, driven from a generated reference
  python/            protocol, workflow and GUI tests against a real simulator
tuner/
  raccoon/
    calibration.py   the 537-item image model, including axis breakpoints
    link.py          framing, parser, live data
    client.py        the tuner's view of an ECU
    transport.py     TCP, serial, loopback
    bridge.py        drive a compiled simulator from Python
    ui/              the PyQt6 application
tools/
  codegen/           the schema, the rules, and the emitters
  gen_parity.py      runs Python, compiles the C, compares the two
configs/             the generated calibration descriptor
docs/                hardware facts, unknowns, open issues
```

---

## What is not done

This is the part worth reading.

**No hardware has been run.** There is no GT650, no dyno, no injector bench, no HIL rig. The
plant model in the simulator is a plausible engine, not a measured one — its idle speed,
charge limits and friction are guesses, which is why the scenario suite asserts *behaviour*
("the engine is running and being given fuel") rather than specific values. The
`docs/OPEN_ITEMS.md` entry `SIM-001` records this.

**Physical unknowns are open.** Trigger wheel geometry, TDC, firing order, injector flow,
coil behaviour, sensor wiring and CAN details are all unmeasured. `docs/UNKNOWN_REGISTER.md`
tracks them; none of them can be resolved from a desk.

**The STM32 HAL does not exist.** `hal_virtual.c` is a simulation. There is no CMSIS support,
no target linker script, no `hal_stm32h7.c`, and no real bootloader. The firmware has never
been compiled for the target.

**The ESP32-S3 side does not exist.** No Wi-Fi, no secure boot, no flash encryption, no OTA,
no RSA signature verification. The architecture is decided and the STM32 half of the link
protocol is done, but the bridge that would carry it over the air is not written.

**The calibration package is not signed.** The unlock code gates who may write; it does not
prove who wrote. An anti-rollback counter does not exist.

**Not everything is production-hardened.** The simulator link server exists to make the tuner
testable, and it is a test fixture, not a product.

So: the code is in a state where the next person can build on it and where the remaining
work is enumerated. It is not a motorcycle ECU you can bolt to a frame.
