/**
 * @file subghz_analyzer.c
 * @brief Sub-GHz Tools — Flipper Zero external application
 *
 * Uses the built-in CC1101 sub-GHz radio to:
 *   - Scan common garage-door / keyfob frequencies
 *   - Capture raw OOK/ASK signals
 *   - Display captured signal info (frequency, RSSI, duration, modulation)
 *   - Replay a captured signal (for authorised testing only)
 *
 * Common target frequencies:
 *   315.000 MHz — North American garage doors (older)
 *   390.000 MHz — Some North American remotes
 *   433.920 MHz — European / worldwide keyfobs & remotes
 *   868.350 MHz — European home-automation devices
 *
 * Build with uFBT or FBT:
 *   ufbt launch APP_DIR=subghz_tools
 */

#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_subghz.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/text_box.h>
#include <gui/modules/popup.h>
#include <gui/modules/widget.h>
#include <notification/notification.h>
#include <notification/notification_messages.h>
#include <lib/subghz/subghz_tx_rx_worker.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ──────────────────────────── constants ──────────────────────────────── */

#define SUBGHZ_SCAN_STEP_HZ     500000U   /* 0.5 MHz step during sweep  */
#define SUBGHZ_LISTEN_MS        300       /* dwell time per frequency    */
#define SUBGHZ_RSSI_THRESHOLD   (-90.0f)  /* dBm — minimum "signal" lvl */
#define SUBGHZ_MAX_HITS         32        /* max entries in scan result  */
#define SUBGHZ_CAPTURE_BUF_SIZE 4096      /* raw pulse buffer (bytes)    */
#define SUBGHZ_REPLAY_REPEAT    3         /* times to resend on replay   */

/* Preset frequencies worth checking (Hz) */
static const uint32_t PRESET_FREQS[] = {
    300000000UL,
    303875000UL,
    304250000UL,
    310000000UL,
    315000000UL,
    318000000UL,
    390000000UL,
    418000000UL,
    433075000UL,
    433920000UL,
    434420000UL,
    868350000UL,
    915000000UL,
};
#define PRESET_FREQ_COUNT ((size_t)(sizeof(PRESET_FREQS) / sizeof(PRESET_FREQS[0])))

/* ──────────────────────────── data types ─────────────────────────────── */

typedef struct {
    uint32_t frequency; /* Hz */
    float rssi;         /* dBm */
} SubGhzHit;

typedef struct {
    uint32_t frequency; /* Hz at capture time */
    float rssi;         /* peak RSSI during capture */
    uint8_t* data;      /* raw pulse data */
    size_t data_len;    /* bytes used */
    bool valid;
} SubGhzCapture;

typedef enum {
    SubGhzViewSubmenu,
    SubGhzViewTextBox,
    SubGhzViewPopup,
} SubGhzView;

typedef enum {
    SubGhzSubmenuScan,
    SubGhzSubmenuCapture,
    SubGhzSubmenuReplay,
    SubGhzSubmenuAbout,
} SubGhzSubmenuIndex;

typedef struct {
    /* GUI */
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    Submenu* submenu;
    TextBox* text_box;
    Popup* popup;

    /* Scan results */
    SubGhzHit hits[SUBGHZ_MAX_HITS];
    size_t hit_count;

    /* Captured signal */
    SubGhzCapture capture;
} SubGhzToolsApp;

/* ──────────────────────────── radio helpers ──────────────────────────── */

/** Initialise the CC1101 for RX at @p freq_hz with AM-OOK modulation. */
static void subghz_setup_rx(uint32_t freq_hz) {
    furi_hal_subghz_reset();
    furi_hal_subghz_load_preset(FuriHalSubGhzPresetOok650Async);
    furi_hal_subghz_set_frequency_and_path(freq_hz);
    furi_hal_subghz_rx();
}

