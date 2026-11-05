// Vesta -- a split-flap (Vestaboard style) watchface for Pebble Time.
//
// The board is 4 rows x 8 flaps. Every flap is a drum that can only turn
// forward through the fixed Vestaboard character order (see FLAP_KEYS in
// glyphs.h), so changing a value means flipping through every character in
// between. Glyphs are hand-drawn pixel art compiled from font/glyphs.txt.
//
// To save battery the step counter uses a single flip per changed flap, and
// eco mode stops animations entirely so new values simply appear. Eco can be
// always on, always off, or "smart" (on after ECO_TIMEOUT_MS without activity).
//
// Rows, starting display, eco mode and theme are set from the hosted settings
// page, reaching the watch through src/pkjs/index.js as one AppMessage.
#include <pebble.h>
#include "glyphs.h"

#define COLS 8
#define ROWS 4

// Tile geometry (pixels). The interior is the dark flap face inside the bezel.
#define TILE_W 15
#define TILE_H 31
#define GAP_X 3
#define GAP_Y 10
#define IN_W (TILE_W - 2)
#define IN_H (TILE_H - 2)
#define SPLIT (IN_H / 2)         // interior row of the split line
#define GLYPH_TOP (SPLIT - 8)    // glyph row 8 sits on the split
#define FLAP_EDGE 9              // interior row of the falling flap's leading edge
#define FLAP_EDGE_LOW (2 * SPLIT - FLAP_EDGE)   // the same edge, past horizontal
#define BOARD_W (COLS * TILE_W + (COLS - 1) * GAP_X)
#define BOARD_H (ROWS * TILE_H + (ROWS - 1) * GAP_Y)

// 30 fps; each flap step is two mid-flip frames plus one frame at rest,
// i.e. 10 characters per second like the real board.
#define FRAME_MS 33
#define FRAMES_PER_STEP 3

#define STARTUP_HOLD_MS 500               // rainbow stays up this long before flipping
#define ECO_TIMEOUT_MS (3 * 60 * 1000)    // no user activity for this long enters eco mode

#define WEATHER_INTERVAL_S (10 * 60)   // weather is checked at most this often

#define SETTINGS_KEY 1
#define WEATHER_KEY 2

#define KEY_HEART '*'
#define RAINBOW_KEYS "roygbv"
#define RAINBOW_LEN ((int)sizeof(RAINBOW_KEYS) - 1)

// Stored in settings and sent by the config page, so the values must not change.
typedef enum {
  ROW_TIME, ROW_DATE, ROW_BATTERY, ROW_STEPS, ROW_WEEKDAY, ROW_CUSTOM, ROW_WEATHER,
  ROW_MODE_COUNT
} RowMode;

typedef enum { ECO_OFF, ECO_SMART, ECO_ON, ECO_MODE_COUNT } EcoMode;
typedef enum { THEME_DARK, THEME_LIGHT, THEME_COUNT } Theme;
typedef enum { FLIP_NONE, FLIP_TOP, FLIP_BOTTOM } FlipStage;

typedef struct {
  uint8_t start[ROWS][COLS];    // flap index shown when the face opens
  uint8_t custom[ROWS][COLS];   // flap indexes for rows set to ROW_CUSTOM
  uint8_t row_modes[ROWS];
  uint8_t eco_mode;
  uint8_t theme;
} Settings;

typedef struct {
  time_t checked;               // last time the phone was asked for weather
  char text[COLS + 1];
} WeatherState;

typedef struct {
  uint8_t bg;         // background, flap face and split line
  uint8_t bezel;      // tile border and falling flap edge
  uint8_t ink;        // characters and the last (filled) panel
  uint8_t aa;         // anti-alias pixels of characters
  uint8_t heart_aa;   // anti-alias pixels of the heart
} Palette;

static const Palette PALETTES[THEME_COUNT] = {
  [THEME_DARK] = { GColorBlackARGB8, GColorDarkGrayARGB8, GColorWhiteARGB8,
                   GColorLightGrayARGB8, GColorDarkCandyAppleRedARGB8 },
  [THEME_LIGHT] = { GColorWhiteARGB8, GColorLightGrayARGB8, GColorBlackARGB8,
                    GColorDarkGrayARGB8, GColorSunsetOrangeARGB8 },
};

