/*
 * Instrumentalls remote — Pebble watch app.
 *
 * A music-app style now-playing screen, colour-coded like the discovery-queue
 * Rich TUI (title cyan, channel magenta, genre colour-coded, TOP yellow, UNTAG
 * red), plus a themeable MenuLayer for the tag/settings actions. All state
 * arrives over AppMessage from the PebbleKit JS bridge; buttons and menu
 * selections are sent back as CMD tokens.
 *
 * Screen (rect):  header strip   clock · loop ↻ · ≡ queue count
 *                 title          2–3 lines, left-aligned
 *                 channel
 *                 chip row       genre pill (filled = saved, outline = not) ★ ⚑
 *                 progress bar   rounded, status-coloured, playhead knob
 *                 time row       elapsed ........ -remaining
 *                 action bar     right-edge strip with the button icons
 * There is no status word: play/pause state is the action-bar icon + the bar
 * colour (green playing / yellow paused|buffering / red offline); offline and
 * buffering are spelled out in the title/channel slots instead.
 *
 * Buttons:  UP   = previous       UP   (long) = mark Top (toggle)
 *           DOWN = skip (remove)  DOWN (long) = mark Untagged (toggle)
 *           (menu "Next song" advances WITHOUT removing — cf. DOWN skip)
 *           SELECT = play/pause   SELECT (long) = open settings menu
 *           BACK = volume screen (UP/DOWN = phone media volume, held repeats;
 *                  SELECT or BACK = back to the player). Holding BACK (the
 *                  system shortcut) quits the app.
 * While a button is held past the long-press threshold, its action-bar icon
 * swaps to the hold action: yellow star (Top), red flag (Untag), gear
 * (settings).
 *
 * Two sources, picked by the phone (Source: Auto / Discovery / Musicolet in the
 * settings menu; Auto = the discovery queue when it is running, else Musicolet):
 *   Discovery  the layout above.
 *   Musicolet  album art centred at the top, then a one-line title, the artist
 *              and the chip row as above (clock, title, artist and chips all centred) — genre pill + ★ (Top; hollow while the
 *              move is queued) + ⚑ (staged in staging_in) — then bar and time.
 *              UP/DOWN = previous/next song, menu "Change genre" retags the file,
 *              UP (long) = Top toggle (into / out of Top; cancels a queued move), DOWN (long) = stage for
 *              untagging. The art arrives as PNG chunks (ART_* keys) and is only
 *              re-sent when the cover changes.
 */

#include <pebble.h>

#define MAX_GENRES 8
#define GENRE_NAME_LEN 24

#define PERSIST_KEY_THEME 1   /* display mode: 0 auto, 1 day, 2 night */

/* Now-playing layout. The header sits at the top and the bar + time row are
 * pinned to the bottom; the title/channel/chip block is centred vertically in
 * the space between (the title is measured, so a one-line title doesn't leave
 * a hole). Round screens drop the filled action bar and centre everything. */
#define AB_W          PBL_IF_ROUND_ELSE(28, 30)  /* action-bar strip width      */
#define CONTENT_M     PBL_IF_ROUND_ELSE(22, 5)   /* content side margin         */
#define HEADER_Y      PBL_IF_ROUND_ELSE(6, 0)
#define HEADER_H_TALL 24    /* header strip height: tall screens (emery, gabbro) */
#define HEADER_H_SHORT 20   /* ...and 168/180px ones */
#define BOTTOM_M      PBL_IF_ROUND_ELSE(16, 6)
#define ROW_CHAN_H    22
#define ROW_CHIP_H    24
#define ROW_BAR_H     12    /* layer height: 6px track + room for the r=5 knob */
#define ROW_TIME_H    18
#define PILL_H        22
#define PILL_R         8
#define KNOB_R         5
#define ART_TALL      90    /* album art edge, Musicolet mode: emery/gabbro... */
#define ART_SHORT     56    /* ...168px rect screens */
#define ART_ROUND_SHORT 48  /* ...and chalk (180px round) */
/* No progress bar (song read from Musicolet's notification): the art takes the
 * freed bar + time rows. Must match ART_SPEC[...].big in index.js. */
#define ART_TALL_BIG      120
#define ART_SHORT_BIG      84
#define ART_ROUND_SHORT_BIG 72
#define ROW_TITLE1_H  28    /* one-line title (Gothic 24 bold) */

#define MODE_DISCOVERY 0
#define MODE_MUSICOLET 1

static Window *s_window;
static Layer *s_header_layer;     /* custom-drawn: clock, loop, queue count */
static TextLayer *s_title_layer;
static TextLayer *s_channel_layer;
static Layer *s_chip_layer;       /* custom-drawn: genre pill + ★ / ⚑ badges */
static Layer *s_progress_layer;
static TextLayer *s_time_layer;   /* elapsed (rect) or "pos / dur" (round) */
static TextLayer *s_remain_layer; /* -remaining, right-aligned (rect only) */
static Layer *s_buttons_layer;    /* right-edge action bar */
static Layer *s_art_layer;        /* album art (Musicolet mode only) */

static Window *s_settings_window;    /* settings list (MenuLayer) */
static MenuLayer *s_settings_menu;
static Window *s_genre_window;       /* "Save to genre" picker sub-screen */
static MenuLayer *s_genre_menu;
static Window *s_volume_window;      /* volume screen (BACK from now-playing) */
static Layer *s_volume_layer;
static AppTimer *s_tick_timer;

/* Which button is currently held past the long-press threshold, so the
 * action-bar icon can swap to that button's hold action:
 *   0 none   1 UP (Top)   2 SELECT (settings)   3 DOWN (Untag). */
static int s_hold = 0;

/* --- state (kept in sync by the AppMessage inbox) --- */
static char s_title[128] = "Instrumentalls";
static char s_channel[64] = "";
static char s_genre[GENRE_NAME_LEN] = "";
static char s_time_buf[32] = "";
static char s_remain_buf[12] = "";

static int s_status = 0;       /* 0 idle  1 playing  2 paused  3 buffering
                                  4 unknown (Musicolet without Shizuku: no state/position) */
static int s_reachable = 0;    /* 0 = bridge not reachable */
static int s_position = 0;     /* seconds */
static int s_duration = 0;     /* seconds */
static int s_queue_size = 0;
static int s_is_top = 0;
static int s_is_untag = 0;
static int s_loop = 0;
static int s_genre_color = 0;  /* colour code, see genre_gcolor() */

/* Source (set by the phone on every update). */
static int s_mode = MODE_DISCOVERY;
static int s_source = 0;        /* user choice: 0 auto  1 discovery  2 musicolet */
static int s_matched = 1;       /* Musicolet: playing song found in the collection */
static int s_top_pending = 0;   /* Musicolet: Move to Top queued (applied next song) */
static char s_error[48] = "";   /* Musicolet: why nothing can be shown */

/* What the genre picker does. */
enum {
  PICK_SAVE = 0,    /* discovery: save to genre */
  PICK_TOP,         /* Musicolet: Move to Top as that genre */
  PICK_GENRE,       /* Musicolet: change the file's genre tag */
};
static int s_picker = PICK_SAVE;

/* Album art: the PNG is assembled from ART_DATA chunks into s_art_buf, then
 * decoded once complete. s_art_bitmap is the cover on screen. */
static GBitmap *s_art_bitmap;
static uint8_t *s_art_buf;
static int s_art_len = 0;
static int s_art_recv = 0;

/* Phone media volume (volume screen). s_vol -1 = not known yet / unavailable.
 * Once a button is pressed the watch is the authority: presses change s_vol at
 * once and the latest value is sent after a short pause (VOL_SEND_MS), so a
 * held button doesn't flood the ~1.4s termux-volume round trip; replies from
 * the phone are then ignored until the screen is reopened. */
#define VOL_SEND_MS 250
static int s_vol = -1;
static int s_vol_max = 15;
static bool s_vol_local = false;
static bool s_vol_failed = false;
static AppTimer *s_vol_timer;

/* Display mode (persisted, set from the settings menu): 0 auto, 1 day, 2 night.
 * Auto resolves to day/night by local clock time. Defaults to night (2) on a
 * fresh install; a persisted choice overrides it. */
static int s_theme = 2;

static char s_genre_names[MAX_GENRES][GENRE_NAME_LEN];
static int s_genre_codes[MAX_GENRES];
static int s_genre_count = 0;

/* --- theme ---------------------------------------------------------------
 * Every colour below is chosen per theme so both modes read well: night keeps
 * the bright accents on black; day swaps to darkened variants on white (a
 * bright cyan/yellow/green would wash out on a light background). On B&W
 * platforms there are no accents — text is simply the contrast of the
 * background (white on black at night, black on white by day). The action bar
 * is dark in BOTH colour themes, so its icons always use the bright variants. */

/* Day = 06:00–18:00 local; Auto resolves to night outside that window. */
static bool theme_is_night(void) {
  if (s_theme == 1) return false;   /* forced day   */
  if (s_theme == 2) return true;    /* forced night */
  time_t now = time(NULL);
  struct tm *lt = localtime(&now);
  int hour = lt ? lt->tm_hour : 20; /* unknown clock → assume night */
  return (hour < 6 || hour >= 18);
}

static GColor col_bg(void)  { return theme_is_night() ? GColorBlack : GColorWhite; }
static GColor col_fg(void)  { return theme_is_night() ? GColorWhite : GColorBlack; }
static GColor col_muted(void) {   /* time/queue text, outline pill, separators */
#ifdef PBL_COLOR
  return theme_is_night() ? GColorLightGray : GColorDarkGray;
#else
  return col_fg();
#endif
}
/* Unfilled track / header rule: a quiet mid-tone against the background. */
static GColor col_track(void) {
  return PBL_IF_COLOR_ELSE(theme_is_night() ? GColorDarkGray : GColorLightGray, col_bg());
}

