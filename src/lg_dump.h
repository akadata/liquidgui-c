/*
 * lg_dump - render a discovery snapshot as JSON.
 *
 * (C) 2026 AKADATA LIMITED - Andrew Smalley
 * Released under the MIT License.
 */

#ifndef LG_DUMP_H
#define LG_DUMP_H

#include "lg_model.h"

/*
 * Serialise a snapshot as JSON. The caller owns the returned string and frees
 * it. Returns NULL on allocation failure.
 */
char *lg_snapshot_to_json(const lg_snapshot *snap, int indent_depth);

#endif /* LG_DUMP_H */
