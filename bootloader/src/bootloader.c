/*
 * bootloader.c — STM32G070KBTx minimal bootloader
 *
 * Flash layout (128 KB total):
 *   0x08000000  Bootloader  (8 KB,  pages 0-3)   ← this code
 *   0x08002000  App Slot A  (56 KB, pages 4-31)  ← running app
 *   0x08010000  App Slot B  (56 KB, pages 32-59) ← OTA download target
 *   0x0801E000  OTA Flags   (8 KB,  pages 60-63)
 *
 * Boot sequence:
 *   1. Read OTA_Flags_t at 0x0801E000
 *   2. If magic == 0xA55A1234:
 *        a. CRC32 check of App Slot B (flags.size bytes)
 *        b. If OK: erase App Slot A pages, copy Slot B → Slot A
 *        c. Erase OTA Flags page
 *   3. jump_to_app() — checks MSP + Reset_Handler sanity
 *   4. Slot B fallback — if Slot A invalid, copy Slot B directly
 *   5. If all slots invalid — start IWDG and spin (~4s reset loop)
 */

#include "stm32g0xx_hal.h"
#include <string.h>
#include <stdbool.h>

/* ── Flash layout ── */
#define BOOTLOADER_ADDR  0x08000000UL
#define APP_SLOT_A_ADDR  0x08002000UL
#define APP_SLOT_B_ADDR  0x08010000UL
#define OTA_FLAGS_ADDR   0x0801E000UL
#define OTA_PAGE_SIZE    2048U
#define OTA_SLOT_SIZE    (56U * 1024U)

/* First page of Slot A (page 4 in STM32G070 2KB-page numbering) */
#define APP_SLOT_A_PAGE  4U
#define APP_SLOT_A_PAGES 28U  /* 56 KB / 2 KB = 28 pages */
#define OTA_FLAGS_PAGE   60U
#define OTA_FLAGS_PAGES  4U

#define OTA_MAGIC        0xA55A1234UL

typedef struct {
    uint32_t magic;
    uint32_t size;
    uint32_t crc32;
    uint32_t reserved;
} OTA_Flags_t;

static inline void boot_wdg_kick(void)
{
    IWDG->KR = 0xAAAAU;
}

/* ── CRC32 (IEEE 802.3 / PKZIP) — matches ota.c implementation ── */
static uint32_t crc32_update(uint32_t crc, const uint8_t *data, uint32_t len)
{
    crc = ~crc;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (crc & 1 ? 0xEDB88320UL : 0);
    }
    return ~crc;
}

/* ── Erase a range of pages ── */
static bool flash_erase_pages(uint32_t first_page, uint32_t num_pages)
{
    FLASH_EraseInitTypeDef e = {
        .TypeErase = FLASH_TYPEERASE_PAGES,
        .NbPages = 1,
    };
    uint32_t page_err = 0xFFFFFFFFUL;
    HAL_FLASH_Unlock();
    for (uint32_t i = 0; i < num_pages; i++) {
        e.Page = first_page + i;
        boot_wdg_kick();
        if (HAL_FLASHEx_Erase(&e, &page_err) != HAL_OK) {
            HAL_FLASH_Lock();
            return false;
        }
        boot_wdg_kick();
    }
    HAL_FLASH_Lock();
    return true;
}

/* ── Copy Slot B → Slot A in 8-byte (double-word) chunks ── */
static bool flash_copy_slot(uint32_t size)
{
    const uint8_t *src = (const uint8_t *)APP_SLOT_B_ADDR;
    uint32_t       dst = APP_SLOT_A_ADDR;
    uint32_t       copy_len = (size + 7U) & ~7U;

    HAL_FLASH_Unlock();
    for (uint32_t i = 0; i + 8 <= copy_len; i += 8) {
        uint64_t dw;
        memcpy(&dw, src + i, 8);
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, dst + i, dw) != HAL_OK) {
            HAL_FLASH_Lock();
            return false;
        }
        if ((i & 0x1FFU) == 0U) {
            boot_wdg_kick();
        }
    }
    HAL_FLASH_Lock();
    return true;
}

