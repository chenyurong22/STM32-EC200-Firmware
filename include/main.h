/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32g0xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);
#if defined(FW_MIN_LOGS)
#define Debug_Print(...) ((void)0)
#else
void Debug_Print(const char *msg);  /* send string to USART2 serial monitor */
#endif

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
/* Latching relay via transistor driver (HIGH = transistor ON = coil energised):
 * PA1 → transistor → Pump1 SET  coil (pulse HIGH 200ms → relay latches ON)
 * PB3 → transistor → Pump1 RESET coil (pulse HIGH 200ms → relay latches OFF)
 * PB4 → transistor → Pump2 SET  coil (pulse HIGH 200ms → relay latches ON)
 * PB5 → transistor → Pump2 RESET coil (pulse HIGH 200ms → relay latches OFF)
 * (PB0 free; PB8/PB9 reserved for future LoRa) */
#define Relay_Pin_Pin        GPIO_PIN_1   /* Pump 1 SET   coil (PA1) */
#define Relay_Pin_GPIO_Port  GPIOA
#define Relay1_RST_Pin       GPIO_PIN_3   /* Pump 1 RESET coil (PB3) */
#define Relay1_RST_GPIO_Port GPIOB
#define Relay2_Pin_Pin       GPIO_PIN_4   /* Pump 2 SET   coil (PB4) */
#define Relay2_Pin_GPIO_Port GPIOB
#define Relay2_RST_Pin       GPIO_PIN_5   /* Pump 2 RESET coil (PB5) */
#define Relay2_RST_GPIO_Port GPIOB
#define DE485_Pin_Pin        GPIO_PIN_8   /* RS485 DE/RE direction control */
#define DE485_Pin_GPIO_Port  GPIOA
#define MODEM_RESET_Pin      GPIO_PIN_14  /* PC14 → EC200U RESET  (active LOW, pulse ≥100ms) */
#define MODEM_RESET_GPIO_Port GPIOC
#define MODEM_PWRKEY_Pin     GPIO_PIN_15  /* PC15 → EC200U PWRKEY (active LOW, hold ≥500ms)  */
#define MODEM_PWRKEY_GPIO_Port GPIOC

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
