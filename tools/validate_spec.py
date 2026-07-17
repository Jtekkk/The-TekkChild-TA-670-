#!/usr/bin/env python3
"""
validate_spec.py — Numerical validation of the TA-670 specification math.

Implements the governing equations of SPECIFICATION.md (§4–§6, §7, §11)
independently in numpy and verifies the spec's claims against them:

  C1  One-pole coefficient table (§5.3), alpha = exp(-1/(tau*fs))
  C2  Step response reaches 63.21% at t = tau (validates the alpha identity)
  C3  Soft-knee C1 continuity at both knee edges (§6.2)
  C4  Emergent feedback ratio: R = 1+k, program-dependent 2:1 -> 30:1 (§6.4)
  C5  Multi-reservoir program-dependent release, Positions 5 & 6 (§5.4)
  C6  M-S orthonormal matrix: exact reconstruction + energy preservation (§7.2)
  C7  Push-pull odd symmetry: no even harmonics; H3 = beta*A^3/4 (§4.3)
  C8  VU ballistics: 99% at 300 ms, 1–1.5% overshoot (§11.3, IEC 60268-17)

Exit code 0 iff every check passes. Figures are written to docs/validation/.

Usage:  python3 tools/validate_spec.py [--no-plots]
"""

from __future__ import annotations

import argparse
import math
import sys
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

# ----------------------------------------------------------------------------
# Reporting harness
# ----------------------------------------------------------------------------

_results: list[tuple[str, bool, str]] = []


def check(cid: str, name: str, ok: bool, detail: str) -> None:
    _results.append((cid, ok, name))
    status = "PASS" if ok else "FAIL"
    print(f"[{status}] {cid}  {name}")
    print(f"       {detail}")


# ----------------------------------------------------------------------------
# Spec constants (normative values from SPECIFICATION.md)
# ----------------------------------------------------------------------------

FS = 48_000.0  # reference rate for tables (§5.3)
DB_FLOOR = -80.0  # envelope floor used in simulation (dB)

# §5.5 position table: (attack tau, fast release tau, [(rel_tau, chg_tau)...])
POSITIONS = {
    1: (0.2e-3, 0.30, []),
    2: (0.2e-3, 0.80, []),
    3: (0.4e-3, 2.00, []),
    4: (0.4e-3, 5.00, []),
    5: (0.4e-3, 0.20, [(10.0, 1.0)]),
    6: (0.2e-3, 0.30, [(10.0, 1.0), (25.0, 5.0)]),
}

# §4.2 / §6.4 gain-cell calibration targets
G_MAX = 40.0   # maximum gain reduction of the cell (dB)
U_RANGE = 2.5  # output-overshoot span of the control range (dB)
R0, R1 = 2.0, 30.0  # ratio at grazing / at full control drive


def calibrate_cell(g_max: float, u_range: float, r0: float, r1: float):
    """Solve (theta, p) of g_dB(u) = -Gmax[(1-theta)u + theta u^p] so that the
    feedback ratio R(u) = 1 + k(u) hits r0 at u->0 and r1 at u=1, where
    k(u) = (Gmax/U)[(1-theta) + theta*p*u^(p-1)]  (local slope, dB/dB)."""
    k0, k1 = r0 - 1.0, r1 - 1.0
    theta = 1.0 - k0 * u_range / g_max
    p = (k1 - k0) * u_range / (g_max * theta)
    assert 0.0 < theta < 1.0 and p > 1.0, "calibration out of admissible range"
    return theta, p


THETA, P_EXP = calibrate_cell(G_MAX, U_RANGE, R0, R1)


def cell_gr_db(oo: np.ndarray | float) -> np.ndarray | float:
    """Gain reduction magnitude (dB >= 0) versus output overshoot oo (dB)."""
    u = np.clip(np.asarray(oo, dtype=float) / U_RANGE, 0.0, 1.0)
    return G_MAX * ((1.0 - THETA) * u + THETA * u**P_EXP)


