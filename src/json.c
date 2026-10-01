#include "json.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    const char *s;
    size_t n, pos;
    Token *tokens;
    int count, capacity;
} Parser;

static bool whitespace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static void skip_space(Parser *p)
{
    while (p->pos < p->n && whitespace(p->s[p->pos]))
        p->pos++;
}

static int hex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool hex4(const char *s, size_t n, size_t *pos, uint32_t *cp)
{
    if (n - *pos < 4) return false;
    *cp = 0;
    for (int i = 0; i < 4; i++) {
        int h = hex(s[(*pos)++]);
        if (h < 0) return false;
        *cp = (*cp << 4) | (uint32_t)h;
    }
    return true;
}

/* Consume one raw UTF-8 code point. */
static bool utf8(const char *s, size_t n, size_t *pos, uint32_t *cp)
{
    if (*pos >= n) return false;
    unsigned char c = (unsigned char)s[(*pos)++];
    if (c < 0x80) { *cp = c; return true; }
    int continuation;
    uint32_t minimum;
    if (c >= 0xc2 && c <= 0xdf) {
        continuation = 1; minimum = 0x80; *cp = c & 0x1f;
    } else if (c >= 0xe0 && c <= 0xef) {
        continuation = 2; minimum = 0x800; *cp = c & 0x0f;
    } else if (c >= 0xf0 && c <= 0xf4) {
        continuation = 3; minimum = 0x10000; *cp = c & 0x07;
    } else return false;
    for (int i = 0; i < continuation; i++) {
        if (*pos >= n) return false;
        c = (unsigned char)s[(*pos)++];
        if ((c & 0xc0) != 0x80) return false;
        *cp = (*cp << 6) | (c & 0x3f);
    }
    return *cp >= minimum && *cp <= 0x10ffff &&
           !(*cp >= 0xd800 && *cp <= 0xdfff);
}

/* Consume one decoded character inside a JSON string. */
static bool character(const char *s, size_t n, size_t *pos, uint32_t *cp)
{
    if (*pos >= n) return false;
    if (s[*pos] != '\\') return utf8(s, n, pos, cp);
    (*pos)++;
    if (*pos == n) return false;
    char escape = s[(*pos)++];
    switch (escape) {
    case '"': *cp = '"'; return true;
    case '\\': *cp = '\\'; return true;
    case '/': *cp = '/'; return true;
    case 'b': *cp = '\b'; return true;
    case 'f': *cp = '\f'; return true;
    case 'n': *cp = '\n'; return true;
    case 'r': *cp = '\r'; return true;
    case 't': *cp = '\t'; return true;
    case 'u':
        if (!hex4(s, n, pos, cp)) return false;
        if (*cp >= 0xdc00 && *cp <= 0xdfff) return false;
        if (*cp >= 0xd800 && *cp <= 0xdbff) {
            uint32_t low;
            if (n - *pos < 6 || s[*pos] != '\\' || s[*pos + 1] != 'u')
                return false;
            *pos += 2;
            if (!hex4(s, n, pos, &low) || low < 0xdc00 || low > 0xdfff)
                return false;
            *cp = 0x10000 + ((*cp - 0xd800) << 10) + low - 0xdc00;
        }
        return true;
    default: return false;
    }
}

static int add_token(Parser *p, int parent, int type)
{
    if (p->count == p->capacity) return -1;
    int index = p->count++;
    p->tokens[index] = (Token){ p->pos, 0, parent, type };
    return index;
}

static int parse_string(Parser *p, int parent)
{
    int index = add_token(p, parent, JSON_STRING);
    if (index < 0) return -1;
    p->tokens[index].start = ++p->pos;
    while (p->pos < p->n) {
        unsigned char c = (unsigned char)p->s[p->pos];
        if (c == '"') {
            p->tokens[index].end = p->pos++;
            return index;
        }
        if (c < 0x20) return -1;
        uint32_t cp;
        if (!character(p->s, p->n, &p->pos, &cp)) return -1;
    }
    return -1;
}

static bool same_key(const char *s, const Token *a, const Token *b)
{
    size_t x = a->start, y = b->start;
    while (x < a->end && y < b->end) {
        uint32_t u, v;
        if (!character(s, a->end, &x, &u) ||
            !character(s, b->end, &y, &v) || u != v) return false;
    }
    return x == a->end && y == b->end;
}

static bool unique_key(Parser *p, int object, int key)
{
    int i = object + 1;
    while (i < key) {
        if (same_key(p->s, &p->tokens[i], &p->tokens[key])) return false;
        i += 2; /* Skip the key and the first token of its value. */
        while (i < key && p->tokens[i].parent != object) i++;
    }
    return true;
}

static bool digit(char c) { return c >= '0' && c <= '9'; }

static bool parse_number(Parser *p)
{
    if (p->s[p->pos] == '-') p->pos++;
    if (p->pos == p->n) return false;
    if (p->s[p->pos] == '0') p->pos++;
    else {
        if (p->s[p->pos] < '1' || p->s[p->pos] > '9') return false;
        while (p->pos < p->n && digit(p->s[p->pos])) p->pos++;
    }
    if (p->pos < p->n && p->s[p->pos] == '.') {
        size_t start = ++p->pos;
        while (p->pos < p->n && digit(p->s[p->pos])) p->pos++;
        if (start == p->pos) return false;
    }
    if (p->pos < p->n && (p->s[p->pos] == 'e' || p->s[p->pos] == 'E')) {
        p->pos++;
        if (p->pos < p->n && (p->s[p->pos] == '+' || p->s[p->pos] == '-'))
            p->pos++;
        size_t start = p->pos;
        while (p->pos < p->n && digit(p->s[p->pos])) p->pos++;
        if (start == p->pos) return false;
    }
    return true;
}

