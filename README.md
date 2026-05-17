# STM32F103 Serial Bootloader

12 KB bootloader residing at 0x08000000 for the [IoT Gateway](https://github.com/vineetjk/iot-gateway) project. Supports serial firmware updates over USB-C and OTA flashing from external W25Q64 SPI flash.

## Features

- **Serial firmware update** -- binary protocol over USART1 (CH340 USB-C) with per-chunk CRC16 and full-image CRC32 verification
- **OTA flash from SPI flash** -- reads firmware from W25Q64 external flash, verifies CRC32, programs internal flash
- **Dual firmware slots** -- Slot A (download target) and Slot B (backup) on W25Q64 for rollback support
- **Application validation** -- checks stack pointer and reset vector before jumping to application
- **SRAM flags** -- application can request serial update or OTA boot via magic values in SRAM
- **Minimal footprint** -- no printf/sprintf, custom print routines; fits in 12 KB
- **SWD-safe startup** -- 100 ms delay on boot so ST-Link can always connect for debugging

## Memory Map

### Internal Flash (STM32F103, 128 KB)

| Address | Size | Region |
|---------|------|--------|
| 0x08000000 | 12 KB | Bootloader |
| 0x08003000 | 108 KB | Application |
| 0x0801E000 | 2 KB | Config Page A |
| 0x0801E800 | 2 KB | Config Page B |

### W25Q64 SPI Flash (8 MB)

| Address | Size | Region |
|---------|------|--------|
| 0x000000 | 4 KB | OTA Metadata |
| 0x001000 | 128 KB | Firmware Slot A (download target) |
| 0x021000 | 128 KB | Firmware Slot B (previous firmware backup) |
| 0x041000 | 16 KB | Configuration mirror |
| 0x045000 | ~7 MB | Telemetry log (ring buffer) |

### SRAM Flags

| Address | Magic Value | Purpose |
|---------|-------------|---------|
| 0x20004FF8 | 0x5E41A1FF | Serial update requested by application |
| 0x20004FFC | 0xDEAD600D | OTA update requested by application |

## Boot Sequence

```
Power On / Reset
      |
      v
[100ms SWD delay]
      |
      v
[Init USART1 @ 115200]
      |
      v
[Check SRAM serial flag] --yes--> [Wait 60s for sync]
      |                                    |
      no                              [Serial Update]
      |                                    |
      v                                    v
[Wait 1.5s for SYNC (0x7F)] --yes--> [Serial Update]
      |
      no
      |
      v
[Init SPI1, detect W25Q64]
      |
      v
[Check SRAM OTA flag + metadata] --valid--> [Flash from SPI] --> [Validate app]
      |                                                                |
      no valid OTA                                                     v
      |                                                          [Jump to app]
      v
[Validate app at 0x08003000]
      |
      +--valid----> [Jump to app]
      |
      +--invalid--> [Wait indefinitely for serial sync]
```

## Serial Update Protocol

The protocol uses USART1 at 115200 baud (8N1). The host sends a sync byte to initiate, then transfers firmware in 256-byte chunks.

### Protocol Phases

#### 1. SYNC

| Direction | Data | Description |
|-----------|------|-------------|
| Host -> BL | `0x7F` | Sync byte (repeated until ACK) |
| BL -> Host | `0x79` | ACK (ready for update) |

#### 2. START

| Direction | Data | Description |
|-----------|------|-------------|
| Host -> BL | `[0x01][size:4 LE][crc32:4 LE]` | Start command with firmware size and expected CRC32 |
| BL -> Host | `0x79` / `0x1F` | ACK (flash erased, ready) or NACK (bad size or erase fail) |

#### 3. DATA (repeated for each chunk)

| Direction | Data | Description |
|-----------|------|-------------|
| Host -> BL | `[0x02][len:2 LE][data:N][crc16:2 LE]` | Data chunk (max 256 bytes) with CRC16-CCITT |
| BL -> Host | `0x79` / `0x1F` | ACK (written) or NACK (CRC mismatch or write fail) |

CRC16-CCITT is computed over the 3-byte header plus the data payload.

#### 4. VERIFY

| Direction | Data | Description |
|-----------|------|-------------|
| Host -> BL | `0x03` | Verify command |
| BL -> Host | `0x79` / `0x1F` | ACK (CRC32 match, update complete) or NACK (verification failed) |

### Protocol Constants

| Constant | Value |
|----------|-------|
| SYNC_BYTE | 0x7F |
| ACK | 0x79 |
| NACK | 0x1F |
| CMD_START | 0x01 |
| CMD_DATA | 0x02 |
| CMD_VERIFY | 0x03 |
| CHUNK_SIZE | 256 bytes |

## OTA Boot Flow

When the application downloads new firmware to SPI flash and triggers a reboot:

1. Bootloader checks SRAM flag at 0x20004FFC for magic value `0xDEAD600D`
2. Clears the flag immediately (prevents infinite OTA loop on failure)
3. Reads OTA metadata from SPI flash address 0x000000
4. Validates: magic (`0xB007AB1E`), metadata CRC32, status == `DOWNLOADED`
5. Erases internal flash application region
6. Copies firmware from SPI flash slot to internal flash in 256-byte chunks
7. Verifies CRC32 of written internal flash against metadata
8. If CRC passes: boots the new application
9. If CRC fails: prints error, attempts to boot whatever is in flash

### OTA Metadata Structure

```c
typedef struct {
    uint32_t    magic;          // 0xB007AB1E
    uint32_t    fw_version;     // Semantic version encoded
    uint32_t    fw_size;        // Firmware binary size in bytes
    uint32_t    fw_crc32;       // CRC32 of firmware binary
    uint32_t    slot_addr;      // SPI flash source address
    uint8_t     status;         // OTA_STATUS_DOWNLOADED = 0x01
    uint32_t    timestamp;      // Unix timestamp
    char        fw_url[128];    // Source URL (informational)
    uint8_t     first_boot_done;// Set by app after successful boot
    uint8_t     boot_attempts;  // Rollback counter
    uint8_t     _pad[2];
    uint32_t    meta_crc32;     // CRC32 of this struct (excluding this field)
} OtaMeta_t;
```

## Building

1. Open in **STM32CubeIDE 1.6.0** (File -> Import -> Existing Projects into Workspace)
2. The linker script `STM32F103C8TX_BOOTLOADER.ld` places the bootloader at 0x08000000 with LENGTH = 12K
3. Build the project (output: `Debug/GATEWAY_BL.bin`)
4. Flash via ST-Link at address 0x08000000

The bootloader must be flashed before the application. The application linker script starts at 0x08003000 and sets VTOR accordingly.

## Application Integration

The application project includes `bootloader.h` with shared definitions and API functions:

```c
void BL_TriggerOTA(void);           // Sets SRAM flag + resets (does not return)
void BL_TriggerSerialUpdate(void);  // Sets serial flag + resets (does not return)
void BL_ConfirmBoot(void);          // Marks OTA as successful (disables rollback)
bool BL_IsFirstBootAfterOTA(void);  // Check if this is first boot after OTA
```

## Related Repositories

- **Gateway Firmware**: [iot-gateway](https://github.com/vineetjk/iot-gateway) -- Main application firmware (Modbus RTU + MQTT + OTA)

## License

MIT License. See [LICENSE](LICENSE) for details.
