// 파일이름	:	ustdlib.h
// 대상장치	:	TMS320F28X EVM V2, TMS320F28388D 초소형 모듈
// 파일버전	:	0.90 (Beta)
// 갱신&설명	:	2021-06-08, 버전 0.90
//				- Prototypes for simple standard library functions.
//				- TI가 F2837xD Support Library v3.11.00.00에 수록한 헤더파일
//				- TMS320F28388D를 대상으로 테스트					
//************************************************************************************************************************************************************************

#ifndef __USTDLIB_H__
#define __USTDLIB_H__

// Included Files
#include <stdarg.h>
#include <time.h>

// If building with a C++ compiler, make all of the definitions in this header have a C binding.
#ifdef __cplusplus
extern "C"
{
#endif

// Function Prototypes
extern void ulocaltime(time_t timer, struct tm *tm);
extern time_t umktime(struct tm *timeptr);
extern int urand(void);
extern int usnprintf(char * restrict s, size_t n, const char * restrict format, ...);
extern int usprintf(char * restrict s, const char * restrict format, ...);
extern void usrand(unsigned int seed);
extern int ustrcasecmp(const char *s1, const char *s2);
extern int ustrcmp(const char *s1, const char *s2);
extern size_t ustrlen(const char *s);
extern int ustrncasecmp(const char *s1, const char *s2, size_t n);
extern int ustrncmp(const char *s1, const char *s2, size_t n);
extern char *ustrncpy(char * restrict s1, const char * restrict s2, size_t n);
extern char *ustrstr(const char *s1, const char *s2);
extern float ustrtof(const char * restrict nptr, const char ** restrict endptr);
extern unsigned long int ustrtoul(const char * restrict nptr, const char ** restrict endptr, int base);
extern int uvsnprintf(char * restrict s, size_t n, const char * restrict format, va_list arg);

// Mark the end of the C bindings section for C++ compilers.
#ifdef __cplusplus
}
#endif

#endif // __USTDLIB_H__

// End of file.