/* Fixed accents, day/night pairs. */
static GColor col_title(void)   { return PBL_IF_COLOR_ELSE(theme_is_night() ? GColorCyan    : GColorDukeBlue,       col_fg()); }
static GColor col_channel(void) { return PBL_IF_COLOR_ELSE(theme_is_night() ? GColorMagenta : GColorImperialPurple, col_fg()); }
static GColor col_top(void)     { return PBL_IF_COLOR_ELSE(theme_is_night() ? GColorYellow  : GColorWindsorTan,     col_fg()); }
static GColor col_untag(void)   { return PBL_IF_COLOR_ELSE(theme_is_night() ? GColorRed     : GColorDarkCandyAppleRed, col_fg()); }

/* --- colours (mirror the Rich palette / JS colour codes) --- */
static GColor genre_gcolor(int code) {
#ifdef PBL_COLOR
  bool night = theme_is_night();
  switch (code) {
    case 1: return night ? GColorCyan    : GColorDukeBlue;          /* Futuristic */
    case 2: return night ? GColorYellow  : GColorWindsorTan;        /* Guitar     */
    case 3: return night ? GColorMagenta : GColorImperialPurple;    /* Trap       */
    case 4: return night ? GColorGreen   : GColorDarkGreen;         /* W/Hook     */
    case 5: return night ? GColorRed     : GColorDarkCandyAppleRed;
    default: return col_fg();
  }
#else
  (void) code;   /* B&W platforms (flint, diorite, aplite): contrast text */
  return col_fg();
#endif
}

/* Status accent: green playing / yellow paused|buffering / red unreachable.
 * `night` picks the bright (dark-background) or dark (light-background) set. */
static GColor status_color_for(bool night) {
#ifdef PBL_COLOR
  if (!s_reachable) return night ? GColorRed : GColorDarkCandyAppleRed;
  switch (s_status) {
    case 1: return night ? GColorGreen  : GColorDarkGreen;    /* playing   */
    case 2: return night ? GColorYellow : GColorWindsorTan;   /* paused    */
    case 3: return night ? GColorYellow : GColorWindsorTan;   /* buffering */
    default: return night ? GColorLightGray : GColorDarkGray; /* idle      */
  }
#else
  (void) night;
  return col_fg();
#endif
}
static GColor status_color(void) { return status_color_for(theme_is_night()); }

/* Action bar: a dark strip in both colour themes (stock Pebble look), so its
 * icons are white / bright accents. On B&W it is the inverse of the background.
 * Round screens have no strip — icons sit on the background in theme colours. */
static GColor ab_bg(void) {
#ifdef PBL_ROUND
  return col_bg();
#else
  return PBL_IF_COLOR_ELSE(theme_is_night() ? GColorDarkGray : GColorBlack, col_fg());
#endif
}
static GColor ab_icon(void) {
#ifdef PBL_ROUND
  return col_title();
#else
  return PBL_IF_COLOR_ELSE(GColorWhite, col_bg());
#endif
}
static GColor ab_accent(GColor bright) {   /* a bright accent, if it reads here */
#ifdef PBL_ROUND
  (void) bright;
  return col_fg();
#else
  return PBL_IF_COLOR_ELSE(bright, col_bg());
#endif
}

static void format_mmss(int seconds, char *buf, int len) {
  if (seconds < 0) seconds = 0;
  snprintf(buf, len, "%d:%02d", seconds / 60, seconds % 60);
}

/* ------------------------------------------------------------------ */
/* Outbound commands                                                   */
/* ------------------------------------------------------------------ */
static void send_cmd_arg(const char *cmd, int arg) {
  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) != APP_MSG_OK) return;
  dict_write_cstring(iter, MESSAGE_KEY_CMD, cmd);
  if (arg >= 0) {
    dict_write_int(iter, MESSAGE_KEY_ARG, &arg, sizeof(int), true);
  }
  app_message_outbox_send();
}

static void send_cmd(const char *cmd) { send_cmd_arg(cmd, -1); }

static void request_refresh(void) {
  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) != APP_MSG_OK) return;
  int one = 1;
  dict_write_int(iter, MESSAGE_KEY_REFRESH, &one, sizeof(int), true);
  app_message_outbox_send();
}

/* ------------------------------------------------------------------ */
/* Small vector icons (drawn, so no bitmap resources are needed)       */
/* ------------------------------------------------------------------ */
/* Regular 5-point star: outer radius 10, inner 4 (≈ the 0.38 golden ratio),
 * points every 36° from straight up, rounded to the pixel grid and shifted
 * down 1px so its bounding box (y -9..9) is centred on the anchor. */
static const GPathInfo STAR_INFO = {
  .num_points = 10,
  .points = (GPoint[]) {
    {0,-9},{2,-2},{10,-2},{4,2},{6,9},{0,5},{-6,9},{-4,2},{-10,-2},{-2,-2}
  }
};

static void fill_tri(GContext *ctx, GPoint a, GPoint b, GPoint c) {
  GPoint pts[3] = { a, b, c };
  GPathInfo info = { .num_points = 3, .points = pts };
  GPath *p = gpath_create(&info);
  gpath_draw_filled(ctx, p);
  gpath_destroy(p);
}

/* triangle centred at (cx,cy) with half-size hs, pointing left / right */
static void tri_left(GContext *ctx, int cx, int cy, int hs) {
  fill_tri(ctx, GPoint(cx - hs, cy), GPoint(cx + hs, cy - hs), GPoint(cx + hs, cy + hs));
}
static void tri_right(GContext *ctx, int cx, int cy, int hs) {
  fill_tri(ctx, GPoint(cx + hs, cy), GPoint(cx - hs, cy - hs), GPoint(cx - hs, cy + hs));
}

/* 20px star (STAR_INFO spans ±10 × ±9) — Top badge and UP-hold icon. The
 * fill rasteriser skips some right/bottom edge pixels, which made the legs
 * look lopsided; a same-colour outline pass evens the edges out. */
static void draw_star(GContext *ctx, int cx, int cy, GColor color) {
  graphics_context_set_fill_color(ctx, color);
  graphics_context_set_stroke_color(ctx, color);
  GPath *p = gpath_create(&STAR_INFO);
  gpath_move_to(p, GPoint(cx, cy));
  gpath_draw_filled(ctx, p);
  gpath_draw_outline(ctx, p);
  gpath_destroy(p);
}

/* Previous (◀◀): two overlapping left triangles. */
static void draw_prev(GContext *ctx, int cx, int cy, GColor color) {
  graphics_context_set_fill_color(ctx, color);
  tri_left(ctx, cx - 3, cy, 6);
  tri_left(ctx, cx + 4, cy, 6);
}

/* Next (▶▶): mirror of previous — Musicolet mode's DOWN tap. */
static void draw_next(GContext *ctx, int cx, int cy, GColor color) {
  graphics_context_set_fill_color(ctx, color);
  tri_right(ctx, cx - 4, cy, 6);
  tri_right(ctx, cx + 3, cy, 6);
}

/* Hollow star: Move to Top is queued but not applied yet. */
static void draw_star_outline(GContext *ctx, int cx, int cy, GColor color) {
  graphics_context_set_stroke_color(ctx, color);
  graphics_context_set_stroke_width(ctx, 2);
  GPath *p = gpath_create(&STAR_INFO);
  gpath_move_to(p, GPoint(cx, cy));
  gpath_draw_outline(ctx, p);
  gpath_destroy(p);
  graphics_context_set_stroke_width(ctx, 1);
}

/* Play (▶) / pause (❚❚). */
static void draw_play(GContext *ctx, int cx, int cy, GColor color) {
  graphics_context_set_fill_color(ctx, color);
  tri_right(ctx, cx + 1, cy, 8);
}
static void draw_pause(GContext *ctx, int cx, int cy, GColor color) {
  graphics_context_set_fill_color(ctx, color);
  graphics_fill_rect(ctx, GRect(cx - 6, cy - 8, 4, 16), 1, GCornersAll);
  graphics_fill_rect(ctx, GRect(cx + 2, cy - 8, 4, 16), 1, GCornersAll);
}
/* Play/pause (▶❚❚): the toggle when the play state is unknown. */
static void draw_playpause(GContext *ctx, int cx, int cy, GColor color) {
  graphics_context_set_fill_color(ctx, color);
  tri_right(ctx, cx - 5, cy, 6);
  graphics_fill_rect(ctx, GRect(cx + 3, cy - 6, 3, 12), 0, GCornerNone);
  graphics_fill_rect(ctx, GRect(cx + 8, cy - 6, 3, 12), 0, GCornerNone);
}

/* Skip-forward icon (▶|): a right triangle butted against a vertical bar —
 * the DOWN-tap hint when the track is saved to a genre, since skipping it
 * only moves on (it stays in the download queue). */
static void draw_skip_fwd(GContext *ctx, int cx, int cy, GColor color) {
  graphics_context_set_fill_color(ctx, color);
  tri_right(ctx, cx - 3, cy, 7);
  graphics_fill_rect(ctx, GRect(cx + 4, cy - 7, 3, 15), 0, GCornerNone);
}

/* No-entry icon (🚫): a ring with a diagonal slash — the DOWN-tap hint for an
 * unsaved track, where "skip (remove)" really discards it (cf. the menu's
 * non-destructive "Next song"). */
