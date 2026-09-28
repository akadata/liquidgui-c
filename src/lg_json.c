/*
 * lg_json - minimal self-contained JSON reader/writer.
 *
 * (C) 2026 AKADATA LIMITED - Andrew Smalley
 * Released under the MIT License.
 */

#include "lg_json.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LG_JSON_MAX_DEPTH 64

/* ------------------------------------------------------------------ arena */

typedef struct lg_arena_block {
    struct lg_arena_block *next;
    size_t used;
    size_t cap;
    char data[];
} lg_arena_block;

struct lg_json_doc {
    lg_arena_block *blocks;
    lg_json *root;
};

static void *arena_alloc(lg_json_doc *doc, size_t size)
{
    size_t need = (size + 15u) & ~(size_t)15u;
    lg_arena_block *block = doc->blocks;

    if (block == NULL || block->cap - block->used < need) {
        size_t cap = need > 8192 ? need : 8192;
        block = malloc(sizeof(*block) + cap);
        if (block == NULL) {
            return NULL;
        }
        block->next = doc->blocks;
        block->used = 0;
        block->cap = cap;
        doc->blocks = block;
    }

    void *out = block->data + block->used;
    block->used += need;
    memset(out, 0, size);
    return out;
}

void lg_json_doc_free(lg_json_doc *doc)
{
    if (doc == NULL) {
        return;
    }

    lg_arena_block *block = doc->blocks;
    while (block != NULL) {
        lg_arena_block *next = block->next;
        free(block);
        block = next;
    }
    free(doc);
}

const lg_json *lg_json_root(const lg_json_doc *doc)
{
    return (doc != NULL) ? doc->root : NULL;
}

/* ----------------------------------------------------------------- values */

struct lg_json {
    lg_json_type type;
    union {
        bool b;
        double num;
        char *str;
        struct {
            lg_json **items;
            size_t len;
        } arr;
        struct {
            char **keys;
            lg_json *vals;
            size_t len;
        } obj;
    } u;
};

lg_json_type lg_json_type_of(const lg_json *value)
{
    return (value != NULL) ? value->type : LG_JSON_NULL;
}

const lg_json *lg_json_get(const lg_json *value, const char *key)
{
    if (value == NULL || value->type != LG_JSON_OBJ || key == NULL) {
        return NULL;
    }

    for (size_t i = 0; i < value->u.obj.len; i++) {
        if (strcmp(value->u.obj.keys[i], key) == 0) {
            return &value->u.obj.vals[i];
        }
    }
    return NULL;
}

const lg_json *lg_json_at(const lg_json *value, size_t index)
{
    if (value == NULL || value->type != LG_JSON_ARR || index >= value->u.arr.len) {
        return NULL;
    }
    return value->u.arr.items[index];
}

const char *lg_json_obj_key_at(const lg_json *value, size_t index)
{
    if (value == NULL || value->type != LG_JSON_OBJ || index >= value->u.obj.len) {
        return NULL;
    }
    return value->u.obj.keys[index];
}

const lg_json *lg_json_obj_val_at(const lg_json *value, size_t index)
{
    if (value == NULL || value->type != LG_JSON_OBJ || index >= value->u.obj.len) {
        return NULL;
    }
    return &value->u.obj.vals[index];
}

size_t lg_json_len(const lg_json *value)
{
    if (value == NULL) {
        return 0;
    }
    if (value->type == LG_JSON_ARR) {
        return value->u.arr.len;
    }
    if (value->type == LG_JSON_OBJ) {
        return value->u.obj.len;
    }
    return 0;
}

bool lg_json_as_bool(const lg_json *value, bool fallback)
{
    if (value == NULL) {
        return fallback;
    }
    if (value->type == LG_JSON_BOOL) {
        return value->u.b;
    }
    /* Tolerate the 0/1 encoding some hand-edited configs use. */
    if (value->type == LG_JSON_NUM) {
        return value->u.num != 0.0;
    }
    return fallback;
}

double lg_json_as_num(const lg_json *value, double fallback)
{
    if (value == NULL || value->type != LG_JSON_NUM) {
        return fallback;
    }
    return value->u.num;
}

const char *lg_json_as_str(const lg_json *value, const char *fallback)
{
    if (value == NULL || value->type != LG_JSON_STR || value->u.str == NULL) {
        return fallback;
    }
    return value->u.str;
}

bool lg_json_get_bool(const lg_json *obj, const char *key, bool fallback)
{
    return lg_json_as_bool(lg_json_get(obj, key), fallback);
}

double lg_json_get_num(const lg_json *obj, const char *key, double fallback)
{
    return lg_json_as_num(lg_json_get(obj, key), fallback);
}

