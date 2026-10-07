//#############################################################################
//
// FILE:   mmc_F28P65x.c  (Bitfield 버전)
//
// TITLE:  TMS320F28P65x용 MMC/SDC(SPI 모드) 제어 모듈 - 비트필드 레지스터
//
// ChaN FatFs MMC/SDC 드라이버를 F28P65x SPI-C 비트필드 레지스터(SpicRegs)로 이식한 것입니다.
// DriverLib 함수는 전혀 쓰지 않습니다.
//
// SPI 핀 연결 (10번 영역 Micro SD 슬롯):
//   - DI  (MOSI): GPIO50 (SPIC_PICO) -> EVM V2 B-Side 45번 핀
//   - DO  (MISO): GPIO51 (SPIC_POCI) -> EVM V2 B-Side 43번 핀
//   - CLK (SCLK): GPIO52 (SPIC_CLK)  -> EVM V2 B-Side 41번 핀
//   - CS  (SS)  : GPIO53 (GPIO 출력)  -> EVM V2 B-Side 39번 핀
//
// SPI 설정 요약
//   - 표준 SPI 모드 3 (CPOL=1, CPHA=1). TI 레지스터로는 CLKPOLARITY=1, CLK_PHASE=0 ("falling edge without delay").
//   - 초기화 단계 400 kHz, 카드 초기화 성공 후 12.5 MHz.  BRR = LSPCLK/속도 - 1  (LSPCLK = 50 MHz)
//   - 송신 버퍼(SPITXBUF)는 좌측 정렬(<<8), 수신 버퍼(SPIRXBUF)는 우측 정렬(& 0xFF)이다.
//   - FIFO를 쓰지 않고 SPISTS.INT_FLAG(수신 완료)를 폴링한다.
//
//#############################################################################

#include "f28x_project.h"
#include "diskio.h"
#include "integer.h"

#define SPI_LSPCLK_HZ       50000000UL      // LSPCLK = SYSCLK(200MHz)/4
#define SPI_BRR_SLOW        ((SPI_LSPCLK_HZ / 400000UL) - 1UL)      // 400 kHz  -> 124
#define SPI_BRR_FAST        ((SPI_LSPCLK_HZ / 12500000UL) - 1UL)    // 12.5 MHz -> 3

//
// MMC/SDC 명령 정의
//
#define CMD0    (0x40+0)    // GO_IDLE_STATE: 대기 상태로 진입
#define CMD1    (0x40+1)    // SEND_OP_COND: 초기화 시작
#define CMD8    (0x40+8)    // SEND_IF_COND: 인터페이스 조건 확인
#define CMD9    (0x40+9)    // SEND_CSD: CSD 레지스터 읽기
#define CMD10   (0x40+10)   // SEND_CID: CID 레지스터 읽기
#define CMD12   (0x40+12)   // STOP_TRANSMISSION: 전송 중지
#define CMD16   (0x40+16)   // SET_BLOCKLEN: 블록 길이 설정
#define CMD17   (0x40+17)   // READ_SINGLE_BLOCK: 단일 블록 읽기
#define CMD18   (0x40+18)   // READ_MULTIPLE_BLOCK: 다중 블록 읽기
#define CMD23   (0x40+23)   // SET_BLOCK_COUNT: 블록 수 설정
#define CMD24   (0x40+24)   // WRITE_BLOCK: 단일 블록 쓰기
#define CMD25   (0x40+25)   // WRITE_MULTIPLE_BLOCK: 다중 블록 쓰기
#define CMD41   (0x40+41)   // SEND_OP_COND (ACMD): 초기화 시작
#define CMD55   (0x40+55)   // APP_CMD: 다음 명령은 응용 명령(ACMD)
#define CMD58   (0x40+58)   // READ_OCR: OCR 레지스터 읽기

//
// CS(칩 선택) 핀 제어: GPIO53 (Active Low), GPIO32~63은 GPB 레지스터
//
static inline void SELECT(void)
{
    GpioDataRegs.GPBCLEAR.bit.GPIO53 = 1;
}

static inline void DESELECT(void)
{
    GpioDataRegs.GPBSET.bit.GPIO53 = 1;
}

//
// 모듈 내부 변수
//
static volatile DSTATUS Stat = STA_NOINIT;  // 디스크 상태
static volatile BYTE Timer1, Timer2;        // 100Hz로 감소하는 타이머
static BYTE CardType;                       // b0:MMC, b1:SDC, b2:블록 주소 지정
static BYTE PowerFlag = 0;                  // 전원/인터페이스가 켜져 있는지 표시

