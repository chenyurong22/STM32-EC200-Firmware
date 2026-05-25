/* lora_ota.c — LoRa OTA relay for Blue Pill firmware update
 *
 * Flow:
 *   1. Receive {"url":"https://..."} on pump/03/lora_ota (via modem.c)
 *   2. HTTPS download → EC200U UFS ("HTTP_GETFILE")
 *   3. Scan pass: AT+QFOPEN / AT+QFREAD to compute CRC32 over entire image
 *   4. LoRa: send OTA:START → OTA:QUERY → OTA:D packets → OTA:END
 *   5. Blue Pill slave verifies CRC32, writes metadata, reboots
 *   6. Blue Pill bootloader copies slot2 → slot1 on next power cycle
 *
 * Protocol (master → slave):
 *   OTA:START:<fw_size>:<num_pkts>:<crc32_8hex>
 *   OTA:QUERY
 *   OTA:D:<seq4>:<hex128>:<crc16_4hex>   (64 bytes = 128 hex chars)
 *   OTA:END
 *   OTA:ABORT
 *
 * Protocol (slave → master):
 *   OTA:ACK:START   OTA:RESUME:<seq4>   OTA:ACK:<seq4>
 *   OTA:ERR:<seq4>:<reason>             OTA:DONE:OK / OTA:DONE:ERR
 */

#include "lora_ota.h"
#include "main.h"
#include "modem.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

extern IWDG_HandleTypeDef hiwdg;

/* ── configuration ─────────────────────────────────────────────────────────── */
#define LORA_OTA_CHUNK_BYTES  64U          /* bytes per packet                  */
#define LORA_OTA_STEP_TIMEOUT 15000UL      /* modem AT command timeout (ms)     */
#define LORA_OTA_ACK_TIMEOUT  14000UL      /* LoRa ACK timeout (ms)             */
#define LORA_OTA_MAX_RETRY    3U           /* retries per packet before abort   */
#define LORA_OTA_PROGRESS_N   10U          /* publish progress every N packets  */
#define LORA_OTA_MAX_FW       11264U       /* max Blue Pill firmware size (11KB)*/

/* MQTT topics — derived from PUMP_ID in modem.h */
#define PROGRESS_TOPIC  "pump/" PUMP_ID "/lora_ota_progress"
#define RESULT_TOPIC    "pump/" PUMP_ID "/lora_ota_result"

/* ── CRC functions ─────────────────────────────────────────────────────────── */

static uint16_t crc16_ccitt(const uint8_t *d, uint16_t len)
{
    uint16_t crc = 0xFFFF;
    for (uint16_t i = 0; i < len; i++) {
        crc ^= (uint16_t)((uint16_t)d[i] << 8);
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x8000U) ? (uint16_t)((crc << 1) ^ 0x1021U)
                                  : (uint16_t)(crc << 1);
    }
    return crc;
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *d, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        crc ^= d[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320UL & -(crc & 1UL));
    }
    return crc;
}

/* ── hex encode 64 bytes → 128 ASCII chars ─────────────────────────────────── */
static const char HEX[] = "0123456789ABCDEF";

static void hex_encode_64(const uint8_t *src, char *dst)
{
    for (uint16_t i = 0; i < LORA_OTA_CHUNK_BYTES; i++) {
        dst[i * 2]     = HEX[src[i] >> 4];
        dst[i * 2 + 1] = HEX[src[i] & 0x0FU];
    }
    dst[128] = '\0';
}

