/**
 * Instrumentalls remote — PebbleKit JS bridge.
 *
 * Runs inside the phone's Pebble/Cobble app and talks to one of two local HTTP
 * bridges, mirroring the state to the watch over AppMessage and relaying the
 * watch's button/menu commands back:
 *
 *   Discovery  RemoteControlServer inside a live `discovery_queue.py` (:8090)
 *   Musicolet  musicolet_remote.py (:8091) — Musicolet via mission-control's
 *              Shizuku media API, plus collection flags, album art and actions
 *
 * Source (chosen on the watch: Settings > Source, kept here in localStorage):
 *   0 Auto      the discovery queue while it is running, else Musicolet
 *   1 Discovery
 *   2 Musicolet
 *
 * Volume (the watch's volume screen) is the phone's media volume in both modes,
 * always through the Musicolet bridge's /api/volume (termux-volume).
 *
 * Termux and the Pebble app run on the same phone, so the bridges are reached
 * over shared Android loopback. No config page (Clay) — the URLs are fixed below.
 */

// ---- edit these if your config.ini differs ----
var BACKEND_URL = 'http://127.0.0.1:8090';    // [remote_control] host/port
var MUSICOLET_URL = 'http://127.0.0.1:8091';  // [musicolet_remote] host/port
var AUTH_TOKEN = '';                           // set only if a bridge token is set
// -----------------------------------------------

var POLL_MS = 2000;
var ART_CHUNK = 1000;   // bytes per AppMessage; the watch inbox is 1536
var pollTimer = null;

var MODE_DISCOVERY = 0;
var MODE_MUSICOLET = 1;
var currentMode = null;
var lastArtId;          // undefined = nothing sent yet; null = "no cover" sent

// Mirror of the Rich TUI palette + the C app's colour codes.
var GENRE_COLORS = {
  'Futuristic': 'cyan',
  'Guitar': 'yellow',
  'Trap': 'magenta',
  'W/Hook': 'green',
  'Hook': 'green'
};
var COLOR_CODE = { white: 0, cyan: 1, yellow: 2, magenta: 3, green: 4, red: 5 };

function colorCode(name) { return COLOR_CODE[name] || 0; }
function genreColorCode(genre) { return colorCode(GENRE_COLORS[genre] || 'white'); }

// Album-art edge per platform — must match ART_TALL / ART_SHORT / ART_ROUND_SHORT in main.c;
// `big` (the *_BIG ones) when there's no progress bar (status "unknown").
// aplite has no art (too little RAM to decode it); B&W watches get a 1-bit dither.
var ART_SPEC = {
  emery: { size: 90, big: 120 }, gabbro: { size: 90, big: 120 },
  basalt: { size: 56, big: 84 }, chalk: { size: 48, big: 72 },
  diorite: { size: 56, big: 84, bw: true }, flint: { size: 56, big: 84, bw: true }
};

function artSpec() {
  try {
    var info = Pebble.getActiveWatchInfo && Pebble.getActiveWatchInfo();
    return (info && ART_SPEC[info.platform]) || null;
  } catch (e) { return null; }
}

function stored(key, fallback) {
  try {
    var v = localStorage.getItem(key);
    return (v === null || v === undefined) ? fallback : v;
  } catch (e) { return fallback; }
}

function cleanUrl(url) { return String(url).trim().replace(/\/+$/, ''); }
function discoveryUrl() { return cleanUrl(stored('backendUrl', BACKEND_URL)); }
function musicoletUrl() { return cleanUrl(stored('musicoletUrl', MUSICOLET_URL)); }
function authToken() { return String(stored('authToken', AUTH_TOKEN)).trim(); }
function getSource() {
  var s = parseInt(stored('source', '0'), 10);
  return (s >= 0 && s <= 2) ? s : 0;
}

