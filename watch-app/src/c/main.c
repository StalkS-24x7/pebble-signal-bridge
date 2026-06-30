#include <pebble.h>

// ============================================================
// Protocol — must match PebbleCommunicator.kt KEY_* constants
// ============================================================
typedef enum {
  KEY_MSG_TYPE    = 0,
  KEY_SENDER      = 1,
  KEY_BODY        = 2,
  KEY_CONV_ID     = 3,
  KEY_HAS_IMAGE   = 4,
  KEY_CHUNK_INDEX = 5,
  KEY_CHUNK_TOTAL = 6,
  KEY_CHUNK_DATA  = 7,
  KEY_IMG_WIDTH   = 8,
  KEY_IMG_HEIGHT  = 9,
  KEY_REPLY_TEXT  = 10,
} AppMsgKey;

typedef enum {
  MSG_NEW_MESSAGE = 1,
  MSG_IMAGE_CHUNK = 2,
  MSG_REPLY       = 3,
} MsgType;

// ============================================================
// Limits — emery: 200×228 display, 256KB app heap
// ============================================================
#define MAX_THREADS      8     // concurrent conversation threads
#define THREAD_MSG_COUNT 5     // messages stored per thread
#define MAX_IMAGES       3     // LRU image cache slots
#define SENDER_LEN       48
#define BODY_LEN         512   // runtime body buffer
#define PERSIST_BODY_LEN 128   // abbreviated body saved to flash
#define CONV_ID_LEN      64
#define IMG_SIDE         80    // 80×80 pixels (keeps static BSS under uint16 limit)
#define CHUNK_BYTES      500
#define INBOX_SIZE       2048
#define OUTBOX_SIZE      512

// Persistent storage keys
#define PKEY_VERSION     0
#define PKEY_THREADS     1     // keys 1..MAX_THREADS for thread data
#define PKEY_COUNT       (PKEY_THREADS + MAX_THREADS)
#define PERSIST_VERSION  3

// Conversation window layout
#define CONV_PAD       4
#define CONV_SENDER_H  24
#define CONV_TS_H      16
#define CONV_SEP_H     1
// Max tracked dynamic layers per conv view: body+ts+sep+bitmap per message
#define MAX_DYN_LAYERS (THREAD_MSG_COUNT * 4)

// ============================================================
// Canned replies
// ============================================================
static const char *CANNED[] = {
  "On my way!",
  "OK, got it",
  "Can't talk right now",
  "Call me later",
  "Yes",
  "No",
  "Thanks!",
  "Running late",
  "5 minutes away",
  "Will call you back",
};
#define NUM_CANNED ((int)(sizeof(CANNED)/sizeof(CANNED[0])))

// ============================================================
// Data model
// ============================================================
typedef struct {
  char   body[BODY_LEN];
  time_t timestamp;
  int8_t image_idx;   // >=0: index in image cache; -1: none; -2: had image, not cached
} TMsg;

typedef struct {
  char    sender [SENDER_LEN];
  char    conv_id[CONV_ID_LEN];
  TMsg    messages[THREAD_MSG_COUNT];
  uint8_t msg_count;
  bool    has_unread;
} Thread;

static Thread  s_threads[MAX_THREADS];
static uint8_t s_thread_count = 0;

// Compact form saved to persistent storage
// Size check: (48+64+(128+5)*5+2) = 921 bytes; 8×921 = 7368 < 8192 ✓
typedef struct __attribute__((packed)) {
  char     body[PERSIST_BODY_LEN];
  uint32_t timestamp;
  uint8_t  had_image;
} PMsg;
typedef struct __attribute__((packed)) {
  char    sender [SENDER_LEN];
  char    conv_id[CONV_ID_LEN];
  PMsg    messages[THREAD_MSG_COUNT];
  uint8_t msg_count;
  uint8_t has_unread;
} PThread;

// ============================================================
// Image cache (LRU ring, MAX_IMAGES slots)
// ============================================================
static uint8_t  s_img_data   [MAX_IMAGES][IMG_SIDE * IMG_SIDE];
static GBitmap *s_img_bitmaps[MAX_IMAGES];
static bool     s_img_valid  [MAX_IMAGES];
static int      s_img_next   = 0;  // next slot to overwrite