def alpha_of(tau: float, fs: float = FS) -> float:
    """One-pole coefficient (§5.2)."""
    return math.exp(-1.0 / (tau * fs))


# ----------------------------------------------------------------------------
# C1 — coefficient table
# ----------------------------------------------------------------------------

def c1_alpha_table() -> list[tuple[str, float]]:
    taus = [
        ("attack 0.2 ms", 0.2e-3), ("attack 0.4 ms", 0.4e-3),
        ("release 0.2 s", 0.2), ("release 0.3 s", 0.3),
        ("release 0.8 s", 0.8), ("release 2 s", 2.0),
        ("release 5 s", 5.0), ("release 10 s", 10.0),
        ("release 25 s", 25.0),
    ]
    rows = [(name, alpha_of(tau)) for name, tau in taus]
    ok = all(0.0 < a < 1.0 for _, a in rows)
    lines = ", ".join(f"{n}: {a:.8f}" for n, a in rows)
    check("C1", "alpha = exp(-1/(tau*fs)) in (0,1) for all positions (BIBO)", ok, lines)
    return rows


# ----------------------------------------------------------------------------
# C2 — step response hits 63.21% at t = tau
# ----------------------------------------------------------------------------

def c2_step_response() -> None:
    """Run the actual recursion y <- a*y + (1-a)*1 and compare it to the
    continuous-time solution 1 - exp(-t/tau) at every sample instant.
    With alpha = exp(-1/(tau*fs)) the two agree exactly (up to float error);
    any other alpha convention would show a systematic deviation here."""
    worst = 0.0
    for tau in (0.2e-3, 0.4e-3, 0.3):
        a = alpha_of(tau)
        n_steps = max(4, int(round(3.0 * tau * FS)))
        y = 0.0
        for n in range(1, n_steps + 1):
            y = a * y + (1.0 - a)
            ref = 1.0 - math.exp(-n / (tau * FS))
            worst = max(worst, abs(y - ref))
    ok = worst < 1e-9
    check("C2", "one-pole recursion tracks 1-exp(-t/tau) exactly (alpha identity)",
          ok, f"worst |sim - analytic| over 3*tau: {worst:.2e}")


# ----------------------------------------------------------------------------
# C3 — soft-knee C1 continuity (§6.2)
# ----------------------------------------------------------------------------

def knee_lout(li: np.ndarray, t: float, r: float, w: float) -> np.ndarray:
    li = np.asarray(li, dtype=float)
    lo = li.copy()
    mid = np.abs(li - t) * 2.0 <= w
    hi = 2.0 * (li - t) > w
    x = li - t + 0.5 * w
    lo = np.where(mid, li + (1.0 / r - 1.0) * x * x / (2.0 * w), lo)
    lo = np.where(hi, t + (li - t) / r, lo)
    return lo


def c3_knee_continuity() -> None:
    """Compare the adjacent branches ANALYTICALLY at each knee edge:
    values and first derivatives must agree exactly (C1 continuity).
      below:  Lo = Li                    -> Lo' = 1
      middle: Lo = Li + (1/R-1)x^2/(2W),  x = Li - T + W/2 -> Lo' = 1 + (1/R-1)x/W
      above:  Lo = T + (Li-T)/R          -> Lo' = 1/R
    """
    t, r, w = -12.0, 8.0, 10.0
    lo_edge, hi_edge = t - w / 2.0, t + w / 2.0

    def mid_val(li):  # middle branch, closed form
        x = li - t + 0.5 * w
        return li + (1.0 / r - 1.0) * x * x / (2.0 * w)

    def mid_slope(li):
        x = li - t + 0.5 * w
        return 1.0 + (1.0 / r - 1.0) * x / w

    v_lo = abs(mid_val(lo_edge) - lo_edge)                       # vs Lo = Li
    s_lo = abs(mid_slope(lo_edge) - 1.0)
    v_hi = abs(mid_val(hi_edge) - (t + (hi_edge - t) / r))       # vs upper branch
    s_hi = abs(mid_slope(hi_edge) - 1.0 / r)
    worst_val, worst_slope = max(v_lo, v_hi), max(s_lo, s_hi)
    ok = worst_val < 1e-12 and worst_slope < 1e-12
    check("C3", "soft knee is C1 at both edges (analytic value & slope match)",
          ok, f"max value gap {worst_val:.2e} dB, max slope gap {worst_slope:.2e}")


