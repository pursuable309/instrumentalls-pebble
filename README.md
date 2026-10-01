# Instrumentalls — Pebble remote

A Pebble companion that remote-controls a running `discovery_queue.py` session —
or **Musicolet** (see [Musicolet mode](#musicolet-mode)):
a colour-coded now-playing screen (mirroring the Rich TUI palette) plus a
themeable `MenuLayer` settings/tag menu. Your **Core Time 2 is the `emery`
platform**; the code is colour-safe (`PBL_IF_COLOR_ELSE`) so it also builds for
every other platform (`basalt`, `chalk`, `diorite`, `emery`, `flint`, `gabbro`,
`aplite`), degrading to white-on-black on the B&W ones.

> The now-playing screen is laid out like the stock Pebble Music app (header,
> title, genre pill, progress bar, action bar), colour-coded like the terminal.
> Discovery mode has no album art; Musicolet mode adds the song's cover.

**Now-playing screen** (rect; round centres everything and drops the strip):

- **Header** — large wall clock on the left, ↻ when loop is on, ≡ queue count on the right.
- **Title** (up to 3 lines on emery, 2 on short screens) and **channel**. When
  the bridge is offline these read *Offline / Start the queue*; while buffering
  the channel slot reads *Buffering…*.
- **Genre pill** — filled in the genre colour when saved, an outline
  *Not saved* pill otherwise; followed by ★ (Top) / ⚑ (Untag) when set.
- **Progress bar** — rounded, coloured by status (green playing, yellow
  paused/buffering, red offline) with a playhead knob; elapsed on the left,
  `-remaining` on the right.
- **Action bar** — a dark right-edge strip with the button icons. There is no
  status word: the play/pause icon + bar colour carry it.

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
| BACK | volume screen (see below) |
| BACK (hold) | exit (the system shortcut; the tap fires on release, so a hold goes straight out) |

The action bar shows each button's tap action (◀◀ prev / ▶ play or ❚❚ pause
/ 🚫 skip-remove, or ▶| when the track is saved). **While a button is held**, its icon swaps to the hold action:
a **yellow ★** (Top) on UP, a **red flag** (Untag) on DOWN, and a **gear**
(settings) on SELECT. When a track is marked top, a **★** follows the genre pill
(mirrors the ⭐ marker in the Rich TUI).

**Volume screen** (BACK, in both modes): UP / DOWN step the phone's media volume
(held = repeat), SELECT or BACK returns to the player. It's Android's music stream,
so it covers Musicolet and the queue's mpv alike, set through the Musicolet bridge's
`/api/volume` (`termux-volume`; `[musicolet_remote] volume_stream`), which
therefore must be running in discovery mode too. The watch shows each step at once
and sends only the latest level after a short pause (a `termux-volume` call takes
~1.4 s). *Unavailable* = the bridge couldn't be reached.

**Settings menu** (`MenuLayer`, fully theme-coloured — the system ActionMenu
ignores its body colour on emery): Save to genre (a pushed sub-screen listing
your genres) · Unsave · Loop on/off · Shuffle · **Display** (tap to cycle
Auto → Day → Night in place) · Skip (remove) · Mark Top · Mark Untagged.

**Display mode** (persisted): **Night** is the dark theme (bright accents on
black), **Day** is a light theme (white background, darkened accent variants so
they stay legible), and **Auto** follows the local clock — day 06:00–18:00,
night otherwise (and flips live while the app is open). Defaults to **Night**.

## Musicolet mode

The same app also remote-controls **Musicolet** (full playback), backed by the
always-on `musicolet_remote.py` bridge (`http://127.0.0.1:8091`, auto-started by
mission-control as "Instrumentalls"), which reads Musicolet from its notification
and controls it through MacroDroid (optionally through mission-control's Shizuku
media API).

**Sources** (`[musicolet_remote] primary`). The default, `notification`, avoids
Shizuku, which dies with Wireless debugging (Wi-Fi off, a reboot):
- The song comes from Musicolet's notification (`termux-notification-list`;
  Termux:API needs notification access). Art, flags and the actions all work.
- The notification has no play state or position. So there's **no progress bar**:
  the cover grows into its space (120px on emery/gabbro, 84px on 168px screens, 72px
  on chalk), and SELECT shows a ▶❚❚ toggle.
- Play/pause, next and previous go out as broadcasts
  `com.mission_control.musicolet.playpause` / `.next` / `.prev`
  (`fallback_intent_prefix`). Each is caught by a MacroDroid macro: *Intent received*
  trigger → *Control media session* (Musicolet).
- If Shizuku happens to be running and reports the same song, its play state and
  position are merged in, so the bar returns. Controls stay on MacroDroid.

`primary = shizuku` restores the old order: Shizuku first, the notification only
while Shizuku is down.