// In-flight image assembly state
static int  s_recv_slot   = -1;
static int  s_chunks_recv = 0, s_chunks_total = 0;
static int  s_recv_w = 0, s_recv_h = 0;

// ============================================================
// Dynamic layer tracking for conversation window
// ============================================================
typedef enum { DLT_TEXT, DLT_BITMAP } DynLayerType;
typedef struct {
  DynLayerType type;
  union { TextLayer *tl; BitmapLayer *bl; };
} DynLayer;

static DynLayer s_dyn[MAX_DYN_LAYERS];
static int      s_dyn_count = 0;

// Per-message body and timestamp string buffers (avoids stack allocations)
static char s_body_bufs[THREAD_MSG_COUNT][BODY_LEN + 16];
static char s_ts_bufs  [THREAD_MSG_COUNT][20];

// ============================================================
// Windows
// ============================================================
static Window     *s_main_win;
static MenuLayer  *s_msg_menu;

static Window      *s_conv_win = NULL;
static ScrollLayer *s_conv_scroll;
static TextLayer   *s_conv_sender_tl;
static int          s_viewing_tidx = 0;

static Window     *s_reply_win;
static MenuLayer  *s_reply_menu;
static char        s_active_conv[CONV_ID_LEN] = "";

// ============================================================
// Forward declarations
// ============================================================
static void main_reload(void);
static void conv_reload(void);
static void push_conv_window(int tidx);
static void push_reply_window(void);
static void persist_save(void);
static void persist_load(void);

// ============================================================
// Helpers
// ============================================================
static GBitmap *make_bitmap(const uint8_t *raw, int w, int h) {
  GBitmap *bmp = gbitmap_create_blank(GSize(w, h), GBitmapFormat8Bit);
  if (!bmp) return NULL;
  uint8_t *data   = (uint8_t *)gbitmap_get_data(bmp);
  uint16_t stride = gbitmap_get_bytes_per_row(bmp);
  for (int y = 0; y < h; y++) {
    memcpy(data + (size_t)y * stride, raw + y * w, w);
  }
  return bmp;
}

static int find_thread(const char *conv_id) {
  for (int i = 0; i < (int)s_thread_count; i++) {
    if (strncmp(s_threads[i].conv_id, conv_id, CONV_ID_LEN) == 0) return i;
  }
  return -1;
}

// Bubble thread tidx to front, shifting others down
static void bubble_thread(int tidx) {
  if (tidx <= 0) return;
  Thread tmp = s_threads[tidx];
  memmove(&s_threads[1], &s_threads[0], sizeof(Thread) * tidx);
  s_threads[0] = tmp;
}

static void thread_prepend(Thread *t, const char *body, time_t ts, int8_t img_idx) {
  for (int i = THREAD_MSG_COUNT - 1; i > 0; i--) t->messages[i] = t->messages[i-1];
  strncpy(t->messages[0].body, body, BODY_LEN - 1);
  t->messages[0].body[BODY_LEN - 1] = '\0';
  t->messages[0].timestamp = ts;
  t->messages[0].image_idx = img_idx;
  if (t->msg_count < THREAD_MSG_COUNT) t->msg_count++;
}

// ============================================================
// Base64 decoder
// ============================================================
static const int8_t B64[256] = {
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
  52,53,54,55,56,57,58,59,60,61,-1,-1,-1, 0,-1,-1,
  -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
  15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
  -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
  41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
};

static int b64_decode(const char *src, uint8_t *dst, int max_out) {
  int n = strlen(src), out = 0;
  for (int i = 0; i + 3 < n && out < max_out; i += 4) {
    int c0 = B64[(uint8_t)src[i]],   c1 = B64[(uint8_t)src[i+1]];
    int c2 = (src[i+2]!='=') ? B64[(uint8_t)src[i+2]] : 0;
    int c3 = (src[i+3]!='=') ? B64[(uint8_t)src[i+3]] : 0;
    if (c0 < 0 || c1 < 0) break;
    uint32_t v = ((uint32_t)c0<<18)|((uint32_t)c1<<12)|((uint32_t)c2<<6)|(uint32_t)c3;
    if (out < max_out) dst[out++] = (v>>16)&0xFF;
    if (src[i+2]!='=' && out < max_out) dst[out++] = (v>>8)&0xFF;
    if (src[i+3]!='=' && out < max_out) dst[out++] = v&0xFF;
  }
  return out;
}