const char *lg_json_get_str(const lg_json *obj, const char *key, const char *fallback)
{
    return lg_json_as_str(lg_json_get(obj, key), fallback);
}

/* ----------------------------------------------------------------- parser */

typedef struct {
    const char *p;
    lg_json_doc *doc;
    const char *err;
    int depth;
} lg_parser;

static lg_json *parse_value(lg_parser *ps);

static void skip_ws(lg_parser *ps)
{
    while (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r') {
        ps->p++;
    }
}

static bool expect(lg_parser *ps, char c)
{
    if (*ps->p != c) {
        ps->err = "unexpected character";
        return false;
    }
    ps->p++;
    return true;
}

/* Encode a code point as UTF-8; returns bytes written. */
static size_t utf8_encode(unsigned long cp, char *out)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static char *parse_string_raw(lg_parser *ps)
{
    if (!expect(ps, '"')) {
        return NULL;
    }

    /* Worst case an escape shrinks, so the remaining length is a safe bound. */
    const char *start = ps->p;
    size_t bound = strlen(start) + 1;
    char *out = arena_alloc(ps->doc, bound);
    if (out == NULL) {
        ps->err = "out of memory";
        return NULL;
    }

    size_t n = 0;
    while (*ps->p != '"') {
        if (*ps->p == '\0') {
            ps->err = "unterminated string";
            return NULL;
        }

        if (*ps->p != '\\') {
            out[n++] = *ps->p++;
            continue;
        }

        ps->p++;
        switch (*ps->p) {
        case '"':  out[n++] = '"';  ps->p++; break;
        case '\\': out[n++] = '\\'; ps->p++; break;
        case '/':  out[n++] = '/';  ps->p++; break;
        case 'b':  out[n++] = '\b'; ps->p++; break;
        case 'f':  out[n++] = '\f'; ps->p++; break;
        case 'n':  out[n++] = '\n'; ps->p++; break;
        case 'r':  out[n++] = '\r'; ps->p++; break;
        case 't':  out[n++] = '\t'; ps->p++; break;
        case 'u': {
            ps->p++;
            unsigned long cp = 0;
            for (int i = 0; i < 4; i++) {
                int nib = hex_nibble(ps->p[i]);
                if (nib < 0) {
                    ps->err = "bad \\u escape";
                    return NULL;
                }
                cp = (cp << 4) | (unsigned long)nib;
            }
            ps->p += 4;
            /* Combine a surrogate pair when the low half follows. */
            if (cp >= 0xD800 && cp <= 0xDBFF && ps->p[0] == '\\' && ps->p[1] == 'u') {
                unsigned long lo = 0;
                bool ok = true;
                for (int i = 0; i < 4; i++) {
                    int nib = hex_nibble(ps->p[2 + i]);
                    if (nib < 0) {
                        ok = false;
                        break;
                    }
                    lo = (lo << 4) | (unsigned long)nib;
                }
                if (ok && lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    ps->p += 6;
                }
            }
            n += utf8_encode(cp, out + n);
            break;
        }
        default:
            ps->err = "bad escape";
            return NULL;
        }
    }

    ps->p++; /* closing quote */
    out[n] = '\0';
    return out;
}

static lg_json *parse_number(lg_parser *ps)
{
    char *end = NULL;
    double v = strtod(ps->p, &end);
    if (end == ps->p) {
        ps->err = "bad number";
        return NULL;
    }
    ps->p = end;

    lg_json *node = arena_alloc(ps->doc, sizeof(*node));
    if (node == NULL) {
        ps->err = "out of memory";
        return NULL;
    }
    node->type = LG_JSON_NUM;
    node->u.num = v;
    return node;
}

static lg_json *parse_literal(lg_parser *ps, const char *word, lg_json_type type, bool bval)
{
    size_t len = strlen(word);
    if (strncmp(ps->p, word, len) != 0) {
        ps->err = "bad literal";
        return NULL;
    }
    ps->p += len;

    lg_json *node = arena_alloc(ps->doc, sizeof(*node));
    if (node == NULL) {
        ps->err = "out of memory";
        return NULL;
    }
    node->type = type;
    if (type == LG_JSON_BOOL) {
        node->u.b = bval;
    }
    return node;
}

