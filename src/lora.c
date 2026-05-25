/* lora.c — Reyax RYLR998 LoRa driver — MASTER (USART3, PB8=TX, PB9=RX)
 *
 * Role   : MASTER  — address 1
 * Slave  : Blue Pill  — address 2  (controls pump03 relay1 + pump04 relay2)
 *
 * Commands sent to slave:
 *   P1:ON   P1:OFF   P2:ON   P2:OFF   STATUS?
 *
 * Replies received from slave:
 *   P1:ON OK   P1:OFF OK   P2:ON OK   P2:OFF OK
 *   S:R1:ON|R2:OFF
 *   ERR:UNKNOWN CMD
 */

#include "lora.h"
#include "lora_ota.h"
#include "main.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ─── config ────────────────────────────────────────────────────────────── */
#define LORA_OWN_ADDR   "1"
#define LORA_SLAVE_ADDR "2"
#define LORA_NETWORK_ID "6"
#define LORA_BAND       "865000000"
#define LORA_TX_POWER   "22"

/* ─── private state ─────────────────────────────────────────────────────── */
static UART_HandleTypeDef *lora_uart = NULL;

#define LORA_LINE_MAX 160
static char     lora_line[LORA_LINE_MAX];
static uint16_t lora_pos = 0;

/* slave relay states — updated whenever a heartbeat/ack arrives */
static int lora_relay3_state = -1;  /* -1 = unknown, 0 = OFF, 1 = ON */
static int lora_relay4_state = -1;

/* last received +RCV signal quality */
static int      lora_last_rssi     = 0;
static int      lora_last_snr      = 0;
static uint32_t lora_last_rcv_tick = 0; /* HAL_GetTick() of last +RCV */

/* ─── helpers ───────────────────────────────────────────────────────────── */

static void lora_send_cmd(const char *cmd)
{
    HAL_UART_Transmit(lora_uart, (const uint8_t *)cmd,    strlen(cmd), 300);
    HAL_UART_Transmit(lora_uart, (const uint8_t *)"\r\n", 2,           50);
}

static int lora_getc(uint8_t *out)
{
    if (__HAL_UART_GET_FLAG(lora_uart, UART_FLAG_ORE))
    {
        __HAL_UART_CLEAR_OREFLAG(lora_uart);
        lora_uart->ErrorCode = HAL_UART_ERROR_NONE;
        lora_uart->RxState   = HAL_UART_STATE_READY;
    }
    return (HAL_UART_Receive(lora_uart, out, 1, 0) == HAL_OK) ? 1 : 0;
}

static void lora_poll(uint32_t timeout_ms)
{
    uint32_t t0 = HAL_GetTick();
    while (HAL_GetTick() - t0 < timeout_ms)
    {
        uint8_t b;
        if (!lora_getc(&b)) continue;
        if (b == '\n')
        {
            lora_line[lora_pos] = '\0';
            if (lora_pos > 0)
            {
                char dbg[LORA_LINE_MAX + 16];
                snprintf(dbg, sizeof(dbg), "[LoRa] %s\r\n", lora_line);
                Debug_Print(dbg);
            }
            lora_pos = 0;
        }
        else if (b != '\r' && lora_pos < LORA_LINE_MAX - 1)
        {
            lora_line[lora_pos++] = (char)b;
        }
    }
}

/* ─── RCV parser ─────────────────────────────────────────────────────────
 * Handles +RCV=<addr>,<len>,<data>,<RSSI>,<SNR>
 * Parses slave heartbeat/ack: S:R1:ON|R2:OFF  or  P1:ON OK  etc.
 * ────────────────────────────────────────────────────────────────────── */
static void lora_process_rcv(const char *line)
{
    /* skip "+RCV=" prefix */
    const char *p = line + 5;

    /* skip addr field */
    p = strchr(p, ','); if (!p) return; p++;
    /* skip len field */
    p = strchr(p, ','); if (!p) return; p++;
    /* p now points to data — no commas inside relay/OTA messages */
    char data[160] = "";
    size_t i = 0;
    while (*p && *p != ',' && i < sizeof(data) - 1)
        data[i++] = *p++;
    data[i] = '\0';

    /* extract RSSI (4th field) and SNR (5th field) */
    if (*p == ',') { p++; lora_last_rssi = (int)strtol(p, NULL, 10); }
    p = strchr(p, ',');
    if (p)          { p++; lora_last_snr  = (int)strtol(p, NULL, 10); }
    lora_last_rcv_tick = HAL_GetTick();

    char dbg[80];
    snprintf(dbg, sizeof(dbg), "[LoRa] slave: %.63s\r\n", data);
    Debug_Print(dbg);

    /* Route OTA protocol responses to lora_ota state machine */
    if (strncmp(data, "OTA:", 4) == 0) {
        LoRaOta_HandleResponse(data);
        return;
    }

    /* Parse status heartbeat: S:R1:ON|R2:OFF */
    if (strncmp(data, "S:R1:", 5) == 0)
    {
        lora_relay3_state = (strncmp(data + 5, "ON", 2) == 0) ? 1 : 0;

        const char *r2 = strstr(data, "R2:");
        if (r2)
            lora_relay4_state = (strncmp(r2 + 3, "ON", 2) == 0) ? 1 : 0;

        snprintf(dbg, sizeof(dbg), "[LoRa] HB relay3=%d relay4=%d\r\n",
                 lora_relay3_state, lora_relay4_state);
        Debug_Print(dbg);
        return;
    }

    /* Parse ack: P1:ON OK or P1:OFF OK */
    if (strncmp(data, "P1:", 3) == 0)
    {
        lora_relay3_state = (strncmp(data + 3, "ON", 2) == 0) ? 1 : 0;
        return;
    }
    if (strncmp(data, "P2:", 3) == 0)
    {
        lora_relay4_state = (strncmp(data + 3, "ON", 2) == 0) ? 1 : 0;
        return;
    }
}