// timeline_item_local_push is not exposed in the open Pebble SDK 4.3 headers.
// Stubbed out — messages still arrive via AppMessage and vibrate normally.
static void push_timeline_pin(const Thread *t, const TMsg *msg) {
  (void)t; (void)msg;
}

// ============================================================
// Persistence — save and load thread store to/from flash
// ============================================================
static void persist_save(void) {
  persist_write_int(PKEY_VERSION, PERSIST_VERSION);
  int count = (int)s_thread_count;
  if (count > MAX_THREADS) count = MAX_THREADS;

  for (int i = 0; i < count; i++) {
    PThread pt;
    Thread *t = &s_threads[i];
    strncpy(pt.sender,  t->sender,  SENDER_LEN  - 1);
    strncpy(pt.conv_id, t->conv_id, CONV_ID_LEN - 1);
    pt.sender [SENDER_LEN  - 1] = '\0';
    pt.conv_id[CONV_ID_LEN - 1] = '\0';
    pt.msg_count  = t->msg_count;
    pt.has_unread = t->has_unread ? 1 : 0;
    for (int j = 0; j < THREAD_MSG_COUNT; j++) {
      strncpy(pt.messages[j].body, t->messages[j].body, PERSIST_BODY_LEN - 1);
      pt.messages[j].body[PERSIST_BODY_LEN - 1] = '\0';
      pt.messages[j].timestamp = (uint32_t)t->messages[j].timestamp;
      pt.messages[j].had_image = (t->messages[j].image_idx != -1) ? 1 : 0;
    }
    persist_write_data(PKEY_THREADS + i, &pt, sizeof(PThread));
  }
  persist_write_int(PKEY_COUNT, count);
}

static void persist_load(void) {
  if (persist_read_int(PKEY_VERSION) != PERSIST_VERSION) return;
  int count = persist_read_int(PKEY_COUNT);
  if (count <= 0 || count > MAX_THREADS) return;

  s_thread_count = 0;
  for (int i = 0; i < count; i++) {
    PThread pt;
    if (persist_read_data(PKEY_THREADS + i, &pt, sizeof(PThread))
        != (int)sizeof(PThread)) continue;

    Thread *t = &s_threads[s_thread_count++];
    memset(t, 0, sizeof(Thread));
    strncpy(t->sender,  pt.sender,  SENDER_LEN  - 1);
    strncpy(t->conv_id, pt.conv_id, CONV_ID_LEN - 1);
    t->msg_count  = pt.msg_count;
    t->has_unread = pt.has_unread != 0;
    for (int j = 0; j < THREAD_MSG_COUNT; j++) {
      strncpy(t->messages[j].body, pt.messages[j].body, BODY_LEN - 1);
      t->messages[j].timestamp = (time_t)pt.messages[j].timestamp;
      // -2 signals "had image in this session but not in cache now"
      t->messages[j].image_idx = pt.messages[j].had_image ? -2 : -1;
    }
  }
  APP_LOG(APP_LOG_LEVEL_INFO, "Loaded %d threads from flash", (int)s_thread_count);
}

// ============================================================
// AppMessage — receive
// ============================================================
static void recv_new_message(DictionaryIterator *iter) {
  Tuple *ts = dict_find(iter, KEY_SENDER);
  Tuple *tb = dict_find(iter, KEY_BODY);
  Tuple *tc = dict_find(iter, KEY_CONV_ID);
  Tuple *ti = dict_find(iter, KEY_HAS_IMAGE);
  if (!ts || !tb || !tc) return;

  const char *sender  = ts->value->cstring;
  const char *body    = tb->value->cstring;
  const char *conv_id = tc->value->cstring;
  bool has_image      = ti ? (bool)ti->value->uint8 : false;
  time_t now          = time(NULL);

  // Find existing thread or create one, then bubble to top
  int tidx = find_thread(conv_id);
  if (tidx < 0) {
    // No existing thread — make room if full
    if (s_thread_count >= MAX_THREADS) {
      // Drop the oldest (last) thread
      s_thread_count = MAX_THREADS - 1;
    }
    // Insert at front
    if (s_thread_count > 0) {
      memmove(&s_threads[1], &s_threads[0], sizeof(Thread) * s_thread_count);
    }
    s_thread_count++;
    memset(&s_threads[0], 0, sizeof(Thread));
    strncpy(s_threads[0].sender,  sender,  SENDER_LEN  - 1);
    strncpy(s_threads[0].conv_id, conv_id, CONV_ID_LEN - 1);
    tidx = 0;
  } else {
    bubble_thread(tidx);
    tidx = 0;
  }
  Thread *t = &s_threads[0];

  // Allocate LRU image cache slot if this message has an image
  int8_t img_idx = -1;
  if (has_image) {
    img_idx = (int8_t)s_img_next;
    if (s_img_bitmaps[img_idx]) {
      gbitmap_destroy(s_img_bitmaps[img_idx]);
      s_img_bitmaps[img_idx] = NULL;
    }
    s_img_valid[img_idx] = false;
    s_img_next = (s_img_next + 1) % MAX_IMAGES;

    s_recv_slot   = img_idx;
    s_chunks_recv  = 0;
    s_chunks_total = 0;
    s_recv_w = s_recv_h = 0;
  }

  thread_prepend(t, body, now, img_idx);
  t->has_unread = true;

  push_timeline_pin(t, &t->messages[0]);
  persist_save();
  main_reload();
  vibes_short_pulse();
}