/** Initialise the CC1101 for TX at @p freq_hz with AM-OOK modulation. */
static void subghz_setup_tx(uint32_t freq_hz) {
    furi_hal_subghz_reset();
    furi_hal_subghz_load_preset(FuriHalSubGhzPresetOok650Async);
    furi_hal_subghz_set_frequency_and_path(freq_hz);
}

/* ──────────────────────────── scan logic ─────────────────────────────── */

/**
 * Sweep every preset frequency for @p listen_ms and record those where
 * the measured RSSI exceeds the threshold.
 */
static void subghz_do_scan(SubGhzToolsApp* app) {
    app->hit_count = 0;

    furi_hal_subghz_acquire();

    for(size_t i = 0; i < PRESET_FREQ_COUNT && app->hit_count < SUBGHZ_MAX_HITS; i++) {
        uint32_t freq = PRESET_FREQS[i];
        subghz_setup_rx(freq);
        furi_delay_ms(SUBGHZ_LISTEN_MS);

        float rssi = furi_hal_subghz_get_rssi();
        if(rssi > SUBGHZ_RSSI_THRESHOLD) {
            app->hits[app->hit_count].frequency = freq;
            app->hits[app->hit_count].rssi = rssi;
            app->hit_count++;
        }
    }

    furi_hal_subghz_idle();
    furi_hal_subghz_release();
}

static void subghz_format_scan_results(SubGhzToolsApp* app, FuriString* out) {
    if(app->hit_count == 0) {
        furi_string_set(
            out,
            "No signals detected.\n\n"
            "Make sure a remote is being\n"
            "actively transmitted nearby.");
        return;
    }

    furi_string_printf(out, "Active frequencies (%zu):\n\n", app->hit_count);
    for(size_t i = 0; i < app->hit_count; i++) {
        uint32_t mhz = app->hits[i].frequency / 1000000UL;
        uint32_t khz = (app->hits[i].frequency % 1000000UL) / 1000UL;
        furi_string_cat_printf(
            out,
            "[%zu] %lu.%03lu MHz  RSSI: %.1f dBm\n",
            i + 1,
            mhz,
            khz,
            (double)app->hits[i].rssi);
    }
}

/* ──────────────────────────── capture logic ─────────────────────────── */

/**
 * Listen on the strongest previously detected frequency (or 433.92 MHz as
 * fallback) and fill app->capture with the first burst of OOK pulses.
 */
static void subghz_do_capture(SubGhzToolsApp* app) {
    SubGhzCapture* cap = &app->capture;
    cap->valid = false;

    /* Choose frequency: first scan hit or default */
    uint32_t freq = (app->hit_count > 0) ? app->hits[0].frequency : 433920000UL;
    cap->frequency = freq;

    if(!cap->data) {
        cap->data = malloc(SUBGHZ_CAPTURE_BUF_SIZE);
        if(!cap->data) return;
    }
    cap->data_len = 0;
    cap->rssi = -999.0f;

    furi_hal_subghz_acquire();
    subghz_setup_rx(freq);

    uint32_t deadline = furi_get_tick() + furi_ms_to_ticks(5000);
    bool capturing = false;

    while(furi_get_tick() < deadline && cap->data_len < SUBGHZ_CAPTURE_BUF_SIZE - 1) {
        float rssi = furi_hal_subghz_get_rssi();
        if(rssi > SUBGHZ_RSSI_THRESHOLD) {
            capturing = true;
            if(rssi > cap->rssi) cap->rssi = rssi;
            if(furi_hal_subghz_rx_pipe_not_empty()) {
                cap->data[cap->data_len++] = furi_hal_subghz_read_data();
            }
        } else if(capturing) {
            /* Signal ended — we have a burst */
            break;
        }
        furi_delay_us(100);
    }

    furi_hal_subghz_idle();
    furi_hal_subghz_release();

    cap->valid = (cap->data_len > 0);
}

