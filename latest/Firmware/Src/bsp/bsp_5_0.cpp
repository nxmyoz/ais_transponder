/*
  Copyright (c) 2016-2020 Peter Antypas

  This file is part of the MAIANA™ transponder firmware.

  The firmware is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <https://www.gnu.org/licenses/>
*/



#include "bsp.hpp"
#include <stm32l4xx_hal.h>
#include "printf_serial.h"
#include <string.h>


#if BOARD_REV==50

SPI_HandleTypeDef hspi1;
IWDG_HandleTypeDef hiwdg;
UART_HandleTypeDef huart2;
UART_HandleTypeDef huart1;
TIM_HandleTypeDef htim2;

void SystemClock_Config();

char_input_cb gnssInputCallback = nullptr;
char_input_cb terminalInputCallback = nullptr;
irq_callback ppsCallback = nullptr;
irq_callback sotdmaCallback = nullptr;
irq_callback trxClockCallback = nullptr;
irq_callback rxClockCallback = nullptr;

#define STATION_DATA_ADDRESS 0x08019000

typedef struct
{
  GPIO_TypeDef *port;
  GPIO_InitTypeDef gpio;
  GPIO_PinState init;
} GPIO;

static const GPIO __gpios[] = {
    {RFSW_CTRL_PORT, {RFSW_CTRL_PIN, GPIO_MODE_OUTPUT_PP, GPIO_NOPULL, GPIO_SPEED_LOW, 0}, GPIO_PIN_SET},
    {TRX_IC_CLK_PORT, {TRX_IC_CLK_PIN, GPIO_MODE_IT_RISING, GPIO_NOPULL, GPIO_SPEED_LOW, 0}, GPIO_PIN_RESET},
    {GNSS_1PPS_PORT, {GNSS_1PPS_PIN, GPIO_MODE_IT_RISING, GPIO_NOPULL, GPIO_SPEED_LOW, 0}, GPIO_PIN_RESET},
    {GNSS_NMEA_RX_PORT, {GNSS_NMEA_RX_PIN, GPIO_MODE_AF_PP, GPIO_PULLUP, GPIO_SPEED_LOW, GPIO_AF7_USART2}, GPIO_PIN_RESET},
    {CS1_PORT, {CS1_PIN, GPIO_MODE_OUTPUT_PP, GPIO_NOPULL, GPIO_SPEED_HIGH, 0}, GPIO_PIN_SET},
    {SCK_PORT, {SCK_PIN, GPIO_MODE_AF_PP, GPIO_NOPULL, GPIO_SPEED_HIGH, GPIO_AF5_SPI1}, GPIO_PIN_SET},
    {MISO_PORT, {MISO_PIN, GPIO_MODE_AF_PP, GPIO_NOPULL, GPIO_SPEED_HIGH, GPIO_AF5_SPI1}, GPIO_PIN_SET},
    {MOSI_PORT, {MOSI_PIN, GPIO_MODE_AF_PP, GPIO_NOPULL, GPIO_SPEED_HIGH, GPIO_AF5_SPI1}, GPIO_PIN_SET},
    {SDN1_PORT, {SDN1_PIN, GPIO_MODE_OUTPUT_PP, GPIO_NOPULL, GPIO_SPEED_LOW, 0}, GPIO_PIN_SET},
    {TRX_IC_DATA_PORT, {TRX_IC_DATA_PIN, GPIO_MODE_INPUT, GPIO_NOPULL, GPIO_SPEED_LOW, 0}, GPIO_PIN_RESET},
    {UART_TX_PORT, {UART_TX_PIN, GPIO_MODE_AF_PP, GPIO_PULLUP, GPIO_SPEED_LOW, GPIO_AF7_USART1}, GPIO_PIN_RESET},
    {UART_RX_PORT, {UART_RX_PIN, GPIO_MODE_AF_PP, GPIO_PULLUP, GPIO_SPEED_LOW, GPIO_AF7_USART1}, GPIO_PIN_RESET},
    {SDN2_PORT, {SDN2_PIN, GPIO_MODE_OUTPUT_PP, GPIO_NOPULL, GPIO_SPEED_LOW, 0}, GPIO_PIN_SET},
    {RX_IC_CLK_PORT, {RX_IC_CLK_PIN, GPIO_MODE_IT_RISING, GPIO_NOPULL, GPIO_SPEED_LOW, 0}, GPIO_PIN_RESET},
    {RX_IC_DATA_PORT, {RX_IC_DATA_PIN, GPIO_MODE_INPUT, GPIO_NOPULL, GPIO_SPEED_LOW, 0}, GPIO_PIN_RESET},
    {PA_BIAS_PORT, {PA_BIAS_PIN, GPIO_MODE_OUTPUT_PP, GPIO_NOPULL, GPIO_SPEED_LOW, 0}, GPIO_PIN_RESET},
    {CS2_PORT, {CS2_PIN, GPIO_MODE_OUTPUT_PP, GPIO_NOPULL, GPIO_SPEED_HIGH, 0}, GPIO_PIN_SET},
};

