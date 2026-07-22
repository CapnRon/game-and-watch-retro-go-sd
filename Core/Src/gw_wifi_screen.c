#include <string.h>
#include <stdio.h>
#include "main.h"
#include "gw_wifi.h"
#include "gw_lcd.h"
#include "odroid_overlay.h"
#include "odroid_input.h"
#include "odroid_colors.h"
#include "gui.h"
#include "rg_i18n.h"

// On-device WiFi configurator + testing toolkit, launched from the Options
// menu (see handle_options_menu() in rg_main.c). Credentials/test targets
// come from /wifi.cfg on the SD card -- there is no on-device text entry
// yet (no keyboard widget exists in this firmware).

typedef struct {
    wifi_cfg_t cfg;
    bool cfg_valid;
    bool connected;
    wifi_status_t status;
} wifi_screen_state_t;

static wifi_screen_state_t g_wifi;

static void wifi_screen_do_connect(void);
static void wifi_screen_do_ping(void);
static void wifi_screen_do_http_get(void);
static void wifi_screen_do_throughput(void);

/* Shared result screen: draws a title + a handful of text lines, then waits
 * for A (retry, if allow_retry) or B (back), petting the watchdog and
 * repainting every tick -- same cadence as sdcard_error_screen() in
 * gw_sdcard.c. Returns true if A was pressed. */
static bool wifi_show_result(const char *title, const char *lines[], int line_count, bool allow_retry)
{
    odroid_overlay_draw_fill_rect(0, 0, GW_LCD_WIDTH, GW_LCD_HEIGHT, curr_colors->bg_c);
    odroid_overlay_draw_text_line(4, 4, GW_LCD_WIDTH - 8, title, C_WHITE, curr_colors->bg_c);
    for (int i = 0; i < line_count; i++)
        odroid_overlay_draw_text_line(4, 20 + i * 12, GW_LCD_WIDTH - 8, lines[i], C_WHITE, curr_colors->bg_c);

    const char *prompt = allow_retry ? "A: Retry   B: Back" : "B: Back";
    odroid_overlay_draw_text_line(4, GW_LCD_HEIGHT - 12, GW_LCD_WIDTH - 8, prompt, C_RED, curr_colors->bg_c);

    while (1) {
        odroid_gamepad_state_t joystick;
        wdog_refresh();
        lcd_sync();
        lcd_swap();
        HAL_Delay(10);
        odroid_input_read_gamepad(&joystick);
        if (joystick.values[ODROID_INPUT_B])
            return false;
        if (allow_retry && joystick.values[ODROID_INPUT_A])
            return true;
    }
}

static bool wifi_connect_cancel_poll(void *ctx)
{
    (void)ctx;
    odroid_gamepad_state_t joystick;
    odroid_input_read_gamepad(&joystick);
    return joystick.values[ODROID_INPUT_B] != 0;
}

static void wifi_screen_do_connect(void)
{
    bool retry;
    do {
        char l0[64], l1[64], l2[64], l3[64];
        const char *lines[4];
        int nlines = 0;

        if (!g_wifi.cfg_valid) {
            snprintf(l0, sizeof(l0), "No /wifi.cfg found on SD card");
            snprintf(l1, sizeof(l1), "Add ssid=/password= and retry");
            lines[0] = l0; lines[1] = l1;
            wifi_show_result("Connect", lines, 2, false);
            return;
        }

        odroid_overlay_draw_fill_rect(0, 0, GW_LCD_WIDTH, GW_LCD_HEIGHT, curr_colors->bg_c);
        odroid_overlay_draw_text_line(4, 4, GW_LCD_WIDTH - 8, "WiFi Tools - Connect", C_WHITE, curr_colors->bg_c);
        char msg[64];
        snprintf(msg, sizeof(msg), "Connecting to %s...", g_wifi.cfg.ssid);
        odroid_overlay_draw_text_line(4, 20, GW_LCD_WIDTH - 8, msg, C_WHITE, curr_colors->bg_c);
        odroid_overlay_draw_text_line(4, GW_LCD_HEIGHT - 12, GW_LCD_WIDTH - 8, "B: Cancel", C_RED, curr_colors->bg_c);
        lcd_sync();
        lcd_swap();

        bool ok = wifi_connect_cb(&g_wifi.cfg, &g_wifi.status, wifi_connect_cancel_poll, NULL);
        g_wifi.connected = ok && g_wifi.status.connected;

        snprintf(l0, sizeof(l0), "SSID: %s", g_wifi.cfg.ssid);
        lines[nlines++] = l0;
        if (g_wifi.connected) {
            snprintf(l1, sizeof(l1), "IP: %s", g_wifi.status.ip);
            snprintf(l2, sizeof(l2), "RSSI: %d dBm", g_wifi.status.rssi);
            snprintf(l3, sizeof(l3), "Status: Connected");
            lines[nlines++] = l1;
            lines[nlines++] = l2;
            lines[nlines++] = l3;
        } else {
            snprintf(l1, sizeof(l1), "Status: Failed / cancelled");
            lines[nlines++] = l1;
        }

        retry = wifi_show_result("Connect", lines, nlines, true);
    } while (retry);
}

