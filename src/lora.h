#pragma once
#include "stm32g0xx_hal.h"
#include <stdbool.h>

void LoRa_Init(UART_HandleTypeDef *huart);
void LoRa_Process(void);
void LoRa_SendRelay(int relay_num, bool on);  /* 1=pump03(P1), 2=pump04(P2) */