//-----------------------------------------------------------------------
// SPI로 MMC에 1바이트 송신
//-----------------------------------------------------------------------
static void xmit_spi(BYTE dat)
{
    // 8비트 데이터는 16비트 TX 버퍼에 좌측 정렬로 써야 한다
    SpicRegs.SPITXBUF = ((Uint16)dat) << 8;

    // 클럭이 다 나가 수신 완료 플래그가 서면 같이 들어온 더미 바이트를 읽어 버린다
    while(SpicRegs.SPISTS.bit.INT_FLAG != 1U)
    {
    }
    (void)SpicRegs.SPIRXBUF;
}

//-----------------------------------------------------------------------
// SPI로 MMC에서 1바이트 수신
//-----------------------------------------------------------------------
static BYTE rcvr_spi(void)
{
    // 0xFF를 보내 클럭을 만든다
    SpicRegs.SPITXBUF = 0xFF00U;

    while(SpicRegs.SPISTS.bit.INT_FLAG != 1U)
    {
    }

    // 수신 버퍼는 우측 정렬이라 하위 8비트가 받은 바이트다
    return (BYTE)(SpicRegs.SPIRXBUF & 0xFFU);
}

static inline void rcvr_spi_m(BYTE *dst)
{
    *dst = rcvr_spi();
}

//-----------------------------------------------------------------------
// 카드 준비 대기
//-----------------------------------------------------------------------
static BYTE wait_ready(void)
{
    BYTE res;

    Timer2 = 50;    // 500ms 타임아웃
    rcvr_spi();
    do {
        res = rcvr_spi();
        DELAY_US(175);
    } while ((res != 0xFF) && Timer2);

    return res;
}

//-----------------------------------------------------------------------
// SPI 모드 진입을 위해 CS를 High로 유지한 채 클럭 80개 송출
//-----------------------------------------------------------------------
static void send_initial_clock_train(void)
{
    unsigned int i;

    // CS는 High여야 한다
    DESELECT();

    // DI를 High로 유지한 채 더미 바이트 10개(클럭 80개) 송신
    for(i = 0; i < 10; i++)
    {
        xmit_spi(0xFF);
    }
}

//-----------------------------------------------------------------------
// SPI-C 레지스터 설정 (속도만 바꿔 다시 쓴다)
//-----------------------------------------------------------------------
static void spi_configure(Uint16 brr)
{
    SpicRegs.SPICCR.bit.SPISWRESET = 0U;            // 설정을 바꾸는 동안 리셋 상태로 둔다
    SpicRegs.SPICCR.bit.SPICHAR = 7U;               // 8비트 문자
    SpicRegs.SPICCR.bit.CLKPOLARITY = 1U;           // 클럭 idle High
    SpicRegs.SPICCR.bit.SPILBK = 0U;                // 루프백 끔
    SpicRegs.SPICTL.bit.CLK_PHASE = 0U;             // CPOL=1 + 이 값 0 => 표준 SPI 모드 3
    SpicRegs.SPICTL.bit.CONTROLLER_PERIPHERAL = 1U; // 마스터(Controller)
    SpicRegs.SPICTL.bit.TALK = 1U;                  // 송신 활성화
    SpicRegs.SPICTL.bit.SPIINTENA = 0U;             // 인터럽트 없이 폴링
    SpicRegs.SPIBRR.bit.SPI_BIT_RATE = brr;
    SpicRegs.SPIFFTX.bit.SPIFFENA = 0U;             // FIFO를 쓰지 않는다(INT_FLAG 폴링과 맞추기 위해)
    SpicRegs.SPIPRI.bit.FREE = 1U;                  // 디버거 정지 중에도 계속 동작
    SpicRegs.SPICCR.bit.SPISWRESET = 1U;            // 동작 시작
}

