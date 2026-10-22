// SPDX-License-Identifier: MIT
// Licensed under the MIT License. See LICENSE for the full text.

//*****************************************************************************
//
// QFARCH.C
//
// Bounds-checked Linux archive directory readers. No files are extracted.
// Copyright 1989-95 Ronald C. Bieber.
//
//*****************************************************************************
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "QFLINUX.H"

#include "QFARCH.H"

typedef void (*PFNARCHIVESEARCH)(PARCHIVEREADER);

static void SearchARC(PARCHIVEREADER);
static void SearchZIP(PARCHIVEREADER);
static void SearchZOO(PARCHIVEREADER);
static void SearchDWC(PARCHIVEREADER);
static void SearchLZH(PARCHIVEREADER);

typedef struct tagJUMPTABLEENTRY {
   const char *pszExtension;
   PFNARCHIVESEARCH pfnSearch;
   const char *pszDescription;
} JUMPTABLEENTRY;

static const JUMPTABLEENTRY archiveSearch[] = {
   { "ARC", SearchARC, "Traditional ARC records, including version-1 headers" },
   { "PAK", SearchARC, "ARC-compatible PAK records" },
   { "ZIP", SearchZIP, "Single-disk ZIP central directories; no ZIP64" },
   { "ZOO", SearchZOO, "Type-1/type-2 entries, including long names" },
   { "DWC", SearchDWC, "Traditional DWC trailer/directory format" },
   { "LZH", SearchLZH, "Level-0 LZH headers only" },
   { "LZS", SearchLZH, "Level-0 LARC headers only" },
   { "ARJ", SearchARJ, "Single-volume ARJ with readable headers" },
   { "CAB", SearchCAB, "Single-cabinet Microsoft Cabinet file tables" },
   { "RAR", SearchRAR, "RAR 1.5-4.x; no RAR5, split volumes, or encrypted headers" },
   { "TAR", SearchTAR, "Uncompressed TAR: V7, USTAR, GNU long names, and PAX" }
};

void ShowArchiveFormats(void)
{
   size_t iEntry;

   puts("Supported archive formats (filename searches without extraction):");
   for (iEntry = 0; iEntry < sizeof(archiveSearch) / sizeof(archiveSearch[0]); iEntry++)
      printf("   .%-3s  %s\n", archiveSearch[iEntry].pszExtension,
             archiveSearch[iEntry].pszDescription);
   puts("ZIP64, split archives, newer LZH/LZS headers, RAR5, and compressed TAR are unsupported.");
}

uint16_t ReadWord(const unsigned char *pbyData)
{
   return ((uint16_t)((uint16_t)pbyData[0] | (uint16_t)pbyData[1] << 8));
}

uint32_t ReadDWord(const unsigned char *pbyData)
{
   return ((uint32_t)pbyData[0] | (uint32_t)pbyData[1] << 8 |
           (uint32_t)pbyData[2] << 16 | (uint32_t)pbyData[3] << 24);
}

void ArchiveError(PARCHIVEREADER par, const char *pszMessage)
{
   PSTR pszPath;

   if (par->bFailed || bInterrupted)
      return;
   par->bFailed = TRUE;
   pszPath = JoinPath(par->pszDirectory, par->pszArchive);
   if (pszPath) {
      ReportError(pszPath, pszMessage);
      free(pszPath);
   }
}

BOOL ReadAt(PARCHIVEREADER par, uint64_t ullOffset,
                    void *pvData, size_t cbData)
{
   if (bInterrupted || par->bFailed)
      return (FALSE);
   if (ullOffset > par->ullLength || cbData > par->ullLength - ullOffset) {
      ArchiveError(par, "archive corrupted: truncated record or invalid offset");
      return (FALSE);
   }
   if (fseeko(par->pfArchive, (off_t)ullOffset, SEEK_SET) ||
       fread(pvData, 1, cbData, par->pfArchive) != cbData) {
      ArchiveError(par, "unable to read archive");
      return (FALSE);
   }
   return (TRUE);
}

