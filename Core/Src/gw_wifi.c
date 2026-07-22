#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "main.h"
#include "gw_wifi.h"
#include "ff.h"
#include "rg_rtc.h"

UART_HandleTypeDef huart1;

#define WIFI_RXBUF_SIZE 512
static volatile uint8_t  wifi_rxbuf[WIFI_RXBUF_SIZE];
static volatile uint16_t wifi_rx_head = 0;
static volatile uint16_t wifi_rx_tail = 0;
static uint8_t wifi_rx_byte;

/*************************
 * USART1 RX ring buffer  *
 *************************/

static void wifi_rx_rearm(void)
{
    HAL_UART_Receive_IT(&huart1, &wifi_rx_byte, 1);
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance != USART1)
        return;

    uint16_t next = (uint16_t)((wifi_rx_head + 1) % WIFI_RXBUF_SIZE);
    if (next != wifi_rx_tail) { // drop byte on overflow rather than corrupt the buffer
        wifi_rxbuf[wifi_rx_head] = wifi_rx_byte;
        wifi_rx_head = next;
    }
    wifi_rx_rearm();
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance != USART1)
        return;
    wifi_rx_rearm(); // framing/overrun errors otherwise stall RX permanently
}

static void wifi_rx_flush(void)
{
    wifi_rx_tail = wifi_rx_head;
}

static void wifi_rx_drain_into(char *buf, size_t buf_len, size_t *len_inout)
{
    while (wifi_rx_tail != wifi_rx_head && *len_inout < buf_len - 1) {
        buf[(*len_inout)++] = (char)wifi_rxbuf[wifi_rx_tail];
        wifi_rx_tail = (uint16_t)((wifi_rx_tail + 1) % WIFI_RXBUF_SIZE);
    }
    buf[*len_inout] = '\0';
}

/*************************
 * Power + UART bring-up *
 *************************/

// NOTE: 2,000,000 was bench-confirmed reliable on the D1 Mini Pro's own
// USB-to-board wiring (20/20 short commands, clean 100 KB bulk transfer),
// but on the actual console it silently fell back to 115200 via the
// self-healing logic below -- the console's hand-soldered wiring has
// different signal characteristics than the bench USB-serial dongle, and
// bench results don't necessarily transfer 1:1. 1,152,000 is the highest
// rate confirmed directly on the console itself (918 kbps measured, near
// that rate's ~921.6 kbps theoretical ceiling). Trying 1,728,000 here as a
// candidate between the two -- if it's also too fast for this specific
// wiring, wifi_switch_baud()'s self-healing fallback drops back to 115200
// safely rather than desyncing, so this is a safe value to test directly on
// the real hardware rather than only on the bench D1 Mini Pro.
#define WIFI_TARGET_BAUD 1728000

static void wifi_uart_init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_USART1_CLK_ENABLE();

    GPIO_InitStruct.Pin = WIFI_UART_TX_Pin | WIFI_UART_RX_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH; // needs clean edges up to WIFI_TARGET_BAUD
    GPIO_InitStruct.Alternate = GPIO_AF7_USART1;
    HAL_GPIO_Init(WIFI_UART_TX_GPIO_Port, &GPIO_InitStruct);

    huart1.Instance = USART1;
    huart1.Init.BaudRate = 115200; // module's factory-default; bumped after liveness check
    huart1.Init.WordLength = UART_WORDLENGTH_8B;
    huart1.Init.StopBits = UART_STOPBITS_1;
    huart1.Init.Parity = UART_PARITY_NONE;
    huart1.Init.Mode = UART_MODE_TX_RX;
    huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    huart1.Init.OverSampling = UART_OVERSAMPLING_16;
    HAL_UART_Init(&huart1);

    HAL_NVIC_SetPriority(USART1_IRQn, 6, 0);
    HAL_NVIC_EnableIRQ(USART1_IRQn);

    wifi_rx_head = 0;
    wifi_rx_tail = 0;
    wifi_rx_rearm();
}

static void wifi_uart_deinit(void)
{
    HAL_NVIC_DisableIRQ(USART1_IRQn);
    HAL_UART_DeInit(&huart1);
}

static void wifi_reconfigure_baud(uint32_t baud)
{
    HAL_NVIC_DisableIRQ(USART1_IRQn);
    huart1.Init.BaudRate = baud;
    HAL_UART_Init(&huart1);
    HAL_NVIC_EnableIRQ(USART1_IRQn);

    wifi_rx_head = 0;
    wifi_rx_tail = 0;
    wifi_rx_rearm();
}

