# Instrumentalls — Pebble remote

A Pebble companion that remote-controls a running `discovery_queue.py` session:
a colour-coded now-playing screen (mirroring the Rich TUI palette) plus a
themeable `MenuLayer` settings/tag menu. Your **Core Time 2 is the `emery`
platform**; the code is colour-safe (`PBL_IF_COLOR_ELSE`) so it also builds for
every other platform (`basalt`, `chalk`, `diorite`, `emery`, `flint`, `gabbro`,
`aplite`), degrading to white-on-black on the B&W ones.

> No album art — the UI is text, colour-coded like the terminal.

## How it fits together

```
Watch (this app, C)  <--AppMessage-->  PebbleKit JS (phone Pebble/Cobble app)
                                             |  HTTP 127.0.0.1:8090
                                             v
                        RemoteControlServer (thread inside discovery_queue.py)
```

The watch app talks only to the phone; the phone's JS (`src/pkjs/index.js`) polls
the discovery-queue HTTP bridge and relays commands. Because Termux and the Pebble
app run on the **same phone**, the default backend URL is `http://127.0.0.1:8090`.

## Prerequisites

**Backend enabled.** In the project `config.ini`:
```ini
[remote_control]
enabled = true
host = 127.0.0.1
port = 8090
token =
```
Then run `python discovery_queue.py` in Termux. Verify:
`curl http://127.0.0.1:8090/api/now-playing`.

## Build & install

A Pebble app compiles to ARM firmware, so it needs the Pebble build toolchain —
which is impractical inside Termux (aarch64: no python2, no ARM cross-compiler).

- **Recommended (phone-only): CloudPebble** — builds in the cloud, installs to the
  watch via the phone's Pebble/Cobble app. See **[CLOUDPEBBLE.md](CLOUDPEBBLE.md)**.
- **On an x86 machine with the Rebble SDK:**
  ```bash
  cd pebble
  pebble build                        # no npm deps — compiles directly
  pebble install --phone <PHONE_IP>   # phone running Termux + discovery_queue
  # UI-only smoke test (backend not reachable from the emulator):
  pebble install --emulator emery
  ```

## Configure

No in-app config page (no Clay dependency — it doesn't yet support the new
`gabbro`/`flint` platforms). The backend is hardcoded at the top of
`src/pkjs/index.js`; edit these two lines only if your `config.ini` differs:
```js
var BACKEND_URL = 'http://127.0.0.1:8090';   // host/port from config.ini
var AUTH_TOKEN = '';                          // only if [remote_control] token is set
```

## Controls

| Input | Action |
|-------|--------|
| UP (short) | previous track (non-destructive) |
| UP (long) | mark Top (toggle) |
| DOWN (short) | skip — remove current track from queue |
| DOWN (long) | mark Untagged (toggle) |
| SELECT (short) | play / pause |
| SELECT (long) | open settings menu |
| BACK | exit |

Small hint icons are drawn on the right edge next to each physical button
(◀◀ prev / ▶ play or ❚❚ pause / ▶| skip); the play/pause icon follows the
current status. **While a button is held**, its icon swaps to the hold action:
a **yellow ★** (Top) on UP, a **red flag** (Untag) on DOWN, and a **gear**
(settings) on SELECT. When a track is marked top, a **★ TOP** badge is shown
(mirrors the ⭐ marker in the Rich TUI).

**Settings menu** (`MenuLayer`, fully theme-coloured — the system ActionMenu
ignores its body colour on emery): Save to genre (a pushed sub-screen listing
your genres) · Unsave · Loop on/off · Shuffle · **Display** (tap to cycle
Auto → Day → Night in place) · Skip (remove) · Mark Top · Mark Untagged.

**Display mode** (persisted): **Night** is the dark theme (bright accents on
black), **Day** is a light theme (white background, darkened accent variants so
they stay legible), and **Auto** follows the local clock — day 06:00–18:00,
night otherwise (and flips live while the app is open). Defaults to **Night**.

## AppMessage protocol

- **Phone → watch:** `STATUS` (0 idle/1 play/2 pause/3 buffering), `TITLE`,
  `CHANNEL`, `GENRE`, `GENRE_COLOR` (0 none/1 cyan/2 yellow/3 magenta/4 green/5 red),
  `IS_TOP`, `IS_UNTAG`, `POSITION`, `DURATION`, `QUEUE_SIZE`, `LOOP`, `REACHABLE`,
  `GENRES` (`"Name:code|Name:code"`).
- **Watch → phone:** `CMD` (`playpause|skip|next|prev|loop|shuffle|top|untag|unsave|genre`),
  `ARG` (genre index for `CMD=genre`), `REFRESH`.

## Notes
- Colours degrade to white-on-black on B&W platforms (`flint` = Core 2 Duo,
  `diorite`, `aplite`) via `PBL_IF_COLOR_ELSE`; full colour on `emery` (Core Time 2),
  `basalt`, `chalk`, `gabbro`.
- The bridge only runs while `discovery_queue.py` is running; otherwise the watch
  shows **OFFLINE — start queue**.
- The **watch launcher** icon is `resources/images/menu_icon.png` (25×25, black
  headphones with a star, transparent background). It shows on the light unselected
  launcher rows and goes faint on the dark highlighted row (single-colour trade-off).
  On CloudPebble paste builds, add it as a **Bitmap** resource named `IMAGE_MENU_ICON`
  marked as the menu icon — see [CLOUDPEBBLE.md](CLOUDPEBBLE.md).
- The phone app's **My Apps** image is *not* this icon — it comes from the app's
  Rebble appstore listing (icon + screenshot/marketing GIFs). A sideloaded personal
  build has no listing, so it's blank there until you publish one (CloudPebble
  **Publish** tab); it can be published private/unlisted.