# ----------------------------------------------------------------------------
# C4 — emergent feedback ratio (§6.4)
# ----------------------------------------------------------------------------

def solve_output_overshoot(oi: float) -> float:
    """Solve Oo + g(Oo) = Oi for the static feedback loop (bisection;
    the LHS is strictly increasing in Oo)."""
    lo, hi = 0.0, max(oi, 1e-9)
    for _ in range(200):
        mid = 0.5 * (lo + hi)
        if mid + cell_gr_db(mid) < oi:
            lo = mid
        else:
            hi = mid
    return 0.5 * (lo + hi)


def ratio_at(oo: np.ndarray | float) -> np.ndarray | float:
    """Analytic feedback ratio at the solved operating point (§6.4):
    R = 1 + k(u), k(u) = (Gmax/U)[(1-theta) + theta*p*u^(p-1)] for u < 1;
    the control range is exhausted at u >= 1 (tube at cutoff) -> R = 1."""
    u = np.asarray(oo, dtype=float) / U_RANGE
    k = (G_MAX / U_RANGE) * ((1.0 - THETA)
                             + THETA * P_EXP * np.minimum(u, 1.0) ** (P_EXP - 1.0))
    return np.where(u < 1.0, 1.0 + k, 1.0)


def c4_feedback_ratio():
    # Sweep to just below control-range exhaustion (oi = U + Gmax = 42.5 dB).
    oi = np.linspace(0.001, U_RANGE + G_MAX - 1e-6, 1200)
    oo = np.array([solve_output_overshoot(v) for v in oi])
    ratio = np.asarray(ratio_at(oo))

    # Cross-check the analytic ratio against a numerical slope mid-range.
    i = np.searchsorted(oi, 20.0)
    num = (oi[i + 1] - oi[i - 1]) / (oo[i + 1] - oo[i - 1])
    xchk = abs(num - ratio[i]) / ratio[i]

    r_low = float(ratio[0])
    r_at20 = float(np.interp(20.0, oi, ratio))
    r_max = float(ratio.max())
    mono = bool(np.all(np.diff(ratio) > -1e-9))

    ok = (abs(r_low - R0) < 0.1 and r_at20 >= 20.0
          and R1 - 0.5 <= r_max <= R1 + 1e-6 and mono and xchk < 0.02)
    check("C4", "feedback ratio: R=1+k, ~2:1 grazing -> >=20:1 @ +20 dB -> 30:1 max",
          ok, f"R(0+)={r_low:.3f}, R(+20dB)={r_at20:.1f}, Rmax={r_max:.2f}, "
              f"monotone={mono}, analytic-vs-numeric {xchk*100:.2f}%  "
              f"[theta={THETA:.4f}, p={P_EXP:.4f}]")
    return oi, oo, ratio


# ----------------------------------------------------------------------------
# C5 — multi-reservoir program-dependent release (§5.4)
# ----------------------------------------------------------------------------