static int parse_value(Parser *p, int parent, int depth)
{
    skip_space(p);
    if (p->pos == p->n || depth > 32) return -1;
    char c = p->s[p->pos];
    if (c == '"') return parse_string(p, parent);
    int type = c == '{' ? JSON_OBJECT : c == '[' ? JSON_ARRAY : JSON_PRIMITIVE;
    int index = add_token(p, parent, type);
    if (index < 0) return -1;
    if (type == JSON_PRIMITIVE) {
        if (c == 't' || c == 'f' || c == 'n') {
            const char *word = c == 't' ? "true" : c == 'f' ? "false" : "null";
            size_t size = strlen(word);
            if (p->n - p->pos < size || memcmp(p->s + p->pos, word, size))
                return -1;
            p->pos += size;
        } else if (!parse_number(p)) return -1;
    } else {
        char end = type == JSON_OBJECT ? '}' : ']';
        p->pos++;
        skip_space(p);
        if (p->pos < p->n && p->s[p->pos] == end) p->pos++;
        else for (;;) {
            if (type == JSON_OBJECT) {
                skip_space(p);
                if (p->pos == p->n || p->s[p->pos] != '"') return -1;
                int key = parse_string(p, index);
                if (key < 0 || !unique_key(p, index, key)) return -1;
                skip_space(p);
                if (p->pos == p->n || p->s[p->pos++] != ':') return -1;
            }
            if (parse_value(p, index, depth + 1) < 0) return -1;
            skip_space(p);
            if (p->pos == p->n) return -1;
            c = p->s[p->pos++];
            if (c == end) break;
            if (c != ',') return -1;
        }
    }
    p->tokens[index].end = p->pos;
    return index;
}

int json_parse(const char *s, size_t n, Token *tokens, int capacity)
{
    Parser p = { s, n, 0, tokens, 0, capacity };
    if (capacity <= 0 || parse_value(&p, -1, 0) < 0) return -1;
    skip_space(&p);
    return p.pos == n ? p.count : -1;
}

int json_equal(const char *s, const Token *token, const char *value)
{
    if (token->type != JSON_STRING)
        return token->end - token->start == strlen(value) &&
               !memcmp(s + token->start, value, strlen(value));
    size_t pos = token->start, v = 0, size = strlen(value);
    while (pos < token->end && v < size) {
        uint32_t a, b;
        if (!character(s, token->end, &pos, &a) ||
            !utf8(value, size, &v, &b) || a != b) return 0;
    }
    return pos == token->end && v == size;
}

int json_field(const char *s, const Token *tokens, int count, int object,
               const char *key)
{
    if (object < 0 || object >= count || tokens[object].type != JSON_OBJECT)
        return -1;
    int i = object + 1;
    while (i < count && tokens[i].parent == object) {
        int key_index = i++;
        if (i == count) return -1;
        if (json_equal(s, &tokens[key_index], key)) return i;
        i++;
        while (i < count && tokens[i].parent != object) i++;
    }
    return -1;
}

int json_string(const char *s, const Token *token, char *out, size_t capacity)
{
    if (token->type != JSON_STRING || !capacity) return -1;
    size_t pos = token->start, used = 0;
    while (pos < token->end) {
        uint32_t cp;
        if (!character(s, token->end, &pos, &cp) || cp == 0) return -1;
        unsigned char bytes[4];
        size_t n;
        if (cp < 0x80) { bytes[0] = (unsigned char)cp; n = 1; }
        else if (cp < 0x800) {
            bytes[0] = (unsigned char)(0xc0 | (cp >> 6));
            bytes[1] = (unsigned char)(0x80 | (cp & 63)); n = 2;
        } else if (cp < 0x10000) {
            bytes[0] = (unsigned char)(0xe0 | (cp >> 12));
            bytes[1] = (unsigned char)(0x80 | ((cp >> 6) & 63));
            bytes[2] = (unsigned char)(0x80 | (cp & 63)); n = 3;
        } else {
            bytes[0] = (unsigned char)(0xf0 | (cp >> 18));
            bytes[1] = (unsigned char)(0x80 | ((cp >> 12) & 63));
            bytes[2] = (unsigned char)(0x80 | ((cp >> 6) & 63));
            bytes[3] = (unsigned char)(0x80 | (cp & 63)); n = 4;
        }
        if (capacity - used <= n) return -1;
        memcpy(out + used, bytes, n);
        used += n;
    }
    out[used] = 0;
    return (int)used;
}

int json_quote(const char *s, char *out, size_t capacity)
{
    static const char digits[] = "0123456789abcdef";
    size_t used = 0;
    if (capacity < 3) return -1;
    out[used++] = '"';
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        size_t needed = *p < 32 ? 6 : (*p == '"' || *p == '\\') ? 2 : 1;
        if (capacity - used <= needed + 1) return -1;
        if (*p < 32) {
            memcpy(out + used, "\\u00", 4); used += 4;
            out[used++] = digits[*p >> 4]; out[used++] = digits[*p & 15];
        } else {
            if (*p == '"' || *p == '\\') out[used++] = '\\';
            out[used++] = (char)*p;
        }
    }
    out[used++] = '"'; out[used] = 0;
    return (int)used;
}
