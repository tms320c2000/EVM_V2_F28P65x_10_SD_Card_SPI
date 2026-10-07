//#############################################################################
//
// FILE:   10_SD_Card_SPI_Driverlib.c
//
// TITLE:  TMS320F28P65x DriverLib용 Micro SD 카드(SPI) 대화형 콘솔
//
// 대상 하드웨어: SyncWorks TMS320F28X EVM V2 + TMS320F28P650DK9 / F28P659DK8-Q1
//
// 하드웨어 연결:
//   1) (10)번 영역 Micro SD 카드(SPI) <-> B-Side CN9001
//      - DI  (MOSI): B-Side 45번 핀 (GPIO50, SPIC_PICO)
//      - DO  (MISO): B-Side 43번 핀 (GPIO51, SPIC_POCI)
//      - CLK (SCLK): B-Side 41번 핀 (GPIO52, SPIC_CLK)
//      - CS  (SS)  : B-Side 39번 핀 (GPIO53, GPIO 출력, Active Low)
//   2) (11)번 영역 2Ch SCI-to-USB <-> A-Side CN9000
//      - RX1 : A-Side 91번 핀 (GPIO13, SCIA_RX)
//      - TX1 : A-Side 89번 핀 (GPIO12, SCIA_TX)
//
// 터미널: 115200 baud, 8N1
//
//#############################################################################

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "driverlib.h"
#include "device.h"
#include "ff.h"
#include "diskio.h"
#include "cmdline.h"
#include "uartstdio.h"
#include "ustdlib.h"

//
// 정의
//
#define PATH_BUF_SIZE               80
#define CMD_BUF_SIZE                64

#define NUM_LIST_STRINGS            32
#define MAX_FILENAME_STRING_LEN     32

#define OPENDIR_ERROR               1
#define NAME_TOO_LONG_ERROR         2

//
// 파일 시스템과 CLI용 전역 변수
//
static char g_cCwdBuf[PATH_BUF_SIZE] = "/";
static char g_cTmpBuf[PATH_BUF_SIZE];
static char g_cCmdBuf[CMD_BUF_SIZE];

static FATFS g_sFatFs;
static DIR g_sDirObject;
static FILINFO g_sFileInfo;
static FIL g_sFileObject;

//
// FatFs 결과 코드 문자열 매핑
//
typedef struct
{
    FRESULT fresult;
    const char *pcResultStr;
} tFresultString;

#define FRESULT_ENTRY(f) { (f), (#f) }

static const tFresultString g_sFresultStrings[] =
{
    FRESULT_ENTRY(FR_OK),
    FRESULT_ENTRY(FR_NOT_READY),
    FRESULT_ENTRY(FR_NO_FILE),
    FRESULT_ENTRY(FR_NO_PATH),
    FRESULT_ENTRY(FR_INVALID_NAME),
    FRESULT_ENTRY(FR_INVALID_DRIVE),
    FRESULT_ENTRY(FR_DENIED),
    FRESULT_ENTRY(FR_EXIST),
    FRESULT_ENTRY(FR_RW_ERROR),
    FRESULT_ENTRY(FR_WRITE_PROTECTED),
    FRESULT_ENTRY(FR_NOT_ENABLED),
    FRESULT_ENTRY(FR_NO_FILESYSTEM),
    FRESULT_ENTRY(FR_INVALID_OBJECT),
    FRESULT_ENTRY(FR_MKFS_ABORTED)
};

#define NUM_FRESULT_CODES (sizeof(g_sFresultStrings) / sizeof(tFresultString))

//
// 전방 선언
//
static const char *StringFromFresult(FRESULT fresult);
__interrupt void SysTickHandler(void);
void ConfigureUART(void);

int Cmd_help(int argc, char *argv[]);
int Cmd_ls(int argc, char *argv[]);
int Cmd_cd(int argc, char *argv[]);
int Cmd_pwd(int argc, char *argv[]);
int Cmd_cat(int argc, char *argv[]);
int Cmd_write(int argc, char *argv[]);
int Cmd_mkdir(int argc, char *argv[]);
int Cmd_rm(int argc, char *argv[]);

