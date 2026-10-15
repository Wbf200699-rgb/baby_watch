#include "thread_code/vision_external_memory.h"

#include "hal_data.h"

#define VISION_SDRAM_BYTES (16U * 1024U * 1024U)
#define VISION_SDRAM_TEST_PATTERN (0x55aa33ccU)
#define VISION_SDRAM_TEST_ANTIPATTERN (~VISION_SDRAM_TEST_PATTERN)
#define VISION_SDRAM_TEST_ADDRESS_COUNT (23U)

static volatile bool g_vision_ospi_ready;
static volatile fsp_err_t g_vision_ospi_error = FSP_ERR_NOT_OPEN;
static volatile uint32_t g_vision_ospi_jedec_id;
static volatile bool g_vision_sdram_ready;
static volatile fsp_err_t g_vision_sdram_error = FSP_ERR_NOT_OPEN;
static volatile uint32_t g_vision_sdram_error_offset;

static fsp_err_t vision_sdram_address_bus_test(void)
{
    volatile uint32_t * addresses[VISION_SDRAM_TEST_ADDRESS_COUNT];
    uint32_t saved[VISION_SDRAM_TEST_ADDRESS_COUNT];
    uint32_t address_count = 1U;
    uintptr_t const base = (uintptr_t) BSP_FEATURE_SDRAM_START_ADDRESS;

    addresses[0] = (volatile uint32_t *) base;
    for (uint32_t offset = sizeof(uint32_t);
         (offset < VISION_SDRAM_BYTES) && (address_count < VISION_SDRAM_TEST_ADDRESS_COUNT);
         offset <<= 1U)
    {
        addresses[address_count++] = (volatile uint32_t *) (base + offset);
    }

    for (uint32_t i = 0U; i < address_count; i++)
    {
        saved[i] = *addresses[i];
        *addresses[i] = VISION_SDRAM_TEST_PATTERN;
    }
    __DSB();

    for (uint32_t i = 0U; i < address_count; i++)
    {
        if (VISION_SDRAM_TEST_PATTERN != *addresses[i])
        {
            g_vision_sdram_error_offset = (uint32_t) ((uintptr_t) addresses[i] - base);
            goto test_failed;
        }
    }

    for (uint32_t i = 0U; i < address_count; i++)
    {
        *addresses[i] = VISION_SDRAM_TEST_ANTIPATTERN;
        __DSB();

        if (VISION_SDRAM_TEST_ANTIPATTERN != *addresses[i])
        {
            g_vision_sdram_error_offset = (uint32_t) ((uintptr_t) addresses[i] - base);
            goto test_failed;
        }

        for (uint32_t j = 0U; j < address_count; j++)
        {
            if ((j != i) && (VISION_SDRAM_TEST_PATTERN != *addresses[j]))
            {
                g_vision_sdram_error_offset = (uint32_t) ((uintptr_t) addresses[j] - base);
                goto test_failed;
            }
        }

        *addresses[i] = VISION_SDRAM_TEST_PATTERN;
    }

    for (uint32_t i = 0U; i < address_count; i++)
    {
        *addresses[i] = saved[i];
    }
    __DSB();
    return FSP_SUCCESS;

test_failed:
    for (uint32_t i = 0U; i < address_count; i++)
    {
        *addresses[i] = saved[i];
    }
    __DSB();
    return FSP_ERR_INVALID_DATA;
}

static fsp_err_t vision_external_memory_read_jedec_id(void)
{
    spi_flash_direct_transfer_t transfer = {
        .data_u64 = 0U,
        .address = 0U,
        .command = 0x9fU,
        .dummy_cycles = 0U,
        .command_length = 1U,
        .address_length = 0U,
        .data_length = 3U
    };
    fsp_err_t err = R_OSPI_B_DirectTransfer(&g_vision_ospi_ctrl,
                                             &transfer,
                                             SPI_FLASH_DIRECT_TRANSFER_DIR_READ);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    g_vision_ospi_jedec_id = (uint32_t) transfer.data_u64 & 0x00ffffffU;
    if ((0U == g_vision_ospi_jedec_id) || (0x00ffffffU == g_vision_ospi_jedec_id))
    {
        return FSP_ERR_INVALID_DATA;
    }

    return FSP_SUCCESS;
}

fsp_err_t vision_external_memory_init(void)
{
    fsp_err_t err;

    g_vision_ospi_ready = false;
    g_vision_ospi_error = FSP_ERR_NOT_OPEN;
    g_vision_ospi_jedec_id = 0U;
    g_vision_sdram_ready = false;
    g_vision_sdram_error = FSP_ERR_NOT_OPEN;
    g_vision_sdram_error_offset = 0U;

    err = vision_sdram_address_bus_test();
    g_vision_sdram_error = err;
    if (FSP_SUCCESS != err)
    {
        g_vision_ospi_error = err;
        return err;
    }
    g_vision_sdram_ready = true;

    /* The board flash uses OSPI0/CS0.  IO2 (WP#) and IO3 (HOLD#/RESET#)
     * share P103 and P101, so keep both inactive before opening OSPI. */
    (void) R_IOPORT_PinWrite(&g_ioport_ctrl, BSP_IO_PORT_01_PIN_03, BSP_IO_LEVEL_HIGH);
    (void) R_IOPORT_PinWrite(&g_ioport_ctrl, BSP_IO_PORT_01_PIN_01, BSP_IO_LEVEL_HIGH);

    err = R_OSPI_B_Open(&g_vision_ospi_ctrl, &g_vision_ospi_cfg);
    if (FSP_SUCCESS == err)
    {
        err = R_OSPI_B_SpiProtocolSet(&g_vision_ospi_ctrl,
                                      SPI_FLASH_PROTOCOL_EXTENDED_SPI);
    }
    if (FSP_SUCCESS == err)
    {
        err = vision_external_memory_read_jedec_id();
    }

    g_vision_ospi_error = err;
    if (FSP_SUCCESS == err)
    {
        g_vision_ospi_ready = true;
    }

    return err;
}

bool vision_external_memory_is_ready(void)
{
    return g_vision_ospi_ready;
}

fsp_err_t vision_external_memory_last_error(void)
{
    return g_vision_ospi_error;
}

uint32_t vision_external_memory_jedec_id(void)
{
    return g_vision_ospi_jedec_id;
}

bool vision_sdram_is_ready(void)
{
    return g_vision_sdram_ready;
}

fsp_err_t vision_sdram_last_error(void)
{
    return g_vision_sdram_error;
}

uint32_t vision_sdram_error_offset(void)
{
    return g_vision_sdram_error_offset;
}