static void recv_image_chunk(DictionaryIterator *iter) {
  if (s_recv_slot < 0) return;
  Tuple *tidx   = dict_find(iter, KEY_CHUNK_INDEX);
  Tuple *ttotal = dict_find(iter, KEY_CHUNK_TOTAL);
  Tuple *tdata  = dict_find(iter, KEY_CHUNK_DATA);
  Tuple *tw     = dict_find(iter, KEY_IMG_WIDTH);
  Tuple *th     = dict_find(iter, KEY_IMG_HEIGHT);
  if (!tidx || !ttotal || !tdata) return;

  int idx   = (int)tidx->value->uint8;
  int total = (int)ttotal->value->uint8;

  if (idx == 0) {
    s_chunks_recv  = 0;
    s_chunks_total = total;
    s_recv_w = tw ? (int)tw->value->uint8 : IMG_SIDE;
    s_recv_h = th ? (int)th->value->uint8 : IMG_SIDE;
  }

  int offset = idx * CHUNK_BYTES;
  int room   = (int)sizeof(s_img_data[0]) - offset;
  if (room > 0) {
    b64_decode(tdata->value->cstring,
               s_img_data[s_recv_slot] + offset, room);
  }
  if (++s_chunks_recv >= s_chunks_total) {
    if (s_img_bitmaps[s_recv_slot]) {
      gbitmap_destroy(s_img_bitmaps[s_recv_slot]);
    }
    s_img_bitmaps[s_recv_slot] = make_bitmap(
      s_img_data[s_recv_slot], s_recv_w, s_recv_h);
    s_img_valid[s_recv_slot] = (s_img_bitmaps[s_recv_slot] != NULL);
    s_recv_slot = -1;
    conv_reload();
  }
}

static void appmsg_received(DictionaryIterator *iter, void *ctx) {
  Tuple *tt = dict_find(iter, KEY_MSG_TYPE);
  if (!tt) return;
  switch ((MsgType)tt->value->uint8) {
    case MSG_NEW_MESSAGE: recv_new_message(iter); break;
    case MSG_IMAGE_CHUNK: recv_image_chunk(iter); break;
    default: break;
  }
}

static void appmsg_dropped(AppMessageResult r, void *ctx) {
  APP_LOG(APP_LOG_LEVEL_WARNING, "AppMsg dropped: %d", (int)r);
}

// ============================================================
// Reply
// ============================================================
static void send_reply(const char *text) {
  DictionaryIterator *out;
  if (app_message_outbox_begin(&out) != APP_MSG_OK) return;
  dict_write_uint8 (out, KEY_MSG_TYPE,   MSG_REPLY);
  dict_write_cstring(out, KEY_REPLY_TEXT, text);
  dict_write_cstring(out, KEY_CONV_ID,    s_active_conv);
  app_message_outbox_send();
}

static DictationSession *s_dictation = NULL;
static void dictation_cb(DictationSession *session,
                          DictationSessionStatus status,
                          char *transcript, void *ctx) {
  if (status == DictationSessionStatusSuccess && transcript && *transcript) {
    send_reply(transcript);
    window_stack_pop_all(false);
  }
  dictation_session_destroy(session);
  s_dictation = NULL;
}
static void start_dictation(void) {
  if (s_dictation) return;
  s_dictation = dictation_session_create(BODY_LEN, dictation_cb, NULL);
  if (s_dictation) dictation_session_start(s_dictation);
}