extern "C"
{
  void Error_Handler(void)
  {
    printf_serial_now("[ERROR]\r\n");
    printf_serial_now("[ERROR] ***** System error handler resetting *****\r\n");
    NVIC_SystemReset();
  }
}


void gpio_pin_init();

void bsp_hw_init()
{
  HAL_Init();
  SystemClock_Config();

  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();

  __HAL_RCC_USART2_CLK_ENABLE();
  __HAL_RCC_USART1_CLK_ENABLE();
  __HAL_RCC_SPI1_CLK_ENABLE();
  __HAL_RCC_TIM2_CLK_ENABLE();

  gpio_pin_init();

  // 1PPS signal
  HAL_NVIC_SetPriority(EXTI2_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(EXTI2_IRQn);


  // RF IC clock interrupts
  HAL_NVIC_SetPriority(EXTI1_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(EXTI1_IRQn);


  HAL_NVIC_SetPriority(EXTI3_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(EXTI3_IRQn);


  // USART1 (main UART)
  huart1.Instance                     = USART1;
  huart1.Init.BaudRate                = 38400;
  huart1.Init.WordLength              = UART_WORDLENGTH_8B;
  huart1.Init.StopBits                = UART_STOPBITS_1;
  huart1.Init.Parity                  = UART_PARITY_NONE;
  huart1.Init.Mode                    = UART_MODE_TX_RX;
  huart1.Init.HwFlowCtl               = UART_HWCONTROL_NONE;
  huart1.Init.OverSampling            = UART_OVERSAMPLING_16;
  huart1.Init.OneBitSampling          = UART_ONE_BIT_SAMPLE_DISABLE;
  huart1.AdvancedInit.AdvFeatureInit  = UART_ADVFEATURE_NO_INIT;
  HAL_UART_Init(&huart1);

  HAL_NVIC_SetPriority(USART1_IRQn, 6, 0);
  HAL_NVIC_EnableIRQ(USART1_IRQn);
  __HAL_UART_ENABLE_IT(&huart1, UART_IT_RXNE);


  // SPI

  hspi1.Instance                = SPI1;
  hspi1.Init.Mode               = SPI_MODE_MASTER;
  hspi1.Init.Direction          = SPI_DIRECTION_2LINES;
  hspi1.Init.DataSize           = SPI_DATASIZE_8BIT;
  hspi1.Init.CLKPolarity        = SPI_POLARITY_LOW;
  hspi1.Init.CLKPhase           = SPI_PHASE_1EDGE;
  hspi1.Init.NSS                = SPI_NSS_SOFT;
  hspi1.Init.BaudRatePrescaler  = SPI_BAUDRATEPRESCALER_8;
  hspi1.Init.FirstBit           = SPI_FIRSTBIT_MSB;
  hspi1.Init.TIMode             = SPI_TIMODE_DISABLE;
  hspi1.Init.CRCCalculation     = SPI_CRCCALCULATION_DISABLE;
  hspi1.Init.CRCPolynomial      = 7;
  hspi1.Init.CRCLength          = SPI_CRC_LENGTH_DATASIZE;
  hspi1.Init.NSSPMode           = SPI_NSS_PULSE_DISABLE;

  if (HAL_SPI_Init(&hspi1) != HAL_OK)
    {
      Error_Handler();
    }

  // Set both CS pins high
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_SET);
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_7, GPIO_PIN_SET);

  __HAL_SPI_ENABLE(&hspi1);


  // USART2 (GNSS, RX only)
  huart2.Instance                     = USART2;
  huart2.Init.BaudRate                = 9600;
  huart2.Init.WordLength              = UART_WORDLENGTH_8B;
  huart2.Init.StopBits                = UART_STOPBITS_1;
  huart2.Init.Parity                  = UART_PARITY_NONE;
  huart2.Init.Mode                    = UART_MODE_TX_RX;
  huart2.Init.HwFlowCtl               = UART_HWCONTROL_NONE;
  huart2.Init.OverSampling            = UART_OVERSAMPLING_16;
  huart2.Init.OneBitSampling          = UART_ONE_BIT_SAMPLE_DISABLE;
  huart2.AdvancedInit.AdvFeatureInit  = UART_ADVFEATURE_NO_INIT;
  HAL_UART_Init(&huart2);

  HAL_NVIC_SetPriority(USART2_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(USART2_IRQn);
  __HAL_UART_ENABLE_IT(&huart2, UART_IT_RXNE);

  // TIM2 for SOTDMA (37.5Hz)
  uint32_t period = (SystemCoreClock / 37.5) - 1;

  __HAL_RCC_TIM2_CLK_ENABLE();
  htim2.Instance               = TIM2;
  htim2.Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
  htim2.Init.Prescaler         = 0;
  htim2.Init.CounterMode       = TIM_COUNTERMODE_UP;
  htim2.Init.Period            = period;
  htim2.Init.RepetitionCounter = 0;

  HAL_TIM_Base_Init(&htim2);

  HAL_NVIC_SetPriority(TIM2_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(TIM2_IRQn);
}

bool bsp_is_tx_disabled()
{
  return false;
}

void SystemClock_Config()
{
  RCC_OscInitTypeDef RCC_OscInitStruct;
  RCC_ClkInitTypeDef RCC_ClkInitStruct;
  RCC_PeriphCLKInitTypeDef PeriphClkInit;

  /**Initializes the CPU, AHB and APB bus clocks
   */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = 16;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = 1;
  RCC_OscInitStruct.PLL.PLLN = 10;  // 80 MHz
  //RCC_OscInitStruct.PLL.PLLN = 8; // 64 MHz
  //RCC_OscInitStruct.PLL.PLLN = 6; // 48 MHz

  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV7;
  RCC_OscInitStruct.PLL.PLLQ = RCC_PLLQ_DIV2;
  RCC_OscInitStruct.PLL.PLLR = RCC_PLLR_DIV2;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
    {
      Error_Handler();
    }

  /**Initializes the CPU, AHB and APB bus clocks
   */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
      |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK)
    {
      Error_Handler();
    }

  PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_USART1;
  PeriphClkInit.Usart1ClockSelection = RCC_USART1CLKSOURCE_HSI;
  if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit) != HAL_OK)
    {
      Error_Handler();
    }

  /**Configure the main internal regulator output voltage
   */
  if (HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1) != HAL_OK)
    {
      Error_Handler();
    }

  /**Configure the Systick interrupt time
   */
  HAL_SYSTICK_Config(HAL_RCC_GetHCLKFreq()/1000);

  /**Configure the Systick
   */
  HAL_SYSTICK_CLKSourceConfig(SYSTICK_CLKSOURCE_HCLK);

  /* SysTick_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(SysTick_IRQn, 0, 0);
}

void gpio_pin_init()
{
  for ( unsigned i = 0; i < sizeof __gpios / sizeof(GPIO); ++i )
    {
      const GPIO* io = &__gpios[i];
      HAL_GPIO_Init(io->port, (GPIO_InitTypeDef*)&io->gpio);
      if ( io->gpio.Mode == GPIO_MODE_OUTPUT_PP || io->gpio.Mode == GPIO_MODE_OUTPUT_OD )
        {
          HAL_GPIO_WritePin(io->port, io->gpio.Pin, io->init);
        }

    }
}

void HAL_MspInit(void)
{
  /* USER CODE BEGIN MspInit 0 */

  /* USER CODE END MspInit 0 */

  __HAL_RCC_SYSCFG_CLK_ENABLE();
  __HAL_RCC_PWR_CLK_ENABLE();

  HAL_NVIC_SetPriorityGrouping(NVIC_PRIORITYGROUP_4);

  /* System interrupt init*/
  /* MemoryManagement_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(MemoryManagement_IRQn, 0, 0);
  /* BusFault_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(BusFault_IRQn, 0, 0);
  /* UsageFault_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(UsageFault_IRQn, 0, 0);
  /* SVCall_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(SVCall_IRQn, 0, 0);
  /* DebugMonitor_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DebugMonitor_IRQn, 0, 0);
  /* PendSV_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(PendSV_IRQn, 0, 0);
  /* SysTick_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(SysTick_IRQn, 0, 0);

  /* USER CODE BEGIN MspInit 1 */

  /* USER CODE END MspInit 1 */
}

