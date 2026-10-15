/* generated configuration header file - do not edit */
#ifndef FREERTOSFATCONFIG_H_
#define FREERTOSFATCONFIG_H_
/*
 * FreeRTOS+FAT Labs Build 160919a (C) 2016 Real Time Engineers ltd.
 * Authors include James Walmsley, Hein Tibosch and Richard Barry
 *
 *******************************************************************************
 ***** NOTE ******* NOTE ******* NOTE ******* NOTE ******* NOTE ******* NOTE ***
 ***                                                                         ***
 ***                                                                         ***
 ***   FREERTOS+FAT IS STILL IN THE LAB:                                     ***
 ***                                                                         ***
 ***   Be aware we are still refining the FreeRTOS+FAT design,               ***
 ***   the source code does not yet fully conform to the strict quality and  ***
 ***   style standards mandated by Real Time Engineers ltd., and the         ***
 ***   documentation and testing is not necessarily complete.                ***
 ***                                                                         ***
 ***   PLEASE REPORT EXPERIENCES USING THE SUPPORT RESOURCES FOUND ON THE    ***
 ***   URL: http://www.FreeRTOS.org/contact  Active early adopters may, at   ***
 ***   the sole discretion of Real Time Engineers Ltd., be offered versions  ***
 ***   under a license other than that described below.                      ***
 ***                                                                         ***
 ***                                                                         ***
 ***** NOTE ******* NOTE ******* NOTE ******* NOTE ******* NOTE ******* NOTE ***
 *******************************************************************************
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * 1 tab == 4 spaces!
 *
 * http://www.FreeRTOS.org
 * http://www.FreeRTOS.org/plus
 * http://www.FreeRTOS.org/labs
 *
 */

#ifdef __cplusplus
extern "C" {
#endif

#define ffconfigBYTE_ORDER                   (pdFREERTOS_LITTLE_ENDIAN)
#ifndef ffconfigHAS_CWD
#define ffconfigHAS_CWD (0)
#endif
#ifndef ffconfigCWD_THREAD_LOCAL_INDEX
#define ffconfigCWD_THREAD_LOCAL_INDEX (0)
#endif
#ifndef ffconfigLFN_SUPPORT
#define ffconfigLFN_SUPPORT (1)
#endif
#ifndef ffconfigINCLUDE_SHORT_NAME
#define ffconfigINCLUDE_SHORT_NAME (1)
#endif
#ifndef ffconfigSHORTNAME_CASE
#define ffconfigSHORTNAME_CASE (0)
#endif
#ifndef ffconfigUNICODE_UTF16_SUPPORT
#define ffconfigUNICODE_UTF16_SUPPORT (0)
#endif
#ifndef ffconfigUNICODE_UTF8_SUPPORT
#define ffconfigUNICODE_UTF8_SUPPORT (0)
#endif
#ifndef ffconfigFAT12_SUPPORT
#define ffconfigFAT12_SUPPORT (0)
#endif
#ifndef ffconfigOPTIMISE_UNALIGNED_ACCESS
#define ffconfigOPTIMISE_UNALIGNED_ACCESS (0)
#endif
#ifndef ffconfigCACHE_WRITE_THROUGH
#define ffconfigCACHE_WRITE_THROUGH (1)
#endif
#ifndef ffconfigWRITE_BOTH_FATS
#define ffconfigWRITE_BOTH_FATS (1)
#endif
#ifndef ffconfigWRITE_FREE_COUNT
#define ffconfigWRITE_FREE_COUNT (0)
#endif
#ifndef ffconfigTIME_SUPPORT
#define ffconfigTIME_SUPPORT (0)
#endif
#ifndef ffconfigREMOVABLE_MEDIA
#define ffconfigREMOVABLE_MEDIA (0)
#endif
#ifndef ffconfigMOUNT_FIND_FREE
#define ffconfigMOUNT_FIND_FREE (0)
#endif
#ifndef ffconfigFSINFO_TRUSTED
#define ffconfigFSINFO_TRUSTED (0)
#endif
#ifndef ffconfigPATH_CACHE
#define ffconfigPATH_CACHE (1)
#endif
#ifndef ffconfigPATH_CACHE_DEPTH
#define ffconfigPATH_CACHE_DEPTH (5)
#endif
#ifndef ffconfigHASH_CACHE
#define ffconfigHASH_CACHE (0)
#endif
#ifndef ffconfigHASH_FUNCTION
#define ffconfigHASH_FUNCTION (CRC16)
#endif
#ifndef ffconfigMKDIR_RECURSIVE
#define ffconfigMKDIR_RECURSIVE (0)
#endif
#ifndef ffconfigMALLOC
 #if (BSP_CFG_RTOS == 2)
#define ffconfigMALLOC (pvPortMalloc)
 #elif (BSP_CFG_RTOS == 0)
#define ffconfigMALLOC (malloc)
 #endif
#endif
#ifndef ffconfigFREE
 #if (BSP_CFG_RTOS == 2)
#define ffconfigFREE (vPortFree)
 #elif (BSP_CFG_RTOS == 0)
#define ffconfigFREE (free)
 #endif
#endif
#ifndef ffconfig64_NUM_SUPPORT
#define ffconfig64_NUM_SUPPORT (0)
#endif
#ifndef ffconfigMAX_PARTITIONS
#define ffconfigMAX_PARTITIONS (4)
#endif
#ifndef ffconfigMAX_FILE_SYS
#define ffconfigMAX_FILE_SYS (4)
#endif
#ifndef ffconfigDRIVER_BUSY_SLEEP_MS
#define ffconfigDRIVER_BUSY_SLEEP_MS (20)
#endif
#ifndef ffconfigFPRINTF_SUPPORT
#define ffconfigFPRINTF_SUPPORT (1)
#endif
#ifndef ffconfigFPRINTF_BUFFER_LENGTH
#define ffconfigFPRINTF_BUFFER_LENGTH (512)
#endif
#ifndef ffconfigINLINE_MEMORY_ACCESS
#define ffconfigINLINE_MEMORY_ACCESS (0)
#endif
#ifndef ffconfigFAT_CHECK
#define ffconfigFAT_CHECK (0)
#endif
#ifndef ffconfigMAX_FILENAME
#define ffconfigMAX_FILENAME (129)
#endif
#ifndef ffconfigUSE_DELTREE
#define ffconfigUSE_DELTREE (0)
#endif
#ifndef FF_PRINTF
#define FF_PRINTF 
#endif

#ifdef __cplusplus
}
#endif
#endif /* FREERTOSFATCONFIG_H_ */