// ============================================================
// Reply Window
// ============================================================
static uint16_t reply_num_rows(MenuLayer *ml, uint16_t si, void *ctx) {
  return (uint16_t)(NUM_CANNED + 1);
}
static void reply_draw_row(GContext *gctx, const Layer *cell,
                            MenuIndex *ci, void *ctx) {
  if ((int)ci->row < NUM_CANNED)
    menu_cell_basic_draw(gctx, cell, CANNED[ci->row], NULL, NULL);
  else
    menu_cell_basic_draw(gctx, cell, "Dictate...", NULL, NULL);
}
static void reply_select(MenuLayer *ml, MenuIndex *ci, void *ctx) {
  if ((int)ci->row < NUM_CANNED) {
    send_reply(CANNED[ci->row]);
    window_stack_pop_all(false);
  } else {
    start_dictation();
  }
}
static void reply_win_load(Window *win) {
  Layer *root = window_get_root_layer(win);
  s_reply_menu = menu_layer_create(layer_get_bounds(root));
  menu_layer_set_callbacks(s_reply_menu, NULL, (MenuLayerCallbacks){
    .get_num_rows = reply_num_rows,
    .draw_row     = reply_draw_row,
    .select_click = reply_select,
  });
  menu_layer_set_click_config_onto_window(s_reply_menu, win);
  layer_add_child(root, menu_layer_get_layer(s_reply_menu));
}
static void reply_win_unload(Window *win) {
  menu_layer_destroy(s_reply_menu);
}
static void push_reply_window(void) {
  s_reply_win = window_create();
  window_set_window_handlers(s_reply_win, (WindowHandlers){
    .load   = reply_win_load,
    .unload = reply_win_unload,
  });
  window_stack_push(s_reply_win, true);
}

// ============================================================
// Conversation Window — threaded multi-message view
// ============================================================
static void conv_destroy_dyn_layers(void) {
  for (int i = 0; i < s_dyn_count; i++) {
    if (s_dyn[i].type == DLT_TEXT) {
      layer_remove_from_parent(text_layer_get_layer(s_dyn[i].tl));
      text_layer_destroy(s_dyn[i].tl);
    } else {
      layer_remove_from_parent(bitmap_layer_get_layer(s_dyn[i].bl));
      bitmap_layer_destroy(s_dyn[i].bl);
    }
  }
  s_dyn_count = 0;
}

static TextLayer *conv_add_text_layer(int x, int y, int w, int h) {
  if (s_dyn_count >= MAX_DYN_LAYERS) return NULL;
  TextLayer *tl = text_layer_create(GRect(x, y, w, h));
  scroll_layer_add_child(s_conv_scroll, text_layer_get_layer(tl));
  s_dyn[s_dyn_count++] = (DynLayer){ .type = DLT_TEXT, .tl = tl };
  return tl;
}

static BitmapLayer *conv_add_bitmap_layer(int x, int y, int w, int h) {
  if (s_dyn_count >= MAX_DYN_LAYERS) return NULL;
  BitmapLayer *bl = bitmap_layer_create(GRect(x, y, w, h));
  scroll_layer_add_child(s_conv_scroll, bitmap_layer_get_layer(bl));
  s_dyn[s_dyn_count++] = (DynLayer){ .type = DLT_BITMAP, .bl = bl };
  return bl;
}

