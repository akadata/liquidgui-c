#!/usr/bin/env python3
#
# (C) 2024-2026 AKADATA LIMITED - Andrew Smalley
# Released under the MIT License.
# 
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:

# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.


"""LiquidGUI runtime, sensor discovery, curve storage, and Tk interface."""

import argparse
import json
import math
import stat
import signal
import subprocess
import sys
import time
import tkinter as tk
from dataclasses import dataclass
from pathlib import Path
from tkinter import messagebox, ttk


HWMON_ROOT = Path("/sys/class/hwmon")
WATCH_FILES = ("liquidgui", "monitor.py")
REFRESH_SECONDS = 3
TEMP_MIN = 20
TEMP_MAX = 90
TEMP_HARD_MIN = 0
TEMP_HARD_MAX = 110
CONFIG_PATH = Path.home() / ".config" / "liquidgui_curves.json"
LEGACY_CONFIG_PATH = Path.home() / ".config" / "liquidctl_curves.json"
SUDO_PREFIX = ["sudo"]


@dataclass
class TempSensor:
    """A readable temperature sensor exposed through hwmon."""

    chip: str
    label: str
    path: str
    value_c: float | None


@dataclass
class FanSensor:
    """A readable fan RPM sensor exposed through hwmon."""

    chip: str
    label: str
    path: str
    rpm: int | None


@dataclass
class ControlChannel:
    """A writable control target backed by liquidctl or hwmon PWM."""

    chip: str
    label: str
    kind: str
    identifier: str
    duty: int | None
    min_duty: int = 0
    max_duty: int = 100

    @property
    def key(self):
        """Return the persistent storage key for this control channel."""

        return f"{self.kind}:{self.identifier}"


@dataclass
class CurveConfig:
    """Saved Bezier control points and enabled state for one control."""

    points: list[tuple[int, int]]
    enabled: bool = True


class LiquidGUIError(RuntimeError):
    """Application-level error raised for discovery or control failures."""

    pass


def is_generic_hwmon_label(label):
    """Return True when a hwmon label is only a generic fan or pwm name."""

    if not label:
        return True

    lowered = label.strip().lower()
    return lowered.startswith("fan") or lowered.startswith("pwm")


def is_writable_hwmon_pwm(path):
    """Return True when a hwmon PWM node is writable by the driver."""

    try:
        mode = Path(path).stat().st_mode
    except OSError:
        return False

    return bool(mode & stat.S_IWUSR)


def read_text(path):
    """Read a sysfs-style text file and return None on read failure."""

    try:
        return Path(path).read_text(encoding="utf-8").strip()
    except OSError:
        return None


def read_int(path):
    """Read an integer file and return None on missing or invalid content."""

    text = read_text(path)
    if text is None:
        return None

    try:
        return int(text)
    except ValueError:
        return None


def celsius_from_millidegrees(value):
    """Convert a millidegree reading to a rounded Celsius float."""

    if value is None:
        return None

    return round(value / 1000.0, 1)


def default_curve_for(control):
    """Return the default curve for a newly discovered control."""

    base = [(30, 30), (40, 38), (50, 50), (60, 72), (72, 100)]
    if "pump" in control.label.lower():
        return [(30, 70), (40, 78), (50, 86), (60, 94), (72, 100)]
    return base


def normalize_curve_points(points):
    """Clamp and normalize curve points into valid temperature and duty ranges."""

    normalized = []
    for point in points:
        temp = int(point[0])
        duty = int(point[1])

        if normalized:
            temp = max(normalized[-1][0], temp)
        temp = max(TEMP_HARD_MIN, min(TEMP_HARD_MAX, temp))

        duty = max(0, min(100, duty))
        normalized.append((temp, duty))

    return normalized


def cubic_bezier_point(p0, p1, p2, p3, t):
    """Sample a cubic Bezier segment at the normalized position ``t``."""

    inv = 1.0 - t
    inv2 = inv * inv
    t2 = t * t
    x = (
        (inv2 * inv * p0[0])
        + (3.0 * inv2 * t * p1[0])
        + (3.0 * inv * t2 * p2[0])
        + (t2 * t * p3[0])
    )
    y = (
        (inv2 * inv * p0[1])
        + (3.0 * inv2 * t * p1[1])
        + (3.0 * inv * t2 * p2[1])
        + (t2 * t * p3[1])
    )
    return (x, max(0.0, min(100.0, y)))


def bezier_curve_samples(points, samples_per_segment=32):
    """Sample a smooth interpolating Bezier path through the control points."""

    if not points:
        return []

    if len(points) == 1:
        return [tuple(points[0])]

    samples = [tuple(points[0])]
    for index in range(len(points) - 1):
        p0 = points[index - 1] if index > 0 else points[index]
        p1 = points[index]
        p2 = points[index + 1]
        p3 = points[index + 2] if index + 2 < len(points) else points[index + 1]

        b0 = p1
        b1 = (
            p1[0] + ((p2[0] - p0[0]) / 6.0),
            p1[1] + ((p2[1] - p0[1]) / 6.0),
        )
        b2 = (
            p2[0] - ((p3[0] - p1[0]) / 6.0),
            p2[1] - ((p3[1] - p1[1]) / 6.0),
        )
        b3 = p2

        for step in range(1, samples_per_segment + 1):
            t = step / samples_per_segment
            samples.append(cubic_bezier_point(b0, b1, b2, b3, t))

    return samples


