// SPDX-License-Identifier: MIT
// Licensed under the MIT License. See LICENSE for the full text.

//*****************************************************************************
//
// QFUTIL.C
//
// Linux filename matching, output, totals, and console input.
// Copyright 1989-95 Ronald C. Bieber.
//
//*****************************************************************************
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <locale.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <wchar.h>

#include "QFLINUX.H"

static FILE *pfTerminal = NULL;
static unsigned usLineCount = 0;
static FILESTUFF *pfsRows = NULL;
static size_t cRows = 0, cAllocatedRows = 0;
static BOOL bListingFailed = FALSE;
static locale_t locDisplay = (locale_t)0;
static BOOL bDisplayLocaleReady = FALSE;
static size_t WriteText(FILE *, const char *);

void ReportError(const char *pszPath, const char *pszMessage)
{
   fputs("qf: ", stderr);
   WriteText(stderr, pszPath);
   fprintf(stderr, ": %s\n", pszMessage);
   bError = TRUE;
}

PSTR JoinPath(const char *pszDirectory, const char *pszName)
{
   size_t cbDirectory = strlen(pszDirectory), cbName = strlen(pszName);
   PSTR pszPath;

   if (cbDirectory > SIZE_MAX - cbName - 2) {
      ReportError(pszDirectory, "path too long");
      return (NULL);
   }

   pszPath = malloc(cbDirectory + cbName + 2);
   if (!pszPath) {
      ReportError(pszDirectory, "out of memory");
      return (NULL);
   }

   memcpy(pszPath, pszDirectory, cbDirectory);
   if (cbDirectory && pszDirectory[cbDirectory - 1] != '/')
      pszPath[cbDirectory++] = '/';
   memcpy(pszPath + cbDirectory, pszName, cbName + 1);
   return (pszPath);
}

static unsigned char FoldCharacter(unsigned char byChar, BOOL bFold)
{
   // Match bytes, without changing the spelling of a Linux filename.
   return (bFold ? (unsigned char)toupper(byChar) : byChar);
}

static BOOL MatchPart(const char *pszPattern, size_t cbPattern,
                      const char *pszName, size_t cbName, BOOL bFold,
                      BOOL bDOS)
{
   size_t iPattern = 0, iName = 0, iStar = SIZE_MAX, iRetry = 0;

   while (iName < cbName) {
      if (iPattern < cbPattern && pszPattern[iPattern] == '*') {
         if (bDOS)
            return (TRUE);       // Original DOS: '*' accepts the remainder.
         iStar = iPattern++;
         iRetry = iName;
      } else if (iPattern < cbPattern &&
                 (pszPattern[iPattern] == '?' ||
                  FoldCharacter((unsigned char)pszPattern[iPattern], bFold) ==
                  FoldCharacter((unsigned char)pszName[iName], bFold))) {
         iPattern++;
         iName++;
      } else if (iStar != SIZE_MAX) {
         iPattern = iStar + 1;
         iName = ++iRetry;
      } else
         return (FALSE);
   }

   while (iPattern < cbPattern && pszPattern[iPattern] == '*')
      if (bDOS)
         return (TRUE);
      else
         iPattern++;
   return (iPattern == cbPattern);
}

BOOL Match(const char *pszPattern, const char *pszName)
{
   const char *pszPatternExt, *pszNameExt;
   size_t cbPatternName;

   if (!opt.bDOSMatch)
      return (MatchPart(pszPattern, strlen(pszPattern), pszName,
                        strlen(pszName), opt.bIgnoreCase, FALSE));

   pszPatternExt = strrchr(pszPattern, '.');
   pszNameExt = strrchr(pszName, '.');
   cbPatternName = pszPatternExt ? (size_t)(pszPatternExt - pszPattern) :
                                  strlen(pszPattern);
   return (MatchPart(cbPatternName ? pszPattern : "*", cbPatternName ? cbPatternName : 1,
                     pszName, pszNameExt ? (size_t)(pszNameExt - pszName) :
                                          strlen(pszName), TRUE, TRUE) &&
           MatchPart(pszPatternExt ? pszPatternExt + 1 : "*",
                     pszPatternExt ? strlen(pszPatternExt + 1) : 1,
                     pszNameExt ? pszNameExt + 1 : "",
                     pszNameExt ? strlen(pszNameExt + 1) : 0, TRUE, TRUE));
}

