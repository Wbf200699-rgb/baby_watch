#include <camera_thread.h>
#include <stdbool.h>
#include <string.h>

#include "thread_code/camera_usb_ipc_shared.h"
#include "thread_code/serial_camera_driver.h"

#define CAMERA_USB_DESCRIPTOR_BUFFER_BYTES (512U)
#define CAMERA_USB_CONTROL_WAIT_TICKS      (pdMS_TO_TICKS(500U))
#define CAMERA_USB_TYPEC_EN_N_PIN          BSP_IO_PORT_06_PIN_06
#define CAMERA_USB_TYPEC_EN_N_CFG          ((uint32_t) IOPORT_CFG_PORT_DIRECTION_OUTPUT | \
                                             (uint32_t) IOPORT_CFG_PORT_OUTPUT_LOW)
#define CAMERA_USB_VBUS_SETTLE_TICKS       (pdMS_TO_TICKS(300U))
#define CAMERA_USB_VBUS_ENABLE_PIN         BSP_IO_PORT_04_PIN_07
#define CAMERA_USB_VBUS_DETECT_PIN         BSP_IO_PORT_04_PIN_08

#define CAMERA_TYPEC_SDA_PIN               BSP_IO_PORT_04_PIN_09
#define CAMERA_TYPEC_SCL_PIN               BSP_IO_PORT_04_PIN_10
#define CAMERA_TYPEC_I2C_RELEASE_CFG        ((uint32_t) IOPORT_CFG_PORT_DIRECTION_INPUT | \
                                             (uint32_t) IOPORT_CFG_PULLUP_ENABLE)
#define CAMERA_TYPEC_I2C_LOW_CFG            ((uint32_t) IOPORT_CFG_PORT_DIRECTION_OUTPUT | \
                                             (uint32_t) IOPORT_CFG_PORT_OUTPUT_LOW | \
                                             (uint32_t) IOPORT_CFG_NMOS_ENABLE)
#define CAMERA_TYPEC_I2C_DELAY_US           (10U)
#define CAMERA_TYPEC_I2C_ADDRESS_HIGH       (0x31U)
#define CAMERA_TYPEC_I2C_ADDRESS_LOW        (0x21U)
#define CAMERA_TYPEC_STARTUP_TICKS          (pdMS_TO_TICKS(120U))
#define CAMERA_TYPEC_ROLE_SETTLE_TICKS      (pdMS_TO_TICKS(200U))

#define FUSB303_REG_DEVICE_ID               (0x01U)
#define FUSB303_REG_DEVICE_TYPE             (0x02U)
#define FUSB303_REG_PORTROLE                (0x03U)
#define FUSB303_REG_CONTROL1                (0x05U)
#define FUSB303_REG_STATUS                  (0x11U)
#define FUSB303_REG_TYPE                    (0x13U)
#define FUSB303_DEVICE_TYPE                 (0x01U)
#define FUSB303B_DEVICE_TYPE                (0x03U)
#define FUSB303_PORTROLE_SOURCE             (0x49U)
#define FUSB303_CONTROL1_ENABLE             (0x08U)

static uint8_t g_camera_device_descriptor[USB_DD_BLENGTH] BSP_ALIGN_VARIABLE(4);
static uint8_t g_camera_config_descriptor[CAMERA_USB_DESCRIPTOR_BUFFER_BYTES] BSP_ALIGN_VARIABLE(4);
static uint8_t g_camera_typec_i2c_address;

static void camera_typec_i2c_delay(void)
{
    R_BSP_SoftwareDelay(CAMERA_TYPEC_I2C_DELAY_US, BSP_DELAY_UNITS_MICROSECONDS);
}

static bool camera_typec_pin_release(bsp_io_port_pin_t pin)
{
    return FSP_SUCCESS == g_ioport.p_api->pinCfg(g_ioport.p_ctrl, pin, CAMERA_TYPEC_I2C_RELEASE_CFG);
}

static bool camera_typec_pin_low(bsp_io_port_pin_t pin)
{
    return FSP_SUCCESS == g_ioport.p_api->pinCfg(g_ioport.p_ctrl, pin, CAMERA_TYPEC_I2C_LOW_CFG);
}

