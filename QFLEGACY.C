//*****************************************************************************
//
// QFLEGACY.C -- ARJ, Microsoft Cabinet, and RAR4 metadata readers.
// Copyright 1989-95 Ronald C. Bieber; All rights reserved.
// See QF.C for the original copyright and distribution notice.
//
//*****************************************************************************
#include <stdlib.h>
#include <string.h>

#include "QFARCH.H"

static uint32_t HeaderCRC(const unsigned char *pbyData, size_t cbData)
{
   uint32_t ulCRC = UINT32_MAX;
   unsigned iBit;

   while (cbData--) {
      ulCRC ^= *pbyData++;
      for (iBit = 0; iBit < 8; iBit++)
         ulCRC = (ulCRC >> 1) ^ ((ulCRC & 1) ? UINT32_C(0xedb88320) : 0);
   }
   return (ulCRC ^ UINT32_MAX);
}

void SearchARJ(PARCHIVEREADER par)
{
   unsigned char byPrefix[4], *pbyHeader, *pbyExtra;
   uint64_t ullOffset = 0, ullData;
   size_t cbHeader, cbExtra, cbName;
   BOOL bMain = TRUE;
   const unsigned char *pbyEnd;
   time_t tModified;

   while (!bInterrupted && ReadAt(par, ullOffset, byPrefix, 4)) {
      if (ReadWord(byPrefix) != 0xea60) {
         ArchiveError(par, "archive corrupted: invalid ARJ signature");
         return;
      }
      cbHeader = ReadWord(byPrefix + 2);
      if (!cbHeader) {
         if (bMain)
            ArchiveError(par, "archive corrupted: missing ARJ main header");
         return;
      }
      if (cbHeader < 32) {
         ArchiveError(par, "archive corrupted: short ARJ header");
         return;
      }
      pbyHeader = malloc(cbHeader + 4);
      if (!pbyHeader) {
         ArchiveError(par, "out of memory");
         return;
      }
      if (!ReadAt(par, ullOffset + 4, pbyHeader, cbHeader + 4)) {
         free(pbyHeader);
         return;
      }
      if (HeaderCRC(pbyHeader, cbHeader) != ReadDWord(pbyHeader + cbHeader) ||
          pbyHeader[0] < 30 || pbyHeader[0] > cbHeader - 2 ||
          (bMain && pbyHeader[6] != 2)) {
         ArchiveError(par, "archive corrupted: invalid ARJ header or checksum");
         free(pbyHeader);
         return;
      }
      pbyEnd = memchr(pbyHeader + pbyHeader[0], 0, cbHeader - pbyHeader[0]);
      if (!pbyEnd || !memchr(pbyEnd + 1, 0,
                             cbHeader - (size_t)(pbyEnd + 1 - pbyHeader))) {
         ArchiveError(par, "archive corrupted: unterminated ARJ name or comment");
         free(pbyHeader);
         return;
      }
      cbName = (size_t)(pbyEnd - pbyHeader - pbyHeader[0]);
      if ((pbyHeader[4] & 0x04) || (!bMain && (pbyHeader[4] & 0x08))) {
         ArchiveError(par, "split ARJ archives are not supported");
         free(pbyHeader);
         return;
      }
      if (pbyHeader[4] & 0x80) {
         ArchiveError(par, "ARJ dual-name headers are not supported");
         free(pbyHeader);
         return;
      }
      ullData = ullOffset + 4 + cbHeader + 4;
      while (!bInterrupted && ReadAt(par, ullData, byPrefix, 2)) {
         ullData += 2;
         cbExtra = ReadWord(byPrefix);
         if (!cbExtra)
            break;
         pbyExtra = malloc(cbExtra + 4);
         if (!pbyExtra) {
            ArchiveError(par, "out of memory");
            break;
         }
         if (ReadAt(par, ullData, pbyExtra, cbExtra + 4) &&
             HeaderCRC(pbyExtra, cbExtra) != ReadDWord(pbyExtra + cbExtra))
            ArchiveError(par, "archive corrupted: invalid ARJ extended-header checksum");
         free(pbyExtra);
         if (par->bFailed)
            break;
         ullData += cbExtra + 4;
      }
      if (par->bFailed || bInterrupted) {
         free(pbyHeader);
         return;
      }
      if (bMain) {
         bMain = FALSE;
         ullOffset = ullData;
      } else {
         if (!cbName || (pbyHeader[6] != 0 && pbyHeader[6] != 1 && pbyHeader[6] != 3)) {
            ArchiveError(par, "archive corrupted or unsupported ARJ entry type");
            free(pbyHeader);
            return;
         }
         if (!CheckRange(par, ullData, ReadDWord(pbyHeader + 12))) {
            free(pbyHeader);
            return;
         }
         if (pbyHeader[3] == 2 || pbyHeader[3] == 8)
            tModified = (time_t)ReadDWord(pbyHeader + 8);
         else
            tModified = DOSDateTime(ReadWord(pbyHeader + 10), ReadWord(pbyHeader + 8));
         if (pbyHeader[6] != 3)
            ShowEntry(par, (const char *)pbyHeader + pbyHeader[0],
                       ReadDWord(pbyHeader + 16), tModified);
         ullOffset = ullData + ReadDWord(pbyHeader + 12);
      }
      free(pbyHeader);
   }
}

