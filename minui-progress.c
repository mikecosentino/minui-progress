// minui-progress draws a real progress bar for MinUI and NextUI paks.
//
// A shell script cannot draw, and minui-presenter can only show messages, so paks
// have faked progress by pre-rendering a screen per percent and stepping through
// them with SIGUSR1. This reads the progress from a file instead:
//
//   minui-progress --file /tmp/progress --title "Metroid Fusion" --cancel-show &
//   printf '42.5\tDownloading\t112 MB of 263 MB\n' > /tmp/progress
//   ...
//   echo done > /tmp/progress        # exits 0
//
// The line format is described in progress_line.h. The file is polled rather than
// read from a pipe, so the writer never blocks, and a progress screen that dies
// takes nothing down with it.
//
// Built on minui-presenter (MIT, Jose Diaz-Gonzalez): the same MinUI/NextUI
// platform code, button groups, power handling and build scaffolding.

#include <fcntl.h>
#include <getopt.h>
#include <math.h>
#include <msettings.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>
#ifdef USE_SDL2
#include <SDL2/SDL_ttf.h>
#else
#include <SDL/SDL_ttf.h>
#endif

#include "defines.h"
#include "api.h"
#include "utils.h"

#include "progress_line.h"

// Platform compatibility: NextUI names this PWR_isOnline
#ifdef PLATFORM_NEXTUI
#define PLAT_isOnline PWR_isOnline
#endif

enum
{
    ExitCodeSuccess = 0,
    ExitCodeError = 1,
    ExitCodeCancelButton = 2,
    ExitCodeKeyboardInterrupt = 130,
    ExitCodeSigterm = 143,
};

// the bar's height, before scaling -- thicker than the settings slider it
// borrows its assets from, since it is the whole screen's subject here
#define BAR_HEIGHT 12
// how often the progress file is read
#define POLL_MS 100
// the indeterminate bar: a segment this fraction of the track, sweeping across
// and back once per this many milliseconds
#define SWEEP_FRACTION 0.28
#define SWEEP_MS 1600

struct AppState
{
    int redraw;
    int quitting;
    int exit_code;

    char file[1024];
    char title[1024];

    char cancel_button[16];
    char cancel_text[256];
    bool cancel_show;
    bool show_hardware_group;

    // what the file said last, raw, so an unchanged file costs no redraw
    char last_raw[1024];
    // what is being drawn
    ProgressLine progress;
    // the percent actually on screen, easing towards progress.percent so a
    // jump from 10% to 40% slides rather than snaps
    double shown_percent;

    long long last_poll_ms;
};

SDL_Surface *screen = NULL;

static long long now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

void log_error(const char *msg)
{
    setvbuf(stderr, NULL, _IONBF, 0);
    fprintf(stderr, "%s\n", msg);
}

// The theme's colors on NextUI; MinUI's fixed palette otherwise.
static SDL_Color text_color(void)
{
#ifdef PLATFORM_NEXTUI
    return uintToColour(THEME_COLOR4_255);
#else
    return COLOR_WHITE;
#endif
}

static SDL_Color dim_text_color(void)
{
#ifdef PLATFORM_NEXTUI
    return uintToColour(THEME_COLOR6_255);
#else
    return COLOR_GRAY;
#endif
}

static uint32_t background_color(SDL_Surface *dst)
{
#ifdef PLATFORM_NEXTUI
    (void)dst;
    return THEME_COLOR7;
#else
    return SDL_MapRGBA(dst->format, 0, 0, 0, 255);
#endif
}

// The track and fill take the colors of the system's own brightness and volume
// sliders -- the theme's on NextUI, MinUI's greys otherwise -- so the bar looks
// like it belongs.
//
// They are drawn rather than blitted from the slider assets: GFX_blitPill cuts
// its end caps from an asset only SETTINGS_SIZE tall, so at any other height the
// caps come out as dots on the ends of a rectangle.
static uint32_t track_color(void)
{
#ifdef PLATFORM_NEXTUI
    return THEME_COLOR3;
#else
    return RGB_DARK_GRAY;
#endif
}

static uint32_t fill_color(void)
{
#ifdef PLATFORM_NEXTUI
    return THEME_COLOR1;
#else
    return RGB_WHITE;
#endif
}