static void draw_no_entry(GContext *ctx, int cx, int cy, GColor color) {
  graphics_context_set_stroke_color(ctx, color);
  graphics_context_set_stroke_width(ctx, 3);
  graphics_draw_circle(ctx, GPoint(cx, cy), 8);
  graphics_draw_line(ctx, GPoint(cx - 5, cy - 5), GPoint(cx + 5, cy + 5));
  graphics_context_set_stroke_width(ctx, 1);
}

/* Pennant flag: a vertical pole with a triangular flag near the top — the
 * Untag badge and DOWN-hold icon, mirrors the 🚩 marker in the Rich TUI.
 * `big` is the action-bar size; otherwise the 14px chip-row size. */
static void draw_flag(GContext *ctx, int cx, int cy, GColor color, bool big) {
  graphics_context_set_fill_color(ctx, color);
  if (big) {
    graphics_fill_rect(ctx, GRect(cx - 6, cy - 10, 3, 20), 0, GCornerNone);
    fill_tri(ctx, GPoint(cx - 3, cy - 10), GPoint(cx - 3, cy), GPoint(cx + 8, cy - 5));
  } else {
    graphics_fill_rect(ctx, GRect(cx - 4, cy - 7, 2, 14), 0, GCornerNone);
    fill_tri(ctx, GPoint(cx - 2, cy - 7), GPoint(cx - 2, cy - 1), GPoint(cx + 6, cy - 4));
  }
}

/* Gear: a filled hub with four teeth and a punched-out centre — the
 * SELECT-hold icon (open settings menu). `hole` is the colour behind it. */
static void draw_gear(GContext *ctx, int cx, int cy, GColor color, GColor hole) {
  graphics_context_set_fill_color(ctx, color);
  int r = 7;
  graphics_fill_rect(ctx, GRect(cx - 2, cy - r - 3, 5, 5), 0, GCornerNone);  /* top */
  graphics_fill_rect(ctx, GRect(cx - 2, cy + r - 2, 5, 5), 0, GCornerNone);  /* bottom */
  graphics_fill_rect(ctx, GRect(cx - r - 3, cy - 2, 5, 5), 0, GCornerNone);  /* left */
  graphics_fill_rect(ctx, GRect(cx + r - 2, cy - 2, 5, 5), 0, GCornerNone);  /* right */
  graphics_fill_circle(ctx, GPoint(cx, cy), r);
  graphics_context_set_fill_color(ctx, hole);
  graphics_fill_circle(ctx, GPoint(cx, cy), 3);
}

/* Queue icon (≡): three short rules; `big` matches the tall header's font. */
static void draw_list(GContext *ctx, int x, int cy, GColor color, bool big) {
  graphics_context_set_fill_color(ctx, color);
  int w = big ? 10 : 9, step = big ? 5 : 4, th = 2;
  for (int i = -1; i <= 1; i++) {
    graphics_fill_rect(ctx, GRect(x, cy + i * step - th / 2, w, th), 0, GCornerNone);
  }
}

#ifndef PBL_ROUND   /* round has no room for it in the header */
/* Loop icon (↻): a ring with a small arrowhead at its top-right. */
static void draw_loop(GContext *ctx, int cx, int cy, GColor color) {
  graphics_context_set_stroke_color(ctx, color);
  graphics_context_set_stroke_width(ctx, 2);
  graphics_draw_circle(ctx, GPoint(cx, cy), 5);
  graphics_context_set_stroke_width(ctx, 1);
  graphics_context_set_fill_color(ctx, color);
  fill_tri(ctx, GPoint(cx + 2, cy - 8), GPoint(cx + 2, cy - 2), GPoint(cx + 7, cy - 5));
}
#endif

/* ------------------------------------------------------------------ */
/* Custom-drawn layers                                                 */
/* ------------------------------------------------------------------ */
static GSize text_size(const char *text, GFont f, int max_w, int max_h) {
  return graphics_text_layout_get_content_size(
      text, f, GRect(0, 0, max_w, max_h),
      GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft);
}

/* Header strip: wall clock on the left, loop ↻ when on, ≡ queue count on the
 * right, with a thin rule underneath. Round: clock + queue as one centred
 * group (the narrow top of the circle has no room for a spread row). */
