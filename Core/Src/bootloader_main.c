/**
 * bootloader_main.c — Serial + SPI OTA Firmware Update Bootloader
 * =========================================================================
 * 12KB at 0x08000000. Application at 0x08003000.
 * Uses USART1 (PA9/PA10, CH340 USB-C) at 115200 baud.
 *
 * Boot sequence:
 *   1. Wait 1.5s for sync byte (0x7F) on USART1
 *   2. If sync → serial firmware update
 *   3. If SRAM flag set by app ('fwupdate') → wait longer for sync
 *   4. Check SPI flash OTA flag → OTA update
 *   5. Validate app → jump to 0x08003000
 * =========================================================================
 */

#include "stm32f1xx_hal.h"
#include <string.h>
#include <stdbool.h>

/* ── Flash layout ────────────────────────────────────────────────── */
#define APP_START       0x08003000UL
#define APP_MAX_SIZE    (108UL * 1024UL)
#define FLASH_PAGE_SZ   1024U
#define SRAM_START      0x20000000UL
#define SRAM_END        0x20005000UL

/* ── Serial protocol ─────────────────────────────────────────────── */
#define SYNC_BYTE       0x7FU
#define ACK             0x79U
#define NACK            0x1FU
#define CMD_START       0x01U
#define CMD_DATA        0x02U
#define CMD_VERIFY      0x03U
#define CHUNK_SIZE      256U
#define SYNC_TIMEOUT_MS 1500U

/* ── SRAM flags ──────────────────────────────────────────────────── */
#define SERIAL_FLAG_ADDR  0x20004FF8UL
#define SERIAL_FLAG_MAGIC 0x5E41A1FFUL
#define OTA_FLAG_ADDR     0x20004FFCUL
#define OTA_FLAG_MAGIC    0xDEAD600DUL

/* ── Peripheral handles ──────────────────────────────────────────── */
static UART_HandleTypeDef huart1;
static SPI_HandleTypeDef  hspi1;

/* ── SPI flash (W25Q64) minimal driver ───────────────────────────── */
static GPIO_TypeDef *cs_port;
static uint16_t      cs_pin;

static void cs_lo(void) { HAL_GPIO_WritePin(cs_port, cs_pin, GPIO_PIN_RESET); }
static void cs_hi(void) { HAL_GPIO_WritePin(cs_port, cs_pin, GPIO_PIN_SET); }

static void spi_tx(const uint8_t *d, uint16_t n) { HAL_SPI_Transmit(&hspi1,(uint8_t*)d,n,200); }
static void spi_rx(uint8_t *d, uint16_t n)       { HAL_SPI_Receive(&hspi1,d,n,200); }

static void w25q_read(uint32_t addr, uint8_t *buf, uint32_t len)
{
    uint8_t cmd[4] = { 0x03, (addr>>16)&0xFF, (addr>>8)&0xFF, addr&0xFF };
    cs_lo(); spi_tx(cmd,4); spi_rx(buf,len); cs_hi();
}

static bool w25q_detect(void)
{
    uint8_t cmd=0x9F, id[3];
    cs_lo(); spi_tx(&cmd,1); spi_rx(id,3); cs_hi();
    uint8_t mfr=id[0], cap=id[2];
    return (mfr==0xEF||mfr==0x20||mfr==0xC8) && cap>=0x16;
}

/* ── OTA metadata (matches bootloader.h) ─────────────────────────── */
#define OTA_META_MAGIC  0xB007AB1EUL
#define OTA_META_ADDR   0x000000UL
#define OTA_SLOT_A      0x001000UL
#define OTA_SLOT_B      0x021000UL

#define OTA_DOWNLOADED  0x01
#define OTA_FLASHING    0x02
#define OTA_SUCCESS     0x03

#pragma pack(1)
typedef struct {
    uint32_t magic, fw_version, fw_size, fw_crc32, slot_addr;
    uint8_t  status;
    uint32_t timestamp;
    char     fw_url[128];
    uint8_t  first_boot_done, boot_attempts, _pad[2];
    uint32_t meta_crc32;
} OtaMeta_t;
#pragma pack()

/* ── Minimal print (no printf — saves ~4KB flash) ────────────────── */
static void bl_print(const char *s)
{ HAL_UART_Transmit(&huart1,(uint8_t*)s,strlen(s),300); }

static void bl_hex32(uint32_t v)
{
    char buf[11]="0x00000000";
    const char h[]="0123456789ABCDEF";
    for(int i=9;i>=2;i--){ buf[i]=h[v&0xF]; v>>=4; }
    HAL_UART_Transmit(&huart1,(uint8_t*)buf,10,100);
}