static lg_json *parse_array(lg_parser *ps)
{
    ps->p++; /* [ */

    lg_json *node = arena_alloc(ps->doc, sizeof(*node));
    if (node == NULL) {
        ps->err = "out of memory";
        return NULL;
    }
    node->type = LG_JSON_ARR;

    size_t cap = 0;
    skip_ws(ps);
    if (*ps->p == ']') {
        ps->p++;
        return node;
    }

    for (;;) {
        lg_json *item = parse_value(ps);
        if (item == NULL) {
            return NULL;
        }
        if (node->u.arr.len == cap) {
            size_t ncap = cap ? cap * 2 : 8;
            /* Copy-on-grow inside the arena; old array is abandoned. */
            lg_json **items = arena_alloc(ps->doc, ncap * sizeof(*items));
            if (items == NULL) {
                ps->err = "out of memory";
                return NULL;
            }
            if (node->u.arr.len > 0) {
                memcpy(items, node->u.arr.items, node->u.arr.len * sizeof(*items));
            }
            node->u.arr.items = items;
            cap = ncap;
        }
        node->u.arr.items[node->u.arr.len++] = item;

        skip_ws(ps);
        if (*ps->p == ',') {
            ps->p++;
            skip_ws(ps);
            continue;
        }
        if (*ps->p == ']') {
            ps->p++;
            return node;
        }
        ps->err = "expected ',' or ']'";
        return NULL;
    }
}

static lg_json *parse_object(lg_parser *ps)
{
    ps->p++; /* { */

    lg_json *node = arena_alloc(ps->doc, sizeof(*node));
    if (node == NULL) {
        ps->err = "out of memory";
        return NULL;
    }
    node->type = LG_JSON_OBJ;

    size_t cap = 0;
    skip_ws(ps);
    if (*ps->p == '}') {
        ps->p++;
        return node;
    }

    for (;;) {
        skip_ws(ps);
        char *key = parse_string_raw(ps);
        if (key == NULL) {
            return NULL;
        }
        skip_ws(ps);
        if (!expect(ps, ':')) {
            return NULL;
        }
        skip_ws(ps);
        lg_json *val = parse_value(ps);
        if (val == NULL) {
            return NULL;
        }

        if (node->u.obj.len == cap) {
            size_t ncap = cap ? cap * 2 : 8;
            char **keys = arena_alloc(ps->doc, ncap * sizeof(*keys));
            lg_json *vals = arena_alloc(ps->doc, ncap * sizeof(*vals));
            if (keys == NULL || vals == NULL) {
                ps->err = "out of memory";
                return NULL;
            }
            if (node->u.obj.len > 0) {
                memcpy(keys, node->u.obj.keys, node->u.obj.len * sizeof(*keys));
                memcpy(vals, node->u.obj.vals, node->u.obj.len * sizeof(*vals));
            }
            node->u.obj.keys = keys;
            node->u.obj.vals = vals;
            cap = ncap;
        }
        node->u.obj.keys[node->u.obj.len] = key;
        /* vals is a flat array of values, so copy the node; children are
         * arena-owned and stay valid regardless of where the struct lands. */
        node->u.obj.vals[node->u.obj.len] = *val;
        node->u.obj.len++;

        skip_ws(ps);
        if (*ps->p == ',') {
            ps->p++;
            continue;
        }
        if (*ps->p == '}') {
            ps->p++;
            return node;
        }
        ps->err = "expected ',' or '}'";
        return NULL;
    }
}

static lg_json *parse_value(lg_parser *ps)
{
    if (ps->depth >= LG_JSON_MAX_DEPTH) {
        ps->err = "nesting too deep";
        return NULL;
    }

    skip_ws(ps);

    lg_json *node = NULL;
    ps->depth++;
    switch (*ps->p) {
    case '{':
        node = parse_object(ps);
        break;
    case '[':
        node = parse_array(ps);
        break;
    case '"': {
        char *s = parse_string_raw(ps);
        if (s != NULL) {
            node = arena_alloc(ps->doc, sizeof(*node));
            if (node == NULL) {
                ps->err = "out of memory";
            } else {
                node->type = LG_JSON_STR;
                node->u.str = s;
            }
        }
        break;
    }
    case 't':
        node = parse_literal(ps, "true", LG_JSON_BOOL, true);
        break;
    case 'f':
        node = parse_literal(ps, "false", LG_JSON_BOOL, false);
        break;
    case 'n':
        node = parse_literal(ps, "null", LG_JSON_NULL, false);
        break;
    default:
        node = parse_number(ps);
        break;
    }
    ps->depth--;

    return node;
}

lg_json_doc *lg_json_parse(const char *text, const char **err)
{
    if (err != NULL) {
        *err = NULL;
    }
    if (text == NULL) {
        if (err != NULL) {
            *err = "no input";
        }
        return NULL;
    }

    lg_json_doc *doc = calloc(1, sizeof(*doc));
    if (doc == NULL) {
        if (err != NULL) {
            *err = "out of memory";
        }
        return NULL;
    }

    lg_parser ps = {.p = text, .doc = doc, .err = NULL, .depth = 0};
    doc->root = parse_value(&ps);

    if (doc->root == NULL) {
        if (err != NULL) {
            *err = ps.err != NULL ? ps.err : "parse failed";
        }
        lg_json_doc_free(doc);
        return NULL;
    }

    skip_ws(&ps);
    if (*ps.p != '\0') {
        if (err != NULL) {
            *err = "trailing data after value";
        }
        lg_json_doc_free(doc);
        return NULL;
    }

    return doc;
}

