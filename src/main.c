/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
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
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "modem.h"
#include "modbus.h"
#include "ota.h"
#include "lora.h"
#include "lora_ota.h"
#include <string.h>
#include <stdio.h>

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#define FW_VER "2026-06-01a"
const char g_fw_ver[] = FW_VER;
extern volatile uint32_t g_reset_reason_magic;
extern volatile uint32_t g_hf_pc;
extern volatile uint32_t g_hf_lr;
extern volatile uint32_t g_hf_psr;

/* Compatibility shim for snapshots where fault markers are not linked */
volatile uint32_t g_reset_reason_magic __attribute__((weak));
volatile uint32_t g_hf_pc __attribute__((weak));
volatile uint32_t g_hf_lr __attribute__((weak));
volatile uint32_t g_hf_psr __attribute__((weak));

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */
#define WDG_KICK() do { IWDG->KR = 0xAAAAU; } while (0)

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
UART_HandleTypeDef huart1;  /* EC200U modem (PB6=TX, PB7=RX)              */
UART_HandleTypeDef huart2;  /* Modbus RS485 (PA2=TX/DI, PA3=RX/RO, 9600) */
UART_HandleTypeDef huart3;  /* Reyax RYL998 LoRa (PB8=TX, PB9=RX, 115200)*/
IWDG_HandleTypeDef hiwdg;   /* Independent watchdog (~4s timeout) */
/* ADC1 handle — used by sensors_adc.c when real ADC sensing is enabled.
 * Requires HAL_ADC_MODULE_ENABLED in hal_conf.h.                           */
#ifdef HAL_ADC_MODULE_ENABLED
ADC_HandleTypeDef hadc1;
#endif

/* USER CODE BEGIN PV */
volatile uint8_t g_boot_phase = 0;
static uint32_t g_hb_last_ms = 0;
static GPIO_PinState g_hb_state = GPIO_PIN_RESET;

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_USART1_UART_Init(void);
static void MX_USART2_UART_Init(void);
static void MX_USART3_UART_Init(void);
static void MX_IWDG_Init(void);
#ifdef HAL_ADC_MODULE_ENABLED
static void MX_ADC1_Init(void);
#endif
/* USER CODE BEGIN . */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */
  /* App starts at 0x08002000 (not flash base) — set VTOR before any interrupt */
  SCB->VTOR = 0x08002000;
  /* Bootloader disabled IRQs before jumping here; re-enable so SysTick works */
  __enable_irq();
  g_boot_phase = 1;
  WDG_KICK();
  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();
  g_boot_phase = 2;
  WDG_KICK();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();
  g_boot_phase = 3;
  WDG_KICK();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  g_boot_phase = 4;
  WDG_KICK();

  MX_USART2_UART_Init();  /* Modbus RS485 at 9600 baud */
  g_boot_phase = 5;
  WDG_KICK();

  MX_USART1_UART_Init();  /* EC200U modem at 115200 baud */
  g_boot_phase = 6;
  WDG_KICK();

  MX_USART3_UART_Init();
  LoRa_Init(&huart3);
  g_boot_phase = 7;
  WDG_KICK();

  Debug_Print("[FW] Version: " FW_VER "\r\n");
  /* Boot state: all coil pins LOW — no pulse, latching relays hold last position */
  HAL_GPIO_WritePin(Relay_Pin_GPIO_Port,  Relay_Pin_Pin,  GPIO_PIN_RESET); /* PA1 LOW — pump1 SET   coil idle */
  HAL_GPIO_WritePin(Relay1_RST_GPIO_Port, Relay1_RST_Pin, GPIO_PIN_RESET); /* PB3 LOW — pump1 RESET coil idle */
  HAL_GPIO_WritePin(Relay2_Pin_GPIO_Port, Relay2_Pin_Pin, GPIO_PIN_RESET); /* PB4 LOW — pump2 SET   coil idle */
  HAL_GPIO_WritePin(Relay2_RST_GPIO_Port, Relay2_RST_Pin, GPIO_PIN_RESET); /* PB5 LOW — pump2 RESET coil idle */

  /* Initialize ADC1 for voltage/current sensing (step 4 — real ADC).
   * Only compiled when HAL_ADC_MODULE_ENABLED is uncommented in hal_conf.h. */
#ifdef HAL_ADC_MODULE_ENABLED
  MX_ADC1_Init();
