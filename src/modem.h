#ifndef MODEM_H
#define MODEM_H

#include "stm32g0xx_hal.h"
#include <stdbool.h>
#include <stdint.h>

/* ── Device identity — change these two lines per device before flashing ─── */
#define PUMP_ID  "03"   /* relay1 pump ID: "01" for pump01/02, "03" for pump03/04 */
#define PUMP_ID2 "04"   /* relay2 pump ID: always PUMP_ID + 1                     */

/* ── Public API ──────────────────────────────────────────────────────────── */
void  Modem_Init(UART_HandleTypeDef *huart);
void  Modem_Process(void);              /* call every loop iteration        */
void  Modem_Send(const char *cmd);      /* send raw AT command              */
bool  Modem_IsConnected(void);          /* true only when MQTT CONNECTED    */

/* ── Network RTC ─────────────────────────────────────────────────────────── */
uint64_t Modem_GetUnixMs(void);         /* ms since Unix epoch, 0 if not synced */

/* ── Relay setters called from MQTT command handler ─────────────────────── */
void  Relay1_Set(bool on);
void  Relay2_Set(bool on);
bool  Relay1_Get(void);
bool  Relay2_Get(void);

/* ── Sensor readings — implement in sensors.c ───────────────────────────── */
float Sensor_ReadVoltagePhase1(void);
float Sensor_ReadVoltagePhase2(void);
float Sensor_ReadVoltagePhase3(void);
float Sensor_ReadCurrentACS712(void);

#endif /* MODEM_H */
