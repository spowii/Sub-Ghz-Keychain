#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <input/input.h>
#include <storage/storage.h>
#include <dialogs/dialogs.h>
#include <flipper_format/flipper_format.h>
#include <toolbox/level_duration.h>
#include <lib/subghz/devices/devices.h>
#include <stdio.h>
#include <string.h>

#define CONFIG_DIR "/ext/apps_data/quad_fob"
#define CONFIG_PATH CONFIG_DIR "/config.txt"
#define DEFAULT_DIR "/ext/subghz"
#define BTN_COUNT 4
#define MAX_SAMPLES 4096

// Button indexes: 0 = Left (Close), 1 = Right (Open), 2 = Up (Alarm), 3 = Down (Start)
static const char* btn_keys[BTN_COUNT] = {"Close", "Open", "Alarm", "Start"};

typedef struct {
    FuriString* paths[BTN_COUNT];
    int pressed;
    char status[16];
    FuriMutex* mutex;
} App;

typedef enum {
    SendOk,
    SendNoFile,
    SendBad,
    SendNotRaw,
    SendTooBig,
    SendNoTx,
} SendResult;

/* ---------- status helpers ---------- */

static void set_status(App* app, const char* text) {
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    snprintf(app->status, sizeof(app->status), "%s", text);
    furi_mutex_release(app->mutex);
}

/* ---------- config ---------- */

static void load_config(App* app, Storage* storage) {
    FlipperFormat* ff = flipper_format_file_alloc(storage);
    if(flipper_format_file_open_existing(ff, CONFIG_PATH)) {
        for(int i = 0; i < BTN_COUNT; i++) {
            flipper_format_rewind(ff);
            FuriString* tmp = furi_string_alloc();
            if(flipper_format_read_string(ff, btn_keys[i], tmp)) {
                furi_string_set(app->paths[i], tmp);
            }
            furi_string_free(tmp);
        }
    }
    flipper_format_free(ff);
}

static void save_config(App* app, Storage* storage) {
    storage_simply_mkdir(storage, "/ext/apps_data");
    storage_simply_mkdir(storage, CONFIG_DIR);
    FlipperFormat* ff = flipper_format_file_alloc(storage);
    if(flipper_format_file_open_always(ff, CONFIG_PATH)) {
        flipper_format_write_header_cstr(ff, "Quad Fob config", 1);
        for(int i = 0; i < BTN_COUNT; i++) {
            if(!furi_string_empty(app->paths[i])) {
                flipper_format_write_string_cstr(
                    ff, btn_keys[i], furi_string_get_cstr(app->paths[i]));
            }
        }
    }
    flipper_format_free(ff);
}

/* ---------- Sub-GHz transmit (RAW files) ---------- */

typedef struct {
    int32_t* data;
    size_t count;
    size_t pos;
} TxCtx;

static LevelDuration tx_callback(void* context) {
    TxCtx* t = context;
    if(t->pos >= t->count) return level_duration_reset();
    int32_t v = t->data[t->pos++];
    bool level = v > 0;
    uint32_t dur = (uint32_t)(v > 0 ? v : -v);
    if(dur == 0) dur = 1;
    return level_duration_make(level, dur);
}

static bool load_preset(FlipperFormat* ff, const SubGhzDevice* device, uint8_t** custom) {
    FuriString* preset = furi_string_alloc();
    bool ok = false;

    flipper_format_rewind(ff);
    if(flipper_format_read_string(ff, "Preset", preset)) {
        const char* p = furi_string_get_cstr(preset);
        if(strstr(p, "Ook270Async")) {
            subghz_devices_load_preset(device, FuriHalSubGhzPresetOok270Async, NULL);
            ok = true;
        } else if(strstr(p, "Ook650Async")) {
            subghz_devices_load_preset(device, FuriHalSubGhzPresetOok650Async, NULL);
            ok = true;
        } else if(strstr(p, "2FSKDev238Async")) {
            subghz_devices_load_preset(device, FuriHalSubGhzPreset2FSKDev238Async, NULL);
            ok = true;
        } else if(strstr(p, "2FSKDev476Async")) {
            subghz_devices_load_preset(device, FuriHalSubGhzPreset2FSKDev476Async, NULL);
            ok = true;
        } else if(strstr(p, "Custom")) {
            uint32_t n = 0;
            flipper_format_rewind(ff);
            if(flipper_format_get_value_count(ff, "Custom_preset_data", &n) && n > 0 &&
               n <= 128) {
                *custom = malloc(n);
                if(flipper_format_read_hex(ff, "Custom_preset_data", *custom, n)) {
                    subghz_devices_load_preset(device, FuriHalSubGhzPresetCustom, *custom);
            ok = true;
                }
            }
        }
    }

    furi_string_free(preset);
    return ok;
}