static void wifi_screen_do_ping(void)
{
    bool retry;
    do {
        int rtt = -1;
        bool ok = wifi_ping(g_wifi.cfg.ping_host, &rtt);

        char l0[72], l1[64];
        snprintf(l0, sizeof(l0), "Host: %s", g_wifi.cfg.ping_host);
        if (ok)
            snprintf(l1, sizeof(l1), "RTT: %d ms", rtt);
        else
            snprintf(l1, sizeof(l1), "Ping failed / timeout");

        const char *lines[2] = { l0, l1 };
        retry = wifi_show_result("Ping Test", lines, 2, true);
    } while (retry);
}

static void wifi_screen_do_http_get(void)
{
    static char resp[512];
    bool retry;
    do {
        bool ok = wifi_http_get(g_wifi.cfg.http_url, resp, sizeof(resp));

        char l0[64], l1[64];
        snprintf(l0, sizeof(l0), "URL: %.48s", g_wifi.cfg.http_url);
        if (ok) {
            char *eol = strpbrk(resp, "\r\n");
            if (eol) *eol = '\0';
            snprintf(l1, sizeof(l1), "%.48s", resp);
        } else {
            snprintf(l1, sizeof(l1), "GET failed");
        }

        const char *lines[2] = { l0, l1 };
        retry = wifi_show_result("HTTP GET Test", lines, 2, true);
    } while (retry);
}

static void wifi_screen_do_throughput(void)
{
    bool retry;
    do {
        char host[64] = {0};
        const char *p = g_wifi.cfg.http_url;
        if (strncmp(p, "http://", 7) == 0) p += 7;
        const char *slash = strchr(p, '/');
        size_t hostlen = slash ? (size_t)(slash - p) : strlen(p);
        if (hostlen >= sizeof(host)) hostlen = sizeof(host) - 1;
        memcpy(host, p, hostlen);

        odroid_overlay_draw_fill_rect(0, 0, GW_LCD_WIDTH, GW_LCD_HEIGHT, curr_colors->bg_c);
        odroid_overlay_draw_text_line(4, 4, GW_LCD_WIDTH - 8, "Throughput Test - sending...", C_WHITE, curr_colors->bg_c);
        lcd_sync();
        lcd_swap();

        uint32_t kbps = 0;
        bool ok = wifi_throughput_test(host, 80, &kbps);

        char l0[64], l1[64];
        snprintf(l0, sizeof(l0), "Host: %s", host);
        if (ok)
            snprintf(l1, sizeof(l1), "Throughput: %lu kbps (UART-limited)", (unsigned long)kbps);
        else
            snprintf(l1, sizeof(l1), "Transfer failed");

        const char *lines[2] = { l0, l1 };
        retry = wifi_show_result("Throughput Test", lines, 2, true);
    } while (retry);
}

void wifi_tools_screen(void)
{
    wifi_module_poweron();

    g_wifi.cfg_valid = wifi_cfg_load(&g_wifi.cfg);
    g_wifi.connected = false;

    bool exit_screen = false;
    while (!exit_screen) {
        odroid_dialog_choice_t choices[] = {
            {1, "Connect", "", 1, NULL},
            {2, "Ping Test", "", g_wifi.connected ? 1 : 0, NULL},
            {3, "HTTP GET Test", "", g_wifi.connected ? 1 : 0, NULL},
            {4, "Throughput Test", "", g_wifi.connected ? 1 : 0, NULL},
            ODROID_DIALOG_CHOICE_SEPARATOR,
            {0, "Back", "", 1, NULL},
            ODROID_DIALOG_CHOICE_LAST};

        int sel = odroid_overlay_dialog("WiFi Tools", choices, -1, &gui_redraw_callback, 0);
        switch (sel) {
            case 1: wifi_screen_do_connect(); break;
            case 2: wifi_screen_do_ping(); break;
            case 3: wifi_screen_do_http_get(); break;
            case 4: wifi_screen_do_throughput(); break;
            default: exit_screen = true; break;
        }
    }

    wifi_module_poweroff();
}