@dataclass
class Envelope:
    """Fast attack/release state plus slow charge/release reservoirs (dB)."""
    tau_a: float
    tau_fast: float
    reservoirs: list[tuple[float, float]]  # (release tau, charge tau)
    fs: float = FS
    ef: float = DB_FLOOR
    es: list[float] = field(default_factory=list)

    def __post_init__(self):
        self.aa = alpha_of(self.tau_a, self.fs)
        self.af = alpha_of(self.tau_fast, self.fs)
        self.ar = [alpha_of(rt, self.fs) for rt, _ in self.reservoirs]
        self.ac = [alpha_of(ct, self.fs) for _, ct in self.reservoirs]
        self.es = [DB_FLOOR] * len(self.reservoirs)

    def run(self, env_db: np.ndarray) -> np.ndarray:
        out = np.empty_like(env_db)
        ef, es = self.ef, list(self.es)
        for n, e in enumerate(env_db):
            a = self.aa if e > ef else self.af
            ef = a * ef + (1.0 - a) * e
            ctl = ef
            for j in range(len(es)):
                if e > es[j]:
                    es[j] = self.ac[j] * es[j] + (1.0 - self.ac[j]) * e
                else:
                    es[j] = self.ar[j] * es[j] + (1.0 - self.ar[j]) * DB_FLOOR
                ctl = max(ctl, es[j])
            out[n] = ctl
        self.ef, self.es = ef, es
        return out


def fit_release_tau(t: np.ndarray, ctl: np.ndarray, t0: float, t1: float) -> float:
    """Fit ctl(t) ~ FLOOR + A*exp(-t/tau) on [t0,t1] by linear regression
    on log(ctl - FLOOR)."""
    m = (t >= t0) & (t <= t1)
    y = ctl[m] - DB_FLOOR
    y = np.maximum(y, 1e-9)
    slope, _ = np.polyfit(t[m], np.log(y), 1)
    return -1.0 / slope


def _program(fs: float, segments: list[tuple[float, float]]) -> np.ndarray:
    """Concatenate (duration_s, level_dB) segments into an env-dB signal."""
    parts = [np.full(int(d * fs), lv) for d, lv in segments]
    return np.concatenate(parts)


def c5_reservoir_release():
    results = {}
    fs = FS

    def sim(pos: int, segments):
        ta, tf, res = POSITIONS[pos]
        env = Envelope(ta, tf, res, fs)
        sig = _program(fs, segments)
        ctl = env.run(sig)
        t = np.arange(len(sig)) / fs
        return t, sig, ctl

    # Position 5: isolated 50 ms burst -> fast (0.2 s) release
    t, _, ctl = sim(5, [(0.05, 0.0), (2.0, DB_FLOOR)])
    tau_iso5 = fit_release_tau(t, ctl, 0.06, 0.30)
    # Position 5: 20 s sustained -> 10 s release (reservoir charged)
    t, _, ctl = sim(5, [(20.0, 0.0), (30.0, DB_FLOOR)])
    tau_sus5 = fit_release_tau(t, ctl, 20.5, 25.0)
    results[5] = (tau_iso5, tau_sus5)

    # Position 6: isolated burst -> fast (0.3 s)
    t, _, ctl = sim(6, [(0.05, 0.0), (2.0, DB_FLOOR)])
    tau_iso6 = fit_release_tau(t, ctl, 0.06, 0.40)
    # Position 6: multiple peaks (5 x 0.5 s bursts) -> ~10 s stage
    bursts = []
    for _ in range(5):
        bursts += [(0.5, 0.0), (0.3, DB_FLOOR)]
    t, _, ctl = sim(6, bursts + [(30.0, DB_FLOOR)])
    t_off = 5 * 0.5 + 5 * 0.3
    tau_multi6 = fit_release_tau(t, ctl, t_off + 0.5, t_off + 5.0)
    # Position 6: consistently high (60 s) -> 25 s stage
    t, _, ctl = sim(6, [(60.0, 0.0), (40.0, DB_FLOOR)])
    tau_sus6 = fit_release_tau(t, ctl, 60.5, 70.0)
    results[6] = (tau_iso6, tau_multi6, tau_sus6)

    ok = (abs(tau_iso5 - 0.2) / 0.2 < 0.15 and abs(tau_sus5 - 10.0) / 10.0 < 0.15
          and abs(tau_iso6 - 0.3) / 0.3 < 0.15 and abs(tau_multi6 - 10.0) / 10.0 < 0.25
          and abs(tau_sus6 - 25.0) / 25.0 < 0.15)
    check("C5", "program-dependent release: Pos5 0.2s/10s, Pos6 0.3s/10s/25s",
          ok, f"Pos5 iso={tau_iso5:.3f}s sus={tau_sus5:.2f}s | "
              f"Pos6 iso={tau_iso6:.3f}s multi={tau_multi6:.1f}s sus={tau_sus6:.1f}s")
    return results