// -- HTTP -------------------------------------------------------------------
function xhr(base, method, path, body, cb) {
  var req = new XMLHttpRequest();
  try { req.open(method, base + path); }
  catch (e) { cb(null); return; }
  req.timeout = 4000;
  req.setRequestHeader('Content-Type', 'application/json');
  var tok = authToken();
  if (tok) { req.setRequestHeader('X-Auth-Token', tok); }
  req.onload = function () {
    var data = null;
    try { data = JSON.parse(req.responseText); } catch (e) { data = null; }
    if (req.status >= 200 && req.status < 300) { cb(data); }
    else { cb(null, data); }
  };
  req.onerror = function () { cb(null); };
  req.ontimeout = function () { cb(null); };
  try { req.send(body ? JSON.stringify(body) : null); }
  catch (e) { cb(null); }
}

// -- watch messaging: one message in flight, retried, state coalesced --------
// AppMessages must be sent one at a time (the next waits for the previous ack),
// or they fail as busy. State updates are coalesced (`coalesce`): a newer one
// replaces a queued older one, so a slow link never builds a backlog. Art
// chunks share the 'art' tag only so a stale transfer can be dropped as a whole.
var outbox = [];
var sending = false;

function enqueue(dict, tag, coalesce) {
  if (tag && coalesce) {
    for (var i = sending ? 1 : 0; i < outbox.length; i++) {
      if (outbox[i].tag === tag) { outbox[i].dict = dict; return; }
    }
  }
  outbox.push({ dict: dict, tag: tag, tries: 0 });
  pump();
}

function dropQueued(tag) {
  outbox = outbox.filter(function (item, i) { return (sending && i === 0) || item.tag !== tag; });
}

function pump() {
  if (sending || !outbox.length) { return; }
  sending = true;
  var item = outbox[0];
  Pebble.sendAppMessage(item.dict, function () {
    outbox.shift();
    sending = false;
    pump();
  }, function () {
    item.tries++;
    if (item.tries >= 3) {
      outbox.shift();
      if (item.tag === 'art') { lastArtId = undefined; dropQueued('art'); }  // retry next poll
    }
    sending = false;
    setTimeout(pump, 200);
  });
}

function sendState(dict) {
  dict.SOURCE = getSource();
  enqueue(dict, 'state', true);
}

// -- album art ----------------------------------------------------------------
var B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';

function b64ToBytes(s) {
  var out = [], buf = 0, bits = 0;
  for (var i = 0; i < s.length; i++) {
    var v = B64.indexOf(s.charAt(i));
    if (v < 0) { continue; }   // '=' padding / whitespace
    buf = (buf << 6) | v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push((buf >> bits) & 0xff);
    }
  }
  return out;
}

function syncArt(artId, big) {
  var spec = artSpec();
  if (!spec) { return; }
  var size = big ? spec.big : spec.size;
  var key = artId ? artId + '@' + size : null;   // re-send when the size flips too
  if (key === lastArtId) { return; }
  lastArtId = key;
  dropQueued('art');
  if (!artId) { enqueue({ ART_LEN: 0 }, 'art'); return; }
  var q = '/api/art?id=' + encodeURIComponent(artId) + '&size=' + size + (spec.bw ? '&bw=1' : '');
  xhr(musicoletUrl(), 'GET', q, null, function (data) {
    if (lastArtId !== key) { return; }                   // the song moved on meanwhile
    if (!data || !data.png_b64) { lastArtId = undefined; return; }   // retry next poll
    var bytes = b64ToBytes(data.png_b64);
    enqueue({ ART_LEN: bytes.length }, 'art');
    for (var off = 0; off < bytes.length; off += ART_CHUNK) {
      enqueue({ ART_OFFSET: off, ART_DATA: bytes.slice(off, off + ART_CHUNK) }, 'art');
    }
  });
}

// -- polling ------------------------------------------------------------------
function setMode(mode) {
  if (mode === currentMode) { return; }
  currentMode = mode;
  lastArtId = undefined;   // re-send the cover when (re)entering Musicolet
  fetchGenres();
}