//-----------------------------------------------------------------------
// 전원 켜기 및 SPI 초기화
//-----------------------------------------------------------------------
static void power_on(void)
{
    // SPI-C 클럭은 InitSysCtrl()이 켜 둔다(CpuSysRegs.PCLKCR8.bit.SPI_C)

    // DI/DO/CLK: GPIO50/51/52 -> SPI-C (GMUX=1, MUX=2 => 6)
    GPIO_SetupPinMux(50, GPIO_MUX_CPU1, 6);
    GPIO_SetupPinMux(51, GPIO_MUX_CPU1, 6);
    GPIO_SetupPinMux(52, GPIO_MUX_CPU1, 6);
    GPIO_SetupPinOptions(50, GPIO_OUTPUT, GPIO_PUSHPULL | GPIO_ASYNC);
    GPIO_SetupPinOptions(51, GPIO_INPUT,  GPIO_PULLUP   | GPIO_ASYNC);
    GPIO_SetupPinOptions(52, GPIO_OUTPUT, GPIO_PUSHPULL | GPIO_ASYNC);

    // CS: GPIO53을 소프트웨어 제어 출력으로 (High로 먼저 만든 뒤 출력으로 바꿔 글리치를 막는다)
    GPIO_SetupPinMux(53, GPIO_MUX_CPU1, 0);
    DESELECT();
    GPIO_SetupPinOptions(53, GPIO_OUTPUT, GPIO_PUSHPULL);

    // 400kHz 저속으로 시작
    spi_configure(SPI_BRR_SLOW);

    PowerFlag = 1;
}

//-----------------------------------------------------------------------
// SPI를 고속(12.5MHz)으로 전환
//-----------------------------------------------------------------------
static void set_max_speed(void)
{
    spi_configure(SPI_BRR_FAST);
}

static void power_off(void)
{
    PowerFlag = 0;
}

static int chk_power(void)
{
    return PowerFlag;
}

//-----------------------------------------------------------------------
// MMC에서 데이터 패킷 수신
//-----------------------------------------------------------------------
static BOOL rcvr_datablock(BYTE *buff, UINT btr)
{
    BYTE token;

    Timer1 = 10;    // 100ms 타임아웃
    do {
        token = rcvr_spi();
    } while ((token == 0xFF) && Timer1);

    if(token != 0xFE)
    {
        return FALSE;   // 유효한 데이터 토큰이 아님
    }

    do {
        rcvr_spi_m(buff++);
        rcvr_spi_m(buff++);
    } while (btr -= 2);

    rcvr_spi(); // CRC 버림
    rcvr_spi();

    return TRUE;
}

//-----------------------------------------------------------------------
// MMC로 데이터 패킷 송신
//-----------------------------------------------------------------------
#if _FS_READONLY == 0
static BOOL xmit_datablock(const BYTE *buff, BYTE token)
{
    BYTE resp, wc;

    if(wait_ready() != 0xFF)
    {
        return FALSE;
    }

    xmit_spi(token);    // 데이터 토큰 송신
    if(token != 0xFD)   // Stop 토큰이 아니면
    {
        wc = 256;       // C28x는 BYTE가 16비트라 256이 들어간다 (2바이트씩 256번 = 512바이트)
        do {
            xmit_spi(*buff++);
            xmit_spi(*buff++);
        } while (--wc);

        xmit_spi(0xFF); // CRC 더미
        xmit_spi(0xFF);

        resp = rcvr_spi();  // 데이터 응답 수신
        if((resp & 0x1F) != 0x05)
        {
            return FALSE;   // 데이터 거부됨
        }
    }

    return TRUE;
}
#endif

//-----------------------------------------------------------------------
// MMC로 명령 패킷 송신
//-----------------------------------------------------------------------
static BYTE send_cmd(BYTE cmd, DWORD arg)
{
    BYTE n, res;

    if(wait_ready() != 0xFF)
    {
        return 0xFF;
    }

    // 명령 패킷 송신
    xmit_spi(cmd);
    xmit_spi((BYTE)(arg >> 24));
    xmit_spi((BYTE)(arg >> 16));
    xmit_spi((BYTE)(arg >> 8));
    xmit_spi((BYTE)arg);

    // SPI 모드에서는 CRC를 검사하지 않지만 끝 비트(bit0)는 반드시 1이어야 카드가 명령으로 인식한다
    n = 0x01;
    if(cmd == CMD0) n = 0x95;   // CMD0(0)의 유효한 CRC
    if(cmd == CMD8) n = 0x87;   // CMD8(0x1AA)의 유효한 CRC
    xmit_spi(n);

    // 응답 수신
    if(cmd == CMD12)
    {
        rcvr_spi(); // 채움 바이트 건너뜀
    }

    n = 10; // 최대 10번 시도하며 대기
    do {
        res = rcvr_spi();
    } while ((res & 0x80) && --n);

    return res;
}

