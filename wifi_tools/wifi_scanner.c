/**
 * @file wifi_scanner.c
 * @brief WiFi Tools — Flipper Zero external application
 *
 * Defensive WiFi security analysis tool that communicates with the WiFi Devboard (ESP32-S2)
 * over UART to perform passive network analysis, security auditing, and channel monitoring.
 *
 * Features:
 *   - Network Scan: SSID, BSSID, channel, RSSI, encryption type
 *   - Security Audit: Identify weak encryption (Open, WEP, WPA-TKIP)
 *   - Channel Analysis: Visualize channel congestion
 *   - Network Monitoring: Track known vs unknown networks
 *
 * ESP32 firmware must expose ESP-AT command interface:
 *   AT+CWMODE=1        — station mode
 *   AT+CWLAP           — list APs
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
#include <gui/modules/variable_item_list.h>
#include <notification/notification.h>
#include <notification/notification_messages.h>
#include <stdlib.h>
#include <string.h>

/* ──────────────────────────── constants ──────────────────────────────── */

#define WIFI_UART_CHANNEL       FuriHalUartIdLP
#define WIFI_UART_BAUD          115200
#define WIFI_RX_BUF_SIZE        2048
#define WIFI_SCAN_TIMEOUT_MS    8000
#define WIFI_AT_EOL             "\r\n"
#define WIFI_MAX_APS            32
#define WIFI_MAX_KNOWN          20

/* AT commands */
#define AT_PING                 "AT" WIFI_AT_EOL
#define AT_STATION_MODE         "AT+CWMODE=1" WIFI_AT_EOL
#define AT_SCAN                 "AT+CWLAP" WIFI_AT_EOL

/* Encryption types (ESP-AT encoding) */
typedef enum {
    ENC_OPEN = 0,
    ENC_WEP = 1,
    ENC_WPA = 2,
    ENC_WPA2 = 3,
    ENC_WPA_WPA2 = 4,
    ENC_WPA3 = 5,
    ENC_WPA2_ENT = 6,
    ENC_WPA3_ENT = 7,
    ENC_WAPI = 8,
} EncryptionType;

static const char* const ENCRYPT_NAMES[] = {
    "Open",      /* 0 */
    "WEP",       /* 1 */
    "WPA",       /* 2 */
    "WPA2",      /* 3 */
    "WPA/WPA2",  /* 4 */
    "WPA3",      /* 5 */
    "WPA2-ENT",  /* 6 */
    "WPA3-ENT",  /* 7 */
    "WAPI",      /* 8 */
};
#define ENCRYPT_NAMES_COUNT ((int)(sizeof(ENCRYPT_NAMES) / sizeof(ENCRYPT_NAMES[0])))

/* Risk levels for security auditing */
typedef enum {
    RISK_LOW,
    RISK_MEDIUM,
    RISK_HIGH,
    RISK_CRITICAL,
} SecurityRisk;

/* ──────────────────────────── data types ─────────────────────────────── */

typedef struct {
    char ssid[33];
    char bssid[18];
    int8_t rssi;
    uint8_t channel;
    uint8_t encryption;
    SecurityRisk risk;
} WifiAP;

typedef struct {
    char ssid[33];
    char bssid[18];
} KnownNetwork;

typedef enum {
    WifiViewSubmenu,
    WifiViewTextBox,
    WifiViewPopup,
    WifiViewVariableList,
} WifiView;

typedef enum {
    WifiSubmenuScan,
    WifiSubmenuSecurityAudit,
    WifiSubmenuChannelAnalysis,
    WifiSubmenuMonitor,
    WifiSubmenuSettings,
    WifiSubmenuAbout,
} WifiSubmenuIndex;

typedef enum {
    WifiSettingFilter,
    WifiSettingSort,
} WifiSettingIndex;

typedef struct {
    /* GUI */
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    Submenu* submenu;
    TextBox* text_box;
    Popup* popup;
    VariableItemList* var_list;

    /* UART */
    FuriStreamBuffer* rx_stream;

    /* Scan results */
    WifiAP aps[WIFI_MAX_APS];
    size_t ap_count;

    /* Known networks for monitoring */
    KnownNetwork known[WIFI_MAX_KNOWN];
    size_t known_count;

    /* Settings */
    bool filter_weak;
    bool sort_by_signal;
} WifiToolsApp;

