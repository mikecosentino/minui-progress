#include "progress_line.h"

#include <stdlib.h>
#include <string.h>

// copy_field copies [start, end) into dst, truncating to fit and trimming
// trailing whitespace.
static void copy_field(char *dst, const char *start, const char *end)
{
    size_t len = (size_t)(end - start);
    if (len >= PROGRESS_TEXT_MAX)
        len = PROGRESS_TEXT_MAX - 1;
    memcpy(dst, start, len);
    dst[len] = '\0';

    while (len > 0 && (dst[len - 1] == ' ' || dst[len - 1] == '\r' || dst[len - 1] == '\t'))
        dst[--len] = '\0';
}

int ProgressLine_Parse(const char *text, ProgressLine *out)
{
    if (text == NULL || out == NULL)
        return 0;

    // first line only
    const char *line_end = strchr(text, '\n');
    if (line_end == NULL)
        line_end = text + strlen(text);

    // split into up to three tab-separated fields
    const char *fields[3] = {text, NULL, NULL};
    const char *ends[3] = {line_end, line_end, line_end};
    int count = 1;
    for (const char *p = text; p < line_end && count < 3; p++)
    {
        if (*p == '\t')
        {
            ends[count - 1] = p;
            fields[count] = p + 1;
            count++;
        }
    }

    char first[64];
    copy_field(first, fields[0], ends[0]);

    ProgressLine parsed;
    memset(&parsed, 0, sizeof(parsed));

    if (strcmp(first, "done") == 0)
    {
        parsed.done = 1;
        parsed.percent = 100;
    }
    else
    {
        if (first[0] == '\0')
            return 0;

        char *rest = NULL;
        double value = strtod(first, &rest);
        if (rest == first)
            return 0;
        // a trailing % is fine; anything else after the number is not
        if (*rest == '%')
            rest++;
        if (*rest != '\0')
            return 0;

        if (value < 0)
        {
            parsed.indeterminate = 1;
            parsed.percent = 0;
        }
        else
        {
            parsed.percent = value > 100 ? 100 : value;
        }
    }

    if (count > 1)
        copy_field(parsed.label, fields[1], ends[1]);
    if (count > 2)
        copy_field(parsed.sublabel, fields[2], ends[2]);

    *out = parsed;
    return 1;
}