static void CloseDisplayLocale(void)
{
   if (locDisplay)
      freelocale(locDisplay);
   locDisplay = (locale_t)0;
}

static size_t WriteText(FILE *pfOutput, const char *pszText)
{
   const unsigned char *pbyText = (const unsigned char *)pszText;
   size_t cbRemaining = strlen(pszText), cbCharacter, cWidth = 0;
   int cCharacterWidth;
   wchar_t wcCharacter;
   mbstate_t mbs;
   locale_t locPrevious = (locale_t)0;

   // Measure terminal cells in a private locale, without changing matching.
   if (!bDisplayLocaleReady) {
      locDisplay = newlocale(LC_CTYPE_MASK, "C.UTF-8", (locale_t)0);
      if (!locDisplay)
         locDisplay = newlocale(LC_CTYPE_MASK, "", (locale_t)0);
      if (locDisplay && atexit(CloseDisplayLocale))
         CloseDisplayLocale();
      bDisplayLocaleReady = TRUE;
   }
   if (locDisplay)
      locPrevious = uselocale(locDisplay);
   memset(&mbs, 0, sizeof(mbs));
   while (cbRemaining) {
      cbCharacter = 1;
      cCharacterWidth = 1;
      if (*pbyText >= 128) {
         cbCharacter = mbrtowc(&wcCharacter, (const char *)pbyText, cbRemaining, &mbs);
         if (cbCharacter == (size_t)-1 || cbCharacter == (size_t)-2 || !cbCharacter)
            cCharacterWidth = -1;
         else
            cCharacterWidth = wcwidth(wcCharacter);
      }
      if (*pbyText == '\\') {
         if (pfOutput)
            fputs("\\\\", pfOutput);
         cWidth += 2;
      } else if (*pbyText < 32 || *pbyText == 127 || cCharacterWidth < 0) {
         if (pfOutput)
            fprintf(pfOutput, "\\x%02x", *pbyText);
         cWidth += 4;
         cbCharacter = 1;
         memset(&mbs, 0, sizeof(mbs));
      } else {
         if (pfOutput)
            fwrite(pbyText, 1, cbCharacter, pfOutput);
         cWidth += (size_t)cCharacterWidth;
      }
      pbyText += cbCharacter;
      cbRemaining -= cbCharacter;
   }
   if (locPrevious)
      uselocale(locPrevious);
   return (cWidth);
}

void PrintText(const char *pszText)
{
   WriteText(stdout, pszText);
}

static void LongToString(uint64_t ullValue, char *pszDest)
{
   char szDigits[21];
   size_t cbDigits, iIndex;

   snprintf(szDigits, sizeof(szDigits), "%" PRIu64, ullValue);
   cbDigits = strlen(szDigits);
   for (iIndex = 0; iIndex < cbDigits; iIndex++) {
      if (iIndex && !((cbDigits - iIndex) % 3))
         *pszDest++ = ',';
      *pszDest++ = szDigits[iIndex];
   }
   *pszDest = 0;
}

BOOL AddTotal(uint64_t *pullTotal, uint64_t ullValue)
{
   if (*pullTotal > UINT64_MAX - ullValue) {
      ReportError("totals", "64-bit total overflow");
      return (FALSE);
   }
   *pullTotal += ullValue;
   return (TRUE);
}

static FILE *OpenTerminal(void)
{
   if (!pfTerminal) {
      pfTerminal = fopen("/dev/tty", "r+");
      if (pfTerminal &&
          fcntl(fileno(pfTerminal), F_SETFL, O_NONBLOCK) == -1) {
         fclose(pfTerminal);
         pfTerminal = NULL;
      }
   }
   return (pfTerminal);
}