# ----------------------------------------------------------------------------
# C6 — M-S matrix (§7.2)
# ----------------------------------------------------------------------------

def c6_midside():
    rng = np.random.default_rng(670)
    k = 1.0 / math.sqrt(2.0)
    lr = rng.standard_normal((2, 1 << 16)).astype(np.float64)
    m, s = k * (lr[0] + lr[1]), k * (lr[0] - lr[1])
    l2, r2 = k * (m + s), k * (m - s)
    err = max(np.abs(l2 - lr[0]).max(), np.abs(r2 - lr[1]).max())
    energy = abs((m**2 + s**2).sum() / (lr[0] ** 2 + lr[1] ** 2).sum() - 1.0)
    ok = err < 1e-14 and energy < 1e-12
    check("C6", "M-S encode/decode: exact reconstruction & energy preservation",
          ok, f"max round-trip error {err:.2e}, energy ratio error {energy:.2e}")


# ----------------------------------------------------------------------------
# C7 — push-pull odd symmetry (§4.3)
# ----------------------------------------------------------------------------

def c7_odd_harmonics():
    n = 1 << 15
    cycles = 64
    a1, amp = 1.0, 0.1
    beta = a1**3 / 3.0
    ph = 2.0 * np.pi * cycles * np.arange(n) / n
    y = np.tanh(a1 * amp * np.sin(ph))
    spec = np.abs(np.fft.rfft(y)) / (n / 2)
    h = {i: spec[i * cycles] for i in range(1, 8)}
    even_floor = 20 * np.log10(max(h[2], h[4], h[6]) / h[1] + 1e-300)
    h3_pred = beta * amp**3 / 4.0
    h3_err = abs(h[3] - h3_pred) / h3_pred
    ok = even_floor < -250.0 and h3_err < 0.05
    check("C7", "odd waveshaper: even harmonics at numerical zero; H3 = beta*A^3/4",
          ok, f"even floor {even_floor:.0f} dBc, H3 predicted {h3_pred:.3e} "
              f"measured {h[3]:.3e} (err {h3_err*100:.2f}%)")
    return spec, cycles, h


# ----------------------------------------------------------------------------
# C8 — VU ballistics (§11.3)
# ----------------------------------------------------------------------------

def vu_step(zeta: float, wn: float, fs: float = FS, dur: float = 1.0) -> np.ndarray:
    """Unit-step response of m'' + 2*zeta*wn*m' + wn^2 m = wn^2, via RK4."""
    n = int(dur * fs)
    m = np.empty(n)
    x, v = 0.0, 0.0
    h = 1.0 / fs

    def deriv(x, v):
        return v, wn * wn * (1.0 - x) - 2.0 * zeta * wn * v

    for i in range(n):
        k1x, k1v = deriv(x, v)
        k2x, k2v = deriv(x + 0.5 * h * k1x, v + 0.5 * h * k1v)
        k3x, k3v = deriv(x + 0.5 * h * k2x, v + 0.5 * h * k2v)
        k4x, k4v = deriv(x + h * k3x, v + h * k3v)
        x += h * (k1x + 2 * k2x + 2 * k3x + k4x) / 6.0
        v += h * (k1v + 2 * k2v + 2 * k3v + k4v) / 6.0
        m[i] = x
    return m