/* ──────────────────────────── UART helpers ───────────────────────────── */

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

static void wifi_send_cmd(const char* cmd) {
    furi_hal_uart_tx(WIFI_UART_CHANNEL, (const uint8_t*)cmd, strlen(cmd));
}

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

/* ──────────────────────────── security assessment ────────────────────── */

/**
 * Assess security risk based on encryption type.
 * Returns risk level for auditing purposes.
 */
static SecurityRisk assess_security_risk(uint8_t encryption) {
    switch(encryption) {
    case ENC_OPEN:
        return RISK_CRITICAL;
    case ENC_WEP:
        return RISK_CRITICAL;
    case ENC_WPA:
        return RISK_HIGH;
    case ENC_WPA_WPA2:
        return RISK_MEDIUM;
    case ENC_WPA2:
    case ENC_WPA2_ENT:
        return RISK_LOW;
    case ENC_WPA3:
    case ENC_WPA3_ENT:
        return RISK_LOW;
    default:
        return RISK_MEDIUM;
    }
}

static const char* get_risk_label(SecurityRisk risk) {
    switch(risk) {
    case RISK_CRITICAL: return "CRITICAL";
    case RISK_HIGH: return "HIGH";
    case RISK_MEDIUM: return "MEDIUM";
    case RISK_LOW: return "LOW";
    default: return "UNKNOWN";
    }
}

static const char* get_risk_advice(SecurityRisk risk) {
    switch(risk) {
    case RISK_CRITICAL:
        return "  <!> No encryption or WEP - All traffic visible\n"
               "      Immediate action required!";
    case RISK_HIGH:
        return "  [!] WPA-only (no WPA2/3) - Vulnerable to attacks\n"
               "      Upgrade to WPA2/WPA3 if possible";
    case RISK_MEDIUM:
        return "  [*] Mixed mode - Some clients may be vulnerable\n"
               "      Consider WPA3 transition mode";
    case RISK_LOW:
        return "  [+] Good encryption - Continue monitoring";
    default:
        return "  [?] Unknown encryption security";
    }
}

/* ──────────────────────────── scan logic ─────────────────────────────── */

static bool wifi_parse_ap_line(const char* line, WifiAP* ap) {
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
    ap->risk = assess_security_risk(ap->encryption);
    return true;
}

