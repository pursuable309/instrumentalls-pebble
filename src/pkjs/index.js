/**
 * Instrumentalls remote — PebbleKit JS bridge.
 *
 * Runs inside the phone's Pebble/Cobble app. Polls the discovery-queue HTTP
 * bridge (RemoteControlServer, in the live `discovery_queue.py` process) and
 * mirrors now-playing state to the watch over AppMessage; relays the watch's
 * button/ActionMenu commands back to the bridge as HTTP POSTs.
 *
 * Termux and the Pebble app run on the same phone, so the backend is reached
 * over shared Android loopback. No config page (Clay) — the URL is fixed below;
 * edit BACKEND_URL / AUTH_TOKEN here if your config.ini differs.
 */

// ---- edit these two if your config.ini [remote_control] differs ----
var BACKEND_URL = 'http://127.0.0.1:8090';   // host/port from config.ini
var AUTH_TOKEN = '';                          // set only if [remote_control] token is set
// --------------------------------------------------------------------

var POLL_MS = 3000;
var pollTimer = null;

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

function backendUrl() {
  // Allow a runtime override via localStorage without a config page.
  var url = BACKEND_URL;
  try {
    var stored = localStorage.getItem('backendUrl');
    if (stored) { url = stored; }
  } catch (e) { /* ignore */ }
  return String(url).trim().replace(/\/+$/, '');
}

function authToken() {
  var tok = AUTH_TOKEN;
  try {
    var stored = localStorage.getItem('authToken');
    if (stored !== null) { tok = stored; }
  } catch (e) { /* ignore */ }
  return String(tok).trim();
}

// -- HTTP -------------------------------------------------------------------
function xhr(method, path, body, cb) {
  var req = new XMLHttpRequest();
  try { req.open(method, backendUrl() + path); }
  catch (e) { cb(null); return; }
  req.timeout = 4000;
  req.setRequestHeader('Content-Type', 'application/json');
  var tok = authToken();
  if (tok) { req.setRequestHeader('X-Auth-Token', tok); }
  req.onload = function () {
    if (req.status >= 200 && req.status < 300) {
      var data = null;
      try { data = JSON.parse(req.responseText); } catch (e) { data = null; }
      cb(data);
    } else {
      cb(null);
    }
  };
  req.onerror = function () { cb(null); };
  req.ontimeout = function () { cb(null); };
  try { req.send(body ? JSON.stringify(body) : null); }
  catch (e) { cb(null); }
}

// -- watch messaging --------------------------------------------------------
function sendToWatch(dict) {
  Pebble.sendAppMessage(dict, function () {}, function (e) {
    console.log('AppMessage send failed: ' + JSON.stringify(e));
  });
}

function fetchGenres() {
  xhr('GET', '/api/genres', null, function (data) {
    if (!data || !data.genres) { return; }
    var parts = data.genres.map(function (g) {
      return g.genre + ':' + colorCode(g.color);
    });
    sendToWatch({ GENRES: parts.join('|') });
  });
}

function poll() {
  xhr('GET', '/api/now-playing', null, function (data) {
    if (!data) { sendToWatch({ REACHABLE: 0, STATUS: 0 }); return; }
    var t = data.track;
    var status;
    if (!t) { status = 0; }
    else if (data.buffering) { status = 3; }
    else if (data.paused) { status = 2; }
    else if (data.playing) { status = 1; }
    else { status = 0; }

    sendToWatch({
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
      DURATION: t ? (t.duration_seconds || 0) : 0
    });
  });
}

function startPolling() {
  if (pollTimer) { clearInterval(pollTimer); }
  poll();
  pollTimer = setInterval(poll, POLL_MS);
}

// Map watch CMD tokens -> bridge control commands.
var CONTROL = {
  playpause: 'x', skip: 'k', next: '>', prev: '<', loop: 'o', shuffle: 'f'
};

function handleCommand(payload) {
  if (payload.REFRESH) { poll(); return; }
  var cmd = payload.CMD;
  if (!cmd) { return; }

  if (CONTROL.hasOwnProperty(cmd)) {
    xhr('POST', '/api/control', { command: CONTROL[cmd] }, function () { poll(); });
  } else if (cmd === 'top' || cmd === 'untag' || cmd === 'unsave') {
    xhr('POST', '/api/action', { type: cmd }, function () { poll(); });
  } else if (cmd === 'genre') {
    var idx = payload.ARG | 0;
    if (idx >= 1) {
      xhr('POST', '/api/action', { type: 'genre', genre_index: idx }, function () { poll(); });
    }
  }
}

// -- lifecycle --------------------------------------------------------------
Pebble.addEventListener('ready', function () {
  fetchGenres();
  startPolling();
});

Pebble.addEventListener('appmessage', function (e) {
  handleCommand(e.payload || {});
});
