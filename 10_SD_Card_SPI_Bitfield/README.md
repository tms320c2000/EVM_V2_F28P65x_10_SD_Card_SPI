# Micro SD 카드 (SPI) 대화형 콘솔 — TMS320F28P65x 비트필드 버전

**요약**
- 개발보드 V2의 **(10) Micro SD 카드 슬롯(SPI)** 을 FatFs로 마운트하고, **(11) SCI-to-USB**로 PC 터미널에서 `ls`, `cat`, `write`, `mkdir`, `rm` 같은 명령으로 SD 카드를 다룹니다.
- **DriverLib을 전혀 쓰지 않고** 레지스터 구조체(`SpicRegs`, `SciaRegs`, `GpioDataRegs`, `CpuTimer0Regs`, `PieCtrlRegs`)를 직접 조작합니다.
- 같은 동작을 [DriverLib 버전](../10_SD_Card_SPI_Driverlib/)과 [FreeRTOS 버전](../10_SD_Card_SPI_Freertos/)으로도 제공합니다(비교표는 아래).

## 1. 하드웨어 배선

![TMS320F28X EVM V2 — SD 카드(SPI=B-Side) · SCI 콘솔(A-Side) 결선 안내](f28xevm_v2_sdcard_wiring.png)

### 1.1 (10) Micro SD 슬롯 ↔ F28P65x 모듈 (B-Side CN9001)

| EVM V2 (10) SD 슬롯 핀 | 신호 | F28P65x 모듈 핀 (B-Side) | GPIO | 레지스터 설정 |
|---|---|---|---|---|
| **DI** | MOSI | **B-Side 45번** | GPIO50 | MUX 6 (SPIC_PICO) |
| **DO** | MISO | **B-Side 43번** | GPIO51 | MUX 6 (SPIC_POCI) |
| **CLK** | SPICLK | **B-Side 41번** | GPIO52 | MUX 6 (SPIC_CLK) |
| **CS** | Chip Select | **B-Side 39번** | GPIO53 | 일반 GPIO 출력(Low 활성, 소프트웨어 제어) |

SD 카드 전원(3.3V, GND)은 보드가 슬롯에 직접 공급하므로 데이터 4선만 연결합니다.

### 1.2 (11) SCI-to-USB ↔ F28P65x 모듈 (A-Side CN9000)

| EVM V2 (11) 핀 | 방향 | F28P65x 모듈 핀 (A-Side) | GPIO |
|---|---|---|---|
| **RX1** | PC → DSP | **A-Side 91번** | GPIO13 (SCIA_RX, MUX 6) |
| **TX1** | DSP → PC | **A-Side 89번** | GPIO12 (SCIA_TX, MUX 6) |

(11) 블록의 Mini-B USB를 PC에 연결하면 가상 COM 포트가 생깁니다. 터미널은 **115200 8N1, 흐름 제어 없음**.

## 2. SD 카드 준비

- Micro SD/SDHC(4~32GB 권장), **FAT16 또는 FAT32**(exFAT/NTFS 미지원)로 포맷.
- 루트에 `readme.txt` 같은 텍스트 파일을 넣어 두면 `cat`으로 바로 읽기를 확인할 수 있습니다.

## 3. 명령어

부팅하면 배너와 함께 `SD Card mounted successfully.`, 프롬프트 `/> `가 나옵니다.

| 명령 | 설명 | 예 |
|---|---|---|
| `help`, `h`, `?` | 명령어 목록 | `help` |
| `ls` | 현재 디렉터리 목록, 파일 수, 남은 용량 | `ls` |
| `pwd` | 현재 경로 | `pwd` |
| `cd`, `chdir` | 디렉터리 이동(`..` 상위, `/` 루트. 루트에서 `..`는 루트 유지) | `cd logs` |
| `cat` | 텍스트 파일 출력 | `cat readme.txt` |
| `write` | 파일에 텍스트 쓰기(없으면 생성) | `write log.txt hello world` |
| `mkdir` | 디렉터리 생성 | `mkdir data` |
| `rm` | 파일 또는 빈 디렉터리 삭제 | `rm log.txt` |

