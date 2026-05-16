/**
 * w25q_spi.h
 * =========================================================================
 * W25Q64 (8MB) SPI flash driver.
 * SPI1: PA5=SCK, PA6=MISO, PA7=MOSI, PA8=CS (GPIO, active low).
 * Compatible with W25Q16/W25Q32/W25Q64/W25Q128 — same command set.
 * =========================================================================
 */
#ifndef W25Q_SPI_H
#define W25Q_SPI_H

#include "stm32f1xx_hal.h"
#include <stdint.h>
#include <stdbool.h>

#define W25Q_PAGE_SIZE    256U
#define W25Q_SECTOR_SIZE  4096U
#define W25Q64_JEDEC_ID   0xEF4017UL

void              W25Q_Init(SPI_HandleTypeDef *hspi,
                             GPIO_TypeDef *cs_port, uint16_t cs_pin);
bool              W25Q_Detect(void);
void              W25Q_ReadID(uint32_t *id);
void              W25Q_Read(uint32_t addr, uint8_t *buf, uint32_t len);
HAL_StatusTypeDef W25Q_EraseSector(uint32_t addr);
HAL_StatusTypeDef W25Q_EraseRange(uint32_t addr, uint32_t len);
HAL_StatusTypeDef W25Q_WritePage(uint32_t addr,
                                  const uint8_t *buf, uint16_t len);
HAL_StatusTypeDef W25Q_Write(uint32_t addr,
                              const uint8_t *buf, uint32_t len);
void              W25Q_WaitBusy(void);

#endif /* W25Q_SPI_H */
