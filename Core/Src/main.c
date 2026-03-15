/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2025 STMicroelectronics.
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
/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "song_hex.h"
#include <math.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
typedef struct {
    uint8_t  active;
    uint8_t  note;
} ActiveNote;

/* 각 트랙의 파싱 상태 */
typedef struct {
    const uint8_t *ptr;
    const uint8_t *end;
    uint32_t next_ticks;      // 다음 이벤트까지 남은 ticks
    uint8_t  running_status;
    uint8_t  finished;        // 1이면 끝난 트랙
} TrackState;

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

#define BUZZER_GPIO_PORT SND_GPIO_Port
#define BUZZER_PIN       SND_Pin

/* 타악기(CH10=9), 불필요한 채널 무시하려면 설정 */
#define IGNORE_CH1   255  // 예: 2 → 255이면 비활성
#define IGNORE_CH2   255
#define DRUM_CHANNEL 9
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
/* USER CODE BEGIN PFP */

/* ====== DWT 딜레이 ====== */
static void DWT_Delay_Init(void);
static void DWT_Delay_us(uint32_t us);

/* ====== 기본 HAL 함수 ====== */
void SystemClock_Config(void);
static void MX_GPIO_Init(void);

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
static void DWT_Delay_Init(void) {
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

static void DWT_Delay_us(uint32_t us) {
    uint32_t cycles = (SystemCoreClock / 1000000UL) * us;
    uint32_t start = DWT->CYCCNT;
    while ((DWT->CYCCNT - start) < cycles);
}

static void play_tone(uint32_t freq, uint32_t duration_ms)
{
    if (freq < 20) {
        HAL_Delay(duration_ms);
        return;
    }

    uint32_t period_us = 1000000UL / freq;
    uint32_t half = period_us / 2;
    uint32_t total = duration_ms * 1000UL;
    uint32_t cycles = total / period_us;

    for (uint32_t i=0; i<cycles; i++) {
        HAL_GPIO_TogglePin(BUZZER_GPIO_PORT, BUZZER_PIN);
        DWT_Delay_us(half);
        HAL_GPIO_TogglePin(BUZZER_GPIO_PORT, BUZZER_PIN);
        DWT_Delay_us(half);
    }
}

static uint32_t midi_read_vlq(const uint8_t **pp, const uint8_t *end)
{
    uint32_t v = 0;
    const uint8_t *p = *pp;
    while (p < end) {
        uint8_t c = *p++;
        v = (v << 7) | (c & 0x7F);
        if (!(c & 0x80)) break;
    }
    *pp = p;
    return v;
}

static uint32_t midi_note_to_freq(uint8_t note)
{
    float x = ((float)note - 69.0f) * 0.0833333f;
    float f = 440.0f * expf(x * 0.69314718f);
    if (f < 1.0f) f = 1.0f;
    return (uint32_t)(f + 0.5f);
}

/* ===================================================================== */
/* ======================   FORMAT-1 PLAYER   =========================== */
/* ===================================================================== */

/* 이벤트 하나를 파싱하고 next_ticks 채우기 */
static void track_get_next_event(TrackState *trk, uint32_t *delta_ticks)
{
    if (trk->finished) {
        *delta_ticks = 0xFFFFFFFF;
        return;
    }

    const uint8_t *p = trk->ptr;

    if (p >= trk->end) {
        trk->finished = 1;
        *delta_ticks = 0xFFFFFFFF;
        return;
    }

    uint32_t dt = midi_read_vlq(&p, trk->end);
    *delta_ticks = dt;
    trk->ptr = p;
}

static void play_midi_from_memory(const uint8_t *data, uint32_t size)
{
    const uint8_t *p = data;
    const uint8_t *end = data + size;

    if (end - p < 14) return;
    if (memcmp(p, "MThd", 4) != 0) return;

    p += 4;
    uint32_t hdr_len = (p[0]<<24)|(p[1]<<16)|(p[2]<<8)|p[3]; p+=4;

    uint16_t format   = (p[0]<<8)|p[1];
    uint16_t ntrks    = (p[2]<<8)|p[3];
    uint16_t division = (p[4]<<8)|p[5];
    p += hdr_len;

    if (format != 1 && format != 0) return;
    if (ntrks < 1) return;
    if (ntrks > 32) return;  // 안전상 한도

    /* 트랙 배열 */
    TrackState tracks[32];
    uint8_t track_count = 0;

    for (int t=0; t<ntrks; t++) {
        if (p+8 > end) break;
        if (memcmp(p, "MTrk", 4) != 0) break;

        uint32_t len = (p[4]<<24)|(p[5]<<16)|(p[6]<<8)|p[7];
        p += 8;

        tracks[track_count].ptr = p;
        tracks[track_count].end = p + len;
        tracks[track_count].finished = 0;
        tracks[track_count].running_status = 0;

        p += len;
        track_count++;
    }

    /* delta time 초기화 */
    for (int t=0; t<track_count; t++) {
        uint32_t dt;
        track_get_next_event(&tracks[t], &dt);
        tracks[t].next_ticks = dt;
    }

    uint32_t tempo_us_per_qn = 500000;
    ActiveNote active[16] = {0};

    /* Playback Loop */
    while (1)
    {
        /* 가장 작은 delta ticks 찾기 */
        uint32_t min_ticks = 0xFFFFFFFF;
        int sel = -1;

        for (int t=0; t<track_count; t++) {
            if (!tracks[t].finished && tracks[t].next_ticks < min_ticks) {
                min_ticks = tracks[t].next_ticks;
                sel = t;
            }
        }

        if (sel < 0 || min_ticks == 0xFFFFFFFF) break;

        /* 모든 트랙의 next_ticks에서 min_ticks 빼기 */
        for (int t=0; t<track_count; t++) {
            if (!tracks[t].finished && tracks[t].next_ticks != 0xFFFFFFFF)
                tracks[t].next_ticks -= min_ticks;
        }

        /* delta time 만큼 재생 */
        if (min_ticks > 0) {
            uint32_t delta_ms =
                ((uint64_t)min_ticks * tempo_us_per_qn) / (division * 1000ULL);

            int best = -1;
            uint8_t best_note = 0;

            for (int ch=0; ch<16; ch++) {
                if (!active[ch].active) continue;
                if (ch == DRUM_CHANNEL) continue;
                if (ch == IGNORE_CH1) continue;
                if (ch == IGNORE_CH2) continue;

                if (best < 0 || ch < best) {
                    best = ch;
                    best_note = active[ch].note;
                }
            }

            if (best >= 0) {
                play_tone(midi_note_to_freq(best_note), delta_ms);
            } else {
                HAL_Delay(delta_ms);
            }
        }

        /* 선택된 트랙의 이벤트 처리 */
        TrackState *trk = &tracks[sel];
        const uint8_t *pp = trk->ptr;

        if (pp >= trk->end) {
            trk->finished = 1;
            continue;
        }

        uint8_t status = *pp;

        /* Meta */
        if (status == 0xFF) {
            pp++;
            uint8_t meta = *pp++;
            uint32_t len = midi_read_vlq(&pp, trk->end);

            if (meta == 0x51 && len == 3) {
                tempo_us_per_qn = (pp[0]<<16)|(pp[1]<<8)|pp[2];
            }

            pp += len;
            trk->ptr = pp;
            track_get_next_event(trk, &trk->next_ticks);
            continue;
        }

        /* SysEx */
        if (status == 0xF0 || status == 0xF7) {
            pp++;
            uint32_t len = midi_read_vlq(&pp, trk->end);
            pp += len;
            trk->ptr = pp;
            track_get_next_event(trk, &trk->next_ticks);
            continue;
        }

        /* Running Status */
        if (status < 0x80) {
            status = trk->running_status;
        } else {
            pp++;
            trk->running_status = status;
        }

        uint8_t type = status & 0xF0;
        uint8_t ch   = status & 0x0F;

        if (type == 0x90 || type == 0x80) {
            uint8_t note = *pp++;
            uint8_t vel  = *pp++;

            uint8_t on = (type == 0x90 && vel > 0);

            if (on) {
                active[ch].active = 1;
                active[ch].note   = note;
            } else {
                if (active[ch].active && active[ch].note == note)
                    active[ch].active = 0;
            }

            trk->ptr = pp;
            track_get_next_event(trk, &trk->next_ticks);
        }
        else {
            uint8_t dlen = (type==0xC0 || type==0xD0) ? 1 : 2;
            pp += dlen;
            trk->ptr = pp;
            track_get_next_event(trk, &trk->next_ticks);
        }
    }
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  /* USER CODE BEGIN 2 */
  DWT_Delay_Init();
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
	  play_midi_from_memory(song_mid, song_mid_len);
	  HAL_Delay(2000);
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

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_NONE;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_HSI;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_0) != HAL_OK)
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
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOA_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOA, SND_Pin|REF_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pins : SND_Pin REF_Pin */
  GPIO_InitStruct.Pin = SND_Pin|REF_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

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