static void header_update_proc(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  bool big = (b.size.h >= HEADER_H_TALL);
  GFont f = fonts_get_system_font(big ? FONT_KEY_GOTHIC_18_BOLD : FONT_KEY_GOTHIC_14_BOLD);
  int cy = b.size.h / 2;
  /* Gothic draws with top leading: shift the text box up so the digits sit
   * on the strip's centre line. */
  int ty = cy - (big ? 13 : 10), tbox = big ? 24 : 18;

  char clock_buf[8], q_buf[8];
  clock_copy_time_string(clock_buf, sizeof(clock_buf));
  snprintf(q_buf, sizeof(q_buf), "%d", s_queue_size);
  GSize cs = text_size(clock_buf, f, b.size.w, b.size.h);
  GSize qs = text_size(q_buf, f, b.size.w, b.size.h);
  int list_w = big ? 10 + 4 : 9 + 3;   /* icon + gap before the count */
  bool queue = (s_mode == MODE_DISCOVERY);   /* Musicolet: clock only */
  if (!queue) qs.w = list_w = 0;

#ifdef PBL_ROUND
  int group_w = cs.w + (queue ? 10 : 0) + list_w + qs.w;
  int x = (b.size.w - group_w) / 2;
  int q_x = x + cs.w + 10;
#else
  int x = queue ? 0 : (b.size.w - cs.w) / 2;   /* Musicolet: centred clock */
  int q_x = b.size.w - qs.w - list_w;
#endif

  graphics_context_set_text_color(ctx, col_fg());
  graphics_draw_text(ctx, clock_buf, f, GRect(x, ty, cs.w + 4, tbox),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

  if (queue) {
    draw_list(ctx, q_x, cy, col_muted(), big);
    graphics_context_set_text_color(ctx, col_muted());
    graphics_draw_text(ctx, q_buf, f, GRect(q_x + list_w, ty, qs.w + 4, tbox),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  }

#ifndef PBL_ROUND
  if (s_loop && queue) draw_loop(ctx, b.size.w / 2, cy, col_muted());
  graphics_context_set_stroke_color(ctx, PBL_IF_COLOR_ELSE(col_track(), col_fg()));
  graphics_draw_line(ctx, GPoint(0, b.size.h - 1), GPoint(b.size.w - 1, b.size.h - 1));
#endif
}

/* Chip row: the saved genre as a filled pill in its genre colour (text in the
 * background colour), or an outline pill; then ★ (Top) and ⚑ (Untag / staged)
 * icons when set. Left-aligned on rect, centred on round and in Musicolet mode. In Musicolet mode the
 * star is hollow while the Move to Top is only queued, the outline pill reads
 * "No genre" / "Not in library", and when the side column next to the art is
 * too narrow for pill + icons, the icons wrap onto a second row below the pill. */
#define CHIP_PAD   8   /* pill text side padding */
#define CHIP_GAP   6   /* pill → icon, icon → icon */
#define STAR_W    20
#define FLAG_W    14

static void chip_update_proc(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  GFont f = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
  bool saved = (s_genre[0] != '\0');
  const char *label = saved ? s_genre : "Not saved";
  if (!saved && s_mode == MODE_MUSICOLET) label = s_matched ? "No genre" : "Not in library";
  bool star = s_is_top || (s_mode == MODE_MUSICOLET && s_top_pending);
  int pad = (b.size.w < 100) ? 4 : CHIP_PAD;   /* narrow column beside the art */

  int icons_w = (star ? CHIP_GAP + STAR_W : 0) + (s_is_untag ? CHIP_GAP + FLAG_W : 0);
  /* Wrap the icons onto their own row if they don't fit beside the pill and the
   * layer is tall enough for two rows. */
  bool wrap = (icons_w > 0 && b.size.h >= 2 * ROW_CHIP_H);
  int max_text_w = b.size.w - 2 * pad - (wrap ? 0 : icons_w);
  if (max_text_w < 10) max_text_w = 10;
  GSize ts = text_size(label, f, max_text_w, PILL_H + 8);
  int pill_w = ts.w + 2 * pad;
  if (wrap && pill_w + icons_w <= b.size.w) wrap = false;   /* fits on one row */
  int row_w = wrap ? pill_w : pill_w + icons_w;
  bool center = PBL_IF_ROUND_ELSE(true, s_mode == MODE_MUSICOLET);

  int x = center ? (b.size.w - row_w) / 2 : 0;
  if (x < 0) x = 0;
  int py = wrap ? (ROW_CHIP_H - PILL_H) / 2 : (b.size.h - PILL_H) / 2;
  GRect pill = GRect(x, py, pill_w, PILL_H);

  GColor text_col;
  if (saved) {
    graphics_context_set_fill_color(ctx, genre_gcolor(s_genre_color));
    graphics_fill_rect(ctx, pill, PILL_R, GCornersAll);
    text_col = col_bg();
  } else {
    graphics_context_set_stroke_color(ctx, col_muted());
    graphics_draw_round_rect(ctx, pill, PILL_R);
    text_col = col_muted();
  }
  graphics_context_set_text_color(ctx, text_col);
  graphics_draw_text(ctx, label, f,
      GRect(x + pad, py + (PILL_H - ts.h) / 2 - 3, ts.w + 2, ts.h + 6),
      GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

  int cy = py + PILL_H / 2;
  if (wrap) {
    /* second row: icons start under the pill (no leading gap) */
    cy = ROW_CHIP_H + ROW_CHIP_H / 2;
    x = (center ? (b.size.w - icons_w + CHIP_GAP) / 2 : 0) - CHIP_GAP;
  } else {
    x += pill_w;
  }
  if (star) {
    x += CHIP_GAP;
    /* filled = settled in Top; hollow = a move into / out of Top is queued */
    if (s_is_top && !(s_mode == MODE_MUSICOLET && s_top_pending)) draw_star(ctx, x + STAR_W / 2, cy, col_top());
    else draw_star_outline(ctx, x + STAR_W / 2, cy, col_top());
    x += STAR_W;
  }
  if (s_is_untag) {
    x += CHIP_GAP;
    draw_flag(ctx, x + FLAG_W / 2, cy, col_untag(), false);
  }
}

/* Album art (Musicolet mode): the decoded cover, centred in the layer, or a
 * placeholder tile with a music note while none is loaded (no art, aplite, or
 * still arriving). */
static void art_update_proc(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  if (s_art_bitmap && s_reachable && s_error[0] == '\0') {
    GSize sz = gbitmap_get_bounds(s_art_bitmap).size;
    GRect r = GRect((b.size.w - sz.w) / 2, (b.size.h - sz.h) / 2, sz.w, sz.h);
    graphics_context_set_compositing_mode(ctx, GCompOpSet);
    graphics_draw_bitmap_in_rect(ctx, s_art_bitmap, r);
    return;
  }
  graphics_context_set_fill_color(ctx, col_track());
  graphics_fill_rect(ctx, b, 6, GCornersAll);
#ifndef PBL_COLOR
  graphics_context_set_stroke_color(ctx, col_fg());
  graphics_draw_round_rect(ctx, b, 6);
#endif
  /* ♪: a note head + stem + flag, scaled to the tile */
  int u = b.size.w / 12;
  if (u < 2) u = 2;
  int hx = b.size.w / 2 - u, hy = b.size.h / 2 + 2 * u;
  graphics_context_set_fill_color(ctx, col_muted());
  graphics_fill_circle(ctx, GPoint(hx, hy), u + u / 2);
  graphics_fill_rect(ctx, GRect(hx + u, hy - 5 * u, u, 5 * u), 0, GCornerNone);
  graphics_fill_rect(ctx, GRect(hx + u, hy - 5 * u, 3 * u, u), 0, GCornerNone);
}

/* Action bar aligned with the physical buttons. Each shows its tap action, and
 * swaps to its hold action while that button is held:
 *   UP:     previous (◀◀)                  → Top (★)       when held
 *   SELECT: play ▶ / pause ❚❚              → settings gear when held
 *   DOWN:   skip/remove (🚫, or ▶| if saved) → Untag (⚑)     when held */
static void buttons_update_proc(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
#ifndef PBL_ROUND
  graphics_context_set_fill_color(ctx, ab_bg());
  graphics_fill_rect(ctx, b, 0, GCornerNone);
#endif
  int cx = b.size.w / 2;
  int up_y   = b.size.h * PBL_IF_ROUND_ELSE(30, 22) / 100;
  int mid_y  = b.size.h / 2;
  int down_y = b.size.h * PBL_IF_ROUND_ELSE(70, 78) / 100;
  GColor icon = ab_icon();
  GColor behind = ab_bg();

  /* UP: previous, or Top while held */
  if (s_hold == 1) draw_star(ctx, cx, up_y, ab_accent(GColorYellow));
  else             draw_prev(ctx, cx, up_y, icon);

  /* SELECT: play/pause, or settings while held */
  if (s_hold == 2) {
    draw_gear(ctx, cx, mid_y, icon, behind);
  } else {
    /* Same colour as the other bar icons: a status tint (e.g. green) was
     * near-invisible on the night strip. The progress bar carries status. */
    if (s_status == 1)      draw_pause(ctx, cx, mid_y, icon);     /* playing → pause hint */
    else if (s_status == 4) draw_playpause(ctx, cx, mid_y, icon); /* state unknown */
    else                    draw_play(ctx, cx, mid_y, icon);
  }

  /* DOWN: skip/remove — 🚫 when unsaved (discards it), ▶| when saved to a
   * genre (it stays in the download queue) — or Untag while held */
  if (s_hold == 3)             draw_flag(ctx, cx, down_y, ab_accent(GColorRed), true);
  else if (s_mode == MODE_MUSICOLET) draw_next(ctx, cx, down_y, icon);
  else if (s_genre[0] != '\0') draw_skip_fwd(ctx, cx, down_y, icon);
  else                         draw_no_entry(ctx, cx, down_y, icon);
}

/* Progress: a 6px rounded track, the played part in the status colour, and a
 * playhead knob. The track is inset by the knob radius so the knob never clips
 * at either end. B&W: outlined track + filled played part. */
static void progress_update_proc(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  int tx = KNOB_R, tw = b.size.w - 2 * KNOB_R;
  int cy = b.size.h / 2;
  GRect track = GRect(tx, cy - 3, tw, 6);

  graphics_context_set_fill_color(ctx, col_track());
  graphics_fill_rect(ctx, track, 3, GCornersAll);
#ifndef PBL_COLOR
  graphics_context_set_stroke_color(ctx, col_fg());
  graphics_draw_round_rect(ctx, track, 3);
#endif

  if (s_duration > 0 && s_position >= 0) {
    int w = (tw * s_position) / s_duration;
    if (w > tw) w = tw;
    if (w < 0) w = 0;
    graphics_context_set_fill_color(ctx, status_color());
    if (w > 0) graphics_fill_rect(ctx, GRect(tx, cy - 3, w, 6), w >= 6 ? 3 : 0, GCornersAll);
    graphics_context_set_fill_color(ctx, col_fg());
    graphics_fill_circle(ctx, GPoint(tx + w, cy), KNOB_R);
  }
}

/* ------------------------------------------------------------------ */
/* UI                                                                  */
/* ------------------------------------------------------------------ */
static void tick_callback(void *data);

/* Arm the 1s interpolation timer, but only while actually playing. When
 * paused, idle or offline there is nothing to advance, so the timer stays
 * dead and the watch sleeps between polls. Idempotent — safe to call from
 * every UI update. */
static void schedule_tick(void) {
  if (!s_tick_timer && s_reachable && s_status == 1) {
    s_tick_timer = app_timer_register(1000, tick_callback, NULL);
  }
}

/* Header at the top, bar + time row pinned to the bottom, and the
 * title/channel/chip block centred between them. The title is measured after
 * its text is set (call this AFTER update_ui sets the texts), capped at 3 lines
 * on tall screens / 2 on short ones. Safe to call on every refresh. */
/* Musicolet mode: album art centred at the top of the content column, then a
 * one-line title, the artist (tall screens only) and the chip row, centred in the
 * space down to the bar; bar + time pinned to the bottom as in discovery mode.
 * With no bar, ``bottom_y`` is the screen bottom and the art grows into the space.
 * The art edge must match ART_SPEC in index.js (the phone renders that size). */
static void layout_musicolet(int x, int cw, bool tall, int header_h, int bottom_y, bool big) {
  int top = HEADER_Y + header_h + 1;
  int bottom = bottom_y - 1;
  int a = big ? (tall ? ART_TALL_BIG : PBL_IF_ROUND_ELSE(ART_ROUND_SHORT_BIG, ART_SHORT_BIG))
              : (tall ? ART_TALL : PBL_IF_ROUND_ELSE(ART_ROUND_SHORT, ART_SHORT));
  layer_set_frame(s_art_layer, GRect(x + (cw - a) / 2, top, a, a));

  bool has_chan = tall && (text_layer_get_text(s_channel_layer)[0] != '\0');
  int chan_h = has_chan ? ROW_CHAN_H : 0;
  int area_top = top + a;
  int block = ROW_TITLE1_H + chan_h + ROW_CHIP_H;
  int y = area_top + (bottom - area_top - block) / 2;
  if (y < area_top) y = area_top;
  layer_set_frame(text_layer_get_layer(s_title_layer), GRect(x, y, cw, ROW_TITLE1_H));
  y += ROW_TITLE1_H;
  layer_set_frame(text_layer_get_layer(s_channel_layer), GRect(x, y, cw, chan_h));
  y += chan_h;
  layer_set_frame(s_chip_layer, GRect(x, y, cw, ROW_CHIP_H));
}

static void layout_now_playing(void) {
  if (!s_window || !s_title_layer) return;
  GRect b = layer_get_bounds(window_get_root_layer(s_window));
  int w = b.size.w, h = b.size.h;
  int x = CONTENT_M;
  int cw = w - AB_W - 2 * CONTENT_M;
#ifdef PBL_ROUND
  cw = w - 2 * CONTENT_M;   /* symmetric inset; icons sit in the curve */
#endif
  bool tall = (h >= 200);
  int gap = tall ? 6 : 1;

  int header_h = tall ? HEADER_H_TALL : HEADER_H_SHORT;
  layer_set_frame(s_header_layer, GRect(x, HEADER_Y, cw, header_h));

  int time_y = h - BOTTOM_M - ROW_TIME_H;
  int bar_y = time_y - ROW_BAR_H;
  layer_set_frame(s_progress_layer, GRect(x, bar_y, cw, ROW_BAR_H));
  layer_set_frame(text_layer_get_layer(s_time_layer),   GRect(x, time_y, cw, ROW_TIME_H));
  layer_set_frame(text_layer_get_layer(s_remain_layer), GRect(x, time_y, cw, ROW_TIME_H));

  layer_set_hidden(s_art_layer, s_mode != MODE_MUSICOLET);
  /* Musicolet with nothing to show (offline / Shizuku off / idle): no chip row */
  layer_set_hidden(s_chip_layer, s_mode == MODE_MUSICOLET && (!s_reachable || s_error[0] != '\0'));
  /* Musicolet read without Shizuku has no position: no bar, no time row */
  bool no_bar = (s_mode == MODE_MUSICOLET && s_status == 4);
  layer_set_hidden(s_progress_layer, no_bar);
  layer_set_hidden(text_layer_get_layer(s_time_layer), no_bar);
  layer_set_hidden(text_layer_get_layer(s_remain_layer), no_bar);
  /* Musicolet centres title + artist (round always does) */
  GTextAlignment align = PBL_IF_ROUND_ELSE(GTextAlignmentCenter,
      s_mode == MODE_MUSICOLET ? GTextAlignmentCenter : GTextAlignmentLeft);
  text_layer_set_text_alignment(s_title_layer, align);
  text_layer_set_text_alignment(s_channel_layer, align);
  if (s_mode == MODE_MUSICOLET) {
    layout_musicolet(x, cw, tall, header_h, no_bar ? h - BOTTOM_M : bar_y, no_bar);
    return;
  }

  /* measure the title within its max box */
  int title_max = tall ? 86 : 58;
  layer_set_frame(text_layer_get_layer(s_title_layer), GRect(x, 0, cw, title_max));
  int th = text_layer_get_content_size(s_title_layer).h + 4;
  if (th > title_max) th = title_max;
  bool has_chan = (text_layer_get_text(s_channel_layer)[0] != '\0');
  int chan_h = has_chan ? ROW_CHAN_H + gap : 0;

  int top = HEADER_Y + header_h + 2;
  int bottom = bar_y - 2;
  int block = th + chan_h + gap + ROW_CHIP_H;
  int y = top + (bottom - top - block) / 2;
  if (y < top) y = top;

  layer_set_frame(text_layer_get_layer(s_title_layer), GRect(x, y, cw, th));
  y += th;
  layer_set_frame(text_layer_get_layer(s_channel_layer), GRect(x, y, cw, has_chan ? ROW_CHAN_H : 0));
  y += chan_h + gap;
  layer_set_frame(s_chip_layer, GRect(x, y, cw, ROW_CHIP_H));
}

static void update_ui(void) {
  /* Re-apply theme colours every refresh so Auto mode can flip live if the app
   * stays open across the day/night boundary. The custom layers read their
   * colours at draw time. */
  if (s_window) window_set_background_color(s_window, col_bg());
  text_layer_set_text_color(s_time_layer, col_muted());
  text_layer_set_text_color(s_remain_layer, col_muted());

  /* Title/channel slots double as the status line for the states that need
   * words (play/pause is shown by the action-bar icon + bar colour). */
  if (!s_reachable) {
    text_layer_set_text(s_title_layer, "Offline");
    text_layer_set_text_color(s_title_layer, col_untag());
    text_layer_set_text(s_channel_layer,
                        s_mode == MODE_MUSICOLET ? "Start Instrumentalls" : "Start the queue");
    text_layer_set_text_color(s_channel_layer, col_muted());
  } else if (s_mode == MODE_MUSICOLET && s_error[0] != '\0') {
    /* Musicolet can't be shown: Shizuku off, Mission Control down, idle... */
    text_layer_set_text(s_title_layer, "Musicolet");
    text_layer_set_text_color(s_title_layer, col_title());
    text_layer_set_text(s_channel_layer, s_error);
    text_layer_set_text_color(s_channel_layer, col_untag());
  } else {
    text_layer_set_text(s_title_layer, s_title);
    text_layer_set_text_color(s_title_layer, col_title());
    if (s_status == 3) {
      text_layer_set_text(s_channel_layer, "Buffering…");
      text_layer_set_text_color(s_channel_layer, status_color());
    } else {
      text_layer_set_text(s_channel_layer, s_channel);
      text_layer_set_text_color(s_channel_layer, col_channel());
    }
  }

  layout_now_playing();   /* after the texts: the title is measured */

  /* time row ("MMM:SS" fits in 8 incl. NUL, even for long tracks) */
  char pos[8], dur[8];
  format_mmss(s_position, pos, sizeof(pos));
  format_mmss(s_duration, dur, sizeof(dur));
#ifdef PBL_ROUND
  snprintf(s_time_buf, sizeof(s_time_buf), "%s / %s", pos, dur);
  s_remain_buf[0] = '\0';
#else
  int rem = s_duration - s_position;
  char rbuf[8];
  format_mmss(rem, rbuf, sizeof(rbuf));
  snprintf(s_time_buf, sizeof(s_time_buf), "%s", pos);
  snprintf(s_remain_buf, sizeof(s_remain_buf), "-%s", rbuf);
#endif
  text_layer_set_text(s_time_layer, s_time_buf);
  text_layer_set_text(s_remain_layer, s_remain_buf);

  layer_mark_dirty(s_header_layer);
  layer_mark_dirty(s_chip_layer);
  layer_mark_dirty(s_progress_layer);
  layer_mark_dirty(s_buttons_layer);   /* play/pause icon follows status */
  layer_mark_dirty(s_art_layer);

  schedule_tick();   /* (re)start interpolation if we just became playing */
}

/* Local 1s interpolation so the progress bar/time advance smoothly between
 * polls. Only advances — and only keeps waking the CPU — while playing;
 * otherwise it lets the timer die and re-arms via schedule_tick() when the
 * next poll reports playback resumed. */
static void tick_callback(void *data) {
  s_tick_timer = NULL;   /* this handle has fired; treat as not running */
  if (s_reachable && s_status == 1) {
    if (s_duration > 0 && s_position < s_duration) {
      s_position++;
      update_ui();       /* re-arms via schedule_tick() */
    } else {
      schedule_tick();   /* still playing, nothing to advance yet — keep alive */
    }
  }
}

/* Wall clock in the header: repaint once a minute. */
static void minute_tick(struct tm *tick_time, TimeUnits changed) {
  if (s_header_layer) layer_mark_dirty(s_header_layer);
}

/* ------------------------------------------------------------------ */
/* Settings menu (MenuLayer)                                           */
/* ------------------------------------------------------------------ */
/* The system ActionMenu ignores its body background colour on emery (it only
 * tints the crumb sidebar), so we use a MenuLayer, whose normal/highlight
 * colours ARE honoured. Two screens: the settings list, and a pushed genre
 * picker sub-screen. */

static void menu_apply_theme(MenuLayer *ml) {
  menu_layer_set_normal_colors(ml, col_bg(), col_fg());
  /* selected row = inverse of the background, so it stays high-contrast. */
  menu_layer_set_highlight_colors(ml, col_fg(), col_bg());
}

/* Compact single-line rows — between the default 44px cell and a tight 32px. */
#define MENU_ROW_H 38
static int16_t menu_cell_height(MenuLayer *ml, MenuIndex *idx, void *ctx) {
  return MENU_ROW_H;
}
/* One line of left-aligned text, vertically centred in the compact cell.
 * MenuLayer has already filled the cell background and set the text colour
 * (normal vs highlight) before this runs, so we only draw the glyphs. */
static void draw_menu_row(GContext *ctx, const Layer *cell, const char *text) {
  GRect b = layer_get_bounds(cell);
  GFont f = fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD);
  GSize ts = graphics_text_layout_get_content_size(
      text, f, GRect(0, 0, b.size.w - 12, 40),
      GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft);
  int y = b.origin.y + (b.size.h - ts.h) / 2 - 3;   /* -3 trims GOTHIC top leading */
  graphics_draw_text(ctx, text, f,
                     GRect(b.origin.x + 6, y, b.size.w - 12, ts.h + 6),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
}

/* ---- "Save to genre" picker sub-screen ---- */
static uint16_t genre_num_rows(MenuLayer *ml, uint16_t section, void *ctx) {
  return s_genre_count > 0 ? s_genre_count : 1;
}
static void genre_draw_row(GContext *ctx, const Layer *cell, MenuIndex *idx, void *cb_ctx) {
  const char *name = (s_genre_count > 0) ? s_genre_names[idx->row] : "(no genres yet)";
  draw_menu_row(ctx, cell, name);
}
static void genre_select(MenuLayer *ml, MenuIndex *idx, void *cb_ctx) {
  if (s_genre_count == 0) { window_stack_pop(true); return; }   /* just back to settings */
  static const char *cmds[] = { "genre", "topgenre", "setgenre" };
  send_cmd_arg(cmds[s_picker], idx->row + 1);
  /* Return all the way to now-playing: drop the settings list from under us,
   * then pop this picker. */
  if (s_settings_window) window_stack_remove(s_settings_window, false);
  window_stack_pop(true);
}
static void genre_window_load(Window *w) {
  window_set_background_color(w, col_bg());   /* fills below the last row */
  Layer *root = window_get_root_layer(w);
  s_genre_menu = menu_layer_create(layer_get_bounds(root));
  menu_layer_set_callbacks(s_genre_menu, NULL, (MenuLayerCallbacks) {
    .get_num_rows = genre_num_rows,
    .get_cell_height = menu_cell_height,
    .draw_row = genre_draw_row,
    .select_click = genre_select,
  });
  menu_apply_theme(s_genre_menu);
  if (s_picker == PICK_GENRE) {   /* start on the song's current genre */
    for (int i = 0; i < s_genre_count; i++) {
      if (strcmp(s_genre_names[i], s_genre) == 0) {
        menu_layer_set_selected_index(s_genre_menu, (MenuIndex) { 0, i }, MenuRowAlignCenter, false);
        break;
      }
    }
  }
  menu_layer_set_click_config_onto_window(s_genre_menu, w);
  layer_add_child(root, menu_layer_get_layer(s_genre_menu));
}
static void genre_window_unload(Window *w) {
  menu_layer_destroy(s_genre_menu);
  s_genre_menu = NULL;
}
static void push_genre_window(void) {
  if (s_genre_window && window_stack_contains_window(s_genre_window)) return;
  if (!s_genre_window) {
    s_genre_window = window_create();
    window_set_window_handlers(s_genre_window, (WindowHandlers) {
      .load = genre_window_load,
      .unload = genre_window_unload,
    });
  }
  window_stack_push(s_genre_window, true);
}

/* ---- main settings list ----
 * The rows depend on the source: discovery-queue actions, or the Musicolet
 * actions. Source and Display are in both. */
enum {
  SROW_GENRE = 0,   /* -> genre picker sub-screen  */
  SROW_NEXT,        /* advance without removing (vs SROW_SKIP) */
  SROW_UNSAVE,
  SROW_LOOP,
  SROW_SHUFFLE,
  SROW_SOURCE,      /* cycles Auto -> Discovery -> Musicolet in place */
  SROW_DISPLAY,     /* cycles Auto -> Day -> Night in place */
  SROW_SKIP,
  SROW_TOP,
  SROW_UNTAG,
  SROW_MTOP,        /* Musicolet: Move to Top (queue) / cancel */
  SROW_TOPGENRE,    /* Musicolet: Move to Top as <genre> -> picker */
  SROW_STAGE,       /* Musicolet: stage for untagging */
  SROW_SETGENRE,    /* Musicolet: change the file's genre -> picker */
};

static const int DISCOVERY_ROWS[] = { SROW_GENRE, SROW_NEXT, SROW_UNSAVE, SROW_LOOP,
                                      SROW_SHUFFLE, SROW_SOURCE, SROW_DISPLAY, SROW_SKIP,
                                      SROW_TOP, SROW_UNTAG };
static const int MUSICOLET_ROWS[] = { SROW_SETGENRE, SROW_MTOP, SROW_TOPGENRE, SROW_STAGE,
                                      SROW_SOURCE, SROW_DISPLAY };
#define ARRAY_LEN(a) ((int) (sizeof(a) / sizeof((a)[0])))

static int settings_row_id(int row) {
  if (s_mode == MODE_MUSICOLET) return MUSICOLET_ROWS[row < ARRAY_LEN(MUSICOLET_ROWS) ? row : 0];
  return DISCOVERY_ROWS[row < ARRAY_LEN(DISCOVERY_ROWS) ? row : 0];
}

static uint16_t settings_num_rows(MenuLayer *ml, uint16_t section, void *ctx) {
  return s_mode == MODE_MUSICOLET ? ARRAY_LEN(MUSICOLET_ROWS) : ARRAY_LEN(DISCOVERY_ROWS);
}
static void settings_draw_row(GContext *ctx, const Layer *cell, MenuIndex *idx, void *cb_ctx) {
  static const char *mode_names[] = { "Auto", "Day", "Night" };
  static const char *source_names[] = { "Auto", "Discovery", "Musicolet" };
  char buf[24];
  const char *text = "";
  switch (settings_row_id(idx->row)) {
    case SROW_GENRE:   text = "Save to genre"; break;
    case SROW_NEXT:    text = "Next song"; break;
    case SROW_UNSAVE:  text = "Unsave"; break;
    case SROW_LOOP:    text = "Loop on/off"; break;
    case SROW_SHUFFLE: text = "Shuffle"; break;
    case SROW_SOURCE:
      snprintf(buf, sizeof(buf), "Source: %s",
               source_names[(s_source >= 0 && s_source <= 2) ? s_source : 0]);
      text = buf; break;
    case SROW_DISPLAY:
      snprintf(buf, sizeof(buf), "Display: %s",
               mode_names[(s_theme >= 0 && s_theme <= 2) ? s_theme : 0]);
      text = buf; break;
    case SROW_SKIP:    text = "Skip (remove)"; break;
    case SROW_TOP:     text = "Mark Top"; break;
    case SROW_UNTAG:   text = "Mark Untagged"; break;
    case SROW_MTOP:
      text = s_top_pending ? "Cancel move" : (s_is_top ? "Remove from Top" : "Move to Top"); break;
    case SROW_TOPGENRE: text = "Top as genre…"; break;
    case SROW_STAGE:   text = s_is_untag ? "Already staged" : "Stage untag"; break;
    case SROW_SETGENRE: text = "Change genre"; break;
  }
  draw_menu_row(ctx, cell, text);
}
static void settings_select(MenuLayer *ml, MenuIndex *idx, void *cb_ctx) {
  switch (settings_row_id(idx->row)) {
    case SROW_GENRE:    s_picker = PICK_SAVE;  push_genre_window(); return;   /* keep settings open */
    case SROW_TOPGENRE: s_picker = PICK_TOP;   push_genre_window(); return;
    case SROW_SETGENRE:
      if (!s_matched) break;                          /* not in the collection: nothing to tag */
      s_picker = PICK_GENRE; push_genre_window(); return;
    case SROW_SOURCE:                                 /* cycle the source in place */
      s_source = (s_source + 1) % 3;
      send_cmd_arg("source", s_source);               /* the phone switches + re-polls */
      layer_mark_dirty(menu_layer_get_layer(ml));
      return;
    case SROW_DISPLAY:                                /* cycle the theme in place */
      s_theme = (s_theme + 1) % 3;
      persist_write_int(PERSIST_KEY_THEME, s_theme);
      update_ui();                                    /* repaint now-playing behind */
      menu_apply_theme(ml);                           /* and re-theme the menu itself */
      layer_mark_dirty(menu_layer_get_layer(ml));
      return;
    case SROW_UNSAVE:  send_cmd("unsave"); break;
    case SROW_LOOP:    send_cmd("loop"); break;
    case SROW_SHUFFLE: send_cmd("shuffle"); break;
    case SROW_NEXT:    send_cmd("next"); break;   /* keeps it queued/saved */
    case SROW_SKIP:    send_cmd("skip"); break;
    case SROW_TOP:     send_cmd("top"); break;
    case SROW_UNTAG:   send_cmd("untag"); break;
    case SROW_MTOP:
      send_cmd((s_is_top || s_top_pending) ? "untop" : "top");
      break;
    case SROW_STAGE:   if (!s_is_untag) send_cmd("stage"); break;
  }
  window_stack_pop(true);   /* action performed -> back to now-playing */
}
static void settings_window_load(Window *w) {
  window_set_background_color(w, col_bg());   /* fills below the last row */
  Layer *root = window_get_root_layer(w);
  s_settings_menu = menu_layer_create(layer_get_bounds(root));
  menu_layer_set_callbacks(s_settings_menu, NULL, (MenuLayerCallbacks) {
    .get_num_rows = settings_num_rows,
    .get_cell_height = menu_cell_height,
    .draw_row = settings_draw_row,
    .select_click = settings_select,
  });
  menu_apply_theme(s_settings_menu);
  menu_layer_set_click_config_onto_window(s_settings_menu, w);
  layer_add_child(root, menu_layer_get_layer(s_settings_menu));
}
static void settings_window_unload(Window *w) {
  menu_layer_destroy(s_settings_menu);
  s_settings_menu = NULL;
}
static void open_settings_menu(void) {
  if (!s_settings_window) {
    s_settings_window = window_create();
    window_set_window_handlers(s_settings_window, (WindowHandlers) {
      .load = settings_window_load,
      .unload = settings_window_unload,
    });
  }
  window_stack_push(s_settings_window, true);
}

/* ------------------------------------------------------------------ */
/* Volume screen                                                       */
/* ------------------------------------------------------------------ */
static void volume_draw(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, col_bg());
  graphics_fill_rect(ctx, b, 0, GCornerNone);

  int ab_x = b.size.w - AB_W;   /* rect: the action-bar strip */
  int x = CONTENT_M;
  int cw = PBL_IF_ROUND_ELSE(b.size.w - 2 * CONTENT_M, ab_x - 2 * CONTENT_M);
  int cy = b.size.h / 2;

  graphics_context_set_text_color(ctx, col_muted());
  graphics_draw_text(ctx, "Volume", fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                     GRect(x, cy - 62, cw, 24), GTextOverflowModeTrailingEllipsis,
                     GTextAlignmentCenter, NULL);

  char num[8];
  const char *big = num;
  if (s_vol >= 0) snprintf(num, sizeof(num), "%d", s_vol);
  else big = s_vol_failed ? "--" : "...";
  graphics_context_set_text_color(ctx, col_title());
  graphics_draw_text(ctx, big, fonts_get_system_font(FONT_KEY_BITHAM_42_BOLD),
                     GRect(x, cy - 40, cw, 50), GTextOverflowModeTrailingEllipsis,
                     GTextAlignmentCenter, NULL);

  /* level bar */
  int bar_w = cw - PBL_IF_ROUND_ELSE(16, 8);
  GRect track = GRect(x + (cw - bar_w) / 2, cy + 18, bar_w, 8);
  graphics_context_set_fill_color(ctx, col_track());
  graphics_fill_rect(ctx, track, 4, GCornersAll);
#ifndef PBL_COLOR
  graphics_context_set_stroke_color(ctx, col_fg());
  graphics_draw_round_rect(ctx, track, 4);
#endif
  if (s_vol > 0 && s_vol_max > 0) {
    int fill = track.size.w * (s_vol > s_vol_max ? s_vol_max : s_vol) / s_vol_max;
    if (fill < 8) fill = 8;   /* keep the rounded ends visible */
    graphics_context_set_fill_color(ctx, PBL_IF_COLOR_ELSE(col_title(), col_fg()));
    graphics_fill_rect(ctx, GRect(track.origin.x, track.origin.y, fill, track.size.h), 4, GCornersAll);
  }

  char sub[24];
  if (s_vol_failed) snprintf(sub, sizeof(sub), "Unavailable");
  else snprintf(sub, sizeof(sub), "of %d", s_vol_max);
  graphics_context_set_text_color(ctx, col_muted());
  graphics_draw_text(ctx, sub, fonts_get_system_font(FONT_KEY_GOTHIC_18),
                     GRect(x, cy + 28, cw, 24), GTextOverflowModeTrailingEllipsis,
                     GTextAlignmentCenter, NULL);

  /* button hints: + (UP), ▶ back to the player (SELECT), − (DOWN) */
#ifndef PBL_ROUND
  graphics_context_set_fill_color(ctx, ab_bg());
  graphics_fill_rect(ctx, GRect(ab_x, 0, AB_W, b.size.h), 0, GCornerNone);
#endif
  int ix = ab_x + AB_W / 2;   /* same column as the player's icons */
  int up_y   = b.size.h * PBL_IF_ROUND_ELSE(30, 22) / 100;
  int down_y = b.size.h * PBL_IF_ROUND_ELSE(70, 78) / 100;
  GColor icon = ab_icon();
  graphics_context_set_fill_color(ctx, icon);
  graphics_fill_rect(ctx, GRect(ix - 6, up_y - 1, 13, 3), 0, GCornerNone);     /* + */
  graphics_fill_rect(ctx, GRect(ix - 1, up_y - 6, 3, 13), 0, GCornerNone);
  graphics_fill_rect(ctx, GRect(ix - 6, down_y - 1, 13, 3), 0, GCornerNone);   /* − */
  draw_play(ctx, ix, b.size.h / 2, icon);
}

static void volume_send(void *data) {
  s_vol_timer = NULL;
  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) != APP_MSG_OK) {   /* busy: try again shortly */
    s_vol_timer = app_timer_register(100, volume_send, NULL);
    return;
  }
  dict_write_cstring(iter, MESSAGE_KEY_CMD, "volset");
  dict_write_int(iter, MESSAGE_KEY_ARG, &s_vol, sizeof(int), true);
  app_message_outbox_send();
}