BOOL CheckRange(PARCHIVEREADER par, uint64_t ullOffset, uint64_t ullSize)
{
   if (ullOffset > par->ullLength || ullSize > par->ullLength - ullOffset) {
      ArchiveError(par, "archive corrupted: invalid entry size");
      return (FALSE);
   }
   return (TRUE);
}

time_t DOSDateTime(uint16_t usDate, uint16_t usTime)
{
   struct tm tmFile;
   unsigned usMonth = (usDate >> 5) & 15, usDay = usDate & 31;

   if (!usMonth || usMonth > 12 || !usDay ||
       (usTime >> 11) > 23 || ((usTime >> 5) & 63) > 59 ||
       (usTime & 31) > 29)
      return ((time_t)-1);
   memset(&tmFile, 0, sizeof(tmFile));
   tmFile.tm_year = (usDate >> 9) + 80;
   tmFile.tm_mon = (int)usMonth - 1;
   tmFile.tm_mday = (int)usDay;
   tmFile.tm_hour = usTime >> 11;
   tmFile.tm_min = (usTime >> 5) & 63;
   tmFile.tm_sec = (usTime & 31) * 2;
   tmFile.tm_isdst = -1;
   return (mktime(&tmFile));
}

static BOOL FixedName(PARCHIVEREADER par, char *pszName,
                      const unsigned char *pbyName, size_t cbName)
{
   const unsigned char *pbyEnd = memchr(pbyName, 0, cbName);
   size_t cbLength;

   if (!pbyEnd || pbyEnd == pbyName) {
      ArchiveError(par, "archive corrupted: invalid filename");
      return (FALSE);
   }
   cbLength = (size_t)(pbyEnd - pbyName);
   memcpy(pszName, pbyName, cbLength);
   pszName[cbLength] = 0;
   return (TRUE);
}

void ShowEntry(PARCHIVEREADER par, const char *pszName,
                      uint64_t ullSize, time_t tModified)
{
   const char *pszBase = pszName, *pszNext;
   size_t cbName = strlen(pszName);

   if (!cbName || pszName[cbName - 1] == '/' ||
       (!par->bUnixPaths && pszName[cbName - 1] == '\\'))
      return;
   for (pszNext = pszName; *pszNext; pszNext++)
      if (*pszNext == '/' || (!par->bUnixPaths && *pszNext == '\\'))
         pszBase = pszNext + 1;
   if (!Match(opt.pszPattern, pszBase) || bInterrupted)
      return;
   FoundArchiveEntry(par->pszDirectory, par->pszArchive, par->pstArchive,
                     pszName, ullSize, tModified, par->ptotalsDirectory,
                     par->pbPrinted, par->bFirst);
   par->bFirst = FALSE;
}

static void SearchARC(PARCHIVEREADER par)
{
   unsigned char byHeader[29];
   char szName[13];
   uint64_t ullOffset = 0, ullSize, ullOriginal;
   size_t cbHeader;

   while (!bInterrupted && ReadAt(par, ullOffset, byHeader, 2)) {
      if (byHeader[0] != 26 || byHeader[1] > 127) {
         ArchiveError(par, "archive corrupted: invalid ARC header");
         return;
      }
      if (!byHeader[1])
         return;
      cbHeader = byHeader[1] == 1 ? 25 : 29;
      if (!ReadAt(par, ullOffset, byHeader, cbHeader) ||
          !FixedName(par, szName, byHeader + 2, 13))
         return;
      ullSize = ReadDWord(byHeader + 15);
      ullOriginal = cbHeader == 25 ? ullSize : ReadDWord(byHeader + 25);
      if (!CheckRange(par, ullOffset + cbHeader, ullSize))
         return;
      ShowEntry(par, szName, ullOriginal,
                 DOSDateTime(ReadWord(byHeader + 19), ReadWord(byHeader + 21)));
      ullOffset += cbHeader + ullSize;
   }
}

