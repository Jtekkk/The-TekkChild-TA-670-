# Tekkchild Model 670 (TA-670)

A world-class **vari-mu tube limiter/compressor plugin** in the lineage of the
legendary Fairchild 660/670 — dual-channel, feedback-detecting, with
program-dependent ratio (≈ 2:1 → 30:1), a naturally soft knee, six switchable
attack/release time constants (0.2 ms attack; releases from 0.3 s to a
program-dependent 25 s), and **dual-mono / stereo / Mid-Side (LAT-VERT)**
operation for mixing, mastering, and vinyl-cutting workflows.

## 📐 Specification

The complete engineering specification — DSP math, analog reference
architecture, control surface, real-time C++ coding standards, numerical
methods, and the test/performance plan — lives in:

**[SPECIFICATION.md](./SPECIFICATION.md)**

| Section | Contents |
|---|---|
| §1 | Purpose, measurable requirements (REQ-001…014), sonic targets |
| §2 | Analog reference architecture (tubes, transformers, sidechain) |
| §3 | System signal flow & feedback topology |
| §4 | Variable-mu gain element: nonlinear tube model |
| §5 | Detection sidechain, six time constants, multi-reservoir release |
| §6 | Static law: soft knee, thresholds, emergent 2:1→30:1 ratio |
| §7 | Stereo / dual-mono / M-S (LAT/VERT) matrix & linking |
| §8 | Transformer, tube & noise coloration |
| §9 | Oversampling & anti-aliasing |
| §10 | Full parameter tables |
| §11 | Metering (GR + IEC VU ballistics) & calibration |
| §12 | Software architecture & real-time coding standards |
| §13 | Numerical methods, precision & stability |
| §14 | Testing, validation & performance budgets |

## Status

**Specification draft v0.1.0** — implementation has not started. The spec is
written to be directly implementable: C++20 core, CLAP-first with VST3/AU/AAX
wrappers, strict real-time-safety rules, and numeric acceptance criteria for
every requirement.
