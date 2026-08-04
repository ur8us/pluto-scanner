# Ten Principles for Modern SDR Tools

This document explains the design principles behind Pluto Scanner in a form
intended for both people and AI coding agents. It describes why a behavior
exists, how the current program realizes it, and what must remain true when the
implementation changes.

This is a design-rationale document. Use `SPEC.MD` for the exact API and runtime
contract, `SPECTRUM_CALC.MD` for the DSP and RF formulas, `PLUTO.MD` for hardware
details, and `AGENTS.md` for repository working rules. When prose here and an
exact formula differ, investigate the source and update the inconsistent
documents rather than silently choosing one.

The examples come from the ten-part video series linked in `README.md` and its
subtitle material. The implementation notes are grounded in `main.c`,
`index.html`, and the checks under `tools/`.

## Quick Reference

| # | Principle | Promise to the user | Core engineering invariant |
|---|-----------|---------------------|----------------------------|
| 1 | AI-first development | Work that was impractical for one developer becomes feasible | AI extends effort; human judgment and reproducible tests remain authoritative |
| 2 | A low-traffic web interface | The scanner works through an ordinary browser without streaming raw RF data to it | Publish only the display product: one processed value per visible CSS pixel |
| 3 | One-million-times zoom | One view moves continuously from a wide survey to hertz-scale detail | Acquisition resolution follows the visible span without breaking frequency coordinates |
| 4 | Seamless scan and reception | Zooming changes scale, not the user's operating workflow | The backend alone selects scan/hop or continuous single-frequency capture |
| 5 | Waterfall speed as a range | The user asks for useful cadence instead of configuring FFT mechanics | Minimum rate uses overlap; maximum rate never breaks a stateful CIC stream |
| 6 | Persistent waterfall history | Zoom, pan, and resize do not unnecessarily erase the past | Retained rows keep their own frequency and time coordinates and are remapped locally |
| 7 | Low-latency resolution changes | Deep-zoom changes respond promptly and continue smoothly | A compatible DSP handoff reuses indexed raw history and rejoins one exact sample timeline |
| 8 | Exact frequency coordinates | Signals are plotted at the frequencies predicted by the hardware tuning model | Correct deterministic PLL rounding in coordinates without hiding unknown oscillator error |
| 9 | Automatic recovery | Temporary backend or receiver loss does not require a manual restart | Preserve explicit Run/Stop intent and reconnect with bounded, observable retries |
| 10 | Simulation-backed development | DSP failures can be reproduced and measured without RF test equipment | Known synthetic inputs must pass; injected sample faults must be detected |

## How People and Agents Should Use These Principles

For a user, the principles describe observable behavior. A user does not need
to know whether the backend selected a 2048-point FFT, an integer CIC factor,
or 55 hop centers. The visible spectrum should remain coherent while those
details change.

For an AI agent or contributor, each principle is a decision filter:

1. Identify which user promise a change affects.
2. Locate the corresponding invariants in this document, `AGENTS.md`,
   `SPEC.MD`, and `SPECTRUM_CALC.MD`.
3. Preserve existing behavior outside the requested scope.
4. Run the listed feedback tests, including live hardware tests when the claim
   depends on Pluto timing or RF behavior.
5. Report what was measured. Do not convert an estimate, a synthetic result,
   or a short smoke test into a claim about long-running live reception.

## 1. AI-First Development

