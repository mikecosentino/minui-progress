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

## Recipes

Everything below is a plain `sh` script of the kind a pak is made of. The shape is
always the same: start `minui-progress` in the background, write a line to its
file whenever you learn something new, and write `done` at the end.

Keep the progress file on `/tmp` (tmpfs), not the SD card — it gets rewritten
hundreds of times.

### A helper to write the file

Worth pasting at the top of a pak. Everything below uses it.

```sh
PROGRESS=/tmp/minui-progress.$$

# progress <percent|-1|done> [label] [sublabel]
progress() {
    printf '%s\t%s\t%s\n' "$1" "$2" "$3" > "$PROGRESS"
}

progress 0 "Starting"
```

### Downloading a file

The total is in the `Content-Length` header and how much has arrived is the size
of the file on disk, so a one-second poll is all it takes. If the server won't
say how big the file is, fall back to the sweeping bar with `-1`.

```sh
url=$1
out=$2

minui-progress --file "$PROGRESS" --title "Metroid Fusion" --cancel-show &
ui=$!

progress -1 "Connecting"
total=$(curl -sfI "$url" | awk 'tolower($1) == "content-length:" { print $2 + 0 }')

curl -sf -o "$out" "$url" &
dl=$!

while kill -0 "$dl" 2>/dev/null; do
    got=$(wc -c < "$out" 2>/dev/null | tr -d ' ')
    : "${got:=0}"
    if [ "${total:-0}" -gt 0 ]; then
        progress $((got * 100 / total)) "Downloading" \
            "$((got / 1048576)) MB of $((total / 1048576)) MB"
    else
        progress -1 "Downloading" "$((got / 1048576)) MB"
    fi
    sleep 1
done

progress done
wait "$ui"
```

### Letting the user cancel

`--cancel-show` draws a cancel button and exits `2` when it is pressed. Nothing
is cancelled for you: run the work in the background, wait on the progress
screen, and kill the work if the screen comes back `2`.

```sh
sync_saves() {
    # ... writes $PROGRESS as it goes, and ends with: progress done
    :
}

minui-progress --file "$PROGRESS" --title "Syncing saves" --cancel-show &
ui=$!
sync_saves &
job=$!

wait "$ui"
code=$?

if [ "$code" -eq 2 ]; then
    kill "$job" 2>/dev/null
    rm -f "$PROGRESS"
    exit 2
fi

wait "$job"
rm -f "$PROGRESS"
```

A `trap 'kill "$ui" "$job" 2>/dev/null' EXIT INT TERM` on top of this is worth
having, so a pak that dies halfway doesn't leave a progress screen sitting over
the menu.

### Working through a list

Counted work is the easy case: the percent is the index, and the sublabel is the
`3 of 12` that tells the user nothing has hung.

```sh
minui-progress --file "$PROGRESS" --title "Box art" &
ui=$!

total=$(ls /mnt/SDCARD/Roms/GBA/*.gba 2>/dev/null | wc -l | tr -d ' ')
i=0

for rom in /mnt/SDCARD/Roms/GBA/*.gba; do
    i=$((i + 1))
    progress $((i * 100 / total)) "$(basename "$rom")" "$i of $total"
    scrape_box_art "$rom"
done

progress done
wait "$ui"
```

### Syncing with rsync

`--info=progress2` prints a running percentage for the whole transfer. Pick it
out of each line and pass it straight through — a trailing `%` is accepted.

```sh
minui-progress --file "$PROGRESS" --title "Saves → NAS" --cancel-show &
ui=$!

rsync -a --info=progress2 /mnt/SDCARD/Saves/ nas:/backup/saves/ 2>/dev/null |
    while read -r line; do
        pct=$(printf '%s\n' "$line" | grep -o '[0-9][0-9]*%' | tail -1)
        [ -n "$pct" ] && progress "$pct" "Uploading saves"
    done

progress done
wait "$ui"
```

### Extracting an archive

`unzip` prints a line per file, so counting those lines against the archive's
file count gives a bar for free.

```sh
total=$(unzip -l "$zip" | tail -1 | awk '{ print $2 }')
i=0

unzip -o "$zip" -d "$dest" |
    while read -r line; do
        case $line in
        *inflating:* | *extracting:*)
            i=$((i + 1))
            progress $((i * 100 / total)) "Extracting" "$i of $total files"
            ;;
        esac
    done

progress done
```

### A job with several steps

When the percent goes *down* the bar restarts from there rather than sliding
backwards, so the simplest thing is a fresh 0–100 per step, with the step's name
as the label:

```sh
progress 0 "Downloading"
# ... 0 → 100
progress 0 "Extracting"
# ... 0 → 100
progress 0 "Installing"
# ... 0 → 100
progress done
```

If you'd rather have one bar that only ever moves forward, give each step a slice
of it:

```sh
# step <base> <span> <percent-within-step> <label> [sublabel]
step() {
    progress $(($1 + $3 * $2 / 100)) "$4" "$5"
}

step 0 60 "$pct" "Downloading" "$got MB of $size MB"   #  0 →  60
step 60 30 "$pct" "Extracting"  "$i of $files files"   # 60 →  90
step 90 10 "$pct" "Installing"                         # 90 → 100
progress done
```

### Waiting for something unmeasurable

A negative percent sweeps the bar, which is the honest thing to show while
waiting on a network, a mount, or anything else with no number attached to it.

```sh
minui-progress --file "$PROGRESS" --title "Online play" &
ui=$!

progress -1 "Waiting for Wi-Fi"
until ping -c1 -W1 1.1.1.1 >/dev/null 2>&1; do sleep 1; done

progress -1 "Connecting to server"
connect_to_server

progress done
wait "$ui"
```

### Wrapping a command that knows nothing about progress

Because the bar is driven by a file and not a pipe, the job's own output can be
left alone and a copy of it sniffed with `tee`.

```sh
make_firmware_backup 2>&1 | tee /tmp/backup.log |
    while read -r line; do
        case $line in
        "Reading block "*) progress "${line##* }" "Backing up" "$line" ;;
        esac
    done
```

None of this can break the job. The file is polled rather than read from a pipe,
so a write never blocks, and a file caught half-written is not an error — the
screen keeps showing what it showed last.
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