/* ── Validate Slot A vector table (MSP in RAM + Reset_Handler in Slot A) ── */
static bool slot_valid(uint32_t base)
{
    const uint32_t *vt = (const uint32_t *)base;
    /* MSP must point into RAM */
    if ((vt[0] & 0xFF000000UL) != 0x20000000UL) return false;
    /* Reset_Handler must be inside this slot (Thumb bit set → odd address) */
    uint32_t pc = vt[1] & ~1UL;
    if (pc < base || pc >= base + OTA_SLOT_SIZE)   return false;
    return true;
}

/* ── Check Slot B has plausible firmware (MSP only) ──────────────────────
 * The app is compiled for Slot A (Reset_Handler at 0x08002xxx), so we
 * cannot check the PC range against Slot B.  MSP in RAM is sufficient to
 * distinguish written firmware from erased flash (0xFFFFFFFF).           */
static bool slot_b_has_firmware(void)
{
    const uint32_t msp = *(const uint32_t *)APP_SLOT_B_ADDR;
    return ((msp & 0xFF000000UL) == 0x20000000UL);
}

/* ── Jump to application at Slot A ── */
static void jump_to_app(void)
{
    if (!slot_valid(APP_SLOT_A_ADDR)) return;

    const uint32_t *vt = (const uint32_t *)APP_SLOT_A_ADDR;

    __disable_irq();
    NVIC->ICER[0] = 0xFFFFFFFFUL;
    NVIC->ICPR[0] = 0xFFFFFFFFUL;

    SCB->VTOR = APP_SLOT_A_ADDR;
    __DSB();
    __ISB();

    __set_MSP(vt[0]);
    void (*app_reset)(void) = (void (*)(void))(vt[1]);
    app_reset();

    while (1) {}
}

/* ══════════════════════════════════════════════════════════════════════════
 * main — called by startup code after stack/BSS init
 * ══════════════════════════════════════════════════════════════════════════ */
int main(void)
{
    HAL_Init();

    /* ── Step 1: OTA flags path ── */
    const OTA_Flags_t *flags = (const OTA_Flags_t *)OTA_FLAGS_ADDR;

    if (flags->magic == OTA_MAGIC &&
        flags->size  > 0         &&
        flags->size  <= OTA_SLOT_SIZE)
    {
        /* Compute CRC32 of Slot B with periodic watchdog kicks. */
        uint32_t crc = 0;
        uint32_t done = 0;
        const uint8_t *src = (const uint8_t *)APP_SLOT_B_ADDR;
        while (done < flags->size) {
            uint32_t step = flags->size - done;
            if (step > 512U) step = 512U;
            crc = crc32_update(crc, src + done, step);
            done += step;
            boot_wdg_kick();
        }

        if (crc == flags->crc32)
        {
            /* CRC OK — copy Slot B to Slot A */
            if (flash_erase_pages(APP_SLOT_A_PAGE, APP_SLOT_A_PAGES))
                flash_copy_slot(flags->size);
        }
        /* Whether copy succeeded or CRC failed, erase the flags so we
         * don't loop trying to re-apply a bad update on every boot.    */
        flash_erase_pages(OTA_FLAGS_PAGE, OTA_FLAGS_PAGES);
    }

    /* ── Step 2: try Slot A ── */
    jump_to_app();

    /* ── Step 3: Slot B fallback — Slot A invalid, try Slot B directly ──
     * Covers the case where power was lost during Slot A erase/copy but
     * Slot B still has valid firmware from the completed OTA download.  */
    if (slot_b_has_firmware())
    {
        if (flash_erase_pages(APP_SLOT_A_PAGE, APP_SLOT_A_PAGES))
            flash_copy_slot(OTA_SLOT_SIZE);
        jump_to_app();
    }

    /* ── Step 4: both slots invalid — start IWDG and reset in ~4s ──
     * Keeps retrying on every reset so a new OTA can recover the device. */
    IWDG->KR  = 0xCCCCU;   /* start IWDG */
    IWDG->KR  = 0x5555U;   /* unlock PR/RLR */
    IWDG->PR  = 3U;         /* prescaler /32 → ~1 kHz tick from LSI */
    IWDG->RLR = 4000U;      /* ~4 s timeout */
    IWDG->KR  = 0xAAAAU;    /* reload */
    while (1) {}            /* IWDG fires → reset → retry */
}

/* ── Minimal HAL time-base (SysTick not strictly needed by bootloader) ── */
void SysTick_Handler(void)
{
    HAL_IncTick();
}

void Error_Handler(void)
{
    __disable_irq();
    while (1) {}
}