void bsp_set_rx_mode()
{
  HAL_GPIO_WritePin(PA_BIAS_PORT, PA_BIAS_PIN, GPIO_PIN_RESET);       // Kill the RF MOSFET bias voltage
  HAL_GPIO_WritePin(RFSW_CTRL_PORT, RFSW_CTRL_PIN, GPIO_PIN_SET);     // RF switch in RX position

  GPIO_InitTypeDef gpio;
  gpio.Pin = TRX_IC_DATA_PIN;
  gpio.Mode = GPIO_MODE_INPUT;
  gpio.Speed = GPIO_SPEED_FREQ_LOW;
  gpio.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(TRX_IC_DATA_PORT, &gpio);
}

void bsp_set_tx_mode()
{
  GPIO_InitTypeDef gpio;
  gpio.Pin = TRX_IC_DATA_PIN;
  gpio.Mode = GPIO_MODE_OUTPUT_PP;
  gpio.Speed = GPIO_SPEED_FREQ_LOW;
  gpio.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(TRX_IC_DATA_PORT, &gpio);

  HAL_GPIO_WritePin(PA_BIAS_PORT, PA_BIAS_PIN, GPIO_PIN_SET);       // RF MOSFET bias voltage
  HAL_GPIO_WritePin(RFSW_CTRL_PORT, RFSW_CTRL_PIN, GPIO_PIN_RESET); // RF switch in TX position
}