//
// cmdline.c용 명령어 표
//
tCmdLineEntry g_psCmdTable[] =
{
    { "help",   Cmd_help,   " : Display list of commands" },
    { "h",      Cmd_help,   "    : alias for help" },
    { "?",      Cmd_help,   "    : alias for help" },
    { "ls",     Cmd_ls,     "   : Display list of files in current directory" },
    { "chdir",  Cmd_cd,     ": Change directory" },
    { "cd",     Cmd_cd,     "   : alias for chdir" },
    { "pwd",    Cmd_pwd,    "  : Show current working directory" },
    { "cat",    Cmd_cat,    "  : Show contents of a text file" },
    { "write",  Cmd_write,  ": Write text to a file (creates if not existing)" },
    { "mkdir",  Cmd_mkdir,  ": Make a new directory" },
    { "rm",     Cmd_rm,     "   : Remove a file or empty directory" },
    { 0, 0, 0 }
};

//*****************************************************************************
// StringFromFresult - FRESULT 코드를 문자열로 변환
//*****************************************************************************
static const char *StringFromFresult(FRESULT fresult)
{
    uint32_t i;

    for(i = 0; i < NUM_FRESULT_CODES; i++)
    {
        if(g_sFresultStrings[i].fresult == fresult)
        {
            return g_sFresultStrings[i].pcResultStr;
        }
    }

    return "UNKNOWN ERROR CODE";
}

//*****************************************************************************
// SysTickHandler - FatFs 타임아웃 감소용 100Hz(10ms) 타이머 ISR
//*****************************************************************************
__interrupt void SysTickHandler(void)
{
    disk_timerproc();
    CPUTimer_clearOverflowFlag(CPUTIMER0_BASE);
    Interrupt_clearACKGroup(INTERRUPT_ACK_GROUP1);
}

//*****************************************************************************
// ConfigureUART - SCI-A(GPIO13 RX, GPIO12 TX)를 115200 8N1로 설정
//*****************************************************************************
void ConfigureUART(void)
{
    GPIO_setPinConfig(GPIO_13_SCIA_RX);
    GPIO_setPinConfig(GPIO_12_SCIA_TX);

    GPIO_setPadConfig(13, GPIO_PIN_TYPE_STD);
    GPIO_setPadConfig(12, GPIO_PIN_TYPE_STD);

    GPIO_setQualificationMode(13, GPIO_QUAL_ASYNC);
    GPIO_setQualificationMode(12, GPIO_QUAL_ASYNC);

    SysCtl_enablePeripheral(SYSCTL_PERIPH_CLK_SCIA);

    UARTStdioConfig(0, 115200, DEVICE_LSPCLK_FREQ);
}

//*****************************************************************************
// Cmd_help - 명령어 목록 표시
//*****************************************************************************
int Cmd_help(int argc, char *argv[])
{
    tCmdLineEntry *pEntry = &g_psCmdTable[0];

    UARTprintf("\nAvailable Commands:\n");
    UARTprintf("------------------------------------------------------\n");

    while(pEntry->pcCmd)
    {
        // 이 경량 printf는 '-' 플래그를 지원하지 않는다(ERROR로 출력됨). 정렬은 도움말 문자열의 공백이 맡는다.
        UARTprintf("  %s%s\n", pEntry->pcCmd, pEntry->pcHelp);
        pEntry++;
    }
    UARTprintf("------------------------------------------------------\n");

    return 0;
}

//*****************************************************************************
// Cmd_ls - 디렉터리 내용 나열
//*****************************************************************************
int Cmd_ls(int argc, char *argv[])
{
    uint32_t ulTotalSize = 0, ulFileCount = 0, ulDirCount = 0;
    FRESULT fresult;
    FATFS *pFatFs;

    fresult = f_opendir(&g_sDirObject, g_cCwdBuf);
    if(fresult != FR_OK)
    {
        return (int)fresult;
    }

    UARTprintf("\n Directory of %s\n\n", g_cCwdBuf);

    for(;;)
    {
        fresult = f_readdir(&g_sDirObject, &g_sFileInfo);
        if(fresult != FR_OK)
        {
            return (int)fresult;
        }

        if(!g_sFileInfo.fname[0])
        {
            break;
        }

        UARTprintf("  %c%c%c%c%c  %u/%02u/%02u  %02u:%02u  %9u  %s\n",
                  // %c도 32비트(unsigned long)로 읽으므로 형을 맞춰 넘긴다
                  (unsigned long)((g_sFileInfo.fattrib & AM_DIR) ? 'D' : '-'),
                  (unsigned long)((g_sFileInfo.fattrib & AM_RDO) ? 'R' : '-'),
                  (unsigned long)((g_sFileInfo.fattrib & AM_HID) ? 'H' : '-'),
                  (unsigned long)((g_sFileInfo.fattrib & AM_SYS) ? 'S' : '-'),
                  (unsigned long)((g_sFileInfo.fattrib & AM_ARC) ? 'A' : '-'),
                  (uint32_t)((g_sFileInfo.fdate >> 9) + 1980),
                  (uint32_t)((g_sFileInfo.fdate >> 5) & 15),
                  (uint32_t)(g_sFileInfo.fdate & 31),
                  (uint32_t)((g_sFileInfo.ftime >> 11)),
                  (uint32_t)((g_sFileInfo.ftime >> 5) & 63),
                  (uint32_t)(g_sFileInfo.fsize),
                  g_sFileInfo.fname);

        if(g_sFileInfo.fattrib & AM_DIR)
        {
            ulDirCount++;
        }
        else
        {
            ulFileCount++;
            ulTotalSize += g_sFileInfo.fsize;
        }
    }

    UARTprintf("\n  %4u File(s), %10u bytes total\n  %4u Dir(s)",
               ulFileCount, ulTotalSize, ulDirCount);

    fresult = f_getfree("/", (DWORD *)&ulTotalSize, &pFatFs);
    if(fresult == FR_OK)
    {
        UARTprintf(", %10u KB free\n", (ulTotalSize * pFatFs->sects_clust) / 2);
    }
    else
    {
        UARTprintf("\n");
    }

    return 0;
}

