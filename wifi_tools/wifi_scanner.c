/**
 * @file wifi_scanner.c
 * @brief WiFi Tools — Flipper Zero external application
 *
 * Communicates with the WiFi Devboard (ESP32-S2) over UART to:
 *   - Scan nearby access points (SSID, BSSID, channel, RSSI, encryption)
 *   - Report open / weak networks for further manual testing
 *
 * The ESP32 firmware must expose a simple AT-command interface:
 *   AT+CWMODE=1        — station mode
 *   AT+CWLAP           — list APs  (response: +CWLAP:(<enc>,<ssid>,<rssi>,<bssid>,<ch>))
 *
 * Build with uFBT or FBT:
 *   ufbt launch APP_DIR=wifi_tools
 */

#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_uart.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/text_box.h>
#include <gui/modules/popup.h>
#include <notification/notification.h>
#include <notification/notification_messages.h>
#include <stdlib.h>
#include <string.h>

/* ──────────────────────────── constants ──────────────────────────────── */

#define WIFI_UART_CHANNEL       FuriHalUartIdLP    /* USART1 on GPIO pins  */
#define WIFI_UART_BAUD          115200
#define WIFI_RX_BUF_SIZE        2048
#define WIFI_SCAN_TIMEOUT_MS    8000
#define WIFI_AT_EOL             "\r\n"
#define WIFI_MAX_APS            32

/* AT commands */
#define AT_PING                 "AT" WIFI_AT_EOL
#define AT_STATION_MODE         "AT+CWMODE=1" WIFI_AT_EOL
#define AT_SCAN                 "AT+CWLAP" WIFI_AT_EOL

/* encryption type strings (matches ESP-AT encoding) */
static const char* const ENCRYPT_NAMES[] = {
    "Open",  /* 0 */
    "WEP",   /* 1 */
    "WPA",   /* 2 */
    "WPA2",  /* 3 */
    "WPA/2", /* 4 */
    "WPA3",  /* 5 */
    "WPA2E", /* 6 */
    "WPA3E", /* 7 */
    "WAPI",  /* 8 */
};
#define ENCRYPT_NAMES_COUNT ((int)(sizeof(ENCRYPT_NAMES) / sizeof(ENCRYPT_NAMES[0])))

/* ──────────────────────────── data types ─────────────────────────────── */

typedef struct {
    char ssid[33];
    char bssid[18];
    int8_t rssi;
    uint8_t channel;
    uint8_t encryption; /* index into ENCRYPT_NAMES */
} WifiAP;

typedef enum {
    WifiViewSubmenu,
    WifiViewTextBox,
    WifiViewPopup,
} WifiView;

typedef enum {
    WifiSubmenuScan,
    WifiSubmenuAbout,
} WifiSubmenuIndex;

typedef struct {
    /* GUI */
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    Submenu* submenu;
    TextBox* text_box;
    Popup* popup;

    /* UART */
    FuriStreamBuffer* rx_stream;

    /* Scan results */
    WifiAP aps[WIFI_MAX_APS];
    size_t ap_count;
} WifiToolsApp;

/* ──────────────────────────── UART helpers ───────────────────────────── */

/** Callback invoked from ISR — pushes received bytes into the stream buffer. */
static void wifi_uart_rx_callback(UartIrqEvent event, uint8_t data, void* ctx) {
    if(event == UartIrqEventRXNE) {
        FuriStreamBuffer* stream = (FuriStreamBuffer*)ctx;
        furi_stream_buffer_send(stream, &data, 1, 0);
    }
}

static void wifi_uart_init(WifiToolsApp* app) {
    app->rx_stream = furi_stream_buffer_alloc(WIFI_RX_BUF_SIZE, 1);
    furi_hal_uart_init(WIFI_UART_CHANNEL, WIFI_UART_BAUD);
    furi_hal_uart_set_irq_cb(WIFI_UART_CHANNEL, wifi_uart_rx_callback, app->rx_stream);
}

static void wifi_uart_deinit(WifiToolsApp* app) {
    furi_hal_uart_set_irq_cb(WIFI_UART_CHANNEL, NULL, NULL);
    furi_hal_uart_deinit(WIFI_UART_CHANNEL);
    furi_stream_buffer_free(app->rx_stream);
}

/** Send a NUL-terminated AT command string. */
static void wifi_send_cmd(const char* cmd) {
    furi_hal_uart_tx(WIFI_UART_CHANNEL, (const uint8_t*)cmd, strlen(cmd));
}

