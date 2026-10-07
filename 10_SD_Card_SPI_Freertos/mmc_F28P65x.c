//#############################################################################
//
// FILE:   mmc_F28P65x.c
//
// TITLE:  TMS320F28P65x DriverLib용 MMC/SDC(SPI 모드) 제어 모듈
//
// ChaN FatFs MMC/SDC 드라이버를 F28P65x DriverLib SPI-C로 이식한 것입니다.
//
// SPI 핀 연결 (10번 영역 Micro SD 슬롯):
//   - DI  (MOSI): GPIO50 (SPIC_PICO) -> EVM V2 B-Side 45번 핀
//   - DO  (MISO): GPIO51 (SPIC_POCI) -> EVM V2 B-Side 43번 핀
//   - CLK (SCLK): GPIO52 (SPIC_CLK)  -> EVM V2 B-Side 41번 핀
//   - CS  (SS)  : GPIO53 (GPIO 출력)  -> EVM V2 B-Side 39번 핀
//
//#############################################################################

#include "driverlib.h"
#include "device.h"
#include "diskio.h"
#include "integer.h"

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
// CS(칩 선택) 핀 제어: GPIO53 (Active Low)
//
static inline void SELECT(void)
{
    GPIO_writePin(53, 0);
}

static inline void DESELECT(void)
{
    GPIO_writePin(53, 1);
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
    // 8비트 데이터는 16비트 TX 버퍼에 좌측 정렬로 쓴다
    SPI_writeDataBlockingNonFIFO(SPIC_BASE, ((uint16_t)dat) << 8);

    // 함께 들어온 더미 바이트를 읽어서 버린다
    (void)SPI_readDataBlockingNonFIFO(SPIC_BASE);
}

//-----------------------------------------------------------------------
// SPI로 MMC에서 1바이트 수신
//-----------------------------------------------------------------------
static BYTE rcvr_spi(void)
{
    // 클럭을 만들려고 더미 바이트 0xFF를 쓴다
    SPI_writeDataBlockingNonFIFO(SPIC_BASE, 0xFF00);

    // 하위 8비트를 결과로 읽는다
    return (BYTE)(SPI_readDataBlockingNonFIFO(SPIC_BASE) & 0xFF);
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
        DEVICE_DELAY_US(175);
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
// 전원 켜기 및 SPI 초기화
//-----------------------------------------------------------------------
static void power_on(void)
{
    // SPI-C 주변장치 클럭 켜기
    SysCtl_enablePeripheral(SYSCTL_PERIPH_CLK_SPIC);

    // SPI-C용 GPIO 핀 설정
    GPIO_setPinConfig(GPIO_50_SPIC_PICO);
    GPIO_setPinConfig(GPIO_51_SPIC_POCI);
    GPIO_setPinConfig(GPIO_52_SPIC_CLK);

    GPIO_setPadConfig(50, GPIO_PIN_TYPE_PULLUP);
    GPIO_setPadConfig(51, GPIO_PIN_TYPE_PULLUP);
    GPIO_setPadConfig(52, GPIO_PIN_TYPE_STD);

    GPIO_setQualificationMode(50, GPIO_QUAL_ASYNC);
    GPIO_setQualificationMode(51, GPIO_QUAL_ASYNC);
    GPIO_setQualificationMode(52, GPIO_QUAL_ASYNC);

    // GPIO53을 수동 제어 CS 출력으로 설정
    GPIO_setPinConfig(GPIO_53_GPIO53);
    GPIO_setDirectionMode(53, GPIO_DIR_MODE_OUT);
    GPIO_setPadConfig(53, GPIO_PIN_TYPE_STD);
    DESELECT();

    // SPI-C를 400kHz 저속 클럭, 8비트, 컨트롤러(마스터) 모드로 설정
    // TI의 SPI_PROT_POL1PHA0 = 표준 SPI 모드 3(CPOL=1, CPHA=1: 클럭 idle High, 상승 에지에서 샘플).
    // SD 카드는 SPI 모드 0과 3을 모두 받는다. (표준 모드 0은 TI 표기로 POL0PHA1)
    SPI_disableModule(SPIC_BASE);
    SPI_disableFIFO(SPIC_BASE);
    SPI_setConfig(SPIC_BASE, DEVICE_LSPCLK_FREQ, SPI_PROT_POL1PHA0,
                  SPI_MODE_CONTROLLER, 400000, 8);
    SPI_enableTalk(SPIC_BASE);
    SPI_setEmulationMode(SPIC_BASE, SPI_EMULATION_FREE_RUN);
    SPI_enableModule(SPIC_BASE);

    PowerFlag = 1;
}

//-----------------------------------------------------------------------
// SPI를 고속(12.5MHz)으로 전환
//-----------------------------------------------------------------------
static void set_max_speed(void)
{
    SPI_disableModule(SPIC_BASE);
    SPI_setConfig(SPIC_BASE, DEVICE_LSPCLK_FREQ, SPI_PROT_POL1PHA0,
                  SPI_MODE_CONTROLLER, 12500000, 8);
    SPI_enableTalk(SPIC_BASE);
    SPI_enableModule(SPIC_BASE);
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
        wc = 256;
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