static bool camera_typec_pin_is_high(bsp_io_port_pin_t pin)
{
    bsp_io_level_t level = BSP_IO_LEVEL_LOW;

    if (FSP_SUCCESS != g_ioport.p_api->pinRead(g_ioport.p_ctrl, pin, &level))
    {
        return false;
    }

    return BSP_IO_LEVEL_HIGH == level;
}

static bool camera_typec_scl_release(void)
{
    if (!camera_typec_pin_release(CAMERA_TYPEC_SCL_PIN))
    {
        return false;
    }

    for (uint32_t i = 0U; i < 100U; i++)
    {
        if (camera_typec_pin_is_high(CAMERA_TYPEC_SCL_PIN))
        {
            return true;
        }

        camera_typec_i2c_delay();
    }

    return false;
}

static void camera_typec_i2c_stop(void)
{
    (void) camera_typec_pin_low(CAMERA_TYPEC_SDA_PIN);
    camera_typec_i2c_delay();
    (void) camera_typec_scl_release();
    camera_typec_i2c_delay();
    (void) camera_typec_pin_release(CAMERA_TYPEC_SDA_PIN);
    camera_typec_i2c_delay();
}

static bool camera_typec_i2c_start(void)
{
    if (!camera_typec_pin_release(CAMERA_TYPEC_SDA_PIN) || !camera_typec_scl_release())
    {
        return false;
    }

    camera_typec_i2c_delay();
    if (!camera_typec_pin_low(CAMERA_TYPEC_SDA_PIN))
    {
        return false;
    }

    camera_typec_i2c_delay();
    return camera_typec_pin_low(CAMERA_TYPEC_SCL_PIN);
}

static bool camera_typec_i2c_write_byte(uint8_t data)
{
    for (uint8_t mask = 0x80U; mask != 0U; mask >>= 1U)
    {
        bool pin_ok = (0U != (data & mask)) ? camera_typec_pin_release(CAMERA_TYPEC_SDA_PIN) :
                                             camera_typec_pin_low(CAMERA_TYPEC_SDA_PIN);
        if (!pin_ok)
        {
            return false;
        }

        camera_typec_i2c_delay();
        if (!camera_typec_scl_release())
        {
            return false;
        }

        camera_typec_i2c_delay();
        if (!camera_typec_pin_low(CAMERA_TYPEC_SCL_PIN))
        {
            return false;
        }
    }

    if (!camera_typec_pin_release(CAMERA_TYPEC_SDA_PIN))
    {
        return false;
    }

    camera_typec_i2c_delay();
    if (!camera_typec_scl_release())
    {
        return false;
    }

    camera_typec_i2c_delay();
    bool acknowledged = !camera_typec_pin_is_high(CAMERA_TYPEC_SDA_PIN);
    (void) camera_typec_pin_low(CAMERA_TYPEC_SCL_PIN);
    camera_typec_i2c_delay();

    return acknowledged;
}

static bool camera_typec_i2c_read_byte(uint8_t * p_data)
{
    uint8_t data = 0U;

    if ((NULL == p_data) || !camera_typec_pin_release(CAMERA_TYPEC_SDA_PIN))
    {
        return false;
    }

    for (uint32_t i = 0U; i < 8U; i++)
    {
        data <<= 1U;
        camera_typec_i2c_delay();
        if (!camera_typec_scl_release())
        {
            return false;
        }

        camera_typec_i2c_delay();
        if (camera_typec_pin_is_high(CAMERA_TYPEC_SDA_PIN))
        {
            data |= 1U;
        }

        if (!camera_typec_pin_low(CAMERA_TYPEC_SCL_PIN))
        {
            return false;
        }
    }

    /* A single-byte register read ends with NACK. */
    (void) camera_typec_pin_release(CAMERA_TYPEC_SDA_PIN);
    camera_typec_i2c_delay();
    if (!camera_typec_scl_release())
    {
        return false;
    }

    camera_typec_i2c_delay();
    (void) camera_typec_pin_low(CAMERA_TYPEC_SCL_PIN);
    (void) camera_typec_pin_release(CAMERA_TYPEC_SDA_PIN);
    *p_data = data;

    return true;
}