/* ── state machine ─────────────────────────────────────────────────────────── */
typedef enum {
    LO_IDLE = 0,
    /* HTTPS setup */
    LO_SSL_ENABLE,    /* sent AT+QHTTPCFG="ssl",1         → waiting OK          */
    LO_SSL_SECLEVEL,  /* sent AT+QSSLCFG="seclevel",1,1   → waiting OK          */
    LO_SSL_VERSION,   /* sent AT+QSSLCFG="sslversion",1,4 → waiting OK          */
    LO_SSL_CIPHER,    /* sent AT+QSSLCFG="ciphersuite"... → waiting OK          */
    LO_SSL_CTXID,     /* sent AT+QHTTPCFG="sslctxid",1   → waiting OK          */
    /* HTTP download */
    LO_URL_CMD,       /* sent AT+QHTTPURL=<len>,30        → waiting CONNECT     */
    LO_URL_BODY,      /* sent URL                         → waiting OK          */
    LO_HTTP_GET,      /* sent AT+QHTTPGET=120             → waiting +QHTTPGET   */
    /* Scan pass: read whole file to compute CRC32 */
    LO_SCAN_OPEN,     /* sent AT+QFOPEN                   → waiting +QFOPEN     */
    LO_SCAN_READ,     /* sent AT+QFREAD=<fd>,64           → waiting CONNECT N   */
    LO_SCAN_DATA,     /* accumulating binary chunk (BinaryPending)              */
    LO_SCAN_CLOSE,    /* sent AT+QFCLOSE                  → waiting OK          */
    /* LoRa start exchange */
    LO_LORA_START,    /* sent OTA:START                   → waiting ACK:START   */
    LO_LORA_QUERY,    /* sent OTA:QUERY                   → waiting RESUME:<seq>*/
    /* Send pass: open file, seek to resume position, send chunks */
    LO_SEND_OPEN,     /* sent AT+QFOPEN                   → waiting +QFOPEN     */
    LO_SEND_SEEK,     /* sent AT+QFSEEK (resume > 0)      → waiting OK          */
    LO_SEND_READ,     /* sent AT+QFREAD=<fd>,64           → waiting CONNECT N   */
    LO_SEND_DATA,     /* accumulating 64 bytes (BinaryPending)                  */
    LO_SEND_PKT,      /* sent OTA:D                       → waiting OTA:ACK     */
    /* Finish */
    LO_LORA_END,      /* sent OTA:END                     → waiting OTA:DONE    */
    LO_CLOSE_DONE,    /* sent AT+QFCLOSE                  → waiting OK (done)   */
    LO_DONE,
    LO_ERROR
} LoraOtaState_t;

static LoraOtaState_t lo_state    = LO_IDLE;
static uint32_t       lo_state_ms = 0;        /* HAL_GetTick() at state entry  */

/* image metadata from HTTP response + scan pass */
static char     lo_url[256];
static uint32_t lo_fw_size    = 0;
static uint32_t lo_num_pkts   = 0;
static uint32_t lo_img_crc32  = 0;           /* final CRC32 (scan result)     */

/* scan-pass accumulator */
static uint32_t lo_scan_crc32  = 0xFFFFFFFFUL;
static uint32_t lo_scan_bytes  = 0;           /* bytes fed into CRC so far     */
static int      lo_scan_fd     = -1;

/* send-pass state */
static int      lo_send_fd     = -1;
static uint16_t lo_resume_seq  = 0;           /* slave's last received seq     */
static uint16_t lo_cur_seq     = 0;           /* current seq being sent (1..N) */
static uint32_t lo_bytes_sent  = 0;           /* bytes covered by sent packets */
static uint8_t  lo_retry_cnt   = 0;

/* binary accumulation (64-byte chunk) */
static uint8_t  lo_chunk[LORA_OTA_CHUNK_BYTES];
static uint32_t lo_binary_expect = 0;
static uint32_t lo_binary_pos    = 0;
static bool     lo_chunk_ready   = false;
static uint32_t lo_chunk_actual  = 0;  /* bytes actually received (≤ 64)       */

/* current packet buffer (kept for retry) */
static char lo_pkt_buf[160];  /* "OTA:D:nnnn:<128hex>:<crc16>" = 144 chars max */

/* callbacks */
static LoRaOtaSendFn    lo_send_fn  = NULL;
static LoRaOtaPublishFn lo_pub_fn   = NULL;
static LoRaOtaLoRaFn    lo_lora_fn  = NULL;

/* ── private helpers ────────────────────────────────────────────────────────── */

static void lo_at(const char *cmd)
{
    if (lo_send_fn) {
        lo_send_fn(cmd);
        lo_send_fn("\r\n");
    }
}

/* Send raw bytes without appending \r\n (used for URL body in AT+QHTTPURL) */
static void lo_raw(const char *data)
{
    if (lo_send_fn) lo_send_fn(data);
}

static void lo_pub(const char *topic, const char *payload)
{
    if (lo_pub_fn) lo_pub_fn(topic, payload);
}

static void lo_lora_send(const char *msg)
{
    if (lo_lora_fn) lo_lora_fn(msg);
}