// fill_pill fills a rect with fully rounded ends, one scanline per row: each
// row of the end caps is inset by how far the circle curves in at that height.
static void fill_pill(SDL_Surface *dst, SDL_Rect rect, uint32_t color)
{
    int h = rect.h;
    int r = h / 2;
    if (rect.w < h)
        rect.w = h;

    for (int row = 0; row < h; row++)
    {
        // distance of this row's centre from the pill's horizontal midline
        double dy = (row + 0.5) - h / 2.0;
        double inset = r - sqrt((double)r * r - dy * dy);
        if (inset < 0)
            inset = 0;
        int in = (int)(inset + 0.5);
        SDL_Rect line = {rect.x + in, rect.y + row, rect.w - in * 2, 1};
        if (line.w > 0)
            SDL_FillRect(dst, &line, color);
    }
}

static void blit_track(SDL_Surface *dst, SDL_Rect *rect)
{
    fill_pill(dst, *rect, track_color());
}

static void blit_fill(SDL_Surface *dst, SDL_Rect *rect)
{
    fill_pill(dst, *rect, fill_color());
}

// read_progress polls the file. Returns non-zero when what is to be drawn has
// changed. A missing file is not an error: the writer may simply not have
// started yet, and until it does the bar sweeps.
static int read_progress(struct AppState *state)
{
    char raw[1024] = "";
    int fd = open(state->file, O_RDONLY);
    if (fd >= 0)
    {
        ssize_t n = read(fd, raw, sizeof(raw) - 1);
        close(fd);
        raw[n > 0 ? n : 0] = '\0';
    }

    if (strcmp(raw, state->last_raw) == 0)
        return 0;
    strncpy(state->last_raw, raw, sizeof(state->last_raw) - 1);

    ProgressLine parsed;
    if (!ProgressLine_Parse(raw, &parsed))
        return 0;

    // a bar that goes backwards is a new step starting (download done,
    // extraction begins); show it from its own start rather than easing down
    if (!parsed.indeterminate && parsed.percent < state->shown_percent)
        state->shown_percent = parsed.percent;

    state->progress = parsed;
    return 1;
}

// draw_text blits a line of text centered at y, or left/right aligned inside
// [x, x + w] when align is -1 / 1. Text too wide for w is truncated with an
// ellipsis. Returns the height used.
static int draw_text(SDL_Surface *dst, TTF_Font *f, const char *text, SDL_Color color,
                     int x, int y, int w, int align)
{
    if (text == NULL || text[0] == '\0')
        return 0;

    char truncated[PROGRESS_TEXT_MAX * 2];
    GFX_truncateText(f, text, truncated, w, 0);

    SDL_Surface *surface = TTF_RenderUTF8_Blended(f, truncated, color);
    if (surface == NULL)
        return 0;

    int dx = x + (w - surface->w) / 2;
    if (align < 0)
        dx = x;
    else if (align > 0)
        dx = x + w - surface->w;

    SDL_Rect pos = {dx, y, surface->w, surface->h};
    SDL_BlitSurface(surface, NULL, dst, &pos);
    int h = surface->h;
    SDL_FreeSurface(surface);
    return h;
}