typedef struct {
  uint8_t cur;      // flap index on display
  uint8_t target;   // flap index to stop at
  uint8_t laps;     // full extra revolutions still owed (wrist flick)
  uint8_t delay;    // flap steps to wait before moving (wrist-flick wave)
  uint8_t flip;     // FlipStage: which half of the flip to the next flap is shown
  bool direct;      // next flip lands straight on target instead of cur + 1
} Cell;

static Window *s_window;
static Layer *s_board_layer;
static AppTimer *s_timer;
static AppTimer *s_eco_timer;
static bool s_eco;
static uint8_t s_phase;
static Cell s_cells[ROWS][COLS];
static uint8_t s_heart_flap;
static Settings s_settings;
static WeatherState s_weather;
static const Palette *s_palette = &PALETTES[THEME_DARK];
static bool s_js_ready;

// The last panel is always the opposite of blank: white on dark, black on light.
static const uint8_t CHIP_COLORS[] = {
  GColorRedARGB8, GColorOrangeARGB8, GColorYellowARGB8, GColorMayGreenARGB8,
  GColorCobaltBlueARGB8, GColorIndigoARGB8,
};
#define FILLED_FLAP (FLAP_COUNT - 1)
#define FIRST_CHIP (FILLED_FLAP - (int)ARRAY_LENGTH(CHIP_COLORS))

// ---------------------------------------------------------------------------
// Flap state

static uint8_t flap_for_key(char key) {
  const char *p = key ? strchr(FLAP_KEYS, key) : NULL;
  return p ? (uint8_t)(p - FLAP_KEYS) : 0;
}

static bool cell_moving(const Cell *c) {
  return c->cur != c->target || c->laps > 0;
}

static uint8_t cell_next(const Cell *c) {
  return c->direct ? c->target : (c->cur + 1) % FLAP_COUNT;
}

static void anim_step(void *context);

static void anim_start(void) {
  if (!s_timer) {
    s_phase = 0;
    s_timer = app_timer_register(FRAME_MS, anim_step, NULL);
  }
}

static void anim_step(void *context) {
  s_timer = NULL;
  uint8_t phase = s_phase;
  s_phase = (s_phase + 1) % FRAMES_PER_STEP;

  bool dirty = false, busy = false;
  for (int r = 0; r < ROWS; r++) {
    for (int col = 0; col < COLS; col++) {
      Cell *c = &s_cells[r][col];
      if (phase == 0 && cell_moving(c)) {
        if (c->delay > 0) {
          c->delay--;
        } else {
          c->flip = FLIP_TOP;
          dirty = true;
        }
      } else if (phase == 1 && c->flip != FLIP_NONE) {
        c->flip = FLIP_BOTTOM;
        dirty = true;
      } else if (phase == 2 && c->flip != FLIP_NONE) {
        c->flip = FLIP_NONE;
        bool direct = c->direct;
        c->cur = cell_next(c);
        c->direct = false;
        if (!direct && c->cur == c->target && c->laps > 0) {
          c->laps--;
        }
        dirty = true;
      }
      busy |= cell_moving(c) || c->flip != FLIP_NONE;
    }
  }
  if (dirty) {
    layer_mark_dirty(s_board_layer);
  }
  if (busy) {
    s_timer = app_timer_register(FRAME_MS, anim_step, NULL);
  }
}

// Point a row of flaps at new targets. With direct set, flaps at rest take one
// flip to the new character rather than turning through the drum; flaps already
// turning just carry on to the new target. In eco mode the change appears
// immediately.
static void set_row_flaps(int row, const uint8_t *flaps, bool direct) {
  bool changed = false;
  for (int col = 0; col < COLS; col++) {
    Cell *c = &s_cells[row][col];
    uint8_t flap = flaps[col];
    if (c->target == flap) {
      continue;
    }
    if (s_eco) {
      c->cur = flap;
    } else if (direct && !cell_moving(c)) {
      c->direct = true;
    }
    c->target = flap;
    changed = true;
  }
  if (!changed) {
    return;
  }
  if (s_eco) {
    layer_mark_dirty(s_board_layer);
  } else {
    anim_start();
  }
}