## 4. 소프트웨어 구조

| 파일 | 역할 |
|---|---|
| `10_SD_Card_SPI_Bitfield.c` | 메인, 명령어 핸들러, CPU Timer0 10ms 인터럽트(`SysTickHandler`) |
| `mmc_F28P65x.c` | **SPI-C 비트필드 SD 드라이버**(ChaN FatFs MMC 드라이버 이식) |
| `utils/uartstdio.c` | **SCI-A 비트필드 콘솔**(`UARTprintf`, `UARTgets`, FIFO 폴링) |
| `utils/cmdline.c`, `ustdlib.c` | 명령행 파서, 경량 printf(DriverLib 의존 `debug.h`를 제거) |
| `fatfs/` | ChaN FatFs(C28x 포팅) |
| `device/`, `f28p65x_headers_nonBIOS_cpu1.cmd` | TI 비트필드 헤더/초기화 소스(`11_SCI_to_USB_Bitfield`와 같은 구성) |

핵심 동작:
- **SPI**: 8비트, **표준 SPI 모드 3**(`CLKPOLARITY=1`, `CLK_PHASE=0`), FIFO 미사용·`SPISTS.INT_FLAG` 폴링. 카드 초기화는 400 kHz(`SPIBRR=124`), 성공 후 12.5 MHz(`SPIBRR=3`). 송신 버퍼는 좌측 정렬(`<<8`), 수신 버퍼는 우측 정렬(`& 0xFF`)입니다.
- **CS**: GPIO53을 `GPBSET`/`GPBCLEAR`로 직접 제어합니다.
- **타임아웃 틱**: SD 대기 루프가 메인 흐름을 붙잡고 있어도 타임아웃이 줄도록 CPU Timer0을 100Hz 인터럽트(PIE 그룹 1.7)로 돌려 `disk_timerproc()`를 부릅니다.
- **링커**: FatFs 버퍼와 코드가 커서 RAM 구성은 `.text`를 RAMD0→LS0~3에, `.stack`을 RAMD1에 둡니다.

## 5. 빌드·실행

1. CCS에서 `File > Import > CCS Projects`로 이 폴더의 `CCS/`를 지정해 `10_SD_Card_SPI_Bitfield`를 가져옵니다.
2. 구성 선택: **CPU1_RAM**(디버깅용), **CPU1_FLASH**(전원을 껐다 켜도 실행).
3. Build → Debug → Run, 터미널에서 `help`를 입력합니다.

### 5.1 실행 화면

(11) SCI-to-USB로 연결한 PC 터미널(CCS의 Serial Console, 115200 8N1)에 나온 화면입니다. COM 번호는 PC마다 다릅니다.

![SD 카드 콘솔 실행 화면 — 부팅 배너, 마운트, ls](sdcard_console_ls.png)

- **부팅 배너와 `SD Card mounted successfully.`**: SCI-A 콘솔(A-Side GPIO12/13)이 살아 있고 `f_mount`가 끝났다는 뜻입니다. `f_mount`는 FatFs에 드라이브를 등록만 하고 카드에는 접근하지 않으므로, 이 문구만으로는 카드 통신을 확인한 것이 아닙니다. 카드 초기화는 첫 명령(`ls`)에서 일어나며, 실패하면 `FR_NOT_READY`가 나옵니다.
- **`ls` 결과**: 카드에서 루트 디렉터리를 읽어 한 줄에 `속성  날짜  시간  크기  이름` 순으로 보여 줍니다. 속성 문자는 `D` 디렉터리, `R` 읽기 전용, `H` 숨김, `S` 시스템, `A` 보관이고 해당하지 않으면 `-`입니다. `SYSTEM~1`은 Windows가 카드에 만든 `System Volume Information` 폴더의 짧은 이름입니다.
- **합계 줄**: `2 File(s), 26 bytes total`은 `test.txt`(15바이트)와 `test1.txt`(11바이트)의 합이고, `2 Dir(s)`는 폴더 수, 마지막은 남은 용량(KB)입니다.
- **날짜**: 이 예제는 RTC가 없어 `get_fattime()`이 고정값 2026-09-17 12:00을 돌려줍니다. 보드에서 `write`/`mkdir`로 만든 항목은 항상 이 시각으로 기록되고, 화면의 `test` 폴더가 이 시각과 같습니다. PC에서 만든 항목은 실제 시각으로 보입니다.

