# TMS320F28X 개발보드 V2 — F28P65x Micro SD 카드(SPI) 예제

[TMS320F28X 범용 개발보드 V2](https://tms320f28x.co.kr/goods/goods_view.php?goodsNo=200903127)의
회로블록 **(10) Micro SD 카드(SPI)** 를 F28P65x 모듈이 FatFs로 읽고 쓰는 예제입니다. **(11) SCI-to-USB**
로 PC 터미널을 연결해 `ls`, `cat`, `write`, `mkdir`, `rm` 같은 명령으로 카드를 다룹니다. 같은 동작을 세 가지
방식으로 각각 구현해서 코드량·메모리 사용량을 비교할 수 있게 만들었습니다.

## 포함된 프로젝트

| 프로젝트 | 설명 |
|---|---|
| [10_SD_Card_SPI_Bitfield](10_SD_Card_SPI_Bitfield/) | 레지스터 비트필드 직접 제어(DriverLib 미사용), CPU Timer0 100Hz 인터럽트 |
| [10_SD_Card_SPI_Driverlib](10_SD_Card_SPI_Driverlib/) | TI DriverLib 사용, CPU Timer0 100Hz 인터럽트 |
| [10_SD_Card_SPI_Freertos](10_SD_Card_SPI_Freertos/) | FreeRTOS 태스크 3개(콘솔/하트비트/디스크 타이머), 정적 할당(힙 미사용), `uptime` 명령 |

각 폴더는 자기완결형(self-contained) CCS 프로젝트입니다 — 폴더 하나만 받아도 필요한 파일이 전부
로컬에 포함되어 있어 Import → Build가 됩니다.

## 배선

**SPI(SD 카드)는 B-Side, SCI(콘솔)는 A-Side** 핀-헤더에 점퍼선으로 연결합니다. SD 카드 전원은 보드가 슬롯에
직접 공급하므로 데이터 4선만 잇고, CD(카드 감지)는 연결하지 않습니다.

| 번호 | 신호 | 개발보드 핀 | 모듈 핀 | GPIO / 기능 |
|---|---|---|---|---|
| 1 | RX1 (PC → DSP) | (11) SCI-to-USB RX1 | A-Side 91번 (CN9000) | GPIO13, SCIA_RX |
| 2 | TX1 (DSP → PC) | (11) SCI-to-USB TX1 | A-Side 89번 (CN9000) | GPIO12, SCIA_TX |
| 3 | CS | (10) Micro SD CS | B-Side 39번 (CN9001) | GPIO53, 일반 GPIO 출력(Low 활성) |
| 4 | DI (MOSI) | (10) Micro SD DI | B-Side 45번 (CN9001) | GPIO50, SPIC_PICO |
| 5 | CLK (SCLK) | (10) Micro SD CLK | B-Side 41번 (CN9001) | GPIO52, SPIC_CLK |
| 6 | DO (MISO) | (10) Micro SD DO | B-Side 43번 (CN9001) | GPIO51, SPIC_POCI |

![TMS320F28X EVM V2 — SD 카드(SPI=B-Side) · SCI 콘솔(A-Side) 결선 안내](10_SD_Card_SPI_Bitfield/f28xevm_v2_sdcard_wiring.png)

## 명령

터미널은 115200 baud, 8N1, 흐름 제어 없음입니다.

| 명령 | 설명 |
|---|---|
| `help`, `h`, `?` | 명령어 목록 |
| `ls` | 현재 디렉터리 목록, 파일 수, 남은 용량 |
| `pwd` | 현재 경로 |
| `cd`, `chdir` | 디렉터리 이동(`..` 상위, `/` 루트) |
| `cat <파일>` | 텍스트 파일 출력 |
| `write <파일> <텍스트>` | 파일에 텍스트 쓰기(없으면 생성) |
| `mkdir <이름>` | 디렉터리 생성 |
| `rm <이름>` | 파일 또는 빈 디렉터리 삭제 |
| `uptime` | 가동 시간과 하트비트 카운터 (FreeRTOS 버전만) |

## 실행 화면

CCS v20의 Serial Console에 나온 화면입니다(비트필드 버전에서 캡처).

![SD 카드 콘솔 실행 화면 — 부팅 배너, 마운트, ls](10_SD_Card_SPI_Bitfield/sdcard_console_ls.png)

`SD Card mounted successfully.`는 FatFs에 드라이브를 등록했다는 뜻이고 카드에는 아직 접근하지 않은 상태입니다.
카드 초기화는 첫 명령(`ls`)에서 일어나며 실패하면 `FR_NOT_READY`가 나옵니다. 자세한 해석은 각 프로젝트의 README를
참고하세요.

## 메모리 사용량 비교

링커 맵의 영역별 사용량 합계(워드, 16비트)입니다. `--opt_level=off`, CGT 25.11.1.LTS로 측정했습니다.

| | Bitfield | DriverLib | FreeRTOS |
|---|---|---|---|
| CPU1_RAM 구성 합계 | 16,922 | 17,748 | 20,334 |
| CPU1_FLASH 구성 — Flash | 12,964 | 14,733 | 16,878 |
| CPU1_FLASH 구성 — RAM | 3,966 | 3,717 | 4,150 |
| `FLASH_BANK0`(131,070워드) 대비 Flash | 약 9.9% | 약 11.2% | 약 12.9% |

RAM 구성은 코드까지 RAM에 올라가므로 크고, 스택(0x800)과 DriverLib/FreeRTOS 판의 힙 예약(0x800)이 포함됩니다.
RAM 합계에는 PIE 벡터 테이블·주변장치 프레임 영역(약 480워드)도 들어 있습니다.

## 검증 상태

- **보드에서 확인함(2026-10-07)**: 비트필드 버전 — 위 배선으로 부팅 배너, SD 마운트, `ls`(카드 초기화·디렉터리 읽기·남은 용량 계산) 성공.
- **빌드만 확인함**: 세 버전 모두 CPU1_RAM, CPU1_FLASH 구성이 오류 없이 컴파일·링크됩니다. DriverLib, FreeRTOS 버전은 보드에서 아직 확인하지 않았습니다.
- **확인 전**: `cat`, `write`, `mkdir`, `rm` 명령, 쓰기 속도, 전원을 껐다 켠 뒤 CPU1_FLASH 단독 실행.
- 점퍼선이 길어 12.5 MHz에서 오류가 나면 `mmc_F28P65x.c`의 SPI 고속 설정을 낮춰 보세요(각 프로젝트 README 참고).

## 관련 링크
- 상품 페이지: https://tms320f28x.co.kr/goods/goods_view.php?goodsNo=200903127
- 게시판 글: https://tms320f28x.co.kr/board/view.php?bdId=tms320f28xevmv2&sno=113

## 프로세서 모듈

- [TMS320F28P650DK9 모듈(산업용)](https://tms320f28x.co.kr/goods/goods_view.php?goodsNo=200903200)
- [TMS320F28P659DK8-Q1 모듈(차량 전장용)](https://tms320f28x.co.kr/goods/goods_view.php?goodsNo=200903201)

## 개발 환경

CCS 21.x(Theia 기반) / C2000Ware 26.00.00.00

## Import 방법 (zip 직접 import도 지원)

압축을 미리 풀어서 "Select search-directory"로 지정하거나, GitHub에서 받은 zip 파일을
그대로 CCS의 "Select archive file"로 지정해도 됩니다.