// Spell text on every row showing this mode; unused columns go blank.
static void set_mode_rows(RowMode mode, const char *text, bool direct) {
  uint8_t flaps[COLS];
  size_t len = strlen(text);
  for (int col = 0; col < COLS; col++) {
    flaps[col] = flap_for_key(col < (int)len ? text[col] : ' ');
  }
  for (int row = 0; row < ROWS; row++) {
    if (s_settings.row_modes[row] == mode) {
      set_row_flaps(row, flaps, direct);
    }
  }
}

// ---------------------------------------------------------------------------
// Eco mode

static void enter_eco(void *context) {
  s_eco_timer = NULL;
  s_eco = true;
  if (s_timer) {
    app_timer_cancel(s_timer);
    s_timer = NULL;
  }
  for (int r = 0; r < ROWS; r++) {
    for (int col = 0; col < COLS; col++) {
      Cell *c = &s_cells[r][col];
      c->cur = c->target;
      c->laps = c->delay = 0;
      c->flip = FLIP_NONE;
      c->direct = false;
    }
  }
  layer_mark_dirty(s_board_layer);
}

// Bring eco state in line with the setting, restarting the smart-mode countdown.
static void apply_eco_mode(void) {
  if (s_eco_timer) {
    app_timer_cancel(s_eco_timer);
    s_eco_timer = NULL;
  }
  if (s_settings.eco_mode == ECO_ON) {
    enter_eco(NULL);
    return;
  }
  s_eco = false;
  if (s_settings.eco_mode == ECO_SMART) {
    s_eco_timer = app_timer_register(ECO_TIMEOUT_MS, enter_eco, NULL);
  }
}

static void note_activity(void) {
  if (s_settings.eco_mode == ECO_SMART) {
    apply_eco_mode();
  }
}

// ---------------------------------------------------------------------------
// Rendering (direct to the 8-bit framebuffer)

// One shade toward the theme background, for ink in the falling flap's shadow.
static uint8_t dim_channel(uint8_t v) {
  if (s_settings.theme == THEME_LIGHT) {
    return v < 3 ? v + 1 : 3;
  }
  return v ? v - 1 : 0;
}

static uint8_t dim(uint8_t argb) {
  return 0xC0 | (dim_channel((argb >> 4) & 3) << 4) | (dim_channel((argb >> 2) & 3) << 2) |
         dim_channel(argb & 3);
}

// Paint one interior row of a flap face. src_row selects which row of the
// flap's artwork to show (lets the falling flap be foreshortened).
static void paint_face_row(uint8_t *px, uint8_t flap, int src_row, bool dimmed) {
  if (flap >= FIRST_CHIP) {
    uint8_t c = flap == FILLED_FLAP ? s_palette->ink : CHIP_COLORS[flap - FIRST_CHIP];
    memset(px, dimmed ? dim(c) : c, IN_W);
    return;
  }
  memset(px, s_palette->bg, IN_W);
  const Glyph *g = &GLYPHS[flap];
  int gy = src_row - GLYPH_TOP;
  if (g->width == 0 || gy < 0 || gy >= GLYPH_ROWS) {
    return;
  }
  uint32_t bits = g->rows[gy];
  if (!bits) {
    return;
  }
  bool heart = flap == s_heart_flap;
  uint8_t ink = heart ? GColorRedARGB8 : s_palette->ink;
  uint8_t aa = heart ? s_palette->heart_aa : s_palette->aa;
  if (dimmed) {
    ink = dim(ink);
    aa = dim(aa);
  }
  uint8_t *dst = px + (IN_W - g->width) / 2;
  for (int x = 0; x < g->width; x++, bits >>= 2) {
    switch (bits & 3) {
      case 1: dst[x] = aa; break;
      case 2: dst[x] = ink; break;
    }
  }
}

// First half of a flip: the flap falls from the top, carrying the current top
// half foreshortened and shading the next character revealed behind it.
static void paint_flip_top(uint8_t *px, const Cell *c, uint8_t next, int iy) {
  if (iy > SPLIT) {
    paint_face_row(px, c->cur, iy, false);
  } else if (iy < FLAP_EDGE) {
    paint_face_row(px, next, iy, true);
  } else if (iy == FLAP_EDGE) {
    memset(px, s_palette->bezel, IN_W);
  } else {
    int src = (iy - FLAP_EDGE - 1) * SPLIT / (SPLIT - FLAP_EDGE - 1);
    paint_face_row(px, c->cur, src, false);
  }
}

