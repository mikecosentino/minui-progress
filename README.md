# minui-progress

A real progress bar for [MinUI](https://github.com/shauninman/MinUI) and
[NextUI](https://github.com/LoveRetro/NextUI) paks.

A pak is a shell script, and shell scripts can't draw. The usual workaround is
[minui-presenter](https://github.com/josegonzalez/minui-presenter) with a JSON file
of one screen per percent, stepped forward with `SIGUSR1`. That breaks in unpleasant
ways: a signal that arrives before the presenter installs its handler kills it.
`minui-progress` reads the progress from a file instead, and draws the bar itself.

```sh
minui-progress --file /tmp/progress --title "Metroid Fusion" --cancel-show &
pid=$!

printf -- '-1\tConnecting\n'                        > /tmp/progress   # sweeping bar
printf '42.5\tDownloading\t112 MB of 263 MB\n'     > /tmp/progress
printf '80\tExtracting\n'                           > /tmp/progress
echo done                                           > /tmp/progress   # exits 0

wait "$pid"   # 0 done, 2 cancelled with B
```

## The progress file

One line, tab-separated. Only the first line is read.

```
<percent>\t<label>\t<sublabel>
```

| field | |
|---|---|
| `percent` | `0`–`100` (a trailing `%` is fine; above 100 clamps). **Negative** means progress can't be measured: the bar sweeps back and forth. The literal **`done`** exits 0. |
| `label` | optional; drawn under the bar on the left, with the percentage on the right |
| `sublabel` | optional; drawn dimmed under the label, e.g. `112 MB of 263 MB` or `3 of 12` |

The file is **polled** every 100 ms, not read from a pipe. The writer never blocks,
and if the progress screen dies the job carries on. A missing or half-written file
is not an error: the screen keeps showing what it showed last.

When the percent goes down (a new step starting, e.g. download done, extraction
begins), the bar restarts from there instead of sliding backwards. When it goes up,
the bar eases towards the new value.

## Options

| flag | default | |
|---|---|---|
| `--file <path>` | *(required)* | the progress file |
| `--title <text>` | none | large text above the bar; truncated with `…` if too wide |
| `--cancel-show` | off | show a cancel button and exit `2` when it is pressed |
| `--cancel-button A\|B\|X\|Y` | `B` | |
| `--cancel-text <text>` | `CANCEL` | |
| `--show-hardware-group` | off | battery/wifi in the corner, as in the menu |

## Exit codes

| code | |
|---|---|
| `0` | the file said `done` |
| `1` | bad arguments |
| `2` | the cancel button was pressed |
| `130` / `143` | SIGINT / SIGTERM |

Auto-sleep is disabled while it runs, since a download shouldn't stall because
the screen dimmed.

## Look

On NextUI the colours come from your theme: the track and fill use the same
colours as the system brightness and volume sliders, and the text uses the theme
text colours. The MinUI builds use MinUI's greys and white.

## Building

This uses the same build as minui-presenter (see its docs for the toolchains).
CI builds every platform; releases attach the binaries.

```sh
# macOS, for development (keyboard stands in for the buttons)
brew install sdl2 sdl2_image sdl2_ttf pkg-config
PLATFORM=macos make setup-resources
PLATFORM=macos make
./minui-progress-macos --file /tmp/progress --title Test --cancel-show

make test   # SDL-free unit tests for the progress-line parser
```

Setting `MINUI_PROGRESS_SNAPSHOT=/path/frame.bmp` saves every drawn frame, so you
can check the layout without a device.

## Credits

Built on [minui-presenter](https://github.com/josegonzalez/minui-presenter) by Jose
Diaz-Gonzalez (MIT): the platform glue, button groups, power handling and build
scaffolding are his.