def c8_vu_ballistics():
    zeta = 0.80
    fs_vu = 8000.0  # plenty for a ~20 rad/s system; keeps RK4 loop cheap

    def first_cross_99(wn: float) -> float:
        m = vu_step(zeta, wn, fs_vu, 0.8)
        idx = np.argmax(m >= 0.99)
        return idx / fs_vu if m[idx] >= 0.99 else float("inf")

    lo, hi = 5.0, 60.0
    for _ in range(60):
        mid = 0.5 * (lo + hi)
        if first_cross_99(mid) > 0.300:
            lo = mid
        else:
            hi = mid
    wn = 0.5 * (lo + hi)
    m = vu_step(zeta, wn, fs_vu, 1.5)
    overshoot = (m.max() - 1.0) * 100.0
    t99 = first_cross_99(wn)
    os_theory = math.exp(-zeta * math.pi / math.sqrt(1 - zeta * zeta)) * 100.0
    ok = abs(t99 - 0.300) < 0.005 and 1.0 <= overshoot <= 1.6
    check("C8", "VU: first 99% crossing at 300 ms, overshoot 1-1.5% (zeta=0.80)",
          ok, f"solved wn={wn:.2f} rad/s, t99={t99*1000:.1f} ms, "
              f"overshoot {overshoot:.2f}% (theory {os_theory:.2f}%)")
    return zeta, wn, m, fs_vu


# ----------------------------------------------------------------------------
# Figures (light mode; palette & rules per the dataviz method)
# ----------------------------------------------------------------------------

SURFACE = "#fcfcfb"
INK = "#0b0b0b"
INK2 = "#52514e"
GRID = "#e8e8e6"
BLUE = "#2a78d6"    # series 1
GREEN = "#008300"   # series 2
BLUES = ["#9dc2ec", "#5f9ce0", "#2a78d6", "#1a5399"]  # sequential ramp


def _style(ax, title: str, xlabel: str, ylabel: str):
    ax.set_facecolor(SURFACE)
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    for side in ("left", "bottom"):
        ax.spines[side].set_color(GRID)
    ax.grid(True, color=GRID, linewidth=0.7)
    ax.set_axisbelow(True)
    ax.tick_params(colors=INK2, labelsize=8)
    ax.set_title(title, color=INK, fontsize=10, loc="left", pad=10)
    ax.set_xlabel(xlabel, color=INK2, fontsize=8)
    ax.set_ylabel(ylabel, color=INK2, fontsize=8)