void bsp_gnss_on()
{
  // Do nothing
}

void bsp_gnss_off()
{
  // Do nothing
}

void USART_putc(USART_TypeDef* USARTx, char c)
{
  while ( !(USARTx->ISR & USART_ISR_TXE) )
    ;

  USARTx->TDR = c;
}

void bsp_write_char(char c)
{
  USART_putc(USART1, c);
}

void bsp_write_string(const char *s)
{
  for ( int i = 0; s[i] != 0; ++i )
    USART_putc(USART1, s[i]);
}

void bsp_start_wdt()
{
  IWDG_InitTypeDef iwdg;
  iwdg.Prescaler = IWDG_PRESCALER_64;
  iwdg.Reload = 0x0fff;
  iwdg.Window = 0x0fff;

  hiwdg.Instance = IWDG;
  hiwdg.Init = iwdg;

  HAL_IWDG_Init(&hiwdg);
}

void bsp_refresh_wdt()
{
  HAL_IWDG_Refresh(&hiwdg);
}

void bsp_set_gnss_input_callback(char_input_cb cb)
{
  gnssInputCallback = cb;
}

void bsp_set_terminal_input_callback(char_input_cb cb)
{
  terminalInputCallback = cb;
}

void bsp_start_sotdma_timer()
{
  HAL_TIM_Base_Start_IT(&htim2);
}

void bsp_stop_sotdma_timer()
{
  HAL_TIM_Base_Stop_IT(&htim2);
}

void bsp_set_gnss_1pps_callback(irq_callback cb)
{
  ppsCallback = cb;
}

void bsp_set_trx_clk_callback(irq_callback cb)
{
  trxClockCallback = cb;
}

void bsp_set_rx_clk_callback(irq_callback cb)
{
  rxClockCallback = cb;
}

void bsp_set_gnss_sotdma_timer_callback(irq_callback cb)
{
  sotdmaCallback = cb;
}

uint32_t bsp_get_sotdma_timer_value()
{
  return TIM2->CNT;
}

void bsp_set_sotdma_timer_value(uint32_t v)
{
  TIM2->CNT = v;
}

uint32_t bsp_get_system_clock()
{
  return SystemCoreClock;
}

uint8_t bsp_tx_spi_byte(uint8_t data)
{
  uint8_t result = 0;
  HAL_SPI_TransmitReceive(&hspi1, &data, &result, 1, 10);
  return result;
}


