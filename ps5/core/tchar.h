// FBNeo PS5: FBNeo's TCHAR names for the core (the PS5 port is built without _UNICODE: TCHAR is char).
//
// SPDX-License-Identifier: MIT
#ifndef FBNEO_PS5_TCHAR_H
#define FBNEO_PS5_TCHAR_H

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <wchar.h>

typedef char TCHAR;
typedef char _TCHAR;

#define __TEXT(q) q
#define _TEXT(x) __TEXT(x)
#define _T(x) __TEXT(x)

#define _tcslen strlen
#define _tcscpy strcpy
#define _tcsncpy strncpy
#define _tcscat strcat
#define _tcschr strchr
#define _tcsrchr strrchr
#define _tcsstr strstr
#define _tcscmp strcmp
#define _tcsncmp strncmp
#define _tcsicmp strcasecmp
#define _tcsnicmp strncasecmp
#define _tcstol strtol
#define _tcstoul strtoul
#define _tcsdup strdup
#define _ttoi atoi

#define _tprintf printf
#define _stprintf sprintf
#define _sntprintf snprintf
#define _vstprintf vsprintf
#define _vsntprintf vsnprintf
#define _vsnprintf vsnprintf
#define _ftprintf fprintf
#define _tsprintf sprintf
#define _stscanf sscanf

#define _totlower(c) tolower((unsigned char)(c))
#define _totupper(c) toupper((unsigned char)(c))
#define _istspace isspace

#define _fgetts fgets
#define _fputts fputs
#define _fputtc fputc
#define _tfopen fopen
#define _tremove remove
#define _trename rename

#define _stricmp strcasecmp
#define stricmp strcasecmp
#define _strnicmp strncasecmp
#define strnicmp strncasecmp

#endif
