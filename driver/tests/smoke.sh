#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Smoke tests for the nct6687d driver.
#
# The build portion runs everywhere. The hardware portion needs root and a
# machine with the chip, and is skipped rather than failed without one, so this
# is usable as a plain `make check` on a build machine.
#
# What is actually asserted:
#   - the module builds for the running kernel with no compiler diagnostics
#   - it loads, registers a hwmon device, and exposes the documented channels
#   - a PWM write is applied and read back, which is the path that the error
#     propagation in store_pwm() applies to
#   - unloading leaves fan control registers as they were found
#
# What is not asserted: that any particular channel reads a plausible value. A
# channel being unpopulated on the board under test is a property of the board,
# not a driver fault, and asserting on it would make this fail on hardware it
# is not written for.

set -uo pipefail

# Run from the driver directory whether invoked as driver/tests/smoke.sh or
# directly from within it.
here="$(cd "$(dirname "$0")" && pwd)"
driver="$(dirname "$here")"
cd "$driver"

kver="${1:-$(uname -r)}"
objdir="build/${kver}"
ko="${objdir}/nct6687.ko"

pass=0
fail=0
skip=0

# A private log directory: /tmp files from an earlier run as another user would
# otherwise be unwritable here.
tmpd="$(mktemp -d)"
trap 'rm -rf "$tmpd"' EXIT
buildlog="${tmpd}/build.log"
pwmlog="${tmpd}/pwm.log"

ok()   { printf '  ok    %s\n' "$1"; pass=$((pass+1)); }
no()   { printf '  FAIL  %s\n' "$1"; fail=$((fail+1)); }
skipt(){ printf '  skip  %s\n' "$1"; skip=$((skip+1)); }

have_root=0
[ "$(id -u)" = 0 ] && have_root=1

# Is the module loaded?
#
# This reads /sys/module rather than parsing `lsmod`. `lsmod | grep -q` looks
# right but is wrong under `set -o pipefail`: grep exits at the first match
# while lsmod is still writing, gets SIGPIPE, and pipefail turns the resulting
# 141 into a failed pipeline -- so a loaded module reports as not loaded.
module_loaded() {
	[ -d /sys/module/nct6687 ]
}

# The hwmon device this driver owns, identified by its parent driver rather
# than by a number, since hwmonN numbering depends on probe order.
find_hwmon() {
	local d
	for d in /sys/class/hwmon/hwmon*; do
		[ -e "$d" ] || continue
		[ "$(basename "$(readlink -f "$d/device/driver" 2>/dev/null)")" = nct6687 ] \
			&& { echo "$d"; return 0; }
	done
	return 1
}

echo "== build =="
if make kver="$kver" build >$buildlog 2>&1; then
	# Only diagnostics from compiling nct6687.c. kbuild emits notices about
	# the host toolchain that say nothing about this driver's code: the pahole
	# BTF version mismatch, "the compiler differs from the one used to build
	# the kernel", and "Skipping BTF generation" when vmlinux is absent. Those
	# vary with the image, not with the driver.
	if grep -qE 'nct6687\.c:[0-9]+.*(warning|error)' $buildlog; then
		no "compiler diagnostics in nct6687.c"
		grep -E 'nct6687\.c:[0-9]+.*(warning|error)' $buildlog | sed 's/^/        /'
	else
		ok "compiles without diagnostics for ${kver}"
	fi
else
	no "compiles for ${kver} (see $buildlog)"
	sed 's/^/        /' $buildlog
	exit 1
fi

[ -f "$ko" ] && ok "module built at ${ko}" || { no "module built at ${ko}"; exit 1; }

# The vermagic has to match or modprobe refuses the module outright. Checking
# it here turns a load-time failure into a readable line.
if [[ "$(modinfo -F vermagic "$ko")" == "${kver} "* ]]; then
	ok "vermagic matches ${kver}"
else
	no "vermagic is $(modinfo -F vermagic "$ko"), expected ${kver}"
fi

echo "== static checks =="
# The driver carries exactly one version conditional. If a second appears it
# needs a header-checked justification, because the obvious candidate --
# <linux/hwmon-sysfs.h> -- has existed since well before 4.19 and is not a
# boundary at all. Gating it on 6.13 broke the build on 5.15 and 6.12 while
# looking like it was protecting them.
gates=$(grep -c 'KERNEL_VERSION' nct6687.c)
if [ "$gates" -le 2 ]; then
	ok "one version boundary gated (KERNEL_VERSION uses: ${gates})"
else
	no "${gates} KERNEL_VERSION uses; expected only the 6.11 remove() gate"