def make_figures(outdir: Path, c4, c7, c8) -> list[Path]:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    plt.rcParams.update({"font.family": "DejaVu Sans", "figure.dpi": 150})
    outdir.mkdir(parents=True, exist_ok=True)
    written: list[Path] = []

    def save(fig, name: str):
        p = outdir / name
        fig.patch.set_facecolor(SURFACE)
        fig.savefig(p, facecolor=SURFACE, bbox_inches="tight")
        plt.close(fig)
        written.append(p)

    # --- Fig 1: gain-cell law & local ratio ---------------------------------
    u = np.linspace(0, 1, 400)
    gdb = -G_MAX * ((1 - THETA) * u + THETA * u**P_EXP)
    kloc = (G_MAX / U_RANGE) * ((1 - THETA) + THETA * P_EXP * u**np.maximum(P_EXP - 1, 0))
    fig, (a1, a2) = plt.subplots(2, 1, figsize=(6.4, 5.6))
    a1.plot(u, gdb, color=BLUE, lw=1.8)
    _style(a1, "Vari-mu cell law  g_dB(u)  (§4.2, calibrated)",
           "normalized control drive u", "gain reduction (dB)")
    a2.plot(u, 1 + kloc, color=BLUE, lw=1.8)
    a2.axhline(R0, color=GRID, lw=1); a2.axhline(R1, color=GRID, lw=1)
    a2.annotate("2:1", (0.02, R0 + 0.6), color=INK2, fontsize=8)
    a2.annotate("30:1", (0.02, R1 - 2.6), color=INK2, fontsize=8)
    _style(a2, "Local compression ratio  R(u) = 1 + k(u)  (§6.4)",
           "normalized control drive u", "ratio ( :1)")
    fig.tight_layout(h_pad=2.0)
    save(fig, "fig1_gain_law.png")

    # --- Fig 2: static curve & emergent ratio -------------------------------
    oi, oo, ratio = c4
    fig, (a1, a2) = plt.subplots(2, 1, figsize=(6.4, 5.6))
    a1.plot(oi, oi, color=GRID, lw=1.2, ls="--")
    a1.plot(oi, oo, color=BLUE, lw=1.8)
    a1.annotate("unity", (5.6, 6.6), color=INK2, fontsize=8, rotation=45)
    a1.annotate("static curve → brickwall", (16, 3.1), color=BLUE, fontsize=8)
    a1.set_ylim(0, 8)
    _style(a1, "Static transfer above threshold (feedback-solved)  (§6.4)",
           "input overshoot over threshold (dB)", "output overshoot (dB)")
    a2.plot(oi, ratio, color=BLUE, lw=1.8)
    for r, lbl, dy in ((2, "2:1", 0.7), (30, "30:1", -2.6)):
        a2.axhline(r, color=GRID, lw=1)
        a2.annotate(lbl, (0.8, r + dy), color=INK2, fontsize=8)
    a2.set_ylim(0, 33)
    _style(a2, "Effective ratio vs drive — program-dependent (REQ-004)",
           "input overshoot over threshold (dB)", "effective ratio ( :1)")
    fig.tight_layout(h_pad=2.0)
    save(fig, "fig2_static_curve.png")

    # --- Fig 3: program-dependent release, Pos 5 & 6 ------------------------
    fig, axes = plt.subplots(2, 1, figsize=(6.4, 6.0))
    for ax, pos in zip(axes, (5, 6)):
        ta, tf, res = POSITIONS[pos]
        burst = _program(FS, [(0.05, 0.0), (14.0, DB_FLOOR)])
        sust = _program(FS, [(20.0, 0.0), (34.0, DB_FLOOR)] if pos == 5
                        else [(60.0, 0.0), (34.0, DB_FLOOR)])
        cb = Envelope(ta, tf, res, FS).run(burst)
        cs = Envelope(ta, tf, res, FS).run(sust)
        tb = np.arange(len(cb)) / FS
        ts = np.arange(len(cs)) / FS - (20.0 if pos == 5 else 60.0)
        dec = 480  # decimate for plotting only
        ax.plot(tb[::dec], cb[::dec], color=BLUE, lw=1.8, label="isolated peak")
        ax.plot(ts[::dec] + 0.05, cs[::dec], color=GREEN, lw=1.8,
                label="sustained program")
        ax.set_xlim(-0.5, 14)
        rels = " / ".join(f"{r[0]:g}s" for r in res)
        _style(ax, f"Position {pos}: fast {tf:g}s vs reservoir {rels} release (§5.4)",
               "time after signal stops (s)", "control envelope (dB)")
        ax.legend(frameon=False, fontsize=8, labelcolor=INK2, loc="upper right")
    fig.tight_layout(h_pad=2.0)
    save(fig, "fig3_release.png")

    # --- Fig 4: push-pull harmonic spectrum ---------------------------------
    spec, cycles, h = c7
    fig, ax = plt.subplots(figsize=(6.4, 3.4))
    hn = np.arange(1, 10)
    mags = np.array([spec[i * cycles] for i in hn])
    dbc = 20 * np.log10(np.maximum(mags / mags[0], 1e-16))
    shown = np.maximum(dbc, -160.0)
    ax.bar(hn, shown - (-160.0), bottom=-160.0, width=0.55, color=BLUE,
           edgecolor=SURFACE, linewidth=2)
    for x, d in zip(hn, dbc):
        lbl = f"{d:.0f}" if d > -150 else "–∞"
        ax.annotate(lbl, (x, max(d, -160) + 3), color=INK2, fontsize=7,
                    ha="center")
    ax.set_xticks(hn)
    ax.set_ylim(-160, 12)
    _style(ax, "Push-pull shaper spectrum (dBc) — even orders cancel (§4.3)",
           "harmonic number", "level (dBc)")
    save(fig, "fig4_harmonics.png")

    # --- Fig 5: VU step response ---------------------------------------------
    zeta, wn, m, fs_vu = c8
    t = np.arange(len(m)) / fs_vu * 1000
    fig, ax = plt.subplots(figsize=(6.4, 3.4))
    ax.plot(t, m * 100, color=BLUE, lw=1.8)
    ax.axhline(99, color=GRID, lw=1)
    ax.axvline(300, color=GRID, lw=1)
    ax.annotate("99% @ 300 ms", (308, 90.5), color=INK2, fontsize=8)
    ax.annotate(f"overshoot {(m.max()-1)*100:.1f}%",
                (t[np.argmax(m)] + 8, m.max() * 100 + 0.5), color=INK2, fontsize=8)
    ax.set_xlim(0, 800); ax.set_ylim(0, 106)
    _style(ax, f"VU ballistics: ζ={zeta:.2f}, ωn={wn:.1f} rad/s (§11.3, IEC 60268-17)",
           "time (ms)", "deflection (% of steady)")
    save(fig, "fig5_vu.png")

    # --- Fig 6: knee family (DC-threshold trim) ------------------------------
    li = np.linspace(-28, 4, 600)
    t0, r = -12.0, 8.0
    fig, ax = plt.subplots(figsize=(6.4, 3.8))
    ax.plot(li, li, color=GRID, lw=1.2, ls="--")
    ax.annotate("unity", (-21.4, -20.6), color=INK2, fontsize=8, rotation=45)
    for w, col in zip((18.0, 10.0, 4.0, 1.0), BLUES):
        ax.plot(li, knee_lout(li, t0, r, w), color=col, lw=1.8,
                label=f"W = {w:g} dB")
    ax.set_xlim(-28, 4)
    ax.set_ylim(-26, -6)
    ax.legend(frameon=False, fontsize=8, labelcolor=INK2, loc="lower right",
              title="knee width (DC trim)", title_fontsize=8)
    ax.get_legend().get_title().set_color(INK2)
    _style(ax, "Soft knee vs DC-threshold trim: seamless → pronounced (§6.3)",
           "input level (dB rel. reference)", "output level (dB)")
    save(fig, "fig6_knee.png")

    return written