static void lo_enter(LoraOtaState_t st)
{
    lo_state    = st;
    lo_state_ms = HAL_GetTick();
}

static void lo_error(const char *reason)
{
    char msg[100];
    snprintf(msg, sizeof(msg), "{\"status\":\"err\",\"reason\":\"%s\"}", reason);
    lo_pub(RESULT_TOPIC, msg);

    Debug_Print("[LoRaOTA] ERROR: ");
    Debug_Print(reason);
    Debug_Print("\r\n");

    /* Close any open file handles */
    char cmd[24];
    if (lo_scan_fd >= 0) {
        snprintf(cmd, sizeof(cmd), "AT+QFCLOSE=%d", lo_scan_fd);
        lo_at(cmd);
        lo_scan_fd = -1;
    }
    if (lo_send_fd >= 0) {
        snprintf(cmd, sizeof(cmd), "AT+QFCLOSE=%d", lo_send_fd);
        lo_at(cmd);
        lo_send_fd = -1;
    }
    lo_enter(LO_ERROR);
}

static void lo_publish_progress(void)
{
    uint32_t pct = lo_num_pkts ? (lo_cur_seq * 100UL / lo_num_pkts) : 0;
    char msg[80];
    snprintf(msg, sizeof(msg),
             "{\"pkt\":%u,\"total\":%u,\"pct\":%u}",
             (unsigned)lo_cur_seq, (unsigned)lo_num_pkts, (unsigned)pct);
    lo_pub(PROGRESS_TOPIC, msg);
}

/* Build lo_pkt_buf for current chunk at seq (includes CRC16 over full 64 bytes) */
static void lo_build_pkt(uint16_t seq)
{
    char hex[129];
    hex_encode_64(lo_chunk, hex);
    uint16_t crc = crc16_ccitt(lo_chunk, LORA_OTA_CHUNK_BYTES);
    snprintf(lo_pkt_buf, sizeof(lo_pkt_buf),
             "OTA:D:%04u:%s:%04X", (unsigned)seq, hex, (unsigned)crc);
}

/* Send the scan QFREAD command */
static void lo_scan_read_next(void)
{
    char cmd[24];
    snprintf(cmd, sizeof(cmd), "AT+QFREAD=%d,64", lo_scan_fd);
    lo_at(cmd);
    lo_enter(LO_SCAN_READ);
}

/* Send the send QFREAD command */
static void lo_send_read_next(void)
{
    char cmd[24];
    snprintf(cmd, sizeof(cmd), "AT+QFREAD=%d,64", lo_send_fd);
    lo_at(cmd);
    lo_enter(LO_SEND_READ);
}

/* ── Public API ─────────────────────────────────────────────────────────────── */

void LoRaOta_SetSendFn(LoRaOtaSendFn fn)     { lo_send_fn  = fn; }
void LoRaOta_SetPublishFn(LoRaOtaPublishFn fn){ lo_pub_fn   = fn; }
void LoRaOta_SetLoRaFn(LoRaOtaLoRaFn fn)     { lo_lora_fn  = fn; }

void LoRaOta_Init(void) { lo_state = LO_IDLE; }

bool LoRaOta_IsActive(void)
{
    return lo_state != LO_IDLE && lo_state != LO_DONE && lo_state != LO_ERROR;
}

bool LoRaOta_BinaryPending(void)
{
    return lo_binary_expect > 0;
}

void LoRaOta_FeedByte(uint8_t b)
{
    if (lo_binary_pos < LORA_OTA_CHUNK_BYTES)
        lo_chunk[lo_binary_pos] = b;
    lo_binary_pos++;
    if (lo_binary_pos >= lo_binary_expect) {
        lo_chunk_actual  = lo_binary_pos;
        lo_binary_expect = 0;  /* clears BinaryPending */
        lo_chunk_ready   = true;
    }
}