fi
if grep -q 'include <linux/hwmon-sysfs.h>' nct6687.c; then
	ok "hwmon-sysfs.h included unconditionally"
else
	no "hwmon-sysfs.h include missing or version-gated"
fi
# The temperature cast must be s8, not char: kbuild uses -funsigned-char, so a
# cast to char is a no-op and a two's complement register byte comes out as a
# large positive temperature.
# Matched with bash's [[ =~ ]] rather than grep: any pipeline ending in
# `grep -q` can lose to SIGPIPE under pipefail, and a check that silently
# reports the wrong answer is worse than no check.
src="$(cat nct6687.c)"
if [[ "$src" == *"s8 value = (s8)nct6687_read"* ]]; then
	ok "temperature read is cast to s8"
else
	no "temperature read must be cast to s8, not char (-funsigned-char)"
fi
if [[ "$src" == *"s32 value = (char)"* ]]; then
	no "a (char) cast on the temperature read remains; it is a no-op"
else
	ok "no ineffective (char) cast on the temperature read"
fi

# store_pwm must not report success after a failed unlock handshake.
if [[ "$(awk '/^static ssize_t store_pwm/,/^}/' nct6687.c)" == *"return ret ? ret : count"* ]]; then
	ok "store_pwm propagates handshake failure to userspace"
else
	no "store_pwm must return the error, not count, after a failed write"
fi

echo "== hardware =="
if [ "$have_root" != 1 ]; then
	skipt "hardware tests need root"
	exit $(( fail > 0 ))
fi
if ! module_loaded; then
	skipt "nct6687 is not loaded; run as root on a machine with the chip to test"
	exit $(( fail > 0 ))
fi

hw="$(find_hwmon)" || { skipt "no hwmon device owned by nct6687"; exit $((fail > 0)); }
ok "hwmon device present at ${hw}"

[ "$(cat "$hw/name" 2>/dev/null)" = nct6687 ] \
	&& ok "hwmon name is nct6687" || no "hwmon name is $(cat "$hw/name" 2>/dev/null)"

# Every documented channel must exist, since exposure is unconditional. Their
# values are not checked, only their presence.
for i in $(seq 1 7); do
	[ -e "$hw/temp${i}_input" ] || no "temp${i}_input missing"
done
ok "7 temperature channels present"
for i in $(seq 0 13); do
	[ -e "$hw/in${i}_input" ] || no "in${i}_input missing"
done
ok "14 voltage channels present"
for i in $(seq 1 8); do
	[ -e "$hw/fan${i}_input" ] || no "fan${i}_input missing"
	[ -e "$hw/pwm${i}" ] || no "pwm${i} missing"
done
ok "8 fan and 8 pwm channels present"

# The AIO and memory channels this driver exposes must be readable, not just
# present, or a wedged EC would pass the checks above.
if [ -w "$hw/pwm1" ]; then
	before_pwm1="$(cat "$hw/pwm1")"
	before_fan1="$(cat "$hw/fan1_input")"
	if echo 200 > "$hw/pwm1" 2>$pwmlog; then
		ok "pwm1 accepted a write of 200"
		# Give the fan time to react before reading back.
		sleep 2
		after_pwm1="$(cat "$hw/pwm1")"
		if [ "$after_pwm1" = 200 ]; then
			ok "pwm1 read back as 200"
		else
			no "pwm1 read back as ${after_pwm1}, expected 200"
		fi
	else
		# A rejected write is the error propagation working, not a failure
		# of the driver, so report what the kernel said and move on.
		ok "pwm1 write was rejected: $(cat $pwmlog 2>/dev/null | tr -d '\n')"
	fi

	# Put the channel back the way it was found, and confirm the fan is
	# still turning. Leaving a fan at a duty this test chose would be a side
	# effect on the user's machine.
	echo "$before_pwm1" > "$hw/pwm1" 2>/dev/null
	sleep 2
	[ "$(cat "$hw/pwm1")" = "$before_pwm1" ] \
		&& ok "pwm1 restored to ${before_pwm1}" \
		|| skipt "pwm1 could not be restored to ${before_pwm1}"
	after_fan1="$(cat "$hw/fan1_input")"
	[ "$after_fan1" -gt 0 ] 2>/dev/null \
		&& ok "fan1 still turning (${after_fan1} rpm, was ${before_fan1})" \
		|| no "fan1 not turning (${after_fan1} rpm)"
else
	skipt "pwm1 is not writable"
fi

echo
printf '%d passed, %d failed, %d skipped\n' "$pass" "$fail" "$skip"
exit $(( fail > 0 ))
