// SPDX-License-Identifier: MIT
// Licensed under the MIT License. See LICENSE for the full text.

//*****************************************************************************
//
// QFLINUX.H
//
// Native Linux types and declarations for Quick Find.
// Copyright 1989-95 Ronald C. Bieber.
//
//*****************************************************************************
#ifndef __QFLINUX_H
#define __QFLINUX_H

#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <signal.h>

#define VERSION "8.00-linux"
#define TRUE 1
#define FALSE 0
#define COMPRESSED_ONLY 0x8000U
#define COMPRESSED      0x4000U
#define FROM_CURDIR     0x2000U
#define PAUSE           0x1000U
#define ALLDRIVES       0x0800U
#define SUBDIRTOT       0x0100U
#define NOFILEINFO      0x0040U
#define FS_ARCHIVE      0x01
#define FS_MAINARCHIVE  0x02
#define FS_DIRECTORY    0x04

typedef int BOOL;
typedef char *PSTR;

typedef struct tagFILESTUFF {
   const char *pszName;
   const char *pszArchiveName;
   uint64_t ullSize;
   time_t tModified;
   unsigned char byType;
} FILESTUFF;

typedef FILESTUFF *PFILESTUFF;

typedef struct tagOPTIONS {
   unsigned usSwitches;
   BOOL bIgnoreCase;
   BOOL bDOSMatch;
   BOOL bCrossFilesystems;
   const char *pszPattern;
   const char *pszRoot;
} OPTIONS;

typedef struct tagTOTALS {
   uint64_t ullFiles;
   uint64_t ullDirectories;
   uint64_t ullBytes;
} TOTALS;

extern OPTIONS opt;
extern TOTALS totals;
extern volatile sig_atomic_t bInterrupted;
extern BOOL bError;

void ReportError(const char *, const char *);
PSTR JoinPath(const char *, const char *);
BOOL Match(const char *, const char *);
void BeginFileListing(void);
void ShowFileInfo(const FILESTUFF *);
void EndFileListing(void);
void ShowTotals(const TOTALS *, const char *);
BOOL AddTotal(uint64_t *, uint64_t);
void CloseTerminal(void);
void PrintText(const char *);
void PauseOutput(void);
int GetArchiveSearchFunction(const char *);
void ShowArchiveFormats(void);
void ReadArchive(int, FILE *, const char *, const char *, const struct stat *,
                 TOTALS *, BOOL *);
void FoundArchiveEntry(const char *, const char *, const struct stat *,
                       const char *, uint64_t, time_t, TOTALS *, BOOL *, BOOL);

#endif