//-----------------------------------------------------------------------
// 다중 섹터 읽기를 끝내려고 CMD12 송신
//-----------------------------------------------------------------------
static BYTE send_cmd12(void)
{
    BYTE n, res = 0xFF, val;

    xmit_spi(CMD12);
    xmit_spi(0);
    xmit_spi(0);
    xmit_spi(0);
    xmit_spi(0);
    xmit_spi(0x01);     // 끝 비트가 1인 더미 CRC

    for(n = 0; n < 10; n++)
    {
        val = rcvr_spi();
        if(val != 0xFF)
        {
            res = val;
        }
    }

    return res;
}

//-----------------------------------------------------------------------
// 공개 함수
//-----------------------------------------------------------------------

//-----------------------------------------------------------------------
// disk_initialize: 디스크 드라이브 초기화
//-----------------------------------------------------------------------
DSTATUS disk_initialize(BYTE drv)
{
    BYTE n, ty, ocr[4];

    if(drv)
    {
        return STA_NOINIT;  // 드라이브 0 하나만 지원
    }

    if(Stat & STA_NODISK)
    {
        return Stat;
    }

    power_on();
    send_initial_clock_train();

    SELECT();
    ty = 0;

    if(send_cmd(CMD0, 0) == 1)  // 대기 상태 진입
    {
        Timer1 = 100;   // 1000ms 타임아웃
        if(send_cmd(CMD8, 0x1AA) == 1)  // SDC 버전 2 이상
        {
            for(n = 0; n < 4; n++) ocr[n] = rcvr_spi();
            if(ocr[2] == 0x01 && ocr[3] == 0xAA)
            {
                do {
                    if(send_cmd(CMD55, 0) <= 1 && send_cmd(CMD41, 1UL << 30) == 0)
                    {
                        break;  // HCS 비트를 세운 ACMD41
                    }
                } while(Timer1);

                if(Timer1 && send_cmd(CMD58, 0) == 0)   // CCS 비트 확인
                {
                    for(n = 0; n < 4; n++) ocr[n] = rcvr_spi();
                    ty = (ocr[0] & 0x40) ? 6 : 2;   // SD v2 (블록 또는 바이트 주소)
                }
            }
        }
        else    // SDC 버전 1 또는 MMC
        {
            ty = (send_cmd(CMD55, 0) <= 1 && send_cmd(CMD41, 0) <= 1) ? 2 : 1;
            do {
                if(ty == 2)
                {
                    if(send_cmd(CMD55, 0) <= 1 && send_cmd(CMD41, 0) == 0) break;
                }
                else
                {
                    if(send_cmd(CMD1, 0) == 0) break;
                }
            } while(Timer1);

            if(!Timer1 || send_cmd(CMD16, 512) != 0)
            {
                ty = 0;
            }
        }
    }

    CardType = ty;
    DESELECT();
    rcvr_spi(); // 유휴 클럭

    if(ty)
    {
        Stat &= ~STA_NOINIT;
        set_max_speed();
    }
    else
    {
        power_off();
    }

    return Stat;
}

//-----------------------------------------------------------------------
// disk_status: 디스크 상태 얻기
//-----------------------------------------------------------------------
DSTATUS disk_status(BYTE drv)
{
    if(drv)
    {
        return STA_NOINIT;
    }
    return Stat;
}

//-----------------------------------------------------------------------
// disk_read: 섹터 읽기
//-----------------------------------------------------------------------
DRESULT disk_read(BYTE drv, BYTE *buff, DWORD sector, BYTE count)
{
    if(drv || !count)
    {
        return RES_PARERR;
    }
    if(Stat & STA_NOINIT)
    {
        return RES_NOTRDY;
    }

    if(!(CardType & 4))
    {
        sector *= 512;  // 바이트 주소 방식이면 바이트 주소로 변환
    }

    SELECT();

    if(count == 1)
    {
        if((send_cmd(CMD17, sector) == 0) && rcvr_datablock(buff, 512))
        {
            count = 0;
        }
    }
    else
    {
        if(send_cmd(CMD18, sector) == 0)
        {
            do {
                if(!rcvr_datablock(buff, 512)) break;
                buff += 512;
            } while(--count);
            send_cmd12();
        }
    }

    DESELECT();
    rcvr_spi();

    return count ? RES_ERROR : RES_OK;
}