static SendResult send_file(Storage* storage, const char* path) {
    FlipperFormat* ff = flipper_format_file_alloc(storage);
    FuriString* tmp = furi_string_alloc();
    int32_t* data = NULL;
    SendResult res = SendBad;

    do {
        if(!flipper_format_file_open_existing(ff, path)) {
            res = SendNoFile;
            break;
        }

        flipper_format_rewind(ff);
        if(!flipper_format_read_string(ff, "Protocol", tmp)) break;
        if(strcmp(furi_string_get_cstr(tmp), "RAW") != 0) {
            res = SendNotRaw;
            break;
        }

        uint32_t freq = 0;
        flipper_format_rewind(ff);
        if(!flipper_format_read_uint32(ff, "Frequency", &freq, 1)) break;

        data = malloc(MAX_SAMPLES * sizeof(int32_t));
        size_t total = 0;
        bool too_big = false;
        uint32_t cnt = 0;

        flipper_format_rewind(ff);
        while(flipper_format_get_value_count(ff, "RAW_Data", &cnt) && cnt > 0) {
            if(total + cnt > MAX_SAMPLES) {
                too_big = true;
                break;
            }
            if(!flipper_format_read_int32(ff, "RAW_Data", data + total, cnt)) break;
            total += cnt;
        }
        if(too_big) {
            res = SendTooBig;
            break;
        }
        if(total == 0) break;

        if(!furi_hal_region_is_frequency_allowed(freq)) {
            res = SendNoTx;
            break;
        }

        subghz_devices_init();
        const SubGhzDevice* device = subghz_devices_get_by_name("cc1101_int");
        if(device) {
            subghz_devices_begin(device);
            subghz_devices_reset(device);
            subghz_devices_idle(device);

            uint8_t* custom = NULL;
            if(load_preset(ff, device, &custom)) {
                subghz_devices_set_frequency(device, freq);

                TxCtx ctx = {.data = data, .count = total, .pos = 0};
                if(subghz_devices_start_async_tx(device, tx_callback, &ctx)) {
                    while(!subghz_devices_is_async_complete_tx(device)) {
                        furi_delay_ms(10);
                    }
                    subghz_devices_stop_async_tx(device);
                    res = SendOk;
                }
            }
            if(custom) free(custom);

            subghz_devices_idle(device);
            subghz_devices_sleep(device);
            subghz_devices_end(device);
        }
        subghz_devices_deinit();
    } while(false);

    if(data) free(data);
    furi_string_free(tmp);
    flipper_format_free(ff);
    return res;
}

/* ---------- drawing ---------- */

static void draw_button(Canvas* c, int cx, int cy, bool pressed, int icon) {
    if(pressed) {
        canvas_draw_disc(c, cx, cy, 9);
        canvas_set_color(c, ColorWhite);
    } else {
        canvas_draw_circle(c, cx, cy, 9);
    }

    switch(icon) {
    case 0: // closed lock
        canvas_draw_box(c, cx - 3, cy, 7, 5);
        canvas_draw_line(c, cx - 2, cy - 3, cx - 2, cy - 1);
        canvas_draw_line(c, cx + 2, cy - 3, cx + 2, cy - 1);
        canvas_draw_line(c, cx - 2, cy - 3, cx + 2, cy - 3);
        break;
    case 1: // open lock
        canvas_draw_box(c, cx - 3, cy, 7, 5);
        canvas_draw_line(c, cx - 2, cy - 3, cx - 2, cy - 1);
        canvas_draw_line(c, cx - 2, cy - 3, cx + 2, cy - 3);
        canvas_draw_line(c, cx + 2, cy - 3, cx + 2, cy - 2);
        break;
    case 2: // alarm
        canvas_set_font(c, FontPrimary);
        canvas_draw_str_aligned(c, cx, cy + 1, AlignCenter, AlignCenter, "!");
        break;
    case 3: // start (play triangle)
        for(int dx = 0; dx < 5; dx++) {
            int half = 4 - dx;
            canvas_draw_line(c, cx - 2 + dx, cy - half, cx - 2 + dx, cy + half);
        }
        break;
    default:
        break;
    }

    canvas_set_color(c, ColorBlack);
}

static void render_callback(Canvas* canvas, void* ctx) {
    App* app = ctx;
    furi_mutex_acquire(app->mutex, FuriWaitForever);

    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontSecondary);

    canvas_draw_str(canvas, 0, 8, app->status);
    canvas_draw_str(canvas, 0, 35, "Close");
    canvas_draw_str(canvas, 100, 35, "Open");
    canvas_draw_str(canvas, 77, 16, "Alarm");
    canvas_draw_str(canvas, 77, 56, "Start");

    draw_button(canvas, 40, 32, app->pressed == 0, 0); // Left  = Close
    draw_button(canvas, 88, 32, app->pressed == 1, 1); // Right = Open
    draw_button(canvas, 64, 13, app->pressed == 2, 2); // Up    = Alarm
    draw_button(canvas, 64, 51, app->pressed == 3, 3); // Down  = Start

    furi_mutex_release(app->mutex);
}

