// 파일이름	:	cmdline.h
// 대상장치	:	TMS320F28X EVM V2, TMS320F28388D 초소형 모듈
// 파일버전	:	0.90 (Beta)
// 갱신&설명	:	2021-06-08, 버전 0.90
//				- Prototypes for command line processing functions.
//				- TI가 F2837xD Support Library v3.11.00.00에 수록한 헤더파일
//				- TMS320F28388D를 대상으로 테스트					
//************************************************************************************************************************************************************************

#ifndef __CMDLINE_H__
#define __CMDLINE_H__

//************************************************************************************************************************************************************************
//
// If building with a C++ compiler, make all of the definitions in this header have a C binding.
//
//************************************************************************************************************************************************************************
#ifdef __cplusplus
extern "C"
{
#endif

// Defines the value that is returned if the command is not found.
#define CMDLINE_BAD_CMD			(-1)

// Defines the value that is returned if there are too many arguments.
#define CMDLINE_TOO_MANY_ARGS	(-2)

// Defines the value that is returned if there are too few arguments.
#define CMDLINE_TOO_FEW_ARGS	(-3)

// Defines the value that is returned if an argument is invalid.
#define CMDLINE_INVALID_ARG		(-4)

// Command line function callback type.
typedef int (*pfnCmdLine)(int argc, char *argv[]);

// Structure for an entry in the command list table.
typedef struct
{
	// A pointer to a string containing the name of the command.
	const char *pcCmd;

	// A function pointer to the implementation of the command.
	pfnCmdLine pfnCmd;

	// A pointer to a string of brief help text for the command.
	const char *pcHelp;
} tCmdLineEntry;

// This is the command table that must be provided by the application.
// The last element of the array must be a structure whose pcCmd field contains a NULL pointer.
extern tCmdLineEntry g_psCmdTable[];

// Function Prototypes
extern int CmdLineProcess(char *pcCmdLine);

// Mark the end of the C bindings section for C++ compilers.
#ifdef __cplusplus
}
#endif

#endif // __CMDLINE_H__

// End of file.