static BOOL FindEOCD(PARCHIVEREADER par, unsigned char *pbyEOCD,
                      uint64_t *pullOffset)
{
   size_t cbTail = par->ullLength > 65557 ? 65557 : (size_t)par->ullLength;
   unsigned char *pbyTail;
   size_t iIndex;
   BOOL bFound = FALSE;

   if (cbTail < 22) {
      ArchiveError(par, "archive corrupted: missing ZIP end record");
      return (FALSE);
   }
   pbyTail = malloc(cbTail);
   if (!pbyTail) {
      ArchiveError(par, "out of memory");
      return (FALSE);
   }
   if (ReadAt(par, par->ullLength - cbTail, pbyTail, cbTail)) {
      iIndex = cbTail - 22;
      for (;;) {
         if (ReadDWord(pbyTail + iIndex) == UINT32_C(0x06054b50) &&
             ReadWord(pbyTail + iIndex + 20) == cbTail - iIndex - 22 &&
             (ReadWord(pbyTail + iIndex + 4) ||
              ReadWord(pbyTail + iIndex + 6) ||
              ReadWord(pbyTail + iIndex + 10) == UINT16_MAX ||
              ReadDWord(pbyTail + iIndex + 12) == UINT32_MAX ||
              ReadDWord(pbyTail + iIndex + 16) == UINT32_MAX ||
              (uint64_t)ReadDWord(pbyTail + iIndex + 12) +
              ReadDWord(pbyTail + iIndex + 16) == par->ullLength - cbTail + iIndex)) {
            memcpy(pbyEOCD, pbyTail + iIndex, 22);
            *pullOffset = par->ullLength - cbTail + iIndex;
            bFound = TRUE;
            break;
         }
         if (!iIndex-- || bInterrupted)
            break;
      }
   }
   free(pbyTail);
   if (!bFound)
      ArchiveError(par, "archive corrupted: missing ZIP end record");
   return (bFound);
}

static void SearchZIP(PARCHIVEREADER par)
{
   unsigned char byEnd[22], byHeader[46], byLocal[30];
   uint64_t ullEnd, ullOffset, ullCentralEnd, ullLocal;
   uint32_t ulCount, iIndex;
   size_t cbName;
   PSTR pszName;

   if (!FindEOCD(par, byEnd, &ullEnd))
      return;
   if (ReadWord(byEnd + 8) == UINT16_MAX ||
       ReadWord(byEnd + 10) == UINT16_MAX ||
       ReadDWord(byEnd + 12) == UINT32_MAX || ReadDWord(byEnd + 16) == UINT32_MAX) {
      ArchiveError(par, "ZIP64 archives are not supported");
      return;
   }
   if (ReadWord(byEnd + 4) || ReadWord(byEnd + 6) ||
       ReadWord(byEnd + 8) != ReadWord(byEnd + 10)) {
      ArchiveError(par, "multi-disk ZIP archives are not supported");
      return;
   }
   ulCount = ReadWord(byEnd + 10);
   ullOffset = ReadDWord(byEnd + 16);
   ullCentralEnd = ullOffset + ReadDWord(byEnd + 12);
   if (ullCentralEnd > ullEnd ||
       (uint64_t)ulCount * sizeof(byHeader) > ullCentralEnd - ullOffset) {
      ArchiveError(par, "archive corrupted: invalid ZIP directory");
      return;
   }
   for (iIndex = 0; iIndex < ulCount && !bInterrupted; iIndex++) {
      if (ullOffset > ullCentralEnd || sizeof(byHeader) > ullCentralEnd - ullOffset ||
          !ReadAt(par, ullOffset, byHeader, sizeof(byHeader))) {
         ArchiveError(par, "archive corrupted: truncated ZIP directory");
         return;
      }
      if (ReadDWord(byHeader) != UINT32_C(0x02014b50)) {
         ArchiveError(par, "archive corrupted: invalid ZIP entry");
         return;
      }
      if (ReadDWord(byHeader + 20) == UINT32_MAX ||
          ReadDWord(byHeader + 24) == UINT32_MAX ||
          ReadDWord(byHeader + 42) == UINT32_MAX) {
         ArchiveError(par, "ZIP64 entries are not supported");
         return;
      }
      if (ReadWord(byHeader + 34)) {
         ArchiveError(par, "multi-disk ZIP entries are not supported");
         return;
      }
      ullOffset += sizeof(byHeader);
      cbName = ReadWord(byHeader + 28);
      if (!cbName || cbName + (uint64_t)ReadWord(byHeader + 30) +
          ReadWord(byHeader + 32) > ullCentralEnd - ullOffset) {
         ArchiveError(par, "archive corrupted: invalid ZIP filename length");
         return;
      }
      pszName = malloc(cbName + 1);
      if (!pszName) {
         ArchiveError(par, "out of memory");
         return;
      }
      if (!ReadAt(par, ullOffset, pszName, cbName)) {
         free(pszName);
         return;
      }
      pszName[cbName] = 0;
      if (memchr(pszName, 0, cbName)) {
         ArchiveError(par, "archive corrupted: embedded NUL in filename");
         free(pszName);
         return;
      }
      ullLocal = ReadDWord(byHeader + 42);
      if (!ReadAt(par, ullLocal, byLocal, sizeof(byLocal)) ||
          ReadDWord(byLocal) != UINT32_C(0x04034b50) ||
          ullLocal + sizeof(byLocal) + ReadWord(byLocal + 26) +
          (uint64_t)ReadWord(byLocal + 28) + ReadDWord(byHeader + 20) >
          ReadDWord(byEnd + 16)) {
         ArchiveError(par, "archive corrupted: invalid ZIP data offset");
         free(pszName);
         return;
      }
      ShowEntry(par, pszName, ReadDWord(byHeader + 24),
                 DOSDateTime(ReadWord(byHeader + 14), ReadWord(byHeader + 12)));
      free(pszName);
      ullOffset += cbName + (uint64_t)ReadWord(byHeader + 30) + ReadWord(byHeader + 32);
   }
}

