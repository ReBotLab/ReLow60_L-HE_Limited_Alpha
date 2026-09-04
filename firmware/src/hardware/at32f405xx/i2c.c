/*
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */

#ifdef OLED_ENABLED

#include "hardware/hardware.h"
#include "hardware/oled_api.h"

#include "at32f402_405.h"

// Hardware I2C1: PB6 = SCL (AF4), PB7 = SDA (AF4)
// PB5 is still physically connected to SDA but configured as Hi-Z input.
//
// APB1 clock = 108MHz (SYSCLK 216MHz / APB1_DIV_2)
//
// clkctrl for ~400kHz Fast Mode:
//   PRESC(divl)=2  -> tPRESC = 3 / 108MHz = 27.78ns
//   SCLL=0x32 (50) -> tLOW  = 51 * 27.78ns = 1417ns  (FM min 1300ns)
//   SCLH=0x26 (38) -> tHIGH = 39 * 27.78ns = 1084ns  (FM min  600ns)
//   SDADEL=1       -> 27.78ns
//   SCLDEL=4       -> 5 * 27.78ns = 139ns  (FM min 100ns)
//   fSCL = 1 / (1417 + 1084) = ~400kHz
#define I2C_CLKCTRL 0x20412632

// 10ms timeout at 216MHz
#define I2C_TIMEOUT_CYCLES (F_CPU / 100)

// DMA state for non-blocking transfers
static volatile bool dma_transfer_busy = false;

// Wait for a DMA transfer in flight to finish. The flag is cleared by the STOP
// interrupt, and a slave that stops clocking mid-transfer never produces that
// STOP, so the wait is bounded: on timeout the transfer is torn down instead
// of the firmware hanging here forever.
static void i2c_wait_dma_idle(void) {
  uint32_t start = DWT->CYCCNT;
  while (dma_transfer_busy) {
    if ((DWT->CYCCNT - start) > I2C_TIMEOUT_CYCLES) {
      dma_channel_enable(DMA1_CHANNEL2, FALSE);
      i2c_dma_enable(I2C1, I2C_DMA_REQUEST_TX, FALSE);
      i2c_interrupt_enable(I2C1, I2C_STOP_INT, FALSE);
      dma_transfer_busy = false;
      break;
    }
  }
}

void oled_i2c_init(void) {
  crm_periph_clock_enable(CRM_GPIOB_PERIPH_CLOCK, TRUE);
  crm_periph_clock_enable(CRM_I2C1_PERIPH_CLOCK, TRUE);
  crm_periph_clock_enable(CRM_DMA1_PERIPH_CLOCK, TRUE);

  gpio_init_type gpio_cfg;

  // PB5: floating input (physically connected to SDA, must not drive the bus)
  gpio_default_para_init(&gpio_cfg);
  gpio_cfg.gpio_pins = GPIO_PINS_5;
  gpio_cfg.gpio_mode = GPIO_MODE_INPUT;
  gpio_cfg.gpio_pull = GPIO_PULL_NONE;
  gpio_init(GPIOB, &gpio_cfg);

  // PB6 (SCL) and PB7 (SDA): AF4 open-drain with pull-up
  gpio_default_para_init(&gpio_cfg);
  gpio_cfg.gpio_pins = GPIO_PINS_6 | GPIO_PINS_7;
  gpio_cfg.gpio_mode = GPIO_MODE_MUX;
  gpio_cfg.gpio_out_type = GPIO_OUTPUT_OPEN_DRAIN;
  gpio_cfg.gpio_pull = GPIO_PULL_UP;
  gpio_cfg.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
  gpio_init(GPIOB, &gpio_cfg);

  gpio_pin_mux_config(GPIOB, GPIO_PINS_SOURCE6, GPIO_MUX_4);
  gpio_pin_mux_config(GPIOB, GPIO_PINS_SOURCE7, GPIO_MUX_4);

  // Wait 150ms for SSD1306 VCC to stabilize
  uint32_t start = DWT->CYCCNT;
  while ((DWT->CYCCNT - start) < (uint32_t)(F_CPU / 1000) * 150)
    ;

  // Initialize I2C1 peripheral
  i2c_reset(I2C1);
  i2c_init(I2C1, 0, I2C_CLKCTRL);
  i2c_own_address1_set(I2C1, I2C_ADDRESS_MODE_7BIT, 0);
  i2c_enable(I2C1, TRUE);

  // Configure DMA1 Channel2 for I2C1_TX via DMAMUX
  dmamux_init(DMA1MUX_CHANNEL2, DMAMUX_DMAREQ_ID_I2C1_TX);
  dma_interrupt_enable(DMA1_CHANNEL2, DMA_FDT_INT, TRUE);
  nvic_irq_enable(DMA1_Channel2_IRQn, 1, 0);
  nvic_irq_enable(I2C1_EVT_IRQn, 1, 0);
}