// Bench-confirmed live: AT+UART_CUR's OK response arrives at the OLD baud
// rate, and the module only switches to the new rate afterward. BUT: if the
// module actually processed the command and switched while our own OK
// detection merely missed it (real risk on the soldered board, which may
// have different signal timing than the bench setup this was verified
// against), blindly trusting that detection would leave the STM32 on the
// old baud while the module is already on the new one -- desynced, breaking
// every AT command after it, including Connect's own liveness check. So:
// always attempt the switch and PROVE the new baud actually works with a
// fresh "AT"; if that fails, fall back to the old baud and re-verify there,
// so we never end up guessing which baud the module is actually on.
static bool wifi_switch_baud(uint32_t new_baud)
{
    char cmd[48];
    uint32_t old_baud = huart1.Init.BaudRate;

    snprintf(cmd, sizeof(cmd), "AT+UART_CUR=%lu,8,1,0,0", (unsigned long)new_baud);
    wifi_at_send_cmd(cmd, "OK", 2000, NULL, 0); // outcome checked below via a live probe, not this return value

    wifi_reconfigure_baud(new_baud);
    if (wifi_at_send_cmd("AT", "OK", 500, NULL, 0))
        return true; // confirmed alive at the new baud

    wifi_reconfigure_baud(old_baud); // new baud didn't answer -- fall back and re-verify there
    return wifi_at_send_cmd("AT", "OK", 500, NULL, 0);
}

void wifi_module_poweron(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    HAL_GPIO_WritePin(WIFI_PWR_GPIO_Port, WIFI_PWR_Pin, GPIO_PIN_RESET);
    GPIO_InitStruct.Pin = WIFI_PWR_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(WIFI_PWR_GPIO_Port, &GPIO_InitStruct);

    HAL_GPIO_WritePin(WIFI_PWR_GPIO_Port, WIFI_PWR_Pin, GPIO_PIN_SET);

    // ESP8266 boot + AT firmware ready time. A single unbroken HAL_Delay(2000)
    // blocks the watchdog from being petted for the whole 2s, which is longer
    // than this project's WWDG window tolerates -- every other wait in this
    // driver pets it every ~5ms (see wifi_at_send_cmd_cb) and this must match.
    for (int waited_ms = 0; waited_ms < 2000; waited_ms += 20) {
        wdog_refresh();
        HAL_Delay(20);
    }

    wifi_uart_init(); // brings USART1 up at 115200, the module's factory default

    // Re-enabled: the Connect regression this was suspected of causing
    // turned out to be a disconnected solder joint, not this logic -- ruled
    // out in software by verifying flash integrity byte-for-byte and by
    // confirming Connect still failed even with this path fully disabled.
    if (wifi_at_send_cmd("AT", "OK", 1000, NULL, 0))
        wifi_switch_baud(WIFI_TARGET_BAUD);
}

void wifi_module_poweroff(void)
{
    wifi_uart_deinit();
    HAL_GPIO_WritePin(WIFI_PWR_GPIO_Port, WIFI_PWR_Pin, GPIO_PIN_RESET);
}

/*************************
 * AT command layer      *
 *************************/

bool wifi_at_send_cmd_cb(const char *cmd, const char *expect_substr, uint32_t timeout_ms,
                          char *resp_buf, size_t resp_buf_len,
                          wifi_poll_cb_t poll_cb, void *ctx)
{
    char line[160];
    char local_buf[256];
    char *buf = resp_buf ? resp_buf : local_buf;
    size_t buf_len = resp_buf ? resp_buf_len : sizeof(local_buf);
    int n = snprintf(line, sizeof(line), "%s\r\n", cmd);

    wifi_rx_flush();
    buf[0] = '\0';

    HAL_UART_Transmit(&huart1, (uint8_t *)line, n, 500);

    size_t len = 0;
    uint32_t start = HAL_GetTick();
    bool found = false;

    while ((HAL_GetTick() - start) < timeout_ms) {
        wdog_refresh();
        wifi_rx_drain_into(buf, buf_len, &len);
        if (expect_substr && strstr(buf, expect_substr)) { found = true; break; }
        if (strstr(buf, "ERROR") || strstr(buf, "FAIL")) break;
        if (poll_cb && poll_cb(ctx)) break; // caller requested cancel
        HAL_Delay(5);
    }
    return found;
}

