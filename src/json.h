#ifndef GW_JSON_H
#define GW_JSON_H

#include <stddef.h>

enum JsonType { JSON_OBJECT = 1, JSON_ARRAY, JSON_STRING, JSON_PRIMITIVE };
typedef struct {
    size_t start, end;
    int parent, type;
} Token;

/* Strict UTF-8 JSON, maximum nesting depth 32. No dynamic allocation. */
int json_parse(const char *s, size_t n, Token *tokens, int capacity);
int json_field(const char *s, const Token *tokens, int count, int object,
               const char *key);
int json_equal(const char *s, const Token *token, const char *value);
/* Decode a JSON string. Returns byte length, or -1 for insufficient capacity. */
int json_string(const char *s, const Token *token, char *out, size_t capacity);
/* Escape a C string for embedding in JSON, including enclosing quotes. */
int json_quote(const char *s, char *out, size_t capacity);

#endif
