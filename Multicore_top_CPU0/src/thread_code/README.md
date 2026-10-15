# M85 Thread Code

This directory stores helper modules and business logic used by M85 FreeRTOS threads.

Suggested usage:
- Put UART/ESP32 protocol code here.
- Put camera and AI pipeline helper code here.
- Put command/fusion helper code here.

Keep thread entry files in `M85/src/` thin and call into modules from this directory.
