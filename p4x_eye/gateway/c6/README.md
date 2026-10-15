# ESP-Hosted C6 image

The embedded `network_adapter` image must match the P4 host transport settings.
For the production SDIO build, both sides use the software checksum:

```text
P4 host: CONFIG_ESP_HOSTED_SDIO_CHECKSUM=y
C6 slave: CONFIG_ESP_SDIO_CHECKSUM=y
```

The upstream slave project is generated under
`managed_components/espressif__esp_hosted/slave`. Apply
`sdkconfig.checksum.defaults` to its generated `sdkconfig`, build target
`esp32c6`, and copy `build_c6/network_adapter.bin` to
`main/assets/esp_hosted_c6_2_12_12.bin` before building the P4 application.
The exact binary is committed because it is part of the P4 firmware image and
is deployed through the gateway's bounded one-time C6 OTA path.
