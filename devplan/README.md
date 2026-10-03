# Development Plans

Design notes and implementation plans for BMDS subsystems. Write a plan here **before** implementing a non-trivial subsystem, and keep it up to date as the implementation changes.

- [`roadmap.md`](roadmap.md) — phased plan and progress checklist.

## Plan Document Template

Each subsystem plan (`devplan/<subsystem>.md`) should cover:

1. **Goal** — what this subsystem provides and why.
2. **Interface** — the public API (kernel-internal and, if any, userspace/ABI surface).
3. **Design** — structure, data flow, execution contexts (ISR / task / hot path), memory spaces used.
4. **IDF usage** — which ESP-IDF layers it builds on, which it replaces, and why.
5. **Resource budget** — expected RAM (per MemSpace), IRAM, flash, CPU; then measured numbers once implemented.
6. **ABI impact** — any userspace-visible surface and how it complies with `docs/ABI.md`.
7. **Testing** — host tests, target tests.
8. **Open questions / future work.**
