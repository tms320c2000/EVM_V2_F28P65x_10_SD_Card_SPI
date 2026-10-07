//#############################################################################
//
// FILE:   uartstdio.c  (Bitfield 버전)
//
// TITLE:  UART console (printf/gets) for TMS320F28P65x - SCI-A Bit-Field registers.
//
// DriverLib 버전과 API가 같은 대화형 콘솔입니다(UARTStdioConfig/UARTprintf/UARTgets ...).
// SCI-A 레지스터(SciaRegs)를 직접 조작하며, 송수신 모두 FIFO를 쓰는 폴링 방식입니다.
//
//#############################################################################

#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>

#include "f28x_project.h"
#include "ustdlib.h"
#include "uartstdio.h"

//
// Internal print buffer size
//
#define UART_PRINT_BUF_SIZE     256

#define SCI_TX_FIFO_DEPTH       16U
#define SCIRXST_RXERROR         0x0080U     // SCIRXST의 RXERROR(프레임/오버런/패리티/브레이크 중 하나)

//*****************************************************************************
//
// UARTStdioConfig - SCI-A를 8N1로 설정하고 FIFO를 켠다. (ui32Port는 SCI-A만 지원)
//
//*****************************************************************************
void UARTStdioConfig(uint32_t ui32PortNum, uint32_t ui32Baud, uint32_t ui32SrcClock)
{
    uint32_t brr;

    (void)ui32PortNum;

    // BRR = LSPCLK / (baud * 8) - 1
    brr = (ui32SrcClock / (ui32Baud * 8UL)) - 1UL;

    SciaRegs.SCICCR.all = 0x0007U;              // SCICHAR=7(8비트), STOPBITS=0, PARITYENA=0
    SciaRegs.SCICTL1.all = 0x0003U;             // RXENA=1, TXENA=1 (SWRESET=0 상태로 설정)
    SciaRegs.SCIHBAUD.all = (brr >> 8) & 0xFFU;
    SciaRegs.SCILBAUD.all = brr & 0xFFU;
    SciaRegs.SCIFFTX.all = 0xC000U;             // SCIRST=1, SCIFFENA=1 (TX FIFO는 리셋 유지)
    SciaRegs.SCIFFRX.all = 0x0000U;             // RX FIFO는 리셋 유지
    SciaRegs.SCIFFCT.all = 0x0000U;
    SciaRegs.SCIFFTX.bit.TXFIFORESET = 1U;      // TX FIFO 동작 시작
    SciaRegs.SCIFFRX.bit.RXFIFORESET = 1U;      // RX FIFO 동작 시작
    SciaRegs.SCICTL1.bit.SWRESET = 1U;          // SCI 동작 시작
}

//*****************************************************************************
//
// 한 문자를 보낸다. TX FIFO에 자리가 날 때까지 기다린다.
//
//*****************************************************************************
static void putChar(char c)
{
    while(SciaRegs.SCIFFTX.bit.TXFFST >= SCI_TX_FIFO_DEPTH)
    {
    }
    SciaRegs.SCITXBUF.all = (uint16_t)c;
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
            putChar('\r');
        }

        putChar(pcBuf[i]);
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
    for(;;)
    {
        if(SciaRegs.SCIFFRX.bit.RXFFST != 0U)
        {
            uint16_t status = SciaRegs.SCIRXST.all;
            unsigned char c = (unsigned char)(SciaRegs.SCIRXBUF.all & 0xFFU);

            if(status & SCIRXST_RXERROR)
            {
                // 수신 오류: SCI를 재동기하고 이 문자는 버린다
                SciaRegs.SCICTL1.bit.SWRESET = 0U;
                SciaRegs.SCICTL1.bit.SWRESET = 1U;
                continue;
            }
            return c;
        }
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
            putChar(c); // Echo
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
