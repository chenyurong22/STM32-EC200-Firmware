/*
 * modem.c  —  EC200U MQTT driver for STM32G071 (PlatformIO / STM32Cube HAL)
 *
 * Replaces the SMS-based relay control with MQTT over 4G LTE.
 *
 * UART wiring (from your stm32g0xx_hal_msp.c):
 *   USART1  PB6=TX  PB7=RX  →  EC200U
 *   USART2  PA2=TX  PA3=RX  →  debug serial monitor (115200)
 *
 * HiveMQ Cloud broker  :  44ad82d486654d68b4ac738e12fb1236.s1.eu.hivemq.cloud:8883
 * Firebase project     :  pump-controller-4398d
 *
 * MQTT topics  (PUMP_ID = "01" for pump 1, "02" for pump 2):
 *   pump/01/status   ← STM32 publishes on state change / 60 s heartbeat
 *   pump/01/alerts   ← STM32 publishes on protection trip
 *   pump/01/cmd      → STM32 subscribes (relay on/off commands)
 *
 * Payload examples:
 *   status : {"relay1_state":1,"relay2_state":0,"v1":415.2,"v2":414.8,
 *              "v3":416.1,"current":12.4,"dry_run":false,"online":true}
 *   cmd    : {"relay1":1,"relay2":0}
 *   alerts : {"overvoltage":false,"undervoltage":false,
 *              "phase_loss":false,"dry_run_trip":true}
 */

#include "modem.h"
#include "modbus.h"
#include "main.h"
#include "ota.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* IWDG handle lives in main.c — refresh it inside long blocking delays */
extern IWDG_HandleTypeDef hiwdg;

extern volatile uint8_t g_boot_phase;
extern const char g_fw_ver[];

/* ═══════════════════════════════════════════════════════════════════════════
 * USER CONFIG  —  change these two lines per device before flashing
 * ═══════════════════════════════════════════════════════════════════════════ */
#define PUMP_ID  "01"                /* relay1 ID  — "01" prod, "03" test  */
#define PUMP_ID2 "02"                /* relay2 ID  — always PUMP_ID + 1    */
#define MQTT_USERNAME ""             /* anonymous — broker.emqx.io         */
#define MQTT_PASSWORD ""

/* ── APN — change to your SIM card ─────────────────────────────────────── */
/* Airtel: "airtelgprs.com"   Jio: "jionet"   BSNL: "bsnlnet"             */
#define SIM_APN "airtelgprs.com"

/* ── Broker ─────────────────────────────────────────────────────────────── */
#define BROKER_HOST "broker.emqx.io"
#define BROKER_PORT "8883"
#define CLIENT_ID "pump" PUMP_ID

/* ── Topics ─────────────────────────────────────────────────────────────── */
#define TOPIC_STATUS   "pump/" PUMP_ID "/status"
#define TOPIC_CMD      "pump/" PUMP_ID "/cmd"
#define TOPIC_ALERTS   "pump/" PUMP_ID "/alerts"
#define TOPIC_OTA        "pump/" PUMP_ID "/ota"
#define TOPIC_OTA_STATUS "pump/" PUMP_ID "/ota/status"
#define TOPIC_SETTINGS "pump/" PUMP_ID "/settings"
#define TOPIC_LOG      "pump/" PUMP_ID "/log"
/* relay2 — same STM32, topics derived from PUMP_ID2 */
#define TOPIC_CMD2       "pump/" PUMP_ID2 "/cmd"
#define TOPIC_STATUS2    "pump/" PUMP_ID2 "/status"
#define TOPIC_ALERTS2    "pump/" PUMP_ID2 "/alerts"
#define TOPIC_LOG2       "pump/" PUMP_ID2 "/log"
#define TOPIC_SETTINGS2  "pump/" PUMP_ID2 "/settings"

/* ── Protection thresholds — runtime configurable via TOPIC_SETTINGS ─────── */
static float cfg_ov    = 480.0f; /* V  — any L-N above this trips relay    */
static float cfg_uv    = 340.0f; /* V  — any L-N below this trips relay    */
static float cfg_pl    = 200.0f; /* V  — L-N below this = phase lost       */
static float cfg_dry_i = 1.5f;   /* A  — below this = dry running (relay1) */
static int   cfg_dry_t = 8;      /* s  — consecutive seconds before trip   */
static int   cfg_hp     = 0;     /* pump rating: 5=5HP 75=7.5HP 0=custom   */
static int   cfg_dry_en  = 1;   /* 1=dry-run enabled  0=disabled           */
static int   cfg_start_t = 300; /* s  — startup grace: skip dry-run count  */
/* ── Relay2-specific settings (dry_i/dry_t/dry_en/hp only — voltage shared) */
static float cfg_dry_i2  = 1.5f; /* A  — dry run threshold for relay2      */
static int   cfg_dry_t2  = 8;    /* s  — dry run delay for relay2          */
static int   cfg_dry_en2 = 1;    /* 1=dry-run enabled for relay2            */
static int   cfg_hp2     = 0;    /* relay2 pump rating                       */
static int   cfg_start_t2 = 300; /* s  — startup grace for relay2           */
#define LOCKOUT_MS 300000UL       /* 5 min lockout after dry-run trip       */
/* Minimum current to consider the motor actually running.
 * If i < this threshold the motor is completely de-energised (external
 * preventer/timer has opened the contactor) — not a dry-run condition.
 * Dry-run only counts when the motor IS spinning but drawing low current. */
#define DRY_RUN_MIN_I_A  0.3f

/* ── Heartbeat interval (event-driven: also publish on every state change) ── */
#define HEARTBEAT_INTERVAL_MS 10000UL   /* publish status every 10 s */
/* QoS0 PUBEX acks can arrive late when modem/network is busy. */
#define MQTT_PUBACK_TIMEOUT_MS 12000UL
#define OTA_POST_QMTCLOSE_DELAY_MS 8000UL
#define OTA_HTTP_REDIRECT_ENABLED 0  /* firmware served from Railway — no redirect */
/* Block immediate OTA retrigger for a while after modem reboot during OTA.
 * This avoids retained pump/01/ota payload causing endless retry loops. */
#define OTA_RETRY_BLOCK_MS 300000UL
/* Raw line tracing is very chatty and can starve UART servicing at 9600 debug. */
#define MODEM_TRACE_LINES 0

/* ── RX buffer ──────────────────────────────────────────────────────────── */
#define RX_BUF_SIZE 512

/* ═══════════════════════════════════════════════════════════════════════════
 * Internal state
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef enum
{
    MQTT_STATE_BOOT,         /* waiting for EC200U power-on            */
    MQTT_STATE_NET_WAIT,     /* waiting for network registration       */
    MQTT_STATE_PDP_OPEN,     /* send AT+QICSGP, wait OK               */
    MQTT_STATE_PDP_ACTIVATE, /* send AT+QIACT=1, wait OK              */
    MQTT_STATE_SSL_CFG,      /* configuring SSL for port 8883          */
    MQTT_STATE_BROKER_OPEN,  /* AT+QMTOPEN — TCP to broker             */
    MQTT_STATE_CONNECTING,   /* AT+QMTCONN — MQTT handshake            */
    MQTT_STATE_SUBSCRIBING,  /* AT+QMTSUB  — subscribe to cmd topic    */
    MQTT_STATE_CONNECTED,    /* fully operational                      */
    MQTT_STATE_PUBLISHING,   /* waiting for '>' prompt after PUBEX     */
    MQTT_STATE_PUB_WAIT_OK,  /* waiting for +QMTPUBEX: 0,0,0          */
    MQTT_STATE_DISCONNECTED, /* lost connection — will reconnect       */
} MqttState;

static UART_HandleTypeDef *modem_uart;
static MqttState mqtt_state = MQTT_STATE_BOOT;
static char rxbuf[RX_BUF_SIZE];
static size_t rxpos = 0;
static uint32_t state_entered_ms = 0;

/* relay + protection */
static bool     relay1 = false;
static bool     relay2 = false;
static uint32_t relay1_on_tick = 0;      /* HAL_GetTick() when relay1 last turned ON */
static uint32_t relay2_on_tick = 0;      /* HAL_GetTick() when relay2 last turned ON */
static uint32_t run_accum1_ms = 0;       /* accumulated confirmed-running time for relay1 (ms) */
static uint32_t run_accum2_ms = 0;       /* accumulated confirmed-running time for relay2 (ms) */
static uint32_t last_r1_run_tick = 0;    /* HAL_GetTick() of last run_protection tick where r1 was running */
static uint32_t last_r2_run_tick = 0;    /* HAL_GetTick() of last run_protection tick where r2 was running */
static bool volt_alert_sent1 = false;    /* voltage fault alert was published for relay1 */
static bool volt_alert_sent2 = false;    /* voltage fault alert was published for relay2 */
static bool recv_payload_pending = false; /* true when +QMTRECV payload on next line */
static char recv_pending_topic[48] = ""; /* topic of pending split-line payload     */
static uint8_t  dry_run_count   = 0;
static bool     dry_run_tripped = false;
static uint32_t lockout_until   = 0;
static uint8_t  dry_run_count2  = 0;
static bool     dry_run_tripped2 = false;
static uint32_t lockout_until2  = 0;

/* publish queue — one pending payload at a time */
static char pub_topic[48];
static char pub_payload[512];
static bool pub_pending = false;

/* OTA error saved here so it survives MQTT reconnect (heartbeat would overwrite pub_pending) */
static char ota_error_msg[80];
static bool force_status_after_ota = false;
/* Deferred pump02 status publish — set true whenever status2 needs sending;
 * processed in the main loop as soon as pub_pending clears.
 * Avoids the drop caused by publish_status() setting pub_pending immediately
 * before publish_status2() is called. */
static bool pub_status2_needed = false;

/* Modem stuck-detection: if not CONNECTED for 90 s → hardware reset EC200U via PC14.
 * 90 s covers: OTA reboot (~30 s init) + network registration (~30 s) + margin.
 * Catches the case where ota_reboot PC14 reset in Modem_Init did not fire
 * (e.g. sentinel not detected) and MQTT is still stuck post-OTA.             */
static uint32_t disconnected_since_ms = 0;
#define MODEM_HARD_RESET_TIMEOUT_MS 90000UL   /* 90 seconds */

/* event-driven publish: track previous state to detect changes */
static uint32_t last_heartbeat_ms  = 0;
static bool     prev_relay1        = false;
static bool     prev_relay2        = false;
static bool     prev_dry_run_trip  = false;

/* signal strength: updated by +CSQ response; 99 = unknown */
static int8_t   last_rssi          = 99;
static uint32_t ota_retry_block_until = 0;
/* Grace period: suppress modem_reinit for 20s after Modem_Init returns.
 * EC200U post-CFUN=1,1 boot may send late URCs (APP RDY) that arrive during
 * the SSL/MQTT config phase — these must not trigger a spurious modem_reinit. */
static uint32_t modem_init_grace_until = 0;

/* set false when PDP_OPEN is entered; set true when AT+QICSGP is sent from
 * Modem_Process (after the RX buffer is drained) so we never mistake the
 * trailing OK of AT+CGREG? for QICSGP's OK.                                */
static bool qicsgp_sent = false;

/* set false when PDP_ACTIVATE is entered; set true when AT+QIACT=1 is sent
 * from Modem_Process (after the RX buffer is drained) so we never mistake
 * stale OK responses from AT+QMTCLOSE/AT+QIDEACT for QIACT's OK.          */
static bool qiact_sent = false;

/* set true on first AT+QIACT=1 ERROR so the handler force-deactivates and
 * retries once before falling back to NET_WAIT.  Reset on PDP_ACTIVATE entry
 * and on success.  Fixes the post-OTA loop: QIDEACT may silently fail in
 * Modem_Init if HTTP is still closing, leaving the PDP context active.
 * EC200U returns ERROR (not OK) when AT+QIACT=1 is sent on an already-active
 * context, which previously caused an infinite NET_WAIT/PDP loop.           */
static bool qiact_retry_done = false;

/* set true when +QMTOPEN: 0,"hostname",port URC arrives (TLS handshake
 * started).  Success fires only on the subsequent OK (TLS complete).     */
static bool qmtopen_tls_seen = false;
/* Set when unsolicited modem reboot URCs are seen (RDY/+CFUN: 1) so
 * Modem_Process can run a full Modem_Init() re-initialization. */
static bool modem_reinit_pending = false;

static bool ota_retry_blocked(void)
{
    return ((int32_t)(HAL_GetTick() - ota_retry_block_until) < 0);
}

static void modem_log_reset_flags(uint32_t csr)
{
    char flags[128];
    flags[0] = '\0';
#ifdef RCC_CSR_LPWRRSTF
    if (csr & RCC_CSR_LPWRRSTF) strcat(flags, " LPWR");
#endif
#ifdef RCC_CSR_WWDGRSTF
    if (csr & RCC_CSR_WWDGRSTF) strcat(flags, " WWDG");
#endif
#ifdef RCC_CSR_IWDGRSTF
    if (csr & RCC_CSR_IWDGRSTF) strcat(flags, " IWDG");
#endif
#ifdef RCC_CSR_SFTRSTF
    if (csr & RCC_CSR_SFTRSTF) strcat(flags, " SFTRST");
#endif
#ifdef RCC_CSR_PORRSTF
    if (csr & RCC_CSR_PORRSTF) strcat(flags, " POR");
#endif
#ifdef RCC_CSR_PINRSTF
    if (csr & RCC_CSR_PINRSTF) strcat(flags, " PIN");
#endif
#ifdef RCC_CSR_BORRSTF
    if (csr & RCC_CSR_BORRSTF) strcat(flags, " BOR");
#endif
    if (flags[0] == '\0') strcpy(flags, " <none>");
    {
        char dbg[160];
        snprintf(dbg, sizeof(dbg), "[MODEM] Reset flags:%s\r\n", flags);
        Debug_Print(dbg);
    }
}


