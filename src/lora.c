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
 *   S:R1:ON,R2:OFF
 *   ERR:UNKNOWN CMD
 */

#include "lora.h"
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
            }
            lora_pos = 0;
        }
        else if (b != '\r' && lora_pos < LORA_LINE_MAX - 1)
        {
            lora_line[lora_pos++] = (char)b;
        }
    }
}