static void subghz_format_capture(SubGhzCapture* cap, FuriString* out) {
    if(!cap->valid || cap->data_len == 0) {
        furi_string_set(
            out,
            "No signal captured.\n\n"
            "Press the remote button while\n"
            "capture is running and retry.");
        return;
    }

    uint32_t mhz = cap->frequency / 1000000UL;
    uint32_t khz = (cap->frequency % 1000000UL) / 1000UL;

    furi_string_printf(
        out,
        "Captured signal:\n"
        "  Freq : %lu.%03lu MHz\n"
        "  RSSI : %.1f dBm\n"
        "  Bytes: %zu\n\n"
        "Hex (first 32 B):\n",
        mhz,
        khz,
        (double)cap->rssi,
        cap->data_len);

    size_t preview = cap->data_len < 32 ? cap->data_len : 32;
    for(size_t i = 0; i < preview; i++) {
        furi_string_cat_printf(out, "%02X ", cap->data[i]);
        if((i + 1) % 8 == 0) furi_string_cat(out, "\n");
    }
    if(preview % 8 != 0) furi_string_cat(out, "\n");
}

/* ──────────────────────────── replay logic ──────────────────────────── */

static void subghz_do_replay(SubGhzToolsApp* app, FuriString* status_out) {
    SubGhzCapture* cap = &app->capture;

    if(!cap->valid || cap->data_len == 0) {
        furi_string_set(status_out, "Nothing to replay.\nCapture a signal first.");
        return;
    }

    furi_hal_subghz_acquire();
    subghz_setup_tx(cap->frequency);

    for(int rep = 0; rep < SUBGHZ_REPLAY_REPEAT; rep++) {
        furi_hal_subghz_tx();
        for(size_t i = 0; i < cap->data_len; i++) {
            /* Write byte to CC1101 TX FIFO and wait for it to drain */
            furi_hal_subghz_write_packet(cap->data + i, 1);
            furi_delay_us(200);
        }
        furi_hal_subghz_idle();
        furi_delay_ms(20);
    }

    furi_hal_subghz_release();

    uint32_t mhz = cap->frequency / 1000000UL;
    uint32_t khz = (cap->frequency % 1000000UL) / 1000UL;
    furi_string_printf(
        status_out,
        "Replayed %zu bytes\n"
        "at %lu.%03lu MHz\n"
        "(%d times).",
        cap->data_len,
        mhz,
        khz,
        SUBGHZ_REPLAY_REPEAT);
}

/* ──────────────────────────── menu callbacks ─────────────────────────── */

static void subghz_submenu_callback(void* ctx, uint32_t index) {
    SubGhzToolsApp* app = (SubGhzToolsApp*)ctx;
    FuriString* msg = furi_string_alloc();

    switch((SubGhzSubmenuIndex)index) {
    case SubGhzSubmenuScan:
        furi_string_set(msg, "Scanning...");
        text_box_set_text(app->text_box, furi_string_get_cstr(msg));
        view_dispatcher_switch_to_view(app->view_dispatcher, SubGhzViewTextBox);

        subghz_do_scan(app);
        subghz_format_scan_results(app, msg);
        text_box_set_text(app->text_box, furi_string_get_cstr(msg));
        text_box_set_focus(app->text_box, TextBoxFocusStart);
        break;

    case SubGhzSubmenuCapture:
        furi_string_set(msg, "Waiting for signal...\n(press remote button)");
        text_box_set_text(app->text_box, furi_string_get_cstr(msg));
        view_dispatcher_switch_to_view(app->view_dispatcher, SubGhzViewTextBox);

        subghz_do_capture(app);
        subghz_format_capture(&app->capture, msg);
        text_box_set_text(app->text_box, furi_string_get_cstr(msg));
        text_box_set_focus(app->text_box, TextBoxFocusStart);
        break;

    case SubGhzSubmenuReplay:
        subghz_do_replay(app, msg);
        text_box_set_text(app->text_box, furi_string_get_cstr(msg));
        text_box_set_focus(app->text_box, TextBoxFocusStart);
        view_dispatcher_switch_to_view(app->view_dispatcher, SubGhzViewTextBox);
        break;

    case SubGhzSubmenuAbout:
        popup_set_header(app->popup, "Sub-GHz Tools", 64, 8, AlignCenter, AlignTop);
        popup_set_text(
            app->popup,
            "Scan, capture & replay\nsub-GHz remotes.\n\nFor authorised testing only.",
            64,
            32,
            AlignCenter,
            AlignCenter);
        popup_set_timeout(app->popup, 4000);
        popup_enable_timeout(app->popup);
        view_dispatcher_switch_to_view(app->view_dispatcher, SubGhzViewPopup);
        break;
    }

    furi_string_free(msg);
}