#endif
  g_boot_phase = 8;

  /* Initialize Modbus RS485 master (USART2, PA8=DE/RE) */
  Modbus_Init(&huart2);

  /* Initialize modem attached to USART1 (PB6=TX, PB7=RX) */
  Modem_Init(&huart1);
  g_boot_phase = 9;
  WDG_KICK();

  /* Enable IWDG so all refresh points in modem/ota are valid and consistent. */
  MX_IWDG_Init();
  Debug_Print("[WDT] Enabled (32s timeout)\r\n");
  g_boot_phase = 10;
  /* USER CODE BEGIN 2 */

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    g_boot_phase = 11;
    Modem_Process();
    LoRa_Process();
    if (!OTA_IsActive()) {
      Modbus_Process();
    }
    OTA_Process();
    LoRaOta_Process();
    /* Diagnostic heartbeat: toggle PA8 (DE485) every 500ms to confirm MCU alive. */
    if ((HAL_GetTick() - g_hb_last_ms) >= 500U) {
      g_hb_last_ms = HAL_GetTick();
      g_hb_state = (g_hb_state == GPIO_PIN_SET) ? GPIO_PIN_RESET : GPIO_PIN_SET;
      HAL_GPIO_WritePin(DE485_Pin_GPIO_Port, DE485_Pin_Pin, g_hb_state);
    }
    g_boot_phase = 12;
    HAL_IWDG_Refresh(&hiwdg);  /* feed watchdog — must happen within 4s */
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSIDiv = RCC_HSI_DIV1;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_NONE;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_HSI;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_0) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief USART1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART1_UART_Init(void)
{

  /* USER CODE BEGIN USART1_Init 0 */

  /* USER CODE END USART1_Init 0 */

  /* USER CODE BEGIN USART1_Init 1 */

  /* USER CODE END USART1_Init 1 */
  huart1.Instance = USART1;
  huart1.Init.BaudRate = 115200;
  huart1.Init.WordLength = UART_WORDLENGTH_8B;
  huart1.Init.StopBits = UART_STOPBITS_1;
  huart1.Init.Parity = UART_PARITY_NONE;
  huart1.Init.Mode = UART_MODE_TX_RX;
  huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart1.Init.OverSampling = UART_OVERSAMPLING_16;
  huart1.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  huart1.Init.ClockPrescaler = UART_PRESCALER_DIV1;
  huart1.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if (HAL_UART_Init(&huart1) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_SetTxFifoThreshold(&huart1, UART_TXFIFO_THRESHOLD_1_2) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_SetRxFifoThreshold(&huart1, UART_RXFIFO_THRESHOLD_1_2) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_EnableFifoMode(&huart1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART1_Init 2 */

  /* USER CODE END USART1_Init 2 */

}

/**
  * @brief USART2 Initialization Function — Modbus RS485 (PA2=DI, PA3=RO, PA8=DE/RE)
  * @param None
  * @retval None
  */
static void MX_USART2_UART_Init(void)
{
  huart2.Instance = USART2;
  huart2.Init.BaudRate = 9600;  /* Selec EM4M default baud rate */
  huart2.Init.WordLength = UART_WORDLENGTH_8B;
  huart2.Init.StopBits = UART_STOPBITS_1;
  huart2.Init.Parity = UART_PARITY_NONE;
  huart2.Init.Mode = UART_MODE_TX_RX;
  huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart2.Init.OverSampling = UART_OVERSAMPLING_16;
  huart2.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  huart2.Init.ClockPrescaler = UART_PRESCALER_DIV1;
  // huart2.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT; // Removed to fix compilation
  if (HAL_UART_Init(&huart2) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_DisableFifoMode(&huart2) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief USART3 Initialization Function — Reyax RYL998 LoRa (PB8=TX, PB9=RX, 115200 baud)
  * @param None
  * @retval None
  */
static void MX_USART3_UART_Init(void)
{
  huart3.Instance = USART3;
  huart3.Init.BaudRate = 115200;
  huart3.Init.WordLength = UART_WORDLENGTH_8B;
  huart3.Init.StopBits = UART_STOPBITS_1;
  huart3.Init.Parity = UART_PARITY_NONE;
  huart3.Init.Mode = UART_MODE_TX_RX;
  huart3.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart3.Init.OverSampling = UART_OVERSAMPLING_16;
  huart3.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  huart3.Init.ClockPrescaler = UART_PRESCALER_DIV1;
  if (HAL_UART_Init(&huart3) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_DisableFifoMode(&huart3) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /* PA1 — pump1 SET coil (transistor driver), idle LOW */
  HAL_GPIO_WritePin(Relay_Pin_GPIO_Port, Relay_Pin_Pin, GPIO_PIN_RESET);
  GPIO_InitStruct.Pin = Relay_Pin_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(Relay_Pin_GPIO_Port, &GPIO_InitStruct);

  /* PB3 — pump1 RESET coil (transistor driver), idle LOW */
  HAL_GPIO_WritePin(Relay1_RST_GPIO_Port, Relay1_RST_Pin, GPIO_PIN_RESET);
  GPIO_InitStruct.Pin = Relay1_RST_Pin;
  HAL_GPIO_Init(Relay1_RST_GPIO_Port, &GPIO_InitStruct);

  /* PB4 — pump2 SET coil (transistor driver), idle LOW */
  HAL_GPIO_WritePin(Relay2_Pin_GPIO_Port, Relay2_Pin_Pin, GPIO_PIN_RESET);
  GPIO_InitStruct.Pin = Relay2_Pin_Pin;
  HAL_GPIO_Init(Relay2_Pin_GPIO_Port, &GPIO_InitStruct);

  /* PB5 — pump2 RESET coil (transistor driver), idle LOW */
  HAL_GPIO_WritePin(Relay2_RST_GPIO_Port, Relay2_RST_Pin, GPIO_PIN_RESET);
  GPIO_InitStruct.Pin = Relay2_RST_Pin;
  HAL_GPIO_Init(Relay2_RST_GPIO_Port, &GPIO_InitStruct);

  /* RS485 DE/RE — PA8, default LOW (receive mode) */
  HAL_GPIO_WritePin(DE485_Pin_GPIO_Port, DE485_Pin_Pin, GPIO_PIN_RESET);
  GPIO_InitStruct.Pin = DE485_Pin_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(DE485_Pin_GPIO_Port, &GPIO_InitStruct);
}

/* USER CODE BEGIN 4 */

/**
  * @brief  Debug_Print — OTA/modem diagnosis via USART3 (PB8=TX, 115200 baud).
  *         Connect USB-serial RX → PB8, GND → GND, 115200 8N1.
  */
#if !defined(FW_MIN_LOGS)
void Debug_Print(const char *msg) {
  const uint8_t *p = (const uint8_t *)msg;
  while (*p) {
    uint32_t guard = 200000U;
    while (((USART3->ISR & USART_ISR_TXE_TXFNF) == 0U) && guard--) {
      WDG_KICK();
    }
    if (guard == 0U) {
      return;
    }
    USART3->TDR = *p++;
    WDG_KICK();
  }
}
#endif

/* USER CODE END 4 */

/**
  * @brief  IWDG Initialization — ~32s timeout using LSI (~32kHz).
  *         Started AFTER Modem_Init so the 10s boot sequence doesn't trigger it.
  *         If the main loop stops feeding the watchdog (firmware crash/hang),
  *         the STM32 resets automatically.
  */
static void MX_IWDG_Init(void)
{
  /* Timeout = (Reload + 1) * Prescaler / LSI_freq
   * = (4095 + 1) * 256 / 32000 ≈ 32.8 s
   * OTA + bootloader copy path can exceed 4 s, so keep a wider watchdog window. */
  hiwdg.Instance       = IWDG;
  hiwdg.Init.Prescaler = IWDG_PRESCALER_256;
  hiwdg.Init.Window    = IWDG_WINDOW_DISABLE;
  hiwdg.Init.Reload    = 4095;
  if (HAL_IWDG_Init(&hiwdg) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief ADC1 Initialization — 12-bit, software trigger, single conversion.
  *        Channels configured per-read in sensors_adc.c.
  *        SamplingTimeCommon1 = 160.5 cycles for accurate readings on slow signals.
  */
#ifdef HAL_ADC_MODULE_ENABLED
static void MX_ADC1_Init(void)
{
  hadc1.Instance                   = ADC1;
  hadc1.Init.ClockPrescaler        = ADC_CLOCK_SYNC_PCLK_DIV2;
  hadc1.Init.Resolution            = ADC_RESOLUTION_12B;
  hadc1.Init.DataAlign             = ADC_DATAALIGN_RIGHT;
  hadc1.Init.ScanConvMode          = ADC_SCAN_DISABLE;
  hadc1.Init.EOCSelection          = ADC_EOC_SINGLE_CONV;
  hadc1.Init.LowPowerAutoWait      = DISABLE;
  hadc1.Init.LowPowerAutoPowerOff  = DISABLE;
  hadc1.Init.ContinuousConvMode    = DISABLE;
  hadc1.Init.NbrOfConversion       = 1;
  hadc1.Init.DiscontinuousConvMode = DISABLE;
  hadc1.Init.ExternalTrigConv      = ADC_SOFTWARE_START;
  hadc1.Init.ExternalTrigConvEdge  = ADC_EXTERNALTRIGCONVEDGE_NONE;
  hadc1.Init.DMAContinuousRequests = DISABLE;
  hadc1.Init.Overrun               = ADC_OVR_DATA_PRESERVED;
  hadc1.Init.SamplingTimeCommon1   = ADC_SAMPLETIME_160CYCLES_5;
  if (HAL_ADC_Init(&hadc1) != HAL_OK)
    Error_Handler();
  /* Self-calibration — run once after init for improved accuracy */
  if (HAL_ADCEx_Calibration_Start(&hadc1) != HAL_OK)
    Error_Handler();
}
#endif

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