// Second half: the flap is past horizontal, bringing the next bottom half down
// foreshortened over the current bottom half, which is now in its shadow.
static void paint_flip_bottom(uint8_t *px, const Cell *c, uint8_t next, int iy) {
  if (iy < SPLIT) {
    paint_face_row(px, next, iy, false);
  } else if (iy > FLAP_EDGE_LOW) {
    paint_face_row(px, c->cur, iy, true);
  } else if (iy == FLAP_EDGE_LOW) {
    memset(px, s_palette->bezel, IN_W);
  } else {
    int src = SPLIT + 1 + (iy - SPLIT - 1) * (IN_H - SPLIT - 1) / (FLAP_EDGE_LOW - SPLIT - 1);
    paint_face_row(px, next, src, false);
  }
}

static void draw_tile(GBitmap *fb, int ox, int oy, const Cell *c) {
  uint8_t next = cell_next(c);
  for (int y = 0; y < TILE_H; y++) {
    uint8_t *row = gbitmap_get_data_row_info(fb, oy + y).data + ox;
    int iy = y - 1;
    if (y == 0 || y == TILE_H - 1) {
      memset(row, s_palette->bezel, TILE_W);
      continue;
    }
    row[0] = row[TILE_W - 1] = s_palette->bezel;
    uint8_t *face = row + 1;
    if (iy == SPLIT) {
      memset(face, s_palette->bg, IN_W);
      continue;
    }
    if (c->flip == FLIP_TOP) {
      paint_flip_top(face, c, next, iy);
    } else if (c->flip == FLIP_BOTTOM) {
      paint_flip_bottom(face, c, next, iy);
    } else {
      paint_face_row(face, c->cur, iy, false);
    }
  }
}

static void board_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, (GColor8) { .argb = s_palette->bg });
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  int x0 = (bounds.size.w - BOARD_W) / 2;
  int y0 = (bounds.size.h - BOARD_H) / 2;
  GBitmap *fb = graphics_capture_frame_buffer(ctx);
  if (!fb) {
    return;
  }
  for (int r = 0; r < ROWS; r++) {
    for (int col = 0; col < COLS; col++) {
      draw_tile(fb, x0 + col * (TILE_W + GAP_X), y0 + r * (TILE_H + GAP_Y), &s_cells[r][col]);
    }
  }
  graphics_release_frame_buffer(ctx, fb);
}

// ---------------------------------------------------------------------------
// Board content

static const char *const WEEKDAYS[] = {
  "SUNDAY", "MONDAY", "TUESDAY", "WEDNES.", "THURSDAY", "FRIDAY", "SATURDAY",
};

static void update_time(struct tm *t) {
  char buf[16];
  int hour = t->tm_hour % 12;
  snprintf(buf, sizeof(buf), "%02d:%02d %s", hour ? hour : 12, t->tm_min,
           t->tm_hour < 12 ? "AM" : "PM");
  set_mode_rows(ROW_TIME, buf, false);
  strftime(buf, sizeof(buf), "%m/%d/%y", t);
  set_mode_rows(ROW_DATE, buf, false);
  set_mode_rows(ROW_WEEKDAY, WEEKDAYS[t->tm_wday], false);
}

static void update_battery(BatteryChargeState state) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%03d%%", state.charge_percent);
  set_mode_rows(ROW_BATTERY, buf, false);
}

static void update_steps(bool direct) {
  int steps = 0;
#if defined(PBL_HEALTH)
  time_t start = time_start_of_today(), end = time(NULL);
  if (health_service_metric_accessible(HealthMetricStepCount, start, end) &
      HealthServiceAccessibilityMaskAvailable) {
    steps = (int)health_service_sum_today(HealthMetricStepCount);
  }
#endif
  if (steps > 999999) {
    steps = 999999;
  }
  char buf[16];
  snprintf(buf, sizeof(buf), "%d%c", steps, KEY_HEART);
  set_mode_rows(ROW_STEPS, buf, direct);
}