bool bsp_erase_flash_page(uint32_t address)
{
  if ( (address < FLASH_BASE) || (address % FLASH_PAGE_SIZE) )
    return false;

  bool result = true;

  DBG("Erasing page at %.8x\r\n\r\n", (unsigned)address);

  HAL_StatusTypeDef status = HAL_FLASH_Unlock();
  if ( status != HAL_OK )
    {
      // Looks like only way for this to happen is for the flash to be unlocked
      HAL_FLASH_Lock();
      DBG("Couldn't unlock flash\r\n");
      return false;
    }

  uint32_t page = (address - FLASH_BASE) / FLASH_PAGE_SIZE;
  __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);

  FLASH_EraseInitTypeDef erase;
  erase.TypeErase = FLASH_TYPEERASE_PAGES;
  erase.NbPages   = 1;
  erase.Banks     = FLASH_BANK_1;
  erase.Page      = page;

  DBG("Calling HAL_FLASHEx_Erase() for page %d\r\n", (int)page);
  uint32_t errPage = 0xFFFFFFFF;
  status = HAL_FLASHEx_Erase(&erase, &errPage);
  if ( status != HAL_OK )
    {
      //err = FLASH_ERASE_HAL_ERASE_FAILED;
      DBG("Page erase failed\r\n");
      result = false;
    }
  else if ( errPage != 0xFFFFFFFF )
    {
      //err = FLASH_ERASE_ERR_PAGE_RETURNED;
      DBG("Flash erase failed for page %.8x\r\n", (unsigned)errPage);
      result = false;
    }
  else
    {
      DBG("Verifying flash erase\r\n\r\n");
      HAL_Delay(100);
      // Verify the data was in fact erased to all 0xFF
      for ( uint32_t i = address; i < address + FLASH_PAGE_SIZE; ++i )
        {
          uint8_t *p = (uint8_t*)i;
          if ( *p != 0xFF )
            {
              //err = FLASH_ERASE_FAILED_SILENT;
              result = false;
              DBG("Flash erase silent failure\r\n");
              break;
            }
        }
    }

  HAL_FLASH_Lock();
  return result;
}

bool bsp_erase_station_data()
{
  return bsp_erase_flash_page(STATION_DATA_ADDRESS);
}


bool bsp_save_station_data(const StationData &data)
{
  if ( !bsp_erase_flash_page(STATION_DATA_ADDRESS) )
    return false;

  //return true;

  uint32_t pageAddress = STATION_DATA_ADDRESS;
  __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);

  HAL_StatusTypeDef status = HAL_FLASH_Unlock();
  if ( status != HAL_OK )
    {
      // Huhh???
      return false;
    }

  status = HAL_OK;
  uint64_t *p = (uint64_t*)&data;
  for ( uint32_t dw = 0; dw < sizeof(StationData) / 8; ++dw, ++p )
    {
      status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, pageAddress + dw*8, *p);
      if ( status != HAL_OK )
        break;
    }
  HAL_FLASH_Lock();

  return status == HAL_OK;
}

void bsp_reboot()
{
  NVIC_SystemReset();
}

bool bsp_read_station_data(StationData &data)
{
  memcpy(&data, (void*)STATION_DATA_ADDRESS, sizeof data);
  return data.magic == STATION_DATA_MAGIC;
}

void bsp_enter_dfu()
{
  // Nothing here
}

extern "C"
{

  void USART1_IRQHandler(void)
  {
    if ( __HAL_UART_GET_IT(&huart1, UART_IT_RXNE) )
      {
        __HAL_UART_CLEAR_IT(&huart1, UART_IT_RXNE);
        char c = USART1->RDR;
        if ( terminalInputCallback )
          terminalInputCallback(c);
      }
  }

  void EXTI2_IRQHandler(void)
  {
    if ( __HAL_GPIO_EXTI_GET_IT(GPIO_PIN_2) != RESET )
      {
        __HAL_GPIO_EXTI_CLEAR_IT(GPIO_PIN_2);
        if ( ppsCallback )
          ppsCallback();
      }
  }

  void USART2_IRQHandler()
  {
    if ( __HAL_UART_GET_IT(&huart2, UART_IT_RXNE) )
      {
        __HAL_UART_CLEAR_IT(&huart2, UART_IT_RXNE);
        char c = (char)USART2->RDR;
        if ( gnssInputCallback )
          gnssInputCallback(c);
      }
  }


  void TIM2_IRQHandler(void)
  {
    if(__HAL_TIM_GET_FLAG(&htim2, TIM_FLAG_UPDATE) != RESET)
      {
        if(__HAL_TIM_GET_IT_SOURCE(&htim2, TIM_IT_UPDATE) !=RESET)
          {
            __HAL_TIM_CLEAR_IT(&htim2, TIM_IT_UPDATE);
            if ( sotdmaCallback )
              sotdmaCallback();
          }
      }
  }

  void EXTI3_IRQHandler(void)
  {
    if ( __HAL_GPIO_EXTI_GET_IT(GPIO_PIN_3) != RESET )
      {
        __HAL_GPIO_EXTI_CLEAR_IT(GPIO_PIN_3);
        if ( rxClockCallback )
          rxClockCallback();
      }
  }

  void EXTI1_IRQHandler(void)
  {
    if ( __HAL_GPIO_EXTI_GET_IT(GPIO_PIN_1) != RESET )
      {
        __HAL_GPIO_EXTI_CLEAR_IT(GPIO_PIN_1);
        if ( trxClockCallback )
          trxClockCallback();
      }
  }

}

#endif