static void input_callback(InputEvent* event, void* ctx) {
    FuriMessageQueue* queue = ctx;
    furi_message_queue_put(queue, event, FuriWaitForever);
}

/* ---------- actions ---------- */

static int key_to_idx(InputKey key) {
    switch(key) {
    case InputKeyLeft:
        return 0;
    case InputKeyRight:
        return 1;
    case InputKeyUp:
        return 2;
    case InputKeyDown:
        return 3;
    default:
        return -1;
    }
}

static void do_send(App* app, Storage* storage, ViewPort* vp, int idx) {
    if(furi_string_empty(app->paths[idx])) {
        set_status(app, "Hold=file");
        view_port_update(vp);
        return;
    }

    furi_mutex_acquire(app->mutex, FuriWaitForever);
    app->pressed = idx;
    snprintf(app->status, sizeof(app->status), "TX...");
    furi_mutex_release(app->mutex);
    view_port_update(vp);
    furi_delay_ms(40);

    SendResult r = send_file(storage, furi_string_get_cstr(app->paths[idx]));

    furi_mutex_acquire(app->mutex, FuriWaitForever);
    app->pressed = -1;
    furi_mutex_release(app->mutex);

    switch(r) {
    case SendOk:
        set_status(app, "Sent");
        break;
    case SendNoFile:
        set_status(app, "No file");
        break;
    case SendNotRaw:
        set_status(app, "Not RAW");
        break;
    case SendTooBig:
        set_status(app, "Too big");
        break;
    case SendNoTx:
        set_status(app, "TX N/A");
        break;
    default:
        set_status(app, "Bad file");
        break;
    }
    view_port_update(vp);
}

static void do_assign(App* app, Storage* storage, DialogsApp* dialogs, ViewPort* vp, int idx) {
    FuriString* path = furi_string_alloc();
    if(furi_string_empty(app->paths[idx])) {
        furi_string_set(path, DEFAULT_DIR);
    } else {
        furi_string_set(path, app->paths[idx]);
    }

    DialogsFileBrowserOptions options;
    dialog_file_browser_set_basic_options(&options, ".sub", NULL);
    options.base_path = DEFAULT_DIR;

    if(dialog_file_browser_show(dialogs, path, path, &options)) {
        furi_string_set(app->paths[idx], path);
        save_config(app, storage);
        set_status(app, "Saved");
    }
    furi_string_free(path);
    view_port_update(vp);
}

/* ---------- entry point ---------- */

int32_t quad_fob_app(void* p) {
    UNUSED(p);

    App* app = malloc(sizeof(App));
    memset(app, 0, sizeof(App));
    app->pressed = -1;
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    for(int i = 0; i < BTN_COUNT; i++) {
        app->paths[i] = furi_string_alloc();
    }
    snprintf(app->status, sizeof(app->status), "Ready");

    Storage* storage = furi_record_open(RECORD_STORAGE);
    DialogsApp* dialogs = furi_record_open(RECORD_DIALOGS);
    load_config(app, storage);

    FuriMessageQueue* queue = furi_message_queue_alloc(8, sizeof(InputEvent));

    ViewPort* view_port = view_port_alloc();
    view_port_draw_callback_set(view_port, render_callback, app);
    view_port_input_callback_set(view_port, input_callback, queue);

    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);

    InputEvent ev;
    bool running = true;
    while(running) {
        if(furi_message_queue_get(queue, &ev, FuriWaitForever) != FuriStatusOk) continue;

        if(ev.key == InputKeyBack && ev.type == InputTypeShort) {
            running = false;
            continue;
        }

        int idx = key_to_idx(ev.key);
        if(idx < 0) continue;

        if(ev.type == InputTypeShort) {
            do_send(app, storage, view_port, idx);
        } else if(ev.type == InputTypeLong) {
            do_assign(app, storage, dialogs, view_port, idx);
        }
    }

    gui_remove_view_port(gui, view_port);
    furi_record_close(RECORD_GUI);
    view_port_free(view_port);
    furi_message_queue_free(queue);
    furi_record_close(RECORD_DIALOGS);
    furi_record_close(RECORD_STORAGE);
    for(int i = 0; i < BTN_COUNT; i++) {
        furi_string_free(app->paths[i]);
    }
    furi_mutex_free(app->mutex);
    free(app);

    return 0;
}
