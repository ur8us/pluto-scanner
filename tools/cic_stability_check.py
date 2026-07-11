#!/usr/bin/env python3
"""Check bounded CIC decimator stability for narrow single-mode plans."""

import json
import math
import sys


CIC_STAGES = 3
RAW_SAMPLE_RATE = 4_000_000.0
TONE_HZ = 12_345.0
TONE_AMP = 0.2
DC_OFFSET = complex(0.01, -0.007)
OUTPUTS_PER_DECIM = 60_000
WARMUP_OUTPUTS = 4_096
BLOCK_OUTPUTS = 4_096


def cic_gain(decim, freq_hz, raw_samplerate):
    """Return normalized CIC magnitude for one frequency."""
    x = math.pi * freq_hz / raw_samplerate
    if abs(x) < 1.0e-12:
        return 1.0
    den = decim * math.sin(x)
    if abs(den) < 1.0e-18:
        return 1.0
    return abs(math.sin(decim * x) / den) ** CIC_STAGES


def run_decim(decim):
    """Run a bounded moving-sum CIC simulation and return stability metrics."""
    rings = [[0j] * decim for _ in range(CIC_STAGES)]
    sums = [0j] * CIC_STAGES
    max_stage_abs = [0.0] * CIC_STAGES
    pos = 0
    phase_count = 0
    output_count = 0
    gain = float(decim**CIC_STAGES)
    blocks = []
    corr = 0j
    corr_count = 0
    raw_total = OUTPUTS_PER_DECIM * decim

    for n in range(raw_total):
        phase = 2.0 * math.pi * TONE_HZ * float(n) / RAW_SAMPLE_RATE
        stage = DC_OFFSET + TONE_AMP * complex(math.cos(phase), math.sin(phase))

        for stage_index in range(CIC_STAGES):
            old = rings[stage_index][pos]
            sums[stage_index] += stage - old
            rings[stage_index][pos] = stage
            stage = sums[stage_index]
            max_stage_abs[stage_index] = max(max_stage_abs[stage_index],
                                             abs(stage))

        pos += 1
        if pos >= decim:
            pos = 0

        phase_count += 1
        if phase_count < decim:
            continue
        phase_count = 0

        output = stage / gain
        if output_count >= WARMUP_OUTPUTS:
            ref = complex(math.cos(-phase), math.sin(-phase))
            corr += (output - DC_OFFSET) * ref
            corr_count += 1
            if corr_count == BLOCK_OUTPUTS:
                blocks.append(abs(corr) / float(corr_count))
                corr = 0j
                corr_count = 0
        output_count += 1

    if corr_count >= BLOCK_OUTPUTS // 2:
        blocks.append(abs(corr) / float(corr_count))

    expected = TONE_AMP * cic_gain(decim, TONE_HZ, RAW_SAMPLE_RATE)
    mean = sum(blocks) / float(len(blocks))
    drift = (max(blocks) - min(blocks)) / mean if mean > 0.0 else 1.0
    expected_error = abs(mean - expected) / expected if expected > 0.0 else 0.0
    state_limit = [
        (abs(DC_OFFSET) + TONE_AMP) * float(decim ** (stage + 1)) * 1.01
        for stage in range(CIC_STAGES)
    ]
    bounded = all(max_stage_abs[i] <= state_limit[i] for i in range(CIC_STAGES))

    return {
        "decim": decim,
        "blocks": len(blocks),
        "mean_amplitude": mean,
        "expected_amplitude": expected,
        "relative_drift": drift,
        "relative_expected_error": expected_error,
        "max_stage_abs": max_stage_abs,
        "state_limit": state_limit,
        "bounded": bounded,
    }


def main():
    """Run the CIC stability checks and print machine-readable results."""
    quiet = "--quiet" in sys.argv
    rows = [run_decim(decim) for decim in (2, 4, 8, 64, 256)]
    errors = []
    for row in rows:
        if row["relative_drift"] > 1.0e-6:
            errors.append(f"decim {row['decim']} amplitude drift")
        if row["relative_expected_error"] > 1.0e-3:
            errors.append(f"decim {row['decim']} gain mismatch")
        if not row["bounded"]:
            errors.append(f"decim {row['decim']} state exceeded bound")

    result = {"status": "ok" if not errors else "error",
              "errors": errors,
              "rows": rows}
    if errors or not quiet:
        print(json.dumps(result, indent=2, sort_keys=True))
    return 0 if not errors else 1


if __name__ == "__main__":
    sys.exit(main())
