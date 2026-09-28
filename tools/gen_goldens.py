#!/usr/bin/env python3
"""Capture parity goldens from the legacy Python implementation.

Run before the C rewrite so the new implementation can be asserted against the
behaviour that shipped. This is a development tool; it is not installed.

Outputs into tests/golden/:
  curve_golden.json  - Bezier samples and duty lookups for every saved curve
  detect_golden.json - the discovery snapshot shape
"""

import json
import math
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import monitor  # noqa: E402

GOLDEN_DIR = Path(__file__).resolve().parent.parent / "tests" / "golden"
SAMPLES_PER_SEGMENT = 32


def load_real_curves():
    """Load the user's live curve store so goldens cover real data."""

    if monitor.CONFIG_PATH.exists():
        return json.loads(monitor.CONFIG_PATH.read_text(encoding="utf-8"))
    return {"curves": {}, "selected_key": None, "auto_apply": False}


def sample_default_curves():
    """Cover the built-in defaults too, not only the user's saved curves."""

    class _Control:
        def __init__(self, label):
            self.label = label
            self.kind = "hwmon"
            self.identifier = "/dev/null"

    out = {}
    for name, label in (("default_fan", "System Fan #1"), ("default_pump", "AIO pump")):
        out[f"builtin:{name}"] = {
            "points": monitor.default_curve_for(_Control(label)),
            "enabled": True,
        }
    return out


def curve_golden():
    """Emit sample tables and duty lookups for every known curve."""

    data = load_real_curves()
    curves = dict(sample_default_curves())
    for key, item in (data.get("curves") or {}).items():
        curves.setdefault(key, item)

    result = {
        "temperature_range_c": [monitor.TEMP_HARD_MIN, monitor.TEMP_HARD_MAX],
        "lookup_temps_c": [round(-5 + 0.5 * i, 1) for i in range(0, 241)],
        "samples_per_segment": SAMPLES_PER_SEGMENT,
        "curves": {},
    }

    for key, item in sorted(curves.items()):
        points = monitor.normalize_curve_points(item.get("points", []))
        if not points:
            continue
        samples = monitor.curve_samples(points)
        result["curves"][key] = {
            "points": [list(p) for p in points],
            "sample_count": len(samples),
            # Full float sample table: the C evaluation must match to 1e-9.
            "samples": [[round(x, 9), round(y, 9)] for x, y in samples],
            "duty": {
                f"{t}": monitor.duty_from_curve(points, t)
                for t in result["lookup_temps_c"]
            },
        }

    return result


def detect_golden():
    """Emit the discovery snapshot so the C port can be diffed against it."""

    backend = monitor.SensorBackend()
    snapshot = backend.discover()
    return {
        "temps": [t.__dict__ for t in snapshot["temps"]],
        "fans": [f.__dict__ for f in snapshot["fans"]],
        "controls": [c.__dict__ for c in snapshot["controls"]],
        "cpu_temp_c": snapshot["cpu_temp_c"],
        "liquidctl": snapshot["liquidctl"],
    }


def main():
    os.makedirs(GOLDEN_DIR, exist_ok=True)

    curve = curve_golden()
    (GOLDEN_DIR / "curve_golden.json").write_text(
        json.dumps(curve, indent=1, sort_keys=True), encoding="utf-8"
    )
    print(f"curve golden: {len(curve['curves'])} curves")

    detect = detect_golden()
    (GOLDEN_DIR / "detect_golden.json").write_text(
        json.dumps(detect, indent=1, sort_keys=True), encoding="utf-8"
    )
    print(
        f"detect golden: {len(detect['temps'])} temps, "
        f"{len(detect['fans'])} fans, {len(detect['controls'])} controls"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