bool wifi_tcp_send(const char *payload, size_t len, uint32_t timeout_ms)
{
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "AT+CIPSEND=%u", (unsigned)len);

    if (!wifi_at_send_cmd(cmd, ">", 2000, NULL, 0))
        return false;

    HAL_UART_Transmit(&huart1, (uint8_t *)payload, len, 2000);

    char resp[64];
    resp[0] = '\0';
    size_t rlen = 0;
    uint32_t start = HAL_GetTick();
    while ((HAL_GetTick() - start) < timeout_ms) {
        wdog_refresh();
        wifi_rx_drain_into(resp, sizeof(resp), &rlen);
        if (strstr(resp, "SEND OK")) return true;
        if (strstr(resp, "ERROR")) return false;
        HAL_Delay(5);
    }
    return false;
}

/*************************
 * Higher-level flows    *
 *************************/

bool wifi_connect_cb(const wifi_cfg_t *cfg, wifi_status_t *status, wifi_poll_cb_t poll_cb, void *ctx)
{
    char cmd[160];
    char resp[256];

    memset(status, 0, sizeof(*status));

    if (!wifi_at_send_cmd("AT", "OK", 1000, resp, sizeof(resp)))
        return false;

    if (!wifi_at_send_cmd("AT+CWMODE=1", "OK", 1000, resp, sizeof(resp)))
        return false;

    snprintf(cmd, sizeof(cmd), "AT+CWJAP=\"%s\",\"%s\"", cfg->ssid, cfg->password);
    if (!wifi_at_send_cmd_cb(cmd, "OK", 15000, resp, sizeof(resp), poll_cb, ctx))
        return false;

    if (wifi_at_send_cmd("AT+CIFSR", "+CIFSR:STAIP", 1000, resp, sizeof(resp))) {
        char *p = strstr(resp, "+CIFSR:STAIP,\"");
        if (p) {
            p += strlen("+CIFSR:STAIP,\"");
            char *end = strchr(p, '"');
            if (end) {
                size_t iplen = (size_t)(end - p);
                if (iplen >= sizeof(status->ip)) iplen = sizeof(status->ip) - 1;
                memcpy(status->ip, p, iplen);
                status->ip[iplen] = '\0';
            }
        }
    }

    // Bench-confirmed live against this exact hardware/firmware (join
    // actually succeeded against a real AP during testing):
    //   +CWJAP:<ssid>,<bssid>,<channel>,<rssi>,<unknown 5th field>
    // Two firmware quirks confirmed by the real response, not docs:
    //  1. 5 comma-separated fields, not 4 -- RSSI is the 4th, not the last.
    //  2. RSSI is a known-buggy uint8_t instead of int8_t (matches
    //     espressif/ESP8266_NONOS_SDK issue #359): a real -62 dBm reading
    //     came back as 194 (194 - 256 == -62). Reinterpret as signed.
    //
    // Wait for "OK" rather than "+CWJAP": the device echoes the command
    // first, and "AT+CWJAP?" itself contains the literal substring
    // "+CWJAP" -- waiting for that would match the echo instantly, before
    // the real "+CWJAP:..." response line has arrived.
    wifi_at_send_cmd("AT+CWJAP?", "OK", 1000, resp, sizeof(resp));
    {
        char *tag = strstr(resp, "+CWJAP:");
        if (tag) {
            char *p = tag + strlen("+CWJAP:");
            for (int commas = 0; commas < 3 && p; commas++) {
                p = strchr(p, ',');
                if (p) p++;
            }
            if (p) {
                int raw = atoi(p);
                status->rssi = (raw > 127) ? (raw - 256) : raw;
            }
        }
    }

    status->connected = true;
    return true;
}

bool wifi_ping_cb(const char *host, int *rtt_ms, wifi_poll_cb_t poll_cb, void *ctx)
{
    char cmd[96];
    char resp[128];

    // Bench-confirmed on the actual hardware (ESP8266_NONOS_SDK legacy AT
    // firmware v1.7.6.0, verified live -- there is no "PING:" tag at all):
    //   success: +<time_ms>\r\n\r\nOK      failure: +timeout\r\n\r\nERROR
    // The device echoes the command first, and "AT+PING" itself contains a
    // literal '+' -- waiting for bare "+" would match that echo instantly,
    // so wait for "OK" (loop already breaks early on "ERROR" too) and only
    // search for the result '+' in the buffer *after* the echoed command.
    snprintf(cmd, sizeof(cmd), "AT+PING=\"%s\"", host);
    wifi_at_send_cmd_cb(cmd, "OK", 3000, resp, sizeof(resp), poll_cb, ctx);

    size_t echolen = strlen(cmd);
    if (strlen(resp) <= echolen)
        return false;
    char *body = resp + echolen;

    if (strstr(body, "timeout") || strstr(body, "ERROR"))
        return false;

    char *p = strchr(body, '+');
    if (!p)
        return false;

    *rtt_ms = atoi(p + 1);
    return true;
}

