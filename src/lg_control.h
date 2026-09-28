/*
 * lg_control - applying duty to a control, with the write protocol each
 * driver actually requires.
 *
 * Write protocols, established by probing this machine rather than by
 * assumption:
 *
 *   nct6687 (motherboard headers)
 *     store_pwm sets the manual bit itself, so a single write to pwmN is
 *     enough. pwmN_enable is already 1 in normal use and re-writing it is
 *     idempotent.
 *
 *   nzxt_kraken3 (Kraken AIO)
 *     A write to pwmN with pwm_enable=0 returns success and is discarded.
 *     pwmN_enable=1 is therefore mandatory. Entering manual mode resets the
 *     PWM register to a driver default, which on this hardware meant pwm2
 *     went to 0 and the radiator fan stalled for about a second. The two
 *     writes are consequently issued back to back in one helper invocation
 *     so the stall window is microseconds rather than tens of milliseconds.
 *
 * The legacy Python issued the two writes as separate `sudo tee` forks, one
 * per file, which widened exactly that window on every apply.
 *
 * (C) 2026 AKADATA LIMITED - Andrew Smalley
 * Released under the MIT License.
 */

#ifndef LG_CONTROL_H
#define LG_CONTROL_H

#include "lg_model.h"

/*
 * Where the privileged helper lives. Must match LIBEXECDIR in the Makefile, and
 * the alternative below is kept for installs made before the prefix moved to
 * /usr, so an existing setuid helper keeps working.
 */
#define LG_HELPER_NAME "lg-helper"
#define LG_HELPER_PATH "/usr/libexec/liquidgui/lg-helper"

typedef enum {
    LG_PRIV_HELPER = 0, /* setuid helper, the normal path */
    LG_PRIV_PKEXEC,      /* polkit agent */
    LG_PRIV_SUDO,        /* sudo -n, for a NOPASSWD admin */
    LG_PRIV_DIRECT,      /* we are already root */
    LG_PRIV_NONE,        /* no way to write */
} lg_priv_mode;

/*
 * Outcome of a write.
 *
 * The distinction matters because the kernel driver cannot report a failed
 * write. nct6687's store_pwm takes an exclusive config lock on the EC's fan
 * registers, gives up after a one second timeout if the EC is mid-update, and
 * then returns the byte count as though the write had succeeded. Userspace
 * therefore has to read the node back to learn whether anything happened.
 *
 * On this machine hwmon12/pwm8 sat at 128 indefinitely while every write to it
 * was silently discarded, and the previous implementation reported success the
 * whole time because it never looked.
 */
typedef enum {
    LG_APPLY_OK = 0,
    LG_APPLY_REJECTED,      /* the write itself failed */
    LG_APPLY_UNRESPONSIVE,  /* the write returned success but the value did not move */
} lg_apply_result;

typedef struct {
    lg_priv_mode mode;
    char helper_path[LG_PATH_MAX];
    bool available;
    char detail[192];
} lg_priv;

void lg_priv_init(lg_priv *priv);

/*
 * Choose the best available write mechanism. Safe to call when already root,
 * in which case it selects LG_PRIV_DIRECT and needs no helper.
 */
void lg_priv_detect(lg_priv *priv);

/* Human-readable description of the active mechanism, for the status bar. */
const char *lg_priv_describe(const lg_priv *priv);

/*
 * Apply a duty percentage to a control, honouring min_duty and the driver's
 * enable requirements, then confirm the value actually landed.
 *
 * Returns LG_APPLY_OK when the readback matches within one raw step,
 * LG_APPLY_REJECTED when the write itself failed, and LG_APPLY_UNRESPONSIVE
 * when the driver accepted the write but the value never changed. Retries a
 * small number of times, because the underlying cause is the EC being briefly
 * locked out of its own fan registers.
 */
lg_apply_result lg_control_apply_verified(const lg_priv *priv, const lg_control *ctl, int duty,
                                          char *err, size_t err_cap);

/*
 * As above, but `attempts` bounds the number of tries. Callers pass 1 for a
 * channel already known to ignore writes, so a broken header cannot dominate
 * the cycle: with ten channels retrying four times each and 250 ms between
 * tries, one unresponsive board would otherwise hold a shutdown for seconds.
 */
lg_apply_result lg_control_apply_verified_n(const lg_priv *priv, const lg_control *ctl, int duty,
                                            int attempts, char *err, size_t err_cap);

/* As above, without readback verification. */
bool lg_control_apply(const lg_priv *priv, const lg_control *ctl, int duty, char *err, size_t err_cap);

/* Hand a control back to the controller's own automatic curve. */
bool lg_control_handback(const lg_priv *priv, const lg_control *ctl, char *err, size_t err_cap);

/* Write 100% to the control, used by the failsafe and restore-on-exit. */
bool lg_control_full_speed(const lg_priv *priv, const lg_control *ctl, char *err, size_t err_cap);

/* Refresh a control's cached duty and RPM from sysfs. */
void lg_control_refresh(lg_control *ctl);

/* Convert a duty percentage to the raw 0..255 value a driver expects. */
int lg_control_duty_to_raw(const lg_control *ctl, int duty_percent);

#endif /* LG_CONTROL_H */