static void SearchDWC(PARCHIVEREADER par)
{
   unsigned char byEnd[27], byHeader[34];
   uint64_t ullOffset, ullCount, iIndex;
   unsigned usFooter, usEntry;
   char szName[13];
   uint32_t ulTime;
   int64_t llTime;

   if (par->ullLength < sizeof(byEnd) ||
       !ReadAt(par, par->ullLength - sizeof(byEnd), byEnd, sizeof(byEnd))) {
      ArchiveError(par, "archive corrupted: missing DWC trailer");
      return;
   }
   usFooter = ReadWord(byEnd);
   usEntry = byEnd[2];
   ullCount = ReadDWord(byEnd + 20);
   if (memcmp(byEnd + 24, "DWC", 3) || usFooter < sizeof(byEnd) ||
       usEntry < sizeof(byHeader) || ullCount > INT32_MAX ||
       usFooter + ullCount * usEntry > par->ullLength) {
      ArchiveError(par, "archive corrupted: invalid DWC directory");
      return;
   }
   ullOffset = par->ullLength - usFooter - ullCount * usEntry;
   for (iIndex = 0; iIndex < ullCount && !bInterrupted; iIndex++) {
      if (!ReadAt(par, ullOffset, byHeader, sizeof(byHeader)) ||
          !FixedName(par, szName, byHeader, 13))
         return;
      if (!CheckRange(par, ReadDWord(byHeader + 25), ReadDWord(byHeader + 21)))
         return;
      ulTime = ReadDWord(byHeader + 17);
      llTime = ulTime >= UINT32_C(0x80000000) ? (int64_t)ulTime - INT64_C(4294967296) :
                                             (int64_t)ulTime;
      ShowEntry(par, szName, ReadDWord(byHeader + 13), (time_t)llTime);
      ullOffset += usEntry;
   }
}

