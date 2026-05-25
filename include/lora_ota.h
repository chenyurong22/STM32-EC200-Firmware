/* lora_ota.h — LoRa OTA relay: download Blue Pill firmware via HTTPS,
 *              forward chunk-by-chunk to Blue Pill slave using OTA:* protocol.
 *
 * Integration points (all in pump03 / pump04 path only):
 *   modem.c  — call LoRaOta_HandleLine() while LoRaOta_IsActive()
 *            — call LoRaOta_FeedByte() while LoRaOta_BinaryPending()
 *            — subscribe pump/03/lora_ota, call LoRaOta_Start(url) on receive
 *   lora.c   — call LoRaOta_HandleResponse(data) when data starts with "OTA:"
 *   main.c   — call LoRaOta_Init() once, LoRaOta_Process() every loop
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Callback: send AT command to modem (→ Modem_Send) */
typedef void (*LoRaOtaSendFn)(const char *cmd);

/* Callback: publish MQTT message (topic, JSON payload) */
typedef void (*LoRaOtaPublishFn)(const char *topic, const char *payload);

/* Callback: send raw message to LoRa slave via AT+SEND (→ LoRa_SendRaw) */
typedef void (*LoRaOtaLoRaFn)(const char *msg);

/* ── Call once from main() before the loop ─────────────────────────────────── */
void LoRaOta_Init(void);

/* ── Call every main loop iteration (handles timeouts + chunk processing) ──── */
void LoRaOta_Process(void);

/* ── Trigger: called by modem.c when {"url":"..."} arrives on lora_ota topic ── */
void LoRaOta_Start(const char *url);

/* ── Called by modem.c process_line() while LoRaOta_IsActive() ─────────────── */
void LoRaOta_HandleLine(const char *line);

/* ── Called by lora.c when +RCV data starts with "OTA:" ─────────────────────── */
void LoRaOta_HandleResponse(const char *data);

/* ── Gating functions used by modem.c byte loop ─────────────────────────────── */
bool LoRaOta_IsActive(void);        /* true while OTA in progress              */
bool LoRaOta_BinaryPending(void);   /* true while expecting raw binary bytes   */
void LoRaOta_FeedByte(uint8_t b);   /* accumulate one binary byte from modem   */

/* ── Set callbacks (call from modem.c / lora.c init paths) ─────────────────── */
void LoRaOta_SetSendFn(LoRaOtaSendFn fn);
void LoRaOta_SetPublishFn(LoRaOtaPublishFn fn);
void LoRaOta_SetLoRaFn(LoRaOtaLoRaFn fn);