/**
 * Read from the stream buffer until @p needle is found in the accumulated
 * response or @p timeout_ms elapses.  Returns the number of bytes collected
 * (always NUL-terminated in @p out).
 */
static size_t wifi_read_until(
    WifiToolsApp* app,
    const char* needle,
    char* out,
    size_t out_size,
    uint32_t timeout_ms) {
    size_t total = 0;
    uint32_t deadline = furi_get_tick() + furi_ms_to_ticks(timeout_ms);

    while(furi_get_tick() < deadline && total < out_size - 1) {
        size_t got = furi_stream_buffer_receive(
            app->rx_stream,
            (uint8_t*)out + total,
            1,
            furi_ms_to_ticks(50));
        if(got == 0) continue;
        total += got;
        out[total] = '\0';
        if(strstr(out, needle)) break;
    }
    out[total] = '\0';
    return total;
}

/* ──────────────────────────── scan logic ─────────────────────────────── */

/**
 * Parse a single +CWLAP line into a WifiAP struct.
 * Expected format:
 *   +CWLAP:(<enc>,<ssid>,<rssi>,<bssid>,<channel>,...)
 */
static bool wifi_parse_ap_line(const char* line, WifiAP* ap) {
    /* locate the opening parenthesis */
    const char* p = strchr(line, '(');
    if(!p) return false;
    p++;

    int enc = 0;
    int rssi = 0;
    int ch = 0;
    char ssid[33] = {0};
    char bssid[18] = {0};

    if(sscanf(p, "%d,\"%32[^\"]\",%d,\"%17[^\"]\",%d", &enc, ssid, &rssi, bssid, &ch) < 5)
        return false;

    ap->encryption = (uint8_t)((enc >= 0 && enc < ENCRYPT_NAMES_COUNT) ? enc : 0);
    strncpy(ap->ssid, ssid, sizeof(ap->ssid) - 1);
    strncpy(ap->bssid, bssid, sizeof(ap->bssid) - 1);
    ap->rssi = (int8_t)rssi;
    ap->channel = (uint8_t)ch;
    return true;
}