static void SearchLZH(PARCHIVEREADER par)
{
   unsigned char byHeader[257];
   char szName[256];
   uint64_t ullOffset = 0;
   size_t cbHeader, cbName, iIndex;
   unsigned usChecksum;

   while (!bInterrupted && ReadAt(par, ullOffset, byHeader, 1)) {
      if (!byHeader[0])
         return;
      cbHeader = (size_t)byHeader[0] + 2;
      if (cbHeader < 24 || !ReadAt(par, ullOffset, byHeader, cbHeader)) {
         ArchiveError(par, "archive corrupted: short LZH header");
         return;
      }
      if (byHeader[2] != '-' || byHeader[3] != 'l' ||
          (byHeader[4] != 'h' && byHeader[4] != 'z') || byHeader[6] != '-') {
         ArchiveError(par, "archive corrupted: invalid LZH method");
         return;
      }
      if (byHeader[20]) {
         ArchiveError(par, "only level-0 LZH/LZS headers are supported");
         return;
      }
      usChecksum = 0;
      for (iIndex = 2; iIndex < cbHeader; iIndex++)
         usChecksum += byHeader[iIndex];
      if ((usChecksum & 255) != byHeader[1]) {
         ArchiveError(par, "archive corrupted: invalid LZH header checksum");
         return;
      }
      cbName = byHeader[21];
      if (!cbName || cbName + 24 > cbHeader || memchr(byHeader + 22, 0, cbName)) {
         ArchiveError(par, "archive corrupted: invalid LZH filename");
         return;
      }
      memcpy(szName, byHeader + 22, cbName);
      szName[cbName] = 0;
      if (!CheckRange(par, ullOffset + cbHeader, ReadDWord(byHeader + 7)))
         return;
      ShowEntry(par, szName, ReadDWord(byHeader + 11),
                 DOSDateTime(ReadWord(byHeader + 17), ReadWord(byHeader + 15)));
      ullOffset += cbHeader + (uint64_t)ReadDWord(byHeader + 7);
   }
}

static BOOL RememberOffset(PARCHIVEREADER par, uint64_t **ppullSeen,
                           size_t *pcSeen, size_t *pcAllocated, uint64_t ullOffset)
{
   size_t iLow = 0, iHigh = *pcSeen, iMiddle;
   uint64_t *pullNew;

   while (iLow < iHigh) {
      iMiddle = iLow + (iHigh - iLow) / 2;
      if ((*ppullSeen)[iMiddle] < ullOffset)
         iLow = iMiddle + 1;
      else
         iHigh = iMiddle;
   }
   if (iLow < *pcSeen && (*ppullSeen)[iLow] == ullOffset) {
      ArchiveError(par, "archive corrupted: ZOO directory cycle");
      return (FALSE);
   }
   if (*pcSeen == *pcAllocated) {
      if (*pcAllocated > SIZE_MAX / sizeof(uint64_t) / 2) {
         ArchiveError(par, "ZOO directory too large");
         return (FALSE);
      }
      iHigh = *pcAllocated ? *pcAllocated * 2 : 32;
      pullNew = realloc(*ppullSeen, iHigh * sizeof(uint64_t));
      if (!pullNew) {
         ArchiveError(par, "out of memory");
         return (FALSE);
      }
      *ppullSeen = pullNew;
      *pcAllocated = iHigh;
   }
   memmove(*ppullSeen + iLow + 1, *ppullSeen + iLow,
           (*pcSeen - iLow) * sizeof(uint64_t));
   (*ppullSeen)[iLow] = ullOffset;
   (*pcSeen)++;
   return (TRUE);
}

