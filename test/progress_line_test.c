// Unit tests for progress_line.c. Host-compiled by `make test`; no SDL needed.

#include "progress_line.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond)                                                    \
    do                                                                 \
    {                                                                  \
        if (!(cond))                                                   \
        {                                                              \
            fprintf(stderr, "%s:%d: FAIL: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                \
        }                                                              \
    } while (0)

static ProgressLine parse_ok(const char *text)
{
    ProgressLine line;
    memset(&line, 0xAB, sizeof(line));
    CHECK(ProgressLine_Parse(text, &line) == 1);
    return line;
}

int main(void)
{
    ProgressLine l;

    l = parse_ok("42.5\tDownloading\t112 MB of 263 MB\n");
    CHECK(fabs(l.percent - 42.5) < 1e-9);
    CHECK(!l.done && !l.indeterminate);
    CHECK(strcmp(l.label, "Downloading") == 0);
    CHECK(strcmp(l.sublabel, "112 MB of 263 MB") == 0);

    l = parse_ok("7");
    CHECK(l.percent == 7 && l.label[0] == '\0' && l.sublabel[0] == '\0');

    l = parse_ok("88%\tConverting");
    CHECK(l.percent == 88 && strcmp(l.label, "Converting") == 0);

    l = parse_ok("250\tover");
    CHECK(l.percent == 100);

    l = parse_ok("-1\tExtracting\r\n");
    CHECK(l.indeterminate && strcmp(l.label, "Extracting") == 0);

    l = parse_ok("done\n");
    CHECK(l.done && l.percent == 100);

    l = parse_ok("done\tFinished");
    CHECK(l.done && strcmp(l.label, "Finished") == 0);

    // only the first line counts
    l = parse_ok("10\tfirst\n90\tsecond\n");
    CHECK(l.percent == 10 && strcmp(l.label, "first") == 0);

    // an empty label between tabs keeps the sublabel in its own slot
    l = parse_ok("50\t\tjust a sublabel");
    CHECK(l.label[0] == '\0' && strcmp(l.sublabel, "just a sublabel") == 0);

    // UTF-8 passes through untouched
    l = parse_ok("1\tPokémon – Emerald");
    CHECK(strcmp(l.label, "Pokémon – Emerald") == 0);

    // long text truncates rather than overflowing
    char big[1024];
    memset(big, 'x', sizeof(big));
    memcpy(big, "5\t", 2);
    big[sizeof(big) - 1] = '\0';
    l = parse_ok(big);
    CHECK(strlen(l.label) == PROGRESS_TEXT_MAX - 1);

    // failures leave the previous value alone
    ProgressLine keep = parse_ok("33\tkeep me");
    CHECK(ProgressLine_Parse("", &keep) == 0);
    CHECK(ProgressLine_Parse("\n", &keep) == 0);
    CHECK(ProgressLine_Parse("abc\tnope", &keep) == 0);
    CHECK(ProgressLine_Parse("12x", &keep) == 0);
    CHECK(ProgressLine_Parse(NULL, &keep) == 0);
    CHECK(keep.percent == 33 && strcmp(keep.label, "keep me") == 0);

    if (failures)
    {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("progress_line: all tests passed\n");
    return 0;
}
