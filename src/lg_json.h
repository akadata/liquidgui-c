/*
 * lg_json - minimal self-contained JSON reader/writer.
 *
 * liquidgui's configuration schema is small and fixed, so this deliberately
 * avoids an external dependency (jansson/json-c are available but would add a
 * link-time requirement to a tool that otherwise needs only libc and GTK3).
 *
 * Reading  : arena-backed DOM, torn down in one call via lg_json_doc_free.
 * Writing  : append-only emitter into a growable buffer.
 *
 * (C) 2026 AKADATA LIMITED - Andrew Smalley
 * Released under the MIT License.
 */

#ifndef LG_JSON_H
#define LG_JSON_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    LG_JSON_NULL = 0,
    LG_JSON_BOOL,
    LG_JSON_NUM,
    LG_JSON_STR,
    LG_JSON_ARR,
    LG_JSON_OBJ
} lg_json_type;

typedef struct lg_json lg_json;

/* ---------------------------------------------------------------- reading */

typedef struct lg_json_doc lg_json_doc;

/*
 * Parse NUL-terminated JSON text. Returns NULL on malformed input or when the
 * nesting exceeds LG_JSON_MAX_DEPTH. On failure *err (when non-NULL) receives a
 * short static description of the failure.
 */
lg_json_doc *lg_json_parse(const char *text, const char **err);

/* Free the document and all memory owned by it, including the arena. */
void lg_json_doc_free(lg_json_doc *doc);

/* Root value of the document. Never NULL for a successfully parsed document. */
const lg_json *lg_json_root(const lg_json_doc *doc);

lg_json_type lg_json_type_of(const lg_json *value);

/* Object member lookup. Returns NULL when absent or when value is not an object. */
const lg_json *lg_json_get(const lg_json *value, const char *key);

/* Array element lookup. Returns NULL when out of range or not an array. */
const lg_json *lg_json_at(const lg_json *value, size_t index);

/*
 * Object key by position. Useful for iterating an object, where lg_json_get
 * only supports lookup by name. Returns NULL when out of range or not object.
 */
const char *lg_json_obj_key_at(const lg_json *value, size_t index);

/* Companion to lg_json_obj_key_at: the value at the same position. */
const lg_json *lg_json_obj_val_at(const lg_json *value, size_t index);

size_t lg_json_len(const lg_json *value);

/*
 * Typed accessors. Each returns the fallback when the type does not match, so
 * callers can read untrusted config without pre-checking every node.
 */
bool lg_json_as_bool(const lg_json *value, bool fallback);
double lg_json_as_num(const lg_json *value, double fallback);
const char *lg_json_as_str(const lg_json *value, const char *fallback);

/* Convenience: object member read with a type-checked fallback. */
bool lg_json_get_bool(const lg_json *obj, const char *key, bool fallback);
double lg_json_get_num(const lg_json *obj, const char *key, double fallback);
const char *lg_json_get_str(const lg_json *obj, const char *key, const char *fallback);

/* ---------------------------------------------------------------- writing */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    bool oom;
} lg_json_writer;

void lg_json_writer_init(lg_json_writer *w);

/* Append raw text. */
void lg_json_raw(lg_json_writer *w, const char *text);
void lg_json_printf(lg_json_writer *w, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

void lg_json_begin_object(lg_json_writer *w);
void lg_json_end_object(lg_json_writer *w);
void lg_json_begin_array(lg_json_writer *w);
void lg_json_end_array(lg_json_writer *w);

/* "key": with the separator left to the caller, matching json-c style. */
void lg_json_key(lg_json_writer *w, const char *key);

void lg_json_write_str(lg_json_writer *w, const char *value);
void lg_json_write_num(lg_json_writer *w, double value);
void lg_json_write_bool(lg_json_writer *w, bool value);
void lg_json_write_null(lg_json_writer *w);

/* Emits a JSON string literal with correct escaping, including control chars. */
void lg_json_write_escaped(lg_json_writer *w, const char *value);

/*
 * Detach the buffer; the caller owns the returned pointer and frees it with
 * free(). Returns NULL if the writer ran out of memory.
 */
char *lg_json_writer_take(lg_json_writer *w);

/* Free the writer's buffer without detaching it. */
void lg_json_writer_free(lg_json_writer *w);

#endif /* LG_JSON_H */