/* ═══════════════════════════════════════════════════════════════════════════
 * Low-level UART helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

void Modem_Send(const char *cmd)
{
    if (!modem_uart || !cmd)
        return;
    HAL_UART_Transmit(modem_uart, (const uint8_t *)cmd, strlen(cmd), 2000);
}

static void modem_cmd(const char *cmd)
{
    Modem_Send(cmd);
    Modem_Send("\r\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Diagnostic blink  — N × 80 ms pulses on Relay1 (PA5), saves & restores.
 * Used to indicate MQTT progress without needing a UART adapter.
 *   blink_n(1) = QMTOPEN TCP connected (sent AT+QMTCONN)
 *   blink_n(2) = publish confirmed (broker ACK / OK received)
 *   blink_n(3) = MQTT fully connected (subscribed)  ← SUCCESS
 *   blink_n(4) = AT+QMTPUBEX command sent (publish attempted)
 *   blink_n(5) = QMTCONN REFUSED  ← wrong credentials / broker rejected
 *   blink_n(6) = QMTCONN ERROR    ← AT command layer error
 *   blink_n(7) = server dropped connection (+QMTSTAT received)
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ═══════════════════════════════════════════════════════════════════════════
 * Relay control
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Latching relay via transistor (HIGH = transistor ON = coil energised):
 *   Pump1: PA1 → SET coil, PB3 → RESET coil
 *   Pump2: PB4 → SET coil, PB5 → RESET coil
 * Pulse HIGH 200 ms to latch; relay holds position without power */

void Relay1_Set(bool on)
{
    relay1 = on;
    if (on) {
        HAL_GPIO_WritePin(Relay_Pin_GPIO_Port,  Relay_Pin_Pin,  GPIO_PIN_SET);   /* PB3 HIGH — SET coil pulse start  */
        HAL_Delay(200);
        HAL_GPIO_WritePin(Relay_Pin_GPIO_Port,  Relay_Pin_Pin,  GPIO_PIN_RESET); /* PB3 LOW  — SET coil pulse end    */
    } else {
        HAL_GPIO_WritePin(Relay1_RST_GPIO_Port, Relay1_RST_Pin, GPIO_PIN_SET);   /* PA1 HIGH — RESET coil pulse start */
        HAL_Delay(200);
        HAL_GPIO_WritePin(Relay1_RST_GPIO_Port, Relay1_RST_Pin, GPIO_PIN_RESET); /* PA1 LOW  — RESET coil pulse end   */
    }
}

void Relay2_Set(bool on)
{
    relay2 = on;
    if (on) {
        HAL_GPIO_WritePin(Relay2_Pin_GPIO_Port, Relay2_Pin_Pin, GPIO_PIN_SET);   /* PB4 HIGH — SET coil pulse start  */
        HAL_Delay(200);
        HAL_GPIO_WritePin(Relay2_Pin_GPIO_Port, Relay2_Pin_Pin, GPIO_PIN_RESET); /* PB4 LOW  — SET coil pulse end    */
    } else {
        HAL_GPIO_WritePin(Relay2_RST_GPIO_Port, Relay2_RST_Pin, GPIO_PIN_SET);   /* PB5 HIGH — RESET coil pulse start */
        HAL_Delay(200);
        HAL_GPIO_WritePin(Relay2_RST_GPIO_Port, Relay2_RST_Pin, GPIO_PIN_RESET); /* PB5 LOW  — RESET coil pulse end   */
    }
}

static void blink_n(int n)
{
    /* PA1 is now RESET coil — cannot use for diagnostic blink (would unlatch relay) */
    (void)n;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Connection status
 * ═══════════════════════════════════════════════════════════════════════════ */

bool Modem_IsConnected(void)
{
    return mqtt_state == MQTT_STATE_CONNECTED;
}

bool Relay1_Get(void) { return relay1; }
bool Relay2_Get(void) { return relay2; }

/* ═══════════════════════════════════════════════════════════════════════════
 * Sensor stubs
 * Override these in a separate sensors.c once you wire up ADC channels.
 * Until then they return safe dummy values so the MQTT loop works.
 * ═══════════════════════════════════════════════════════════════════════════ */

__attribute__((weak)) float Sensor_ReadVoltagePhase1(void) { return 415.0f; }
__attribute__((weak)) float Sensor_ReadVoltagePhase2(void) { return 415.0f; }
__attribute__((weak)) float Sensor_ReadVoltagePhase3(void) { return 415.0f; }
__attribute__((weak)) float Sensor_ReadCurrentACS712(void) { return 5.0f; }

/* ═══════════════════════════════════════════════════════════════════════════
 * MQTT publish helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

static void queue_publish(const char *topic, const char *payload)
{
    if (pub_pending)
        return; /* drop if previous not sent yet      */

    strncpy(pub_topic, topic, sizeof(pub_topic) - 1);
    strncpy(pub_payload, payload, sizeof(pub_payload) - 1);
    pub_topic[sizeof(pub_topic) - 1] = '\0';
    pub_payload[sizeof(pub_payload) - 1] = '\0';
    pub_pending = true;
}

/* Format float to 1 decimal place without requiring -u_printf_float.
 * Handles negative values and clamps out-of-range to "0.0".           */
static void fmt_f1(char *out, int sz, float v)
{
    if (v != v || v > 99999.0f || v < -9999.0f) { snprintf(out, sz, "0.0"); return; }
    int neg = (v < 0.0f);
    float a = neg ? -v : v;
    int   w = (int)a;
    int   d = (int)((a - (float)w) * 10.0f);
    if (neg) snprintf(out, sz, "-%d.%d",  w, d);
    else     snprintf(out, sz,  "%d.%d",  w, d);
}

static void fmt_f2(char *out, int sz, float v)
{
    if (v != v || v > 9999.0f || v < -999.0f) { snprintf(out, sz, "0.00"); return; }
    int neg = (v < 0.0f);
    float a = neg ? -v : v;
    int   w = (int)a;
    int   d = (int)((a - (float)w) * 100.0f);
    if (neg) snprintf(out, sz, "-%d.%02d", w, d);
    else     snprintf(out, sz,  "%d.%02d", w, d);
}

static void publish_status(void)
{
    float v1 = Sensor_ReadVoltagePhase1();
    float v2 = Sensor_ReadVoltagePhase2();
    float v3 = Sensor_ReadVoltagePhase3();
    float i  = Sensor_ReadCurrentACS712();

    char sv1[12], sv2[12], sv3[12], sci[12];
    fmt_f1(sv1, sizeof(sv1), v1);
    fmt_f1(sv2, sizeof(sv2), v2);
    fmt_f1(sv3, sizeof(sv3), v3);
    fmt_f2(sci, sizeof(sci), i);

    char scfg_ov[10], scfg_uv[10], scfg_pl[10], scfg_dry_i[10], scfg_dry_i2[10];
    fmt_f1(scfg_ov,    sizeof(scfg_ov),    cfg_ov);
    fmt_f1(scfg_uv,    sizeof(scfg_uv),    cfg_uv);
    fmt_f1(scfg_pl,    sizeof(scfg_pl),    cfg_pl);
    fmt_f2(scfg_dry_i, sizeof(scfg_dry_i), cfg_dry_i);
    fmt_f2(scfg_dry_i2,sizeof(scfg_dry_i2),cfg_dry_i2);

    bool data_ok   = Modbus_IsDataValid();
    bool r1_running = relay1 && data_ok && (i > cfg_dry_i);
    bool r2_running = relay2 && data_ok && (i > cfg_dry_i2); /* relay2 uses its own threshold */

    char payload[640];
    snprintf(payload, sizeof(payload),
             "{\"relay1_state\":%d,\"relay2_state\":%d,"
             "\"relay1_running\":%d,\"relay2_running\":%d,"
             "\"v1\":%s,\"v2\":%s,\"v3\":%s,"
             "\"current\":%s,"
             "\"dry_run\":%s,\"dry_run2\":%s,\"online\":true,"
             "\"mb_ok\":%d,\"mb_rx\":%d,\"rssi\":%d,\"boot_phase\":%u,"
             "\"fw\":\"%s\","
             "\"cfg_ov\":%s,\"cfg_uv\":%s,\"cfg_pl\":%s,"
             "\"cfg_dry_i\":%s,\"cfg_dry_t\":%d,\"cfg_start_t\":%d,\"cfg_hp\":%d,\"cfg_dry_en\":%d,"
             "\"cfg_dry_i2\":%s,\"cfg_dry_t2\":%d,\"cfg_start_t2\":%d,\"cfg_hp2\":%d,\"cfg_dry_en2\":%d}",
             relay1 ? 1 : 0,
             relay2 ? 1 : 0,
             r1_running ? 1 : 0,
             r2_running ? 1 : 0,
             sv1, sv2, sv3, sci,
             dry_run_tripped  ? "true" : "false",
             dry_run_tripped2 ? "true" : "false",
             data_ok ? 1 : 0,
             (int)Modbus_GetLastRx(),
             (int)last_rssi,
             (unsigned)g_boot_phase,
             g_fw_ver,
             scfg_ov, scfg_uv, scfg_pl,
             scfg_dry_i, cfg_dry_t, cfg_start_t, cfg_hp, cfg_dry_en,
             scfg_dry_i2, cfg_dry_t2, cfg_start_t2, cfg_hp2, cfg_dry_en2);

    queue_publish(TOPIC_STATUS, payload);
}

static void publish_status2(void)
{
    /* pump02 status — relay1_state maps to physical relay2 (PB4/PB5) */
    char payload[48];
    snprintf(payload, sizeof(payload),
             "{\"relay1_state\":%d,\"online\":true}",
             relay2 ? 1 : 0);
    queue_publish(TOPIC_STATUS2, payload);
}

static void publish_alert(bool ov, bool uv, bool pl, bool dr)
{
    char payload[128];
    snprintf(payload, sizeof(payload),
             "{\"overvoltage\":%s,\"undervoltage\":%s,"
             "\"phase_loss\":%s,\"dry_run_trip\":%s}",
             ov ? "true" : "false",
             uv ? "true" : "false",
             pl ? "true" : "false",
             dr ? "true" : "false");

    queue_publish(TOPIC_ALERTS, payload);
}

static void publish_alert2(bool ov, bool uv, bool pl, bool dr)
{
    char payload[128];
    snprintf(payload, sizeof(payload),
             "{\"overvoltage\":%s,\"undervoltage\":%s,"
             "\"phase_loss\":%s,\"dry_run_trip\":%s}",
             ov ? "true" : "false",
             uv ? "true" : "false",
             pl ? "true" : "false",
             dr ? "true" : "false");
    queue_publish(TOPIC_ALERTS2, payload);
}

static void RelayState_Save(void); /* forward declaration */

/* ═══════════════════════════════════════════════════════════════════════════
 * Relay log — publishes on/off events to TOPIC_LOG
 * ═══════════════════════════════════════════════════════════════════════════ */