//*****************************************************************************
// 보조 함수: ChangeToDirectory
//*****************************************************************************
static FRESULT ChangeToDirectory(char *pcDirectory, uint32_t *pulReason)
{
    uint32_t uIdx;
    FRESULT fresult;

    strcpy(g_cTmpBuf, g_cCwdBuf);

    if(pcDirectory[0] == '/')
    {
        if(strlen(pcDirectory) + 1 > sizeof(g_cCwdBuf))
        {
            *pulReason = NAME_TOO_LONG_ERROR;
            return FR_OK;
        }
        strncpy(g_cTmpBuf, pcDirectory, sizeof(g_cTmpBuf));
    }
    else if(!strcmp(pcDirectory, ".."))
    {
        //
        // 루트("/")에서는 더 올라갈 곳이 없다. (TI 원본은 여기서 경로를 빈 문자열로 만들어 프롬프트가 깨졌음)
        //
        if(strcmp(g_cTmpBuf, "/"))
        {
            uIdx = (uint32_t)strlen(g_cTmpBuf) - 1;
            while((g_cTmpBuf[uIdx] != '/') && (uIdx > 0))
            {
                uIdx--;
            }
            if(uIdx == 0)
            {
                g_cTmpBuf[1] = '\0';    // "/dir" 의 상위는 루트 "/"
            }
            else
            {
                g_cTmpBuf[uIdx] = '\0';
            }
        }
    }
    else
    {
        if(strlen(g_cTmpBuf) + strlen(pcDirectory) + 2 > sizeof(g_cCwdBuf))
        {
            *pulReason = NAME_TOO_LONG_ERROR;
            return FR_INVALID_OBJECT;
        }
        if(strcmp(g_cTmpBuf, "/"))
        {
            strcat(g_cTmpBuf, "/");
        }
        strcat(g_cTmpBuf, pcDirectory);
    }

    fresult = f_opendir(&g_sDirObject, g_cTmpBuf);
    if(fresult != FR_OK)
    {
        *pulReason = OPENDIR_ERROR;
        return fresult;
    }

    strncpy(g_cCwdBuf, g_cTmpBuf, sizeof(g_cCwdBuf));
    return FR_OK;
}

//*****************************************************************************
// Cmd_cd - 디렉터리 변경
//*****************************************************************************
int Cmd_cd(int argc, char *argv[])
{
    uint32_t ulReason = 0;
    FRESULT fresult;

    if(argc < 2)
    {
        UARTprintf("Usage: cd <path>\n");
        return 0;
    }

    fresult = ChangeToDirectory(argv[1], &ulReason);
    if(fresult != FR_OK)
    {
        if(ulReason == OPENDIR_ERROR)
        {
            UARTprintf("Directory not found or invalid: %s\n", argv[1]);
        }
        else if(ulReason == NAME_TOO_LONG_ERROR)
        {
            UARTprintf("Resulting path name is too long\n");
        }
        else
        {
            UARTprintf("cd error: %s\n", StringFromFresult(fresult));
        }
    }

    return 0;
}