**Source** (settings menu, cycles in place): **Auto** = the discovery queue while
`discovery_queue.py` runs, else Musicolet · **Discovery** · **Musicolet**. The
choice lives in the phone JS (`localStorage`).

**Screen:** album art centred at the top (90px on emery/gabbro, 56px on 168px
screens, 48px on chalk), then a one-line title, the artist (tall screens) and the
chip row — genre pill, ★, ⚑ — as in discovery mode, then progress bar and time.
B&W watches get a 1-bit cover; aplite shows a placeholder (too little RAM to
decode PNGs).

- **★** the song is in a `Top_<Genre>` folder; **☆** (hollow) a move into or out of
  Top is queued. Leaving Top puts the file back in the collection root; the album-art
  audit then swaps its Top cover for the standard one. With `[musicolet_remote] top_move = immediate` (the default) the file
  moves right away, even mid-song; `next_song` queues it until a *different* song
  is playing.
- **⚑** a copy sits in `staging_in/` (staged for untagging).
- Pill: the song's genre; outline *No genre* / *Not in library* otherwise.

| Input | Musicolet action |
|-------|------------------|
| UP / DOWN | previous / next song |
| SELECT | play / pause |
| UP (long) | Top toggle: Move to Top, or take a Top song back out to the collection root (cancels a queued move) |
| DOWN (long) | stage for untagging (copy to `staging_in/` + staging album tag) |
| SELECT (long) | menu: Change genre · Move to / Remove from Top · Top as genre… · Stage untag · Source · Display |
| BACK | volume screen (as in discovery mode) |

A song with no genre asks which Top folder to use (genre picker).

**Change genre** rewrites the file's genre tag (`Instrumental (<Genre>)`, as the
downloader writes it). A song in a Top folder also moves to the new
genre's Top folder (immediately, or on the next song — see `top_move`) — otherwise
the Top-folder genre audit would
set the tag back to match the old folder. Album, art and the W/Hook title suffix
are left to the collection audits.

## AppMessage protocol

- **Phone → watch:** `STATUS` (0 idle/1 play/2 pause/3 buffering/4 unknown — Musicolet
  from its notification: no bar, bigger art, ▶❚❚), `TITLE`,
  `CHANNEL` (artist in Musicolet mode), `GENRE`, `GENRE_COLOR` (0 none/1 cyan/2
  yellow/3 magenta/4 green/5 red), `IS_TOP`, `IS_UNTAG` (staged, in Musicolet mode),
  `POSITION`, `DURATION`, `QUEUE_SIZE`, `LOOP`, `REACHABLE`, `GENRES`
  (`"Name:code|Name:code"`), `MODE` (0 discovery/1 Musicolet), `SOURCE` (0 auto/1
  discovery/2 Musicolet), `MATCHED`, `TOP_PENDING`, `ERROR` (Musicolet status text),
  `PROMPT_GENRE` (open the Top-genre picker).
- **Album art (Musicolet):** `ART_LEN` (PNG bytes; 0 = no cover), then
  `ART_OFFSET` + `ART_DATA` chunks (≤1000 bytes) in order; the watch decodes the PNG
  once complete. Re-sent only when the cover (`art_id`) changes. The phone JS sends
  one AppMessage at a time (acked, retried) and coalesces state updates.
- **Watch → phone:** `CMD` (`playpause|skip|next|prev|loop|shuffle|top|untag|unsave|genre`;
  Musicolet: `playpause|prev|next|top|untop|stage|topgenre|setgenre`; both: `source`),
  `ARG` (genre index for `genre`/`topgenre`/`setgenre`, source for `source`), `REFRESH`.

## Notes
- Colours degrade to white-on-black on B&W platforms (`flint` = Core 2 Duo,
  `diorite`, `aplite`) via `PBL_IF_COLOR_ELSE`; full colour on `emery` (Core Time 2),
  `basalt`, `chalk`, `gabbro`.
- The discovery bridge only runs while `discovery_queue.py` is running; with
  Source = Discovery the watch shows **Offline — Start the queue** otherwise (Auto
  falls back to Musicolet).
- The **watch launcher** icon is `resources/images/menu_icon.png` (25×25, black
  headphones with a star, transparent background). It shows on the light unselected
  launcher rows and goes faint on the dark highlighted row (single-colour trade-off).
  On CloudPebble paste builds, add it as a **Bitmap** resource named `IMAGE_MENU_ICON`
  marked as the menu icon — see [CLOUDPEBBLE.md](CLOUDPEBBLE.md).
- The phone app's **My Apps** image is *not* this icon — it comes from the app's
  Rebble appstore listing (icon + screenshot/marketing GIFs). A sideloaded personal
  build has no listing, so it's blank there until you publish one (CloudPebble
  **Publish** tab); it can be published private/unlisted.