void LoRaOta_Start(const char *url)
{
    if (LoRaOta_IsActive()) return;

    strncpy(lo_url, url, sizeof(lo_url) - 1);
    lo_url[sizeof(lo_url) - 1] = '\0';

    lo_fw_size     = 0;
    lo_num_pkts    = 0;
    lo_img_crc32   = 0;
    lo_scan_crc32  = 0xFFFFFFFFUL;
    lo_scan_bytes  = 0;
    lo_scan_fd     = -1;
    lo_send_fd     = -1;
    lo_resume_seq  = 0;
    lo_cur_seq     = 0;
    lo_bytes_sent  = 0;
    lo_retry_cnt   = 0;
    lo_binary_expect = 0;
    lo_binary_pos    = 0;
    lo_chunk_ready   = false;

    lo_pub(PROGRESS_TOPIC, "{\"status\":\"starting\"}");
    Debug_Print("[LoRaOTA] Starting download\r\n");

    if (strncmp(lo_url, "https://", 8) == 0) {
        lo_at("AT+QHTTPCFG=\"ssl\",1");
        lo_enter(LO_SSL_ENABLE);
    } else {
        char cmd[40];
        snprintf(cmd, sizeof(cmd), "AT+QHTTPURL=%d,30", (int)strlen(lo_url));
        lo_at(cmd);
        lo_enter(LO_URL_CMD);
    }
}