function fetchGenres() {
  var base = currentMode === MODE_MUSICOLET ? musicoletUrl() : discoveryUrl();
  xhr(base, 'GET', '/api/genres', null, function (data) {
    if (!data || !data.genres) { return; }
    var parts = data.genres.map(function (g) {
      return g.genre + ':' + colorCode(g.color);
    });
    enqueue({ GENRES: parts.join('|') }, 'genres', true);
  });
}

function pollDiscovery(onUnreachable) {
  xhr(discoveryUrl(), 'GET', '/api/now-playing', null, function (data) {
    if (!data) {
      if (onUnreachable) { onUnreachable(); return; }
      setMode(MODE_DISCOVERY);
      sendState({ MODE: MODE_DISCOVERY, REACHABLE: 0, STATUS: 0 });
      return;
    }
    setMode(MODE_DISCOVERY);
    var t = data.track;
    var status;
    if (!t) { status = 0; }
    else if (data.buffering) { status = 3; }
    else if (data.paused) { status = 2; }
    else if (data.playing) { status = 1; }
    else { status = 0; }

    sendState({
      MODE: MODE_DISCOVERY,
      REACHABLE: 1,
      STATUS: status,
      LOOP: data.loop_enabled ? 1 : 0,
      QUEUE_SIZE: data.queue_size || 0,
      TITLE: t ? String(t.title || '') : '',
      CHANNEL: t ? String(t.channel || '') : '',
      GENRE: (t && t.saved_to_genre) ? String(t.saved_to_genre) : '',
      GENRE_COLOR: (t && t.saved_to_genre) ? genreColorCode(t.saved_to_genre) : 0,
      IS_TOP: (t && t.is_top) ? 1 : 0,
      IS_UNTAG: (t && t.needs_untagging) ? 1 : 0,
      POSITION: (t && data.position) ? Math.round(data.position) : 0,
      DURATION: t ? (t.duration_seconds || 0) : 0,
      ERROR: ''
    });
  });
}

// unknown: Musicolet read from its notification (Shizuku down) — no state/position
var MUSICOLET_STATUS = { playing: 1, paused: 2, buffering: 3, unknown: 4 };

function musicoletError(d) {
  if (!d.reachable) { return 'Mission Control down'; }
  if (d.status === 'none') {
    var err = String(d.error || '');
    if (/shizuku|rish/i.test(err)) { return 'Shizuku off'; }
    if (/no media session/i.test(err)) { return 'Not running'; }
    return 'Nothing playing';
  }
  return '';
}

function pollMusicolet() {
  xhr(musicoletUrl(), 'GET', '/api/now-playing', null, function (d) {
    setMode(MODE_MUSICOLET);
    if (!d) { sendState({ MODE: MODE_MUSICOLET, REACHABLE: 0, STATUS: 0 }); return; }
    sendState({
      MODE: MODE_MUSICOLET,
      REACHABLE: 1,
      STATUS: MUSICOLET_STATUS[d.status] || 0,
      ERROR: musicoletError(d),
      TITLE: String(d.title || ''),
      CHANNEL: String(d.artist || ''),
      GENRE: String(d.genre || ''),
      GENRE_COLOR: genreColorCode(d.genre),
      MATCHED: d.matched ? 1 : 0,
      IS_TOP: d.is_top ? 1 : 0,
      TOP_PENDING: d.top_pending ? 1 : 0,
      IS_UNTAG: d.is_staged ? 1 : 0,
      POSITION: d.position || 0,
      DURATION: d.duration || 0
    });
    if (d.status !== 'none') { syncArt(d.art_id, d.status === 'unknown'); }
  });
}

function poll() {
  var source = getSource();
  if (source === 1) { pollDiscovery(); }
  else if (source === 2) { pollMusicolet(); }
  else { pollDiscovery(pollMusicolet); }   // Auto: the queue wins while it runs
}