static int ReadTerminalCharacter(void)
{
   struct pollfd pfd;
   unsigned char byChar;
   ssize_t cbRead;
   int iReady;

   pfd.fd = fileno(pfTerminal);
   pfd.events = POLLIN;
   while (!bInterrupted) {
      // Bound the wait in case a signal arrives just before poll(). The read
      // itself is nonblocking, so the same race cannot leave it waiting.
      iReady = poll(&pfd, 1, 100);
      if (iReady < 0) {
         if (errno == EINTR)
            continue;
         ReportError("terminal", strerror(errno));
         break;
      }
      if (!iReady || bInterrupted)
         continue;
      cbRead = read(pfd.fd, &byChar, 1);
      if (cbRead == 1)
         return (byChar);
      if (!cbRead)
         break;
      if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
         ReportError("terminal", strerror(errno));
         break;
      }
   }
   return (EOF);
}

void CloseTerminal(void)
{
   if (pfTerminal)
      fclose(pfTerminal);
   pfTerminal = NULL;
}

void PauseOutput(void)
{
   struct termios termOld, termNew;
   int iChar;

   if (!(opt.usSwitches & PAUSE) || !isatty(STDOUT_FILENO) ||
       ++usLineCount < 24 || bInterrupted)
      return;
   usLineCount = 0;
   if (!OpenTerminal() || tcgetattr(fileno(pfTerminal), &termOld))
      return;
   termNew = termOld;
   termNew.c_lflag &= (tcflag_t)~(ICANON | ECHO);
   termNew.c_cc[VMIN] = 1;
   termNew.c_cc[VTIME] = 0;
   if (tcsetattr(fileno(pfTerminal), TCSANOW, &termNew))
      return;
   fflush(stdout);
   fputs("Program paused; press a key (ESC to quit).", pfTerminal);
   fflush(pfTerminal);
   iChar = ReadTerminalCharacter();
   while (tcsetattr(fileno(pfTerminal), TCSANOW, &termOld) && errno == EINTR)
      ;
   fputs("\r                                         \r", pfTerminal);
   fflush(pfTerminal);
   if (iChar == 27)
      bInterrupted = SIGINT;
}

static void FreeFileListing(void)
{
   size_t iIndex;

   for (iIndex = 0; iIndex < cRows; iIndex++)
      free((void *)pfsRows[iIndex].pszName);
   free(pfsRows);
   pfsRows = NULL;
   cRows = cAllocatedRows = 0;
}

void BeginFileListing(void)
{
   FreeFileListing();
   bListingFailed = FALSE;
}

void ShowFileInfo(const FILESTUFF *pfs)
{
   FILESTUFF *pfsNew;
   char *pszName;
   size_t cNew;

   if (bListingFailed)
      return;
   // A matching disk archive becomes a heading when its first member matches.
   if ((pfs->byType & FS_MAINARCHIVE) && cRows &&
       !(pfsRows[cRows - 1].byType & (FS_ARCHIVE | FS_DIRECTORY)) &&
       !strcmp(pfsRows[cRows - 1].pszName, pfs->pszName)) {
      pfsRows[cRows - 1].byType = FS_MAINARCHIVE;
      return;
   }
   if (cRows == cAllocatedRows) {
      if (cAllocatedRows > SIZE_MAX / sizeof(FILESTUFF) / 2)
         goto memory;
      cNew = cAllocatedRows ? cAllocatedRows * 2 : 32;
      pfsNew = realloc(pfsRows, cNew * sizeof(FILESTUFF));
      if (!pfsNew)
         goto memory;
      pfsRows = pfsNew;
      cAllocatedRows = cNew;
   }
   pszName = strdup(pfs->pszName);
   if (!pszName)
      goto memory;
   pfsRows[cRows] = *pfs;
   pfsRows[cRows].pszName = pszName;
   pfsRows[cRows].pszArchiveName = NULL;
   cRows++;
   return;

memory:
   ReportError("listing", "out of memory");
   bListingFailed = TRUE;
}

