/*
 * Instrumentalls remote — Pebble watch app.
 *
 * A colour-coded now-playing screen that mirrors the discovery-queue Rich TUI
 * (title cyan, channel magenta, status green/yellow, genre colour-coded, TOP
 * yellow, UNTAG red), plus a themeable MenuLayer for the tag/settings actions.
 * All state arrives over AppMessage from the PebbleKit JS bridge; buttons and
 * menu selections are sent back as CMD tokens.
 *
 * Buttons:  UP   = previous       UP   (long) = mark Top (toggle)
 *           DOWN = skip (remove)  DOWN (long) = mark Untagged (toggle)
 *           SELECT = play/pause   SELECT (long) = open settings menu
 *           BACK = exit
 * While a button is held past the long-press threshold, its right-edge hint
 * icon swaps to the hold action: yellow star (Top), red flag (Untag), gear
 * (settings).
 */

#include <pebble.h>

#define MAX_GENRES 8
#define GENRE_NAME_LEN 24

#define PERSIST_KEY_THEME 1   /* display mode: 0 auto, 1 day, 2 night */

/* Now-playing layout: one evenly-spaced stack, vertically centred in the
 * screen. Row heights are fixed; the badge row is only allotted space when a
 * TOP/UNTAG badge is actually set, so an untagged track has no gap in the
 * middle. Centres via (h - total)/2, clamped to a minimum top so it never
 * clips the top on the shorter B&W platforms. */
#define ROW_TITLE_H    48
#define ROW_CHAN_H     22
#define ROW_STATUS_H   24
#define ROW_GENRE_H    24
#define ROW_BADGE_H    20
#define ROW_BAR_H      12
#define ROW_TQ_H       18
#define ROW_GAP         6
#define LAYOUT_TOP_MIN  4

static Window *s_window;
static TextLayer *s_title_layer;
static TextLayer *s_channel_layer;
static TextLayer *s_status_layer;
static TextLayer *s_genre_layer;
static Layer *s_badges_layer;     /* custom-drawn: ★ TOP and/or ⚑ UNTAG, one line */
static TextLayer *s_time_layer;
static TextLayer *s_queue_layer;
static Layer *s_progress_layer;
static Layer *s_buttons_layer;    /* right-edge button-hint icons */

static Window *s_settings_window;    /* settings list (MenuLayer) */
static MenuLayer *s_settings_menu;
static Window *s_genre_window;       /* "Save to genre" picker sub-screen */
static MenuLayer *s_genre_menu;
static AppTimer *s_tick_timer;

/* Which button is currently held past the long-press threshold, so the
 * right-edge hint icon can swap to that button's hold action:
 *   0 none   1 UP (Top)   2 SELECT (settings)   3 DOWN (Untag). */
static int s_hold = 0;

/* Absolute Y of each button's related content, set by layout_now_playing() and
 * read by buttons_update_proc so the right-edge hints line up with what they
 * drive (prev↔title, play/pause↔status, skip↔transport). */
static int s_up_y = 0, s_mid_y = 0, s_down_y = 0;

/* --- state (kept in sync by the AppMessage inbox) --- */
static char s_title[128] = "Instrumentalls";
static char s_channel[64] = "";
static char s_genre[GENRE_NAME_LEN] = "";
static char s_time_buf[32] = "";
static char s_queue_buf[24] = "";

static int s_status = 0;       /* 0 idle  1 playing  2 paused  3 buffering */
static int s_reachable = 0;    /* 0 = bridge not reachable */
static int s_position = 0;     /* seconds */
static int s_duration = 0;     /* seconds */
static int s_queue_size = 0;
static int s_is_top = 0;
static int s_is_untag = 0;
static int s_loop = 0;
static int s_genre_color = 0;  /* colour code, see genre_gcolor() */

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
 * background (white on black at night, black on white by day). */

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
static GColor col_muted(void) {   /* time/queue text, progress frame, gear */
#ifdef PBL_COLOR
  return theme_is_night() ? GColorLightGray : GColorDarkGray;
#else
  return col_fg();
#endif
}

