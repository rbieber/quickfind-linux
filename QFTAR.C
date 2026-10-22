// SPDX-License-Identifier: MIT
// Licensed under the MIT License. See LICENSE for the full text.

//*****************************************************************************
//
// QFTAR.C -- uncompressed V7, USTAR, GNU, and PAX filename searching.
// Copyright 1989-95 Ronald C. Bieber.
//
//*****************************************************************************
#include <stdlib.h>
#include <string.h>

#include "QFARCH.H"

#define TAR_BLOCK_SIZE 512
#define TAR_METADATA_LIMIT (16U * 1024U * 1024U)

typedef struct tagTAROPTIONS {
   char *pszPath;
   BOOL bSize;
   BOOL bTime;
   uint64_t ullSize;
   int64_t llTime;
} TAROPTIONS;

static BOOL TarNumber(PARCHIVEREADER par, const unsigned char *pbyData,
                       size_t cbData, BOOL bSigned, int64_t *pllValue)
{
   uint64_t ullValue = 0;
   size_t iIndex = 0;
   BOOL bNegative = FALSE, bTrailing = FALSE;
   unsigned byChar;

   if (pbyData[0] & 128) {
      bNegative = (pbyData[0] & 64) != 0;
      if (cbData < 9 || (!bSigned && bNegative))
         goto invalid;
      for (iIndex = 0; iIndex < cbData - 8; iIndex++)
         if (pbyData[iIndex] != (bNegative ? 255 : (iIndex ? 0 : 128)))
            goto invalid;
      for (; iIndex < cbData; iIndex++)
         ullValue = (ullValue << 8) | pbyData[iIndex];
      if (bNegative) {
         if (ullValue <= INT64_MAX)
            goto invalid;
         *pllValue = -(int64_t)(~ullValue) - 1;
         return (TRUE);
      }
   } else {
      while (iIndex < cbData && pbyData[iIndex] == ' ')
         iIndex++;
      for (; iIndex < cbData; iIndex++) {
         byChar = pbyData[iIndex];
         if (!byChar || byChar == ' ')
            bTrailing = TRUE;
         else if (bTrailing || byChar < '0' || byChar > '7' ||
                  ullValue > ((uint64_t)INT64_MAX - (byChar - '0')) / 8)
            goto invalid;
         else
            ullValue = ullValue * 8 + byChar - '0';
      }
   }
   if (ullValue > INT64_MAX)
      goto invalid;
   *pllValue = (int64_t)ullValue;
   return (TRUE);
invalid:
   ArchiveError(par, "archive corrupted: invalid or overflowing TAR number");
   return (FALSE);
}

static BOOL TarDecimal(const char *pszData, size_t cbData, BOOL bTime,
                        uint64_t *pullValue, BOOL *pbNegative)
{
   size_t iIndex = 0, cDigits = 0;
   uint64_t ullValue = 0;
   BOOL bFraction = FALSE;

   *pbNegative = FALSE;
   if (bTime && cbData && pszData[0] == '-') {
      *pbNegative = TRUE;
      iIndex++;
   }
   for (; iIndex < cbData && pszData[iIndex] >= '0' && pszData[iIndex] <= '9'; iIndex++) {
      if (ullValue > (UINT64_MAX - (unsigned)(pszData[iIndex] - '0')) / 10)
         return (FALSE);
      ullValue = ullValue * 10 + (unsigned)(pszData[iIndex] - '0');
      cDigits++;
   }
   if (!cDigits)
      return (FALSE);
   if (bTime && iIndex < cbData && pszData[iIndex] == '.') {
      if (++iIndex == cbData)
         return (FALSE);
      for (; iIndex < cbData; iIndex++) {
         if (pszData[iIndex] < '0' || pszData[iIndex] > '9')
            return (FALSE);
         bFraction |= pszData[iIndex] != '0';
      }
   }
   if (iIndex != cbData || (*pbNegative && bFraction && ullValue == UINT64_MAX))
      return (FALSE);
   if (*pbNegative && bFraction)
      ullValue++;                // Preserve the second containing a negative fraction.
   *pullValue = ullValue;
   return (TRUE);
}

static BOOL SetTarPath(PARCHIVEREADER par, TAROPTIONS *pto,
                       const char *pszData, size_t cbData)
{
   char *pszPath;

   if (!cbData || memchr(pszData, 0, cbData)) {
      ArchiveError(par, "archive corrupted: empty TAR path or embedded NUL");
      return (FALSE);
   }
   pszPath = malloc(cbData + 1);
   if (!pszPath) {
      ArchiveError(par, "out of memory");
      return (FALSE);
   }
   memcpy(pszPath, pszData, cbData);
   pszPath[cbData] = 0;
   free(pto->pszPath);
   pto->pszPath = pszPath;
   return (TRUE);
}

