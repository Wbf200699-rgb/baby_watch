# M33 Thread Code

This directory stores helper modules and business logic used by M33 FreeRTOS threads.

Suggested usage:
- Put sensor handling code here.
- Put SD/FatFS helper code here.
- Put IPC helper code for M33 here.

Keep thread entry files in `M33/src/` thin and call into modules from this directory.