static void volume_step(int delta) {
  if (s_vol < 0) { send_cmd("volget"); return; }   /* not known yet: ask again */
  int v = s_vol + delta;
  if (v < 0) v = 0;
  if (v > s_vol_max) v = s_vol_max;
  if (v == s_vol) return;
  s_vol = v;
  s_vol_local = true;
  if (s_volume_layer) layer_mark_dirty(s_volume_layer);
  if (s_vol_timer) app_timer_reschedule(s_vol_timer, VOL_SEND_MS);
  else s_vol_timer = app_timer_register(VOL_SEND_MS, volume_send, NULL);
}

static void volume_up(ClickRecognizerRef r, void *ctx)   { volume_step(1); }
static void volume_down(ClickRecognizerRef r, void *ctx) { volume_step(-1); }
static void volume_close(ClickRecognizerRef r, void *ctx) { window_stack_pop(true); }

static void volume_click_config(void *ctx) {
  window_single_repeating_click_subscribe(BUTTON_ID_UP, 150, volume_up);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, 150, volume_down);
  window_single_click_subscribe(BUTTON_ID_SELECT, volume_close);
}

static void volume_window_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  s_volume_layer = layer_create(layer_get_bounds(root));
  layer_set_update_proc(s_volume_layer, volume_draw);
  layer_add_child(root, s_volume_layer);
}
static void volume_window_unload(Window *w) {
  layer_destroy(s_volume_layer);
  s_volume_layer = NULL;
}
static void open_volume(ClickRecognizerRef r, void *ctx) {
  if (s_volume_window && window_stack_contains_window(s_volume_window)) return;
  if (!s_volume_window) {
    s_volume_window = window_create();
    window_set_click_config_provider(s_volume_window, volume_click_config);
    window_set_window_handlers(s_volume_window, (WindowHandlers) {
      .load = volume_window_load,
      .unload = volume_window_unload,
    });
  }
  s_vol_local = false;    /* take the phone's level again */
  s_vol_failed = false;
  window_stack_push(s_volume_window, true);
  send_cmd("volget");
}