/** Perform a full WiFi scan and populate app->aps / app->ap_count. */
static bool wifi_do_scan(WifiToolsApp* app, FuriString* status_out) {
    char buf[WIFI_RX_BUF_SIZE];

    /* Ping the devboard */
    wifi_send_cmd(AT_PING);
    if(wifi_read_until(app, "OK", buf, sizeof(buf), 2000) == 0) {
        furi_string_set(status_out, "WiFi Devboard not detected.\nConnect the devboard and retry.");
        return false;
    }

    /* Set station mode */
    wifi_send_cmd(AT_STATION_MODE);
    wifi_read_until(app, "OK", buf, sizeof(buf), 2000);

    /* Trigger scan */
    furi_string_set(status_out, "Scanning...");
    wifi_send_cmd(AT_SCAN);

    size_t len = wifi_read_until(app, "OK\r\n", buf, sizeof(buf), WIFI_SCAN_TIMEOUT_MS);
    if(len == 0) {
        furi_string_set(status_out, "Scan timed out.\nCheck devboard connection.");
        return false;
    }

    /* Parse results */
    app->ap_count = 0;
    char* saveptr = NULL;
    char* line = strtok_r(buf, "\n", &saveptr);
    while(line && app->ap_count < WIFI_MAX_APS) {
        if(strncmp(line, "+CWLAP:", 7) == 0) {
            WifiAP ap = {0};
            if(wifi_parse_ap_line(line + 7, &ap)) {
                app->aps[app->ap_count++] = ap;
            }
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }
    return true;
}

/** Format scan results into a human-readable FuriString for the TextBox. */
static void wifi_format_results(WifiToolsApp* app, FuriString* out) {
    if(app->ap_count == 0) {
        furi_string_set(out, "No access points found.");
        return;
    }

    furi_string_printf(out, "Found %zu network(s):\n\n", app->ap_count);
    for(size_t i = 0; i < app->ap_count; i++) {
        WifiAP* ap = &app->aps[i];
        const char* enc =
            (ap->encryption < (uint8_t)ENCRYPT_NAMES_COUNT) ? ENCRYPT_NAMES[ap->encryption] :
                                                               "?";
        furi_string_cat_printf(
            out,
            "[%zu] %s\n"
            "  BSSID: %s\n"
            "  RSSI:  %d dBm  CH:%u  %s%s\n\n",
            i + 1,
            ap->ssid[0] ? ap->ssid : "(hidden)",
            ap->bssid,
            ap->rssi,
            ap->channel,
            enc,
            (ap->encryption == 0) ? "  <!> OPEN" : "");
    }
}

/* ──────────────────────────── menu callbacks ─────────────────────────── */

static void wifi_submenu_callback(void* ctx, uint32_t index) {
    WifiToolsApp* app = (WifiToolsApp*)ctx;

    if(index == WifiSubmenuScan) {
        FuriString* msg = furi_string_alloc();

        bool ok = wifi_do_scan(app, msg);
        if(ok) {
            wifi_format_results(app, msg);
        }

        text_box_set_text(app->text_box, furi_string_get_cstr(msg));
        text_box_set_focus(app->text_box, TextBoxFocusStart);
        furi_string_free(msg);
        view_dispatcher_switch_to_view(app->view_dispatcher, WifiViewTextBox);

    } else if(index == WifiSubmenuAbout) {
        popup_set_header(app->popup, "WiFi Tools", 64, 10, AlignCenter, AlignTop);
        popup_set_text(
            app->popup,
            "Scan nearby WiFi networks\n"
            "via the WiFi Devboard.\n"
            "\nFor authorised testing only.",
            64,
            32,
            AlignCenter,
            AlignCenter);
        popup_set_timeout(app->popup, 4000);
        popup_enable_timeout(app->popup);
        view_dispatcher_switch_to_view(app->view_dispatcher, WifiViewPopup);
    }
}

static uint32_t wifi_back_to_submenu(void* ctx) {
    UNUSED(ctx);
    return WifiViewSubmenu;
}

static uint32_t wifi_exit_callback(void* ctx) {
    UNUSED(ctx);
    return VIEW_NONE;
}

/* ──────────────────────────── app lifecycle ──────────────────────────── */

static WifiToolsApp* wifi_tools_app_alloc(void) {
    WifiToolsApp* app = malloc(sizeof(WifiToolsApp));
    furi_check(app);

    app->gui = furi_record_open(RECORD_GUI);
    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_enable_queue(app->view_dispatcher);
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    /* Submenu */
    app->submenu = submenu_alloc();
    submenu_add_item(app->submenu, "Scan Networks", WifiSubmenuScan, wifi_submenu_callback, app);
    submenu_add_item(app->submenu, "About", WifiSubmenuAbout, wifi_submenu_callback, app);
    view_set_previous_callback(submenu_get_view(app->submenu), wifi_exit_callback);
    view_dispatcher_add_view(app->view_dispatcher, WifiViewSubmenu, submenu_get_view(app->submenu));

    /* TextBox for results */
    app->text_box = text_box_alloc();
    view_set_previous_callback(text_box_get_view(app->text_box), wifi_back_to_submenu);
    view_dispatcher_add_view(
        app->view_dispatcher, WifiViewTextBox, text_box_get_view(app->text_box));

    /* Popup for About / errors */
    app->popup = popup_alloc();
    view_set_previous_callback(popup_get_view(app->popup), wifi_back_to_submenu);
    view_dispatcher_add_view(app->view_dispatcher, WifiViewPopup, popup_get_view(app->popup));

    return app;
}

static void wifi_tools_app_free(WifiToolsApp* app) {
    view_dispatcher_remove_view(app->view_dispatcher, WifiViewSubmenu);
    view_dispatcher_remove_view(app->view_dispatcher, WifiViewTextBox);
    view_dispatcher_remove_view(app->view_dispatcher, WifiViewPopup);

    submenu_free(app->submenu);
    text_box_free(app->text_box);
    popup_free(app->popup);

    view_dispatcher_free(app->view_dispatcher);
    furi_record_close(RECORD_GUI);
    free(app);
}

int32_t wifi_tools_app(void* p) {
    UNUSED(p);

    WifiToolsApp* app = wifi_tools_app_alloc();
    wifi_uart_init(app);

    view_dispatcher_switch_to_view(app->view_dispatcher, WifiViewSubmenu);
    view_dispatcher_run(app->view_dispatcher);

    wifi_uart_deinit(app);
    wifi_tools_app_free(app);
    return 0;
}