static bool camera_typec_i2c_recover(void)
{
    (void) camera_typec_pin_release(CAMERA_TYPEC_SDA_PIN);

    for (uint32_t i = 0U; i < 9U; i++)
    {
        if (!camera_typec_pin_low(CAMERA_TYPEC_SCL_PIN))
        {
            return false;
        }

        camera_typec_i2c_delay();
        if (!camera_typec_scl_release())
        {
            return false;
        }

        camera_typec_i2c_delay();
    }

    camera_typec_i2c_stop();
    return camera_typec_pin_is_high(CAMERA_TYPEC_SDA_PIN) && camera_typec_pin_is_high(CAMERA_TYPEC_SCL_PIN);
}

static bool camera_typec_i2c_read_register(uint8_t address, uint8_t reg, uint8_t * p_value)
{
    bool success = camera_typec_i2c_start() &&
                   camera_typec_i2c_write_byte((uint8_t) (address << 1U)) &&
                   camera_typec_i2c_write_byte(reg) &&
                   camera_typec_i2c_start() &&
                   camera_typec_i2c_write_byte((uint8_t) (((uint32_t) address << 1U) | 1U)) &&
                   camera_typec_i2c_read_byte(p_value);

    camera_typec_i2c_stop();
    return success;
}

static bool camera_typec_i2c_write_register(uint8_t address, uint8_t reg, uint8_t value)
{
    bool success = camera_typec_i2c_start() &&
                   camera_typec_i2c_write_byte((uint8_t) (address << 1U)) &&
                   camera_typec_i2c_write_byte(reg) &&
                   camera_typec_i2c_write_byte(value);

    camera_typec_i2c_stop();
    return success;
}

static void __attribute__((unused)) camera_usb_enable_typec_source(void)
{
    volatile camera_usb_ipc_mailbox_t * p_cam = CAMERA_USB_IPC_MAILBOX;
    fsp_err_t err;

    err = g_ioport.p_api->pinCfg(g_ioport.p_ctrl, CAMERA_USB_TYPEC_EN_N_PIN, CAMERA_USB_TYPEC_EN_N_CFG);
    p_cam->typec_enable_err = (uint32_t) err;

    if (FSP_SUCCESS == err)
    {
        err = g_ioport.p_api->pinWrite(g_ioport.p_ctrl, CAMERA_USB_TYPEC_EN_N_PIN, BSP_IO_LEVEL_LOW);
    }

    p_cam->typec_write_err = (uint32_t) err;
    p_cam->typec_enabled = (FSP_SUCCESS == err) ? 1U : 0U;
}

static void __attribute__((unused)) camera_usb_sample_board_pins(void)
{
    volatile camera_usb_ipc_mailbox_t * p_cam = CAMERA_USB_IPC_MAILBOX;
    uint32_t levels = p_cam->typec_enabled & 1U;

    /* bit 0: controller enabled; bits 1..5: EN_N, VBUSEN, VBUS, SDA and SCL levels. */
    if (camera_typec_pin_is_high(CAMERA_USB_TYPEC_EN_N_PIN))
    {
        levels |= (1U << 1U);
    }
    if (camera_typec_pin_is_high(CAMERA_USB_VBUS_ENABLE_PIN))
    {
        levels |= (1U << 2U);
    }
    if (camera_typec_pin_is_high(CAMERA_USB_VBUS_DETECT_PIN))
    {
        levels |= (1U << 3U);
    }
    if (camera_typec_pin_is_high(CAMERA_TYPEC_SDA_PIN))
    {
        levels |= (1U << 4U);
    }
    if (camera_typec_pin_is_high(CAMERA_TYPEC_SCL_PIN))
    {
        levels |= (1U << 5U);
    }

    p_cam->typec_enabled = levels;
}