/* Fixed accents, day/night pairs. */
static GColor col_title(void)   { return PBL_IF_COLOR_ELSE(theme_is_night() ? GColorCyan    : GColorDukeBlue,       col_fg()); }
static GColor col_channel(void) { return PBL_IF_COLOR_ELSE(theme_is_night() ? GColorMagenta : GColorImperialPurple, col_fg()); }
static GColor col_top(void)     { return PBL_IF_COLOR_ELSE(theme_is_night() ? GColorYellow  : GColorWindsorTan,     col_fg()); }
static GColor col_untag(void)   { return PBL_IF_COLOR_ELSE(theme_is_night() ? GColorRed     : GColorDarkCandyAppleRed, col_fg()); }
static GColor col_nav(void)     { return PBL_IF_COLOR_ELSE(theme_is_night() ? GColorCyan    : GColorDukeBlue,       col_fg()); }

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

/* Accent colour for the status text + progress bar: mirrors the panel's
 * green(playing) / yellow(paused|buffering) border, red when unreachable. */
static GColor status_color(void) {
#ifdef PBL_COLOR
  bool night = theme_is_night();
  if (!s_reachable) return night ? GColorRed : GColorDarkCandyAppleRed;
  switch (s_status) {
    case 1: return night ? GColorGreen  : GColorDarkGreen;    /* playing   */
    case 2: return night ? GColorYellow : GColorWindsorTan;   /* paused    */
    case 3: return night ? GColorYellow : GColorWindsorTan;   /* buffering */
    default: return col_muted();                              /* idle      */
  }
#else
  return col_fg();
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
static const GPathInfo STAR_INFO = {
  .num_points = 10,
  .points = (GPoint[]) {
    {0,-9},{2,-3},{9,-3},{3,2},{6,8},{0,4},{-6,8},{-3,2},{-9,-3},{-2,-3}
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

static void draw_star(GContext *ctx, int cx, int cy, GColor color) {
  graphics_context_set_fill_color(ctx, color);
  GPath *p = gpath_create(&STAR_INFO);
  gpath_move_to(p, GPoint(cx, cy));
  gpath_draw_filled(ctx, p);
  gpath_destroy(p);
}

/* Skip-forward icon (▶|): a right triangle butted against a vertical bar —
 * the DOWN-tap hint, now "skip (remove)" rather than plain fast-forward. */
static void draw_skip(GContext *ctx, int cx, int cy, GColor color) {
  graphics_context_set_fill_color(ctx, color);
  tri_right(ctx, cx - 2, cy, 4);
  graphics_fill_rect(ctx, GRect(cx + 3, cy - 4, 2, 8), 0, GCornerNone);
}

/* Pennant flag: a vertical pole with a triangular flag near the top — the
 * DOWN-hold hint (mark Untagged), mirrors the 🚩 marker in the Rich TUI. */
static void draw_flag(GContext *ctx, int cx, int cy, GColor color) {
  graphics_context_set_fill_color(ctx, color);
  graphics_fill_rect(ctx, GRect(cx - 4, cy - 7, 2, 14), 0, GCornerNone);   /* pole */
  fill_tri(ctx, GPoint(cx - 2, cy - 7), GPoint(cx - 2, cy - 1), GPoint(cx + 6, cy - 4));
}

/* Gear: a filled hub with four teeth and a punched-out centre — the
 * SELECT-hold hint (open settings menu). */
static void draw_gear(GContext *ctx, int cx, int cy, GColor color) {
  graphics_context_set_fill_color(ctx, color);
  int r = 5;
  graphics_fill_rect(ctx, GRect(cx - 1, cy - r - 2, 3, 3), 0, GCornerNone);  /* top */
  graphics_fill_rect(ctx, GRect(cx - 1, cy + r - 1, 3, 3), 0, GCornerNone);  /* bottom */
  graphics_fill_rect(ctx, GRect(cx - r - 2, cy - 1, 3, 3), 0, GCornerNone);  /* left */
  graphics_fill_rect(ctx, GRect(cx + r - 1, cy - 1, 3, 3), 0, GCornerNone);  /* right */
  graphics_fill_circle(ctx, GPoint(cx, cy), r);
  graphics_context_set_fill_color(ctx, col_bg());
  graphics_fill_circle(ctx, GPoint(cx, cy), 2);                             /* hole */
}

/* Badge row: ★ TOP and/or ⚑ UNTAG, drawn as one centred group on a single line
 * (mirrors the ⭐/🚩 markers in the Rich TUI — star yellow for Top, pennant red
 * for Untag). Only the set badges show; layout_now_playing() collapses the row
 * entirely when neither is set. */
#define BADGE_STAR_W 18   /* star footprint (STAR_INFO spans ±9)   */
#define BADGE_FLAG_W 14   /* pennant footprint                     */
#define BADGE_IT_GAP  3   /* icon → its own label                  */
#define BADGE_GAP    12   /* between the two badges                */

/* One badge = icon + label; draws at x (left edge), returns its width. When
 * measure_only is true it draws nothing and just reports the width. */
static int draw_badge(GContext *ctx, const Layer *layer, int x, bool is_flag,
                      const char *label, GColor color, bool measure_only) {
  GRect b = layer_get_bounds(layer);
  GFont f = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
  GSize ts = graphics_text_layout_get_content_size(
      label, f, GRect(0, 0, 100, 22),
      GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft);
  int icon_w = is_flag ? BADGE_FLAG_W : BADGE_STAR_W;
  if (!measure_only) {
    if (is_flag) draw_flag(ctx, x + icon_w / 2, b.size.h / 2, color);
    else         draw_star(ctx, x + icon_w / 2, b.size.h / 2, color);
    graphics_context_set_text_color(ctx, color);
    graphics_draw_text(ctx, label, f,
        GRect(x + icon_w + BADGE_IT_GAP, (b.size.h - ts.h) / 2 - 3, ts.w + 6, ts.h + 6),
        GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  }
  return icon_w + BADGE_IT_GAP + ts.w;
}

static void badges_update_proc(Layer *layer, GContext *ctx) {
  if (!s_is_top && !s_is_untag) return;
  GRect b = layer_get_bounds(layer);

  /* measure first so the whole group can be centred on one line */
  int top_w = s_is_top   ? draw_badge(ctx, layer, 0, false, "TOP",   col_top(),   true) : 0;
  int unt_w = s_is_untag ? draw_badge(ctx, layer, 0, true,  "UNTAG", col_untag(), true) : 0;
  int group_w = top_w + unt_w + ((s_is_top && s_is_untag) ? BADGE_GAP : 0);

  int x = (b.size.w - group_w) / 2;
  if (x < 0) x = 0;

  if (s_is_top)   x += draw_badge(ctx, layer, x, false, "TOP",   col_top(),   false) + BADGE_GAP;
  if (s_is_untag)      draw_badge(ctx, layer, x, true,  "UNTAG", col_untag(), false);
}

/* Right-edge hints aligned with the physical buttons. Each shows its tap
 * action, and swaps to its hold action while that button is held:
 *   UP:     previous (◀◀)      → Top (yellow ★)   when held
 *   SELECT: play/pause         → settings (gear)  when held
 *   DOWN:   skip/remove (▶|)   → Untag (red flag) when held */
static void buttons_update_proc(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  int cx = b.size.w / 2;
  /* Anchored to the related content (set by layout_now_playing); fall back to
   * even thirds before the first layout pass. */
  int up_y   = s_up_y   ? s_up_y   : b.size.h * 22 / 100;
  int mid_y  = s_mid_y  ? s_mid_y  : b.size.h / 2;
  int down_y = s_down_y ? s_down_y : b.size.h * 78 / 100;
  int hs = 4;

  GColor nav = col_nav();

  /* UP: previous (◀◀), or Top (★) while held */
  if (s_hold == 1) {
    draw_star(ctx, cx, up_y, col_top());
  } else {
    graphics_context_set_fill_color(ctx, nav);
    tri_left(ctx, cx - 3, up_y, hs);
    tri_left(ctx, cx + 4, up_y, hs);
  }

  /* SELECT: play/pause, or settings (gear) while held */
  if (s_hold == 2) {
    draw_gear(ctx, cx, mid_y, col_muted());
  } else {
    graphics_context_set_fill_color(ctx, status_color());
    if (s_status == 1) {                /* playing → show pause hint (❚❚) */
      graphics_fill_rect(ctx, GRect(cx - 4, mid_y - 5, 3, 10), 0, GCornerNone);
      graphics_fill_rect(ctx, GRect(cx + 1, mid_y - 5, 3, 10), 0, GCornerNone);
    } else {                            /* otherwise show play hint (▶) */
      tri_right(ctx, cx, mid_y, 5);
    }
  }

  /* DOWN: skip/remove (▶|), or Untag (red flag) while held */
  if (s_hold == 3) {
    draw_flag(ctx, cx, down_y, col_untag());
  } else {
    draw_skip(ctx, cx, down_y, nav);
  }
}

/* ------------------------------------------------------------------ */
/* UI                                                                  */
/* ------------------------------------------------------------------ */
static void progress_update_proc(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);

  /* Unplayed track: dark fill + a thin frame so the bar reads as a defined
   * track even when empty — on b&w the fill matches the window bg, so the
   * frame is what makes the empty portion visible at all. */
  graphics_context_set_fill_color(ctx,
      PBL_IF_COLOR_ELSE(theme_is_night() ? GColorDarkGray : GColorLightGray, col_bg()));
  graphics_fill_rect(ctx, b, 0, GCornerNone);
  graphics_context_set_stroke_color(ctx, col_muted());
  graphics_draw_rect(ctx, b);

  if (s_duration > 0 && s_position >= 0) {
    int w = (b.size.w * s_position) / s_duration;
    if (w > b.size.w) w = b.size.w;
    if (w < 0) w = 0;
    /* Played section: bright accent fill (green/yellow/red per status). */
    graphics_context_set_fill_color(ctx, status_color());
    graphics_fill_rect(ctx, GRect(b.origin.x, b.origin.y, w, b.size.h), 0, GCornerNone);
    /* Playhead: a bright vertical marker at the boundary gives a clean,
     * high-contrast read of exactly how far the track has played. */
    if (w > 0 && w < b.size.w) {
      graphics_context_set_stroke_color(ctx, col_fg());
      graphics_draw_line(ctx, GPoint(b.origin.x + w, b.origin.y),
                         GPoint(b.origin.x + w, b.origin.y + b.size.h - 1));
    }
  }
}

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

/* Position the now-playing content as one evenly-spaced stack, vertically
 * centred in the window. The badge row is only allotted space when a TOP or
 * UNTAG badge is set; otherwise the bar/transport move up to close the gap.
 * Also records the button-hint anchors so the right-edge icons line up with
 * the content they drive. Safe to call on every refresh. */
static void layout_now_playing(void) {
  if (!s_window || !s_title_layer) return;
  GRect b = layer_get_bounds(window_get_root_layer(s_window));
  int w = b.size.w, h = b.size.h;
  int m = 4, rp = 18, cw = w - m - rp;
  bool badges = (s_is_top || s_is_untag);

  int total = ROW_TITLE_H + ROW_CHAN_H + ROW_STATUS_H + ROW_GENRE_H
              + ROW_BAR_H + ROW_TQ_H + ROW_GAP * 5;
  if (badges) total += ROW_BADGE_H + ROW_GAP;

  int y = (h - total) / 2;
  if (y < LAYOUT_TOP_MIN) y = LAYOUT_TOP_MIN;

  layer_set_frame(text_layer_get_layer(s_title_layer),   GRect(m, y, cw, ROW_TITLE_H));
  int title_y = y;  y += ROW_TITLE_H + ROW_GAP;

  layer_set_frame(text_layer_get_layer(s_channel_layer), GRect(m, y, cw, ROW_CHAN_H));
  y += ROW_CHAN_H + ROW_GAP;

  layer_set_frame(text_layer_get_layer(s_status_layer),  GRect(m, y, cw, ROW_STATUS_H));
  int status_y = y;  y += ROW_STATUS_H + ROW_GAP;

  layer_set_frame(text_layer_get_layer(s_genre_layer),   GRect(m, y, cw, ROW_GENRE_H));
  y += ROW_GENRE_H + ROW_GAP;

  if (badges) {
    layer_set_frame(s_badges_layer, GRect(m, y, cw, ROW_BADGE_H));
    y += ROW_BADGE_H + ROW_GAP;
  } else {
    /* collapse: zero-height, so nothing draws and no space is reserved */
    layer_set_frame(s_badges_layer, GRect(m, y, cw, 0));
  }

  layer_set_frame(s_progress_layer, GRect(m, y, cw, ROW_BAR_H));
  y += ROW_BAR_H + ROW_GAP;

  /* time + queue share one baseline: both span the width, opposite alignment */
  int tq_y = y;
  layer_set_frame(text_layer_get_layer(s_time_layer),  GRect(m, tq_y, cw, ROW_TQ_H));
  layer_set_frame(text_layer_get_layer(s_queue_layer), GRect(m, tq_y, cw, ROW_TQ_H));

  s_up_y   = title_y  + ROW_TITLE_H  / 2;
  s_mid_y  = status_y + ROW_STATUS_H / 2;
  s_down_y = tq_y     + ROW_TQ_H     / 2;

  if (s_buttons_layer) layer_mark_dirty(s_buttons_layer);
}

static void update_ui(void) {
  layout_now_playing();   /* reflow first: the badge row may have just appeared/vanished */

  /* Re-apply theme colours every refresh so Auto mode can flip live if the app
   * stays open across the day/night boundary. status + genre colours are set
   * below where their dynamic state is known. */
  if (s_window) window_set_background_color(s_window, col_bg());
  text_layer_set_text_color(s_title_layer, col_title());
  text_layer_set_text_color(s_channel_layer, col_channel());
  text_layer_set_text_color(s_time_layer, col_muted());
  text_layer_set_text_color(s_queue_layer, col_muted());

  text_layer_set_text(s_title_layer, s_title);

  text_layer_set_text(s_channel_layer, s_channel);

  /* status line */
  const char *status_text;
  if (!s_reachable) {
    status_text = "OFFLINE — start queue";
  } else {
    switch (s_status) {
      case 1: status_text = s_loop ? "PLAYING  (loop)" : "PLAYING"; break;
      case 2: status_text = "PAUSED"; break;
      case 3: status_text = "BUFFERING…"; break;
      default: status_text = "IDLE"; break;
    }
  }
  text_layer_set_text(s_status_layer, status_text);
  text_layer_set_text_color(s_status_layer, status_color());

  /* saved genre chip */
  if (s_genre[0] != '\0') {
    text_layer_set_text(s_genre_layer, s_genre);
    text_layer_set_text_color(s_genre_layer, genre_gcolor(s_genre_color));
  } else {
    text_layer_set_text(s_genre_layer, "not saved");
    text_layer_set_text_color(s_genre_layer, col_muted());
  }

  /* badges: ★ TOP / ⚑ UNTAG are custom-drawn together on one line */
  layer_mark_dirty(s_badges_layer);

  /* time + queue ("MMM:SS" fits in 8 incl. NUL, even for long tracks) */
  char pos[8], dur[8];
  format_mmss(s_position, pos, sizeof(pos));
  format_mmss(s_duration, dur, sizeof(dur));
  snprintf(s_time_buf, sizeof(s_time_buf), "%s / %s", pos, dur);
  text_layer_set_text(s_time_layer, s_time_buf);

  snprintf(s_queue_buf, sizeof(s_queue_buf), "Queue: %d", s_queue_size);
  text_layer_set_text(s_queue_layer, s_queue_buf);

  layer_mark_dirty(s_progress_layer);
  layer_mark_dirty(s_buttons_layer);   /* play/pause hint follows status */

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
  send_cmd_arg("genre", idx->row + 1);
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
  menu_layer_set_click_config_onto_window(s_genre_menu, w);
  layer_add_child(root, menu_layer_get_layer(s_genre_menu));
}
static void genre_window_unload(Window *w) {
  menu_layer_destroy(s_genre_menu);
  s_genre_menu = NULL;
}
static void push_genre_window(void) {
  if (!s_genre_window) {
    s_genre_window = window_create();
    window_set_window_handlers(s_genre_window, (WindowHandlers) {
      .load = genre_window_load,
      .unload = genre_window_unload,
    });
  }
  window_stack_push(s_genre_window, true);
}

/* ---- main settings list ---- */
enum {
  SROW_GENRE = 0,   /* -> genre picker sub-screen  */
  SROW_UNSAVE,
  SROW_LOOP,
  SROW_SHUFFLE,
  SROW_DISPLAY,     /* cycles Auto -> Day -> Night in place */
  SROW_SKIP,
  SROW_TOP,
  SROW_UNTAG,
  SROW_COUNT
};

static uint16_t settings_num_rows(MenuLayer *ml, uint16_t section, void *ctx) {
  return SROW_COUNT;
}
static void settings_draw_row(GContext *ctx, const Layer *cell, MenuIndex *idx, void *cb_ctx) {
  static const char *mode_names[] = { "Auto", "Day", "Night" };
  char buf[24];
  const char *text = "";
  switch (idx->row) {
    case SROW_GENRE:   text = "Save to genre"; break;
    case SROW_UNSAVE:  text = "Unsave"; break;
    case SROW_LOOP:    text = "Loop on/off"; break;
    case SROW_SHUFFLE: text = "Shuffle"; break;
    case SROW_DISPLAY:
      snprintf(buf, sizeof(buf), "Display: %s",
               mode_names[(s_theme >= 0 && s_theme <= 2) ? s_theme : 0]);
      text = buf; break;
    case SROW_SKIP:    text = "Skip (remove)"; break;
    case SROW_TOP:     text = "Mark Top"; break;
    case SROW_UNTAG:   text = "Mark Untagged"; break;
  }
  draw_menu_row(ctx, cell, text);
}
static void settings_select(MenuLayer *ml, MenuIndex *idx, void *cb_ctx) {
  switch (idx->row) {
    case SROW_GENRE:   push_genre_window(); return;   /* keep settings open */
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
    case SROW_SKIP:    send_cmd("skip"); break;
    case SROW_TOP:     send_cmd("top"); break;
    case SROW_UNTAG:   send_cmd("untag"); break;
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
/* Buttons                                                             */
/* ------------------------------------------------------------------ */
/* Taps */
static void up_click(ClickRecognizerRef r, void *ctx)     { send_cmd("prev"); }
static void down_click(ClickRecognizerRef r, void *ctx)   { send_cmd("skip"); }
static void select_click(ClickRecognizerRef r, void *ctx) { send_cmd("playpause"); }

/* Holds: the "down" handler fires once the long-press threshold is reached —
 * light up the button's hold-action hint icon, then perform the action; the
 * shared "up" handler clears the hint on release. */
static void up_long_down(ClickRecognizerRef r, void *ctx) {
  s_hold = 1;
  if (s_buttons_layer) layer_mark_dirty(s_buttons_layer);
  send_cmd("top");
}
static void down_long_down(ClickRecognizerRef r, void *ctx) {
  s_hold = 3;
  if (s_buttons_layer) layer_mark_dirty(s_buttons_layer);
  send_cmd("untag");
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

static void inbox_received(DictionaryIterator *iter, void *context) {
  Tuple *t;
  if ((t = dict_find(iter, MESSAGE_KEY_REACHABLE))) s_reachable = t->value->int32;
  if ((t = dict_find(iter, MESSAGE_KEY_STATUS)))    s_status = t->value->int32;
  if ((t = dict_find(iter, MESSAGE_KEY_POSITION)))  s_position = t->value->int32;
  if ((t = dict_find(iter, MESSAGE_KEY_DURATION)))  s_duration = t->value->int32;
  if ((t = dict_find(iter, MESSAGE_KEY_QUEUE_SIZE))) s_queue_size = t->value->int32;
  if ((t = dict_find(iter, MESSAGE_KEY_IS_TOP)))    s_is_top = t->value->int32;
  if ((t = dict_find(iter, MESSAGE_KEY_IS_UNTAG)))  s_is_untag = t->value->int32;
  if ((t = dict_find(iter, MESSAGE_KEY_LOOP)))      s_loop = t->value->int32;
  if ((t = dict_find(iter, MESSAGE_KEY_GENRE_COLOR))) s_genre_color = t->value->int32;

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

static void window_load(Window *window) {
  window_set_background_color(window, col_bg());
  Layer *root = window_get_root_layer(window);
  GRect b = layer_get_bounds(root);
  int w = b.size.w;
  int h = b.size.h;
  int m = 4;              /* left margin */
  int rp = 18;            /* right edge reserved for the button-hint icons */
  int cw = w - m - rp;    /* content width (leaves the right column free) */

  /* All content frames are (re)positioned by layout_now_playing(); the frames
   * passed here are placeholders (real Y depends on badge state + height). */
  s_title_layer = make_label(root, GRect(m, 0, cw, ROW_TITLE_H),
                             FONT_KEY_GOTHIC_24_BOLD,
                             col_title(), GTextAlignmentCenter);
  text_layer_set_text(s_title_layer, s_title);
  text_layer_set_overflow_mode(s_title_layer, GTextOverflowModeTrailingEllipsis);

  s_channel_layer = make_label(root, GRect(m, 0, cw, ROW_CHAN_H),
                               FONT_KEY_GOTHIC_18,
                               col_channel(), GTextAlignmentCenter);
  text_layer_set_text(s_channel_layer, s_channel);

  s_status_layer = make_label(root, GRect(m, 0, cw, ROW_STATUS_H),
                              FONT_KEY_GOTHIC_18_BOLD,
                              status_color(), GTextAlignmentCenter);

  s_genre_layer = make_label(root, GRect(m, 0, cw, ROW_GENRE_H),
                             FONT_KEY_GOTHIC_18_BOLD, col_fg(), GTextAlignmentCenter);

  /* badge row: ★ TOP and/or ⚑ UNTAG, drawn as one centred group on one line */
  s_badges_layer = layer_create(GRect(m, 0, cw, ROW_BADGE_H));
  layer_set_update_proc(s_badges_layer, badges_update_proc);
  layer_add_child(root, s_badges_layer);

  s_progress_layer = layer_create(GRect(m, 0, cw, ROW_BAR_H));
  layer_set_update_proc(s_progress_layer, progress_update_proc);
  layer_add_child(root, s_progress_layer);

  /* time + queue share one baseline row: bold time on the left, queue on the
   * right. Both span the full width with opposite alignment, so the two short
   * strings sit at either end with no layer in the middle. */
  s_time_layer = make_label(root, GRect(m, 0, cw, ROW_TQ_H),
                            FONT_KEY_GOTHIC_14_BOLD,
                            col_muted(), GTextAlignmentLeft);
  s_queue_layer = make_label(root, GRect(m, 0, cw, ROW_TQ_H),
                             FONT_KEY_GOTHIC_14,
                             col_muted(), GTextAlignmentRight);

  /* right-edge button hints (span full height; each icon anchored to its
   * content by layout_now_playing) */
  s_buttons_layer = layer_create(GRect(w - rp, 0, rp, h));
  layer_set_update_proc(s_buttons_layer, buttons_update_proc);
  layer_add_child(root, s_buttons_layer);

  update_ui();   /* runs layout_now_playing() first, then paints */
}

static void window_unload(Window *window) {
  text_layer_destroy(s_title_layer);
  text_layer_destroy(s_channel_layer);
  text_layer_destroy(s_status_layer);
  text_layer_destroy(s_genre_layer);
  layer_destroy(s_badges_layer);
  text_layer_destroy(s_time_layer);
  text_layer_destroy(s_queue_layer);
  layer_destroy(s_progress_layer);
  layer_destroy(s_buttons_layer);
}

/* Fires when the now-playing screen becomes visible again (incl. returning from
 * the settings menu). The SELECT release that would clear the held-gear hint
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
  /* Inbox must hold TITLE(128)+CHANNEL(64)+GENRES + several ints + dict
   * overhead; 1024 is comfortable headroom. */
  app_message_open(1024, 256);

  window_stack_push(s_window, true);
  /* No timer here: startup is idle. schedule_tick() (via update_ui on each
   * poll) arms the 1s interpolation only once playback is actually reported. */
  request_refresh();
}

static void deinit(void) {
  if (s_tick_timer) app_timer_cancel(s_tick_timer);
  if (s_genre_window) window_destroy(s_genre_window);
  if (s_settings_window) window_destroy(s_settings_window);
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
