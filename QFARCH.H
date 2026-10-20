//*****************************************************************************
//
// QFARCH.H -- private native archive-reader interface.
// Copyright 1989-95 Ronald C. Bieber; All rights reserved.
// See QF.C for the original copyright and distribution notice.
//
//*****************************************************************************
#ifndef __QFARCH_H
#define __QFARCH_H

#include "QFLINUX.H"

typedef struct tagARCHIVEREADER {
   FILE *pfArchive;
   const char *pszDirectory;
   const char *pszArchive;
   const struct stat *pstArchive;
   TOTALS *ptotalsDirectory;
   BOOL *pbPrinted;
   BOOL bFirst;
   BOOL bFailed;
   BOOL bUnixPaths;
   uint64_t ullLength;
} ARCHIVEREADER;

typedef ARCHIVEREADER *PARCHIVEREADER;
uint16_t ReadWord(const unsigned char *);
uint32_t ReadDWord(const unsigned char *);
void ArchiveError(PARCHIVEREADER, const char *);
BOOL ReadAt(PARCHIVEREADER, uint64_t, void *, size_t);
BOOL CheckRange(PARCHIVEREADER, uint64_t, uint64_t);
time_t DOSDateTime(uint16_t, uint16_t);
void ShowEntry(PARCHIVEREADER, const char *, uint64_t, time_t);
void SearchARJ(PARCHIVEREADER);
void SearchCAB(PARCHIVEREADER);
void SearchRAR(PARCHIVEREADER);
void SearchTAR(PARCHIVEREADER);

#endif