static bool camera_typec_refresh_status(void)
{
    volatile camera_usb_ipc_mailbox_t * p_cam = CAMERA_USB_IPC_MAILBOX;
    uint8_t portrole;
    uint8_t control1;
    uint8_t status;
    uint8_t type;

    if ((0U == g_camera_typec_i2c_address) ||
        !camera_typec_i2c_read_register(g_camera_typec_i2c_address, FUSB303_REG_PORTROLE, &portrole) ||
        !camera_typec_i2c_read_register(g_camera_typec_i2c_address, FUSB303_REG_CONTROL1, &control1) ||
        !camera_typec_i2c_read_register(g_camera_typec_i2c_address, FUSB303_REG_STATUS, &status) ||
        !camera_typec_i2c_read_register(g_camera_typec_i2c_address, FUSB303_REG_TYPE, &type))
    {
        p_cam->typec_i2c_status = CAMERA_TYPEC_I2C_BUS_ERROR;
        return false;
    }

    p_cam->typec_portrole = portrole;
    p_cam->typec_identity = (p_cam->typec_identity & 0x00FFFFFFU) | ((uint32_t) control1 << 24U);
    p_cam->typec_status = status;
    p_cam->typec_type = type;
    if (0U == (control1 & FUSB303_CONTROL1_ENABLE))
    {
        p_cam->typec_i2c_status = CAMERA_TYPEC_I2C_VERIFY_ERROR;
        return false;
    }

    p_cam->typec_i2c_status = CAMERA_TYPEC_I2C_OK;

    return true;
}

static bool __attribute__((unused)) camera_typec_configure_source(void)
{
    static uint8_t const addresses[] = {CAMERA_TYPEC_I2C_ADDRESS_HIGH, CAMERA_TYPEC_I2C_ADDRESS_LOW};
    volatile camera_usb_ipc_mailbox_t * p_cam = CAMERA_USB_IPC_MAILBOX;
    uint8_t address = 0U;
    uint8_t device_id = 0U;
    uint8_t device_type = 0U;
    uint8_t portrole = 0U;
    uint8_t control1 = 0U;
    bool device_responded = false;

    g_camera_typec_i2c_address = 0U;
    if (!camera_typec_i2c_recover())
    {
        p_cam->typec_i2c_status = CAMERA_TYPEC_I2C_BUS_ERROR;
        return false;
    }

    for (uint32_t i = 0U; i < (sizeof(addresses) / sizeof(addresses[0])); i++)
    {
        if (camera_typec_i2c_read_register(addresses[i], FUSB303_REG_DEVICE_ID, &device_id) &&
            camera_typec_i2c_read_register(addresses[i], FUSB303_REG_DEVICE_TYPE, &device_type))
        {
            device_responded = true;
            p_cam->typec_identity = ((uint32_t) addresses[i] << 16U) |
                                    ((uint32_t) device_type << 8U) |
                                    (uint32_t) device_id;
            if ((FUSB303_DEVICE_TYPE == device_type) || (FUSB303B_DEVICE_TYPE == device_type))
            {
                address = addresses[i];
                break;
            }
        }
    }

    if (0U == address)
    {
        p_cam->typec_i2c_status = device_responded ? CAMERA_TYPEC_I2C_BAD_ID : CAMERA_TYPEC_I2C_NOT_FOUND;
        return false;
    }

    if (!camera_typec_i2c_read_register(address, FUSB303_REG_CONTROL1, &control1))
    {
        p_cam->typec_i2c_status = CAMERA_TYPEC_I2C_BUS_ERROR;
        return false;
    }

    control1 |= FUSB303_CONTROL1_ENABLE;
    if (!camera_typec_i2c_write_register(address, FUSB303_REG_PORTROLE, FUSB303_PORTROLE_SOURCE) ||
        !camera_typec_i2c_write_register(address, FUSB303_REG_CONTROL1, control1))
    {
        p_cam->typec_i2c_status = CAMERA_TYPEC_I2C_WRITE_ERROR;
        return false;
    }

    g_camera_typec_i2c_address = address;
    vTaskDelay(CAMERA_TYPEC_ROLE_SETTLE_TICKS);
    if (!camera_typec_i2c_read_register(address, FUSB303_REG_PORTROLE, &portrole) ||
        !camera_typec_i2c_read_register(address, FUSB303_REG_CONTROL1, &control1))
    {
        p_cam->typec_i2c_status = CAMERA_TYPEC_I2C_BUS_ERROR;
        return false;
    }

    p_cam->typec_portrole = portrole;
    p_cam->typec_identity = (p_cam->typec_identity & 0x00FFFFFFU) | ((uint32_t) control1 << 24U);
    if ((1U != (portrole & 0x07U)) || (0U == (control1 & FUSB303_CONTROL1_ENABLE)))
    {
        p_cam->typec_i2c_status = CAMERA_TYPEC_I2C_VERIFY_ERROR;
        return false;
    }

    return camera_typec_refresh_status();
}