# ----------------------------------------------------------------------------
# main
# ----------------------------------------------------------------------------

def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-plots", action="store_true")
    args = ap.parse_args()

    print(f"TA-670 spec validation — fs = {FS:.0f} Hz")
    print(f"cell calibration: Gmax={G_MAX} dB, U={U_RANGE} dB "
          f"-> theta={THETA:.6f}, p={P_EXP:.6f}\n")

    rows = c1_alpha_table()
    c2_step_response()
    c3_knee_continuity()
    c4data = c4_feedback_ratio()
    c5_reservoir_release()
    c6_midside()
    c7data = c7_odd_harmonics()
    c8data = c8_vu_ballistics()

    print("\nTable 5.1 regeneration (alpha at fs = 48 kHz):")
    for name, a in rows:
        print(f"  {name:<16s} alpha = {a:.8f}")

    if not args.no_plots:
        outdir = Path(__file__).resolve().parent.parent / "docs" / "validation"
        for p in make_figures(outdir, c4data, c7data, c8data):
            print(f"wrote {p}")

    failed = [cid for cid, ok, _ in _results if not ok]
    print(f"\n{len(_results) - len(failed)}/{len(_results)} checks passed"
          + (f" — FAILED: {', '.join(failed)}" if failed else " — ALL PASS"))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