// draw_screen lays out, top to bottom and centered as a block:
//
//   title                (large)
//   [=========      ]    the bar
//   label        42%     (small)
//   sublabel             (small, dimmed)
void draw_screen(SDL_Surface *dst, struct AppState *state)
{
    SDL_FillRect(dst, NULL, background_color(dst));

    int margin = SCALE1(PADDING * 4);
    int width = dst->w - margin * 2;
    int bar_h = SCALE1(BAR_HEIGHT);
    int gap = SCALE1(PADDING);

    int title_h = TTF_FontHeight(font.large);
    int small_h = TTF_FontHeight(font.small);
    int has_title = state->title[0] != '\0';
    int has_sub = state->progress.sublabel[0] != '\0';

    int block_h = bar_h + gap + small_h;
    if (has_title)
        block_h += title_h + gap * 2;
    if (has_sub)
        block_h += small_h;

    int y = (dst->h - block_h) / 2;

    if (has_title)
    {
        draw_text(dst, font.large, state->title, text_color(), margin, y, width, 0);
        y += title_h + gap * 2;
    }

    SDL_Rect track = {margin, y, width, bar_h};
    blit_track(dst, &track);

    if (state->progress.indeterminate)
    {
        // ping-pong a segment across the track
        int seg = (int)(width * SWEEP_FRACTION);
        double phase = (double)(now_ms() % SWEEP_MS) / SWEEP_MS; // 0..1
        double t = phase < 0.5 ? phase * 2 : (1 - phase) * 2;     // 0..1..0
        int sx = margin + (int)((width - seg) * t);
        SDL_Rect fill = {sx, y, seg, bar_h};
        blit_fill(dst, &fill);
    }
    else if (state->shown_percent > 0)
    {
        int fw = (int)(width * state->shown_percent / 100.0);
        // a pill narrower than it is tall draws its two end caps overlapping;
        // hold the fill at a round dot until there is room for a bar
        if (fw < bar_h)
            fw = bar_h;
        SDL_Rect fill = {margin, y, fw, bar_h};
        blit_fill(dst, &fill);
    }
    y += bar_h + gap;

    char percent_text[16] = "";
    int percent_w = 0;
    if (!state->progress.indeterminate)
    {
        snprintf(percent_text, sizeof(percent_text), "%d%%", (int)floor(state->shown_percent + 0.0001));
        TTF_SizeUTF8(font.small, percent_text, &percent_w, NULL);
        draw_text(dst, font.small, percent_text, text_color(), margin, y, width, 1);
    }
    draw_text(dst, font.small, state->progress.label, text_color(),
              margin, y, width - percent_w - (percent_w ? gap : 0), -1);
    y += small_h;

    if (has_sub)
        draw_text(dst, font.small, state->progress.sublabel, dim_text_color(), margin, y, width, -1);

    if (state->cancel_show)
        GFX_blitButtonGroup((char *[]){state->cancel_button, state->cancel_text, NULL}, 1, dst, 1);

    state->redraw = 0;
}

static int button_for(const char *name)
{
    if (strcmp(name, "A") == 0)
        return BTN_A;
    if (strcmp(name, "B") == 0)
        return BTN_B;
    if (strcmp(name, "X") == 0)
        return BTN_X;
    if (strcmp(name, "Y") == 0)
        return BTN_Y;
    return BTN_NONE;
}

void handle_input(struct AppState *state)
{
    PAD_poll();

    if (!state->cancel_show)
        return;

    int button = button_for(state->cancel_button);
    if (button != BTN_NONE && PAD_justReleased(button))
    {
        state->quitting = 1;
        state->exit_code = ExitCodeCancelButton;
    }
}

void signal_handler(int signal)
{
    if (signal == SIGINT)
        exit(ExitCodeKeyboardInterrupt);
    if (signal == SIGTERM)
        exit(ExitCodeSigterm);
    exit(ExitCodeError);
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s --file <path> [--title <text>] [--cancel-show]\n"
            "          [--cancel-button A|B|X|Y] [--cancel-text <text>]\n"
            "          [--show-hardware-group]\n"
            "\n"
            "Reads '<percent>\\t<label>\\t<sublabel>' from --file (a negative percent\n"
            "sweeps; 'done' exits 0). Exits 2 when the cancel button is pressed.\n",
            argv0);
}

bool parse_arguments(struct AppState *state, int argc, char *argv[])
{
    static struct option long_options[] = {
        {"file", required_argument, 0, 'f'},
        {"title", required_argument, 0, 't'},
        {"cancel-button", required_argument, 0, 'b'},
        {"cancel-text", required_argument, 0, 'c'},
        {"cancel-show", no_argument, 0, 'C'},
        {"show-hardware-group", no_argument, 0, 'H'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}};

    int opt;
    while ((opt = getopt_long(argc, argv, "f:t:b:c:CHh", long_options, NULL)) != -1)
    {
        switch (opt)
        {
        case 'f':
            strncpy(state->file, optarg, sizeof(state->file) - 1);
            break;
        case 't':
            strncpy(state->title, optarg, sizeof(state->title) - 1);
            break;
        case 'b':
            strncpy(state->cancel_button, optarg, sizeof(state->cancel_button) - 1);
            break;
        case 'c':
            strncpy(state->cancel_text, optarg, sizeof(state->cancel_text) - 1);
            break;
        case 'C':
            state->cancel_show = true;
            break;
        case 'H':
            state->show_hardware_group = true;
            break;
        default:
            usage(argv[0]);
            return false;
        }
    }

    if (state->file[0] == '\0')
    {
        log_error("minui-progress: --file is required");
        usage(argv[0]);
        return false;
    }
    if (button_for(state->cancel_button) == BTN_NONE)
    {
        log_error("minui-progress: --cancel-button must be A, B, X or Y");
        return false;
    }

    return true;
}