static void conv_reload(void) {
  if (!s_conv_win || s_viewing_tidx >= (int)s_thread_count) return;
  Thread *t = &s_threads[s_viewing_tidx];

  t->has_unread = false;
  text_layer_set_text(s_conv_sender_tl, t->sender);

  conv_destroy_dyn_layers();

  GRect sb = layer_get_bounds(scroll_layer_get_layer(s_conv_scroll));
  int w = sb.size.w;
  int y = CONV_PAD + CONV_SENDER_H + CONV_PAD;

  for (int i = 0; i < (int)t->msg_count; i++) {
    TMsg *msg = &t->messages[i];

    // Separator between messages
    if (i > 0) {
      TextLayer *sep = conv_add_text_layer(0, y, w, CONV_SEP_H);
      if (sep) text_layer_set_background_color(sep, GColorLightGray);
      y += CONV_SEP_H + CONV_PAD;
    }

    // Image: show if in cache, placeholder text if had but evicted
    int8_t idx = msg->image_idx;
    bool in_cache = (idx >= 0 && idx < MAX_IMAGES && s_img_valid[idx]);

    if (in_cache) {
      int img_x = (w - IMG_SIDE) / 2;
      BitmapLayer *bl = conv_add_bitmap_layer(img_x, y, IMG_SIDE, IMG_SIDE);
      if (bl) {
        bitmap_layer_set_bitmap(bl, s_img_bitmaps[idx]);
        bitmap_layer_set_alignment(bl, GAlignCenter);
      }
      y += IMG_SIDE + CONV_PAD;
    }

    // Body text (prefix with [Photo] if image was evicted from cache)
    bool had_evicted = (idx == -2 && !in_cache);
    if (had_evicted) {
      snprintf(s_body_bufs[i], sizeof(s_body_bufs[i]),
               "[Photo]\n%s", msg->body);
    } else {
      strncpy(s_body_bufs[i], msg->body, sizeof(s_body_bufs[i]) - 1);
      s_body_bufs[i][sizeof(s_body_bufs[i])-1] = '\0';
    }

    TextLayer *btl = conv_add_text_layer(CONV_PAD, y, w - CONV_PAD*2, 800);
    if (btl) {
      text_layer_set_font(btl, fonts_get_system_font(FONT_KEY_GOTHIC_14));
      text_layer_set_overflow_mode(btl, GTextOverflowModeWordWrap);
      text_layer_set_text(btl, s_body_bufs[i]);
      GSize sz = text_layer_get_content_size(btl);
      layer_set_frame(text_layer_get_layer(btl),
                      GRect(CONV_PAD, y, w - CONV_PAD*2, sz.h + 2));
      y += sz.h + 2 + CONV_PAD;
    }

    // Timestamp (right-aligned, muted)
    struct tm *lt = localtime(&msg->timestamp);
    strftime(s_ts_bufs[i], sizeof(s_ts_bufs[i]), "%H:%M", lt);

    TextLayer *ttl = conv_add_text_layer(CONV_PAD, y, w - CONV_PAD*2, CONV_TS_H);
    if (ttl) {
      text_layer_set_font(ttl, fonts_get_system_font(FONT_KEY_GOTHIC_14));
      text_layer_set_text_alignment(ttl, GTextAlignmentRight);
      text_layer_set_text_color(ttl, GColorDarkGray);
      text_layer_set_text(ttl, s_ts_bufs[i]);
    }
    y += CONV_TS_H + CONV_PAD;
  }

  scroll_layer_set_content_size(s_conv_scroll, GSize(w, y + CONV_PAD));
}

static void conv_select_click(ClickRecognizerRef cr, void *ctx) {
  push_reply_window();
}
static void conv_click_config(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_SELECT, conv_select_click);
}