static uint32_t subghz_back_to_submenu(void* ctx) {
    UNUSED(ctx);
    return SubGhzViewSubmenu;
}

static uint32_t subghz_exit_callback(void* ctx) {
    UNUSED(ctx);
    return VIEW_NONE;
}

/* ──────────────────────────── app lifecycle ──────────────────────────── */

static SubGhzToolsApp* subghz_tools_app_alloc(void) {
    SubGhzToolsApp* app = malloc(sizeof(SubGhzToolsApp));
    furi_check(app);
    memset(app, 0, sizeof(*app));

    app->gui = furi_record_open(RECORD_GUI);
    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_enable_queue(app->view_dispatcher);
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    /* Submenu */
    app->submenu = submenu_alloc();
    submenu_add_item(
        app->submenu, "Scan Frequencies", SubGhzSubmenuScan, subghz_submenu_callback, app);
    submenu_add_item(
        app->submenu, "Capture Signal", SubGhzSubmenuCapture, subghz_submenu_callback, app);
    submenu_add_item(
        app->submenu, "Replay Signal", SubGhzSubmenuReplay, subghz_submenu_callback, app);
    submenu_add_item(
        app->submenu, "About", SubGhzSubmenuAbout, subghz_submenu_callback, app);
    view_set_previous_callback(submenu_get_view(app->submenu), subghz_exit_callback);
    view_dispatcher_add_view(
        app->view_dispatcher, SubGhzViewSubmenu, submenu_get_view(app->submenu));

    /* TextBox for results */
    app->text_box = text_box_alloc();
    view_set_previous_callback(text_box_get_view(app->text_box), subghz_back_to_submenu);
    view_dispatcher_add_view(
        app->view_dispatcher, SubGhzViewTextBox, text_box_get_view(app->text_box));

    /* Popup for About */
    app->popup = popup_alloc();
    view_set_previous_callback(popup_get_view(app->popup), subghz_back_to_submenu);
    view_dispatcher_add_view(
        app->view_dispatcher, SubGhzViewPopup, popup_get_view(app->popup));

    return app;
}

static void subghz_tools_app_free(SubGhzToolsApp* app) {
    view_dispatcher_remove_view(app->view_dispatcher, SubGhzViewSubmenu);
    view_dispatcher_remove_view(app->view_dispatcher, SubGhzViewTextBox);
    view_dispatcher_remove_view(app->view_dispatcher, SubGhzViewPopup);

    submenu_free(app->submenu);
    text_box_free(app->text_box);
    popup_free(app->popup);

    view_dispatcher_free(app->view_dispatcher);
    furi_record_close(RECORD_GUI);

    if(app->capture.data) free(app->capture.data);
    free(app);
}

int32_t subghz_tools_app(void* p) {
    UNUSED(p);

    SubGhzToolsApp* app = subghz_tools_app_alloc();

    view_dispatcher_switch_to_view(app->view_dispatcher, SubGhzViewSubmenu);
    view_dispatcher_run(app->view_dispatcher);

    subghz_tools_app_free(app);
    return 0;
}
