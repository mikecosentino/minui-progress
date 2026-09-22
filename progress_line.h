#ifndef PROGRESS_LINE_H
#define PROGRESS_LINE_H

// progress_line parses the one line minui-progress reads from its --file. It is
// SDL-free so it can be unit tested with the host compiler (see
// test/progress_line_test.c).
//
// The line is tab-separated:
//
//   <percent>\t<label>\t<sublabel>
//
//   42.5\tDownloading\t112 MB of 263 MB
//   -1\tExtracting                         (negative: indeterminate)
//   done                                   (finished; exit 0)
//
// Only the first line of the file is read. The label and sublabel are optional.
// A trailing "%" on the percent is accepted, and values above 100 clamp to 100.

#define PROGRESS_TEXT_MAX 256

typedef struct
{
    // non-zero when the line is the literal "done"
    int done;
    // non-zero when the percent is negative: progress that cannot be measured
    int indeterminate;
    // 0 to 100; meaningless when indeterminate or done
    double percent;
    char label[PROGRESS_TEXT_MAX];
    char sublabel[PROGRESS_TEXT_MAX];
} ProgressLine;

// ProgressLine_Parse fills *out from text. Returns non-zero on success and zero
// when the text holds no usable percent -- an empty file, or one caught half
// written -- in which case *out is left untouched, so the caller keeps drawing
// what it drew last rather than flickering to zero.
int ProgressLine_Parse(const char *text, ProgressLine *out);

#endif // PROGRESS_LINE_H