static void MeasureFileInfo(const FILESTUFF *pfs, size_t *pcName, size_t *pcSize)
{
   char szSize[27];
   size_t cName = WriteText(NULL, pfs->pszName), cSize;

   if (pfs->byType & FS_DIRECTORY)
      strcpy(szSize, "<DIR>");
   else
      LongToString(pfs->ullSize, szSize);
   cSize = strlen(szSize);
   if (cName > *pcName)
      *pcName = cName;
   if (cSize > *pcSize)
      *pcSize = cSize;
}

static void PrintFileInfo(const FILESTUFF *pfs, size_t cNameWidth, size_t cSizeWidth)
{
   char szSize[27], szDate[64];
   struct tm tmFile;
   size_t cName, cPadding;

   fputs((pfs->byType & FS_ARCHIVE) ? "      *" : "   ", stdout);
   cName = WriteText(stdout, pfs->pszName);
   if (pfs->byType & FS_MAINARCHIVE)
      putchar('/');
   else if (!(opt.usSwitches & NOFILEINFO)) {
      cPadding = cNameWidth - cName + 2;
      while (cPadding--)
         putchar(' ');
      if (pfs->byType & FS_DIRECTORY)
         strcpy(szSize, "<DIR>");
      else
         LongToString(pfs->ullSize, szSize);
      strcpy(szDate, "-");
      if (pfs->tModified != (time_t)-1 && localtime_r(&pfs->tModified, &tmFile))
         if (!strftime(szDate, sizeof(szDate), "%I:%M %p  %b %e %Y", &tmFile))
            strcpy(szDate, "-");
      printf("%*s %-5s  %s", (int)cSizeWidth, szSize,
             (pfs->byType & FS_DIRECTORY) ? "" : "bytes", szDate);
   }
   putchar('\n');
   PauseOutput();
}

void EndFileListing(void)
{
   size_t iIndex, iMember, cNameWidth = 0, cSizeWidth = 0;
   size_t cDiskNameWidth = 0, cDiskSizeWidth = 0;

   // Buffer one directory so its columns fit the actual matching names/sizes.
   if (!(opt.usSwitches & NOFILEINFO))
      for (iIndex = 0; iIndex < cRows && !bInterrupted; iIndex++)
         if (!(pfsRows[iIndex].byType & (FS_ARCHIVE | FS_MAINARCHIVE)))
            MeasureFileInfo(&pfsRows[iIndex], &cDiskNameWidth, &cDiskSizeWidth);
   for (iIndex = 0; iIndex < cRows && !bInterrupted; iIndex++) {
      if (pfsRows[iIndex].byType & FS_MAINARCHIVE) {
         cNameWidth = cSizeWidth = 0;
         if (!(opt.usSwitches & NOFILEINFO))
            for (iMember = iIndex + 1; iMember < cRows && !bInterrupted &&
                 (pfsRows[iMember].byType & FS_ARCHIVE); iMember++)
               MeasureFileInfo(&pfsRows[iMember], &cNameWidth, &cSizeWidth);
      } else if (!(pfsRows[iIndex].byType & FS_ARCHIVE)) {
         cNameWidth = cDiskNameWidth;
         cSizeWidth = cDiskSizeWidth;
      }
      PrintFileInfo(&pfsRows[iIndex], cNameWidth, cSizeWidth);
   }
   FreeFileListing();
}

void ShowTotals(const TOTALS *ptotals, const char *pszDescription)
{
   char szFiles[27], szDirectories[27], szBytes[27];

   LongToString(ptotals->ullFiles, szFiles);
   LongToString(ptotals->ullDirectories, szDirectories);
   LongToString(ptotals->ullBytes, szBytes);
   printf("   %s file(s), %s directory(s), %s bytes %s\n",
          szFiles, szDirectories, szBytes, pszDescription);
   PauseOutput();
}