static void conv_win_load(Window *win) {
  Layer *root = window_get_root_layer(win);
  GRect b = layer_get_bounds(root);

  s_conv_scroll = scroll_layer_create(b);
  scroll_layer_set_click_config_onto_window(s_conv_scroll, win);
  scroll_layer_set_callbacks(s_conv_scroll, (ScrollLayerCallbacks){
    .click_config_provider = conv_click_config,
  });
  layer_add_child(root, scroll_layer_get_layer(s_conv_scroll));

  // Fixed sender header — always visible at top of scroll content
  s_conv_sender_tl = text_layer_create(
    GRect(CONV_PAD, CONV_PAD, b.size.w - CONV_PAD*2, CONV_SENDER_H));
  text_layer_set_font(s_conv_sender_tl,
                      fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  text_layer_set_overflow_mode(s_conv_sender_tl, GTextOverflowModeTrailingEllipsis);
  scroll_layer_add_child(s_conv_scroll, text_layer_get_layer(s_conv_sender_tl));

  s_dyn_count = 0;
  window_set_click_config_provider(win, conv_click_config);
  conv_reload();
}

static void conv_win_unload(Window *win) {
  conv_destroy_dyn_layers();
  layer_remove_from_parent(text_layer_get_layer(s_conv_sender_tl));
  text_layer_destroy(s_conv_sender_tl);
  scroll_layer_destroy(s_conv_scroll);
  window_destroy(win);
  s_conv_win = NULL;
}

static void push_conv_window(int tidx) {
  s_viewing_tidx = tidx;
  strncpy(s_active_conv, s_threads[tidx].conv_id, CONV_ID_LEN - 1);
  s_conv_win = window_create();
  window_set_window_handlers(s_conv_win, (WindowHandlers){
    .load   = conv_win_load,
    .unload = conv_win_unload,
  });
  window_stack_push(s_conv_win, true);
}

// ============================================================
// Main Window — thread list with unread indicator
// ============================================================
static uint16_t main_num_rows(MenuLayer *ml, uint16_t si, void *ctx) {
  return s_thread_count > 0 ? (uint16_t)s_thread_count : 1;
}

static int16_t main_row_height(MenuLayer *ml, MenuIndex *ci, void *ctx) {
  return 48;
}

static void main_draw_row(GContext *gctx, const Layer *cell,
                           MenuIndex *ci, void *ctx) {
  if (s_thread_count == 0) {
    menu_cell_basic_draw(gctx, cell, "No messages", "Waiting...", NULL);
    return;
  }
  int i = (int)ci->row;
  if (i >= (int)s_thread_count) return;
  Thread *t = &s_threads[i];

  GRect b = layer_get_bounds(cell);

  // Unread dot (top-right corner)
  if (t->has_unread) {
    graphics_context_set_fill_color(gctx, GColorRed);
    graphics_fill_circle(gctx, GPoint(b.size.w - 8, 8), 5);
  }

  // Sender + preview
  const char *preview = (t->msg_count > 0) ? t->messages[0].body : "";
  // Show "[Photo]" prefix if the latest message had an image
  static char row_preview[80];
  if (t->msg_count > 0 && t->messages[0].image_idx != -1 && preview[0] == '\0') {
    strncpy(row_preview, "[Photo]", sizeof(row_preview)-1);
  } else if (t->msg_count > 0 && t->messages[0].image_idx != -1) {
    snprintf(row_preview, sizeof(row_preview), "[Photo] %s", preview);
  } else {
    strncpy(row_preview, preview, sizeof(row_preview)-1);
  }
  row_preview[sizeof(row_preview)-1] = '\0';

  menu_cell_basic_draw(gctx, cell, t->sender, row_preview, NULL);
}

static void main_select(MenuLayer *ml, MenuIndex *ci, void *ctx) {
  if (s_thread_count > 0) push_conv_window((int)ci->row);
}

static void main_reload(void) {
  if (s_main_win && s_msg_menu) menu_layer_reload_data(s_msg_menu);
}

static void main_win_load(Window *win) {
  Layer *root = window_get_root_layer(win);
  s_msg_menu = menu_layer_create(layer_get_bounds(root));
  menu_layer_set_callbacks(s_msg_menu, NULL, (MenuLayerCallbacks){
    .get_num_rows   = main_num_rows,
    .get_cell_height = main_row_height,
    .draw_row       = main_draw_row,
    .select_click   = main_select,
  });
  menu_layer_set_click_config_onto_window(s_msg_menu, win);
  layer_add_child(root, menu_layer_get_layer(s_msg_menu));
}

static void main_win_unload(Window *win) {
  menu_layer_destroy(s_msg_menu);
}

// ============================================================
// App lifecycle
// ============================================================
static void init(void) {
  // Clear image cache
  memset(s_img_valid,   0, sizeof(s_img_valid));
  memset(s_img_bitmaps, 0, sizeof(s_img_bitmaps));

  // Load persisted thread store first
  persist_load();

  app_message_register_inbox_received(appmsg_received);
  app_message_register_inbox_dropped(appmsg_dropped);
  app_message_open(INBOX_SIZE, OUTBOX_SIZE);

  s_main_win = window_create();
  window_set_window_handlers(s_main_win, (WindowHandlers){
    .load   = main_win_load,
    .unload = main_win_unload,
  });
  window_stack_push(s_main_win, true);
}

static void deinit(void) {
  persist_save();
  for (int i = 0; i < MAX_IMAGES; i++) {
    if (s_img_bitmaps[i]) gbitmap_destroy(s_img_bitmaps[i]);
  }
  if (s_dictation) dictation_session_destroy(s_dictation);
  window_destroy(s_main_win);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