static void SearchZOO(PARCHIVEREADER par)
{
   unsigned char byHeader[51], byVariable[7];
   uint64_t ullOffset, ullNext, *pullSeen = NULL;
   size_t cSeen = 0, cAllocated = 0, cbName, cbDirectory;
   char szName[256], szDirectory[256], szFullName[513];

   if (!ReadAt(par, 0, byHeader, 34))
      return;
   if (ReadDWord(byHeader + 20) != UINT32_C(0xfdc4a7dc) ||
       (uint32_t)(ReadDWord(byHeader + 24) + ReadDWord(byHeader + 28)) != 0) {
      ArchiveError(par, "archive corrupted or unsupported ZOO header");
      return;
   }
   ullOffset = ReadDWord(byHeader + 24);
   if (ullOffset < 34 || (ullOffset > 34 &&
       (!ReadAt(par, 34, byHeader + 34, 1) || byHeader[34] != 1))) {
      ArchiveError(par, "archive corrupted or unsupported ZOO header");
      return;
   }
   while (!bInterrupted) {
      if (!RememberOffset(par, &pullSeen, &cSeen, &cAllocated, ullOffset) ||
          !ReadAt(par, ullOffset, byHeader, sizeof(byHeader)))
         break;
      if (ReadDWord(byHeader) != UINT32_C(0xfdc4a7dc) ||
          (byHeader[4] != 1 && byHeader[4] != 2)) {
         ArchiveError(par, "archive corrupted: invalid ZOO entry");
         break;
      }
      ullNext = ReadDWord(byHeader + 6);
      if (!ullNext)
         break;
      if (!CheckRange(par, ReadDWord(byHeader + 10), ReadDWord(byHeader + 24)) ||
          !FixedName(par, szName, byHeader + 38, 13))
         break;
      *szFullName = 0;
      if (byHeader[4] == 2) {
         if (!ReadAt(par, ullOffset + 51, byVariable, sizeof(byVariable)))
            break;
         cbName = byVariable[5];
         if (ReadWord(byVariable) < 2 ||
             cbName + (size_t)byVariable[6] > (size_t)ReadWord(byVariable) - 2 ||
             !CheckRange(par, ullOffset + 56, ReadWord(byVariable))) {
            ArchiveError(par, "archive corrupted: invalid ZOO variable header");
            break;
         }
         if (cbName) {
            if (!ReadAt(par, ullOffset + 58, szName, cbName))
               break;
            if (szName[cbName - 1] == 0)
               cbName--;
            if (memchr(szName, 0, cbName)) {
               ArchiveError(par, "archive corrupted: embedded NUL in ZOO filename");
               break;
            }
            szName[cbName] = 0;
         }
         cbDirectory = byVariable[6];
         if (cbDirectory) {
            if (!ReadAt(par, ullOffset + 58 + byVariable[5],
                         szDirectory, cbDirectory))
               break;
            if (!szDirectory[cbDirectory - 1])
               cbDirectory--;
            if (!cbDirectory || memchr(szDirectory, 0, cbDirectory)) {
               ArchiveError(par, "archive corrupted: invalid ZOO directory name");
               break;
            }
            szDirectory[cbDirectory] = 0;
            snprintf(szFullName, sizeof(szFullName), "%s%s%s", szDirectory,
                      szDirectory[cbDirectory - 1] == '/' ||
                      szDirectory[cbDirectory - 1] == '\\' ? "" : "/", szName);
         }
      }
      if (!byHeader[30])
         ShowEntry(par, *szFullName ? szFullName : szName, ReadDWord(byHeader + 20),
                    DOSDateTime(ReadWord(byHeader + 14), ReadWord(byHeader + 16)));
      ullOffset = ullNext;
   }
   free(pullSeen);
}

int GetArchiveSearchFunction(const char *pszFilename)
{
   const char *pszExtension = strrchr(pszFilename, '.');
   size_t iEntry;

   if (pszExtension)
      for (iEntry = 0; iEntry < sizeof(archiveSearch) / sizeof(archiveSearch[0]); iEntry++)
         if (!strcasecmp(pszExtension + 1, archiveSearch[iEntry].pszExtension))
            return ((int)iEntry);
   return (-1);
}

void ReadArchive(int iArchive, FILE *pfArchive, const char *pszDirectory,
                 const char *pszArchive, const struct stat *pstArchive,
                 TOTALS *ptotalsDirectory, BOOL *pbPrinted)
{
   ARCHIVEREADER ar;

   memset(&ar, 0, sizeof(ar));
   ar.pfArchive = pfArchive;
   ar.pszDirectory = pszDirectory;
   ar.pszArchive = pszArchive;
   ar.pstArchive = pstArchive;
   ar.ptotalsDirectory = ptotalsDirectory;
   ar.pbPrinted = pbPrinted;
   ar.bFirst = TRUE;
   ar.ullLength = (uint64_t)pstArchive->st_size;
   (*archiveSearch[iArchive].pfnSearch)(&ar);
}