static void camera_usb_sample_registers(void)
{
    volatile camera_usb_ipc_mailbox_t * p_cam = CAMERA_USB_IPC_MAILBOX;
    uint32_t dpusr2r;

    p_cam->reg_syscfg = (uint32_t) R_USB_HS0->SYSCFG;
    p_cam->reg_syssts0 = (uint32_t) R_USB_HS0->SYSSTS0;
    p_cam->reg_dvstctr0 = (uint32_t) R_USB_HS0->DVSTCTR0;
    p_cam->reg_intsts0 = (uint32_t) R_USB_HS0->INTSTS0;
    p_cam->reg_dpusb0r = (uint32_t) R_USB_HS0->DPUSR0R;
    dpusr2r = (uint32_t) R_USB_HS0->DPUSR2R;
    p_cam->reg_dpusb2r = dpusr2r;
    p_cam->line_dpval = (dpusr2r & R_USB_HS0_DPUSR2R_DPVAL_Msk) ? 1U : 0U;
    p_cam->line_dmval = (dpusr2r & R_USB_HS0_DPUSR2R_DMVAL_Msk) ? 1U : 0U;
}

static uint16_t camera_usb_u16(uint8_t const * p_data)
{
    return (uint16_t) (p_data[0] | ((uint16_t) p_data[1] << 8));
}

static bool camera_usb_control_read(uint8_t device_address,
                                    uint16_t request_type,
                                    uint16_t request_value,
                                    uint16_t request_index,
                                    uint8_t * p_buffer,
                                    uint16_t length)
{
    volatile camera_usb_ipc_mailbox_t * p_cam = CAMERA_USB_IPC_MAILBOX;
    usb_setup_t setup;
    uint32_t request_count = p_cam->request_complete_count;
    TickType_t start_tick = xTaskGetTickCount();
    fsp_err_t err;

    setup.request_type = request_type;
    setup.request_value = request_value;
    setup.request_index = request_index;
    setup.request_length = length;

    p_cam->status = CAMERA_USB_IPC_STATUS_DESC_REQUEST;
    p_cam->sequence++;

    err = g_basic0.p_api->hostControlTransfer(g_basic0.p_ctrl, &setup, p_buffer, device_address);
    p_cam->last_fsp_err = (uint32_t) err;
    if (FSP_SUCCESS != err)
    {
        p_cam->descriptor_error_count++;
        p_cam->status = CAMERA_USB_IPC_STATUS_DESC_ERROR;
        p_cam->sequence++;
        return false;
    }

    while ((xTaskGetTickCount() - start_tick) < CAMERA_USB_CONTROL_WAIT_TICKS)
    {
        if (p_cam->request_complete_count != request_count)
        {
            return true;
        }

        vTaskDelay(pdMS_TO_TICKS(5U));
    }

    p_cam->descriptor_error_count++;
    p_cam->status = CAMERA_USB_IPC_STATUS_DESC_ERROR;
    p_cam->sequence++;
    return false;
}

