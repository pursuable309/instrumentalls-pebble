# Building this app on CloudPebble (no local toolchain)

A Pebble app compiles to ARM firmware, which needs the Pebble build toolchain.
Rather than install that in Termux (aarch64 — no python2 / no ARM cross-compiler),
build in the cloud with **CloudPebble** and install to the watch over the phone's
Pebble/Cobble app. Everything below is doable from the phone.

CloudPebble: <https://cloudpebble.repebble.com> (sign in with your Rebble account).
This app's project: <https://cloudpebble.repebble.com/ide/project/30205>.

The cloud build only produces the `.pbw`; the app still **runs on the watch/phone**,
where it reaches the Termux bridge at `127.0.0.1:8090` — cloud-building changes
nothing about connectivity.

> **No Clay / no config page.** Clay (`pebble-clay`) is a JS library that
> auto-generates a watch app's **settings screen** — the form you'd reach from
> the gear next to the app in the phone's Pebble app. You declare the settings as
> JSON and Clay builds the HTML page, persists the values, and ships them to the
> app over AppMessage. We don't use it: `pebble-clay` does **not** yet support the
> new `gabbro` (Pebble Round 2) / `flint` (Pebble 2 Duo) platforms and breaks the
> build. So there's no phone-side config page — the backend URL is hardcoded near
> the top of `src/pkjs/index.js` (`BACKEND_URL` / `AUTH_TOKEN`; edit those two
> lines if your `config.ini` `[remote_control]` host/port/token differ), and any
> user-facing settings live **in-app** in the long-press ActionMenu instead.
>
> **The JS source is a normal `src/pkjs/index.js`**, imported straight from
> GitHub (Route A) — no paste, no rename step. Only Route B pastes it into
> CloudPebble's `index.js` field.

The C code is colour-safe (`PBL_IF_COLOR_ELSE`), so it compiles on every platform;
your **Core Time 2 is the `emery` platform** and renders in full colour.

---

## Route A — Import from GitHub (recommended; no clipboard)

CloudPebble's importer needs `package.json` / `src/` / `wscript` at the **repo
root**, but here they live under `pebble/`. `deploy-to-github.sh` publishes the
committed `pebble/` tree as a one-commit mirror repo (via `git commit-tree` — no
`git subtree` needed), so you never paste source into the mobile editor. This is
the fix for **Android's ~500-line clipboard limit**, which silently truncates a
pasted `main.c` (>500 lines) and breaks the build with
`expected declaration or statement at end of input`.

The main `instrumentalls` repo stays the single source of truth; the mirror is
always overwritten from `HEAD`, so never edit the mirror directly. The JS
bridge is a normal `src/pkjs/index.js`, published verbatim, so it's picked up
by the build automatically.

**One-time setup** (public repo is fine — no secrets: `AUTH_TOKEN` is empty,
`BACKEND_URL` is localhost):

1. Create an **empty** GitHub repo, e.g. `instrumentalls-pebble`.
2. From the project root, add it as a remote:
   ```bash
   git remote add pebble-deploy https://github.com/<you>/instrumentalls-pebble.git
   ```

**Each update** (the CloudPebble project is already linked to the mirror repo and
**builds automatically on push** — no manual "Run build"):

1. Commit your `pebble/` changes in the main repo (`git add pebble/ && git commit`).
2. `bash pebble/deploy-to-github.sh` — pushes the mirror. Claude can run this from
   the proot session now that the GitHub credential is wired up (see
   **Credentials** below); no need to run it on the host.
