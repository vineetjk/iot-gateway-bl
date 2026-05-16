/**
 * bootloader.h
 * =========================================================================
 * Shared definitions used by BOTH the bootloader project and the
 * application project. Copy this file into both STM32CubeIDE projects.
 *
 * Flash map (STM32F103CBT6 — 128 KB internal flash):
 *   0x08000000   Bootloader       12 KB
 *   0x08003000   Application     108 KB
 *   0x0801F000   Config Page A     2 KB
 *   0x0801F800   Config Page B     2 KB
 *
 * W25Q64 SPI flash map (8 MB):
 *   0x000000     OTA Metadata      4 KB
 *   0x001000     FW Slot A       128 KB  (download target)
 *   0x021000     FW Slot B       128 KB  (previous FW backup)
 *   0x041000     Config mirror    16 KB
 *   0x045000     Telemetry log    ~7 MB  (ring buffer)
 * =========================================================================
 */
#ifndef BOOTLOADER_H
#define BOOTLOADER_H

#include "stm32f1xx_hal.h"
#include <stdint.h>
#include <stdbool.h>

/* ── Internal flash ───────────────────────────────────────────────── */
#define BL_FLASH_BOOT_START     0x08000000UL
#define BL_FLASH_APP_START      0x08003000UL
#define BL_FLASH_APP_SIZE       (108U * 1024U)
#define BL_FLASH_APP_END        (BL_FLASH_APP_START + BL_FLASH_APP_SIZE)
#define BL_FLASH_PAGE_SIZE      1024U   /* STM32F103 medium-density = 1KB pages */
#define BL_FLASH_CFG_PAGE_A     0x0801E000UL
#define BL_FLASH_CFG_PAGE_B     0x0801E800UL

/* ── SRAM boundaries ─────────────────────────────────────────────── */
#define BL_SRAM_START           0x20000000UL
#define BL_SRAM_END             0x20005000UL   /* 20 KB on F103CB */
#define OTA_FLAG_ADDR           0x20004FFCUL   /* last 4 bytes SRAM */
#define OTA_FLAG_MAGIC          0xDEAD600DUL
#define OTA_FLAG_NONE           0x00000000UL

/* ── W25Q64 SPI flash map ────────────────────────────────────────── */
#define SPI_FLASH_OTA_META_ADDR 0x000000UL
#define SPI_FLASH_SLOT_A_ADDR   0x001000UL
#define SPI_FLASH_SLOT_B_ADDR   0x021000UL
#define SPI_FLASH_CFG_ADDR      0x041000UL
#define SPI_FLASH_LOG_ADDR      0x045000UL
#define SPI_FLASH_SLOT_SIZE     (128U * 1024U)

/* ── OTA metadata ────────────────────────────────────────────────── */
#define OTA_META_MAGIC          0xB007AB1EUL

typedef enum {
    OTA_STATUS_EMPTY      = 0x00,
    OTA_STATUS_DOWNLOADED = 0x01,
    OTA_STATUS_FLASHING   = 0x02,
    OTA_STATUS_SUCCESS    = 0x03,
    OTA_STATUS_FAILED     = 0x04,
    OTA_STATUS_ROLLBACK   = 0x05,
} OtaStatus_t;

#pragma pack(1)
typedef struct {
    uint32_t    magic;
    uint32_t    fw_version;
    uint32_t    fw_size;
    uint32_t    fw_crc32;
    uint32_t    slot_addr;
    OtaStatus_t status;
    uint32_t    timestamp;
    char        fw_url[128];
    uint8_t     first_boot_done;
    uint8_t     boot_attempts;
    uint8_t     _pad[2];
    uint32_t    meta_crc32;
} OtaMeta_t;
#pragma pack()

/* ── Serial update protocol ─────────────────────────────────────── */
#define BL_SYNC_BYTE            0x7FU    /* PC sends this to initiate */
#define BL_ACK                  0x79U
#define BL_NACK                 0x1FU
#define BL_CMD_START            0x01U    /* start update: [cmd][size:4][crc32:4] */
#define BL_CMD_DATA             0x02U    /* data chunk: [cmd][len:2][data:N][crc16:2] */
#define BL_CMD_VERIFY           0x03U    /* verify & boot */
#define BL_CHUNK_SIZE           256U
#define BL_SYNC_TIMEOUT_MS      1500U   /* wait this long for sync on boot */
#define BL_SERIAL_FLAG_MAGIC    0x5E41A1FFUL  /* "SERIAL_UF" - serial update flag */
#define BL_SERIAL_FLAG_ADDR     0x20004FF8UL  /* 4 bytes before OTA flag */

/* ── API callable from application ──────────────────────────────── */
void BL_TriggerOTA(void);      /* sets flag + resets — does NOT return */
void BL_TriggerSerialUpdate(void); /* sets serial flag + resets */
void BL_ConfirmBoot(void);     /* call after MQTT up post-OTA          */
bool BL_IsFirstBootAfterOTA(void);

#endif /* BOOTLOADER_H */