static void update_weather(void) {
  set_mode_rows(ROW_WEATHER, s_weather.text[0] ? s_weather.text : "WEATHER", false);
}

static void update_board(void) {
  time_t now = time(NULL);
  update_time(localtime(&now));
  update_battery(battery_state_service_peek());
  update_steps(false);
  update_weather();
  for (int row = 0; row < ROWS; row++) {
    if (s_settings.row_modes[row] == ROW_CUSTOM) {
      set_row_flaps(row, s_settings.custom[row], false);
    }
  }
}

// Ask the phone for weather if a row shows it and the last check is old enough.
// The check time is stored so reopening the face can't trigger extra requests.
static void maybe_request_weather(void) {
  bool wanted = false;
  for (int row = 0; row < ROWS; row++) {
    wanted |= s_settings.row_modes[row] == ROW_WEATHER;
  }
  time_t now = time(NULL);
  time_t age = now - s_weather.checked;
  if (!wanted || !s_js_ready || (age >= 0 && age < WEATHER_INTERVAL_S)) {
    return;
  }
  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) != APP_MSG_OK) {
    return;
  }
  dict_write_uint8(iter, MESSAGE_KEY_WeatherRequest, 1);
  if (app_message_outbox_send() == APP_MSG_OK) {
    s_weather.checked = now;
    persist_write_data(WEATHER_KEY, &s_weather, sizeof(s_weather));
  }
}

static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  update_time(tick_time);
  update_steps(true);
  maybe_request_weather();
}

#if defined(PBL_HEALTH)
static void health_handler(HealthEventType event, void *context) {
  if (event == HealthEventMovementUpdate || event == HealthEventSignificantUpdate) {
    update_steps(true);
  }
}
#endif

// Wrist flick: every flap that shows a character spins one full revolution.
// Within each row the spins ripple left to right, each filled flap trailing
// its filled neighbour by one flap step.
static void tap_handler(AccelAxisType axis, int32_t direction) {
  if (s_settings.eco_mode == ECO_ON) {
    return;
  }
  note_activity();
  bool any = false;
  for (int r = 0; r < ROWS; r++) {
    uint8_t wave = 0;
    for (int col = 0; col < COLS; col++) {
      Cell *c = &s_cells[r][col];
      if (c->target == 0) {
        continue;
      }
      if (c->laps == 0) {
        c->laps = 1;
        c->delay = wave;
        any = true;
      }
      wave++;
    }
  }
  if (any) {
    anim_start();
  }
}

// Watchfaces never receive button clicks, but regaining focus means a
// notification or other system window over the face has just closed.
static void focus_handler(bool in_focus) {
  if (in_focus) {
    note_activity();
  }
}

// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Settings

// Defaults: the first four rows, smart eco, dark theme, and a rainbow starting
// display shifted one column right on each row.
static void settings_set_defaults(void) {
  memset(&s_settings, 0, sizeof(s_settings));
  for (int r = 0; r < ROWS; r++) {
    s_settings.row_modes[r] = r;
    for (int col = 0; col < COLS; col++) {
      s_settings.start[r][col] = flap_for_key(RAINBOW_KEYS[(col - r + RAINBOW_LEN) % RAINBOW_LEN]);
    }
  }
  s_settings.eco_mode = ECO_SMART;
  s_settings.theme = THEME_DARK;
}

static void settings_load(void) {
  settings_set_defaults();
  if (persist_get_size(SETTINGS_KEY) == (int)sizeof(s_settings)) {
    persist_read_data(SETTINGS_KEY, &s_settings, sizeof(s_settings));
  }
  s_palette = &PALETTES[s_settings.theme];
  if (persist_get_size(WEATHER_KEY) == (int)sizeof(s_weather)) {
    persist_read_data(WEATHER_KEY, &s_weather, sizeof(s_weather));
  }
}

static void read_flaps(DictionaryIterator *iter, uint32_t key, uint8_t *dst) {
  Tuple *t = dict_find(iter, key);
  if (!t || t->type != TUPLE_BYTE_ARRAY || t->length != ROWS * COLS) {
    return;
  }
  const uint8_t *data = t->value->data;
  for (int i = 0; i < ROWS * COLS; i++) {
    dst[i] = data[i] < FLAP_COUNT ? data[i] : 0;
  }
}