/* ── LoRaOta_HandleLine — called by modem.c for ALL lines while IsActive() ─── */
void LoRaOta_HandleLine(const char *line)
{
    switch (lo_state)
    {
    /* ── SSL setup chain ─────────────────────────────────────────────────── */
    case LO_SSL_ENABLE:
        if (strcmp(line, "OK") == 0) {
            lo_at("AT+QSSLCFG=\"seclevel\",1,1");
            lo_enter(LO_SSL_SECLEVEL);
        } else if (strstr(line, "ERROR"))
            lo_error("ssl enable failed");
        break;

    case LO_SSL_SECLEVEL:
        if (strcmp(line, "OK") == 0) {
            lo_at("AT+QSSLCFG=\"sslversion\",1,4");
            lo_enter(LO_SSL_VERSION);
        } else if (strstr(line, "ERROR"))
            lo_error("ssl seclevel failed");
        break;

    case LO_SSL_VERSION:
        if (strcmp(line, "OK") == 0) {
            lo_at("AT+QSSLCFG=\"ciphersuite\",1,0xFFFF");
            lo_enter(LO_SSL_CIPHER);
        } else if (strstr(line, "ERROR"))
            lo_error("ssl version failed");
        break;

    case LO_SSL_CIPHER:
        if (strcmp(line, "OK") == 0) {
            lo_at("AT+QHTTPCFG=\"sslctxid\",1");
            lo_enter(LO_SSL_CTXID);
        } else if (strstr(line, "ERROR"))
            lo_error("ssl cipher failed");
        break;

    case LO_SSL_CTXID:
        if (strcmp(line, "OK") == 0) {
            char cmd[40];
            snprintf(cmd, sizeof(cmd), "AT+QHTTPURL=%d,30", (int)strlen(lo_url));
            lo_at(cmd);
            lo_enter(LO_URL_CMD);
        } else if (strstr(line, "ERROR"))
            lo_error("ssl ctxid failed");
        break;

    /* ── HTTP URL setup ───────────────────────────────────────────────────── */
    case LO_URL_CMD:
        if (strncmp(line, "CONNECT", 7) == 0) {
            lo_raw(lo_url);   /* send URL as raw bytes — no \r\n */
            lo_enter(LO_URL_BODY);
        } else if (strstr(line, "ERROR"))
            lo_error("QHTTPURL cmd failed");
        break;

    case LO_URL_BODY:
        if (strcmp(line, "OK") == 0) {
            lo_pub(PROGRESS_TOPIC, "{\"status\":\"downloading\"}");
            lo_at("AT+QHTTPGET=120");
            lo_enter(LO_HTTP_GET);
        } else if (strstr(line, "ERROR"))
            lo_error("QHTTPURL body failed");
        break;

    /* ── HTTP GET result ──────────────────────────────────────────────────── */
    case LO_HTTP_GET:
        if (strstr(line, "+QHTTPGET:")) {
            /* +QHTTPGET: 0,200,<content_length>  or  0,0,<size> */
            const char *p = strchr(line, ',');
            int http_code = 0;
            while (p) {
                http_code = atoi(p + 1);
                if (http_code == 200 || http_code == 206) break;
                p = strchr(p + 1, ',');
            }
            if (http_code != 200 && http_code != 206) {
                lo_error("HTTP not 200/206");
                break;
            }
            p = strchr(p + 1, ',');
            if (p) lo_fw_size = (uint32_t)atoi(p + 1);
            if (lo_fw_size == 0 || lo_fw_size > LORA_OTA_MAX_FW) {
                lo_error("bad fw size");
                break;
            }
            lo_num_pkts = (lo_fw_size + LORA_OTA_CHUNK_BYTES - 1U) / LORA_OTA_CHUNK_BYTES;
            char dbg[64];
            snprintf(dbg, sizeof(dbg),
                     "[LoRaOTA] fw=%lu bytes, %lu pkts\r\n",
                     (unsigned long)lo_fw_size, (unsigned long)lo_num_pkts);
            Debug_Print(dbg);
            /* Open file for CRC32 scan */
            lo_at("AT+QFOPEN=\"HTTP_GETFILE\",0");
            lo_enter(LO_SCAN_OPEN);
        } else if (strstr(line, "ERROR"))
            lo_error("QHTTPGET failed");
        break;

    /* ── Scan pass: open / read / close ─────────────────────────────────── */
    case LO_SCAN_OPEN:
        if (strncmp(line, "+QFOPEN:", 8) == 0) {
            lo_scan_fd = atoi(line + 8);
            lo_scan_crc32 = 0xFFFFFFFFUL;
            lo_scan_bytes = 0;
            Debug_Print("[LoRaOTA] Scanning for CRC32\r\n");
            lo_scan_read_next();
        } else if (strstr(line, "ERROR"))
            lo_error("QFOPEN scan failed");
        break;

    case LO_SCAN_READ:
        /* "CONNECT N" — binary data follows */
        if (strncmp(line, "CONNECT", 7) == 0) {
            uint32_t n = (uint32_t)atoi(line + 8);  /* "CONNECT 64" → n=64 */
            if (n == 0) n = LORA_OTA_CHUNK_BYTES;   /* fallback if no number */
            memset(lo_chunk, 0xFF, LORA_OTA_CHUNK_BYTES);
            lo_binary_expect = n;
            lo_binary_pos    = 0;
            lo_chunk_ready   = false;
            lo_enter(LO_SCAN_DATA);
        } else if (strstr(line, "ERROR"))
            lo_error("QFREAD scan failed");
        /* Ignore "OK" from previous QFREAD */
        break;

    case LO_SCAN_DATA:
        /* chunk processing happens in LoRaOta_Process() when lo_chunk_ready */
        /* Ignore any stray lines ("OK" from QFREAD, etc.) */
        break;

    case LO_SCAN_CLOSE:
        if (strcmp(line, "OK") == 0) {
            lo_scan_fd = -1;
            /* Finalise CRC32 */
            lo_img_crc32 = lo_scan_crc32 ^ 0xFFFFFFFFUL;
            char dbg[64];
            snprintf(dbg, sizeof(dbg),
                     "[LoRaOTA] CRC32=0x%08lX, sending OTA:START\r\n",
                     (unsigned long)lo_img_crc32);
            Debug_Print(dbg);
            /* Send OTA:START */
            char start[48];
            snprintf(start, sizeof(start), "OTA:START:%lu:%lu:%08lX",
                     (unsigned long)lo_fw_size,
                     (unsigned long)lo_num_pkts,
                     (unsigned long)lo_img_crc32);
            lo_lora_send(start);
            lo_enter(LO_LORA_START);
        }
        break;

    /* ── LoRa exchange states — handled by HandleResponse(), not lines ────── */
    case LO_LORA_START:
    case LO_LORA_QUERY:
    case LO_LORA_END:
        break;  /* LoRa ACKs come via HandleResponse() */

    /* ── Send pass: open / seek / read ──────────────────────────────────── */
    case LO_SEND_OPEN:
        if (strncmp(line, "+QFOPEN:", 8) == 0) {
            lo_send_fd = atoi(line + 8);
            if (lo_resume_seq > 0) {
                /* Seek to byte offset resume_seq * 64 */
                char cmd[40];
                snprintf(cmd, sizeof(cmd), "AT+QFSEEK=%d,%lu,0",
                         lo_send_fd, (unsigned long)lo_resume_seq * LORA_OTA_CHUNK_BYTES);
                lo_at(cmd);
                lo_enter(LO_SEND_SEEK);
            } else {
                lo_cur_seq    = 1;
                lo_bytes_sent = 0;
                lo_send_read_next();
            }
        } else if (strstr(line, "ERROR"))
            lo_error("QFOPEN send failed");
        break;

    case LO_SEND_SEEK:
        if (strcmp(line, "OK") == 0) {
            lo_cur_seq    = lo_resume_seq + 1U;
            lo_bytes_sent = (uint32_t)lo_resume_seq * LORA_OTA_CHUNK_BYTES;
            char dbg[48];
            snprintf(dbg, sizeof(dbg), "[LoRaOTA] Resumed at seq %u\r\n",
                     (unsigned)lo_cur_seq);
            Debug_Print(dbg);
            lo_send_read_next();
        } else if (strstr(line, "ERROR"))
            lo_error("QFSEEK failed");
        break;

    case LO_SEND_READ:
        if (strncmp(line, "CONNECT", 7) == 0) {
            uint32_t n = (uint32_t)atoi(line + 8);
            if (n == 0) n = LORA_OTA_CHUNK_BYTES;
            memset(lo_chunk, 0xFF, LORA_OTA_CHUNK_BYTES);
            lo_binary_expect = n;
            lo_binary_pos    = 0;
            lo_chunk_ready   = false;
            lo_enter(LO_SEND_DATA);
        } else if (strstr(line, "ERROR"))
            lo_error("QFREAD send failed");
        break;

    case LO_SEND_DATA:
        /* chunk processing in LoRaOta_Process() when lo_chunk_ready */
        break;

    case LO_SEND_PKT:
        break;  /* ACK handled by HandleResponse() */

    case LO_CLOSE_DONE:
        if (strcmp(line, "OK") == 0) {
            lo_send_fd = -1;
            lo_pub(RESULT_TOPIC, "{\"status\":\"ok\"}");
            Debug_Print("[LoRaOTA] Done\r\n");
            lo_enter(LO_DONE);
        }
        break;

    default:
        break;
    }
}