/* ------------------------------------------------------------------ */
/* Buttons                                                             */
/* ------------------------------------------------------------------ */
/* Taps */
static void up_click(ClickRecognizerRef r, void *ctx)     { send_cmd("prev"); }
static void down_click(ClickRecognizerRef r, void *ctx) {
  send_cmd(s_mode == MODE_MUSICOLET ? "next" : "skip");   /* Musicolet: just next song */
}
static void select_click(ClickRecognizerRef r, void *ctx) { send_cmd("playpause"); }

/* Holds: the "down" handler fires once the long-press threshold is reached —
 * light up the button's hold-action hint icon, then perform the action; the
 * shared "up" handler clears the hint on release. */
static void up_long_down(ClickRecognizerRef r, void *ctx) {
  s_hold = 1;
  if (s_buttons_layer) layer_mark_dirty(s_buttons_layer);
  if (s_mode == MODE_MUSICOLET) {
    /* toggle: cancel a queued move, take a Top song out of Top, else Move to Top */
    send_cmd((s_is_top || s_top_pending) ? "untop" : "top");
  } else {
    send_cmd("top");
  }
}
static void down_long_down(ClickRecognizerRef r, void *ctx) {
  s_hold = 3;
  if (s_buttons_layer) layer_mark_dirty(s_buttons_layer);
  send_cmd(s_mode == MODE_MUSICOLET ? "stage" : "untag");
}
static void select_long_down(ClickRecognizerRef r, void *ctx) {
  s_hold = 2;
  if (s_buttons_layer) layer_mark_dirty(s_buttons_layer);
  open_settings_menu();
}
static void hold_release(ClickRecognizerRef r, void *ctx) {
  s_hold = 0;
  if (s_buttons_layer) layer_mark_dirty(s_buttons_layer);
}