static void bl_dec(uint32_t v)
{
    char buf[11]; int i=10; buf[i]=0;
    if(!v) buf[--i]='0';
    while(v){ buf[--i]='0'+(v%10); v/=10; }
    HAL_UART_Transmit(&huart1,(uint8_t*)&buf[i],10-i,100);
}

static void bl_send(uint8_t b)
{ HAL_UART_Transmit(&huart1,&b,1,100); }

static bool bl_recv(uint8_t *buf, uint16_t len, uint32_t timeout)
{ return HAL_UART_Receive(&huart1,buf,len,timeout)==HAL_OK; }

/* ── CRC-32 ──────────────────────────────────────────────────────── */
static uint32_t bl_crc32(const uint8_t *d, uint32_t len)
{
    uint32_t c=0xFFFFFFFFUL;
    while(len--){ c^=*d++; for(uint8_t b=0;b<8;b++) c=(c&1)?((c>>1)^0xEDB88320UL):(c>>1); }
    return c^0xFFFFFFFFUL;
}

/* ── Flash helpers ───────────────────────────────────────────────── */
static HAL_StatusTypeDef fl_erase(uint32_t addr, uint32_t len)
{
    FLASH_EraseInitTypeDef e={.TypeErase=FLASH_TYPEERASE_PAGES,.PageAddress=addr,
        .NbPages=(len+FLASH_PAGE_SZ-1)/FLASH_PAGE_SZ};
    uint32_t err=0;
    HAL_FLASH_Unlock();
    HAL_StatusTypeDef st=HAL_FLASHEx_Erase(&e,&err);
    HAL_FLASH_Lock();
    return st;
}

static HAL_StatusTypeDef fl_write(uint32_t dst, const uint8_t *src, uint32_t len)
{
    HAL_FLASH_Unlock();
    uint32_t addr=dst;
    for(uint32_t i=0;i<(len+1)/2;i++){
        uint8_t lo=(i*2<len)?src[i*2]:0xFF;
        uint8_t hi=(i*2+1<len)?src[i*2+1]:0xFF;
        if(HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD,addr,(uint16_t)(lo|(hi<<8)))!=HAL_OK)
            { HAL_FLASH_Lock(); return HAL_ERROR; }
        addr+=2;
    }
    HAL_FLASH_Lock();
    return HAL_OK;
}

/* ── Serial firmware update ──────────────────────────────────────── */
/* IMPORTANT: NO bl_print() during binary protocol exchange!
 * Debug text on the same UART corrupts ACK/NACK bytes. Only use
 * bl_send() for single-byte protocol responses (ACK/NACK). */
static void serial_update(void)
{
    uint8_t hdr[9];
    if(!bl_recv(hdr,9,30000)||hdr[0]!=CMD_START){ bl_send(NACK); return; }

    uint32_t fw_size=(uint32_t)hdr[1]|((uint32_t)hdr[2]<<8)|((uint32_t)hdr[3]<<16)|((uint32_t)hdr[4]<<24);
    uint32_t fw_crc =(uint32_t)hdr[5]|((uint32_t)hdr[6]<<8)|((uint32_t)hdr[7]<<16)|((uint32_t)hdr[8]<<24);

    if(!fw_size||fw_size>APP_MAX_SIZE){ bl_send(NACK); return; }

    if(fl_erase(APP_START,fw_size)!=HAL_OK){ bl_send(NACK); return; }
    bl_send(ACK);

    uint32_t total=0;
    uint8_t chunk[CHUNK_SIZE], phdr[3];

    while(total<fw_size){
        if(!bl_recv(phdr,3,10000)||phdr[0]!=CMD_DATA){ bl_send(NACK); return; }
        uint16_t clen=(uint16_t)phdr[1]|((uint16_t)phdr[2]<<8);
        if(!clen||clen>CHUNK_SIZE){ bl_send(NACK); return; }

        if(!bl_recv(chunk,clen,5000)){ bl_send(NACK); return; }
        uint8_t cb[2];
        if(!bl_recv(cb,2,5000)){ bl_send(NACK); return; }
        uint16_t rx_crc=(uint16_t)cb[0]|((uint16_t)cb[1]<<8);

        /* CRC16-CCITT over header+data */
        uint16_t cc=0xFFFFU;
        for(uint8_t i=0;i<3;i++){cc^=(uint16_t)phdr[i]<<8;for(uint8_t b=0;b<8;b++)cc=(cc&0x8000)?((cc<<1)^0x1021):(cc<<1);}
        for(uint16_t i=0;i<clen;i++){cc^=(uint16_t)chunk[i]<<8;for(uint8_t b=0;b<8;b++)cc=(cc&0x8000)?((cc<<1)^0x1021):(cc<<1);}

        if(cc!=rx_crc){ bl_send(NACK); continue; }
        if(fl_write(APP_START+total,chunk,clen)!=HAL_OK){ bl_send(NACK); return; }
        total+=clen;
        bl_send(ACK);
    }

    uint8_t vcmd;
    if(!bl_recv(&vcmd,1,5000)||vcmd!=CMD_VERIFY){ bl_send(NACK); return; }
    if(bl_crc32((const uint8_t*)APP_START,fw_size)!=fw_crc){ bl_send(NACK); return; }
    bl_send(ACK);
    HAL_Delay(50);
    bl_print("[BL] OK!\r\n");
}