static char *ReadCABName(PARCHIVEREADER par, uint64_t *pullOffset, uint64_t ullEnd)
{
   unsigned char byBlock[256], *pbyEnd;
   char *pszName = NULL, *pszNew;
   size_t cbName = 0, cbBlock, cbCopy;

   while (*pullOffset < ullEnd && !bInterrupted) {
      cbBlock = ullEnd - *pullOffset > sizeof(byBlock) ? sizeof(byBlock) :
                                                        (size_t)(ullEnd - *pullOffset);
      if (!ReadAt(par, *pullOffset, byBlock, cbBlock))
         break;
      pbyEnd = memchr(byBlock, 0, cbBlock);
      cbCopy = pbyEnd ? (size_t)(pbyEnd - byBlock) : cbBlock;
      if (cbName + cbCopy > 65535) {
         ArchiveError(par, "CAB filename exceeds 65535 bytes");
         break;
      }
      pszNew = realloc(pszName, cbName + cbCopy + 1);
      if (!pszNew) {
         ArchiveError(par, "out of memory");
         break;
      }
      pszName = pszNew;
      memcpy(pszName + cbName, byBlock, cbCopy);
      cbName += cbCopy;
      *pullOffset += cbCopy;
      if (pbyEnd && cbName) {
         pszName[cbName] = 0;
         (*pullOffset)++;
         return (pszName);
      }
      if (pbyEnd)
         break;
   }
   ArchiveError(par, "archive corrupted: empty or unterminated CAB filename");
   free(pszName);
   return (NULL);
}