static void click_config_provider(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_UP, up_click);
  window_single_click_subscribe(BUTTON_ID_DOWN, down_click);
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click);
  /* BACK tap = volume; holding BACK is the system's quit. The (double-tap)
   * multi-click makes the tap fire on release, not press, so a hold quits
   * without flashing the volume screen first. */
  window_single_click_subscribe(BUTTON_ID_BACK, open_volume);
  window_multi_click_subscribe(BUTTON_ID_BACK, 2, 2, 150, true, open_volume);
  window_long_click_subscribe(BUTTON_ID_UP, 0, up_long_down, hold_release);
  window_long_click_subscribe(BUTTON_ID_DOWN, 0, down_long_down, hold_release);
  window_long_click_subscribe(BUTTON_ID_SELECT, 0, select_long_down, hold_release);
}

/* ------------------------------------------------------------------ */
/* AppMessage inbox                                                    */
/* ------------------------------------------------------------------ */
static void parse_genres(const char *s) {
  /* Format: "Futuristic:1|Guitar:2|Trap:3|W/Hook:4" */
  s_genre_count = 0;
  if (!s) return;
  const char *p = s;
  while (*p && s_genre_count < MAX_GENRES) {
    /* read name up to ':' */
    int ni = 0;
    while (*p && *p != ':' && *p != '|' && ni < GENRE_NAME_LEN - 1) {
      s_genre_names[s_genre_count][ni++] = *p++;
    }
    s_genre_names[s_genre_count][ni] = '\0';
    int code = 0;
    if (*p == ':') {
      p++;
      while (*p >= '0' && *p <= '9') { code = code * 10 + (*p - '0'); p++; }
    }
    s_genre_codes[s_genre_count] = code;
    if (ni > 0) s_genre_count++;
    while (*p && *p != '|') p++;   /* skip to next separator */
    if (*p == '|') p++;
  }
}

static void copy_str(Tuple *t, char *dst, int len) {
  if (!t) return;
  strncpy(dst, t->value->cstring, len - 1);
  dst[len - 1] = '\0';
}

/* ---- album art transfer ----
 * The phone sends ART_LEN (total PNG bytes; 0 = no cover) to start, then
 * ART_OFFSET + ART_DATA chunks in order. Once every byte is in, the PNG is
 * decoded into s_art_bitmap and the buffer freed. A new ART_LEN abandons any
 * transfer in progress. The current cover stays on screen until the new one is
 * complete, so a song change never flashes the placeholder. */
static void art_reset_buffer(void) {
  if (s_art_buf) free(s_art_buf);
  s_art_buf = NULL;
  s_art_len = s_art_recv = 0;
}

static void art_set_bitmap(GBitmap *bmp) {
  if (s_art_bitmap) gbitmap_destroy(s_art_bitmap);
  s_art_bitmap = bmp;
  if (s_art_layer) layer_mark_dirty(s_art_layer);
}

static void art_begin(int len) {
  art_reset_buffer();
  if (len <= 0) { art_set_bitmap(NULL); return; }   /* song has no cover */
  s_art_buf = malloc(len);
  if (!s_art_buf) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "art: no memory for %d bytes", len);
    return;
  }
  s_art_len = len;
}