/* ── LoRaOta_HandleResponse — called by lora.c when data starts with "OTA:" ── */
void LoRaOta_HandleResponse(const char *data)
{
    char dbg[80];
    snprintf(dbg, sizeof(dbg), "[LoRaOTA] rcv: %.62s\r\n", data);
    Debug_Print(dbg);

    switch (lo_state)
    {
    case LO_LORA_START:
        if (strcmp(data, "OTA:ACK:START") == 0) {
            lo_lora_send("OTA:QUERY");
            lo_enter(LO_LORA_QUERY);
        }
        break;

    case LO_LORA_QUERY:
        /* OTA:RESUME:<seq4> — slave tells us where to resume from */
        if (strncmp(data, "OTA:RESUME:", 11) == 0) {
            lo_resume_seq = (uint16_t)atoi(data + 11);
            char dbgr[40];
            snprintf(dbgr, sizeof(dbgr), "[LoRaOTA] resume seq=%u\r\n",
                     (unsigned)lo_resume_seq);
            Debug_Print(dbgr);
            /* Open file for sending */
            lo_at("AT+QFOPEN=\"HTTP_GETFILE\",0");
            lo_enter(LO_SEND_OPEN);
        }
        break;

    case LO_SEND_PKT:
        /* OTA:ACK:<seq4> — packet accepted */
        if (strncmp(data, "OTA:ACK:", 8) == 0) {
            uint16_t acked = (uint16_t)atoi(data + 8);
            if (acked == lo_cur_seq) {
                lo_retry_cnt = 0;
                lo_bytes_sent += LORA_OTA_CHUNK_BYTES;

                /* Progress publish every N packets */
                if (lo_cur_seq % LORA_OTA_PROGRESS_N == 0)
                    lo_publish_progress();

                if (lo_cur_seq >= lo_num_pkts) {
                    /* All packets sent — ask slave to verify */
                    Debug_Print("[LoRaOTA] All pkts sent, sending END\r\n");
                    lo_lora_send("OTA:END");
                    lo_enter(LO_LORA_END);
                } else {
                    lo_cur_seq++;
                    lo_send_read_next();
                }
            }
        }
        /* OTA:ERR:<seq>:<reason> — slave requests retransmit */
        else if (strncmp(data, "OTA:ERR:", 8) == 0) {
            if (lo_retry_cnt < LORA_OTA_MAX_RETRY) {
                lo_retry_cnt++;
                char dbgr[48];
                snprintf(dbgr, sizeof(dbgr),
                         "[LoRaOTA] ERR pkt %u retry %u\r\n",
                         (unsigned)lo_cur_seq, (unsigned)lo_retry_cnt);
                Debug_Print(dbgr);
                lo_lora_send(lo_pkt_buf);  /* retransmit same packet */
                lo_enter(LO_SEND_PKT);     /* reset timeout */
            } else {
                lo_error("max retries reached");
            }
        }
        break;

    case LO_LORA_END:
        if (strcmp(data, "OTA:DONE:OK") == 0) {
            lo_publish_progress();  /* final progress = 100% */
            char cmd[24];
            snprintf(cmd, sizeof(cmd), "AT+QFCLOSE=%d", lo_send_fd);
            lo_at(cmd);
            lo_enter(LO_CLOSE_DONE);
        } else if (strcmp(data, "OTA:DONE:ERR") == 0) {
            lo_error("slave CRC32 mismatch");
        }
        break;

    default:
        break;
    }
}