bool wifi_http_get(const char *url, char *out_buf, size_t out_buf_len)
{
    char host[64] = {0};
    char path[128] = "/";
    char cmd[96];
    char req[256];

    const char *p = url;
    if (strncmp(p, "http://", 7) == 0) p += 7;
    const char *slash = strchr(p, '/');
    size_t hostlen = slash ? (size_t)(slash - p) : strlen(p);
    if (hostlen >= sizeof(host)) hostlen = sizeof(host) - 1;
    memcpy(host, p, hostlen);
    host[hostlen] = '\0';
    if (slash) strncpy(path, slash, sizeof(path) - 1);

    snprintf(cmd, sizeof(cmd), "AT+CIPSTART=\"TCP\",\"%s\",80", host);
    // Stock AT firmware replies "CONNECT\r\nOK" on some versions, just "OK" on
    // others -- accept either (verify exact string against the bench-flashed
    // firmware per the plan's version-risk note).
    wifi_at_send_cmd(cmd, "OK", 5000, NULL, 0);

    int reqlen = snprintf(req, sizeof(req),
                           "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n",
                           path, host);

    if (!wifi_tcp_send(req, (size_t)reqlen, 2000)) {
        wifi_at_send_cmd("AT+CIPCLOSE", "OK", 1000, NULL, 0);
        return false;
    }

    out_buf[0] = '\0';
    size_t len = 0;
    uint32_t start = HAL_GetTick();
    while ((HAL_GetTick() - start) < 3000) {
        wdog_refresh();
        wifi_rx_drain_into(out_buf, out_buf_len, &len);
        HAL_Delay(20);
    }

    wifi_at_send_cmd("AT+CIPCLOSE", "OK", 1000, NULL, 0);
    return len > 0;
}

// Bench-confirmed live against this exact firmware: the original design
// (repeated AT+CIPSEND=<len> + wait-for-ack per chunk) measures round-trip
// latency to the remote host, not local UART/WiFi bit-rate -- each chunk's
// time is dominated by the network round-trip, so raising the UART baud
// barely moved the number (measured ~24 kbps regardless of baud). Espressif's
// AT+CIPMODE=1 "transparent transmission" mode removes the per-chunk
// command/ack round-trip entirely: after one AT+CIPSEND (no length arg) and
// its ">" prompt, everything written to the UART streams straight to the
// TCP connection. Confirmed end-to-end on real hardware: 55 kbps at 115200
// baud vs 113.7 kbps at 1,152,000 baud on the same link -- this design
// actually reflects the baud change, unlike the old one.
bool wifi_throughput_test(const char *host, uint16_t port, uint32_t *out_kbps)
{
    char cmd[96];
    static uint8_t payload[1024];
    const int num_chunks = 48; // ~48 KB total, matches the bench-tested scale

    if (!wifi_at_send_cmd("AT+CIPMUX=0", "OK", 1000, NULL, 0)) // passthrough needs single-connection mode
        return false;

    snprintf(cmd, sizeof(cmd), "AT+CIPSTART=\"TCP\",\"%s\",%u", host, port);
    if (!wifi_at_send_cmd(cmd, "OK", 5000, NULL, 0))
        return false;

    if (!wifi_at_send_cmd("AT+CIPMODE=1", "OK", 1000, NULL, 0)) {
        wifi_at_send_cmd("AT+CIPCLOSE", "OK", 1000, NULL, 0);
        return false;
    }

    if (!wifi_at_send_cmd("AT+CIPSEND", ">", 2000, NULL, 0)) {
        wifi_at_send_cmd("AT+CIPCLOSE", "OK", 1000, NULL, 0);
        return false;
    }

    memset(payload, 'A', sizeof(payload));

    uint32_t start = HAL_GetTick();
    for (int i = 0; i < num_chunks; i++) {
        wdog_refresh();
        HAL_UART_Transmit(&huart1, payload, sizeof(payload), 2000);
    }
    uint32_t elapsed_ms = HAL_GetTick() - start;

    // Exit transparent mode: "+++" must arrive as its own isolated write with
    // a quiet period before it, and the module needs ~1s afterward before it
    // accepts AT commands again (both bench-confirmed against this firmware).
    for (int waited = 0; waited < 500; waited += 20) {
        wdog_refresh();
        HAL_Delay(20);
    }
    HAL_UART_Transmit(&huart1, (uint8_t *)"+++", 3, 500);
    for (int waited = 0; waited < 1500; waited += 20) {
        wdog_refresh();
        HAL_Delay(20);
    }

    wifi_at_send_cmd("AT", "OK", 2000, NULL, 0); // confirm back in command mode
    wifi_at_send_cmd("AT+CIPCLOSE", "OK", 1000, NULL, 0);

    if (elapsed_ms == 0)
        return false;

    uint32_t total_bits = (uint32_t)num_chunks * sizeof(payload) * 8;
    *out_kbps = total_bits / elapsed_ms; // bits/ms == kbits/s
    return true;
}