static void art_chunk(int offset, const uint8_t *data, int len) {
  if (!s_art_buf || offset < 0 || offset + len > s_art_len) return;
  memcpy(s_art_buf + offset, data, len);
  s_art_recv += len;
  if (s_art_recv < s_art_len) return;
  GBitmap *bmp = gbitmap_create_from_png_data(s_art_buf, s_art_len);
  art_reset_buffer();
  if (bmp) art_set_bitmap(bmp);
  else APP_LOG(APP_LOG_LEVEL_WARNING, "art: PNG decode failed");
}

static void inbox_received(DictionaryIterator *iter, void *context) {
  Tuple *t;
  /* Art messages carry nothing else: handle them without a full UI refresh. */
  if ((t = dict_find(iter, MESSAGE_KEY_ART_LEN))) { art_begin(t->value->int32); return; }
  if ((t = dict_find(iter, MESSAGE_KEY_ART_DATA))) {
    Tuple *o = dict_find(iter, MESSAGE_KEY_ART_OFFSET);
    art_chunk(o ? (int) o->value->int32 : 0, t->value->data, t->length);
    return;
  }
  if ((t = dict_find(iter, MESSAGE_KEY_VOLUME))) {
    /* -1 = the phone couldn't read/set it; else take it unless buttons own it */
    int v = t->value->int32;
    Tuple *m = dict_find(iter, MESSAGE_KEY_VOLUME_MAX);
    if (m && m->value->int32 > 0) s_vol_max = m->value->int32;
    if (v < 0) { s_vol_failed = true; s_vol = -1; s_vol_local = false; }
    else if (!s_vol_local) { s_vol_failed = false; s_vol = v; }
    if (s_volume_layer) layer_mark_dirty(s_volume_layer);
    return;
  }
  if ((t = dict_find(iter, MESSAGE_KEY_PROMPT_GENRE))) {
    /* the song has no genre: ask which Top folder to move it to */
    s_picker = PICK_TOP;
    push_genre_window();
    return;
  }

  if ((t = dict_find(iter, MESSAGE_KEY_REACHABLE))) s_reachable = t->value->int32;
  if ((t = dict_find(iter, MESSAGE_KEY_STATUS)))    s_status = t->value->int32;
  if ((t = dict_find(iter, MESSAGE_KEY_POSITION)))  s_position = t->value->int32;
  if ((t = dict_find(iter, MESSAGE_KEY_DURATION)))  s_duration = t->value->int32;
  if ((t = dict_find(iter, MESSAGE_KEY_QUEUE_SIZE))) s_queue_size = t->value->int32;
  if ((t = dict_find(iter, MESSAGE_KEY_IS_TOP)))    s_is_top = t->value->int32;
  if ((t = dict_find(iter, MESSAGE_KEY_IS_UNTAG)))  s_is_untag = t->value->int32;
  if ((t = dict_find(iter, MESSAGE_KEY_LOOP)))      s_loop = t->value->int32;
  if ((t = dict_find(iter, MESSAGE_KEY_GENRE_COLOR))) s_genre_color = t->value->int32;
  if ((t = dict_find(iter, MESSAGE_KEY_MODE)))      s_mode = t->value->int32;
  if ((t = dict_find(iter, MESSAGE_KEY_SOURCE)))    s_source = t->value->int32;
  if ((t = dict_find(iter, MESSAGE_KEY_MATCHED)))   s_matched = t->value->int32;
  if ((t = dict_find(iter, MESSAGE_KEY_TOP_PENDING))) s_top_pending = t->value->int32;
  copy_str(dict_find(iter, MESSAGE_KEY_ERROR), s_error, sizeof(s_error));

  copy_str(dict_find(iter, MESSAGE_KEY_TITLE), s_title, sizeof(s_title));
  copy_str(dict_find(iter, MESSAGE_KEY_CHANNEL), s_channel, sizeof(s_channel));
  copy_str(dict_find(iter, MESSAGE_KEY_GENRE), s_genre, sizeof(s_genre));

  if ((t = dict_find(iter, MESSAGE_KEY_GENRES))) parse_genres(t->value->cstring);

  update_ui();
}

static void inbox_dropped(AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_WARNING, "AppMessage dropped: %d", (int) reason);
}

/* ------------------------------------------------------------------ */
/* Window                                                              */
/* ------------------------------------------------------------------ */
static TextLayer *make_label(Layer *parent, GRect frame, const char *font_key,
                             GColor color, GTextAlignment align) {
  TextLayer *tl = text_layer_create(frame);
  text_layer_set_background_color(tl, GColorClear);
  text_layer_set_text_color(tl, color);
  text_layer_set_font(tl, fonts_get_system_font(font_key));
  text_layer_set_text_alignment(tl, align);
  layer_add_child(parent, text_layer_get_layer(tl));
  return tl;
}

static Layer *make_drawn(Layer *parent, LayerUpdateProc proc) {
  Layer *l = layer_create(GRect(0, 0, 0, 0));
  layer_set_update_proc(l, proc);
  layer_add_child(parent, l);
  return l;
}

static void window_load(Window *window) {
  window_set_background_color(window, col_bg());
  Layer *root = window_get_root_layer(window);
  GRect b = layer_get_bounds(root);
  GTextAlignment text_align = PBL_IF_ROUND_ELSE(GTextAlignmentCenter, GTextAlignmentLeft);

  /* All content frames are (re)positioned by layout_now_playing(); the frames
   * passed here are placeholders. */
  s_header_layer = make_drawn(root, header_update_proc);

  s_title_layer = make_label(root, GRectZero, FONT_KEY_GOTHIC_24_BOLD,
                             col_title(), text_align);
  text_layer_set_overflow_mode(s_title_layer, GTextOverflowModeTrailingEllipsis);

  s_channel_layer = make_label(root, GRectZero, FONT_KEY_GOTHIC_18,
                               col_channel(), text_align);
  text_layer_set_overflow_mode(s_channel_layer, GTextOverflowModeTrailingEllipsis);

  s_chip_layer = make_drawn(root, chip_update_proc);
  s_art_layer = make_drawn(root, art_update_proc);
  s_progress_layer = make_drawn(root, progress_update_proc);

  /* time row: elapsed on the left, -remaining on the right (both span the
   * width with opposite alignment). Round: one centred "pos / dur". */
  s_time_layer = make_label(root, GRectZero, FONT_KEY_GOTHIC_14_BOLD, col_muted(),
                            PBL_IF_ROUND_ELSE(GTextAlignmentCenter, GTextAlignmentLeft));
  s_remain_layer = make_label(root, GRectZero, FONT_KEY_GOTHIC_14, col_muted(),
                              GTextAlignmentRight);

  /* action bar: full-height strip on the right edge */
  s_buttons_layer = make_drawn(root, buttons_update_proc);
  layer_set_frame(s_buttons_layer, GRect(b.size.w - AB_W, 0, AB_W, b.size.h));

  update_ui();   /* sets the texts, then runs layout_now_playing() */
}

static void window_unload(Window *window) {
  layer_destroy(s_header_layer);
  text_layer_destroy(s_title_layer);
  text_layer_destroy(s_channel_layer);
  layer_destroy(s_chip_layer);
  layer_destroy(s_progress_layer);
  text_layer_destroy(s_time_layer);
  text_layer_destroy(s_remain_layer);
  layer_destroy(s_buttons_layer);
  layer_destroy(s_art_layer);
  s_art_layer = NULL;
  s_header_layer = NULL;
  s_title_layer = NULL;
}

/* Fires when the now-playing screen becomes visible again (incl. returning from
 * the settings menu). The SELECT release that would clear the held-gear icon
 * goes to the menu window instead, so reset it here. */
static void main_window_appear(Window *window) {
  s_hold = 0;
  if (s_buttons_layer) layer_mark_dirty(s_buttons_layer);
}

static void init(void) {
  /* Restore the saved display mode (defaults to 2 = night on first run). */
  if (persist_exists(PERSIST_KEY_THEME)) s_theme = persist_read_int(PERSIST_KEY_THEME);

  s_window = window_create();
  window_set_click_config_provider(s_window, click_config_provider);
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = window_load,
    .unload = window_unload,
    .appear = main_window_appear,
  });

  app_message_register_inbox_received(inbox_received);
  app_message_register_inbox_dropped(inbox_dropped);
  /* Inbox must hold TITLE(128)+CHANNEL(64)+GENRES+ERROR + several ints + dict
   * overhead, and an album-art chunk (ART_CHUNK bytes in index.js + offset);
   * 1536 is comfortable headroom for both. */
  app_message_open(1536, 256);

  window_stack_push(s_window, true);
  tick_timer_service_subscribe(MINUTE_UNIT, minute_tick);   /* header clock */
  /* No 1s timer here: startup is idle. schedule_tick() (via update_ui on each
   * poll) arms the interpolation only once playback is actually reported. */
  request_refresh();
}

static void deinit(void) {
  tick_timer_service_unsubscribe();
  if (s_tick_timer) app_timer_cancel(s_tick_timer);
  if (s_vol_timer) app_timer_cancel(s_vol_timer);
  if (s_volume_window) window_destroy(s_volume_window);
  if (s_genre_window) window_destroy(s_genre_window);
  if (s_settings_window) window_destroy(s_settings_window);
  window_destroy(s_window);
  art_reset_buffer();
  if (s_art_bitmap) gbitmap_destroy(s_art_bitmap);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