static void log_relay_event(int relay_num, bool on, const char *reason)
{
    char payload[96];
    uint32_t *on_tick = (relay_num == 2) ? &relay2_on_tick   : &relay1_on_tick;
    uint32_t *accum   = (relay_num == 2) ? &run_accum2_ms    : &run_accum1_ms;
    uint32_t *last_tk = (relay_num == 2) ? &last_r2_run_tick : &last_r1_run_tick;
    if (on) {
        *on_tick  = HAL_GetTick();
        *accum    = 0;   /* reset accumulator for new run session */
        *last_tk  = 0;
        snprintf(payload, sizeof(payload),
                 "{\"event\":\"on\",\"reason\":\"%s\",\"relay\":%d}", reason, relay_num);
    } else {
        /* run_s = total confirmed-running time this session.
         * Accumulator only increments when current > threshold, so preventer
         * OFF gaps (current=0) are excluded from the run time. */
        uint32_t run_s = *accum / 1000;
        *on_tick  = 0;
        *accum    = 0;
        *last_tk  = 0;
        snprintf(payload, sizeof(payload),
                 "{\"event\":\"off\",\"reason\":\"%s\",\"run_s\":%lu,\"relay\":%d}",
                 reason, (unsigned long)run_s, relay_num);
    }
    queue_publish((relay_num == 2) ? TOPIC_LOG2 : TOPIC_LOG, payload);
    RelayState_Save();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Protection logic  — called every 1 s when connected
 * ═══════════════════════════════════════════════════════════════════════════ */

static bool is_volt_fault(void)
{
    float v1 = Sensor_ReadVoltagePhase1();
    float v2 = Sensor_ReadVoltagePhase2();
    float v3 = Sensor_ReadVoltagePhase3();
    return (v1 > cfg_ov || v2 > cfg_ov || v3 > cfg_ov) ||
           (v1 < cfg_uv || v2 < cfg_uv || v3 < cfg_uv) ||
           (v1 < cfg_pl || v2 < cfg_pl || v3 < cfg_pl);
}

static void run_protection(void)
{
    static bool prev_r1_running = false;
    static bool prev_r2_running = false;

    float v1 = Sensor_ReadVoltagePhase1();
    float v2 = Sensor_ReadVoltagePhase2();
    float v3 = Sensor_ReadVoltagePhase3();
    float i = Sensor_ReadCurrentACS712();

    bool ov = (v1 > cfg_ov || v2 > cfg_ov || v3 > cfg_ov);
    bool uv = (v1 < cfg_uv || v2 < cfg_uv || v3 < cfg_uv);
    bool pl = (v1 < cfg_pl || v2 < cfg_pl || v3 < cfg_pl);

    /* Startup grace windows — computed once, reused for both voltage and dry-run suppression.
     * During the grace window UV+PL are ignored: motor inrush can briefly dip the supply.
     * OV protection stays active at all times (overvoltage during startup is dangerous). */
    bool relay1_in_startup = relay1_on_tick &&
        (HAL_GetTick() - relay1_on_tick < (uint32_t)cfg_start_t * 1000U);
    bool relay2_in_startup = relay2_on_tick &&
        (HAL_GetTick() - relay2_on_tick < (uint32_t)cfg_start_t2 * 1000U);

    /* OV always trips.  UV+PL only trip once the startup grace window has expired. */
    bool volt_trip1 = ov || (!relay1_in_startup && (uv || pl));
    bool volt_trip2 = ov || (!relay2_in_startup && (uv || pl));

    if (volt_trip1 && relay1)
    {
        Relay1_Set(false);
        log_relay_event(1, false, ov ? "overvoltage" : (uv ? "undervoltage" : "phase_loss"));
        publish_alert(ov, uv, pl, false);
        volt_alert_sent1 = true;
        Debug_Print("[PROT] Voltage fault — pump1 OFF\r\n");
    }

    if (volt_trip2 && relay2)
    {
        Relay2_Set(false);
        log_relay_event(2, false, ov ? "overvoltage" : (uv ? "undervoltage" : "phase_loss"));
        publish_alert2(ov, uv, pl, false);
        volt_alert_sent2 = true;
        Debug_Print("[PROT] Voltage fault — pump2 OFF\r\n");
    }

    if ((ov || uv || pl) && (relay1 || relay2))
        return;

    /* Voltage OK — clear any outstanding voltage fault alert in Firebase */
    if (!ov && !uv && !pl)
    {
        if (volt_alert_sent1) {
            volt_alert_sent1 = false;
            publish_alert(false, false, false, false);
            Debug_Print("[PROT] Voltage fault cleared — pump1 alert reset\r\n");
        }
        if (volt_alert_sent2) {
            volt_alert_sent2 = false;
            publish_alert2(false, false, false, false);
            Debug_Print("[PROT] Voltage fault cleared — pump2 alert reset\r\n");
        }
    }

    /* relay1_in_startup already computed above (shared grace window for voltage + dry-run) */
    if (relay1_in_startup)
    {
        dry_run_count = 0; /* keep counter clear during startup grace */
    }
    else if (relay1 && !dry_run_tripped && cfg_dry_en)
    {
        /* i >= DRY_RUN_MIN_I_A: motor is running but drawing low current → dry run.
         * i <  DRY_RUN_MIN_I_A: motor de-energised by external preventer/timer → ignore. */
        if (i >= DRY_RUN_MIN_I_A && i < cfg_dry_i)
        {
            dry_run_count++;
            if (dry_run_count >= (uint8_t)cfg_dry_t)
            {
                dry_run_tripped = true;
                lockout_until = HAL_GetTick() + LOCKOUT_MS;
                Relay1_Set(false);
                log_relay_event(1, false, "dry_run");
                publish_alert(false, false, false, true);
                Debug_Print("[PROT] Dry run — pump OFF + lockout\r\n");
            }
        }
        else
        {
            dry_run_count = 0;
        }
    }

    if (dry_run_tripped && HAL_GetTick() >= lockout_until)
    {
        dry_run_tripped = false;
        dry_run_count = 0;
        publish_alert(false, false, false, false);
        Debug_Print("[PROT] Lockout cleared\r\n");
    }

    /* ── Relay2 dry run protection ─────────────────────────────────────── */
    /* relay2_in_startup already computed above (shared grace window for voltage + dry-run) */
    if (relay2_in_startup)
    {
        dry_run_count2 = 0;
    }
    else if (relay2 && !dry_run_tripped2 && cfg_dry_en2)
    {
        if (i >= DRY_RUN_MIN_I_A && i < cfg_dry_i2)
        {
            dry_run_count2++;
            if (dry_run_count2 >= (uint8_t)cfg_dry_t2)
            {
                dry_run_tripped2 = true;
                lockout_until2 = HAL_GetTick() + LOCKOUT_MS;
                Relay2_Set(false);
                log_relay_event(2, false, "dry_run");
                publish_alert2(false, false, false, true);
                Debug_Print("[PROT] Dry run2 — pump2 OFF + lockout\r\n");
            }
        }
        else
        {
            dry_run_count2 = 0;
        }
    }

    if (dry_run_tripped2 && HAL_GetTick() >= lockout_until2)
    {
        dry_run_tripped2 = false;
        dry_run_count2 = 0;
        publish_alert2(false, false, false, false);
        Debug_Print("[PROT] Lockout2 cleared\r\n");
    }

    /* Publish immediately when running state changes (relay ON + current confirmed).
     * Without this, STARTING→RUNNING transition waits up to 10s for next heartbeat.
     * run_protection fires every 1s; Modbus updates every 2s — transition seen within 3s. */
    bool data_ok   = Modbus_IsDataValid();
    bool r1_running = relay1 && data_ok && (i > cfg_dry_i);
    bool r2_running = relay2 && data_ok && (i > cfg_dry_i2); /* relay2 uses its own threshold */

    /* Accumulate confirmed-running time — only when current > threshold.
     * Pauses automatically when preventer/timer cuts the contactor (current=0).
     * Resumes when current returns. run_s in the log reflects actual motor
     * running time, not wall-clock time from first detection to relay OFF. */
    uint32_t _now = HAL_GetTick();
    if (r1_running) {
        if (last_r1_run_tick) run_accum1_ms += _now - last_r1_run_tick;
        last_r1_run_tick = _now;
    } else {
        last_r1_run_tick = 0;   /* pause accumulator while motor is stopped */
    }
    if (r2_running) {
        if (last_r2_run_tick) run_accum2_ms += _now - last_r2_run_tick;
        last_r2_run_tick = _now;
    } else {
        last_r2_run_tick = 0;
    }

    if (r1_running != prev_r1_running)
    {
        prev_r1_running = r1_running;
        publish_status();
    }
    if (r2_running != prev_r2_running)
    {
        prev_r2_running = r2_running;
        publish_status2();
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * JSON field extractor  — extracts integer value for a key
 * e.g. extract_int("{\"relay1\":1,\"relay2\":0}", "relay1") → 1
 * ═══════════════════════════════════════════════════════════════════════════ */

static int extract_int(const char *json, const char *key)
{
    char search[32];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p)
        return -1;
    p += strlen(search);
    while (*p == ' ')
        p++;
    return atoi(p);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * JSON float field extractor — returns float value for key, or def if missing
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Lightweight float parser — avoids newlib strtod() bloat (~15 KB).
 * Handles formats like "1.5", "480", "-2.3". */
static float parse_float_lite(const char *s)
{
    while (*s == ' ') s++;
    float sign = 1.0f;
    if (*s == '-') { sign = -1.0f; s++; }
    float result = 0.0f;
    while (*s >= '0' && *s <= '9') { result = result * 10.0f + (*s++ - '0'); }
    if (*s == '.') {
        s++;
        float frac = 0.1f;
        while (*s >= '0' && *s <= '9') { result += (*s++ - '0') * frac; frac *= 0.1f; }
    }
    return sign * result;
}

static float extract_float(const char *json, const char *key, float def)
{
    char search[32];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return def;
    p += strlen(search);
    while (*p == ' ') p++;
    return parse_float_lite(p);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * JSON string field extractor — copies value of "key":"<value>" into out
 * Returns true if key was found and value is non-empty.
 * ═══════════════════════════════════════════════════════════════════════════ */

static bool extract_str(const char *json, const char *key, char *out, size_t max)
{
    char search[40];
    snprintf(search, sizeof(search), "\"%s\":\"", key);
    const char *p = strstr(json, search);
    if (!p) return false;
    p += strlen(search);
    size_t i = 0;
    while (*p && *p != '"' && i < max - 1)
        out[i++] = *p++;
    out[i] = '\0';
    return (i > 0);
}

/* Accept OTA trigger payloads in either format:
 *   {"url":"https://..."}   or   https://...
 * Extract URL from any line containing http/https text. */
static bool extract_ota_url_any(const char *text, char *out, size_t max)
{
    const char *p = strstr(text, "https://");
    if (!p) p = strstr(text, "http://");
    if (!p) return false;

    size_t i = 0;
    while (*p && i < max - 1) {
        char c = *p;
        if (c == '"' || c == ',' || c == ' ' || c == '\r' || c == '\n')
            break;
        out[i++] = c;
        p++;
    }
    out[i] = '\0';
    return (i > 0);
}

static void modem_ota_start(const char *url); /* forward declaration */
static void modem_ota_publish(const char *topic, const char *payload); /* forward declaration */
static bool modem_is_exact_reboot_urc(const char *line);

/* ── Relay state persistence (Flash page 31 = 0x0800F800, 2KB) ──────────────
 * Saves relay1/relay2 ON/OFF state so a power-cycle reboot restores the
 * physical latching-relay position correctly instead of defaulting to OFF. */
#define RELAY_STATE_MAGIC  0xFEED5A5AU
#define RELAY_STATE_ADDR   0x0800F800U
#define RELAY_STATE_PAGE   31U

typedef struct {
    uint32_t magic;
    uint32_t relay1;   /* 1 = ON, 0 = OFF */
    uint32_t relay2;
    uint32_t _pad;     /* 16 bytes total — 2 doublewords */
} RelayState_t;

static void RelayState_Save(void)
{
    RelayState_t s;
    s.magic  = RELAY_STATE_MAGIC;
    s.relay1 = relay1 ? 1U : 0U;
    s.relay2 = relay2 ? 1U : 0U;
    s._pad   = 0U;

    HAL_FLASH_Unlock();
    FLASH_EraseInitTypeDef erase = {
        .TypeErase = FLASH_TYPEERASE_PAGES,
        .Page      = RELAY_STATE_PAGE,
        .NbPages   = 1,
    };
    uint32_t page_err = 0;
    HAL_FLASHEx_Erase(&erase, &page_err);

    uint64_t buf[2];
    memcpy(buf, &s, sizeof(s));
    HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, RELAY_STATE_ADDR,       buf[0]);
    HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, RELAY_STATE_ADDR + 8U,  buf[1]);
    HAL_FLASH_Lock();
    Debug_Print("[CFG] Relay state saved to Flash\r\n");
}

static void RelayState_Load(void)
{
    const RelayState_t *p = (const RelayState_t *)RELAY_STATE_ADDR;
    if (p->magic != RELAY_STATE_MAGIC) {
        Debug_Print("[CFG] No saved relay state — defaulting OFF\r\n");
        /* Bistable relays hold position through power-loss.
         * Explicitly reset both so contacts match the expected default. */
        Relay1_Set(false);
        Relay2_Set(false);
        return;
    }
    bool r1 = p->relay1 ? true : false;
    bool r2 = (p->relay2 && !r1) ? true : false;  /* interlock: relay2 OFF if relay1 ON */

    /* Step 1: RESET both relays to a known OFF state first.
     * Bistable relays hold their mechanical position through power-loss so
     * the physical state on boot is unknown.  Resetting both guarantees a
     * clean starting point before re-applying the saved state. */
    Relay1_Set(false);
    Relay2_Set(false);

    /* Step 2: wait 500 ms before re-energising.
     * On a watchdog/software reboot the motor contactor may still be closed
     * and the motor shaft still spinning.  The delay allows the contactor
     * to fully open and the motor to coast down before the SET pulse is
     * sent, avoiding inrush current and mechanical stress on re-close. */
    HAL_Delay(500);

    /* Step 3: restore saved state — SET whichever relay was ON. */
    if (r1) Relay1_Set(true);
    if (r2) Relay2_Set(true);
    Debug_Print("[CFG] Relay state restored from Flash\r\n");
}

/* Apply protection settings from JSON payload */
static void apply_settings(const char *json)
{
    float v;
    int   t;
    v = extract_float(json, "ov",    cfg_ov);    if (v > 0.0f) cfg_ov    = v;
    v = extract_float(json, "uv",    cfg_uv);    if (v > 0.0f) cfg_uv    = v;
    v = extract_float(json, "pl",    cfg_pl);    if (v > 0.0f) cfg_pl    = v;
    v = extract_float(json, "dry_i", cfg_dry_i); if (v > 0.0f) cfg_dry_i = v;
    t = extract_int(json, "dry_t");              if (t > 0)    cfg_dry_t = t;
    t = extract_int(json, "hp");                 if (t > 0)    cfg_hp     = t;
    if (strstr(json, "\"dry_en\":"))             cfg_dry_en = extract_int(json, "dry_en") ? 1 : 0;
    t = extract_int(json, "start_t");            if (t > 0)    cfg_start_t = t;
    Debug_Print("[CFG] Settings updated\r\n");
    publish_status(); /* reflect new cfg_ values immediately — don't wait for next heartbeat */
}

/* Apply relay2-specific settings (dry_i/dry_t/dry_en/hp only — voltage shared) */
static void apply_settings2(const char *json)
{
    float v;
    int   t;
    v = extract_float(json, "dry_i", cfg_dry_i2); if (v > 0.0f) cfg_dry_i2 = v;
    t = extract_int(json, "dry_t");               if (t > 0)    cfg_dry_t2 = t;
    t = extract_int(json, "hp");                  if (t > 0)    cfg_hp2    = t;
    if (strstr(json, "\"dry_en\":"))              cfg_dry_en2 = extract_int(json, "dry_en") ? 1 : 0;
    t = extract_int(json, "start_t");             if (t > 0)    cfg_start_t2 = t;
    Debug_Print("[CFG] Settings2 updated\r\n");
    publish_status(); /* cfg_dry_en2 is in pump01 status — publish immediately */
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Line processor — called for every complete line received from EC200U
 * ═══════════════════════════════════════════════════════════════════════════ */

static void process_line(const char *line)
{
    char dbg[80];

    /* ── always log non-empty lines to debug UART ── */
    if (line[0] && MODEM_TRACE_LINES)
    {
        snprintf(dbg, sizeof(dbg), "[EC200U] %s\r\n", line);
        Debug_Print(dbg);
    }

    /* Modem restarted unexpectedly (brownout/reset): all runtime MQTT/SSL
     * config is lost. Schedule a full Modem_Init() instead of partial retry.
     * Suppress during the 20s grace period after Modem_Init — late boot URCs
     * (APP RDY, +CFUN: 1) from the AT+CFUN=1,1 OTA reboot path must not
     * trigger a spurious re-init.                                            */
    if (!OTA_IsActive() &&
        modem_is_exact_reboot_urc(line) &&
        mqtt_state != MQTT_STATE_BOOT &&
        (int32_t)(HAL_GetTick() - modem_init_grace_until) >= 0)
    {
        modem_reinit_pending = true;
        Debug_Print("[MODEM] Unsolicited modem reboot detected\r\n");
    }

    /* Forward ALL lines to OTA state machine during an active OTA download.
     * OTA_HandleLine() handles HTTP/file URCs (+QHTTPGET, +QFOPEN, CONNECT).
     * Return early so MQTT logic doesn't misinterpret AT responses as MQTT. */
    if (OTA_IsActive())
    {
        OTA_HandleLine(line);
        return;
    }

    /* +QMTRECV: incoming command — handle in ANY state that has an active
     * MQTT session so relay commands work even during PUBLISHING/PUB_WAIT_OK.
     * Some EC200U firmware versions put the JSON payload on the SAME line:
     *   +QMTRECV: 0,0,"pump/01/cmd","{"relay1":1,"relay2":0}"
     * Others put it on the NEXT line (split format):
     *   +QMTRECV: 0,0,"pump/01/cmd",23
     *   {"relay1":1,"relay2":0}
     * The recv_payload_pending flag handles the split-line case.             */

    /* Case: payload arrived on line following +QMTRECV header */
    if (recv_payload_pending)
    {
        const char *json = strchr(line, '{');
        if (json)
        {
            recv_payload_pending = false;
            /* Settings topic — check recv_pending_topic (split-line) OR
             * the full line (inline response after AT+QMTRECV poll where
             * recv_pending_topic was cleared in the buffer-mode handler). */
            if (strstr(recv_pending_topic, TOPIC_SETTINGS) ||
                strstr(line, TOPIC_SETTINGS))
            {
                apply_settings(json);
                recv_pending_topic[0] = '\0';
                return;
            }
            if (strstr(recv_pending_topic, TOPIC_SETTINGS2) ||
                strstr(line, TOPIC_SETTINGS2))
            {
                apply_settings2(json);
                recv_pending_topic[0] = '\0';
                return;
            }
            /* pump/02/cmd — relay1 field maps to physical relay2 (PB4/PB5)
             * Check BOTH recv_pending_topic (split-line) AND current line
             * (inline: topic+payload on same AT+QMTRECV response line) */
            bool is_pump2_cmd = strstr(recv_pending_topic, TOPIC_CMD2) != NULL ||
                                 strstr(line, TOPIC_CMD2) != NULL;
            recv_pending_topic[0] = '\0';
            if (is_pump2_cmd)
            {
                int r = extract_int(json, "relay1");
                if (r >= 0)
                {
                    char cmd_src[16] = "";
                    extract_str(json, "src", cmd_src, sizeof(cmd_src));
                    if (r == 1 && relay1) {
                        Debug_Print("[CMD] Relay2 ON blocked — relay1 active\r\n");
                        return;
                    }
                    if (r == 1 && is_volt_fault()) {
                        Debug_Print("[CMD] Relay2 ON blocked — voltage fault\r\n");
                        return;
                    }
                    if (r == 1 && HAL_GetTick() < lockout_until2) {
                        Debug_Print("[CMD] Relay2 ON blocked — lockout active\r\n");
                        return;
                    }
                    bool prev2 = relay2;
                    Relay2_Set(r == 1);
                    if (relay2 != prev2) {
                        log_relay_event(2, relay2, cmd_src[0] ? cmd_src : "manual");
                        publish_status2();
                    }
                }
                return;
            }
            /* OTA command: {"url":"https://..."} */
            char ota_url[200];
            if (extract_str(json, "url", ota_url, sizeof(ota_url)))
            {
                if (ota_retry_blocked()) {
                    Debug_Print("[OTA] Ignored trigger (cooldown after reboot)\r\n");
                    return;
                }
                modem_ota_start(ota_url);
                return;
            }
            int r1 = extract_int(json, "relay1");
            if (r1 >= 0)
            {
                if (r1 == 1 && relay2)
                    Debug_Print("[CMD] Relay1 ON blocked — relay2 active\r\n");
                else if (r1 == 1 && HAL_GetTick() < lockout_until)
                    Debug_Print("[CMD] Relay1 ON blocked — lockout active\r\n");
                else if (r1 == 1 && is_volt_fault())
                    Debug_Print("[CMD] Relay1 ON blocked — voltage fault\r\n");
                else
                {
                    char cmd_src[16] = "";
                    extract_str(json, "src", cmd_src, sizeof(cmd_src));
                    bool prev1 = relay1;
                    Relay1_Set(r1 == 1);
                    if (relay1 != prev1) {
                        log_relay_event(1, relay1, cmd_src[0] ? cmd_src : "manual");
                        publish_status();
                    }
                    Debug_Print(r1 ? "[CMD] Relay1 ON\r\n" : "[CMD] Relay1 OFF\r\n");
                }
            }
            return;
        }
        /* Plain URL payload on next line (non-JSON bridge format). */
        {
            char ota_url[200];
            if (extract_ota_url_any(line, ota_url, sizeof(ota_url)))
            {
                recv_payload_pending = false;
                modem_ota_start(ota_url);
                return;
            }
        }
        /* "OK" ends an AT+QMTRECV exchange with no usable payload — clear flag */
        if (strcmp(line, "OK") == 0)
            recv_payload_pending = false;
        /* Other lines (e.g. +QMTRECV header) — keep waiting, fall through */
    }

    if (strstr(line, "+QMTRECV:"))
    {
        char *json = strchr(line, '{');
        if (json)
        {
            /* Settings topic inline: check topic in the +QMTRECV line */
            if (strstr(line, TOPIC_SETTINGS))
            {
                apply_settings(json);
                return;
            }
            if (strstr(line, TOPIC_SETTINGS2))
            {
                apply_settings2(json);
                return;
            }
            /* OTA command: {"url":"https://..."} */
            char ota_url[200];
            if (extract_str(json, "url", ota_url, sizeof(ota_url)))
            {
                if (ota_retry_blocked()) {
                    Debug_Print("[OTA] Ignored trigger (cooldown after reboot)\r\n");
                    return;
                }
                modem_ota_start(ota_url);
                return;
            }
            /* Inline payload — check if pump/02/cmd (relay1→physical relay2) */
            if (strstr(line, TOPIC_CMD2))
            {
                int r = extract_int(json, "relay1");
                if (r >= 0)
                {
                    char cmd_src[16] = "";
                    extract_str(json, "src", cmd_src, sizeof(cmd_src));
                    if (r == 1 && relay1) {
                        Debug_Print("[CMD] Relay2 ON blocked — relay1 active\r\n");
                        return;
                    }
                    if (r == 1 && is_volt_fault()) {
                        Debug_Print("[CMD] Relay2 ON blocked — voltage fault\r\n");
                        return;
                    }
                    if (r == 1 && HAL_GetTick() < lockout_until2) {
                        Debug_Print("[CMD] Relay2 ON blocked — lockout active\r\n");
                        return;
                    }
                    bool prev2 = relay2;
                    Relay2_Set(r == 1);
                    if (relay2 != prev2) {
                        log_relay_event(2, relay2, cmd_src[0] ? cmd_src : "manual");
                        publish_status2();
                    }
                }
            }
            else
            {
                int r1 = extract_int(json, "relay1");
                if (r1 >= 0)
                {
                    if (r1 == 1 && relay2)
                        Debug_Print("[CMD] Relay1 ON blocked — relay2 active\r\n");
                    else if (r1 == 1 && HAL_GetTick() < lockout_until)
                        Debug_Print("[CMD] Relay1 ON blocked — lockout active\r\n");
                    else if (r1 == 1 && is_volt_fault())
                        Debug_Print("[CMD] Relay1 ON blocked — voltage fault\r\n");
                    else
                    {
                        char cmd_src[16] = "";
                        extract_str(json, "src", cmd_src, sizeof(cmd_src));
                        bool prev1 = relay1;
                        Relay1_Set(r1 == 1);
                        if (relay1 != prev1) {
                            log_relay_event(1, relay1, cmd_src[0] ? cmd_src : "manual");
                            publish_status();
                        }
                        Debug_Print(r1 ? "[CMD] Relay1 ON\r\n" : "[CMD] Relay1 OFF\r\n");
                    }
                }
            }
        }
        else
        {
            /* Inline non-JSON payload with URL in the same +QMTRECV line. */
            char ota_url[200];
            if (extract_ota_url_any(line, ota_url, sizeof(ota_url)))
            {
                modem_ota_start(ota_url);
                return;
            }
        }
        if (strchr(line, '"'))
        {
            /* Has quoted topic but no '{': response header from AT+QMTRECV.
             * Payload arrives on the next line — store topic for dispatch.  */
            recv_payload_pending = true;
            /* Extract topic name from: +QMTRECV: 0,0,"pump/01/settings",N  */
            const char *tq = strchr(line, '"');
            if (tq)
            {
                tq++;
                size_t ti = 0;
                while (*tq && *tq != '"' && ti < sizeof(recv_pending_topic) - 1)
                    recv_pending_topic[ti++] = *tq++;
                recv_pending_topic[ti] = '\0';
            }
        }
        else
        {
            /* Buffer-mode notification: "+QMTRECV: 0,<id>" — no topic, no
             * payload.  EC200U has buffered the message; request it now.   */
            const char *p = strchr(line, ',');
            if (p)
            {
                int msgid = atoi(p + 1);
                char recv_cmd[32];
                snprintf(recv_cmd, sizeof(recv_cmd), "AT+QMTRECV=0,%d", msgid);
                modem_cmd(recv_cmd);
                recv_payload_pending = true;
                recv_pending_topic[0] = '\0'; /* topic unknown in buffer-mode */
            }
        }
        return;
    }

    /* +QMTSTAT: server closed the connection.
     * IGNORE in early states (NET_WAIT→SUBSCRIBING) — it is a stale URC from
     * a previous AT+QMTCLOSE that arrived late.  Only act when we have a live
     * MQTT session (CONNECTED / PUBLISHING / PUB_WAIT_OK).                   */
    if (strstr(line, "+QMTSTAT:"))
    {
        if (mqtt_state == MQTT_STATE_CONNECTED ||
            mqtt_state == MQTT_STATE_PUBLISHING ||
            mqtt_state == MQTT_STATE_PUB_WAIT_OK)
        {
            Debug_Print("[MQTT] Disconnected (QMTSTAT)\r\n");
            blink_n(7); /* 7 blinks = server dropped connection unexpectedly */
            pub_pending = false;
            recv_payload_pending = false;
            mqtt_state = MQTT_STATE_DISCONNECTED;
        }
        /* else: ignore — stale URC from previous QMTCLOSE */
        return;
    }

    /* +CSQ: <rssi>,<ber>  — signal quality response */
    if (strstr(line, "+CSQ:"))
    {
        int rssi = 99;
        const char *p = strchr(line, ':');
        if (p)
        {
            p++;
            while (*p == ' ')
                p++;
            rssi = atoi(p);
        }
        if (rssi < 0 || rssi > 31) rssi = 99; /* sanitize */
        last_rssi = (int8_t)rssi;
        return;
    }

    switch (mqtt_state)
    {

    /* ── waiting for EC200U ready after boot ──────────────────────────── */
    case MQTT_STATE_BOOT:
        if (strstr(line, "RDY") || strstr(line, "OK"))
        {
            mqtt_state = MQTT_STATE_NET_WAIT;
            state_entered_ms = HAL_GetTick();
            modem_cmd("ATE0"); /* echo off                  */
            HAL_Delay(200);
            modem_cmd("AT+CMEE=2"); /* verbose errors            */
            HAL_Delay(200);
            modem_cmd("AT+CGREG?"); /* GPRS / 3G registration    */
            HAL_Delay(200);
            modem_cmd("AT+CEREG?"); /* LTE / 4G registration     */
        }
        break;

    /* ── wait for network registration (GPRS or LTE) ─────────────────── */
    case MQTT_STATE_NET_WAIT:
        if (strstr(line, "+CGREG: 0,1") || strstr(line, "+CGREG: 0,5") ||
            strstr(line, "+CEREG: 0,1") || strstr(line, "+CEREG: 0,5"))
        {
            Debug_Print("[NET] Registered\r\n");
            mqtt_state = MQTT_STATE_PDP_OPEN;
            state_entered_ms = HAL_GetTick();
            qicsgp_sent      = false; /* Modem_Process sends QICSGP after buffer drains */
            qiact_retry_done = false; /* reset retry flag for fresh PDP_ACTIVATE attempt */
        }
        /* retry registration every 5s */
        if (HAL_GetTick() - state_entered_ms > 5000)
        {
            Debug_Print("[NET] Waiting for registration...\r\n");
            state_entered_ms = HAL_GetTick();
            modem_cmd("AT+CGREG?");
            HAL_Delay(80);
            modem_cmd("AT+CEREG?");
        }
        break;

    /* ── AT+QICSGP sent — wait for OK then trigger AT+QIACT=1 ────────── */
    case MQTT_STATE_PDP_OPEN:
        if (strstr(line, "OK") && qicsgp_sent)
        {
            Debug_Print("[NET] APN set — activating PDP...\r\n");
            mqtt_state = MQTT_STATE_PDP_ACTIVATE;
            state_entered_ms = HAL_GetTick();
            qiact_sent = false; /* Modem_Process sends QIACT after buffer drains */
        }
        if (strstr(line, "ERROR"))
        {
            Debug_Print("[NET] APN config error — retrying\r\n");
            mqtt_state = MQTT_STATE_NET_WAIT;
            state_entered_ms = HAL_GetTick();
            qicsgp_sent = false;
        }
        break;

    /* ── AT+QIACT=1 sent — wait for OK (up to 30 s) ──────────────────── */
    case MQTT_STATE_PDP_ACTIVATE:
        /* Guard with qiact_sent: stale "OK" from AT+QMTCLOSE / AT+QIDEACT
         * (sent during reconnect with blocking delays) must not be mistaken
         * for QIACT's OK.  SSL is already configured in Modem_Init.        */
        if (strstr(line, "OK") && qiact_sent)
        {
            Debug_Print("[NET] PDP active — opening broker\r\n");
            modem_cmd("AT+QIDNSCFG=1,\"8.8.8.8\",\"8.8.4.4\"");
            HAL_Delay(200);
            qiact_retry_done = false;
            mqtt_state = MQTT_STATE_BROKER_OPEN;
            state_entered_ms = HAL_GetTick();

            char open_cmd[128];
            snprintf(open_cmd, sizeof(open_cmd),
                     "AT+QMTOPEN=0,\"%s\",%s", BROKER_HOST, BROKER_PORT);
            modem_cmd(open_cmd);
        }
        if (strstr(line, "ERROR"))
        {
            if (!qiact_retry_done)
            {
                /* EC200U returns ERROR when AT+QIACT=1 is sent on an already-
                 * active context (e.g., after OTA reboot where QIDEACT in
                 * Modem_Init failed because HTTP was still closing).
                 * Force-deactivate and retry QIACT once before giving up.   */
                qiact_retry_done = true;
                Debug_Print("[NET] QIACT error — force QIDEACT and retry\r\n");
                modem_cmd("AT+QIDEACT=1");
                HAL_Delay(2000);
                HAL_IWDG_Refresh(&hiwdg);
                { uint8_t _c; while (HAL_UART_Receive(modem_uart, &_c, 1, 1) == HAL_OK) {} }
                qiact_sent = false;        /* Modem_Process will send QIACT=1 again */
                state_entered_ms = HAL_GetTick(); /* reset 30 s timeout           */
            }
            else
            {
                /* Retry also failed — give up and restart from NET_WAIT */
                qiact_retry_done = false;
                Debug_Print("[NET] PDP activate failed (retry exhausted) — restarting\r\n");
                mqtt_state = MQTT_STATE_NET_WAIT;
                state_entered_ms = HAL_GetTick();
            }
        }
        /* 30 s timeout — QIACT sometimes silently fails */
        if (HAL_GetTick() - state_entered_ms > 30000)
        {
            Debug_Print("[NET] PDP activate timeout — retrying\r\n");
            mqtt_state = MQTT_STATE_NET_WAIT;
            state_entered_ms = HAL_GetTick();
        }
        break;

    /* ── waiting for +QMTOPEN: 0,0 ───────────────────────────────────── */
    case MQTT_STATE_BROKER_OPEN:
        /* This EC200U firmware sends two lines for a successful open:
         *   +QMTOPEN: 0,"hostname",port  ← TLS handshake started (set flag)
         *   OK                            ← TLS complete, session ready → CONNECT
         * Older FW sends only: +QMTOPEN: 0,0  (also handled below).
         * Do NOT send AT+QMTCONN until the OK arrives — TLS is not done yet. */
        if (strstr(line, "+QMTOPEN: 0,\""))
        {
            /* TCP connected, TLS handshake in progress — NOT ready yet.
             * Stay in BROKER_OPEN and wait for +QMTOPEN: 0,0            */
            Debug_Print("[MQTT] QMTOPEN TLS in progress...\r\n");
            qmtopen_tls_seen = true;
        }
        else if (strcmp(line, "OK") == 0 && qmtopen_tls_seen)
        {
            /* Intermediate OK — TLS still completing. Stay and wait for
             * +QMTOPEN: 0,0 which is the definitive success URC.        */
            Debug_Print("[MQTT] QMTOPEN intermediate OK — awaiting 0,0\r\n");
        }
        else if (strstr(line, "+QMTOPEN: 0,0"))
        {
            /* Definitive success — TLS complete, session ready. */
            qmtopen_tls_seen = false;
            mqtt_state = MQTT_STATE_CONNECTING;
            state_entered_ms = HAL_GetTick();

            /* Force MQTT 3.1.1 right before CONNECT — version may revert to 3
             * (MQTT 3.1) between init and here, which strips credentials.    */
            modem_cmd("AT+QMTCFG=\"version\",0,4");
            HAL_Delay(500); /* wait for OK before sending CONNECT             */

            char conn_cmd[128];
            if (MQTT_USERNAME[0] != '\0')
                snprintf(conn_cmd, sizeof(conn_cmd),
                         "AT+QMTCONN=0,\"%s\",\"%s\",\"%s\"",
                         CLIENT_ID, MQTT_USERNAME, MQTT_PASSWORD);
            else
                snprintf(conn_cmd, sizeof(conn_cmd),
                         "AT+QMTCONN=0,\"%s\"", CLIENT_ID);
            /* DEBUG: print exact CONN command so credentials are visible in serial monitor */
            { char dbg2[160]; snprintf(dbg2, sizeof(dbg2), "[MQTT] >> %s\r\n", conn_cmd); Debug_Print(dbg2); }
            modem_cmd(conn_cmd);
            Debug_Print("[MQTT] Broker open (0,0) — QMTCONN sent\r\n");
        }
        else if (strstr(line, "+QMTOPEN: 0,"))
        {
            qmtopen_tls_seen = false;
            Debug_Print("[MQTT] Broker open FAILED — reconnecting\r\n");
            blink_n(1); /* 1 blink = BROKER_OPEN failed (TLS/TCP error) */
            mqtt_state = MQTT_STATE_DISCONNECTED;
        }
        if (strstr(line, "+QMTCLOSE:") || strstr(line, "+QMTSTAT:"))
        {
            /* Connection closed while waiting — restart */
            qmtopen_tls_seen = false;
            Debug_Print("[MQTT] Connection closed in BROKER_OPEN — reconnecting\r\n");
            mqtt_state = MQTT_STATE_DISCONNECTED;
        }
        if (strstr(line, "ERROR"))
        {
            qmtopen_tls_seen = false;
            Debug_Print("[MQTT] Broker open ERROR — reconnecting\r\n");
            mqtt_state = MQTT_STATE_DISCONNECTED;
        }
        break;

    /* ── waiting for +QMTCONN response ───────────────────────────────── */
    case MQTT_STATE_CONNECTING:
        if (strstr(line, "+QMTCONN:"))
        {
            /* Response formats seen in the wild:
             *   +QMTCONN: 0,0      → success (older FW, no CONNACK code)
             *   +QMTCONN: 0,0,0    → success (CONNACK=0)
             *   +QMTCONN: 0,0,5    → refused (CONNACK=5, bad credentials)
             *   +QMTCONN: 0,1,X    → packet not sent (network issue)  */
            bool conn_ok =
                (strstr(line, "+QMTCONN: 0,0,0") != NULL) ||
                /* "+QMTCONN: 0,0" with no third param (older FW) */
                (strstr(line, "+QMTCONN: 0,0") != NULL &&
                 strstr(line, "+QMTCONN: 0,0,") == NULL);

            if (conn_ok)
            {
                Debug_Print("[MQTT] Connected to HiveMQ Cloud\r\n");
                mqtt_state = MQTT_STATE_SUBSCRIBING;
                state_entered_ms = HAL_GetTick();

                char sub_cmd[64];
                snprintf(sub_cmd, sizeof(sub_cmd),
                         "AT+QMTSUB=0,1,\"%s\",1", TOPIC_CMD);
                modem_cmd(sub_cmd);
            }
            else
            {
                /* Distinguish failure codes by blink count:
                 *  2 blinks = 0,2  transport error (TCP dropped)
                 *  4 blinks = 0,0,4 bad username/password
                 *  5 blinks = 0,0,5 not authorised
                 *  6 blinks = 0,0,1/2/3 protocol/ID/server error
                 *  7 blinks = 0,1  no CONNACK / retransmission      */
                if      (strstr(line, "+QMTCONN: 0,2"))   { Debug_Print("[MQTT] CONN fail: transport error\r\n");   blink_n(2); }
                else if (strstr(line, "+QMTCONN: 0,0,4")) { Debug_Print("[MQTT] CONN fail: bad credentials\r\n");  blink_n(4); }
                else if (strstr(line, "+QMTCONN: 0,0,5")) { Debug_Print("[MQTT] CONN fail: not authorised\r\n");   blink_n(5); }
                else if (strstr(line, "+QMTCONN: 0,1"))   { Debug_Print("[MQTT] CONN fail: no CONNACK\r\n");       blink_n(7); }
                else                                       { Debug_Print("[MQTT] CONN fail: other\r\n");            blink_n(6); }
                mqtt_state = MQTT_STATE_DISCONNECTED;
            }
        }
        if (strstr(line, "ERROR"))
        {
            /* AT layer rejected the command (syntax / state error).
             * 6 fast blinks = CONN ERROR. */
            Debug_Print("[MQTT] CONN command ERROR\r\n");
            blink_n(6);
            mqtt_state = MQTT_STATE_DISCONNECTED;
        }
        break;

    /* ── waiting for +QMTSUB: 0,1,0 then 0,2,0 ──────────────────────── */
    case MQTT_STATE_SUBSCRIBING:
        if (strstr(line, "+QMTSUB: 0,1,0"))
        {
            /* cmd topic subscribed — now subscribe to OTA topic */
            Debug_Print("[MQTT] Subscribed to " TOPIC_CMD " — subscribing OTA...\r\n");
            char sub_cmd[64];
            snprintf(sub_cmd, sizeof(sub_cmd), "AT+QMTSUB=0,2,\"%s\",1", TOPIC_OTA);
            modem_cmd(sub_cmd);
        }
        if (strstr(line, "+QMTSUB: 0,2,0"))
        {
            /* OTA topic subscribed — now subscribe to settings topic */
            Debug_Print("[MQTT] Subscribed to " TOPIC_OTA " — subscribing settings...\r\n");
            char sub_cmd[64];
            snprintf(sub_cmd, sizeof(sub_cmd), "AT+QMTSUB=0,3,\"%s\",1", TOPIC_SETTINGS);
            modem_cmd(sub_cmd);
        }
        if (strstr(line, "+QMTSUB: 0,3,0"))
        {
            /* Settings topic subscribed — subscribe to pump02 cmd topic */
            Debug_Print("[MQTT] Subscribed to " TOPIC_SETTINGS " — subscribing pump02...\r\n");
            char sub_cmd[64];
            snprintf(sub_cmd, sizeof(sub_cmd), "AT+QMTSUB=0,4,\"%s\",1", TOPIC_CMD2);
            modem_cmd(sub_cmd);
        }
        if (strstr(line, "+QMTSUB: 0,4,0"))
        {
            /* pump02/cmd subscribed — now subscribe to pump02/settings */
            Debug_Print("[MQTT] Subscribed to " TOPIC_CMD2 " — subscribing settings2...\r\n");
            char sub_cmd[64];
            snprintf(sub_cmd, sizeof(sub_cmd), "AT+QMTSUB=0,5,\"%s\",1", TOPIC_SETTINGS2);
            modem_cmd(sub_cmd);
        }
        if (strstr(line, "+QMTSUB: 0,5,0"))
        {
            /* pump02/settings subscribed — fully connected now */
            Debug_Print("[MQTT] Subscribed to " TOPIC_SETTINGS2 "\r\n");
            blink_n(3); /* 3 blinks = MQTT fully connected! */
            HAL_IWDG_Refresh(&hiwdg);

            /* Do NOT flush here — the broker delivers any retained message
             * (e.g. pump/01/ota) immediately after subscribe.  A flush
             * loop at 115200 baud has no 1ms gap and swallows the entire
             * QMTRECV line, silently discarding the OTA trigger.
             * Just reset the partial-line buffer; stale "OK" or duplicate
             * +QMTSUB lines in CONNECTED state are harmless.               */
            rxpos = 0;
            HAL_IWDG_Refresh(&hiwdg);

            mqtt_state = MQTT_STATE_CONNECTED;
            /* publish current state immediately so Firebase is up to date on connect */
            prev_relay1       = relay1;
            prev_relay2       = relay2;
            prev_dry_run_trip = dry_run_tripped;
            last_heartbeat_ms = HAL_GetTick();
            modem_cmd("AT+CSQ"); /* seed rssi before first publish */
            publish_status();
            pub_status2_needed = true; /* deferred — queue busy after publish_status() */
        }
        break;

    /* ── fully connected — handle incoming messages ───────────────────── */
    case MQTT_STATE_CONNECTED:
        /* +QMTRECV is handled globally at the top of process_line so it
         * works in any state (PUBLISHING / PUB_WAIT_OK too).  No duplicate
         * handling needed here.                                            */

        /* unsolicited disconnect */
        if (strstr(line, "+QMTSTAT:"))
        {
            Debug_Print("[MQTT] Disconnected (QMTSTAT)\r\n");
            mqtt_state = MQTT_STATE_DISCONNECTED;
        }
        break;

    /* ── waiting for '>' publish prompt ──────────────────────────────── */
    case MQTT_STATE_PUBLISHING:
        /* handled byte-by-byte in Modem_Process for '>' character */
        if (strstr(line, "ERROR"))
        {
            /* ERROR here means QMTPUBEX was rejected — connection is dead */
            Debug_Print("[MQTT] Publish prompt error — reconnecting\r\n");
            pub_pending = false;
            mqtt_state = MQTT_STATE_DISCONNECTED;
        }
        break;

    /* ── waiting for +QMTPUBEX: 0,0,0 ───────────────────────────────── */
    case MQTT_STATE_PUB_WAIT_OK:
        if (strstr(line, "+QMTPUBEX: 0,0,"))  /* any result code */
        {
            pub_pending = false;
            mqtt_state = MQTT_STATE_CONNECTED;
        }
        /* Some FW only sends "OK" for QoS=0 without a +QMTPUBEX URC */
        else if (strcmp(line, "OK") == 0)
        {
            pub_pending = false;
            mqtt_state = MQTT_STATE_CONNECTED;
        }
        if (strstr(line, "ERROR"))
        {
            Debug_Print("[MQTT] Publish failed\r\n");
            pub_pending = false;
            mqtt_state = MQTT_STATE_CONNECTED;
        }
        break;

    /* ── disconnected — will reconnect in Modem_Process ─────────────── */
    /* ── disconnected — will reconnect in Modem_Process ─────────────── */
    case MQTT_STATE_DISCONNECTED:
        break;

    /* ── SSL config — responses handled inline in Modem_Init sequence ── */
    case MQTT_STATE_SSL_CFG:
        break;
    }
}

/* Last non-empty line seen by modem_sync_expect — used in error messages */
static char modem_last_resp[64];

/* ── Synchronous line reader — polls UART until a line containing `expected`
 * is received, or ERROR, or timeout.  Returns true on match.
 * Refreshes IWDG every 1 ms so the watchdog never fires during the wait.
 * Sets modem_last_resp to the last non-empty line received (or "timeout").  */
static bool modem_sync_expect(const char *expected, uint32_t timeout_ms)
{
    char     linebuf[64];
    int      lpos = 0;
    uint32_t t0   = HAL_GetTick();
    modem_last_resp[0] = '\0';
    while (HAL_GetTick() - t0 < timeout_ms) {
        HAL_IWDG_Refresh(&hiwdg);
        uint8_t c;
        if (HAL_UART_Receive(modem_uart, &c, 1, 1) != HAL_OK) continue;
        if (c == '\r') continue;
        if (c == '\n') {
            linebuf[lpos] = '\0';
            if (lpos > 0) {
                strncpy(modem_last_resp, linebuf, sizeof(modem_last_resp) - 1);
                modem_last_resp[sizeof(modem_last_resp) - 1] = '\0';
                if (strstr(linebuf, expected)) return true;
                if (strstr(linebuf, "ERROR"))  return false;
            }
            lpos = 0;
        } else if (lpos < (int)sizeof(linebuf) - 1) {
            linebuf[lpos++] = (char)c;
        }
    }
    strncpy(modem_last_resp, "timeout", sizeof(modem_last_resp) - 1);
    return false; /* timeout */
}

/* Send an AT command and wait for an OK response.
 * Retries are used in Modem_Init because EC200U can still be settling
 * for a short time after CFUN reset during post-OTA reboot. */
static bool modem_sync_cmd_ok(const char *cmd, uint32_t timeout_ms, uint8_t retries)
{
    for (uint8_t i = 0; i < retries; i++) {
        modem_cmd(cmd);
        if (modem_sync_expect("OK", timeout_ms))
            return true;
        HAL_Delay(300);
        HAL_IWDG_Refresh(&hiwdg);
    }
    return false;
}

static bool modem_is_exact_reboot_urc(const char *line)
{
    if (!line || !line[0]) return false;
    if (strcmp(line, "RDY") == 0) return true;
    if (strcmp(line, "+CFUN: 1") == 0) return true;
    return false;
}

static bool modem_extract_host(const char *url, char *host, size_t host_sz)
{
    const char *p = strstr(url, "://");
    const char *start = p ? (p + 3) : url;
    size_t i = 0;
    if (!host || host_sz < 2 || !start || *start == '\0')
        return false;
    while (start[i] &&
           start[i] != '/' &&
           start[i] != ':' &&
           start[i] != '?' &&
           start[i] != '#' &&
           i < (host_sz - 1U))
    {
        host[i] = start[i];
        i++;
    }
    host[i] = '\0';
    return i > 0;
}

/* ── OTA trigger helper — re-applies HTTP config before each download ────── */
static void modem_ota_start(const char *url)
{
    char ota_host[96] = {0};
    bool have_ota_host = modem_extract_host(url, ota_host, sizeof(ota_host));
    /* Central cooldown guard: block OTA retrigger after modem reboot during OTA. */
    if (ota_retry_blocked()) {
        Debug_Print("[OTA] Trigger ignored (cooldown active)\r\n");
        queue_publish(TOPIC_OTA_STATUS,
                      "{\"ota_status\":\"error\",\"reason\":\"ota cooldown active\"}");
        return;
    }
    if (ota_error_msg[0] != '\0') {
        Debug_Print("[OTA] Trigger ignored while OTA result publish is pending\r\n");
        return;
    }

    /* ── PHASE 1: everything done while MQTT is still connected ──────────────
     *
     * Key insight: AT+QHTTPURL only stores the URL in RAM — it does NOT start
     * a TLS handshake or any network activity.  The TLS/TCP connection only
     * happens later during AT+QHTTPGET.
     *
     * By doing SSL config, QHTTPCFG, and QHTTPURL while MQTT is connected, any
     * failure (ERROR / timeout) can be published to Firebase immediately via the
     * live MQTT connection — no reconnect cycle needed.
     *
     * AT+QHTTPGET is the only command that cannot coexist with MQTT TLS
     * (EC200U cannot run two simultaneous TLS sessions), so MQTT disconnect
     * is deferred until just before AT+QHTTPGET.                            */

    /* Publish "starting" — tells bridge.js to clear the retained OTA message
     * on pump/01/ota so the board doesn't re-trigger OTA on every reconnect. */
    /* Publish "starting" even if a status heartbeat is mid-flight.
     * The retained pump/01/ota message often arrives while mqtt_state is
     * PUBLISHING or PUB_WAIT_OK (connection heartbeat queued immediately on
     * CONNECTED).  Wait 700 ms for the in-progress publish to complete, then
     * flush stale modem bytes before sending our own AT+QMTPUBEX.            */
    if (mqtt_state == MQTT_STATE_CONNECTED  ||
        mqtt_state == MQTT_STATE_PUBLISHING ||
        mqtt_state == MQTT_STATE_PUB_WAIT_OK) {
        HAL_Delay(700);                        /* let previous publish finish */
        HAL_IWDG_Refresh(&hiwdg);
        { uint8_t _c; while (HAL_UART_Receive(modem_uart, &_c, 1, 50) == HAL_OK) {} }

        const char *ota_payload = "{\"ota_status\":\"starting\"}";
        char pub_cmd[80];
        snprintf(pub_cmd, sizeof(pub_cmd),
                 "AT+QMTPUBEX=0,0,0,0,\"" TOPIC_OTA_STATUS "\",%d",
                 (int)strlen(ota_payload));
        modem_cmd(pub_cmd);
        uint8_t c;
        uint32_t t0 = HAL_GetTick();
        bool got_prompt = false;
        while (HAL_GetTick() - t0 < 3000) {
            if (HAL_UART_Receive(modem_uart, &c, 1, 5) == HAL_OK && c == '>') {
                got_prompt = true; break;
            }
        }
        if (got_prompt) {
            HAL_UART_Transmit(modem_uart, (uint8_t*)ota_payload,
                              strlen(ota_payload), 1000);
            uint8_t ctrlz = 0x1A;  /* Ctrl-Z commits the MQTT publish */
            HAL_UART_Transmit(modem_uart, &ctrlz, 1, 1000);
            HAL_Delay(500);        /* wait for modem to process publish */
            /* Flush +QMTPUBEX response so it doesn't interfere with SSL cmds */
            { uint8_t _c; while (HAL_UART_Receive(modem_uart, &_c, 1, 50) == HAL_OK) {} }
        }
        HAL_IWDG_Refresh(&hiwdg);
    }

    /* SSL context 1 — used by HTTP client (independent of context 0 = MQTT).
     * Re-applied here because AT+QIDEACT (from a previous reconnect) resets
     * QSSLCFG RAM settings.                                                 */
    modem_cmd("AT+QSSLCFG=\"sslversion\",1,4");
    HAL_Delay(300);
    HAL_IWDG_Refresh(&hiwdg);
    modem_cmd("AT+QSSLCFG=\"ciphersuite\",1,0xFFFF");
    HAL_Delay(300);
    HAL_IWDG_Refresh(&hiwdg);
    modem_cmd("AT+QSSLCFG=\"seclevel\",1,0");
    HAL_Delay(300);
    HAL_IWDG_Refresh(&hiwdg);
    /* EC200U HTTPS servers behind CDN (e.g., GitHub raw) typically require
     * TLS SNI and can reject handshake when modem RTC is stale. */
    if (have_ota_host) {
        char sni_cmd[160];
        snprintf(sni_cmd, sizeof(sni_cmd), "AT+QSSLCFG=\"sni\",1,\"%s\"", ota_host);
        modem_cmd(sni_cmd);
    } else {
        modem_cmd("AT+QSSLCFG=\"sni\",1,1");
    }
    HAL_Delay(300);
    HAL_IWDG_Refresh(&hiwdg);
    modem_cmd("AT+QSSLCFG=\"ignorelocaltime\",1,1");
    HAL_Delay(300);
    HAL_IWDG_Refresh(&hiwdg);
    modem_cmd("AT+QHTTPCFG=\"contextid\",1");
    HAL_Delay(300);
    HAL_IWDG_Refresh(&hiwdg);
    modem_cmd("AT+QHTTPCFG=\"sslctxid\",1");
    HAL_Delay(300);
    HAL_IWDG_Refresh(&hiwdg);
    /* Without "ssl",1 the HTTP client uses plain TCP even for https:// URLs.
     * The TLS handshake never happens and GitHub returns a connection error. */
    modem_cmd("AT+QHTTPCFG=\"ssl\",1");
    HAL_Delay(300);
    HAL_IWDG_Refresh(&hiwdg);
    modem_cmd(OTA_HTTP_REDIRECT_ENABLED ? "AT+QHTTPCFG=\"redirect\",1"
                                        : "AT+QHTTPCFG=\"redirect\",0");
    HAL_Delay(300);
    HAL_IWDG_Refresh(&hiwdg);
    modem_cmd("AT+QHTTPCFG=\"responseheader\",0");
    HAL_Delay(300);
    HAL_IWDG_Refresh(&hiwdg);
    modem_cmd("AT+QHTTPCFG=\"requestheader\",0");
    HAL_Delay(300);
    HAL_IWDG_Refresh(&hiwdg);

    /* Stop any lingering HTTP session and delete any leftover file from a
     * previous failed OTA attempt.  AT+QHTTPGET auto-saves the response to
     * "HTTP_GETFILE" on EC200U UFS (no drive prefix).  Delete it first so
     * stale content cannot be flashed if a previous download was incomplete.
     * Errors from both commands are ignored. */
    modem_cmd("AT+QHTTPSTOP");
    HAL_Delay(500);
    HAL_IWDG_Refresh(&hiwdg);
    modem_cmd("AT+QFDEL=\"HTTP_GETFILE\"");
    HAL_Delay(300);
    HAL_IWDG_Refresh(&hiwdg);
    { uint8_t _c; while (HAL_UART_Receive(modem_uart, &_c, 1, 50) == HAL_OK) {} }
    HAL_IWDG_Refresh(&hiwdg);

    /* Set URL.  Use a 5-second input window so the modem exits URL-input mode
     * before modem_sync_expect (10 s) returns — keeps the modem in a clean
     * command-mode state on failure.                                         */
    char urlcmd[32];
    snprintf(urlcmd, sizeof(urlcmd), "AT+QHTTPURL=%d,5", (int)strlen(url));
    modem_cmd(urlcmd);

    if (!modem_sync_expect("CONNECT", 10000)) {
        /* URL setup failed — MQTT is still connected.  Publish error directly
         * via queue_publish so it appears in Firebase within seconds.
         * No reconnect needed; just return and let the main loop resume.    */
        char errbuf[80];
        snprintf(errbuf, sizeof(errbuf),
                 "{\"ota_status\":\"error\",\"reason\":\"no CONNECT: %s\"}", modem_last_resp);
        pub_pending = false;
        queue_publish(TOPIC_OTA_STATUS, errbuf);
        return;   /* MQTT still connected — normal operation resumes         */
    }

    /* Send URL bytes (no \r\n — modem counts exact bytes) */
    Modem_Send(url);
    HAL_IWDG_Refresh(&hiwdg);

    if (!modem_sync_expect("OK", 10000)) {
        char errbuf[80];
        snprintf(errbuf, sizeof(errbuf),
                 "{\"ota_status\":\"error\",\"reason\":\"URL rejected: %s\"}", modem_last_resp);
        pub_pending = false;
        queue_publish(TOPIC_OTA_STATUS, errbuf);
        return;   /* MQTT still connected                                    */
    }
    HAL_IWDG_Refresh(&hiwdg);

    /* ── PHASE 2: disconnect MQTT, full modem reset, restart download ───────
     *
     * Problem: AT+QFOPEN is silently ignored after a previous failed QFOPEN
     * attempt.  The EC200U's UFS file system mutex gets stuck and the only
     * command that clears it is AT+CFUN=1,1 (full modem software reset).
     * QIDEACT+QIACT frees TLS RAM (fixes 729) but does NOT clear the FS
     * mutex.  CFUN=1,1 fixes both.
     *
     * Since CFUN=1,1 wipes all modem config (SSL, HTTP, URL), we call
     * OTA_Start() instead of OTA_StartFromGet() so the state machine
     * re-applies SSL context and URL from scratch on the clean modem.      */
    modem_cmd("AT+QMTDISC=0");
    HAL_Delay(500);
    HAL_IWDG_Refresh(&hiwdg);
    modem_cmd("AT+QMTCLOSE=0");
    HAL_Delay(300);
    HAL_IWDG_Refresh(&hiwdg);

    Debug_Print("[OTA] CFUN=1,1 — full modem reset to clear UFS/TLS state\r\n");
    modem_cmd("AT+CFUN=1,1");
    modem_sync_expect("RDY", 30000);          /* wait up to 30 s for modem  */
    IWDG->KR = 0xAAAAU;
    for (int _i = 0; _i < 30; _i++) {        /* 15 s: SIM CPIN + network   */
        HAL_Delay(500); IWDG->KR = 0xAAAAU;
    }
    { uint8_t _c; while (HAL_UART_Receive(modem_uart, &_c, 1, 200) == HAL_OK) {} }
    IWDG->KR = 0xAAAAU;

    modem_sync_cmd_ok("ATE0", 2000, 5);       /* disable echo after reset   */

    /* Reactivate PDP context — retry up to 3× in case network not ready   */
    for (int _a = 0; _a < 3; _a++) {
        modem_cmd("AT+QIACT=1");
        if (modem_sync_expect("OK", 15000)) break;
        for (int _i = 0; _i < 6; _i++) {     /* 3 s between retries        */
            HAL_Delay(500); IWDG->KR = 0xAAAAU;
        }
    }
    modem_sync_cmd_ok("AT+QIDNSCFG=1,\"8.8.8.8\",\"8.8.4.4\"", 3000, 2);
    HAL_IWDG_Refresh(&hiwdg);

    /* Re-sync parser and configure HTTP/TLS synchronously after CFUN reset.
     * This avoids async SSL state races right after reboot. */
    if (!modem_sync_cmd_ok("AT", 2000, 8))
        Debug_Print("[OTA] WARN: AT sync before OTA HTTP cfg failed\r\n");
    modem_sync_cmd_ok("ATE0", 2000, 3);
    HAL_Delay(300); IWDG->KR = 0xAAAAU;

    modem_sync_cmd_ok("AT+QSSLCFG=\"sslversion\",1,4", 3000, 3);
    modem_sync_cmd_ok("AT+QSSLCFG=\"ciphersuite\",1,0xFFFF", 3000, 3);
    modem_sync_cmd_ok("AT+QSSLCFG=\"seclevel\",1,0", 3000, 3);
    if (have_ota_host) {
        char sni_cmd2[160];
        snprintf(sni_cmd2, sizeof(sni_cmd2), "AT+QSSLCFG=\"sni\",1,\"%s\"", ota_host);
        modem_sync_cmd_ok(sni_cmd2, 3000, 3);
    } else {
        modem_sync_cmd_ok("AT+QSSLCFG=\"sni\",1,1", 3000, 3);
    }
    modem_sync_cmd_ok("AT+QSSLCFG=\"ignorelocaltime\",1,1", 3000, 3);
    modem_sync_cmd_ok("AT+QHTTPCFG=\"contextid\",1", 3000, 3);
    modem_sync_cmd_ok("AT+QHTTPCFG=\"sslctxid\",1", 3000, 3);
    modem_sync_cmd_ok("AT+QHTTPCFG=\"ssl\",1", 3000, 3);
    modem_sync_cmd_ok(OTA_HTTP_REDIRECT_ENABLED ? "AT+QHTTPCFG=\"redirect\",1"
                                                : "AT+QHTTPCFG=\"redirect\",0",
                     3000, 3);
    modem_sync_cmd_ok("AT+QHTTPCFG=\"responseheader\",0", 3000, 3);
    modem_sync_cmd_ok("AT+QHTTPCFG=\"requestheader\",0", 3000, 3);
    modem_sync_cmd_ok("AT+QHTTPSTOP", 3000, 2);
    modem_cmd("AT+QFDEL=\"HTTP_GETFILE\"");  /* may fail if file absent */
    HAL_Delay(300); IWDG->KR = 0xAAAAU;
    { uint8_t _c; while (HAL_UART_Receive(modem_uart, &_c, 1, 100) == HAL_OK) {} }

    /* Set URL synchronously, then jump OTA state machine to QHTTPGET. */
    {
        char urlcmd2[32];
        snprintf(urlcmd2, sizeof(urlcmd2), "AT+QHTTPURL=%d,5", (int)strlen(url));
        modem_cmd(urlcmd2);
        if (!modem_sync_expect("CONNECT", 10000)) {
            queue_publish(TOPIC_OTA_STATUS,
                          "{\"ota_status\":\"error\",\"reason\":\"no CONNECT (post-cfun)\"}");
            return;
        }
        Modem_Send(url);
        if (!modem_sync_expect("OK", 10000)) {
            queue_publish(TOPIC_OTA_STATUS,
                          "{\"ota_status\":\"error\",\"reason\":\"URL reject (post-cfun)\"}");
            return;
        }
    }

    mqtt_state = MQTT_STATE_DISCONNECTED;
    HAL_IWDG_Refresh(&hiwdg);

    /* URL + HTTP config are already set above; start directly from GET. */
    OTA_StartFromGet(url);
}

/* ── OTA publish callback — lets ota.c publish via MQTT ─────────────────── */
static void modem_ota_publish(const char *topic, const char *payload)
{
    (void)topic;  /* topic always TOPIC_OTA_STATUS — matches ota_error_msg path */

    /* Only save meaningful OTA status updates to ota_error_msg so they
     * survive MQTT disconnect+reconnect.  Skip ota_debug trace lines —
     * they are published for every received AT line during download and
     * would clobber the real error/success status before MQTT reconnects.*/
    if (strstr(payload, "\"ota_status\"")) {
        strncpy(ota_error_msg, payload, sizeof(ota_error_msg) - 1);
        ota_error_msg[sizeof(ota_error_msg) - 1] = '\0';
    }

    if (strstr(payload, "modem rebooted during ota")) {
        ota_retry_block_until = HAL_GetTick() + OTA_RETRY_BLOCK_MS;
        Debug_Print("[OTA] Cooldown armed after modem reboot during OTA\r\n");
    }

    pub_pending = false;
    queue_publish(TOPIC_OTA_STATUS, payload);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Modem_Init  — called once from main.c after UART init
 * ═══════════════════════════════════════════════════════════════════════════ */

void Modem_Init(UART_HandleTypeDef *huart)
{
    modem_uart = huart;
    rxpos = 0;
    mqtt_state = MQTT_STATE_BOOT;
    state_entered_ms = HAL_GetTick();

    /* Restore relay state from Flash before anything else — ensures latching
     * relays are driven to match their last known position on every reboot. */
    RelayState_Load();

    Debug_Print("\r\n=== EC200U MQTT Pump Controller ===\r\n");
    Debug_Print("[MODEM] Pump ID : " PUMP_ID "\r\n");
    Debug_Print("[MODEM] Broker  : " BROKER_HOST "\r\n");
    Debug_Print("[MODEM] Waiting for EC200U ready...\r\n");

    /* Detect software reset (NVIC_SystemReset after OTA) vs cold power-on.
     * RCC_CSR_SFTRSTF is set by NVIC_SystemReset; PORRSTF is set on power-on.
     * Used below to choose the initial MQTT state. */
    uint32_t reset_csr = RCC->CSR;
    bool ota_reboot = ((reset_csr & RCC_CSR_SFTRSTF) != 0) && OTA_WasRebootPending();
    {
        char dbg[96];
        snprintf(dbg, sizeof(dbg), "[MODEM] Reset CSR: 0x%08lX\r\n", (unsigned long)reset_csr);
        Debug_Print(dbg);
    }
    modem_log_reset_flags(reset_csr);
    RCC->CSR |= RCC_CSR_RMVF;   /* clear reset-cause flags for next check */

    if (ota_reboot) {
        /* Check OTA flags to determine if the bootloader applied the update.
         * Bootloader erases the flags page (all bytes → 0xFF) after a
         * successful App B → App A copy.  If the flags magic is still
         * OTA_MAGIC the bootloader CRC check failed and old App A is kept.  */
        volatile uint32_t *flags_magic = (volatile uint32_t*)OTA_FLAGS_ADDR;
        if (*flags_magic == 0xFFFFFFFFUL) {
            /* Erased flags = bootloader ran + copy succeeded → new firmware */
            strncpy(ota_error_msg, "{\"ota_status\":\"success\"}",
                    sizeof(ota_error_msg) - 1);
            ota_error_msg[sizeof(ota_error_msg) - 1] = '\0';
            Debug_Print("[OTA] New firmware running — will publish success\r\n");
        } else {
            /* Flags still present = bootloader CRC check failed, old App A kept */
            strncpy(ota_error_msg, "{\"ota_status\":\"error\",\"reason\":\"crc_fail\"}",
                    sizeof(ota_error_msg) - 1);
            ota_error_msg[sizeof(ota_error_msg) - 1] = '\0';
            Debug_Print("[OTA] Post-OTA: bootloader CRC fail — old firmware kept\r\n");
        }
        /* ota.c already hardware-reset the EC200U (PC14 pulse) before calling
         * NVIC_SystemReset, so the modem is already booting or fully booted.
         * "RDY" URC was likely sent during bootloader execution and is lost —
         * skip waiting for it.  Just flush stale UART bytes and let
         * modem_sync_cmd_ok("AT",...) below handle final sync with retries.  */
        Debug_Print("[MODEM] OTA reboot — modem already reset by ota.c\r\n");
        /* 5 s settle: enough for modem to finish boot if still starting up  */
        for (int i = 0; i < 10; i++) { HAL_Delay(500); IWDG->KR = 0xAAAAU; }
        { uint8_t _c; while (HAL_UART_Receive(modem_uart, &_c, 1, 100) == HAL_OK) {} }
        IWDG->KR = 0xAAAAU;
        Debug_Print("[MODEM] OTA reboot settle complete\r\n");
    } else {
        /* Cold boot — EC200U powers on and takes ~5 s before it accepts AT. */
        for (int i = 0; i < 10; i++) { HAL_Delay(500); IWDG->KR = 0xAAAAU; }
    }
    if (!modem_sync_cmd_ok("AT", 2000, 15))
        Debug_Print("[MODEM] WARN: AT sync failed, continuing\r\n");
    if (!modem_sync_cmd_ok("ATE0", 2000, 5))
        Debug_Print("[MODEM] WARN: ATE0 failed, continuing\r\n");
    /* Lock CEREG URC mode to 0 so AT+CEREG? always returns "+CEREG: 0,<stat>".
     * After an OTA reboot the modem may retain mode=1 or 2 from a previous
     * session, changing the response format and preventing NET_WAIT from
     * recognising the registration status.                                   */
    if (!modem_sync_cmd_ok("AT+CEREG=0", 2000, 5))
        Debug_Print("[MODEM] WARN: CEREG mode set failed, continuing\r\n");

    /* SSL context 1 — used by HTTP client for HTTPS OTA downloads.
     * Must be separate from context 0 (MQTT) so the broker SNI doesn't
     * interfere with GitHub/CDN TLS handshake.                             */
    modem_cmd("AT+QSSLCFG=\"sslversion\",1,4");
    HAL_Delay(300);
    modem_cmd("AT+QSSLCFG=\"ciphersuite\",1,0xFFFF");
    HAL_Delay(300);
    modem_cmd("AT+QSSLCFG=\"seclevel\",1,0");   /* no cert verify for testing */
    HAL_Delay(300);
    /* HTTP client: use SSL context 1, follow GitHub → CDN redirects */
    modem_cmd("AT+QHTTPCFG=\"sslctxid\",1");
    HAL_Delay(300);
    modem_cmd("AT+QHTTPCFG=\"redirect\",1");
    HAL_Delay(300);

    IWDG->KR = 0xAAAAU;   /* ~3 s elapsed since last pet — refresh before long delays */

    /* Cancel any lingering HTTP session (may be left over from a failed OTA).
     * Must be done BEFORE AT+QMTCLOSE so the SSL subsystem is fully released. */
    modem_cmd("AT+QHTTPSTOP");
    HAL_Delay(2000);            /* was 500 ms — after OTA the HTTP session needs
                                 * extra time to fully close before QIDEACT.    */
    IWDG->KR = 0xAAAAU;        /* pet: 2 s elapsed since pet before QHTTPSTOP  */

    /* Close any lingering MQTT session and PDP context FIRST.
     * AT+QMTCLOSE reloads NVM MQTT config — so ALL AT+QMTCFG commands must
     * come AFTER this to avoid being overwritten by the NVM reload.          */
    modem_cmd("AT+QMTCLOSE=0");
    HAL_Delay(1500);
    { uint8_t _c; while (HAL_UART_Receive(modem_uart, &_c, 1, 1) == HAL_OK) {} }
    IWDG->KR = 0xAAAAU;   /* pet after QMTCLOSE — 2 s since last pet */
    modem_cmd("AT+QIDEACT=1");
    /* Wait up to 40 s for the PDP context to fully deactivate (Quectel max spec).
     * modem_sync_expect pets IWDG every 1 ms so the watchdog cannot fire.
     * After OTA the modem may take several seconds to tear down the HTTP/TLS
     * session before it can release the PDP bearer — a fixed 2 s delay was not
     * enough, leaving the context ACTIVE and causing QIACT=1 to return ERROR. */
    modem_sync_expect("OK", 40000);
    { uint8_t _c; while (HAL_UART_Receive(modem_uart, &_c, 1, 1) == HAL_OK) {} }
    IWDG->KR = 0xAAAAU;

    /* Re-apply SSL context 0 AFTER QMTCLOSE+QIDEACT.
     * AT+QMTCLOSE reloads NVM MQTT config which may reset QSSLCFG context 0
     * settings (seclevel, ciphersuite, SNI) back to factory defaults.
     * Placing these here guarantees they are set correctly regardless of any
     * NVM reload, and they survive until AT+QMTOPEN is called.              */
    modem_cmd("AT+QSSLCFG=\"sslversion\",0,4");
    HAL_Delay(300);
    modem_cmd("AT+QSSLCFG=\"ciphersuite\",0,0xFFFF");
    HAL_Delay(300);
    modem_cmd("AT+QSSLCFG=\"seclevel\",0,0");   /* no cert verify — EMQX self-signed */
    HAL_Delay(300);
    modem_cmd("AT+QSSLCFG=\"sni\",0,\"" BROKER_HOST "\"");
    HAL_Delay(300);
    IWDG->KR = 0xAAAAU;   /* pet: 4×300 ms = 1.2 s since last pet              */

    /* Configure MQTT client AFTER QMTCLOSE so NVM reload can't undo these.  */
    modem_cmd("AT+QMTCFG=\"ssl\",0,1,0");       /* MQTT client 0 → SSL context 0  */
    HAL_Delay(300);
    /* EC200U NVM default is MQTT 3.1 (version=3) which omits credentials from
     * the CONNECT packet — broker sees anonymous → CONNACK=5.
     * Must explicitly set version=4 (MQTT 3.1.1) AFTER every QMTCLOSE.     */
    modem_cmd("AT+QMTCFG=\"version\",0,4");     /* MQTT 3.1.1 — sends credentials */
    HAL_Delay(300);
    modem_cmd("AT+QMTCFG=\"pdpcid\",0,1");      /* bind to PDP context 1          */
    HAL_Delay(300);
    modem_cmd("AT+QMTCFG=\"keepalive\",0,300"); /* keepalive 300 s — survives OTA */
    HAL_Delay(300);
    modem_cmd("AT+QMTCFG=\"session\",0,1");     /* clean session                  */
    HAL_Delay(300);
    modem_cmd("AT+QMTCFG=\"will\",0,0");        /* disable Will — no stale topic  */
    HAL_Delay(300);
    modem_cmd("AT+QMTCFG=\"recv/mode\",0,1,1"); /* direct push + full payload URC */
    HAL_Delay(300);

    /* Wire up OTA send/publish callbacks so ota.c can issue AT commands
     * and publish MQTT status without a circular header dependency.         */
    OTA_Init();
    OTA_SetSendFn(Modem_Send);
    OTA_SetPublishFn(modem_ota_publish);

    /* PDP is now deactivated (modem_sync_expect confirmed OK above).
     * Use the same NET_WAIT path for both cold boot and OTA reboot.
     * NET_WAIT polls CEREG until LTE is registered, then NET_WAIT→PDP_OPEN
     * sends QICSGP to configure the APN — QIACT=1 requires a valid APN
     * context configuration even after a previous QIDEACT.              */
    if (ota_reboot)
        Debug_Print("[MODEM] OTA reboot — PDP deactivated, going to NET_WAIT\r\n");
    mqtt_state       = MQTT_STATE_NET_WAIT;
    state_entered_ms = HAL_GetTick();
    /* Suppress modem_reinit for 20 s after init — late EC200U boot URCs
     * (APP RDY, +CFUN: 1) that arrive during the config phase must not
     * re-trigger a full Modem_Init unnecessarily.                       */
    modem_init_grace_until = HAL_GetTick() + 20000UL;
    modem_cmd("AT+CEREG?");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Modem_Process  — called every loop iteration from main.c
 * ═══════════════════════════════════════════════════════════════════════════ */

void Modem_Process(void)
{
    if (!modem_uart)
        return;

    /* Keep feeding watchdog even when no UART data arrives. */
    HAL_IWDG_Refresh(&hiwdg);

    /* ── read bytes from EC200U into line buffer ── */
    /* Direct hardware register read — same approach as Modbus RX.
     * Bypasses HAL lock/unlock overhead (~6 µs per call) so the main loop
     * runs at <1 µs/iteration, fast enough to read USART2 Modbus bytes
     * (one every 1040 µs at 9600 baud) without overrun.                  */
    uint8_t c;
    static uint8_t ota_connect_match = 0;
    static bool ota_skip_to_lf = false;
    static uint16_t ota_rx_pet = 0;
    for (;;) {
        uint32_t isr1 = modem_uart->Instance->ISR;
        if (isr1 & (USART_ISR_ORE | USART_ISR_FE | USART_ISR_NE)) {
            modem_uart->Instance->ICR = USART_ICR_ORECF | USART_ICR_FECF | USART_ICR_NECF;
            modem_uart->ErrorCode = HAL_UART_ERROR_NONE;
        }
        if (!(isr1 & USART_ISR_RXNE_RXFNE)) break;
        c = (uint8_t)(modem_uart->Instance->RDR & 0xFF);
        if (++ota_rx_pet >= 64U) { HAL_IWDG_Refresh(&hiwdg); ota_rx_pet = 0; }

        /* Detect prompt-style CONNECT from AT+QHTTPREAD that may arrive without
         * a clean line break. */
        if (OTA_ExpectingHttpReadConnect()) {
            static const char token[] = "CONNECT";
            if (c == (uint8_t)token[ota_connect_match]) {
                ota_connect_match++;
                if (ota_connect_match >= (sizeof(token) - 1U)) {
                    ota_connect_match = 0;
                    ota_skip_to_lf = true;
                    OTA_HandleLine("CONNECT");
                }
                continue;
            }
            ota_connect_match = (c == (uint8_t)token[0]) ? 1U : 0U;
            /* While waiting for CONNECT, keep bytes isolated from line parser. */
            continue;
        } else {
            ota_connect_match = 0;
        }

        /* Drop trailing CONNECT line ending before passing binary bytes to OTA. */
        if (ota_skip_to_lf) {
            if (c == '\n') {
                ota_skip_to_lf = false;
            }
            continue;
        }

        /* OTA binary passthrough — raw bytes from AT+QFREAD/QHTTPREAD */
        if (OTA_BinaryPending()) {
            OTA_FeedByte(c);
            continue;
        }

        if (c == '\r')
            continue;

        /* '>' is the publish prompt — not followed by '\n', catch it here */
                if (c == '>')
        {
            if (mqtt_state == MQTT_STATE_PUBLISHING && pub_pending)
            {
                /* Send payload immediately � any blocking delay here risks
                 * the modem aborting the input window.                     */
                HAL_UART_Transmit(modem_uart,
                                  (uint8_t *)pub_payload, strlen(pub_payload), 3000);
                /* Ctrl-Z commits the message */
                uint8_t ctrlz = 0x1A;
                HAL_UART_Transmit(modem_uart, &ctrlz, 1, 1000);
                mqtt_state = MQTT_STATE_PUB_WAIT_OK;
                state_entered_ms = HAL_GetTick();
            }
            rxpos = 0;
            continue;
        }

        if (c == '\n')
        {
            rxbuf[rxpos] = '\0';
            if (rxpos > 0)
                process_line(rxbuf);
            rxpos = 0;
        }
        else
        {
            if (rxpos < RX_BUF_SIZE - 1)
                rxbuf[rxpos++] = (char)c;
            else
                rxpos = 0; /* overflow guard */
        }
    }

    /* Full modem reset recovery path: re-run Modem_Init so SSL/MQTT config
     * is restored before attempting any reconnect. */
    if (modem_reinit_pending && !OTA_IsActive()) {
        modem_reinit_pending   = false;
        recv_payload_pending   = false;
        pub_pending            = false;
        pub_status2_needed     = false;
        disconnected_since_ms  = HAL_GetTick(); /* restart stuck timer after reinit */
        OTA_Init();
        Modem_Init(modem_uart);
        return;
    }

    /* Hardware stuck-detection: if not CONNECTED for 5 minutes, pulse PC14
     * (EC200U RESET) to force a hardware reset of the modem.  Software
     * reinit alone cannot recover a modem that has internally crashed.
     * Not triggered during OTA or within first 5 min of boot. */
    if (mqtt_state != MQTT_STATE_CONNECTED && !OTA_IsActive()) {
        if (disconnected_since_ms == 0)
            disconnected_since_ms = HAL_GetTick();

        if ((int32_t)(HAL_GetTick() - disconnected_since_ms) >
            (int32_t)MODEM_HARD_RESET_TIMEOUT_MS)
        {
            Debug_Print("[MODEM] 5 min stuck — hardware reset EC200U via PC14\r\n");
            disconnected_since_ms = HAL_GetTick(); /* reset timer — don't rapid-fire */
            recv_payload_pending  = false;
            pub_pending           = false;
            pub_status2_needed    = false;

            /* Pulse RESET LOW for 300 ms — EC200U hardware reset */
            HAL_GPIO_WritePin(MODEM_RESET_GPIO_Port, MODEM_RESET_Pin, GPIO_PIN_RESET);
            HAL_Delay(300);
            HAL_GPIO_WritePin(MODEM_RESET_GPIO_Port, MODEM_RESET_Pin, GPIO_PIN_SET);
            HAL_Delay(3000); /* wait for modem to boot (EC200U takes ~3 s) */

            /* Re-run full init — SSL/MQTT config must be re-applied after reset */
            OTA_Init();
            Modem_Init(modem_uart);
            return;
        }
    } else {
        disconnected_since_ms = 0; /* connected — clear stuck timer */
    }

    /* ── send AT+QIACT=1 after buffer is drained (avoids stale QMTCLOSE/QIDEACT OK) ── */
    if (mqtt_state == MQTT_STATE_PDP_ACTIVATE && !qiact_sent)
    {
        qiact_sent = true;
        modem_cmd("AT+QIACT=1");
    }

    /* ── send AT+QICSGP after buffer is drained (avoids spurious CGREG OK) ── */
    if (mqtt_state == MQTT_STATE_PDP_OPEN && !qicsgp_sent)
    {
        qicsgp_sent = true;
        char apn_cmd[80];
        snprintf(apn_cmd, sizeof(apn_cmd),
                 "AT+QICSGP=1,1,\"%s\",\"\",\"\",0", SIM_APN);
        modem_cmd(apn_cmd);
    }

    /* ── BOOT heartbeat: keep probing modem until first RDY/OK is seen ── */
    if (mqtt_state == MQTT_STATE_BOOT &&
        (HAL_GetTick() - state_entered_ms) > 3000U)
    {
        Debug_Print("[MODEM] BOOT probe: AT\r\n");
        modem_cmd("AT");
        state_entered_ms = HAL_GetTick();
    }

    /* ── BROKER_OPEN / CONNECTING / SUBSCRIBING timeouts → reconnect ── */
    if ((mqtt_state == MQTT_STATE_BROKER_OPEN ||
         mqtt_state == MQTT_STATE_CONNECTING  ||
         mqtt_state == MQTT_STATE_SUBSCRIBING) &&
        HAL_GetTick() - state_entered_ms > 30000)
    {
        Debug_Print("[MQTT] Connection timeout — reconnecting\r\n");
        if (mqtt_state == MQTT_STATE_BROKER_OPEN) blink_n(1); /* 1 blink = BROKER_OPEN timed out */
        mqtt_state = MQTT_STATE_DISCONNECTED;
    }

    /* ── PUBLISHING timeout — no '>' within 10s means connection is dead ── */
    if (mqtt_state == MQTT_STATE_PUBLISHING &&
        HAL_GetTick() - state_entered_ms > 10000)
    {
        Debug_Print("[MQTT] Publish timeout — reconnecting\r\n");
        pub_pending = false;
        mqtt_state = MQTT_STATE_DISCONNECTED;
    }

    /* ── PUB_WAIT_OK timeout — if +QMTPUBEX never arrives, back to CONNECTED ── */
    if (mqtt_state == MQTT_STATE_PUB_WAIT_OK &&
        HAL_GetTick() - state_entered_ms > MQTT_PUBACK_TIMEOUT_MS)
    {
        Debug_Print("[MQTT] PubWaitOK timeout — continuing\r\n");
        pub_pending = false;
        mqtt_state = MQTT_STATE_CONNECTED;
    }

    /* ── periodic tasks (only when fully connected) ── */
    if (mqtt_state == MQTT_STATE_CONNECTED && !OTA_IsActive())
    {

        /* Deferred pump02 status — send as soon as the queue is free */
        if (pub_status2_needed && !pub_pending)
        {
            pub_status2_needed = false;
            publish_status2();
        }

        /* publish on state change — relay or dry-run trip changed */
        if (!pub_pending &&
            (relay1 != prev_relay1 || relay2 != prev_relay2 ||
             dry_run_tripped != prev_dry_run_trip))
        {
            bool relay2_changed = (relay2 != prev_relay2);
            prev_relay1       = relay1;
            prev_relay2       = relay2;
            prev_dry_run_trip = dry_run_tripped;
            publish_status();
            if (relay2_changed) pub_status2_needed = true; /* deferred — queue busy */
        }

        /* heartbeat every 60 s — keeps Firebase online:true fresh */
        if (HAL_GetTick() - last_heartbeat_ms >= HEARTBEAT_INTERVAL_MS)
        {
            last_heartbeat_ms = HAL_GetTick();
            modem_cmd("AT+CSQ"); /* refresh signal strength; +CSQ updates last_rssi */
            publish_status();
            pub_status2_needed = true; /* deferred — queue busy after publish_status() */
        }

        /* run protection every 1 s */
        static uint32_t last_prot_ms = 0;
        if (HAL_GetTick() - last_prot_ms >= 1000)
        {
            last_prot_ms = HAL_GetTick();
            run_protection();
        }
    }

    /* ── send OTA error (saved across reconnect) as soon as MQTT is up ── */
    if (ota_error_msg[0] && mqtt_state == MQTT_STATE_CONNECTED)
    {
        pub_pending = false; /* prioritize OTA result over queued heartbeat */
        queue_publish(TOPIC_OTA_STATUS, ota_error_msg);
        ota_error_msg[0] = '\0';
        force_status_after_ota = true;
    }

    if (force_status_after_ota &&
        mqtt_state == MQTT_STATE_CONNECTED &&
        !OTA_IsActive() &&
        !pub_pending)
    {
        modem_cmd("AT+CSQ");
        publish_status();
        force_status_after_ota = false;
    }

    /* Feed once more after draining RX loop. */
    HAL_IWDG_Refresh(&hiwdg);

    /* ── send queued publish when connected and idle (not during OTA) ── */
    if (pub_pending && mqtt_state == MQTT_STATE_CONNECTED && !OTA_IsActive())
    {
        char pub_cmd[128];
        snprintf(pub_cmd, sizeof(pub_cmd),
                 "AT+QMTPUBEX=0,0,0,0,\"%s\",%d",  /* QoS=0: msgID must be 0 */
                 pub_topic, (int)strlen(pub_payload));
        modem_cmd(pub_cmd);
        mqtt_state = MQTT_STATE_PUBLISHING;
        state_entered_ms = HAL_GetTick();
    }

    /* ── auto-reconnect (not during OTA — would disrupt HTTP download) ── */
    if (mqtt_state == MQTT_STATE_DISCONNECTED && !OTA_IsActive())
    {
        static uint32_t last_reconnect = 0;
        if (HAL_GetTick() - last_reconnect > 10000)
        {
            last_reconnect = HAL_GetTick();
            state_entered_ms = HAL_GetTick();
            /* Refresh IWDG immediately — blink_n(5/6/7) may have consumed up to
             * 3800ms before setting DISCONNECTED, leaving < 200ms until timeout. */
            HAL_IWDG_Refresh(&hiwdg);
            qmtopen_tls_seen = false;
            Debug_Print("[MQTT] Reconnecting...\r\n");
            /* Stop any active HTTP session BEFORE closing MQTT and deactivating
             * the PDP bearer.  After a failed OTA, the HTTP context may still be
             * "open" (QHTTPCFG configured, partial QHTTPURL).  AT+QIDEACT fails
             * silently when an HTTP session is active, leaving the PDP in a bad
             * state and preventing MQTT from ever reconnecting.
             * Modem_Init already does this; we must mirror it here.            */
            modem_cmd("AT+QHTTPSTOP");
            HAL_Delay(500);
            HAL_IWDG_Refresh(&hiwdg);
            modem_cmd("AT+QMTCLOSE=0");
            HAL_Delay(1500);
            HAL_IWDG_Refresh(&hiwdg);
            /* Flush URCs from QMTCLOSE (TCP teardown can take several seconds).
             * Refresh IWDG every 500 ms so a chatty modem can't trigger a reset. */
            {
                uint8_t _c; uint32_t _t = HAL_GetTick();
                while (HAL_UART_Receive(modem_uart, &_c, 1, 1) == HAL_OK) {
                    if (HAL_GetTick() - _t >= 500) { HAL_IWDG_Refresh(&hiwdg); _t = HAL_GetTick(); }
                }
                rxpos = 0;
            }
            /* Re-apply after QMTCLOSE — QMTCLOSE reloads NVM which resets version
             * to default=3 (MQTT 3.1) which strips credentials from CONNECT.  */
            modem_cmd("AT+QMTCFG=\"version\",0,4");  /* keep MQTT 3.1.1        */
            HAL_Delay(300);
            modem_cmd("AT+QMTCFG=\"will\",0,0");
            HAL_Delay(300);
            /* QMTCLOSE reloads NVM which resets ssl to factory default (0=disabled).
             * Without this, QMTOPEN uses plain TCP on port 8883 (TLS expected) and
             * fails silently every reconnect cycle — confirmed root cause of
             * post-OTA MQTT never connecting.                                   */
            modem_cmd("AT+QMTCFG=\"ssl\",0,1,0"); /* MQTT ctx 0 → SSL ctx 0    */
            HAL_Delay(300);
            HAL_IWDG_Refresh(&hiwdg);
            modem_cmd("AT+QIDEACT=1");
            HAL_Delay(1500);
            HAL_IWDG_Refresh(&hiwdg);
            /* Flush URCs from QIDEACT (PDP deactivation URCs may arrive late). */
            {
                uint8_t _c; uint32_t _t = HAL_GetTick();
                while (HAL_UART_Receive(modem_uart, &_c, 1, 1) == HAL_OK) {
                    if (HAL_GetTick() - _t >= 500) { HAL_IWDG_Refresh(&hiwdg); _t = HAL_GetTick(); }
                }
                rxpos = 0;
            }
            recv_payload_pending = false; /* clear stale recv state           */
            pub_pending          = false; /* drop unsent publish              */
            /* Re-apply SSL context 0 after QMTCLOSE — NVM reload may have
             * reset seclevel/ciphersuite/SNI back to factory defaults,
             * causing TLS handshake to fail when QMTOPEN is sent.          */
            modem_cmd("AT+QSSLCFG=\"sslversion\",0,4");
            HAL_Delay(300);
            HAL_IWDG_Refresh(&hiwdg);
            modem_cmd("AT+QSSLCFG=\"ciphersuite\",0,0xFFFF");
            HAL_Delay(300);
            HAL_IWDG_Refresh(&hiwdg);
            modem_cmd("AT+QSSLCFG=\"seclevel\",0,0");
            HAL_Delay(300);
            HAL_IWDG_Refresh(&hiwdg);
            modem_cmd("AT+QSSLCFG=\"sni\",0,\"" BROKER_HOST "\"");
            HAL_Delay(300);
            HAL_IWDG_Refresh(&hiwdg);
            /* APN already configured — skip QICSGP, go straight to QIACT.
             * Modem_Process sends AT+QIACT=1 after the buffer is drained
             * so stale OK from QMTCLOSE/QIDEACT above is not misread.      */
            mqtt_state       = MQTT_STATE_PDP_ACTIVATE;
            qiact_sent       = false;
            qiact_retry_done = false; /* fresh attempt — PDP just deactivated */
        }
    }

    /* ── net registration timeout — retry both GPRS and LTE ── */
    if (mqtt_state == MQTT_STATE_NET_WAIT &&
        HAL_GetTick() - state_entered_ms > 5000)
    {
        state_entered_ms = HAL_GetTick();
        modem_cmd("AT+CGREG?");
        HAL_Delay(200);
        modem_cmd("AT+CEREG?");
    }
}