/* ── LoRaOta_Process — called every main loop iteration ─────────────────────── */
void LoRaOta_Process(void)
{
    HAL_IWDG_Refresh(&hiwdg);

    /* ── chunk-ready processing ─────────────────────────────────────────────── */
    if (lo_chunk_ready) {
        lo_chunk_ready = false;

        if (lo_state == LO_SCAN_DATA) {
            /* Accumulate CRC32 over actual bytes (up to fw_size) */
            uint32_t actual = lo_chunk_actual;
            uint32_t remaining = lo_fw_size - lo_scan_bytes;
            if (actual > remaining) actual = remaining;
            lo_scan_crc32 = crc32_update(lo_scan_crc32, lo_chunk, actual);
            lo_scan_bytes += actual;

            if (lo_scan_bytes >= lo_fw_size) {
                /* Scan complete — close file */
                char cmd[24];
                snprintf(cmd, sizeof(cmd), "AT+QFCLOSE=%d", lo_scan_fd);
                lo_at(cmd);
                lo_enter(LO_SCAN_CLOSE);
            } else {
                lo_scan_read_next();
            }
        }
        else if (lo_state == LO_SEND_DATA) {
            /* Build OTA:D packet from chunk (last chunk already padded with 0xFF) */
            lo_build_pkt(lo_cur_seq);
            lo_lora_send(lo_pkt_buf);
            lo_enter(LO_SEND_PKT);
        }
        return;
    }

    /* ── timeout handling ───────────────────────────────────────────────────── */
    uint32_t elapsed = HAL_GetTick() - lo_state_ms;

    uint32_t timeout = LORA_OTA_STEP_TIMEOUT;

    /* LoRa states need longer timeout */
    if (lo_state == LO_LORA_START  ||
        lo_state == LO_LORA_QUERY  ||
        lo_state == LO_SEND_PKT    ||
        lo_state == LO_LORA_END)
        timeout = LORA_OTA_ACK_TIMEOUT;

    if (elapsed < timeout) return;  /* not timed out yet */

    switch (lo_state)
    {
    case LO_SEND_PKT:
        /* LoRa ACK timeout — retry packet */
        if (lo_retry_cnt < LORA_OTA_MAX_RETRY) {
            lo_retry_cnt++;
            char dbg[48];
            snprintf(dbg, sizeof(dbg),
                     "[LoRaOTA] ACK timeout pkt %u retry %u\r\n",
                     (unsigned)lo_cur_seq, (unsigned)lo_retry_cnt);
            Debug_Print(dbg);
            lo_lora_send(lo_pkt_buf);
            lo_enter(LO_SEND_PKT);
        } else {
            lo_error("LoRa ACK timeout max retries");
        }
        break;

    case LO_LORA_START:
        lo_error("LoRa START timeout");
        break;

    case LO_LORA_QUERY:
        lo_error("LoRa QUERY timeout");
        break;

    case LO_LORA_END:
        lo_error("LoRa END timeout");
        break;

    case LO_SCAN_DATA:
        lo_error("QFREAD scan timeout");
        break;

    case LO_SEND_DATA:
        lo_error("QFREAD send timeout");
        break;

    case LO_IDLE:
    case LO_DONE:
    case LO_ERROR:
        break;  /* no timeout in these states */

    default:
        lo_error("step timeout");
        break;
    }
}