def curve_samples(points):
    """Return sampled points for the rendered and evaluated control curve."""

    return bezier_curve_samples(points)


def duty_from_curve(points, temp_c):
    """Evaluate the curve at the provided temperature and return duty percent."""

    samples = curve_samples(points)
    if not samples:
        return 0

    if temp_c <= samples[0][0]:
        return int(round(samples[0][1]))

    if temp_c >= samples[-1][0]:
        return int(round(samples[-1][1]))

    best = samples[-1][1]
    for left, right in zip(samples, samples[1:]):
        min_x = min(left[0], right[0])
        max_x = max(left[0], right[0])
        if min_x <= temp_c <= max_x:
            span = right[0] - left[0]
            if abs(span) < 1e-9:
                best = max(left[1], right[1])
                continue
            ratio = (temp_c - left[0]) / span
            duty = left[1] + ((right[1] - left[1]) * ratio)
            return int(round(max(0, min(100, duty))))

    return int(round(best))


class SensorBackend:
    """Discover sensors and apply control changes through liquidctl or hwmon."""

    def discover(self):
        """Return the current snapshot of temperatures, fans, and controls."""

        liquidctl = self._liquidctl_status()
        snapshot = {
            "temps": self._discover_temps(),
            "fans": self._discover_fans(),
            "controls": self._discover_controls(liquidctl),
            "liquidctl": liquidctl,
        }
        snapshot["cpu_temp_c"] = self._pick_cpu_temp(snapshot["temps"])
        return snapshot

    def apply_control(self, control, duty_percent):
        """Apply a duty percentage to a single control channel."""

        if control.kind == "liquidctl":
            self._apply_liquidctl(control.identifier, duty_percent)
            return

        if control.kind == "hwmon":
            self._apply_hwmon(control.identifier, duty_percent)
            return

        raise LiquidGUIError(f"Unsupported control kind: {control.kind}")

    def _discover_temps(self):
        """Enumerate readable temperature sensors from hwmon."""

        temps = []
        for hwmon in sorted(HWMON_ROOT.glob("hwmon*")):
            chip = read_text(hwmon / "name") or hwmon.name
            for input_path in sorted(hwmon.glob("temp*_input")):
                index = input_path.stem.split("_")[0][4:]
                label = read_text(hwmon / f"temp{index}_label") or f"temp{index}"
                value_c = celsius_from_millidegrees(read_int(input_path))
                temps.append(TempSensor(chip=chip, label=label, path=str(input_path), value_c=value_c))
        return temps

    def _discover_fans(self):
        """Enumerate readable fan RPM sensors from hwmon."""

        fans = []
        for hwmon in sorted(HWMON_ROOT.glob("hwmon*")):
            chip = read_text(hwmon / "name") or hwmon.name
            for input_path in sorted(hwmon.glob("fan*_input")):
                index = input_path.stem.split("_")[0][3:]
                label = read_text(hwmon / f"fan{index}_label") or f"fan{index}"
                fans.append(FanSensor(chip=chip, label=label, path=str(input_path), rpm=read_int(input_path)))
        return fans

    def _discover_controls(self, liquidctl):
        """Enumerate writable AIO and motherboard fan controls."""

        controls = []

        if liquidctl:
            for channel in liquidctl["channels"]:
                controls.append(
                    ControlChannel(
                        chip=liquidctl["device"],
                        label=channel["label"],
                        kind="liquidctl",
                        identifier=channel["name"],
                        duty=channel["duty"],
                    )
                )

        motherboard_index = 1
        for hwmon in sorted(HWMON_ROOT.glob("hwmon*")):
            chip = read_text(hwmon / "name") or hwmon.name
            if chip == "kraken2023":
                continue

            for pwm_path in sorted(hwmon.glob("pwm[0-9]")):
                if not is_writable_hwmon_pwm(pwm_path):
                    continue

                suffix = pwm_path.name[3:]
                fan_label = read_text(hwmon / f"fan{suffix}_label")
                label = "" #f"{motherboard_index}"
                if fan_label and not is_generic_hwmon_label(fan_label):
                    label = f"{label} ({fan_label})"
                pwm_raw = read_int(pwm_path)
                duty = None if pwm_raw is None else round((pwm_raw / 255.0) * 100)
                controls.append(
                    ControlChannel(
                        chip=chip,
                        label=label,
                        kind="hwmon",
                        identifier=str(pwm_path),
                        duty=duty,
                    )
                )
                motherboard_index += 1

        return controls

    def _pick_cpu_temp(self, temps):
        """Choose the best CPU temperature source for curve evaluation."""

        package_sensor = next(
            (
                sensor
                for sensor in temps
                if sensor.chip == "coretemp" and sensor.label.lower() == "package id 0"
            ),
            None,
        )
        if package_sensor is not None:
            return package_sensor.value_c

        coretemp_sensor = next((sensor for sensor in temps if sensor.chip == "coretemp"), None)
        if coretemp_sensor is not None:
            return coretemp_sensor.value_c

        return next((sensor.value_c for sensor in temps if sensor.value_c is not None), None)

    def _liquidctl_status(self):
        """Read liquidctl status output and normalize it into channel data."""

        try:
            result = subprocess.run(
                ["liquidctl", "status"],
                capture_output=True,
                text=True,
                check=False,
            )
        except FileNotFoundError:
            return None

        if result.returncode != 0:
            return None

        device = None
        channels = []
        telemetry = {}

        for raw_line in result.stdout.splitlines():
            line = raw_line.strip()
            if not line:
                continue

            is_tree_line = line.startswith("├──") or line.startswith("└──")
            if is_tree_line:
                line = line[3:].strip()
            elif device is None:
                device = line
                continue

            parts = line.split()
            if len(parts) < 3:
                continue

            name = " ".join(parts[:-2])
            value = parts[-2]
            unit = parts[-1]

            try:
                numeric = float(value)
            except ValueError:
                continue

            key = name.lower()
            telemetry[key] = {"value": numeric, "unit": unit}

            if key == "fan duty":
                channels.append({"name": "fan", "label": "AIO fan", "duty": round(numeric)})
            if key == "pump duty":
                channels.append({"name": "pump", "label": "AIO pump", "duty": round(numeric)})

        if not device:
            return None

        return {"device": device, "channels": channels, "telemetry": telemetry}

    def _apply_liquidctl(self, channel, duty_percent):
        """Apply a duty percentage to a liquidctl-backed control channel."""

        result = subprocess.run(
            SUDO_PREFIX + ["liquidctl", "set", channel, "speed", str(int(duty_percent))],
            capture_output=True,
            text=True,
            check=False,
        )
        if result.returncode == 0:
            return

        detail = (result.stderr or result.stdout).strip()
        if not detail:
            detail = f"liquidctl exited with status {result.returncode}"
        if "insufficient permissions" in detail.lower():
            detail += "\nGrant device access with your existing udev rule or run the app with the privileges needed for liquidctl control."
        raise LiquidGUIError(detail)

    def _apply_hwmon(self, pwm_path, duty_percent):
        """Apply a duty percentage to a hwmon PWM control node."""

        pwm = Path(pwm_path)
        enable = pwm.with_name(f"{pwm.name}_enable")
        raw_value = str(max(0, min(255, round((duty_percent / 100.0) * 255))))

        try:
            if enable.exists():
                self._sudo_write_text(enable, "1\n")
            self._sudo_write_text(pwm, f"{raw_value}\n")
        except OSError as exc:
            raise LiquidGUIError(str(exc)) from exc

    def _sudo_write_text(self, path, value):
        """Write text to a privileged file path using ``sudo tee``."""

        result = subprocess.run(
            SUDO_PREFIX + ["tee", str(path)],
            input=value,
            capture_output=True,
            text=True,
            check=False,
        )
        if result.returncode != 0:
            detail = (result.stderr or result.stdout).strip()
            if not detail:
                detail = f"sudo tee exited with status {result.returncode}"
            raise LiquidGUIError(detail)


