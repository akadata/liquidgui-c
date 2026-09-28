#!/usr/bin/env python3
"""Compare the C discovery output against the legacy Python golden file.

The two implementations are expected to differ. This script exists so the
differences are reviewed rather than assumed, and so a regression that is *not*
an intended change still shows up.

Intended differences:
  - Controls are keyed on driver identity rather than the absolute sysfs path.
  - The Kraken AIO appears as hwmon controls instead of liquidctl pseudo-keys.
  - Dead temperature channels are marked invalid instead of being reported.
  - Voltages, currents and thresholds are read in addition to temps and fans.
"""

import json
import sys


def load(path):
    with open(path, encoding="utf-8") as handle:
        return json.load(handle)


def main():
    if len(sys.argv) < 3:
        print("usage: compare_detect.py <golden.json> <new.json>")
        return 2

    golden = load(sys.argv[1])
    new = load(sys.argv[2])

    print("=" * 72)
    print("discovery comparison: legacy Python vs C rewrite")
    print("=" * 72)

    # --- controls ---
    g_ctrl = golden.get("controls", [])
    n_ctrl = new.get("controls", [])
    print(f"\ncontrols: {len(g_ctrl)} -> {len(n_ctrl)}")
    print("  legacy keys:")
    for c in g_ctrl[:4]:
        print(f"    {c.get('identifier', c.get('key'))}")
    if len(g_ctrl) > 4:
        print(f"    ... and {len(g_ctrl) - 4} more")
    print("  new keys:")
    for c in n_ctrl:
        tag = "AIO " if c.get("is_aio") else "    "
        print(f"  {tag} {c['key']:24} {c['label']:16} duty={c['duty']}")

    n_aio = sum(1 for c in n_ctrl if c.get("is_aio"))
    g_aio = sum(1 for c in g_ctrl if c.get("kind") == "liquidctl")
    print(f"\n  AIO controls: {g_aio} -> {n_aio}")
    if n_aio > g_aio:
        print("  OK: the AIO is now reachable as first-class hwmon controls")
    unstable = [c for c in n_ctrl if "hwmon" in c["key"] and "/sys/" in c["key"]]
    print(f"  keys containing an absolute sysfs path: {len(unstable)}")
    if not unstable:
        print("  OK: every key is boot-stable")

    # --- sensors ---
    print("\nsensors by class:")
    g_keys = {
        "Temperature": len(golden.get("temps", [])),
        "Fan speed": len(golden.get("fans", [])),
        "Voltage": 0,
    }
    n_keys = {
        "Temperature": len(new.get("Temperature_list", [])),
        "Fan speed": len(new.get("Fan speed_list", [])),
        "Voltage": len(new.get("Voltage_list", [])),
        "Current": len(new.get("Current_list", [])),
        "Power": len(new.get("Power_list", [])),
    }
    for name in ("Temperature", "Fan speed", "Voltage", "Current", "Power"):
        before = g_keys.get(name, "n/a")
        after = n_keys.get(name, 0)
        extra = ""
        if before == 0 and after:
            extra = "  <- newly read"
        print(f"  {name:12} {before:>5} -> {after:<5}{extra}")

    # --- validity filtering ---
    invalid = [s for s in new.get("Temperature_list", []) if not s.get("valid", True)]
    print(f"\ntemperature channels rejected as implausible: {len(invalid)}")
    for s in invalid:
        print(f"  {s['chip']:10} {s['label']:22} = {s['value']:>8}  ({s.get('note', '')})")

    # A channel the legacy view reported verbatim as a real temperature.
    legacy_bogus = {"PCIe x1": 193.0, "Virtual 0": -63.0, "M2_1": 0.0}
    shown_before = [
        s["label"]
        for s in invalid
        if s["label"] in legacy_bogus and abs(s["value"] - legacy_bogus[s["label"]]) < 0.01
    ]
    if shown_before:
        print(f"  OK: {len(shown_before)} channel(s) the legacy view displayed as real "
              "readings are now filtered")

    print(f"\ntotals: {golden.get('nsensors', len(golden.get('temps', [])) + len(golden.get('fans', [])))}"
          f" -> {new.get('nsensors', '?')} sensors,"
          f" {len(g_ctrl)} -> {len(n_ctrl)} controls")
    print()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