void oled_i2c_write(uint8_t addr, const uint8_t *data, uint16_t len) {
  if (len == 0)
    return;

  // Wait for any pending DMA transfer
  i2c_wait_dma_idle();

  uint32_t timeout;

  // Wait for bus idle
  timeout = DWT->CYCCNT;
  while (i2c_flag_get(I2C1, I2C_BUSYF_FLAG) == SET) {
    if ((DWT->CYCCNT - timeout) > I2C_TIMEOUT_CYCLES)
      return;
  }

  // Start transfer with auto-stop (all OLED transfers are <= 255 bytes)
  i2c_transmit_set(I2C1, (uint16_t)(addr << 1), (uint8_t)len,
                   I2C_AUTO_STOP_MODE, I2C_GEN_START_WRITE);

  for (uint16_t i = 0; i < len; i++) {
    // Wait for transmit data register empty
    timeout = DWT->CYCCNT;
    while (i2c_flag_get(I2C1, I2C_TDIS_FLAG) != SET) {
      if (i2c_flag_get(I2C1, I2C_ACKFAIL_FLAG) == SET) {
        i2c_flag_clear(I2C1, I2C_ACKFAIL_FLAG);
        return;
      }
      if ((DWT->CYCCNT - timeout) > I2C_TIMEOUT_CYCLES)
        return;
    }
    i2c_data_send(I2C1, data[i]);
  }

  // Wait for auto-generated STOP condition
  timeout = DWT->CYCCNT;
  while (i2c_flag_get(I2C1, I2C_STOPF_FLAG) != SET) {
    if ((DWT->CYCCNT - timeout) > I2C_TIMEOUT_CYCLES)
      return;
  }
  i2c_flag_clear(I2C1, I2C_STOPF_FLAG);
}

void oled_i2c_write_dma(uint8_t addr, const uint8_t *data, uint16_t len) {
  if (len == 0)
    return;

  // Wait for any pending DMA transfer
  i2c_wait_dma_idle();

  // Wait for bus idle
  uint32_t timeout = DWT->CYCCNT;
  while (i2c_flag_get(I2C1, I2C_BUSYF_FLAG) == SET) {
    if ((DWT->CYCCNT - timeout) > I2C_TIMEOUT_CYCLES)
      return;
  }

  dma_transfer_busy = true;

  // Configure DMA1 Channel2
  dma_channel_enable(DMA1_CHANNEL2, FALSE);
  dma_flag_clear(DMA1_FDT2_FLAG);

  dma_init_type dma_cfg;
  dma_default_para_init(&dma_cfg);
  dma_cfg.buffer_size = len;
  dma_cfg.direction = DMA_DIR_MEMORY_TO_PERIPHERAL;
  dma_cfg.memory_base_addr = (uint32_t)data;
  dma_cfg.memory_data_width = DMA_MEMORY_DATA_WIDTH_BYTE;
  dma_cfg.memory_inc_enable = TRUE;
  dma_cfg.peripheral_base_addr = (uint32_t)&I2C1->txdt;
  dma_cfg.peripheral_data_width = DMA_PERIPHERAL_DATA_WIDTH_BYTE;
  dma_cfg.peripheral_inc_enable = FALSE;
  dma_cfg.priority = DMA_PRIORITY_MEDIUM;
  dma_cfg.loop_mode_enable = FALSE;
  dma_init(DMA1_CHANNEL2, &dma_cfg);

  // Enable I2C STOP interrupt to detect end of transfer
  i2c_interrupt_enable(I2C1, I2C_STOP_INT, TRUE);

  // Enable I2C1 DMA TX
  i2c_dma_enable(I2C1, I2C_DMA_REQUEST_TX, TRUE);

  // Start I2C transfer with auto-stop
  i2c_transmit_set(I2C1, (uint16_t)(addr << 1), (uint8_t)len,
                   I2C_AUTO_STOP_MODE, I2C_GEN_START_WRITE);

  // Enable DMA channel to begin transfer
  dma_channel_enable(DMA1_CHANNEL2, TRUE);

  // Returns immediately - DMA handles the rest
}

bool oled_i2c_dma_busy(void) { return dma_transfer_busy; }

//--------------------------------------------------------------------+
// DMA1 Channel2 transfer complete: all bytes sent to I2C TX
//--------------------------------------------------------------------+

void DMA1_Channel2_IRQHandler(void) {
  if (dma_interrupt_flag_get(DMA1_FDT2_FLAG) == SET) {
    dma_flag_clear(DMA1_FDT2_FLAG);
    dma_channel_enable(DMA1_CHANNEL2, FALSE);
    i2c_dma_enable(I2C1, I2C_DMA_REQUEST_TX, FALSE);
  }
}

//--------------------------------------------------------------------+
// I2C1 event interrupt: STOP condition detected (transfer complete)
//--------------------------------------------------------------------+

void I2C1_EVT_IRQHandler(void) {
  if (i2c_flag_get(I2C1, I2C_STOPF_FLAG) == SET) {
    i2c_flag_clear(I2C1, I2C_STOPF_FLAG);
    i2c_interrupt_enable(I2C1, I2C_STOP_INT, FALSE);
    dma_transfer_busy = false;
  }
}

#endif