bool wifi_sync_time(int tz_offset_hours, struct tm *out_tm)
{
    static char resp[256];

    if (!wifi_at_send_cmd("AT+CIPSTART=\"TCP\",\"time.nist.gov\",13", "OK", 5000, NULL, 0))
        return false;

    // Daytime protocol: the server sends its response immediately on
    // connect, unprompted -- no request needed from us, just drain and
    // wait for the +IPD,<len>: framed line to show up.
    resp[0] = '\0';
    size_t len = 0;
    uint32_t start = HAL_GetTick();
    while ((HAL_GetTick() - start) < 3000) {
        wdog_refresh();
        wifi_rx_drain_into(resp, sizeof(resp), &len);
        if (strstr(resp, "+IPD,"))
            break;
        HAL_Delay(20);
    }

    wifi_at_send_cmd("AT+CIPCLOSE", "OK", 1000, NULL, 0);

    char *body = strstr(resp, "+IPD,");
    if (!body)
        return false;
    body = strchr(body, ':');
    if (!body)
        return false;
    body++;

    // NIST Daytime format (NIST SP250-59, stable for decades):
    //   JJJJJ YY-MM-DD HH:MM:SS TT L H msADV UTC(NIST) OTM
    // e.g. "57054 15-02-01 20:40:40 00 0 0 832.3 UTC(NIST) *"
    int mjd, yy, mon, day, hh, mi, ss;
    if (sscanf(body, "%d %2d-%2d-%2d %2d:%2d:%2d",
               &mjd, &yy, &mon, &day, &hh, &mi, &ss) != 7)
        return false;

    struct tm tm = {0};
    tm.tm_year = (2000 + yy) - 1900;
    tm.tm_mon = mon - 1;
    tm.tm_mday = day;
    tm.tm_hour = hh + tz_offset_hours; // may go out of [0,23]; mktime() normalizes below
    tm.tm_min = mi;
    tm.tm_sec = ss;

    mktime(&tm); // normalizes tm in place (day/month/year rollover from the offset)
    GW_SetUnixTM(&tm);

    if (out_tm) *out_tm = tm;
    return true;
}

/*************************
 * /wifi.cfg parsing     *
 *************************/

bool wifi_cfg_load(wifi_cfg_t *cfg)
{
    FIL f;
    char buf[512];
    UINT br;

    memset(cfg, 0, sizeof(*cfg));
    strcpy(cfg->ping_host, "8.8.8.8");
    strcpy(cfg->http_url, "http://example.com/");
    strcpy(cfg->throughput_host, "tcpbin.com");
    cfg->throughput_port = 4242;

    if (f_open(&f, "/wifi.cfg", FA_READ) != FR_OK)
        return false;
    f_read(&f, buf, sizeof(buf) - 1, &br);
    f_close(&f);
    buf[br] = '\0';

    char *saveptr = NULL;
    char *line = strtok_r(buf, "\r\n", &saveptr);
    while (line) {
        char *eq = strchr(line, '=');
        if (eq && line[0] != '#') {
            *eq = '\0';
            const char *key = line;
            const char *val = eq + 1;
            if      (!strcmp(key, "ssid"))      strncpy(cfg->ssid, val, sizeof(cfg->ssid) - 1);
            else if (!strcmp(key, "password"))  strncpy(cfg->password, val, sizeof(cfg->password) - 1);
            else if (!strcmp(key, "ping_host")) strncpy(cfg->ping_host, val, sizeof(cfg->ping_host) - 1);
            else if (!strcmp(key, "http_url"))  strncpy(cfg->http_url, val, sizeof(cfg->http_url) - 1);
            else if (!strcmp(key, "throughput_host")) strncpy(cfg->throughput_host, val, sizeof(cfg->throughput_host) - 1);
            else if (!strcmp(key, "throughput_port")) cfg->throughput_port = atoi(val);
            else if (!strcmp(key, "tz_offset_hours")) cfg->tz_offset_hours = atoi(val);
        }
        line = strtok_r(NULL, "\r\n", &saveptr);
    }

    cfg->valid = (cfg->ssid[0] != '\0');
    return cfg->valid;
}
