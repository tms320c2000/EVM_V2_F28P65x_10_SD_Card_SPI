//#############################################################################
//
// FILE:   uartstdio.c
//
// TITLE:  Driver to provide simple UART console functions using F28P65x DriverLib SCI.
//
//#############################################################################

#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>

#include "driverlib.h"
#include "device.h"
#include "FreeRTOS.h"
#include "task.h"
#include "ustdlib.h"
#include "uartstdio.h"

//
// Base addresses of supported SCI modules
//
static const uint32_t g_ui32UARTBase[2] = {
    SCIA_BASE, SCIB_BASE
};

static uint32_t g_ui32Base = SCIA_BASE;

//
// Internal print buffer size
//
#define UART_PRINT_BUF_SIZE     256

//*****************************************************************************
//
// UARTStdioConfig - Configures the UART console.
//
//*****************************************************************************
void UARTStdioConfig(uint32_t ui32PortNum, uint32_t ui32Baud, uint32_t ui32SrcClock)
{
    if(ui32PortNum < 2)
    {
        g_ui32Base = g_ui32UARTBase[ui32PortNum];
    }
    else
    {
        g_ui32Base = SCIA_BASE;
    }

    //
    // Initialize SCI FIFO and baud settings
    //
    SCI_performSoftwareReset(g_ui32Base);

    SCI_setConfig(g_ui32Base, ui32SrcClock, ui32Baud,
                  (SCI_CONFIG_WLEN_8 | SCI_CONFIG_STOP_ONE | SCI_CONFIG_PAR_NONE));

    SCI_resetChannels(g_ui32Base);
    SCI_resetRxFIFO(g_ui32Base);
    SCI_resetTxFIFO(g_ui32Base);
    SCI_clearInterruptStatus(g_ui32Base, SCI_INT_RXFF | SCI_INT_TXFF);
    SCI_enableFIFO(g_ui32Base);
    SCI_enableModule(g_ui32Base);
    SCI_performSoftwareReset(g_ui32Base);
}

//*****************************************************************************
//
// UARTwrite - Writes a buffer to the UART, expanding \n to \r\n.
//
//*****************************************************************************
int UARTwrite(const char *pcBuf, uint32_t ui32Len)
{
    uint32_t i;

    for(i = 0; i < ui32Len; i++)
    {
        if(pcBuf[i] == '\0')
        {
            break;
        }

        if(pcBuf[i] == '\n')
        {
            SCI_writeCharBlockingFIFO(g_ui32Base, '\r');
        }

        SCI_writeCharBlockingFIFO(g_ui32Base, pcBuf[i]);
    }

    return (int)i;
}

//*****************************************************************************
//
// UARTgetc - Reads a single character from UART (blocking).
//
//*****************************************************************************
unsigned char UARTgetc(void)
{
    //
    // FreeRTOS 버전: 입력이 없으면 1ms씩 CPU를 양보한다(SCI_readCharBlockingFIFO는 CPU를 붙잡고 기다림).
    // 115200bps에서 1ms 동안 들어오는 문자는 약 11개라 16단 RX FIFO가 넘치지 않는다.
    //
    for(;;)
    {
        if(SCI_getRxFIFOStatus(g_ui32Base) != SCI_FIFO_RX0)
        {
            uint16_t status = SCI_getRxStatus(g_ui32Base);
            unsigned char c = (unsigned char)(SCI_readCharNonBlocking(g_ui32Base) & 0xFF);

            if(status & SCI_RXSTATUS_ERROR)
            {
                SCI_performSoftwareReset(g_ui32Base);   // 수신 오류(프레임/오버런/패리티): SCI를 재동기하고 이 문자는 버린다
                continue;
            }
            return c;
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

//*****************************************************************************
//
// UARTgets - Reads a line from UART with basic editing and echo back.
//
//*****************************************************************************
int UARTgets(char *pcBuf, uint32_t ui32Len)
{
    uint32_t count = 0;
    char c;

    if(ui32Len == 0)
    {
        return 0;
    }

    while(count < (ui32Len - 1))
    {
        c = (char)UARTgetc();

        // Handle Return / Newline
        if(c == '\r' || c == '\n')
        {
            UARTwrite("\r\n", 2);
            break;
        }
        // Handle Backspace (BS: 0x08, DEL: 0x7F)
        else if(c == '\b' || c == 0x7F)
        {
            if(count > 0)
            {
                count--;
                UARTwrite("\b \b", 3);
            }
        }
        // Printable characters
        else if(c >= ' ' && c <= '~')
        {
            pcBuf[count++] = c;
            SCI_writeCharBlockingFIFO(g_ui32Base, c); // Echo
        }
    }

    pcBuf[count] = '\0';
    return (int)count;
}

//*****************************************************************************
//
// UARTvprintf - Formatted print to UART using va_list.
//
//*****************************************************************************
void UARTvprintf(const char *pcString, va_list vaArgP)
{
    char buf[UART_PRINT_BUF_SIZE];
    int len;

    len = uvsnprintf(buf, sizeof(buf), pcString, vaArgP);
    if(len > 0)
    {
        UARTwrite(buf, (uint32_t)len);
    }
}

//*****************************************************************************
//
// UARTprintf - Formatted print to UART.
//
//*****************************************************************************
void UARTprintf(const char *pcString, ...)
{
    va_list vaArgP;

    va_start(vaArgP, pcString);
    UARTvprintf(pcString, vaArgP);
    va_end(vaArgP);
}

// End of file.
