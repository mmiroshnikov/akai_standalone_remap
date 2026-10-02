# hwremap

Remap MPC hardware buttons to open any screen, on standalone MPC OS (tested on Hakai, MPC 3.9.1,
first-generation MPC Live).

MPC has no setting for this, so `hwremap.so` is an `LD_PRELOAD` shim that sits between the
controller and the MPC app. It rewrites the controller's MIDI stream: a press of a chosen button is
swallowed and replaced with another button or a short sequence of buttons and pads. To MPC this
looks exactly like you pressed them yourself.

The main trick: holding **Menu** and tapping a pad opens the icon in that cell of the 4×4 Mode
Menu. So any button can open any screen. Put the icon in the menu cell and map the button to
"Menu + that pad".

## Current mapping (`hwremap.conf`)

| Button        | Opens (Mode Menu cell)      |
|---------------|-----------------------------|
| Pad Bank A–D  | top row, icons 1–4          |
| `+`           | second row, icon 1          |
| `+` twice     | second row, icon 3          |
| `−`           | second row, icon 2          |
| `−` twice     | second row, icon 4          |

Hold **Shift** to get the original function of a remapped button (Shift + `+`/`−` changes values).
Pad banks A–D are not reachable while the remap is on.

Rearrange the icons in the Mode Menu to choose what each button opens. The change applies right
away, with no restart.

## How it works

- The shim hooks `snd_rawmidi_open` and `snd_rawmidi_read` and only touches the controller input
  port whose name contains "Private". Every other MIDI port passes through untouched.
- Buttons arrive as notes on channel 1: press is `90 nn 7F`, release is `90 nn 00`. Pads are notes
  on channel 10. The shim parses the stream properly, including running status, SysEx and realtime
  bytes, and replaces only the mapped button notes.
- Injected releases use note-on with velocity 0, like the hardware does. MPC ignores note-off for
  buttons: a Menu "released" with `80 7B ..` stays held, and then tapping an icon in the menu opens it
  instead of letting you drag it.
- The config is re-read whenever its modification time changes, so edits apply on the next button
  press.

## Config reference

File: `/sdcard/hwremap.conf`. One rule per line; `#` starts a comment.

```
log 1                 log every input byte to /tmp/hwremap.log (keep 0 normally)
hold 49               while this button (Shift) is held, remapped buttons pass through
54 d123 p0x30 u123    on press of note 54 (+): Menu down, tap pad 0x30, Menu up
dbl 54 d123 p0x2D u123  second press within dblms runs this instead
dblms 350             double-tap window in milliseconds (default 350)
55 b126               tap another button (126 = MPC X XYFX button)
54 mClip_Matrix       Menu + the pad under that mode, looked up in MPC.settings
```

Tokens (N is decimal or `0x` hex):

| Token   | Meaning                                                    |
|---------|------------------------------------------------------------|
| `dN`    | button N down                                              |
| `uN`    | button N up                                                |
| `bN`    | button N tap (down + up)                                   |
| `pN`    | pad tap, N is the pad note                                 |
| `xHH`   | raw MIDI byte                                              |
| `mName` | mode by name, underscores = spaces (must be the only token) |

With a double-tap rule, the first press still runs the single action right away. A double tap
therefore briefly shows the first screen before the second one.

`mName` reads the Mode Menu layout from
`/media/az01-internal/Settings/MPC/MPC.settings`. MPC writes that file only when it saves
preferences or quits, so a moved icon is picked up only after that. Only the 16 cells reachable
with Menu + pad work (`ModeMenuPage1Slot0..15`). Fixed `d123 pN u123` rules do not have this delay.

### Notes

Buttons (channel 1, MPC Live):

| Button | Note | Button     | Note |
|--------|------|------------|------|
| Shift  | 49   | `+`        | 54   |
| Menu   | 123  | `−`        | 55   |
| Main   | 52   | Pad Bank A | 35   |
| Next Seq | 42 | Pad Bank B | 36   |
|        |      | Pad Bank C | 37   |
|        |      | Pad Bank D | 38   |