//-----------------------------------------------------------------------
// disk_write: 섹터 쓰기
//-----------------------------------------------------------------------
#if _FS_READONLY == 0
DRESULT disk_write(BYTE drv, const BYTE *buff, DWORD sector, BYTE count)
{
    if(drv || !count)
    {
        return RES_PARERR;
    }
    if(Stat & STA_NOINIT)
    {
        return RES_NOTRDY;
    }
    if(Stat & STA_PROTECT)
    {
        return RES_WRPRT;
    }

    if(!(CardType & 4))
    {
        sector *= 512;
    }

    SELECT();

    if(count == 1)
    {
        if((send_cmd(CMD24, sector) == 0) && xmit_datablock(buff, 0xFE))
        {
            count = 0;
        }
    }
    else
    {
        if(CardType & 2)
        {
            send_cmd(CMD55, 0);
            send_cmd(CMD23, count);
        }
        if(send_cmd(CMD25, sector) == 0)
        {
            do {
                if(!xmit_datablock(buff, 0xFC)) break;
                buff += 512;
            } while(--count);

            if(!xmit_datablock(0, 0xFD))    // STOP_TRAN 토큰
            {
                count = 1;
            }
        }
    }

    DESELECT();
    rcvr_spi();

    return count ? RES_ERROR : RES_OK;
}
#endif

//-----------------------------------------------------------------------
// disk_ioctl: 기타 제어 기능
//-----------------------------------------------------------------------
DRESULT disk_ioctl(BYTE drv, BYTE ctrl, void *buff)
{
    DRESULT res;
    BYTE n, csd[16], *ptr = (BYTE *)buff;
    WORD csize;

    if(drv)
    {
        return RES_PARERR;
    }

    res = RES_ERROR;

    if(ctrl == CTRL_POWER)
    {
        switch(*ptr)
        {
        case 0:
            if(chk_power()) power_off();
            res = RES_OK;
            break;
        case 1:
            power_on();
            res = RES_OK;
            break;
        case 2:
            *(ptr + 1) = (BYTE)chk_power();
            res = RES_OK;
            break;
        default:
            res = RES_PARERR;
            break;
        }
    }
    else
    {
        if(Stat & STA_NOINIT)
        {
            return RES_NOTRDY;
        }

        SELECT();

        switch(ctrl)
        {
        case GET_SECTOR_COUNT:
            if((send_cmd(CMD9, 0) == 0) && rcvr_datablock(csd, 16))
            {
                if((csd[0] >> 6) == 1)  // SDC 버전 2.00
                {
                    csize = csd[9] + ((WORD)csd[8] << 8) + 1;
                    *(DWORD *)buff = (DWORD)csize << 10;
                }
                else    // MMC 또는 SDC 버전 1.xx
                {
                    n = (csd[5] & 15) + ((csd[10] & 128) >> 7) + ((csd[9] & 3) << 1) + 2;
                    csize = (csd[8] >> 6) + ((WORD)csd[7] << 2) + ((WORD)(csd[6] & 3) << 10) + 1;
                    *(DWORD *)buff = (DWORD)csize << (n - 9);
                }
                res = RES_OK;
            }
            break;

        case GET_SECTOR_SIZE:
            *(WORD *)buff = 512;
            res = RES_OK;
            break;

        case CTRL_SYNC:
            if(wait_ready() == 0xFF)
            {
                res = RES_OK;
            }
            break;

        case MMC_GET_CSD:
            if(send_cmd(CMD9, 0) == 0 && rcvr_datablock(ptr, 16))
            {
                res = RES_OK;
            }
            break;

        case MMC_GET_CID:
            if(send_cmd(CMD10, 0) == 0 && rcvr_datablock(ptr, 16))
            {
                res = RES_OK;
            }
            break;

        case MMC_GET_OCR:
            if(send_cmd(CMD58, 0) == 0)
            {
                for(n = 0; n < 4; n++)
                {
                    *ptr++ = rcvr_spi();
                }
                res = RES_OK;
            }
            break;

        default:
            res = RES_PARERR;
            break;
        }

        DESELECT();
        rcvr_spi();
    }

    return res;
}

//-----------------------------------------------------------------------
// disk_timerproc: 타임아웃용 100Hz(10ms) 타이머 처리
//-----------------------------------------------------------------------
void disk_timerproc(void)
{
    BYTE n;

    n = Timer1;
    if(n) Timer1 = --n;
    n = Timer2;
    if(n) Timer2 = --n;
}

//-----------------------------------------------------------------------
// get_fattime: FatFs용 실시간 시계(RTC) 서비스
//-----------------------------------------------------------------------
DWORD get_fattime(void)
{
    return ((2026UL - 1980) << 25)  // 연 = 2026
         | (9UL << 21)              // 월 = 9월
         | (17UL << 16)             // 일 = 17
         | (12U << 11)              // 시 = 12
         | (0U << 5)                // 분 = 0
         | (0U >> 1);               // 초 = 0
}

// 파일 끝.