/* ─── public API ────────────────────────────────────────────────────────── */

void LoRa_Init(UART_HandleTypeDef *huart)
{
    lora_uart = huart;
    lora_pos  = 0;

    lora_send_cmd("AT+RESET");
    lora_poll(3500);

    lora_send_cmd("AT");
    lora_poll(300);
    lora_send_cmd("AT+ADDRESS=" LORA_OWN_ADDR);
    lora_poll(300);
    lora_send_cmd("AT+NETWORKID=" LORA_NETWORK_ID);
    lora_poll(300);
    lora_send_cmd("AT+BAND=" LORA_BAND);
    lora_poll(500);
    lora_send_cmd("AT+CRFOP=" LORA_TX_POWER);
    lora_poll(300);
    lora_send_cmd("AT+PARAMETER=9,7,1,12");
    lora_poll(300);
    lora_send_cmd("AT+MODE=0");
    lora_poll(300);

    Debug_Print("[LoRa] --- module settings ---\r\n");
    lora_send_cmd("AT+ADDRESS?");   lora_poll(300);
    lora_send_cmd("AT+NETWORKID?"); lora_poll(300);
    lora_send_cmd("AT+BAND?");      lora_poll(300);
    lora_send_cmd("AT+PARAMETER?"); lora_poll(300);
    Debug_Print("[LoRa] MASTER init done (addr=1 net=6 band=865MHz)\r\n");
}

/*
 * LoRa_SendRelay — send relay command to slave (Blue Pill, addr=2)
 *   relay_num : 1 = pump03 (P1), 2 = pump04 (P2)
 *   on        : true = ON, false = OFF
 */
void LoRa_SendRelay(int relay_num, bool on)
{
    if (!lora_uart) return;

    char msg[16];
    snprintf(msg, sizeof(msg), "P%d:%s", relay_num, on ? "ON" : "OFF");

    char cmd[64];
    snprintf(cmd, sizeof(cmd), "AT+SEND=%s,%d,%s",
             LORA_SLAVE_ADDR, (int)strlen(msg), msg);

    char dbg[80];
    snprintf(dbg, sizeof(dbg), "[LoRa] TX -> slave: %s\r\n", msg);
    Debug_Print(dbg);

    lora_send_cmd(cmd);
}

void LoRa_Process(void)
{
    if (!lora_uart) return;

    uint8_t b;
    while (lora_getc(&b))
    {
        if (b == '\n')
        {
            lora_line[lora_pos] = '\0';
            if (lora_pos > 0)
            {
                char dbg[LORA_LINE_MAX + 16];
                snprintf(dbg, sizeof(dbg), "[LoRa] RX: %s\r\n", lora_line);
                Debug_Print(dbg);

                if (strncmp(lora_line, "+RCV=", 5) == 0)
                    lora_process_rcv(lora_line);
            }
            lora_pos = 0;
        }
        else if (b != '\r' && lora_pos < LORA_LINE_MAX - 1)
        {
            lora_line[lora_pos++] = (char)b;
        }
    }
}

/* Send an arbitrary message to the slave (used by lora_ota.c for OTA protocol) */
void LoRa_SendRaw(const char *msg)
{
    if (!lora_uart) return;
    char cmd[200];  /* OTA:D:nnnn:<128hex>:<crc16> = 144 chars + AT+SEND header */
    snprintf(cmd, sizeof(cmd), "AT+SEND=%s,%d,%s", LORA_SLAVE_ADDR, (int)strlen(msg), msg);
    lora_send_cmd(cmd);
}

int      LoRa_GetRelay3State(void)  { return lora_relay3_state; }
int      LoRa_GetRelay4State(void)  { return lora_relay4_state; }
int      LoRa_GetLastRSSI(void)     { return lora_last_rssi; }
int      LoRa_GetLastSNR(void)      { return lora_last_snr; }
/* Returns ms since last +RCV, or 0xFFFFFFFF if never received */
uint32_t LoRa_GetLastRcvAge(void)
{
    if (lora_last_rcv_tick == 0) return 0xFFFFFFFFUL;
    return HAL_GetTick() - lora_last_rcv_tick;
}