// The config page always sends every setting together, marked by Theme.
static void settings_receive(DictionaryIterator *iter) {
  Tuple *theme = dict_find(iter, MESSAGE_KEY_Theme);
  if (!theme) {
    return;
  }
  if (theme->value->uint8 < THEME_COUNT) {
    s_settings.theme = theme->value->uint8;
  }
  Tuple *eco = dict_find(iter, MESSAGE_KEY_EcoMode);
  if (eco && eco->value->uint8 < ECO_MODE_COUNT) {
    s_settings.eco_mode = eco->value->uint8;
  }
  Tuple *modes = dict_find(iter, MESSAGE_KEY_RowModes);
  if (modes && modes->type == TUPLE_BYTE_ARRAY && modes->length == ROWS) {
    const uint8_t *data = modes->value->data;
    for (int r = 0; r < ROWS; r++) {
      if (data[r] < ROW_MODE_COUNT) {
        s_settings.row_modes[r] = data[r];
      }
    }
  }
  read_flaps(iter, MESSAGE_KEY_StartGrid, &s_settings.start[0][0]);
  read_flaps(iter, MESSAGE_KEY_CustomRows, &s_settings.custom[0][0]);
  persist_write_data(SETTINGS_KEY, &s_settings, sizeof(s_settings));

  s_palette = &PALETTES[s_settings.theme];
  apply_eco_mode();
  update_board();
  layer_mark_dirty(s_board_layer);
  maybe_request_weather();
}

static void inbox_received(DictionaryIterator *iter, void *context) {
  if (dict_find(iter, MESSAGE_KEY_JSReady)) {
    s_js_ready = true;
    maybe_request_weather();
  }
  Tuple *weather = dict_find(iter, MESSAGE_KEY_WeatherText);
  if (weather && weather->type == TUPLE_CSTRING) {
    strncpy(s_weather.text, weather->value->cstring, COLS);
    s_weather.text[COLS] = '\0';
    persist_write_data(WEATHER_KEY, &s_weather, sizeof(s_weather));
    update_weather();
  }
  settings_receive(iter);
}

// ---------------------------------------------------------------------------

static void set_start_display(void) {
  for (int r = 0; r < ROWS; r++) {
    for (int col = 0; col < COLS; col++) {
      s_cells[r][col].cur = s_cells[r][col].target = s_settings.start[r][col];
    }
  }
}

static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  s_board_layer = layer_create(layer_get_bounds(root));
  layer_set_update_proc(s_board_layer, board_update_proc);
  layer_add_child(root, s_board_layer);

  // Hold the starting display for a moment; the first update then flips it
  // into place. With eco always on, the values simply appear instead.
  set_start_display();
  apply_eco_mode();
  if (!s_eco) {
    s_timer = app_timer_register(STARTUP_HOLD_MS, anim_step, NULL);
  }
  update_board();
}

static void window_unload(Window *window) {
  if (s_timer) {
    app_timer_cancel(s_timer);
    s_timer = NULL;
  }
  if (s_eco_timer) {
    app_timer_cancel(s_eco_timer);
    s_eco_timer = NULL;
  }
  layer_destroy(s_board_layer);
}

static void init(void) {
  s_heart_flap = flap_for_key(KEY_HEART);
  settings_load();

  app_message_register_inbox_received(inbox_received);
  app_message_open(256, 64);

  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = window_load,
    .unload = window_unload,
  });
  window_stack_push(s_window, true);

  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
  battery_state_service_subscribe(update_battery);
  accel_tap_service_subscribe(tap_handler);
  app_focus_service_subscribe_handlers((AppFocusHandlers) { .did_focus = focus_handler });
#if defined(PBL_HEALTH)
  health_service_events_subscribe(health_handler, NULL);
#endif
}

static void deinit(void) {
  tick_timer_service_unsubscribe();
  battery_state_service_unsubscribe();
  accel_tap_service_unsubscribe();
  app_focus_service_unsubscribe();
#if defined(PBL_HEALTH)
  health_service_events_unsubscribe();
#endif
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