// MinUI's init and teardown print to stdout on some platforms; the caller is
// usually a script whose stdout is a log or a command substitution, so both
// run with stdout and stderr pointed at /dev/null.
static void quietly(void (*func)(void))
{
    int out = dup(STDOUT_FILENO);
    int err = dup(STDERR_FILENO);
    fcntl(out, F_SETFD, FD_CLOEXEC);
    fcntl(err, F_SETFD, FD_CLOEXEC);
    int devnull = open("/dev/null", O_WRONLY);
    dup2(devnull, STDOUT_FILENO);
    dup2(devnull, STDERR_FILENO);
    close(devnull);

    func();

    fflush(stdout);
    fflush(stderr);
    dup2(out, STDOUT_FILENO);
    dup2(err, STDERR_FILENO);
    close(out);
    close(err);
}

static void init(void)
{
    PWR_setCPUSpeed(CPU_SPEED_MENU);
    screen = GFX_init(MODE_MAIN);
    PAD_init();
    PWR_init();
    InitSettings();
}

static void destruct(void)
{
    QuitSettings();
    PWR_quit();
    PAD_quit();
    GFX_quit();
}

int main(int argc, char *argv[])
{
    struct AppState state;
    memset(&state, 0, sizeof(state));
    state.redraw = 1;
    state.exit_code = ExitCodeSuccess;
    state.progress.indeterminate = 1;
    strncpy(state.cancel_button, "B", sizeof(state.cancel_button) - 1);
    strncpy(state.cancel_text, "CANCEL", sizeof(state.cancel_text) - 1);

    if (!parse_arguments(&state, argc, argv))
        return ExitCodeError;

    quietly(init);

    struct sigaction sa = {.sa_handler = signal_handler, .sa_flags = SA_RESTART};
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    // a progress screen is waiting on work the device is doing; sleeping
    // part-way through a download would stall it
    PWR_disableAutosleep();

    int was_online = PLAT_isOnline();
    int show_setting = 0;

    read_progress(&state);
    state.shown_percent = state.progress.percent;
    state.last_poll_ms = now_ms();

    while (!state.quitting)
    {
        GFX_startFrame();
        PWR_update(&state.redraw, &show_setting, NULL, NULL);

        int is_online = PLAT_isOnline();
        if (was_online != is_online)
            state.redraw = 1;
        was_online = is_online;

        handle_input(&state);
        if (state.quitting)
            break;

        long long now = now_ms();
        if (now - state.last_poll_ms >= POLL_MS)
        {
            state.last_poll_ms = now;
            if (read_progress(&state))
                state.redraw = 1;
            if (state.progress.done)
            {
                state.exit_code = ExitCodeSuccess;
                break;
            }
        }

        // ease the shown percent towards the real one, and keep the
        // indeterminate sweep moving
        if (!state.progress.indeterminate)
        {
            double diff = state.progress.percent - state.shown_percent;
            if (fabs(diff) > 0.05)
            {
                state.shown_percent += diff * 0.2;
                state.redraw = 1;
            }
            else if (diff != 0)
            {
                state.shown_percent = state.progress.percent;
                state.redraw = 1;
            }
        }
        else
        {
            state.redraw = 1;
        }

        if (state.redraw)
        {
            GFX_clear(screen);
            draw_screen(screen, &state);
            if (state.show_hardware_group)
                GFX_blitHardwareGroup(screen, show_setting);
            // development aid: MINUI_PROGRESS_SNAPSHOT=<path.bmp> saves every
            // drawn frame, so the layout can be checked without a device
            const char *snapshot = getenv("MINUI_PROGRESS_SNAPSHOT");
            if (snapshot != NULL && snapshot[0] != '\0')
            {
                // written aside and renamed, so a reader never catches a
                // half-written frame (BMP rows run bottom-up; the top goes
                // missing first)
                char tmp[1100];
                snprintf(tmp, sizeof(tmp), "%s.tmp", snapshot);
                if (SDL_SaveBMP(screen, tmp) == 0)
                    rename(tmp, snapshot);
            }
            GFX_flip(screen);
        }
        else
        {
            GFX_sync();
        }
    }

    quietly(destruct);
    return state.exit_code;
}