function startPolling() {
  if (pollTimer) { clearInterval(pollTimer); }
  poll();
  pollTimer = setInterval(poll, POLL_MS);
}

// -- commands from the watch ----------------------------------------------------
// Discovery: watch CMD tokens -> bridge control commands.
var CONTROL = {
  playpause: 'x', skip: 'k', next: '>', prev: '<', loop: 'o', shuffle: 'f'
};

function handleDiscoveryCommand(cmd, arg) {
  var base = discoveryUrl();
  if (CONTROL.hasOwnProperty(cmd)) {
    xhr(base, 'POST', '/api/control', { command: CONTROL[cmd] }, function () { poll(); });
  } else if (cmd === 'top' || cmd === 'untag' || cmd === 'unsave') {
    xhr(base, 'POST', '/api/action', { type: cmd }, function () { poll(); });
  } else if (cmd === 'genre' && arg >= 1) {
    xhr(base, 'POST', '/api/action', { type: 'genre', genre_index: arg }, function () { poll(); });
  }
}

function handleMusicoletCommand(cmd, arg) {
  var base = musicoletUrl();
  var afterAction = function (data, errData) {
    var r = data || errData;
    if (r && r.needs_genre) { enqueue({ PROMPT_GENRE: 1 }); }
    poll();
  };
  if (cmd === 'playpause' || cmd === 'prev' || cmd === 'next') {
    xhr(base, 'POST', '/api/control', { command: cmd }, function () { poll(); });
  } else if (cmd === 'top' || cmd === 'untop' || cmd === 'stage') {
    xhr(base, 'POST', '/api/action', { type: cmd }, afterAction);
  } else if (cmd === 'topgenre' && arg >= 1) {
    xhr(base, 'POST', '/api/action', { type: 'top', genre_index: arg }, afterAction);
  } else if (cmd === 'setgenre' && arg >= 1) {
    xhr(base, 'POST', '/api/action', { type: 'genre', genre_index: arg }, afterAction);
  }
}

// -- volume (both modes, via the Musicolet bridge) ---------------------------------
// termux-volume takes ~1.4s per call: one request in flight, and only the newest
// requested level is sent after it (the watch already shows that level).
var volBusy = false;
var volWanted = null;

function sendVolume(data) {
  enqueue(data ? { VOLUME: data.level, VOLUME_MAX: data.max } : { VOLUME: -1 }, 'volume', true);
}

function volumeGet() {
  xhr(musicoletUrl(), 'GET', '/api/volume', null, function (data) {
    sendVolume(data && typeof data.level === 'number' ? data : null);
  });
}

function volumeSet(level) {
  if (volBusy) { volWanted = level; return; }
  volBusy = true;
  xhr(musicoletUrl(), 'POST', '/api/volume', { level: level }, function (data) {
    volBusy = false;
    if (!data) { volWanted = null; sendVolume(null); return; }
    if (volWanted !== null) {
      var next = volWanted;
      volWanted = null;
      volumeSet(next);
    }
  });
}

function handleCommand(payload) {
  if (payload.REFRESH) { poll(); return; }
  var cmd = payload.CMD;
  if (!cmd) { return; }
  var arg = payload.ARG | 0;

  if (cmd === 'volget') { volumeGet(); return; }
  if (cmd === 'volset') { volumeSet(arg); return; }
  if (cmd === 'source') {
    try { localStorage.setItem('source', String(arg)); } catch (e) { /* ignore */ }
    poll();
  } else if (currentMode === MODE_MUSICOLET) {
    handleMusicoletCommand(cmd, arg);
  } else {
    handleDiscoveryCommand(cmd, arg);
  }
}

// -- lifecycle --------------------------------------------------------------
Pebble.addEventListener('ready', function () {
  startPolling();   // the first poll picks the mode, which fetches the genres
});

Pebble.addEventListener('appmessage', function (e) {
  handleCommand(e.payload || {});
});