//*****************************************************************************
// Cmd_pwd - 현재 작업 디렉터리 출력
//*****************************************************************************
int Cmd_pwd(int argc, char *argv[])
{
    UARTprintf("%s\n", g_cCwdBuf);
    return 0;
}

//*****************************************************************************
// Cmd_cat - 파일 내용 출력
//*****************************************************************************
int Cmd_cat(int argc, char *argv[])
{
    FRESULT fresult;
    WORD usBytesRead;

    if(argc < 2)
    {
        UARTprintf("Usage: cat <filename>\n");
        return 0;
    }

    if(strlen(g_cCwdBuf) + strlen(argv[1]) + 2 > sizeof(g_cTmpBuf))
    {
        UARTprintf("Resulting path name is too long\n");
        return 0;
    }

    strcpy(g_cTmpBuf, g_cCwdBuf);
    if(strcmp("/", g_cCwdBuf))
    {
        strcat(g_cTmpBuf, "/");
    }
    strcat(g_cTmpBuf, argv[1]);

    fresult = f_open(&g_sFileObject, g_cTmpBuf, FA_READ);
    if(fresult != FR_OK)
    {
        UARTprintf("File open error: %s (%s)\n", argv[1], StringFromFresult(fresult));
        return (int)fresult;
    }

    do
    {
        fresult = f_read(&g_sFileObject, (BYTE *)g_cTmpBuf, sizeof(g_cTmpBuf) - 1, &usBytesRead);
        if(fresult != FR_OK)
        {
            UARTprintf("\nRead error: %s\n", StringFromFresult(fresult));
            f_close(&g_sFileObject);
            return (int)fresult;
        }

        g_cTmpBuf[usBytesRead] = '\0';
        UARTprintf("%s", g_cTmpBuf);
    }
    while(usBytesRead == sizeof(g_cTmpBuf) - 1);

    UARTprintf("\n");
    f_close(&g_sFileObject);
    return 0;
}

//*****************************************************************************
// Cmd_write - 파일에 텍스트 쓰기
//*****************************************************************************
int Cmd_write(int argc, char *argv[])
{
    FRESULT fresult;
    WORD usBytesWritten;
    int i = 2;
    char writeBuff[CMD_BUF_SIZE] = {0};

    if(argc < 3)
    {
        UARTprintf("Usage: write <filename> <text string...>\n");
        return 0;
    }

    if(strlen(g_cCwdBuf) + strlen(argv[1]) + 2 > sizeof(g_cTmpBuf))
    {
        UARTprintf("Resulting path name is too long\n");
        return 0;
    }

    strcpy(g_cTmpBuf, g_cCwdBuf);
    if(strcmp("/", g_cCwdBuf))
    {
        strcat(g_cTmpBuf, "/");
    }
    strcat(g_cTmpBuf, argv[1]);

    fresult = f_open(&g_sFileObject, g_cTmpBuf, FA_WRITE | FA_CREATE_ALWAYS);
    if(fresult != FR_OK)
    {
        UARTprintf("File open/create error: %s\n", StringFromFresult(fresult));
        return (int)fresult;
    }

    while(i < argc)
    {
        strcat(writeBuff, argv[i]);
        if(i < argc - 1)
        {
            strcat(writeBuff, " ");
        }
        i++;
    }
    strcat(writeBuff, "\r\n");

    fresult = f_write(&g_sFileObject, (const BYTE *)writeBuff, (WORD)strlen(writeBuff), &usBytesWritten);
    if(fresult != FR_OK)
    {
        UARTprintf("Write error: %s\n", StringFromFresult(fresult));
        f_close(&g_sFileObject);
        return (int)fresult;
    }

    f_close(&g_sFileObject);
    UARTprintf("Successfully wrote %u bytes to %s\n", (uint32_t)usBytesWritten, argv[1]);
    return 0;
}

//*****************************************************************************
// Cmd_mkdir - 디렉터리 생성
//*****************************************************************************
int Cmd_mkdir(int argc, char *argv[])
{
    FRESULT fresult;

    if(argc < 2)
    {
        UARTprintf("Usage: mkdir <directory_name>\n");
        return 0;
    }

    if(strlen(g_cCwdBuf) + strlen(argv[1]) + 2 > sizeof(g_cTmpBuf))
    {
        UARTprintf("Resulting path name is too long\n");
        return 0;
    }

    strcpy(g_cTmpBuf, g_cCwdBuf);
    if(strcmp("/", g_cCwdBuf))
    {
        strcat(g_cTmpBuf, "/");
    }
    strcat(g_cTmpBuf, argv[1]);

    fresult = f_mkdir(g_cTmpBuf);
    if(fresult != FR_OK)
    {
        UARTprintf("mkdir error: %s\n", StringFromFresult(fresult));
        return (int)fresult;
    }

    UARTprintf("Directory created: %s\n", argv[1]);
    return 0;
}