void SearchCAB(PARCHIVEREADER par)
{
   unsigned char byHeader[36], byFolder[8], byFile[16], byReserve[4];
   uint64_t ullOffset, ullFiles, ullData, ullCabinet;
   unsigned usFolders, usFiles, usFlags, usFolderReserve = 0, iIndex;
   char *pszName;

   if (!ReadAt(par, 0, byHeader, sizeof(byHeader)))
      return;
   ullCabinet = ReadDWord(byHeader + 8);
   ullFiles = ReadDWord(byHeader + 16);
   usFolders = ReadWord(byHeader + 26);
   usFiles = ReadWord(byHeader + 28);
   usFlags = ReadWord(byHeader + 30);
   if (memcmp(byHeader, "MSCF", 4) || byHeader[24] != 3 || byHeader[25] != 1 ||
       ullCabinet < 36 || ullCabinet > par->ullLength || ullFiles > ullCabinet ||
       (usFlags & ~7U) || (usFiles && !usFolders)) {
      ArchiveError(par, "archive corrupted or unsupported CAB header");
      return;
   }
   if ((usFlags & 3) || ReadWord(byHeader + 34)) {
      ArchiveError(par, "split CAB archives are not supported");
      return;
   }
   par->ullLength = ullCabinet;   // Ignore trailing signatures outside the cabinet.
   ullOffset = sizeof(byHeader);
   if (usFlags & 4) {
      if (!ReadAt(par, ullOffset, byReserve, sizeof(byReserve)))
         return;
      ullOffset += sizeof(byReserve) + ReadWord(byReserve);
      usFolderReserve = byReserve[2];
   }
   if (ullOffset > ullFiles ||
       (uint64_t)usFolders * (8 + usFolderReserve) > ullFiles - ullOffset) {
      ArchiveError(par, "archive corrupted: invalid CAB folder table");
      return;
   }
   ullData = ullCabinet;
   for (iIndex = 0; iIndex < usFolders && !bInterrupted; iIndex++) {
      if (!ReadAt(par, ullOffset, byFolder, sizeof(byFolder)))
         return;
      if (!CheckRange(par, ReadDWord(byFolder), (uint64_t)ReadWord(byFolder + 4) * 8))
         return;
      if (ReadWord(byFolder + 4) && ReadDWord(byFolder) < ullData)
         ullData = ReadDWord(byFolder);
      ullOffset += 8 + usFolderReserve;
   }
   ullOffset = ullFiles;
   for (iIndex = 0; iIndex < usFiles && !bInterrupted; iIndex++) {
      if (ullOffset > ullData || sizeof(byFile) > ullData - ullOffset ||
          !ReadAt(par, ullOffset, byFile, sizeof(byFile))) {
         ArchiveError(par, "archive corrupted: truncated CAB file table");
         return;
      }
      if (ReadWord(byFile + 8) >= usFolders) {
         ArchiveError(par, "invalid CAB folder index or split CAB entry");
         return;
      }
      ullOffset += sizeof(byFile);
      pszName = ReadCABName(par, &ullOffset, ullData);
      if (!pszName)
         return;
      ShowEntry(par, pszName, ReadDWord(byFile),
                 DOSDateTime(ReadWord(byFile + 10), ReadWord(byFile + 12)));
      free(pszName);
   }
}

