# NCT6687 / NCT6687D hwmon driver

Vendored from [`akadata/nct6687d`](https://github.com/akadata/nct6687d), pinned
at `5695970`. Upstream development happens there; this copy
exists so that `liquidgui` is complete on a machine with one of these chips
without a second checkout.

Installed by `sudo make install-driver` from the parent Makefile, through DKMS,
so it is rebuilt when the kernel changes. It is also buildable and testable on
its own:

```bash
make -C driver build      # build for the running kernel
make -C driver check      # build, static checks, and a PWM write/readback
```

## Why it is a dependency and not an optional extra

The NCT6687 and NCT6687D expose the CPU, VRM, PCH and chipset temperatures, the
voltage rails and the fan headers on these boards. There is no in-tree driver
for the chip, so without this module none of those channels exist as far as
userspace is concerned, and `liquidgui` has almost nothing to display.

Installed through DKMS rather than copied into
`/lib/modules/<kver>/kernel/drivers/hwmon/` because a copied module is built
against one kernel and stops matching `vermagic` after the next upgrade. On a
rolling distro that means the sensors vanish on a routine system update with
nothing reporting why.

## Licensing

GPL-2.0-or-later, as kernel modules must be. It is compiled separately and never
linked into the MIT-licensed `liquidgui` binary, so the two remain separate
works and the parent project's licence is unaffected.

## What this pinned copy contains

Relative to the pre-existing release, these are the fixes that matter to
`liquidgui`:

- **Temperatures are read as signed.** `kbuild` compiles with
  `-funsigned-char`, so a cast to plain `char` is a no-op and the register
  byte `0xC1` was read as 193 and reported as 193.0 °C. On this board the
  unpopulated "PCIe x1" channel showed +193 °C while the machine sat at 30 °C.
  Both the probe-time init and the update path now cast to `s8`. The parent
  application keeps a range check as a backstop, and has dropped the
  `*_min` sentinel rule that this fix made invalid.
- **A discarded PWM write is reported as a failure.** `store_pwm` took an
  EC lock/unlock handshake, and on timeout skipped the write but still returned
  success, so userspace believed a duty had been applied when it had not. It now
  returns `-ETIMEDOUT`, or `-EIO` when the EC reports it rejected the
  configuration.
- **Restore on exit writes full speed by default.** Handing a channel back with
  `pwm_enable=2` is documented on `nct6687` as "the controller runs its own
  curve", but was measured on an `nzxt_kraken3` to zero both outputs and stop
  the radiator fan. A stopped fan is worse than an over-speed one, so full speed
  is the default and handback is refused per control where it is not known safe.
  `liquidgui` follows the same rule.
- **The temperature cast and the PWM error path are asserted by the driver's own
  tests,** because both were invisible to a runtime check on the machine that
  has the hardware: the module that misreported 193 °C loaded cleanly, and the
  code that discarded a write reported success on every run.

See `TESTING_RESULTS.md` upstream for the full record, including what has not
been verified.
