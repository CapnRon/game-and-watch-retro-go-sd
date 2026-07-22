#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "main.h"
#include "gw_wifi.h"
#include "ff.h"

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

static void wifi_uart_init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_USART1_CLK_ENABLE();

    GPIO_InitStruct.Pin = WIFI_UART_TX_Pin | WIFI_UART_RX_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    GPIO_InitStruct.Alternate = GPIO_AF7_USART1;
    HAL_GPIO_Init(WIFI_UART_TX_GPIO_Port, &GPIO_InitStruct);

    huart1.Instance = USART1;
    huart1.Init.BaudRate = 115200;
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

    wifi_uart_init();
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

bool wifi_ping(const char *host, int *rtt_ms)
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
    wifi_at_send_cmd(cmd, "OK", 3000, resp, sizeof(resp));

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

bool wifi_throughput_test(const char *host, uint16_t port, uint32_t *out_kbps)
{
    char cmd[96];
    static char chunk[1024];
    const int num_chunks = 20;

    snprintf(cmd, sizeof(cmd), "AT+CIPSTART=\"TCP\",\"%s\",%u", host, port);
    wifi_at_send_cmd(cmd, "OK", 5000, NULL, 0);

    memset(chunk, 'A', sizeof(chunk));

    uint32_t start = HAL_GetTick();
    int sent_chunks = 0;
    for (int i = 0; i < num_chunks; i++) {
        if (!wifi_tcp_send(chunk, sizeof(chunk), 3000))
            break;
        sent_chunks++;
    }
    uint32_t elapsed_ms = HAL_GetTick() - start;

    wifi_at_send_cmd("AT+CIPCLOSE", "OK", 1000, NULL, 0);

    if (elapsed_ms == 0 || sent_chunks == 0)
        return false;

    uint32_t total_bits = (uint32_t)sent_chunks * sizeof(chunk) * 8;
    *out_kbps = total_bits / elapsed_ms; // bits/ms == kbits/s
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
        }
        line = strtok_r(NULL, "\r\n", &saveptr);
    }

    cfg->valid = (cfg->ssid[0] != '\0');
    return cfg->valid;
}