static char *RARName(PARCHIVEREADER par, const unsigned char *pbyName,
                      size_t cbName, BOOL bUnicode)
{
   const unsigned char *pbyZero = memchr(pbyName, 0, cbName);
   size_t cbANSI, iEncoded, cEncoded, cUnits = 0, cRun, iIndex, cbUTF8 = 0;
   unsigned usFlags = 0, cFlagBits = 0, usHigh, usCode, usCorrection;
   uint16_t *pusUnits;
   char *pszName;
   uint32_t ulCode;

   if (!cbName || (!bUnicode && pbyZero))
      goto invalid;
   if (!pbyZero) {
      pszName = malloc(cbName + 1);
      if (!pszName)
         goto memory;
      memcpy(pszName, pbyName, cbName);
      pszName[cbName] = 0;
      return (pszName);
   }
   cbANSI = (size_t)(pbyZero - pbyName);
   iEncoded = cbANSI + 1;
   cEncoded = cbName;
   if (iEncoded == cEncoded)
      goto invalid;
   usHigh = pbyName[iEncoded++];
   pusUnits = malloc(65535 * sizeof(uint16_t));
   if (!pusUnits)
      goto memory;
   while (iEncoded < cEncoded) {
      if (!cFlagBits) {
         usFlags = pbyName[iEncoded++];
         cFlagBits = 8;
      }
      usCode = (usFlags >> 6) & 3;
      usFlags <<= 2;
      cFlagBits -= 2;
      if (iEncoded >= cEncoded || cUnits == 65535)
         goto invalid_units;
      if (usCode == 3) {
         cRun = pbyName[iEncoded++];
         usCorrection = 0;
         if (cRun & 128) {
            if (iEncoded >= cEncoded)
               goto invalid_units;
            usCorrection = pbyName[iEncoded++];
         }
         if (cUnits > cbANSI || (cRun & 127) + 2 > cbANSI - cUnits)
            goto invalid_units;
         for (iIndex = 0; iIndex < (cRun & 127) + 2; iIndex++) {
            pusUnits[cUnits] = (uint16_t)((pbyName[cUnits] + usCorrection) & 255);
            if (cRun & 128)
               pusUnits[cUnits] |= (uint16_t)(usHigh << 8);
            cUnits++;
         }
      } else {
         if (usCode == 2) {
            if (cEncoded - iEncoded < 2)
               goto invalid_units;
            pusUnits[cUnits++] = ReadWord(pbyName + iEncoded);
            iEncoded += 2;
         } else {
            pusUnits[cUnits++] = (uint16_t)(pbyName[iEncoded++] |
                                            (usCode == 1 ? usHigh << 8 : 0));
         }
      }
   }
   if (!cUnits)
      goto invalid_units;
   pszName = malloc(cUnits * 4 + 1);
   if (!pszName) {
      free(pusUnits);
      goto memory;
   }
   for (iIndex = 0; iIndex < cUnits; iIndex++) {
      ulCode = pusUnits[iIndex];
      if (ulCode >= 0xd800 && ulCode <= 0xdbff) {
         if (iIndex + 1 >= cUnits || pusUnits[iIndex + 1] < 0xdc00 ||
             pusUnits[iIndex + 1] > 0xdfff)
            goto invalid_utf8;
         ulCode = 0x10000 + ((ulCode - 0xd800) << 10) + (pusUnits[++iIndex] - 0xdc00U);
      } else if (!ulCode || (ulCode >= 0xdc00 && ulCode <= 0xdfff))
         goto invalid_utf8;
      if (ulCode >= 0x10000) {
         pszName[cbUTF8++] = (char)(0xf0 | (ulCode >> 18));
         pszName[cbUTF8++] = (char)(0x80 | ((ulCode >> 12) & 63));
         pszName[cbUTF8++] = (char)(0x80 | ((ulCode >> 6) & 63));
         pszName[cbUTF8++] = (char)(0x80 | (ulCode & 63));
      } else if (ulCode >= 0x800) {
         pszName[cbUTF8++] = (char)(0xe0 | (ulCode >> 12));
         pszName[cbUTF8++] = (char)(0x80 | ((ulCode >> 6) & 63));
         pszName[cbUTF8++] = (char)(0x80 | (ulCode & 63));
      } else if (ulCode >= 0x80) {
         pszName[cbUTF8++] = (char)(0xc0 | (ulCode >> 6));
         pszName[cbUTF8++] = (char)(0x80 | (ulCode & 63));
      } else
         pszName[cbUTF8++] = (char)ulCode;
   }
   pszName[cbUTF8] = 0;
   free(pusUnits);
   return (pszName);

invalid_utf8:
   free(pszName);
invalid_units:
   free(pusUnits);
invalid:
   ArchiveError(par, "archive corrupted: invalid RAR filename");
   return (NULL);
memory:
   ArchiveError(par, "out of memory");
   return (NULL);
}

