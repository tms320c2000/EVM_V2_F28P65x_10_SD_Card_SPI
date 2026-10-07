//#############################################################################
//
// FILE:   uartstdio.h
//
// TITLE:  Prototypes for the UART console functions using F28P65x DriverLib SCI.
//
//#############################################################################

#ifndef __UARTSTDIO_H__
#define __UARTSTDIO_H__

#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C"
{
#endif

//
// Function Prototypes
//
extern void UARTStdioConfig(uint32_t ui32Port, uint32_t ui32Baud, uint32_t ui32SrcClock);
extern int UARTgets(char *pcBuf, uint32_t ui32Len);
extern unsigned char UARTgetc(void);
extern void UARTprintf(const char *pcString, ...);
extern void UARTvprintf(const char *pcString, va_list vaArgP);
extern int UARTwrite(const char *pcBuf, uint32_t ui32Len);

#ifdef __cplusplus
}
#endif

#endif // __UARTSTDIO_H__