3. CloudPebble auto-builds the pushed commit. Open the project
   (<https://cloudpebble.repebble.com/ide/project/30205>) and **download the
   `.pbw`** once the build finishes.
4. Install the `.pbw` to the watch (open it in the phone's Pebble/Cobble app), with
   `python discovery_queue.py` running and `[remote_control] enabled = true`.

> **First-time / re-link only:** if the project isn't linked to the mirror yet, do
> CloudPebble **Import → Import from GitHub** with the mirror URL once; after that
> the auto-build-on-push above applies.

### Credentials (why the push works from Claude's session)

The push needs the GitHub credential for the `pebble-deploy` remote. Git's `store`
helper reads `~/.git-credentials`, which in the proot session is `/root/.git-credentials`.
That path is symlinked to the real credential file in the Termux home
(`/data/data/com.termux/files/home/.git-credentials`), so `deploy-to-github.sh` can
authenticate from inside proot. If the push ever fails with
`could not read Username for 'https://github.com'`, that symlink is missing —
recreate it (from the host/`!` prompt):
```bash
ln -sf /data/data/com.termux/files/home/.git-credentials /root/.git-credentials
```

## Route B — New project + paste (always works)

1. **Create → New project**, type **Pebble C SDK**, SDK **3**, watchapp (not a face).
2. **C file:** replace the default `main.c` with `src/c/main.c` from this folder.
3. **JS file:** create `index.js` = `src/pkjs/index.js`. That's the only JS file.
4. **Settings → PebbleKit JS Message Keys** — add these, assignment **Automatic**:
   ```
   STATUS TITLE CHANNEL GENRE GENRE_COLOR IS_TOP IS_UNTAG
   POSITION DURATION QUEUE_SIZE REACHABLE LOOP GENRES CMD ARG REFRESH
   ```
5. **Settings → Dependencies:** leave empty (do **not** add `pebble-clay`).
6. **Menu icon** (see below) — upload `resources/images/menu_icon.png`.
7. Build and **Install and Run** as in Route A.

## Already created the project with Clay? Fix an existing project

1. Replace `main.c` with the current `src/c/main.c` (colour-safe).
2. Replace `index.js` with the current `src/pkjs/index.js` (no `require('pebble-clay')`).
3. **Delete** the `config.js` / `config.json` file from the project.
4. **Settings → Dependencies:** remove `pebble-clay`.
5. (Optional) **Settings:** uncheck "Configurable".
6. **Menu icon** (see below) — upload `resources/images/menu_icon.png`.
7. Rebuild — it now compiles for all platforms (gabbro/flint included).

## App menu icon

`menu_icon.png` (25×25, black headphones with a star, transparent background) is the
**watch launcher** icon only. The launcher draws its literal pixels (no auto-tint), so
the black glyph shows on the light unselected rows and goes faint on the dark
**highlighted** row (a white glyph has the opposite problem — that's the single-colour
trade-off).

**This is NOT the phone "My Apps" image.** The phone app's app list shows the app's
**Rebble appstore listing** assets (a small icon plus screenshot / marketing images,
which is why some apps animate as GIFs). Those live on the appstore, not in the
`.pbw`, so a sideloaded personal build has **no listing → blank** in My Apps. To give
it an image you have to publish a listing (CloudPebble **Publish** tab / the Rebble
developer portal) and upload an icon (+ optional screenshots); you can publish it
private/unlisted. The `menu_icon` resource here does not affect that.

With a GitHub import (Route A) it's picked up automatically from `package.json`. For a
pasted project (Route B / fix-existing), add it once:

1. **Resources → Add new**, upload `resources/images/menu_icon.png`.
2. Resource kind **Bitmap**, identifier **`IMAGE_MENU_ICON`**.
3. Tick **"This is the app's menu icon"** and save, then rebuild.

> **Must be a single resource.** Use the **Bitmap** kind (not a "PNG – black/white
> transparent" / `png-trans` kind). The 2-tone kind splits the image into
> `IMAGE_MENU_ICON_WHITE` + `IMAGE_MENU_ICON_BLACK`, and the build then fails with
> `'RESOURCE_ID_IMAGE_MENU_ICON' undeclared … did you mean …_BLACK?` — because the
> menu-icon slot references the single, unsuffixed name. If you hit that, change the
> resource kind to **Bitmap** (or plain **PNG**) and rebuild. As a last resort you
> can just delete the resource — the app builds fine with Pebble's default icon.

---

## After install — set the backend (only if it differs from the default)

Edit the top of `src/pkjs/index.js`:
```js
var BACKEND_URL = 'http://127.0.0.1:8090';   // host/port from config.ini
var AUTH_TOKEN = '';                          // only if [remote_control] token is set
```

## Sanity check before building
With the queue running in Termux:
```bash
curl http://127.0.0.1:8090/api/now-playing
curl http://127.0.0.1:8090/api/genres
```
If those return JSON, the watch app will too.