void SearchRAR(PARCHIVEREADER par)
{
   unsigned char bySignature[8], byPrefix[7], *pbyHeader;
   uint64_t ullOffset = 7, ullPacked, ullOriginal, ullData;
   size_t cbHeader, cbName, iName, cbCRC;
   unsigned usFlags, byType;
   BOOL bMain = TRUE;
   char *pszName;

   if (!ReadAt(par, 0, bySignature, 7))
      return;
   if (!memcmp(bySignature, "Rar!\x1a\x07\x01", 7)) {
      ArchiveError(par, "RAR5 archives are not supported; only RAR4 headers are supported");
      return;
   }
   if (memcmp(bySignature, "Rar!\x1a\x07\x00", 7)) {
      ArchiveError(par, "archive corrupted: invalid RAR4 signature");
      return;
   }
   while (!bInterrupted && ullOffset < par->ullLength) {
      if (!ReadAt(par, ullOffset, byPrefix, sizeof(byPrefix)))
         return;
      usFlags = ReadWord(byPrefix + 3);
      cbHeader = ReadWord(byPrefix + 5);
      byType = byPrefix[2];
      if (cbHeader < ((usFlags & 0x8000) ? 11U : 7U) ||
          (bMain && (byType != 0x73 || cbHeader < 13)) ||
          (byType == 0x73 && !bMain)) {
         ArchiveError(par, "archive corrupted: invalid RAR4 block");
         return;
      }
      pbyHeader = malloc(cbHeader);
      if (!pbyHeader) {
         ArchiveError(par, "out of memory");
         return;
      }
      if (!ReadAt(par, ullOffset, pbyHeader, cbHeader)) {
         free(pbyHeader);
         return;
      }
      cbCRC = cbHeader;
      if (byType == 0x73 && (usFlags & 2))
         cbCRC = 13;             // Old archive comments have a separate checksum.
      if (byType == 0x74 && (usFlags & 8)) {
         iName = (usFlags & 0x100) ? 40 : 32;
         if (cbHeader < iName || ReadWord(pbyHeader + 26) > cbHeader - iName) {
            ArchiveError(par, "archive corrupted: invalid commented RAR4 header");
            free(pbyHeader);
            return;
         }
         cbCRC = iName + ReadWord(pbyHeader + 26);
      }
      if ((HeaderCRC(pbyHeader + 2, cbCRC - 2) & 65535) != ReadWord(pbyHeader)) {
         ArchiveError(par, "archive corrupted: invalid RAR4 header checksum");
         free(pbyHeader);
         return;
      }
      ullPacked = (usFlags & 0x8000) ? ReadDWord(pbyHeader + 7) : 0;
      ullData = ullOffset + cbHeader;
      if (bMain) {
         bMain = FALSE;
         if (usFlags & (1 | 0x80)) {
            ArchiveError(par, "split RAR archives and encrypted RAR headers are not supported");
            free(pbyHeader);
            return;
         }
      } else if (byType == 0x74) {
         iName = (usFlags & 0x100) ? 40 : 32;
         if (cbHeader < iName || !(usFlags & 0x8000)) {
            ArchiveError(par, "archive corrupted: short RAR4 file header");
            free(pbyHeader);
            return;
         }
         ullOriginal = ReadDWord(pbyHeader + 11);
         if (usFlags & 0x100) {
            ullPacked |= (uint64_t)ReadDWord(pbyHeader + 32) << 32;
            ullOriginal |= (uint64_t)ReadDWord(pbyHeader + 36) << 32;
         }
         cbName = ReadWord(pbyHeader + 26);
         if (usFlags & 3) {
            ArchiveError(par, "split RAR entries are not supported");
            free(pbyHeader);
            return;
         }
         if (cbName > cbHeader - iName || !CheckRange(par, ullData, ullPacked)) {
            ArchiveError(par, "archive corrupted: invalid RAR4 filename or data length");
            free(pbyHeader);
            return;
         }
         pszName = RARName(par, pbyHeader + iName, cbName, (usFlags & 0x200) != 0);
         if (!pszName) {
            free(pbyHeader);
            return;
         }
         if ((usFlags & 0xe0) != 0xe0)
            ShowEntry(par, pszName, ullOriginal,
                       DOSDateTime(ReadWord(pbyHeader + 22), ReadWord(pbyHeader + 20)));
         free(pszName);
      } else if (byType == 0x7b) {
         if (usFlags & 1)
            ArchiveError(par, "split RAR archives are not supported");
         free(pbyHeader);
         return;
      } else if (byType < 0x75 || byType > 0x7a) {
         ArchiveError(par, "unsupported RAR4 block type");
         free(pbyHeader);
         return;
      }
      free(pbyHeader);
      if (!CheckRange(par, ullData, ullPacked))
         return;
      ullOffset = ullData + ullPacked;
   }
   if (bMain)
      ArchiveError(par, "archive corrupted: missing RAR4 main header");
}