/* ── SPI OTA update ──────────────────────────────────────────────── */
static bool spi_ota_update(void)
{
    if(!w25q_detect()){ bl_print("[BL] No W25Q\r\n"); return false; }
    bl_print("[BL] W25Q OK\r\n");

    if(!(*(volatile uint32_t*)OTA_FLAG_ADDR==OTA_FLAG_MAGIC)) return false;
    *(volatile uint32_t*)OTA_FLAG_ADDR=0;

    OtaMeta_t m;
    w25q_read(OTA_META_ADDR,(uint8_t*)&m,sizeof(m));
    if(m.magic!=OTA_META_MAGIC) return false;
    uint32_t mc=bl_crc32((const uint8_t*)&m,sizeof(OtaMeta_t)-4);
    if(mc!=m.meta_crc32||m.status!=OTA_DOWNLOADED) return false;

    bl_print("[BL] OTA: "); bl_dec(m.fw_size); bl_print("B\r\n");
    if(fl_erase(APP_START,m.fw_size)!=HAL_OK) return false;

    uint8_t buf[256];
    for(uint32_t off=0;off<m.fw_size;off+=256){
        uint32_t n=(m.fw_size-off>256)?256:(m.fw_size-off);
        w25q_read(m.slot_addr+off,buf,n);
        if(fl_write(APP_START+off,buf,n)!=HAL_OK) return false;
        if(off%8192==0){ bl_print("."); }
    }
    bl_print("\r\n");

    if(bl_crc32((const uint8_t*)APP_START,m.fw_size)!=m.fw_crc32){
        bl_print("[BL] OTA CRC FAIL\r\n"); return false;
    }
    bl_print("[BL] OTA OK\r\n");
    return true;
}

/* ── Validate & jump ─────────────────────────────────────────────── */
static bool app_valid(void)
{
    uint32_t sp=*(volatile uint32_t*)APP_START;
    uint32_t pc=*(volatile uint32_t*)(APP_START+4);
    return (sp>=SRAM_START&&sp<=SRAM_END&&pc>=APP_START&&pc<=(APP_START+APP_MAX_SIZE));
}

static void jump_to_app(void)
{
    __disable_irq();
    SysTick->CTRL=0;
    HAL_UART_DeInit(&huart1);
    HAL_SPI_DeInit(&hspi1);
    HAL_RCC_DeInit();
    SCB->VTOR=APP_START;
    __set_MSP(*(volatile uint32_t*)APP_START);
    __enable_irq();
    ((void(*)(void))(*(volatile uint32_t*)(APP_START+4)))();
}

/* ── Peripheral init (HSI 8 MHz) ─────────────────────────────────── */
static void sys_init(void)
{
    HAL_Init();
    RCC_OscInitTypeDef o={0};
    o.OscillatorType=RCC_OSCILLATORTYPE_HSI; o.HSIState=RCC_HSI_ON;
    o.HSICalibrationValue=16; o.PLL.PLLState=RCC_PLL_NONE;
    HAL_RCC_OscConfig(&o);
    RCC_ClkInitTypeDef c={0};
    c.ClockType=RCC_CLOCKTYPE_SYSCLK|RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
    c.SYSCLKSource=RCC_SYSCLKSOURCE_HSI; c.AHBCLKDivider=RCC_SYSCLK_DIV1;
    c.APB1CLKDivider=RCC_HCLK_DIV1; c.APB2CLKDivider=RCC_HCLK_DIV1;
    HAL_RCC_ClockConfig(&c,FLASH_LATENCY_0);
}