//*****************************************************************************
// Cmd_rm - 파일 또는 빈 디렉터리 삭제
//*****************************************************************************
int Cmd_rm(int argc, char *argv[])
{
    FRESULT fresult;

    if(argc < 2)
    {
        UARTprintf("Usage: rm <filename/dir>\n");
        return 0;
    }

    if(strlen(g_cCwdBuf) + strlen(argv[1]) + 2 > sizeof(g_cTmpBuf))
    {
        UARTprintf("Resulting path name is too long\n");
        return 0;
    }

    strcpy(g_cTmpBuf, g_cCwdBuf);
    if(strcmp("/", g_cCwdBuf))
    {
        strcat(g_cTmpBuf, "/");
    }
    strcat(g_cTmpBuf, argv[1]);

    fresult = f_unlink(g_cTmpBuf);
    if(fresult != FR_OK)
    {
        UARTprintf("rm error: %s\n", StringFromFresult(fresult));
        return (int)fresult;
    }

    UARTprintf("Removed: %s\n", argv[1]);
    return 0;
}

//*****************************************************************************
// 메인 진입점
//*****************************************************************************
int main(void)
{
    int nStatus;
    FRESULT fresult;

    //
    // 시스템 클럭과 장치 주변장치 초기화
    //
    Device_init();
    Device_initGPIO();

    //
    // PIE와 벡터 테이블 초기화
    //
    Interrupt_initModule();
    Interrupt_initVectorTable();

    //
    // CPUTimer0을 100Hz(10ms) SysTick으로 설정
    //
    CPUTimer_setPeriod(CPUTIMER0_BASE, (DEVICE_SYSCLK_FREQ / 100) - 1);
    CPUTimer_setPreScaler(CPUTIMER0_BASE, 0);
    CPUTimer_stopTimer(CPUTIMER0_BASE);
    CPUTimer_reloadTimerCounter(CPUTIMER0_BASE);

    Interrupt_register(INT_TIMER0, &SysTickHandler);
    CPUTimer_enableInterrupt(CPUTIMER0_BASE);
    Interrupt_enable(INT_TIMER0);
    CPUTimer_startTimer(CPUTIMER0_BASE);

    //
    // 전역 인터럽트 허용
    //
    Interrupt_enableGlobal();

    //
    // SCI-A 콘솔 설정
    //
    ConfigureUART();

    //
    // 시작 안내 문구
    //
    UARTprintf("\n\n");
    UARTprintf("======================================================\n");
    UARTprintf(" TMS320F28P65x Micro SD Card SPI Interactive Console  \n");
    UARTprintf(" SyncWorks EVM V2 Area (10) Micro SD + Area (11) SCI  \n");
    UARTprintf("======================================================\n");
    UARTprintf("Type 'help' or '?' for command list.\n");

    //
    // 논리 디스크 0으로 파일 시스템 마운트
    //
    fresult = f_mount(0, &g_sFatFs);
    if(fresult != FR_OK)
    {
        UARTprintf("f_mount error: %s (%d)\n", StringFromFresult(fresult), (unsigned long)fresult);
        UARTprintf("Please verify SD card is formatted (FAT16/32) and inserted.\n");
    }
    else
    {
        UARTprintf("SD Card mounted successfully.\n");
    }

    //
    // 대화형 명령 처리 루프
    //
    while(1)
    {
        UARTprintf("\n%s> ", g_cCwdBuf);

        UARTgets(g_cCmdBuf, sizeof(g_cCmdBuf));

        if(g_cCmdBuf[0] == '\0')
        {
            continue;
        }

        nStatus = CmdLineProcess(g_cCmdBuf);
        if(nStatus == CMDLINE_BAD_CMD)
        {
            UARTprintf("Bad command! Type 'help' for available commands.\n");
        }
        else if(nStatus == CMDLINE_TOO_MANY_ARGS)
        {
            UARTprintf("Too many arguments for command processor!\n");
        }
        else if(nStatus != 0)
        {
            UARTprintf("Command error code: %s\n", StringFromFresult((FRESULT)nStatus));
        }
    }
}

// 파일 끝.
