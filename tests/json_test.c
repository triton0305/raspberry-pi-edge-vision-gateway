#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "failed at line %d: %s\n", __LINE__, #expr); exit(1); \
} } while (0)

static void invalid(const char *s, size_t len)
{
    Token tokens[128];
    CHECK(json_parse(s, len, tokens, 128) < 0);
}

int main(void)
{
    Token tokens[128];
    const char *document = "{\"before\":{\"type\":\"fake\"},\"ty\\u0070e\":\"vi\\u0073ion\","
                           "\"data\":{\"nested\":[true,false,null,-0.25e+2]},"
                           "\"text\":\"한글 \\ud83d\\ude00 \\\" \\\\ \\n\"}";
    int n = json_parse(document, strlen(document), tokens, 128);
    CHECK(n > 0);
    int type = json_field(document, tokens, n, 0, "type");
    CHECK(type >= 0 && json_equal(document, &tokens[type], "vision"));
    CHECK(json_field(document, tokens, n, 0, "missing") == -1);
    int data = json_field(document, tokens, n, 0, "data");
    CHECK(data >= 0 && tokens[data].type == JSON_OBJECT);
    CHECK(json_field(document, tokens, n, data, "nested") >= 0);
    CHECK(json_field(document, tokens, n, 0, "nested") == -1);
    int text = json_field(document, tokens, n, 0, "text");
    char decoded[128], quoted[256];
    CHECK(json_string(document, &tokens[text], decoded, sizeof(decoded)) > 0);
    CHECK(!strcmp(decoded, "한글 😀 \" \\ \n"));
    CHECK(json_string(document, &tokens[text], decoded, 2) == -1);
    CHECK(json_quote("quoted \" \\ \n", quoted, sizeof(quoted)) > 0);
    n = json_parse(quoted, strlen(quoted), tokens, 128);
    CHECK(n == 1);
    CHECK(json_equal(quoted, &tokens[0], "quoted \" \\ \n"));
    const char *bad[] = {
        "", " ", "{", "[1,]", "{\"a\":1,}", "01", "1.", "1e+", "+1",
        "[true false]", "{\"a\":1,\"a\":2}", "{\"a\":{},\"\\u0061\":2}",
        "\"\\ud800\"", "\"\\udc00\"", "\"\\u123x\"", "\"\\x20\"", "{}[]",
        "\"unescaped\nline\"", "\"\xc0\xaf\"", "\"\xed\xa0\x80\"",
        "\"\xf4\x90\x80\x80\"", "\"\xf0\x9f\""
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        invalid(bad[i], strlen(bad[i]));
    const char nul[] = {'{', '}', '\0'};
    invalid(nul, sizeof(nul));
    const char escaped_nul[] = "\"\\u0000\"";
    CHECK(json_parse(escaped_nul, strlen(escaped_nul), tokens, 128) == 1);
    CHECK(json_string(escaped_nul, &tokens[0], decoded, sizeof(decoded)) < 0);
    CHECK(json_parse("[1,2,3]", 7, tokens, 2) < 0);
    char deep[81];
    memset(deep, '[', 40); memset(deep + 40, ']', 40); deep[80] = 0;
    invalid(deep, 80);
    puts("PASS: JSON structure, escapes, UTF-8, duplicate keys, bounds, nesting");
    return 0;
}