## 6. 검증 상태

- **확인함(2026-10-05)**: CCS가 `.projectspec`을 가져와 빌드하는 과정(파일 복사 규칙, 전체 소스 컴파일, 링크)을 흉내 낸 빌드로 **CPU1_RAM, CPU1_FLASH 모두 오류 없이 링크**. `ff.c`의 열거형 혼용 경고는 `(FRESULT)` 캐스트로 없앴고, 남은 것은 부동소수점 나눗셈 성능 조언(`#2614-D`) 2건뿐입니다.
- **핀 변경(SPI=B-Side, SCI=A-Side) 후 재확인**: 같은 방식으로 CPU1_RAM, CPU1_FLASH 모두 오류·경고 없이 링크(실제 보드 동작은 확인 전). 핀 번호는 모듈 보드 파일과 C2000Ware `pin_map.h`를 대조해 정했습니다.
- **확인함(2026-10-07, 보드)**: 새 배선(SPI=B-Side GPIO50~53, SCI=A-Side GPIO12/13)에서 부팅 배너, SD 마운트, `ls`(카드 초기화, 디렉터리 읽기, 남은 용량 계산) 성공. `disk_initialize`가 성공하면 SPI를 12.5 MHz로 올리므로 그 속도에서 읽기가 된 것입니다(위 5.1 화면). 원인 추적 중 SD 명령 패킷의 끝 비트(`0x01`) 누락을 고친 뒤 확인되었습니다.
- **확인 전**: `cat`, `write`, `mkdir`, `rm` 명령, 쓰기 속도, 전원을 껐다 켠 뒤 CPU1_FLASH 단독 실행(화면은 디버거에서 실행한 것입니다).
- 점퍼선이 길어 12.5 MHz에서 오류가 나면 `mmc_F28P65x.c`의 `SPI_BRR_FAST`를 6(약 7.1 MHz) 또는 9(5 MHz)로 낮춰 보세요.

## 7. 세 버전 비교

| 버전 | 폴더 | 핵심 차이 | RAM 구성 사용량 | FLASH 구성 사용량 |
|---|---|---|---|---|
| **비트필드(이 폴더)** | 10_SD_Card_SPI_Bitfield | 레지스터 직접 조작, DriverLib 없음, 타이머 인터럽트 | 16,922 워드 | Flash 12,964 / RAM 3,966 |
| DriverLib | [10_SD_Card_SPI_Driverlib](../10_SD_Card_SPI_Driverlib/) | TI 표준 HAL 함수, 타이머 인터럽트 | 17,748 워드 | Flash 14,733 / RAM 3,717 |
| FreeRTOS | [10_SD_Card_SPI_Freertos](../10_SD_Card_SPI_Freertos/) | 태스크 3개(콘솔/하트비트/디스크 타이머), 정적 할당 | 20,334 워드 | Flash 16,878 / RAM 4,150 |

링크 맵의 영역별 사용량 합계(워드)입니다. 측정: 2026-10-07, C2000 CGT 25.11.1.LTS(`--opt_level=off`), CCS 빌드와 같은 옵션으로 링크한 맵 기준입니다FLASH 구성의 Flash는 `FLASH_BANK0`(131,070워드) 대비 비트필드 약 9.9%, DriverLib 약 11.2%, FreeRTOS 약 12.9%입니다. RAM 합계에는 PIE 벡터 테이블·주변장치 프레임 영역(약 480워드)도 들어 있습니다. RAM 구성은 코드까지 RAM에 올라가므로 크고, 스택(0x800)과 DriverLib 판의 힙 예약(0x800)이 포함됩니다.

## 관련 링크
- 상품 페이지: https://tms320f28x.co.kr/goods/goods_view.php?goodsNo=200903127
- 게시판 글: https://tms320f28x.co.kr/board/view.php?bdId=tms320f28xevmv2&sno=113
