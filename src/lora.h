#pragma once
#include "stm32g0xx_hal.h"
#include <stdbool.h>

void LoRa_Init(UART_HandleTypeDef *huart);
void LoRa_Process(void);
void LoRa_SendRelay(int relay_num, bool on);  /* 1=pump03(P1), 2=pump04(P2) */
void LoRa_SendRaw(const char *msg);           /* send arbitrary msg to slave  */

/* Returns last known state from slave heartbeat/ack: 1=ON, 0=OFF, -1=unknown */
int      LoRa_GetRelay3State(void);
int      LoRa_GetRelay4State(void);
/* Signal quality of last received +RCV frame */
int      LoRa_GetLastRSSI(void);
int      LoRa_GetLastSNR(void);
/* Milliseconds since last +RCV received; 0xFFFFFFFF if never */
uint32_t LoRa_GetLastRcvAge(void);
