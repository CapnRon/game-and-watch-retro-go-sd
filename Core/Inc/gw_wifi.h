#ifndef _GW_WIFI_H_
#define _GW_WIFI_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "stm32h7xx_hal.h"

extern UART_HandleTypeDef huart1;

typedef struct {
    char ssid[33];
    char password[65];
    char ping_host[64];
    char http_url[128];
    char throughput_host[64]; // needs a raw-byte-accepting echo service, not a real HTTP server
    int throughput_port;
    int tz_offset_hours; // applied to NIST's UTC response before setting the RTC
    bool valid;
} wifi_cfg_t;

typedef struct {
    bool connected;
    char ip[16];
    int rssi;
} wifi_status_t;

// Returns true if /wifi.cfg was found on the SD card and contained a non-empty ssid.
// ping_host/http_url are pre-filled with defaults if the file omits them.
bool wifi_cfg_load(wifi_cfg_t *cfg);

// Powers the module on (MOSFET gate + boot delay) and brings up USART1, or
// tears both down. Every entry/exit path of the WiFi Tools screen must pair
// these calls so the module is never left powered outside that screen.
void wifi_module_poweron(void);
void wifi_module_poweroff(void);

// Returning true from poll_cb aborts the wait early (used for a B-to-cancel
// button check during the long AT+CWJAP join). Either callback arg may be NULL.
typedef bool (*wifi_poll_cb_t)(void *ctx);

bool wifi_at_send_cmd_cb(const char *cmd, const char *expect_substr, uint32_t timeout_ms,
                          char *resp_buf, size_t resp_buf_len,
                          wifi_poll_cb_t poll_cb, void *ctx);

static inline bool wifi_at_send_cmd(const char *cmd, const char *expect_substr, uint32_t timeout_ms,
                                     char *resp_buf, size_t resp_buf_len)
{
    return wifi_at_send_cmd_cb(cmd, expect_substr, timeout_ms, resp_buf, resp_buf_len, NULL, NULL);
}

bool wifi_tcp_send(const char *payload, size_t len, uint32_t timeout_ms);

bool wifi_connect_cb(const wifi_cfg_t *cfg, wifi_status_t *status, wifi_poll_cb_t poll_cb, void *ctx);
bool wifi_ping_cb(const char *host, int *rtt_ms, wifi_poll_cb_t poll_cb, void *ctx);
bool wifi_http_get(const char *url, char *out_buf, size_t out_buf_len);
bool wifi_throughput_test(const char *host, uint16_t port, uint32_t *out_kbps);

// Fetches time.nist.gov's Daytime-protocol (port 13) response, applies
// tz_offset_hours, and sets the console's RTC via GW_SetUnixTM(). On success
// *out_tm holds the (already offset-adjusted, normalized) time that was set.
bool wifi_sync_time(int tz_offset_hours, struct tm *out_tm);

// The on-device configurator/testing toolkit screen, launched from the
// Options menu. Handles its own module power-on/off.
void wifi_tools_screen(void);

#endif
