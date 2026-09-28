/*
 * lg_liquidctl - optional HID-backed AIO discovery via liquidctl.
 *
 * This is a fallback path only. On hardware where the AIO is also exposed
 * through hwmon (an in-kernel nzxt_kraken3, for example) the hwmon controls
 * are preferred, because driving one cooler through two paths at once invites
 * the two to fight over the same register.
 *
 * Parsing uses `liquidctl --json status` rather than the human-readable tree.
 * The tree form interleaves box-drawing characters with values and is brittle
 * to parse; the JSON form is stable and machine-readable.
 *
 * (C) 2026 AKADATA LIMITED - Andrew Smalley
 * Released under the MIT License.
 */

#ifndef LG_LIQUIDCTL_H
#define LG_LIQUIDCTL_H

#include "lg_model.h"

/*
 * Probe liquidctl and append any pump/fan channels it reports to snap.
 * Returns true when a device was found. Never fails the caller: a missing
 * binary or a permission error simply yields false.
 */
bool lg_liquidctl_discover(lg_snapshot *snap, const char *liquidctl_bin);

/*
 * Set a liquidctl channel to a duty percentage. Returns true on success and
 * fills err (when non-NULL) with a short message otherwise.
 */
bool lg_liquidctl_set(const char *liquidctl_bin, const char *channel, int duty_percent,
                      char *err, size_t err_cap);

#endif /* LG_LIQUIDCTL_H */