MPC X buttons that MPC Live also accepts: XYFX 126, Channel Mixer 116, Pad Mixer 115.

Pads (channel 10), laid out like the hardware. With Menu held, each pad opens the icon in the same
position of the Mode Menu:

```
pad 13: 0x31   pad 14: 0x37   pad 15: 0x33   pad 16: 0x35    <- menu row 1
pad  9: 0x30   pad 10: 0x2F   pad 11: 0x2D   pad 12: 0x2B    <- menu row 2
pad  5: 0x28   pad  6: 0x26   pad  7: 0x2E   pad  8: 0x2C    <- menu row 3
pad  1: 0x25   pad  2: 0x24   pad  3: 0x2A   pad  4: 0x52    <- menu row 4
```

To find the note of another button, set `log 1`, press it, and read `/tmp/hwremap.log`.

## Build

Cross-compile for the device (armhf, glibc 2.39 on the device) with Docker:

```sh
cd hwremap
docker run --rm --platform linux/arm/v7 -v "$PWD":/b -w /b arm32v7/gcc:12 bash -euc \
  'gcc -O2 -Wall -Wextra -fPIC -shared -fvisibility=hidden -std=gnu11 hwremap.c -ldl -lpthread -o hwremap.so && strip hwremap.so'
```

Host tests (parser, sequences, double tap, mode lookup) run with ASan/UBSan:

```sh
docker run --rm -v "$PWD":/w -w /w gcc:12 bash -c \
  'gcc -O1 -g -Wall -Wextra -fsanitize=address,undefined test_hwremap.c -ldl -lpthread -o /tmp/t && /tmp/t'
```

## Install

Over SSH as root. The root filesystem is read-only, and BusyBox has no `install`.

```sh
scp hwremap.so root@<mpc>:/tmp/hwremap.so.new
scp hwremap.conf root@<mpc>:/sdcard/hwremap.conf.new

ssh root@<mpc>
mv /sdcard/hwremap.conf.new /sdcard/hwremap.conf
systemctl stop acvs                      # stops MPC, save your project first
mount -o remount,rw /
cp /tmp/hwremap.so.new /usr/lib/hwremap.so.new && chmod 755 /usr/lib/hwremap.so.new
mv /usr/lib/hwremap.so.new /usr/lib/hwremap.so
```

First install only: add the shim to the MPC launcher. Keep a backup and append
`/usr/lib/hwremap.so` to every `LD_PRELOAD` line:

```sh
cp /usr/bin/az01-launch-MPC /usr/bin/az01-launch-MPC.orig-hwremap
sed -i 's#customBufferSizeMPC.so \$CURSOR_SO#customBufferSizeMPC.so /usr/lib/hwremap.so $CURSOR_SO#' /usr/bin/az01-launch-MPC
grep LD_PRELOAD /usr/bin/az01-launch-MPC
```

Then:

```sh
sync; mount -o remount,ro /
systemctl start acvs
grep -c hwremap /proc/$(pidof MPC)/maps  # non-zero = loaded
```

Config changes need no restart. A new `hwremap.so` needs an MPC restart (`systemctl restart acvs`).

## Uninstall

```sh
systemctl stop acvs
mount -o remount,rw /
cp /usr/bin/az01-launch-MPC.orig-hwremap /usr/bin/az01-launch-MPC
rm /usr/lib/hwremap.so
sync; mount -o remount,ro /
systemctl start acvs
```

To disable remapping without uninstalling, empty `/sdcard/hwremap.conf`. With no rules, the shim
passes everything through.

## Limitations

- No MIDI note opens Clip Matrix or Clip Editor directly. Newer-model buttons like Matrix are not
  accepted by this firmware, so those screens are reached through the Mode Menu.
- Injected Shift is ignored: MPC reads the real Shift state, so Shift combos cannot be synthesized.
- Nothing tells the shim which screen is showing, so a mapping cannot toggle between two screens.
- The Lua `--script` option does nothing in the release MPC build.
