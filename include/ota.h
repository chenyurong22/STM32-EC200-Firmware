/* ota.h — OTA firmware update via EC200U HTTP + dual-bank flash
 *
 * Flow:
 *   1. Receive {"url":"https://..."} on MQTT topic pump/XX/ota
 *   2. Download .bin to EC200U UFS via AT+QHTTPURL + AT+QHTTPGET
 *   3. Read binary from EC200U UFS via AT+QFOPEN + AT+QFREAD (256-byte chunks)
 *   4. Write chunks to App Slot B (0x08010000) with CRC32 tracking
 *   5. Write OTA flags to 0x0801E000 + NVIC_SystemReset()
 *   6. Bootloader copies Slot B → Slot A and boots new firmware
 *
 * Flash layout (set by STM32G070KBTX_APP.ld):
 *   0x08000000  Bootloader  (8 KB)
 *   0x08002000  App Slot A  (56 KB)  ← running app
 *   0x08010000  App Slot B  (56 KB)  ← OTA download target
 *   0x0801E000  OTA Flags   (8 KB)
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

/* OTA magic word written to flags page to request a swap on next reboot */
#define OTA_MAGIC        0xA55A1234UL

/* Flash addresses (match linker script symbols) */
#define OTA_SLOT_B_ADDR  0x08010000UL
#define OTA_FLAGS_ADDR   0x0801E000UL
#define OTA_PAGE_SIZE    2048U        /* STM32G0: 2 KB per page */
#define OTA_SLOT_SIZE    (56U * 1024U)

/* OTA flags structure written to flash at OTA_FLAGS_ADDR when update is ready */
typedef struct {
    uint32_t magic;    /* OTA_MAGIC = update pending                */
    uint32_t size;     /* bytes written to Slot B                   */
    uint32_t crc32;    /* CRC32 of Slot B (verified by bootloader)  */
    uint32_t reserved;
} OTA_Flags_t;

/* ── Public API ──────────────────────────────────────────────────────────── */

/* Call once from main() before the main loop */
void OTA_Init(void);

/* Called from Modem_Process() every loop iteration */
void OTA_Process(void);

/* Called by modem.c when {"url":"..."} arrives on pump/XX/ota topic.
 * url must remain valid until OTA completes (it is copied internally). */
void OTA_Start(const char *url);

/* Called by modem.c after the AT+QHTTPURL/CONNECT/URL/OK exchange has been
 * completed synchronously (blocking poll).  Skips the URL-setup states and
 * enters directly at AT+QHTTPGET so the state machine only handles the GET,
 * file-read, and flash phases.                                              */
void OTA_StartFromGet(const char *url);

/* Called by modem.c process_line() — passes AT response lines to OTA state
 * machine.  modem.c should call this (and return early) for ALL lines while
 * OTA_IsActive() is true so OTA URCs are not misinterpreted as MQTT ones.  */
void OTA_HandleLine(const char *line);
bool OTA_ExpectingHttpReadConnect(void);
void OTA_ForceHttpReadStream(void);
bool OTA_ShouldYieldRx(void);

/* Returns true while an OTA download/flash sequence is in progress.
 * modem.c uses this to:
 *   - Skip queued MQTT publishes
 *   - Forward all AT lines to OTA_HandleLine()                              */
bool OTA_IsActive(void);
bool OTA_WasRebootPending(void);  /* true only after intentional OTA_ST_REBOOT reset */
bool OTA_WasCFUNPreDone(void);   /* true only if CFUN was pre-done in OTA_ST_REBOOT  */

/* Binary passthrough for AT+QFREAD raw data.
 * After modem.c sees "CONNECT N\n", it calls OTA_SetBinaryExpect(N), then
 * for every subsequent byte from USART1 calls OTA_FeedByte() instead of
 * adding it to the line buffer.  OTA_BinaryPending() goes false when N
 * bytes have been received and the chunk is ready to flash.                */
void  OTA_SetBinaryExpect(uint32_t n);
bool  OTA_BinaryPending(void);
void  OTA_FeedByte(uint8_t b);

/* Modem_Send pointer — set by OTA_Init() from modem.h so ota.c can send
 * AT commands without a circular header dependency.                        */
typedef void (*OTA_SendFn)(const char *cmd);
void OTA_SetSendFn(OTA_SendFn fn);

/* Publish function pointer — set by modem.c so OTA can push status via MQTT */
typedef void (*OTA_PublishFn)(const char *topic, const char *payload);
void OTA_SetPublishFn(OTA_PublishFn fn);