static bool wifi_do_scan(WifiToolsApp* app, FuriString* status_out) {
    char buf[WIFI_RX_BUF_SIZE];

    wifi_send_cmd(AT_PING);
    if(wifi_read_until(app, "OK", buf, sizeof(buf), 2000) == 0) {
        furi_string_set(status_out, "WiFi Devboard not detected.\nConnect the devboard and retry.");
        return false;
    }

    wifi_send_cmd(AT_STATION_MODE);
    wifi_read_until(app, "OK", buf, sizeof(buf), 2000);

    furi_string_set(status_out, "Scanning...");
    wifi_send_cmd(AT_SCAN);

    size_t len = wifi_read_until(app, "OK\r\n", buf, sizeof(buf), WIFI_SCAN_TIMEOUT_MS);
    if(len == 0) {
        furi_string_set(status_out, "Scan timed out.\nCheck devboard connection.");
        return false;
    }

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

/* ──────────────────────────── result formatting ──────────────────────── */

static int8_t rssi_to_quality(int8_t rssi) {
    if(rssi >= -50) return 100;
    if(rssi >= -60) return 75;
    if(rssi >= -70) return 50;
    if(rssi >= -80) return 25;
    return 10;
}

static void draw_signal_bars(char* out, int8_t rssi) {
    int8_t quality = rssi_to_quality(rssi);
    if(quality >= 75) {
        strcat(out, "████");
    } else if(quality >= 50) {
        strcat(out, "███░");
    } else if(quality >= 25) {
        strcat(out, "██░░");
    } else {
        strcat(out, "█░░░");
    }
}

static void wifi_format_results(WifiToolsApp* app, FuriString* out) {
    if(app->ap_count == 0) {
        furi_string_set(out, "No access points found.");
        return;
    }

    furi_string_printf(out, "WiFi Networks (%zu found)\n", app->ap_count);
    furi_string_cat_printf(out, "═\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\n");

    for(size_t i = 0; i < app->ap_count; i++) {
        WifiAP* ap = &app->aps[i];
        const char* enc = (ap->encryption < (uint8_t)ENCRYPT_NAMES_COUNT) ?
                          ENCRYPT_NAMES[ap->encryption] : "?";

        char signal[16] = {0};
        draw_signal_bars(signal, ap->rssi);

        furi_string_cat_printf(out,
            "#%zu %s\n"
            "  BSSID: %s\n"
            "  CH: %2u  RSSI: %4d dBm  %s\n"
            "  Security: %s [%s]\n",
            i + 1,
            ap->ssid[0] ? ap->ssid : "(hidden)",
            ap->bssid,
            ap->channel,
            ap->rssi,
            signal,
            enc,
            get_risk_label(ap->risk));

        if(ap->risk == RISK_CRITICAL || ap->risk == RISK_HIGH) {
            furi_string_cat_printf(out, "  %s\n", get_risk_advice(ap->risk));
        }
        furi_string_cat_printf(out, "\n");
    }
}

static void wifi_format_security_audit(WifiToolsApp* app, FuriString* out) {
    size_t critical = 0, high = 0, medium = 0, low = 0;

    furi_string_set(out, "SECURITY AUDIT REPORT\n");
    furi_string_cat_printf(out, "═\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\n\n");

    for(size_t i = 0; i < app->ap_count; i++) {
        switch(app->aps[i].risk) {
        case RISK_CRITICAL: critical++; break;
        case RISK_HIGH: high++; break;
        case RISK_MEDIUM: medium++; break;
        case RISK_LOW: low++; break;
        }
    }

    furi_string_cat_printf(out,
        "SUMMARY\n"
        "  Critical: %zu  High: %zu  Medium: %zu  Low: %zu\n\n",
        critical, high, medium, low);

    if(critical > 0 || high > 0) {
        furi_string_cat_printf(out,
            "⚠️  SECURITY CONCERNS DETECTED\n"
            "═\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\n\n");
    }

    for(size_t i = 0; i < app->ap_count; i++) {
        WifiAP* ap = &app->aps[i];
        if(ap->risk == RISK_CRITICAL || ap->risk == RISK_HIGH) {
            const char* enc = (ap->encryption < (uint8_t)ENCRYPT_NAMES_COUNT) ?
                              ENCRYPT_NAMES[ap->encryption] : "?";
            furi_string_cat_printf(out,
                "[%s] %s\n"
                "  Encryption: %s\n"
                "  Risk Level: %s\n%s\n\n",
                get_risk_label(ap->risk),
                ap->ssid[0] ? ap->ssid : "(hidden)",
                enc,
                get_risk_label(ap->risk),
                get_risk_advice(ap->risk));
        }
    }

    if(critical == 0 && high == 0) {
        furi_string_cat_printf(out,
            "✓ No critical or high-risk networks detected.\n"
            "  All networks use WPA2 or WPA3 encryption.\n\n");
    }

    furi_string_cat_printf(out,
        "═\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\n"
        "Recommendations:\n"
        "• Use WPA3 where supported\n"
        "• Disable WPS on all access points\n"
        "• Use strong passphrases (20+ chars)\n"
        "• Regularly audit your network security\n");
}

static void wifi_format_channel_analysis(WifiToolsApp* app, FuriString* out) {
    uint8_t channel_count[14] = {0};
    uint8_t max_count = 0;

    furi_string_set(out, "CHANNEL ANALYSIS\n");
    furi_string_cat_printf(out, "═\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\n\n");

    for(size_t i = 0; i < app->ap_count; i++) {
        uint8_t ch = app->aps[i].channel;
        if(ch >= 1 && ch <= 13) {
            channel_count[ch - 1]++;
            if(channel_count[ch - 1] > max_count) {
                max_count = channel_count[ch - 1];
            }
        }
    }

    furi_string_cat_printf(out, "Channel Usage (2.4 GHz):\n\n");

    for(uint8_t ch = 1; ch <= 13; ch++) {
        uint8_t count = channel_count[ch - 1];
        furi_string_cat_printf(out, "CH %2u: ", ch);

        if(count == 0) {
            furi_string_cat_printf(out, "      (clear)");
        } else {
            for(uint8_t i = 0; i < count; i++) {
                furi_string_cat_printf(out, "█");
            }
            furi_string_cat_printf(out, " (%u)", count);
        }

        if(ch == 1 || ch == 6 || ch == 11) {
            furi_string_cat_printf(out, " ← Non-overlapping");
        }
        furi_string_cat_printf(out, "\n");
    }

    furi_string_cat_printf(out, "\n");
    furi_string_cat_printf(out, "═\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\n");
    furi_string_cat_printf(out, "Best channels for AP setup: ");

    uint8_t best_ch = 1;
    for(uint8_t ch = 1; ch <= 13; ch++) {
        if(channel_count[ch - 1] < channel_count[best_ch - 1]) {
            best_ch = ch;
        }
    }

    furi_string_cat_printf(out, "%u (least congested)\n", best_ch);
    furi_string_cat_printf(out, "Recommended: 1, 6, or 11\n");

    if(max_count > 5) {
        furi_string_cat_printf(out, "\n[!] High congestion detected - consider 5 GHz\n");
    }
}

static void wifi_format_monitor(WifiToolsApp* app, FuriString* out, bool* changes_detected) {
    size_t new_count = 0;
    size_t missing_count = 0;

    furi_string_set(out, "NETWORK MONITOR\n");
    furi_string_cat_printf(out, "═\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\n\n");

    if(app->known_count == 0) {
        furi_string_cat_printf(out,
            "No known networks registered.\n"
            "Current scan will be used as baseline.\n\n");

        app->known_count = (app->ap_count < WIFI_MAX_KNOWN) ? app->ap_count : WIFI_MAX_KNOWN;
        for(size_t i = 0; i < app->known_count; i++) {
            strncpy(app->known[i].ssid, app->aps[i].ssid, 32);
            strncpy(app->known[i].bssid, app->aps[i].bssid, 17);
        }
        furi_string_cat_printf(out, "Registered %zu networks as known.\n", app->known_count);
        *changes_detected = false;
        return;
    }

    for(size_t i = 0; i < app->ap_count; i++) {
        bool found = false;
        for(size_t j = 0; j < app->known_count; j++) {
            if(strcmp(app->aps[i].bssid, app->known[j].bssid) == 0) {
                found = true;
                break;
            }
        }
        if(!found) {
            new_count++;
            furi_string_cat_printf(out,
                "[+] NEW: %s (%s)\n"
                "    CH: %u  RSSI: %d dBm  %s\n\n",
                app->aps[i].ssid[0] ? app->aps[i].ssid : "(hidden)",
                app->aps[i].bssid,
                app->aps[i].channel,
                app->aps[i].rssi,
                ENCRYPT_NAMES[app->aps[i].encryption]);
        }
    }

    for(size_t j = 0; j < app->known_count; j++) {
        bool found = false;
        for(size_t i = 0; i < app->ap_count; i++) {
            if(strcmp(app->aps[i].bssid, app->known[j].bssid) == 0) {
                found = true;
                break;
            }
        }
        if(!found) {
            missing_count++;
            furi_string_cat_printf(out, "[-] MISSING: %s (%s)\n",
                app->known[j].ssid, app->known[j].bssid);
        }
    }

    *changes_detected = (new_count > 0 || missing_count > 0);

    if(!*changes_detected) {
        furi_string_cat_printf(out, "✓ No changes detected.\n");
        furi_string_cat_printf(out, "  %zu known networks present.\n", app->known_count);
    } else {
        furi_string_cat_printf(out, "\n");
        furi_string_cat_printf(out, "═\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\n");
        furi_string_cat_printf(out, "Summary: +%zu new, -%zu missing\n", new_count, missing_count);
    }
}

/* ──────────────────────────── menu callbacks ─────────────────────────── */

static void wifi_submenu_scan_callback(void* ctx) {
    WifiToolsApp* app = (WifiToolsApp*)ctx;
    FuriString* msg = furi_string_alloc();

    bool ok = wifi_do_scan(app, msg);
    if(ok) {
        wifi_format_results(app, msg);
    }

    text_box_set_text(app->text_box, furi_string_get_cstr(msg));
    text_box_set_focus(app->text_box, TextBoxFocusStart);
    furi_string_free(msg);
    view_dispatcher_switch_to_view(app->view_dispatcher, WifiViewTextBox);
}

static void wifi_submenu_audit_callback(void* ctx) {
    WifiToolsApp* app = (WifiToolsApp*)ctx;
    FuriString* msg = furi_string_alloc();

    bool ok = wifi_do_scan(app, msg);
    if(ok) {
        wifi_format_security_audit(app, msg);
    }

    text_box_set_text(app->text_box, furi_string_get_cstr(msg));
    text_box_set_focus(app->text_box, TextBoxFocusStart);
    furi_string_free(msg);
    view_dispatcher_switch_to_view(app->view_dispatcher, WifiViewTextBox);
}

static void wifi_submenu_channel_callback(void* ctx) {
    WifiToolsApp* app = (WifiToolsApp*)ctx;
    FuriString* msg = furi_string_alloc();

    bool ok = wifi_do_scan(app, msg);
    if(ok) {
        wifi_format_channel_analysis(app, msg);
    }

    text_box_set_text(app->text_box, furi_string_get_cstr(msg));
    text_box_set_focus(app->text_box, TextBoxFocusStart);
    furi_string_free(msg);
    view_dispatcher_switch_to_view(app->view_dispatcher, WifiViewTextBox);
}

static void wifi_submenu_monitor_callback(void* ctx) {
    WifiToolsApp* app = (WifiToolsApp*)ctx;
    FuriString* msg = furi_string_alloc();
    bool changes = false;

    bool ok = wifi_do_scan(app, msg);
    if(ok) {
        wifi_format_monitor(app, msg, &changes);
    }

    text_box_set_text(app->text_box, furi_string_get_cstr(msg));
    text_box_set_focus(app->text_box, TextBoxFocusStart);

    if(changes) {
        notification_message(app->gui, &sequence_notification);
    }

    furi_string_free(msg);
    view_dispatcher_switch_to_view(app->view_dispatcher, WifiViewTextBox);
}

static void wifi_submenu_about_callback(void* ctx) {
    WifiToolsApp* app = (WifiToolsApp*)ctx;

    popup_set_header(app->popup, "WiFi Tools v2.0", 64, 10, AlignCenter, AlignTop);
    popup_set_text(
        app->popup,
        "Defensive WiFi Analysis\n"
        "\n"
        "• Network Scanning\n"
        "• Security Auditing\n"
        "• Channel Analysis\n"
        "• Network Monitoring\n"
        "\n"
        "For authorized testing only.",
        64,
        32,
        AlignCenter,
        AlignCenter);
    popup_set_timeout(app->popup, 5000);
    popup_enable_timeout(app->popup);
    view_dispatcher_switch_to_view(app->view_dispatcher, WifiViewPopup);
}

static void wifi_submenu_callback(void* ctx, uint32_t index) {
    WifiToolsApp* app = (WifiToolsApp*)ctx;

    switch(index) {
    case WifiSubmenuScan:
        wifi_submenu_scan_callback(app);
        break;
    case WifiSubmenuSecurityAudit:
        wifi_submenu_audit_callback(app);
        break;
    case WifiSubmenuChannelAnalysis:
        wifi_submenu_channel_callback(app);
        break;
    case WifiSubmenuMonitor:
        wifi_submenu_monitor_callback(app);
        break;
    case WifiSubmenuAbout:
        wifi_submenu_about_callback(app);
        break;
    default:
        break;
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

    app->filter_weak = false;
    app->sort_by_signal = true;
    app->known_count = 0;

    app->submenu = submenu_alloc();
    submenu_add_item(app->submenu, "Scan Networks", WifiSubmenuScan, wifi_submenu_callback, app);
    submenu_add_item(app->submenu, "Security Audit", WifiSubmenuSecurityAudit, wifi_submenu_callback, app);
    submenu_add_item(app->submenu, "Channel Analysis", WifiSubmenuChannelAnalysis, wifi_submenu_callback, app);
    submenu_add_item(app->submenu, "Network Monitor", WifiSubmenuMonitor, wifi_submenu_callback, app);
    submenu_add_item(app->submenu, "About", WifiSubmenuAbout, wifi_submenu_callback, app);
    view_set_previous_callback(submenu_get_view(app->submenu), wifi_exit_callback);
    view_dispatcher_add_view(app->view_dispatcher, WifiViewSubmenu, submenu_get_view(app->submenu));

    app->text_box = text_box_alloc();
    view_set_previous_callback(text_box_get_view(app->text_box), wifi_back_to_submenu);
    view_dispatcher_add_view(
        app->view_dispatcher, WifiViewTextBox, text_box_get_view(app->text_box));

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