/* ----------------------------------------------------------------- writer */

void lg_json_writer_init(lg_json_writer *w)
{
    w->data = NULL;
    w->len = 0;
    w->cap = 0;
    w->oom = false;
}

static bool writer_reserve(lg_json_writer *w, size_t extra)
{
    if (w->oom) {
        return false;
    }
    if (w->cap - w->len > extra) {
        return true;
    }

    size_t need = w->len + extra + 1;
    size_t cap = w->cap ? w->cap : 256;
    while (cap < need) {
        cap *= 2;
    }

    char *buf = realloc(w->data, cap);
    if (buf == NULL) {
        w->oom = true;
        return false;
    }
    w->data = buf;
    w->cap = cap;
    return true;
}

void lg_json_raw(lg_json_writer *w, const char *text)
{
    if (text == NULL) {
        return;
    }
    size_t n = strlen(text);
    if (!writer_reserve(w, n)) {
        return;
    }
    memcpy(w->data + w->len, text, n);
    w->len += n;
    w->data[w->len] = '\0';
}

void lg_json_printf(lg_json_writer *w, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);

    if (n < 0 || !writer_reserve(w, (size_t)n)) {
        va_end(ap2);
        return;
    }
    vsnprintf(w->data + w->len, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    w->len += (size_t)n;
}

void lg_json_begin_object(lg_json_writer *w) { lg_json_raw(w, "{"); }
void lg_json_end_object(lg_json_writer *w)   { lg_json_raw(w, "}"); }
void lg_json_begin_array(lg_json_writer *w)  { lg_json_raw(w, "["); }
void lg_json_end_array(lg_json_writer *w)    { lg_json_raw(w, "]"); }
void lg_json_key(lg_json_writer *w, const char *key) { lg_json_write_escaped(w, key); lg_json_raw(w, ":"); }

void lg_json_write_escaped(lg_json_writer *w, const char *value)
{
    lg_json_raw(w, "\"");
    if (value == NULL) {
        lg_json_raw(w, "\"");
        return;
    }

    for (const unsigned char *p = (const unsigned char *)value; *p != '\0'; p++) {
        switch (*p) {
        case '"':  lg_json_raw(w, "\\\""); break;
        case '\\': lg_json_raw(w, "\\\\"); break;
        case '\b': lg_json_raw(w, "\\b");  break;
        case '\f': lg_json_raw(w, "\\f");  break;
        case '\n': lg_json_raw(w, "\\n");  break;
        case '\r': lg_json_raw(w, "\\r");  break;
        case '\t': lg_json_raw(w, "\\t");  break;
        default:
            if (*p < 0x20) {
                lg_json_printf(w, "\\u%04x", *p);
            } else {
                if (!writer_reserve(w, 1)) {
                    return;
                }
                w->data[w->len++] = (char)*p;
                w->data[w->len] = '\0';
            }
            break;
        }
    }
    lg_json_raw(w, "\"");
}

void lg_json_write_str(lg_json_writer *w, const char *value) { lg_json_write_escaped(w, value); }

void lg_json_write_num(lg_json_writer *w, double value)
{
    /*
     * JSON has no way to spell NaN or infinity, and emitting bare "nan" or
     * "inf" produces a document no parser will accept. A sensor that has no
     * reading is a null, which is also what a consumer wants to see: it is
     * distinct from a reading of zero.
     */
    if (isnan(value) || isinf(value)) {
        lg_json_write_null(w);
        return;
    }

    if (value == (double)(long long)value && value > -1e15 && value < 1e15) {
        lg_json_printf(w, "%lld", (long long)value);
    } else {
        lg_json_printf(w, "%.17g", value);
    }
}

void lg_json_write_bool(lg_json_writer *w, bool value) { lg_json_raw(w, value ? "true" : "false"); }
void lg_json_write_null(lg_json_writer *w) { lg_json_raw(w, "null"); }

char *lg_json_writer_take(lg_json_writer *w)
{
    if (w->oom) {
        lg_json_writer_free(w);
        return NULL;
    }
    if (w->data == NULL) {
        w->data = calloc(1, 1);
    }
    char *out = w->data;
    w->data = NULL;
    w->len = 0;
    w->cap = 0;
    return out;
}

void lg_json_writer_free(lg_json_writer *w)
{
    free(w->data);
    lg_json_writer_init(w);
}
