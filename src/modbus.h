/* modbus.h
 * Non-blocking Modbus RTU master for Selec EM4M-3P energy meter.
 * Uses USART2 (PA2=TX, PA3=RX) + PA8 for DE/RE direction control.
 *
 * Poll interval: 2 seconds.
 * Registers: FC04 request 1 — V1N/V2N/V3N, I1/I2/I3 (start 0x0000, count 22)
 *            FC04 request 2 — Total kW (start 0x002A, count 2); issued after request 1.
 */
#pragma once
#include "stm32g0xx_hal.h"
#include <stdbool.h>
#include <stdint.h>

void  Modbus_Init(UART_HandleTypeDef *huart);
void  Modbus_Process(void);

/* Latest readings — return safe defaults until first successful read */
float   Modbus_GetV1(void);         /* Phase 1 voltage L-N  (Volts) */
float   Modbus_GetV2(void);         /* Phase 2 voltage L-N  (Volts) */
float   Modbus_GetV3(void);         /* Phase 3 voltage L-N  (Volts) */
float   Modbus_GetI1(void);         /* Phase 1 current       (Amps)  */
float   Modbus_GetI2(void);         /* Phase 2 current       (Amps)  */
float   Modbus_GetI3(void);         /* Phase 3 current       (Amps)  */
float   Modbus_GetPF1(void);        /* Phase 1 power factor  (-1..1) */
float   Modbus_GetPF2(void);        /* Phase 2 power factor  (-1..1) */
float   Modbus_GetPF3(void);        /* Phase 3 power factor  (-1..1) */
float   Modbus_GetKW(void);         /* Total active power     (kW)   */
bool    Modbus_IsDataValid(void);   /* true if last CRC passed       */
uint8_t Modbus_GetLastRx(void);     /* bytes received in last tx/rx  */
