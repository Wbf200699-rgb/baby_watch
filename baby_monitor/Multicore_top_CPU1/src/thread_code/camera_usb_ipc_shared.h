#ifndef CAMERA_USB_IPC_SHARED_H
#define CAMERA_USB_IPC_SHARED_H

#include <stdint.h>

#define CAMERA_USB_IPC_MAGIC   (0x43414D55U) /* CAMU */
#define CAMERA_USB_IPC_ADDRESS (0x221CFF00UL)

typedef enum e_camera_usb_ipc_status
{
    CAMERA_USB_IPC_STATUS_EMPTY = 0,
    CAMERA_USB_IPC_STATUS_INIT,
    CAMERA_USB_IPC_STATUS_OPENED,
    CAMERA_USB_IPC_STATUS_OPEN_ERROR,
    CAMERA_USB_IPC_STATUS_POWERED,
    CAMERA_USB_IPC_STATUS_DEFAULT,
    CAMERA_USB_IPC_STATUS_ADDRESS,
    CAMERA_USB_IPC_STATUS_CONFIGURED,
    CAMERA_USB_IPC_STATUS_SUSPEND,
    CAMERA_USB_IPC_STATUS_RESUME,
    CAMERA_USB_IPC_STATUS_DETACH,
    CAMERA_USB_IPC_STATUS_OVERCURRENT,
    CAMERA_USB_IPC_STATUS_NOT_SUPPORT,
    CAMERA_USB_IPC_STATUS_EVENT,
    CAMERA_USB_IPC_STATUS_DESC_REQUEST,
    CAMERA_USB_IPC_STATUS_DESC_READY,
    CAMERA_USB_IPC_STATUS_DESC_ERROR,
    CAMERA_USB_IPC_STATUS_UVC_READY
} camera_usb_ipc_status_t;

typedef enum e_camera_typec_i2c_status
{
    CAMERA_TYPEC_I2C_NOT_RUN = 0,
    CAMERA_TYPEC_I2C_OK,
    CAMERA_TYPEC_I2C_BUS_ERROR,
    CAMERA_TYPEC_I2C_NOT_FOUND,
    CAMERA_TYPEC_I2C_BAD_ID,
    CAMERA_TYPEC_I2C_WRITE_ERROR,
    CAMERA_TYPEC_I2C_VERIFY_ERROR
} camera_typec_i2c_status_t;

typedef struct st_camera_usb_ipc_mailbox
{
    uint32_t magic;
    volatile uint32_t sequence;
    volatile uint32_t status;
    volatile uint32_t heartbeat;
    volatile uint32_t open_count;
    volatile uint32_t callback_count;
    volatile uint32_t powered_count;
    volatile uint32_t default_count;
    volatile uint32_t address_count;
    volatile uint32_t configured_count;
    volatile uint32_t detach_count;
    volatile uint32_t not_support_count;
    volatile uint32_t overcurrent_count;
    volatile uint32_t last_fsp_err;
    volatile uint32_t last_event;
    volatile uint32_t last_usb_status;
    volatile uint32_t last_usb_state;
    volatile uint32_t last_module;
    volatile uint32_t last_address;
    volatile uint32_t last_pipe;
    volatile uint32_t last_type;
    volatile uint32_t last_data_size;
    volatile uint32_t open_tick;
    volatile uint32_t last_event_tick;
    volatile uint32_t vbus_enabled;
    volatile uint32_t request_complete_count;
    volatile uint32_t descriptor_read_count;
    volatile uint32_t descriptor_error_count;
    volatile uint32_t device_bcd_usb;
    volatile uint32_t device_class;
    volatile uint32_t device_subclass;
    volatile uint32_t device_protocol;
    volatile uint32_t vendor_id;
    volatile uint32_t product_id;
    volatile uint32_t config_total_length;
    volatile uint32_t config_interface_count;
    volatile uint32_t video_interface_count;
    volatile uint32_t video_control_interface;
    volatile uint32_t video_stream_interface;
    volatile uint32_t mjpeg_format_count;
    volatile uint32_t yuy2_format_count;
    volatile uint32_t frame_count;
    volatile uint32_t first_width;
    volatile uint32_t first_height;
    volatile uint32_t stream_endpoint;
    volatile uint32_t stream_endpoint_attr;
    volatile uint32_t stream_endpoint_packet;
    volatile uint32_t reg_syscfg;
    volatile uint32_t reg_syssts0;
    volatile uint32_t reg_dvstctr0;
    volatile uint32_t reg_intsts0;
    volatile uint32_t reg_dpusb0r;
    volatile uint32_t reg_dpusb2r;
    volatile uint32_t line_dpval;
    volatile uint32_t line_dmval;
    volatile uint32_t typec_enable_err;
    volatile uint32_t typec_write_err;
    volatile uint32_t typec_enabled;
    volatile uint32_t vbus_set_err;
    volatile uint32_t typec_i2c_status;
    volatile uint32_t typec_identity;
    volatile uint32_t typec_portrole;
    volatile uint32_t typec_status;
    volatile uint32_t typec_type;
} camera_usb_ipc_mailbox_t;

#define CAMERA_USB_IPC_MAILBOX ((volatile camera_usb_ipc_mailbox_t *) CAMERA_USB_IPC_ADDRESS)

#endif /* CAMERA_USB_IPC_SHARED_H */