class CurveStore:
    """Load, migrate, and persist per-control curve configuration."""

    def __init__(self):
        self.path = CONFIG_PATH
        self.selected_key = None
        self.auto_apply = False
        self.curves = {}
        self._load()

    def _load(self):
        """Load the current curve store or migrate the legacy config format."""

        data = None
        if self.path.exists():
            try:
                data = json.loads(self.path.read_text(encoding="utf-8"))
            except (OSError, json.JSONDecodeError):
                data = None
        elif LEGACY_CONFIG_PATH.exists():
            data = self._load_legacy()

        if not data:
            return

        self.selected_key = data.get("selected_key")
        self.auto_apply = bool(data.get("auto_apply", False))
        curves = data.get("curves", {})
        for key, item in curves.items():
            points = item.get("points", [])
            if points:
                self.curves[key] = CurveConfig(
                    points=normalize_curve_points(points),
                    enabled=bool(item.get("enabled", True)),
                )

    def _load_legacy(self):
        """Translate the old liquidctl curve file into the new storage shape."""

        try:
            data = json.loads(LEGACY_CONFIG_PATH.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            return None

        curves = {}
        fan_points = data.get("fan")
        pump_points = data.get("pump")
        if fan_points:
            curves["liquidctl:fan"] = {"points": fan_points, "enabled": True}
        if pump_points:
            curves["liquidctl:pump"] = {"points": pump_points, "enabled": True}
        return {
            "selected_key": "liquidctl:fan" if fan_points else "liquidctl:pump",
            "auto_apply": False,
            "curves": curves,
        }

    def save(self):
        """Persist the current curve configuration to disk."""

        self.path.parent.mkdir(parents=True, exist_ok=True)
        payload = {
            "selected_key": self.selected_key,
            "auto_apply": self.auto_apply,
            "curves": {
                key: {
                    "points": config.points,
                    "enabled": config.enabled,
                }
                for key, config in self.curves.items()
            },
        }
        self.path.write_text(json.dumps(payload, indent=2), encoding="utf-8")

    def get_curve(self, control):
        """Return the saved curve for a control, creating a default if needed."""

        config = self.curves.get(control.key)
        if config is None:
            config = CurveConfig(points=default_curve_for(control))
            self.curves[control.key] = config
        return config


class LiquidGUI:
    """Tk application for editing and applying Bezier fan curves."""

    def __init__(self, root, backend):
        self.root = root
        self.backend = backend
        self.snapshot = None
        self.controls = {}
        self.fan_map = {}
        self.curves = CurveStore()
        self.selected_key = None
        self.drag_index = None
        self.last_auto_apply_at = 0.0
        self.auto_apply_var = tk.BooleanVar(value=self.curves.auto_apply)
        self.info_var = tk.StringVar(value="")
        self.cpu_var = tk.StringVar(value="CPU package: n/a")
        self.status_text = None
        self.status_columns = []
        self.control_list = None
        self.canvas = None
        self.details_var = tk.StringVar(value="")
        self.auto_status_var = tk.StringVar(value="Auto apply disabled\n")
        self.pane_ratio = 0.34
        self.pane_resize_after_id = None

        self.root.title("LiquidGUI")
        self.root.geometry("900x620")
        self.root.minsize(760, 520)
        self._build_ui()
        self._refresh_loop()
        self.root.protocol("WM_DELETE_WINDOW", self.close)

    def _build_ui(self):
        """Create the application layout and bind the interactive widgets."""

        self.root.grid_rowconfigure(1, weight=1)
        self.root.grid_columnconfigure(0, weight=1)

        header = ttk.Frame(self.root, padding=(10, 10, 10, 6))
        header.grid(row=0, column=0, sticky="ew")
        header.grid_columnconfigure(1, weight=1)

        ttk.Label(header, textvariable=self.cpu_var, font=("TkDefaultFont", 12, "bold")).grid(row=0, column=0, sticky="w")
        ttk.Checkbutton(
            header,
            text="Auto apply curves",
            variable=self.auto_apply_var,
            command=self._toggle_auto_apply,
        ).grid(row=0, column=1, sticky="e")
        self.auto_status_label = ttk.Label(
            header,
            textvariable=self.auto_status_var,
            justify=tk.LEFT,
            anchor="w",
        )
        self.auto_status_label.grid(row=1, column=0, columnspan=2, sticky="w", pady=(4, 0))

        main = ttk.PanedWindow(self.root, orient=tk.HORIZONTAL)
        main.grid(row=1, column=0, sticky="nsew", padx=10, pady=(0, 10))

        left = ttk.Frame(main, padding=8, width=320)
        left.grid_columnconfigure(0, weight=1)
        left.grid_rowconfigure(1, weight=1)
        main.add(left, weight=1)

        ttk.Label(left, text="Detected controls").grid(row=0, column=0, sticky="w")
        self.control_list = tk.Listbox(left, exportselection=False, height=10, width=34)
        self.control_list.grid(row=1, column=0, sticky="nsew", pady=(6, 8))
        self.control_list.bind("<<ListboxSelect>>", self._on_control_selected)

        button_row = ttk.Frame(left)
        button_row.grid(row=2, column=0, sticky="ew")
        button_row.grid_columnconfigure(0, weight=1)
        button_row.grid_columnconfigure(1, weight=1)
        ttk.Button(button_row, text="Apply selected", command=self._apply_selected_curve).grid(row=0, column=0, sticky="ew", padx=(0, 4))
        ttk.Button(button_row, text="Apply all", command=self._apply_all_curves).grid(row=0, column=1, sticky="ew", padx=(4, 0))
        ttk.Button(left, text="Reset selected curve", command=self._reset_selected_curve).grid(row=3, column=0, sticky="ew", pady=(8, 0))
        self.selected_enabled = tk.BooleanVar(value=True)
        self.selected_enabled_button = ttk.Checkbutton(
            left,
            text="Selected curve enabled",
            variable=self.selected_enabled,
            command=self._toggle_selected_curve,
        )
        self.selected_enabled_button.grid(row=4, column=0, sticky="w", pady=(8, 0))

        right = ttk.Frame(main, padding=8)
        right.grid_rowconfigure(1, weight=1)
        right.grid_columnconfigure(0, weight=1)
        main.add(right, weight=2)

        main.bind("<Configure>", lambda _event: self._schedule_pane_ratio(main))
        self.root.after(150, lambda: self._apply_pane_ratio(main))

        ttk.Label(right, textvariable=self.info_var, justify=tk.LEFT).grid(row=0, column=0, sticky="ew")
        self.canvas = tk.Canvas(right, background="#101214", height=260, highlightthickness=0)
        self.canvas.grid(row=1, column=0, sticky="nsew", pady=(8, 8))
        self.canvas.bind("<Configure>", lambda _event: self._draw_curve())
        self.canvas.bind("<ButtonPress-1>", self._on_canvas_press)
        self.canvas.bind("<B1-Motion>", self._on_canvas_drag)
        self.canvas.bind("<ButtonRelease-1>", self._on_canvas_release)

        details = ttk.Label(right, textvariable=self.details_var, justify=tk.LEFT)
        details.grid(row=2, column=0, sticky="ew")

        status_frame = ttk.LabelFrame(self.root, text="Sensor summary", padding=8)
        status_frame.grid(row=2, column=0, sticky="nsew", padx=10, pady=(0, 10))
        status_frame.grid_rowconfigure(0, weight=1)

        self.status_columns = []
        for column in range(3):
            status_frame.grid_columnconfigure(column, weight=1, uniform="status")
            text = tk.Text(
                status_frame,
                height=10,
                state=tk.DISABLED,
                wrap=tk.NONE,
                width=1,
                font=("TkFixedFont", 9),
            )
            text.grid(
                row=0,
                column=column,
                sticky="nsew",
                padx=(0 if column == 0 else 6, 0),
            )
            self.status_columns.append(text)

    def _schedule_pane_ratio(self, paned):
        """Debounce pane resize handling so the left/right split remains stable."""

        if self.pane_resize_after_id is not None:
            self.root.after_cancel(self.pane_resize_after_id)

        self.pane_resize_after_id = self.root.after(
            80,
            lambda: self._apply_pane_ratio(paned),
        )

    def _apply_pane_ratio(self, paned):
        """Keep the PanedWindow split at the configured left/right ratio."""

        self.pane_resize_after_id = None
        paned.update_idletasks()
        width = paned.winfo_width()

        if width < 300:
            return

        paned.sashpos(0, int(width * self.pane_ratio))


    def _refresh_loop(self):
        """Refresh sensor state and optionally auto-apply curves on a timer."""

        try:
            snapshot = self.backend.discover()
            self.snapshot = snapshot
            self._render(snapshot)
            if self.auto_apply_var.get():
                self._auto_apply_if_needed()
        except Exception as exc:
            self._set_status_text(f"Refresh failed:\n{exc}")

        self.root.after(REFRESH_SECONDS * 1000, self._refresh_loop)

    def _render(self, snapshot):
        """Render a fresh sensor/control snapshot into the UI."""

        cpu = snapshot["cpu_temp_c"]
        if cpu is None:
            self.cpu_var.set("CPU package: n/a")
        else:
            self.cpu_var.set(f"CPU package: {cpu:.1f} C")

        self.controls = {control.key: control for control in snapshot["controls"]}
        self.fan_map = {f"{fan.chip}:{fan.label}": fan for fan in snapshot["fans"]}
        self._sync_control_list()
        self._render_status_text(snapshot)
        self._update_editor_labels()
        self._draw_curve()

    def _sync_control_list(self):
        """Rebuild the control list while preserving the current selection."""

        previous = self.selected_key or self.curves.selected_key
        items = list(self.controls.values())
        self.control_list.delete(0, tk.END)
        for control in items:
            current = "n/a" if control.duty is None else f"{control.duty}%"
            enabled = "on" if self.curves.get_curve(control).enabled else "off"
            self.control_list.insert(tk.END, f"{control.label} [{current}] curve {enabled}")

        if not items:
            self.selected_key = None
            self.curves.selected_key = None
            self.info_var.set("No writable fan or pump controls were detected.")
            self.selected_enabled.set(False)
            return

        if previous not in self.controls:
            previous = items[0].key
        self.selected_key = previous
        self.curves.selected_key = previous
        self.curves.save()

        for index, control in enumerate(items):
            if control.key == previous:
                self.control_list.selection_clear(0, tk.END)
                self.control_list.selection_set(index)
                self.control_list.see(index)
                break

        self.selected_enabled.set(self.curves.get_curve(self.controls[self.selected_key]).enabled)

    def _render_status_text(self, snapshot):
        """Render the sensor summary panel across three columns."""

        temp_lines = []
        for sensor in snapshot["temps"]:
            value = "n/a" if sensor.value_c is None else f"{sensor.value_c:.1f} C"
            temp_lines.append(f"{sensor.chip}: {sensor.label} = {value}")

        fan_lines = []
        for sensor in snapshot["fans"]:
            value = "n/a" if sensor.rpm is None else f"{sensor.rpm} RPM"
            fan_lines.append(f"{sensor.chip}: {sensor.label} = {value}")

        control_lines = []
        for control in snapshot["controls"]:
            value = "n/a" if control.duty is None else f"{control.duty}%"
            control_lines.append(f"{control.chip}: {control.label} = {value}")

        midpoint = (len(temp_lines) + 1) // 2

        first_column = ["Temperatures"] + [f"  {line}" for line in temp_lines[:midpoint]]

        second_column = []
        if temp_lines[midpoint:]:
            second_column = ["Temperatures"] + [f"  {line}" for line in temp_lines[midpoint:]]

        third_column = ["Fans"] + [f"  {line}" for line in fan_lines]

        if control_lines:
            third_column.extend(["", "Controls"])
            third_column.extend(f"  {line}" for line in control_lines)

        if not any(control.kind == "hwmon" for control in snapshot["controls"]):
            third_column.extend(["", "No motherboard PWM controls are currently exposed by hwmon on this machine."])

        self._set_status_columns(
            [
                "\n".join(first_column),
                "\n".join(second_column),
                "\n".join(third_column),
            ]
        )

    def _selected_control(self):
        """Return the currently selected control channel, if any."""

        if self.selected_key is None:
            return None
        return self.controls.get(self.selected_key)

    def _selected_curve(self):
        """Return the curve configuration for the selected control."""

        control = self._selected_control()
        if control is None:
            return None
        return self.curves.get_curve(control)

    def _toggle_auto_apply(self):
        """Persist and display the current auto-apply setting."""

        self.curves.auto_apply = self.auto_apply_var.get()
        self.curves.save()
        self._set_auto_status("Auto apply enabled" if self.auto_apply_var.get() else "Auto apply disabled")

    def _toggle_selected_curve(self):
        """Enable or disable the currently selected control curve."""

        curve = self._selected_curve()
        if curve is None:
            return
        curve.enabled = self.selected_enabled.get()
        self.curves.save()
        self._sync_control_list()
        self._draw_curve()

    def _on_control_selected(self, _event):
        """Update editor state when the selected control changes."""

        selection = self.control_list.curselection()
        if not selection:
            return
        control = list(self.controls.values())[selection[0]]
        self.selected_key = control.key
        self.curves.selected_key = control.key
        self.selected_enabled.set(self.curves.get_curve(control).enabled)
        self.curves.save()
        self._update_editor_labels()
        self._draw_curve()

    def _reset_selected_curve(self):
        """Reset the selected curve back to its default control profile."""

        control = self._selected_control()
        if control is None:
            return
        config = self.curves.get_curve(control)
        config.points = default_curve_for(control)
        config.enabled = True
        self.selected_enabled.set(True)
        self.curves.save()
        self._sync_control_list()
        self._update_editor_labels()
        self._draw_curve()

    def _update_editor_labels(self):
        """Refresh the curve editor header and instructional text."""

        control = self._selected_control()
        curve = self._selected_curve()
        if control is None or curve is None:
            self.info_var.set("Select a control to edit its curve.")
            self.details_var.set("")
            return

        cpu_temp = None if self.snapshot is None else self.snapshot.get("cpu_temp_c")
        predicted = "n/a" if cpu_temp is None else f"{duty_from_curve(curve.points, cpu_temp)}%"
        current = "n/a" if control.duty is None else f"{control.duty}%"
        self.info_var.set(
            f"{control.chip} / {control.label}\nCurrent duty: {current}   Predicted at CPU temp: {predicted}"
        )
        self.details_var.set(
            "Drag the orange points directly on the Bezier curve. Raising a point pushes later points up on the Y axis, and lowering a point on the right pulls earlier points down to match. Curves are saved automatically."
        )
        self._set_auto_status("Auto apply enabled" if self.auto_apply_var.get() else "Auto apply disabled")

    def _curve_geometry(self):
        """Return the current canvas dimensions and drawing margins."""

        width = max(self.canvas.winfo_width(), 320)
        height = max(self.canvas.winfo_height(), 220)
        margin = {"left": 46, "right": 18, "top": 18, "bottom": 32}
        return width, height, margin

    def _curve_temp_bounds(self):
        """Return the active temperature range for the selected curve."""

        curve = self._selected_curve()
        if curve is None or not curve.points:
            return TEMP_MIN, TEMP_MAX

        start = curve.points[0][0]
        end = curve.points[-1][0]
        if end - start < 10:
            end = start + 10
        return start, end

    def _point_to_canvas(self, temp, duty):
        """Convert a curve-space point into canvas coordinates."""

        width, height, margin = self._curve_geometry()
        start, end = self._curve_temp_bounds()
        usable_width = width - margin["left"] - margin["right"]
        usable_height = height - margin["top"] - margin["bottom"]
        x = margin["left"] + ((temp - start) / (end - start)) * usable_width
        y = margin["top"] + ((100 - duty) / 100.0) * usable_height
        return x, y

    def _canvas_to_point(self, x, y):
        """Convert canvas coordinates back into curve-space values."""

        width, height, margin = self._curve_geometry()
        start, end = self._curve_temp_bounds()
        usable_width = width - margin["left"] - margin["right"]
        usable_height = height - margin["top"] - margin["bottom"]
        temp = start + ((x - margin["left"]) / usable_width) * (end - start)
        duty = 100 - (((y - margin["top"]) / usable_height) * 100)
        return int(round(temp)), int(round(duty))

    def _draw_curve(self):
        """Redraw the current Bezier curve, grid, and draggable control points."""

        self.canvas.delete("all")
        curve = self._selected_curve()
        if curve is None:
            self.canvas.create_text(20, 20, anchor="nw", fill="#f0f0f0", text="No control selected")
            return

        width, height, margin = self._curve_geometry()
        right = width - margin["right"]
        bottom = height - margin["bottom"]

        start, end = self._curve_temp_bounds()
        temp_step = max(2, int(round((end - start) / 5)))
        for temp in range(start, end + 1, temp_step):
            x, _y = self._point_to_canvas(temp, 0)
            self.canvas.create_line(x, margin["top"], x, bottom, fill="#24272d")
            self.canvas.create_text(x, bottom + 14, text=str(temp), fill="#c5c8cf", font=("TkDefaultFont", 8))

        for duty in range(0, 101, 20):
            _x, y = self._point_to_canvas(TEMP_MIN, duty)
            self.canvas.create_line(margin["left"], y, right, y, fill="#24272d")
            self.canvas.create_text(margin["left"] - 20, y, text=str(duty), fill="#c5c8cf", font=("TkDefaultFont", 8))

        self.canvas.create_rectangle(margin["left"], margin["top"], right, bottom, outline="#626a76")

        curve_points = []
        for temp, duty in curve_samples(curve.points):
            curve_points.extend(self._point_to_canvas(temp, duty))
        if len(curve_points) >= 4:
            self.canvas.create_line(*curve_points, fill="#79c0ff", width=3, smooth=True)

        for index, (temp, duty) in enumerate(curve.points):
            x, y = self._point_to_canvas(temp, duty)
            radius = 2
            self.canvas.create_oval(x - radius, y - radius, x + radius, y + radius, fill="#ffb454", outline="#f0f0f0")
            self.canvas.create_text(x, y - 14, text=f"{temp}C/{duty}%", fill="#f0f0f0", font=("TkDefaultFont", 8))
            if index == self.drag_index:
                self.canvas.create_oval(x - 4, y - 4, x + 4, y + 4, outline="#ffffff")

    def _on_canvas_press(self, event):
        """Start dragging the nearest control point under the cursor."""

        curve = self._selected_curve()
        if curve is None:
            return

        nearest_index = None
        nearest_distance = None
        for index, point in enumerate(curve.points):
            px, py = self._point_to_canvas(point[0], point[1])
            distance = math.hypot(event.x - px, event.y - py)
            if distance <= 12 and (nearest_distance is None or distance < nearest_distance):
                nearest_index = index
                nearest_distance = distance
        self.drag_index = nearest_index
        self._draw_curve()

    def _on_canvas_drag(self, event):
        """Move the active point and propagate Y-axis constraints across the curve."""

        curve = self._selected_curve()
        if curve is None or self.drag_index is None:
            return

        points = list(curve.points)
        temp, duty = self._canvas_to_point(event.x, event.y)
        index = self.drag_index

        if index == 0:
            temp = points[0][0]
        elif index == len(points) - 1:
            temp = points[-1][0]
        else:
            temp = max(points[index - 1][0], temp)

        duty = max(0, min(100, duty))

        points[index] = (temp, duty)
        for right_index in range(index + 1, len(points)):
            right_temp, right_duty = points[right_index]
            prev_duty = points[right_index - 1][1]

            if right_duty < prev_duty:
                right_duty = prev_duty
                points[right_index] = (right_temp, right_duty)
                continue

            break

        for left_index in range(index - 1, -1, -1):
            left_temp, left_duty = points[left_index]
            next_duty = points[left_index + 1][1]

            if left_duty > next_duty:
                left_duty = next_duty
                points[left_index] = (left_temp, left_duty)
                continue

            break

        curve.points = normalize_curve_points(points)
        self.curves.save()
        self._update_editor_labels()
        self._draw_curve()

    def _on_canvas_release(self, _event):
        """End an active drag operation and redraw without a selection ring."""

        self.drag_index = None
        self._draw_curve()

    def _apply_curve_to_control(self, control):
        """Evaluate the control curve at CPU temperature and apply the result."""

        curve = self.curves.get_curve(control)
        if not curve.enabled:
            return None

        cpu_temp = None if self.snapshot is None else self.snapshot.get("cpu_temp_c")
        if cpu_temp is None:
            raise LiquidGUIError("No CPU package temperature is available to evaluate the control curve.")

        duty = duty_from_curve(curve.points, cpu_temp)
        self.backend.apply_control(control, duty)
        return duty

    def _apply_selected_curve(self):
        """Apply the selected curve to its current control target."""

        control = self._selected_control()
        if control is None:
            return

        try:
            duty = self._apply_curve_to_control(control)
        except Exception as exc:
            messagebox.showerror("LiquidGUI", f"Failed to apply {control.label}:\n{exc}")
            return

        if duty is None:
            messagebox.showinfo("LiquidGUI", f"Curve for {control.label} is disabled.")
            return

        self._set_auto_status(f"Applied {duty}% to {control.label}")

    def _set_auto_status(self, text):
        """Set the auto/apply status text while reserving two visible lines."""

        lines = str(text).splitlines()

        if len(lines) == 0:
            lines = ["", ""]

        if len(lines) == 1:
            lines.append("")

        self.auto_status_var.set("\n".join(lines[:2]))

    def _format_applied_curves(self, applied):
        """Return applied curve status split across two readable lines."""

        if not applied:
            return "No enabled curves to apply"

        midpoint = (len(applied) + 1) // 2
        first_line = ", ".join(applied[:midpoint])
        second_line = ", ".join(applied[midpoint:])

        if second_line:
            return f"Applied curves: {first_line}\n{second_line}"

        return f"Applied curves: {first_line}"

    def _apply_all_curves(self):
        """Apply every enabled control curve in the current snapshot."""

        if not self.controls:
            return

        applied = []
        for control in self.controls.values():
            try:
                duty = self._apply_curve_to_control(control)
            except Exception as exc:
                messagebox.showerror("LiquidGUI", f"Failed to apply {control.label}:\n{exc}")
                return
            if duty is not None:
                applied.append(f"{control.label}={duty}%")

        if applied:
            self._set_auto_status(self._format_applied_curves(applied))
        else:
            self._set_auto_status("No enabled curves to apply")

    def _auto_apply_if_needed(self):
        """Throttle automatic curve application to the refresh cadence."""

        now = time.monotonic()
        if now - self.last_auto_apply_at < REFRESH_SECONDS - 0.25:
            return

        try:
            self._apply_all_curves()
        except Exception:
            self.auto_apply_var.set(False)
            self._toggle_auto_apply()
            raise
        self.last_auto_apply_at = now

    def _set_status_columns(self, columns):
        """Replace the contents of the three sensor summary columns."""

        while len(columns) < 3:
            columns.append("")

        for widget, text in zip(self.status_columns, columns[:3]):
            widget.config(state=tk.NORMAL)
            widget.delete("1.0", tk.END)
            widget.insert(tk.END, text)
            widget.config(state=tk.DISABLED)

    def _set_status_text(self, text):
        """Replace the sensor summary with a single left-column message."""

        self._set_status_columns([text, "", ""])

    def close(self):
        """Persist current UI state and close the application window."""

        self.curves.auto_apply = self.auto_apply_var.get()
        self.curves.selected_key = self.selected_key
        self.curves.save()
        self.root.destroy()


def run_gui():
    """Start the Tk GUI using the default sensor backend."""

    root = tk.Tk()
    LiquidGUI(root, SensorBackend())
    root.mainloop()


def run_dev_reloader():
    """Restart the GUI process whenever watched local files change."""

    watch_paths = [Path(__file__).with_name(name) for name in WATCH_FILES]

    def snapshot_mtimes():
        """Capture modification times for the watched source files."""

        current = {}
        for path in watch_paths:
            if path.exists():
                current[str(path)] = path.stat().st_mtime
        return current

    while True:
        child = subprocess.Popen([sys.executable, str(Path(__file__).with_name("liquidgui")), "--run"])
        mtimes = snapshot_mtimes()

        try:
            while child.poll() is None:
                time.sleep(1)
                if snapshot_mtimes() != mtimes:
                    child.send_signal(signal.SIGTERM)
                    child.wait(timeout=5)
                    break
        except KeyboardInterrupt:
            child.send_signal(signal.SIGTERM)
            child.wait(timeout=5)
            return 0


def parse_args(argv=None):
    """Parse command-line flags for normal, dump, and dev-reload modes."""

    parser = argparse.ArgumentParser()
    parser.add_argument("--dev", action="store_true", help="Restart the GUI when local Python files change.")
    parser.add_argument("--dump-detect", action="store_true", help="Print detected sensors and controls as JSON.")
    parser.add_argument("--run", action="store_true", help=argparse.SUPPRESS)
    return parser.parse_args(argv)


def main(argv=None):
    """Application entry point for GUI, dump-detect, or dev-reload execution."""

    args = parse_args(argv)
    backend = SensorBackend()

    if args.dump_detect:
        snapshot = backend.discover()
        print(json.dumps(snapshot, default=lambda item: item.__dict__, indent=2))
        return 0

    if args.dev:
        return run_dev_reloader()

    run_gui()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