static void camera_usb_parse_config_descriptor(uint8_t const * p_desc, uint32_t length)
{
    volatile camera_usb_ipc_mailbox_t * p_cam = CAMERA_USB_IPC_MAILBOX;
    uint32_t offset = 0U;
    uint8_t current_interface_class = 0U;
    uint8_t current_interface_subclass = 0U;

    p_cam->video_interface_count = 0U;
    p_cam->video_control_interface = 0xFFU;
    p_cam->video_stream_interface = 0xFFU;
    p_cam->mjpeg_format_count = 0U;
    p_cam->yuy2_format_count = 0U;
    p_cam->frame_count = 0U;
    p_cam->first_width = 0U;
    p_cam->first_height = 0U;
    p_cam->stream_endpoint = 0U;
    p_cam->stream_endpoint_attr = 0U;
    p_cam->stream_endpoint_packet = 0U;

    if (length >= USB_CD_BLENGTH)
    {
        p_cam->config_total_length = camera_usb_u16(&p_desc[2]);
        p_cam->config_interface_count = p_desc[4];
    }

    while ((offset + 2U) <= length)
    {
        uint8_t desc_len = p_desc[offset];
        uint8_t desc_type = p_desc[offset + 1U];

        if ((desc_len < 2U) || ((offset + desc_len) > length))
        {
            break;
        }

        if ((USB_DT_INTERFACE == desc_type) && (desc_len >= USB_ID_BLENGTH))
        {
            current_interface_class = p_desc[offset + 5U];
            current_interface_subclass = p_desc[offset + 6U];
            if (USB_IFCLS_VID == current_interface_class)
            {
                p_cam->video_interface_count++;
                if (1U == current_interface_subclass)
                {
                    p_cam->video_control_interface = p_desc[offset + 2U];
                }
                else if (2U == current_interface_subclass)
                {
                    p_cam->video_stream_interface = p_desc[offset + 2U];
                }
            }
        }
        else if ((USB_DT_ENDPOINT == desc_type) && (desc_len >= USB_ED_BLENGTH))
        {
            if ((USB_IFCLS_VID == current_interface_class) && (2U == current_interface_subclass) &&
                (0U != (p_desc[offset + 2U] & USB_EP_IN)))
            {
                p_cam->stream_endpoint = p_desc[offset + 2U];
                p_cam->stream_endpoint_attr = p_desc[offset + 3U];
                p_cam->stream_endpoint_packet = camera_usb_u16(&p_desc[offset + 4U]);
            }
        }
        else if ((0x24U == desc_type) && (desc_len >= 4U) &&
                 (USB_IFCLS_VID == current_interface_class) && (2U == current_interface_subclass))
        {
            uint8_t subtype = p_desc[offset + 2U];

            if (0x06U == subtype)
            {
                p_cam->mjpeg_format_count++;
            }
            else if (0x04U == subtype)
            {
                p_cam->yuy2_format_count++;
            }
            else if (((0x07U == subtype) || (0x05U == subtype)) && (desc_len >= 9U))
            {
                p_cam->frame_count++;
                if (0U == p_cam->first_width)
                {
                    p_cam->first_width = camera_usb_u16(&p_desc[offset + 5U]);
                    p_cam->first_height = camera_usb_u16(&p_desc[offset + 7U]);
                }
            }
        }

        offset += desc_len;
    }
}