static void uart_init(void)
{
    __HAL_RCC_USART1_CLK_ENABLE(); __HAL_RCC_GPIOA_CLK_ENABLE();
    GPIO_InitTypeDef g={0};
    g.Pin=GPIO_PIN_9; g.Mode=GPIO_MODE_AF_PP; g.Speed=GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOA,&g);
    g.Pin=GPIO_PIN_10; g.Mode=GPIO_MODE_INPUT; g.Pull=GPIO_NOPULL;
    HAL_GPIO_Init(GPIOA,&g);
    huart1.Instance=USART1; huart1.Init.BaudRate=115200;
    huart1.Init.WordLength=UART_WORDLENGTH_8B; huart1.Init.StopBits=UART_STOPBITS_1;
    huart1.Init.Parity=UART_PARITY_NONE; huart1.Init.Mode=UART_MODE_TX_RX;
    huart1.Init.HwFlowCtl=UART_HWCONTROL_NONE;
    HAL_UART_Init(&huart1);
}

static void spi_init(void)
{
    __HAL_RCC_SPI1_CLK_ENABLE(); __HAL_RCC_GPIOA_CLK_ENABLE();
    GPIO_InitTypeDef g={0};
    g.Pin=GPIO_PIN_5|GPIO_PIN_7; g.Mode=GPIO_MODE_AF_PP; g.Speed=GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOA,&g);
    g.Pin=GPIO_PIN_6; g.Mode=GPIO_MODE_INPUT; g.Pull=GPIO_NOPULL;
    HAL_GPIO_Init(GPIOA,&g);
    g.Pin=GPIO_PIN_8; g.Mode=GPIO_MODE_OUTPUT_PP; g.Speed=GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOA,&g);
    HAL_GPIO_WritePin(GPIOA,GPIO_PIN_8,GPIO_PIN_SET);
    cs_port=GPIOA; cs_pin=GPIO_PIN_8;

    hspi1.Instance=SPI1; hspi1.Init.Mode=SPI_MODE_MASTER;
    hspi1.Init.Direction=SPI_DIRECTION_2LINES; hspi1.Init.DataSize=SPI_DATASIZE_8BIT;
    hspi1.Init.CLKPolarity=SPI_POLARITY_LOW; hspi1.Init.CLKPhase=SPI_PHASE_1EDGE;
    hspi1.Init.NSS=SPI_NSS_SOFT; hspi1.Init.BaudRatePrescaler=SPI_BAUDRATEPRESCALER_4;
    hspi1.Init.FirstBit=SPI_FIRSTBIT_MSB; hspi1.Init.TIMode=SPI_TIMODE_DISABLED;
    hspi1.Init.CRCCalculation=SPI_CRCCALCULATION_DISABLED;
    HAL_SPI_Init(&hspi1);
}

/* ════════════════════════════════════════════════════════════════════ */
int main(void)
{
    sys_init();

    /* Brief delay so ST-Link/SWD can grab the MCU for debugging/flashing.
     * Without this, the bootloader runs too fast and SWD can't connect. */
    HAL_Delay(100);

    uart_init();
    bl_print("\r\n[BL] v2.0\r\n");

    /* Check serial update flag from app */
    bool force=(*(volatile uint32_t*)SERIAL_FLAG_ADDR==SERIAL_FLAG_MAGIC);
    if(force){ *(volatile uint32_t*)SERIAL_FLAG_ADDR=0; bl_print("[BL] FW update\r\n"); }

    /* Wait for serial sync */
    uint8_t sync;
    uint32_t t0=HAL_GetTick();
    bool synced=false;
    uint32_t timeout=force?60000:SYNC_TIMEOUT_MS;
    while((HAL_GetTick()-t0)<timeout){
        if(bl_recv(&sync,1,100)&&sync==SYNC_BYTE){ synced=true; break; }
    }
    if(synced){ bl_send(ACK); serial_update(); goto validate; }

    /* SPI OTA check */
    spi_init();
    spi_ota_update();

validate:
    if(!app_valid()){
        bl_print("[BL] No app! Waiting...\r\n");
        while(1){
            if(bl_recv(&sync,1,1000)&&sync==SYNC_BYTE){
                bl_send(ACK); serial_update();
                if(app_valid()) break;
            }
        }
    }
    bl_print("[BL] Boot\r\n");
    HAL_Delay(10);
    jump_to_app();
    while(1){}
}

void Error_Handler(void){ while(1){} }