static BOOL ParsePAX(PARCHIVEREADER par, TAROPTIONS *pto,
                      char *pszData, size_t cbData)
{
   size_t iOffset = 0, iIndex, iStart, cbRecord, cbKey, cbValue;
   char *pszKey, *pszValue, *pszEqual;
   uint64_t ullValue;
   BOOL bNegative;

   while (iOffset < cbData && !bInterrupted) {
      iIndex = iOffset;
      cbRecord = 0;
      while (iIndex < cbData && pszData[iIndex] >= '0' && pszData[iIndex] <= '9') {
         if (cbRecord > (SIZE_MAX - (unsigned)(pszData[iIndex] - '0')) / 10)
            goto invalid;
         cbRecord = cbRecord * 10 + (unsigned)(pszData[iIndex++] - '0');
      }
      if (iIndex == iOffset || iIndex == cbData || pszData[iIndex++] != ' ' ||
          cbRecord > cbData - iOffset || cbRecord <= iIndex - iOffset + 2)
         goto invalid;
      iStart = iIndex;
      if (pszData[iOffset + cbRecord - 1] != '\n')
         goto invalid;
      pszKey = pszData + iStart;
      pszEqual = memchr(pszKey, '=', cbRecord - (iStart - iOffset) - 1);
      if (!pszEqual || pszEqual == pszKey)
         goto invalid;
      cbKey = (size_t)(pszEqual - pszKey);
      pszValue = pszEqual + 1;
      cbValue = iOffset + cbRecord - (size_t)(pszValue - pszData) - 1;
      if (memchr(pszKey, 0, cbKey) || memchr(pszValue, 0, cbValue))
         goto invalid;
      if ((cbKey >= 11 && !memcmp(pszKey, "GNU.sparse.", 11)) ||
          (cbKey == 14 && !memcmp(pszKey, "SCHILY.realsize", 14)) ||
          (cbKey == 14 && !memcmp(pszKey, "SCHILY.filetype", 14) &&
           cbValue == 6 && !memcmp(pszValue, "sparse", 6))) {
         ArchiveError(par, "sparse TAR entries are not supported");
         return (FALSE);
      }
      if (cbKey == 4 && !memcmp(pszKey, "path", 4)) {
         if (!SetTarPath(par, pto, pszValue, cbValue))
            return (FALSE);
      } else if (cbKey == 4 && !memcmp(pszKey, "size", 4)) {
         if (!TarDecimal(pszValue, cbValue, FALSE, &ullValue, &bNegative))
            goto invalid;
         pto->ullSize = ullValue;
         pto->bSize = TRUE;
      } else if (cbKey == 5 && !memcmp(pszKey, "mtime", 5)) {
         if (!TarDecimal(pszValue, cbValue, TRUE, &ullValue, &bNegative) ||
             ullValue > (uint64_t)INT64_MAX + (bNegative ? 1U : 0U))
            goto invalid;
         if (bNegative)
            pto->llTime = ullValue ? -(int64_t)(ullValue - 1) - 1 : 0;
         else
            pto->llTime = (int64_t)ullValue;
         pto->bTime = TRUE;
      }
      iOffset += cbRecord;
   }
   return (!bInterrupted);
invalid:
   ArchiveError(par, "archive corrupted: invalid PAX extended header");
   return (FALSE);
}

static BOOL IsZeroBlock(const unsigned char *pbyHeader)
{
   size_t iIndex;

   for (iIndex = 0; iIndex < TAR_BLOCK_SIZE; iIndex++)
      if (pbyHeader[iIndex])
         return (FALSE);
   return (TRUE);
}