static void __attribute__((unused)) camera_usb_read_descriptors(uint8_t device_address)
{
    volatile camera_usb_ipc_mailbox_t * p_cam = CAMERA_USB_IPC_MAILBOX;
    uint16_t total_length;

    memset(g_camera_device_descriptor, 0, sizeof(g_camera_device_descriptor));
    memset(g_camera_config_descriptor, 0, sizeof(g_camera_config_descriptor));

    if (!camera_usb_control_read(device_address,
                                 USB_GET_DESCRIPTOR | USB_DEV_TO_HOST | USB_STANDARD | USB_DEVICE,
                                 (uint16_t) (USB_DT_DEVICE << 8),
                                 0U,
                                 g_camera_device_descriptor,
                                 sizeof(g_camera_device_descriptor)))
    {
        return;
    }

    p_cam->device_bcd_usb = camera_usb_u16(&g_camera_device_descriptor[2]);
    p_cam->device_class = g_camera_device_descriptor[4];
    p_cam->device_subclass = g_camera_device_descriptor[5];
    p_cam->device_protocol = g_camera_device_descriptor[6];
    p_cam->vendor_id = camera_usb_u16(&g_camera_device_descriptor[8]);
    p_cam->product_id = camera_usb_u16(&g_camera_device_descriptor[10]);

    if (!camera_usb_control_read(device_address,
                                 USB_GET_DESCRIPTOR | USB_DEV_TO_HOST | USB_STANDARD | USB_DEVICE,
                                 (uint16_t) (USB_DT_CONFIGURATION << 8),
                                 0U,
                                 g_camera_config_descriptor,
                                 USB_CD_BLENGTH))
    {
        return;
    }

    total_length = camera_usb_u16(&g_camera_config_descriptor[2]);
    if (total_length > CAMERA_USB_DESCRIPTOR_BUFFER_BYTES)
    {
        total_length = CAMERA_USB_DESCRIPTOR_BUFFER_BYTES;
    }

    if (!camera_usb_control_read(device_address,
                                 USB_GET_DESCRIPTOR | USB_DEV_TO_HOST | USB_STANDARD | USB_DEVICE,
                                 (uint16_t) (USB_DT_CONFIGURATION << 8),
                                 0U,
                                 g_camera_config_descriptor,
                                 total_length))
    {
        return;
    }

    camera_usb_parse_config_descriptor(g_camera_config_descriptor, total_length);
    p_cam->descriptor_read_count++;
    p_cam->status = (p_cam->video_interface_count > 0U) ? CAMERA_USB_IPC_STATUS_UVC_READY :
                    CAMERA_USB_IPC_STATUS_DESC_READY;
    p_cam->sequence++;
}

static void camera_usb_ipc_set_status_from_event(volatile camera_usb_ipc_mailbox_t * p_cam,
                                                 usb_status_t event)
{
    switch (event)
    {
        case USB_STATUS_POWERED:
            p_cam->status = CAMERA_USB_IPC_STATUS_POWERED;
            p_cam->powered_count++;
            break;

        case USB_STATUS_DEFAULT:
            p_cam->status = CAMERA_USB_IPC_STATUS_DEFAULT;
            p_cam->default_count++;
            break;

        case USB_STATUS_ADDRESS:
            p_cam->status = CAMERA_USB_IPC_STATUS_ADDRESS;
            p_cam->address_count++;
            break;

        case USB_STATUS_CONFIGURED:
            p_cam->status = CAMERA_USB_IPC_STATUS_CONFIGURED;
            p_cam->configured_count++;
            break;

        case USB_STATUS_SUSPEND:
            p_cam->status = CAMERA_USB_IPC_STATUS_SUSPEND;
            break;

        case USB_STATUS_RESUME:
            p_cam->status = CAMERA_USB_IPC_STATUS_RESUME;
            break;

        case USB_STATUS_DETACH:
            p_cam->status = CAMERA_USB_IPC_STATUS_DETACH;
            p_cam->detach_count++;
            break;

        case USB_STATUS_OVERCURRENT:
            p_cam->status = CAMERA_USB_IPC_STATUS_OVERCURRENT;
            p_cam->overcurrent_count++;
            break;

        case USB_STATUS_NOT_SUPPORT:
            p_cam->status = CAMERA_USB_IPC_STATUS_NOT_SUPPORT;
            p_cam->not_support_count++;
            break;

        default:
            p_cam->status = CAMERA_USB_IPC_STATUS_EVENT;
            break;
    }
}

