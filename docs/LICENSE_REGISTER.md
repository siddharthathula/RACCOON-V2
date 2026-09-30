# LICENSE REGISTER (index)

**Document ID:** LIC-001

RACCOON V2 has **one** licence register, held in two files with distinct purposes:

| File | Purpose |
|---|---|
| [`THIRD_PARTY_LICENSE_REGISTER.md`](THIRD_PARTY_LICENSE_REGISTER.md) | The authoritative register of every third-party project, its licence, what was read, whether code was copied, and the legal implication. **This is the record.** |
| `LICENSE` (repository root) | RACCOON V2's own licence, to be chosen at commercial release (`UNK-SW-002`, pending legal review) |

**Current stance (Phase 1):** RACCOON V2 is developed as a **closed-source commercial
product**. This is why the clean-room protocol in §4 of the third-party register exists,
and why no GPL-licensed code may be read into a module while it is being written.

**Summary of the position:**

| Class | Projects | RACCOON action |
|---|---|---|
| GPL-family (copyleft) | Speeduino (GPL-2.0), rusEFI (GPL-3.0 + terms), MegaSquirt family | Public documentation consulted for ideas only. **No code, no structural translation.** Independently implemented |
| Proprietary / closed | TunerStudio, MegaLogViewer, Haltech, Link, FuelTech, MaxxECU, ECUMaster, MoTeC, Syvecs, Marelli | Public product information consulted for market/feature analysis only. **No code, no bundling, no reverse engineering, no protocol reimplementation without an independent specification** |
| Proprietary silicon (datasheets) | MAX9924/9926 (ADI), MC33810/MC33813 (NXP), CJ125 (Bosch) | Purchased components. Datasheet-published functional parameters and recommended application values are used to design our **own** schematics. Reference designs are not copied |
| Permissive runtime dependencies | STM32 HAL/CMSIS (Apache-2.0/BSD-3), ARM CMSIS (Apache-2.0) | Used, notices preserved, confined to `firmware/hardware/` |
| Permissive/LGPL application dependencies | PyQt6 (LGPL-3.0), NumPy, SciPy, pyqtgraph, pytest, ruff, mypy, bandit, pydantic, SQLite | Used. PyQt6 is dynamically linked and unmodified; LGPL notices are shipped with every distribution |

**The single most important rule:** the register is updated **before** any third-party
material is read into the build, vendored, or linked. A row written afterwards is a
process failure, not a formality.

See also `RELEASE_PROCESS.md` §4 (licence gate) and §4 of
`THIRD_PARTY_LICENSE_REGISTER.md` (clean-room protocol).