void SearchTAR(PARCHIVEREADER par)
{
   unsigned char byHeader[TAR_BLOCK_SIZE];
   uint64_t ullOffset = 0, ullData, ullSize, ullPadded;
   int64_t llSize, llTime, llChecksum, llSigned;
   uint32_t ulChecksum;
   size_t iIndex, cbName, cbPrefix;
   unsigned byType;
   TAROPTIONS toGlobal = { NULL, FALSE, FALSE, 0, 0 };
   TAROPTIONS toLocal = { NULL, FALSE, FALSE, 0, 0 };
   char szName[257], *pszMetadata;
   const char *pszName, *pszEnd;
   BOOL bPending = FALSE;

   par->bUnixPaths = TRUE;       // A backslash is a legal POSIX filename byte.
   while (!bInterrupted && ReadAt(par, ullOffset, byHeader, sizeof(byHeader))) {
      if (IsZeroBlock(byHeader)) {
         if (!ReadAt(par, ullOffset + TAR_BLOCK_SIZE, byHeader, sizeof(byHeader)))
            break;
         if (!IsZeroBlock(byHeader) || bPending)
            ArchiveError(par, "archive corrupted: incomplete TAR end records or pending metadata");
         break;
      }
      if (!TarNumber(par, byHeader + 148, 8, FALSE, &llChecksum) ||
          !TarNumber(par, byHeader + 124, 12, FALSE, &llSize) ||
          !TarNumber(par, byHeader + 136, 12, TRUE, &llTime))
         break;
      ulChecksum = 0;
      llSigned = 0;
      for (iIndex = 0; iIndex < sizeof(byHeader); iIndex++) {
         if (iIndex >= 148 && iIndex < 156) {
            ulChecksum += ' ';
            llSigned += ' ';
         } else {
            ulChecksum += byHeader[iIndex];
            llSigned += byHeader[iIndex] < 128 ? byHeader[iIndex] :
                                              (int)byHeader[iIndex] - 256;
         }
      }
      if (llChecksum != ulChecksum && llChecksum != llSigned) {
         ArchiveError(par, "archive corrupted: invalid TAR header checksum");
         break;
      }
      byType = byHeader[156];
      if (byType == 'S') {
         ArchiveError(par, "sparse TAR entries are not supported");
         break;
      }
      ullData = ullOffset + TAR_BLOCK_SIZE;
      ullSize = (uint64_t)llSize;
      if (byType != 'x' && byType != 'g' && byType != 'L' && byType != 'K') {
         if (toGlobal.bSize)
            ullSize = toGlobal.ullSize;
         if (toLocal.bSize)
            ullSize = toLocal.ullSize;
      }
      if (!CheckRange(par, ullData, ullSize))
         break;
      ullPadded = ullSize + (TAR_BLOCK_SIZE - ullSize % TAR_BLOCK_SIZE) % TAR_BLOCK_SIZE;
      if (!CheckRange(par, ullData, ullPadded))
         break;
      if (byType == 'x' || byType == 'g' || byType == 'L' || byType == 'K') {
         if (ullSize > TAR_METADATA_LIMIT) {
            ArchiveError(par, "TAR extended metadata exceeds 16 MiB");
            break;
         }
         pszMetadata = malloc((size_t)ullSize + 1);
         if (!pszMetadata) {
            ArchiveError(par, "out of memory");
            break;
         }
         if (!ReadAt(par, ullData, pszMetadata, (size_t)ullSize)) {
            free(pszMetadata);
            break;
         }
         pszMetadata[ullSize] = 0;
         if (byType == 'x' || byType == 'g')
            ParsePAX(par, byType == 'x' ? &toLocal : &toGlobal,
                      pszMetadata, (size_t)ullSize);
         else if (byType == 'L') {
            pszEnd = memchr(pszMetadata, 0, (size_t)ullSize);
            if (!pszEnd)
               ArchiveError(par, "archive corrupted: unterminated GNU TAR long name");
            else
               SetTarPath(par, &toLocal, pszMetadata, (size_t)(pszEnd - pszMetadata));
         }
         free(pszMetadata);
         if (par->bFailed)
            break;
         if (byType != 'g')
            bPending = TRUE;
      } else {
         if (byType && (byType < '0' || byType > '7')) {
            ArchiveError(par, "unsupported TAR entry type");
            break;
         }
         cbName = strnlen((const char *)byHeader, 100);
         cbPrefix = !memcmp(byHeader + 257, "ustar\0", 6) ?
                     strnlen((const char *)byHeader + 345, 155) : 0;
         iIndex = 0;
         if (cbPrefix) {
            memcpy(szName, byHeader + 345, cbPrefix);
            iIndex = cbPrefix;
            szName[iIndex++] = '/';
         }
         memcpy(szName + iIndex, byHeader, cbName);
         szName[iIndex + cbName] = 0;
         pszName = toLocal.pszPath ? toLocal.pszPath :
                    (toGlobal.pszPath ? toGlobal.pszPath : szName);
         if (!*pszName) {
            ArchiveError(par, "archive corrupted: empty TAR filename");
            break;
         }
         if (toGlobal.bTime)
            llTime = toGlobal.llTime;
         if (toLocal.bTime)
            llTime = toLocal.llTime;
         if (byType != '5')
            ShowEntry(par, pszName, ullSize, (time_t)llTime);
         free(toLocal.pszPath);
         memset(&toLocal, 0, sizeof(toLocal));
         bPending = FALSE;
      }
      ullOffset = ullData + ullPadded;
   }
   free(toLocal.pszPath);
   free(toGlobal.pszPath);
}