Video: [AI-first development](https://www.youtube.com/watch?v=ZIS-IX3Hf2Q)

### Meaning

AI-first development means choosing work whose breadth, repetition, or time
cost would otherwise make it impractical. It does not mean that an agent owns
the product decisions. The human supplies domain knowledge, decides what a
useful waterfall looks and feels like, notices real RF artifacts, and sets the
acceptance standard. Agents help cross technology boundaries, maintain long
investigations, automate repetitive operations, and turn observations into
repeatable checks.

The original example is one developer bringing C and DSP experience while AI
helps with JavaScript, browser/server integration, Server-Sent Events, GitHub,
and hours of repetitive browser interaction. The value is not code generation
alone; it is the ability to continue from observation through diagnosis,
implementation, and verification.

### How Pluto Scanner Applies It

- The backend and DSP stay in a compact C program, while the browser UI remains
  a self-contained HTML application.
- `AGENTS.md`, `SPEC.MD`, `SPECTRUM_CALC.MD`, and this document preserve intent
  across agent sessions.
- Browser tools exercise controls, zoom ladders, Go To, SSE rendering, and
  recovery repeatedly.
- Numerical tools check planning formulas, CIC continuity, level
  normalization, frequency coordinates, and startup state.
- Live tests remain part of the process because USB timing, RF signals, and
  visual usability cannot be fully represented by source inspection.

### Example

A human notices that an x64 CIC carrier is wider than it was in another
receiver implementation. An agent can trace every sample through the queue and
decimator, add sequence counters, run clean and damaged synthetic tones, and
repeat the same zoom transition hundreds of times. The human then checks that
the corrected live waterfall also looks right. Neither side alone supplies the
whole result.

### Contract for AI Agents

- Read the relevant code and documents before proposing a change.
- Treat user observations as evidence to investigate, not as details to smooth
  over with presentation changes.
- Prefer measurable acceptance criteria over statements such as "looks fixed."
- Keep edits scoped and preserve unrelated user work in a dirty worktree.
- State when hardware, browser, duration, or environment limitations prevent a
  requested test.
- Update documentation when implementation changes invalidate a stated rule.

### Feedback

Use the smallest relevant check first, then widen testing according to risk.
The usual layers are `make ci-check`, `make check`, focused scripts under
`tools/`, browser automation, and a live Pluto run. UI appearance and RF quality
still require human review.

## 2. A Web Interface That Minimizes Traffic

Video: [A web interface that minimizes traffic](https://www.youtube.com/watch?v=8sGSH4dmKbU)

### Meaning

The browser is a portable control and display surface, not a remote DSP engine.
Pluto sends I/Q samples to the C backend. The backend performs tuning, FFT,
CIC, filtering, stitching, calibration, and display reduction. The frontend
receives only the frequency coordinates, metadata, and values needed to draw
the requested spectrum and waterfall.

This keeps the same UI available on desktop and mobile operating systems while
avoiding raw-I/Q network traffic and duplicated browser-side DSP work.

### How Pluto Scanner Applies It

- JSON endpoints carry control and status data.
- `/api/waterfall` uses EventSource/SSE for one-way row delivery.
- Every current row contains exactly `display_bins` values, where
  `display_bins` is the logical CSS width requested by the frontend.
- The row payload is packed `uint8` data encoded as base64 (`u8b64`). The
  frontend still understands legacy `d:[...]` rows for compatibility.
- Raw FFT and CIC dimensions are diagnostic metadata, not oversized frontend
  arrays.
- Device-pixel-ratio changes resize and resample canvas backing stores locally;
  they do not request more RF bins or restart Pluto.
- The traffic indicator measures compressed SSE bytes delivered to one
  frontend stream.

### Example

The video demonstrates about five waterfall lines per second using roughly
25-30 KB/s. A wider browser asks for more logical pixels; a narrow browser asks
for fewer. Neither receives the 61.44 MSPS scan stream. A browser zoom or window
resize can redraw retained rows without causing an SDR retune.

### Contract for AI Agents

- Keep DSP in the backend unless a task explicitly changes the architecture.
- Preserve one output value per visible CSS pixel.
- Do not use high-DPI backing pixels as `display_bins`.
- Do not make canvas resize, browser page zoom, or device-pixel-ratio changes
  restart acquisition.
- Preserve SSE framing and both current `u8b64` and documented legacy row
  decoding when changing transport code.
- Add metadata only when it has a concrete control, display, or diagnostic use.

### Feedback

Run `tools/http_smoke_test.sh`, `tools/headless_tester.py`, and
`tools/ui_browser_test.py`. For row-width and planning changes, also run
`tools/frontend_random_validation.py` and inspect `published_bins`,
`display_bins`, and measured traffic in `/api/status` and SSE metadata.

## 3. One Spectrum with One-Million-Times Zoom

Video: [A spectrum view with very large zoom](https://www.youtube.com/watch?v=fvIV3nouoQ4)

### Meaning

A spectrum tool should not force the operator to choose between a panoramic
scanner and a fine-resolution analyzer. Pluto Scanner treats the configured
band as one frequency coordinate system. `1x` shows that whole band;
`1,000,000x` reduces the visible span by a factor of one million around the
selected point.

Increasing frequency resolution necessarily consumes more samples and lowers
the natural time resolution. The program may adapt acquisition and DSP, but it
must not pretend that a deep FFT is instantaneous or stretch an old low-detail
row as if it were new high-resolution data.

### How Pluto Scanner Applies It

- Wide views use host-controlled scan/hop acquisition at 61.44 MSPS with a
  20 MHz RF bandwidth and 0.85 passband usage.
- Narrow views use one continuous Pluto stream and the smallest hardware RF
  profile that covers the visible span plus the zero-IF guard.
- The single-frequency planner starts with the smallest power-of-two FFT that
  covers the visible CSS pixels. Integer CIC decimation supplies deeper
  resolution when that FFT alone is insufficient.
- A phase-continuous NCO moves zero IF outside the visible band before CIC.
- Rulers, hover coordinates, markers, and rows use the same binary64 hertz
  coordinate transform; text formatting does not feed back into tuning.
- Zoom-in at the maximum and zoom-out at the minimum are strict no-ops.

### Examples

The video moves from a roughly 1 GHz view of QO-100 through its transponders to
a beacon only a few kilohertz wide, then to a carrier and a roughly 400 Hz CW
signal. It also moves from the Pluto tuning range into FM broadcast, DAB, and
cellular activity. These are different resolutions of one tool, not separate
applications or user modes.

At deep zoom, CW keying may become harder to read vertically because one row
represents a longer acquisition interval. That is a real time/frequency
tradeoff, not a rendering bug.

### Contract for AI Agents

- Preserve the `1x..1,000,000x` range and strict endpoint no-op behavior.
- Keep frequency calculations in hertz using sufficient precision; round only
  for display text or the exact integer request written to IIO.
- Choose FFT/CIC coefficients from visible span and logical display width, not
  from status-label formatting or browser backing pixels.
- Minimize FFT size first, then use the smallest feasible integer CIC factor.
- Never improve apparent responsiveness by labelling stretched low-resolution
  data as a newly calculated high-resolution row.

### Feedback

Use `tools/zoom_sweep.py`, `tools/browser_zoom_matrix.py`,
`tools/phase3_browser_stress.py`, and `tools/frontend_random_validation.py`.
Check both wide scan mode and the deepest practical single-frequency CIC view.

## 4. Seamless Scan/Hop and Single-Frequency Reception

Video: [Seamless scan and fixed reception](https://www.youtube.com/watch?v=KnPlvGlBu_s)

### Meaning

The user changes the visible frequency span; the backend chooses how to acquire
it. There is no user-facing mode switch between a stitched scanner and a
continuous receiver. Controls, frequency coordinates, spectrum, and waterfall
remain one interface while the internal plan changes.

### How Pluto Scanner Applies It

In scan/hop mode, the host builds air-frequency centers, converts each center
to a valid Pluto LO frequency, retunes, discards stale buffers, captures one
useful buffer, transforms it, crops the usable passband, and stitches the
result. The normal hop spacing is:

```text
20 MHz * 0.85 = 17 MHz
```

In single-frequency mode, one analog passband covers the view continuously.
The backend selects a conservative sample-rate/RF-bandwidth profile and, when
needed, an NCO plus integer CIC and FFT plan. The UI displays status such as
`SCAN: 55 steps` or `SCAN: Fixed`, but that status is explanatory rather than
a mode control.

### Example

The video starts with about 1 GHz assembled from roughly 55 hop positions,
then zooms through three steps and two steps into fixed reception. Zooming out
reverses the sequence. The expected result is no obvious visual boundary in
the waterfall when the acquisition method changes.

### Contract for AI Agents

- Keep air frequency distinct from Pluto receiver frequency, especially with a
  converter and automatic inverted conversion.
- Keep scan/hop tuning fast and stale-data latency low; continuity between
  separately tuned hop centers is not meaningful.
- Keep continuous single-frequency capture stateful and uninterrupted; it uses
  four queued IIO kernel buffers because CIC state spans refills.
- Do not expose internal mode selection as an ordinary user setting.
- Preserve the visible interval, view generation, level calibration, and
  retained history across the transition.
- Do not import Fobos-specific response compensation or acquisition
  assumptions into the Pluto path.

### Feedback

Run `tools/headless_tester.py`, `tools/ui_browser_test.py`,
`tools/zoom_sweep.py`, and `tools/frontend_random_validation.py`. Verify the
mode boundary in both directions and inspect backend logs for unnecessary
restart or retune loops.

## 5. Waterfall Speed Expressed as a Range

Video: [Waterfall speed FROM and TO lines per second](https://www.youtube.com/watch?v=GDP-NtIiRhI)

### Meaning

The user should describe a useful display cadence, not reverse-engineer FFT
size, sample rate, decimation, and averaging controls. The minimum rate asks
the backend to avoid an unnecessarily frozen display. The maximum rate limits
CPU, network, and rendering load. The result is best effort where physics or
scan retuning imposes a lower rate.

### How Pluto Scanner Applies It

- In deep single-frequency CIC mode, the minimum rate reuses decimated samples
  through overlapping FFT windows.
- The overlap is an integer power of two. It reduces `decim_hop` while keeping
  FFT size, CIC factor, hardware profile, source span, and visible raw-bin
  density unchanged.
- Every published row still comes from exactly one FFT. Intentional overlap
  shares samples between adjacent FFT windows; it is not sample duplication in
  the capture stream.
- The maximum rate throttles complete products. In CIC mode, raw buffers always
  enter the stateful decimator exactly once, and a credit-based limiter acts
  only after an FFT line has been processed.
- Scan mode may drop a complete scan cycle before FFT. Single non-CIC mode may
  drop a whole independent raw FFT buffer. Those paths have no stateful CIC
  continuity to corrupt.
- The FFT status shows the natural, non-overlapped cadence so the physical
  time/frequency tradeoff remains visible.

### Example

A status such as `FFT: 2048 x7761 x128 (0.116 lines/s)` means a 2048-point FFT,
CIC decimation by 7761, and 128-way FFT-window overlap. The value in
parentheses is the base cadence before overlap. Raising the minimum rate may
increase overlap, but it must not choose a smaller CIC factor and silently
reduce frequency resolution.

In a wide 55-hop scan, the backend cannot create more complete RF surveys than
the retune/capture cycle permits. The minimum is therefore a request, while the
maximum remains enforceable.

### Contract for AI Agents

- Never satisfy minimum waterfall rate by reducing FFT size, CIC factor,
  sample rate, RF bandwidth, or source resolution for the same view.
- Never rate-drop, queue-drop, overwrite, skip, or duplicate raw buffers before
  CIC.
- Do not average several FFTs into one waterfall row.
- Keep maximum-rate changes in scan mode in place when no RF replan is needed.
- Expose achieved and natural rates honestly in status and diagnostics.

### Feedback

Run `tools/min_rate_overlap_check.py`, `tools/cic_continuity_check.py`,
`tools/cic_stability_check.py`, and `tools/frontend_random_validation.py`.
For timing behavior, measure SSE arrivals with `tools/zoom_sweep.py`; for CIC
changes, also perform a long live run and inspect sample accounting.

## 6. Persistent Waterfall History

Video: [Persistent waterfall history](https://www.youtube.com/watch?v=wFkcWrQWWDI)

### Meaning

Changing the view should reinterpret useful recorded information rather than
erase it. The waterfall is a time-frequency record. Each historical row knows
the frequency interval and time at which it was acquired, so the frontend can
map the overlapping portion into a new view.

This is not merely retaining one screenshot. If the user zooms in, old rows
must stretch around the same frequency coordinates. If the user zooms out,
they must compress back into their earlier positions. Areas for which a row has
no data must not be invented.

### How Pluto Scanner Applies It

- `index.html` retains waterfall rows and redraws them through the current
  frequency transform.
- Immediate local redraw gives feedback while coalesced backend view updates
  arrange new correctly resolved rows.
- Browser resize and page zoom resample retained rows locally through the
  `ResizeObserver` path instead of clearing history or restarting SDR capture.
- Timestamps are kept nondecreasing. While a tab is hidden, only the newest
  pending row is retained, preventing delayed browser timers from producing a
  burst of out-of-order time marks.
- Spectrum and historical waterfall pixels may be stretched temporarily for
  continuity, but metadata and incoming rows retain their real acquisition
  coordinates.

### Example

The video zooms a 55-step scan inward and outward while earlier signals remain
visible in the waterfall. A browser window resize should have the same
property: history changes scale to fit the new canvas but is not erased solely
because the CSS dimensions changed.

### Contract for AI Agents

- Do not clear history for ordinary zoom, pan, Go To, responsive resize, or
  browser page zoom.
- Store and transform frequency coordinates with each row; do not treat
  history as an unlabelled bitmap.
- Preserve chronological row order and background-tab coalescing.
- Do not restart the backend merely because frontend geometry changed.
- Keep Principle 6 separate from Principle 7: rendered history preserves visual
  context, while indexed raw history can produce a newly calculated FFT.

### Feedback

Run `tools/waterfall_history_check.py`, `tools/ui_browser_test.py`, and
`tools/phase3_browser_stress.py`. Test zoom, pan, browser resize, browser page
zoom, and leaving/returning to the tab; verify both retained signal alignment
and monotonic time marks.

## 7. Low-Latency, Smooth Resolution Changes

Video: [Low-latency smooth resolution changes](https://www.youtube.com/watch?v=HPQgIPxhlEk)

### Meaning

At deep zoom, collecting one full new FFT frame can take seconds. A view change
should not force the user to wait that whole interval when compatible samples
already exist. It should also not emit a fast burst of cached rows followed by
a long pause. The transition must respond promptly and then continue at a
planned, honest cadence.

### How Pluto Scanner Applies It

Continuous high-zoom capture assigns every accepted raw sample an absolute
index and a continuity epoch in a bounded Q15 history ring. For a compatible
view change:

1. Pluto capture and the physical LO continue running.
2. The worker chooses one exact handoff sample index.
3. It initializes the new CIC, Hann window, and FFT generation from history
   ending at that handoff.
4. The first reused row is published with `preview=1`.
5. Samples captured after the handoff are consumed from the same indexed ring.
6. Live queue processing resumes at the exact next sample with `preview=0`.

Compatibility is deliberately conservative. Hardware sample rate and RF
bandwidth must remain unchanged, the target source must fit inside the existing
analog capture, the raw ring must contain the required frame and CIC warm-up,
and the old and new DSP centers must lie inside each other's decimated source
spans. Larger moves use the bounded restart-preview fallback rather than risk
aliasing an unrelated old carrier into the new view.

The planner also obeys a work-minimization rule: FFT size is the smallest power
of two that covers the visible CSS pixels, normally 2048 for an 1800-pixel
view. CIC supplies deeper resolution. Keeping an oversized 65536-point FFT
when 2048 bins cover the screen would waste processing power and make the
handoff harder.

### Example

At maximum zoom, a natural cadence around 0.11 lines/s implies roughly nine
seconds of samples per independent row. Switching between compatible 453 Hz
and 630 Hz views should still produce a reused first row in less than two
seconds, retain one capture epoch and physical LO, and avoid later SSE gaps of
one second or more when minimum-rate overlap is active.

### Contract for AI Agents

- Keep every accepted raw sample exactly once and in order across the handoff.
- Treat a single missing or repeated sample as a correctness failure.
- Never reset the IIO stream or physical LO for a compatible hot replan.
- Keep preview and live rows identifiable through metadata and paced through
  the same frontend presentation path.
- Do not widen compatibility beyond what analog and decimated source coverage
  proves safe.
- Minimize FFT size before selecting CIC decimation.
- Reject burst-then-pause behavior as a failed smoothness requirement even if
  first-row latency alone passes.

### Feedback

Run `tools/cached_preview_check.py` for deterministic indexed handoffs, then
`tools/live_principle7_check.py --duration-seconds 300` with Pluto. Confirm one
capture epoch and physical LO, exact frame ranges, a reused first row below two
seconds, no later one-second SSE gap, zero accounting errors, and no sample
order errors. Use `tools/browser_zoom_matrix.py` to cover browser-driven view
changes.

## 8. Exact Frequency Coordinates

Video: [Exact frequency tuning](https://www.youtube.com/watch?v=_p2u7YL9E5U)

### Meaning

The displayed frequency should represent where the tuned hardware actually
places a signal, within the limits of the known model. Floating-point rounding
is only one source of error. Integer IIO requests, the AD936x clock bridge, and
fractional-N PLL tuning words quantize the requested LO in deterministic ways.

This principle is about accurate coordinates, not a magic calibration value.
Unknown reference-clock error, converter offset, drift, and signal-source error
still require measurement.

### How Pluto Scanner Applies It

With `fq_err_correction=1`, single-frequency mode models the 40 MHz-reference
AD936x RFPLL path used by the stock driver:

- Apply the same integer-hertz `llround()` request written to IIO.
- Apply the AD936x even-hertz clock bridge behavior.
- Use the doubled 80 MHz RFPLL parent, modulus 8388593, the selected VCO
  divider, and the driver's integer tuning-word rounding.
- Compute `actual_lo - unrounded_requested_lo`.
- Convert the signed receiver error through the active converter branch.
- Add the result to source-bin coordinates only.

The correction does not alter the IIO LO request, converter setting, or user
frequency. API and SSE coordinates retain fractional hertz. The visible peak
diagnostic searches only the displayed interval so an off-screen zero-IF peak
cannot replace the carrier being inspected.

### Example

The video uses harmonics of the external 40 MHz reference as known carriers.
The 19th harmonic should appear at exactly 760 MHz. Without the model, the
observed coordinate was displaced by a few tenths of a hertz; with the model,
five-minute tests at multiple frequencies and spans were around 0.01 Hz in the
demonstrated setup. The method corrects deterministic tuning quantization, not
the quality of an unrelated web receiver or oscillator.

### Contract for AI Agents

- Do not introduce a fixed `fq_calibration_hz` residual as a substitute for
  the hardware model.
- Keep the correction in source-bin coordinates; do not silently retune Pluto.
- Preserve fractional-hertz API and SSE values through serialization.
- Apply converter inversion with the correct sign.
- Keep status diagnostics for modeled and effective correction and raw peak
  frequency.
- Do not claim sub-hertz absolute accuracy unless the reference and test source
  support that claim over the measured duration.

### Feedback

Run `tools/frequency_coordinate_check.py` for deterministic formulas and
`tools/live_frequency_coordinate_check.py` for measured carriers. Use a stable
reference, record the configured band and converter, test multiple frequencies
and spans for at least five minutes when that duration is part of the claim,
and compare the visible peak against the expected frequency.

## 9. Automatic Device Recovery

Video: [Automatic device recovery](https://www.youtube.com/watch?v=pBepJdQ0E8A)

### Meaning

A temporary USB disconnect, Pluto failure, or backend restart is a recoverable
operating state. If reception was running, it should resume when the required
component returns. If the user explicitly pressed Stop, recovery must not start
it again.

### How Pluto Scanner Applies It

- Hard IIO failures end the scan thread, close the invalid device context, and
  retain an auto-restart request.
- The main loop polls for a missing device no more often than every five
  seconds.
- A successful reconnect can restart the saved scan without a frontend Run
  request.
- A three-second suppression/debounce rule prevents endless restart loops when
  a device reconnects and immediately fails again.
- `resume_scan` persists explicit Run intent in `pluto-scanner.conf`. Run sets
  it; Stop clears it. A backend restart therefore restores a running receive
  but preserves an intentionally idle state.
- The frontend reconnects its status and EventSource connections and reports
  disconnected, reconnecting, idle, or scanning state without a page reload.
- Expected cancellation-side `-9` messages are distinguished from hard device
  errors.

### Example

The video demonstrates three cases: restarting the backend while reception is
active, unplugging and reconnecting Pluto while the backend remains open, and
starting the backend without hardware before plugging Pluto in. In each case,
the waterfall resumes without a restart button. Pressing Stop is the deliberate
exception.

### Contract for AI Agents

- Preserve the difference between temporary device state and user intent.
- Do not treat expected canceled-buffer errors as a disconnect by themselves.
- Close stale IIO state after a hard error before attempting to reopen it.
- Keep reconnect polling and restart attempts bounded and observable in logs.
- Do not make the frontend send repeated Run requests to implement backend
  recovery.
- Test both resume and no-resume paths, including rapid failure after reconnect.

### Feedback

Run `tools/startup_resume_check.py` and browser/SSE smoke tests. For hardware
changes, physically disconnect and reconnect Pluto, start once without Pluto,
and restart the backend in both Run and Stop states. Inspect logs for one
coherent sequence of close, reconnect, and optional auto-restart rather than a
tight loop.

## 10. Simulation-Backed Development

Video: [Simulation-backed development](https://www.youtube.com/watch?v=KN4PUU8bcSc)

### Meaning

Important DSP behavior should be tested with mathematically known input, not
only by looking at whatever happens to be on the air. A synthetic source makes
the expected tone frequency, amplitude, phase progression, and sample sequence
known. Deliberate damage then proves that the diagnostics and spectral checks
can detect the failures they claim to prevent.

Synthetic sources are development instruments. They are not normal frontend
features and must not replace the Pluto input in release executables.

### How Pluto Scanner Applies It

The developer-only build selected by `PSEUDO_RANDOM_SAMPLE_SOURCE=2` replaces
hardware refill data with one continuous complex tone. Its phase/source index
persists across generated buffers. Environment-controlled fault modes can:

- skip one logical source sample, advancing phase too far; or
- duplicate the preceding source sample, repeating phase once.

The generated samples still travel through the production queue, NCO, bounded
CIC, Hann window, FFT, display reduction, and SSE publication path. Sequence
metadata detects missing or repeated indexes, while spectral checks detect the
resulting energy spread around what should be one narrow carrier.

`tools/cic_synthetic_signal_check.py` checks clean x2, x64, x256, x5581, and
x7761 decimation plans with minimum-rate overlap. Clean tones must remain
narrow and continuous. Skip and duplicate controls must produce sample-order
errors and fail the narrow-spectrum gate. `tools/cic_continuity_check.py` and
`tools/cic_stability_check.py` separately exercise accounting and long-running
bounded CIC behavior.

### Example

A pure bin-centered tone should appear as one coherent carrier with only the
expected Hann main lobe, and it should remain at the same frequency as zoom and
decimation change. Injecting a periodic one-sample discontinuity produces
additional spectral energy and explicit sequence errors. That converts the
claim "samples may be missing" into a repeatable pass/fail result that an agent
can use while debugging.

The same approach applies beyond continuity: known signals can expose wrong
amplitude normalization, frequency offsets, missing or extra output blocks,
scan-segment boundaries, and FFT/decimation artifacts across combinations of
start/end frequency, converter, zoom, and waterfall rate.

### Contract for AI Agents

- Keep synthetic generation phase-continuous across buffer boundaries unless a
  test explicitly injects a fault.
- Test negative controls: a suite that passes clean input but also passes a
  deliberately damaged stream has not proved detection.
- Route synthetic data through production DSP rather than a simplified copy of
  the algorithm.
- Keep the normal `pluto-scanner` binary hardware-backed.
- Keep `pluto-scanner-cic-test` and other synthetic executables developer-only;
  do not ship them or add them to routine GitHub CI without an explicit policy
  change.
- Use synthetic tests before live RF tests, not instead of them.

### Feedback

Run `make check` locally for the developer test binary and full synthetic suite.
Use `make ci-check` for routine GitHub-safe checks. For a DSP fix, report the
clean-tone spectral result, sample accounting, both skip and duplicate negative
controls, the tested FFT/CIC/overlap plans, and a live Pluto result when the bug
was observed on air.

## Keeping the Principles Consistent

Changes often affect more than one principle. For example, increasing
`display_bins` may affect traffic (2), zoom planning (3), rate control (5),
history redraw (6), and hot handoff memory (7). A reconnect change may affect
both recovery (9) and persistent frontend state (6). A frequency-coordinate
change should be tested synthetically (10) and with a known RF source (8).

When a principle appears to conflict with another, preserve the user promises
and make the tradeoff explicit. Do not solve a timing problem by weakening DSP
resolution, solve traffic by dropping CIC input, solve history by mislabelling
stretched data, or solve frequency error with an unexplained constant. Update
this document, the exact specification, formulas, tests, and source together
when the intended behavior genuinely changes.