void usb_basic_callback(usb_event_info_t * p_event_info, usb_hdl_t cur_task, usb_onoff_t usb_state)
{
    volatile camera_usb_ipc_mailbox_t * p_cam = CAMERA_USB_IPC_MAILBOX;

    FSP_PARAMETER_NOT_USED(cur_task);

    p_cam->magic = CAMERA_USB_IPC_MAGIC;
    camera_usb_sample_registers();
    p_cam->callback_count++;
    p_cam->sequence++;
    p_cam->last_usb_state = (uint32_t) usb_state;
    p_cam->last_event_tick = (uint32_t) xTaskGetTickCount();

    if (NULL != p_event_info)
    {
        p_cam->last_event = (uint32_t) p_event_info->event;
        p_cam->last_usb_status = (uint32_t) p_event_info->status;
        p_cam->last_module = (uint32_t) p_event_info->module_number;
        p_cam->last_address = (uint32_t) p_event_info->device_address;
        p_cam->last_pipe = (uint32_t) p_event_info->pipe;
        p_cam->last_type = (uint32_t) p_event_info->type;
        p_cam->last_data_size = p_event_info->data_size;
        if (USB_STATUS_REQUEST_COMPLETE == p_event_info->event)
        {
            p_cam->request_complete_count++;
        }
        camera_usb_ipc_set_status_from_event(p_cam, p_event_info->event);
    }
}

/* New Thread entry function */
/* pvParameters contains TaskHandle_t */
void camera_thread_entry(void *pvParameters)
{
    FSP_PARAMETER_NOT_USED (pvParameters);

    /* The product now uses the TTL VC0703 camera on SCI3. USB host support is
     * retained above for reference, but is intentionally not opened here. */
    (void) serial_camera_driver_init();
    vTaskDelay(pdMS_TO_TICKS(1000U));

    while (1)
    {
        serial_camera_poll();
        vTaskDelay(pdMS_TO_TICKS(5U));
    }

#if 0
    volatile camera_usb_ipc_mailbox_t * p_cam = CAMERA_USB_IPC_MAILBOX;
    fsp_err_t err;

    memset((void *) p_cam, 0, sizeof(*p_cam));
    p_cam->magic = CAMERA_USB_IPC_MAGIC;
    p_cam->status = CAMERA_USB_IPC_STATUS_INIT;
    camera_usb_sample_registers();
    p_cam->sequence++;

    camera_usb_enable_typec_source();
    vTaskDelay(CAMERA_TYPEC_STARTUP_TICKS);
    (void) camera_typec_configure_source();
    camera_usb_sample_board_pins();
    camera_usb_sample_registers();
    p_cam->sequence++;

    p_cam->vbus_enabled = 0U;

    err = g_basic0.p_api->open(g_basic0.p_ctrl, g_basic0.p_cfg);
    p_cam->last_fsp_err = (uint32_t) err;
    p_cam->open_tick = (uint32_t) xTaskGetTickCount();
    p_cam->open_count++;
    camera_usb_sample_registers();
    p_cam->sequence++;

    if (FSP_SUCCESS == err)
    {
        p_cam->status = CAMERA_USB_IPC_STATUS_OPENED;
    }
    else
    {
        p_cam->status = CAMERA_USB_IPC_STATUS_OPEN_ERROR;
    }

    if (FSP_SUCCESS == err)
    {
        err = g_basic0.p_api->vbusSet(g_basic0.p_ctrl, USB_ON);
        p_cam->vbus_set_err = (uint32_t) err;
        p_cam->vbus_enabled = (FSP_SUCCESS == err) ? 1U : 0U;
        if (FSP_SUCCESS == err)
        {
            vTaskDelay(CAMERA_USB_VBUS_SETTLE_TICKS);
        }
        (void) camera_typec_refresh_status();
        camera_usb_sample_board_pins();
        camera_usb_sample_registers();
    }

    while (1)
    {
        if (CAMERA_TYPEC_I2C_OK == p_cam->typec_i2c_status)
        {
            (void) camera_typec_refresh_status();
        }
        else if (0U == (p_cam->heartbeat % 5U))
        {
            camera_usb_enable_typec_source();
            vTaskDelay(CAMERA_TYPEC_STARTUP_TICKS);
            (void) camera_typec_configure_source();
        }

        camera_usb_sample_board_pins();
        camera_usb_sample_registers();

        if ((0U != p_cam->last_address) &&
            (p_cam->descriptor_read_count == 0U) &&
            ((p_cam->configured_count > 0U) || (p_cam->address_count > 0U)))
        {
            camera_usb_read_descriptors((uint8_t) p_cam->last_address);
        }

        p_cam->heartbeat++;
        p_cam->sequence++;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
#endif
}
