// SPDX-License-Identifier: MIT
// Licensed under the MIT License. See LICENSE for the full text.

//*****************************************************************************
//
// QFLINUX.C
//
// Quick Find -- native Linux command line and directory searching.
// Copyright 1989-95 Ronald C. Bieber.
//
//*****************************************************************************
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "QFLINUX.H"

OPTIONS opt = { 0, FALSE, FALSE, FALSE, NULL, NULL };
TOTALS totals = { 0, 0, 0 };
volatile sig_atomic_t bInterrupted = 0;
BOOL bError = FALSE;

typedef struct tagWALKFRAME {
   DIR *pdir;
   int iDirectory;
   dev_t devDirectory;
   ino_t inoDirectory;
   PSTR pszPath;
   struct tagWALKFRAME *pframeParent;
} WALKFRAME;

static void usage(void)
{
   printf("Quick File Finder for Linux version %s\n", VERSION);
   puts("usage: qf [options] pattern [directory]\n"
        "  -a, --all-drives        Start at / (instead of current directory)\n"
        "  -b, --bare              Omit sizes, dates, banner, and totals\n"
        "  -c, --current           Start in the current directory\n"
        "  -cf, --archives         Search files and archive members\n"
        "  -co, --archives-only    Search only archive members\n"
        "  -e, --formats           List supported archive formats and limits\n"
        "  -p, --pause             Pause terminal output every 24 lines\n"
        "  -t, --totals            Show totals for each directory\n"
        "  -i, --ignore-case       Match without regard to ASCII letter case\n"
        "      --dos-match         Use DOS name/extension and '*' rules\n"
        "      --cross-filesystems Traverse mounted filesystems too\n"
        "  -h, --help              Display this help\n"
        "      --                 End option processing\n\n"
        "Quote patterns such as '*.c' so the shell does not expand them.\n"
        "An explicit directory overrides -a/-c. Symlinks are not traversed.\n"
        "Files and archive members are listed without modification.\n"
        "QF_OPT supplies whitespace-separated default options; no quoting.");
   putchar('\n');
   ShowArchiveFormats();
}

static int ParseOption(const char *pszOption, BOOL bEnvironment)
{
   // Keep the original short switches, including -CF and -CO.
   if (!strcmp(pszOption, "--help") || !strcmp(pszOption, "-h") ||
       !strcmp(pszOption, "-?")) {
      if (bEnvironment) {
         ReportError("QF_OPT", "help is only accepted on the command line");
         return (-1);
      }
      usage();
      return (1);
   }
   if (!strcmp(pszOption, "--formats") || !strcasecmp(pszOption, "-e")) {
      if (bEnvironment) {
         ReportError("QF_OPT", "formats are only displayed on the command line");
         return (-1);
      }
      ShowArchiveFormats();
      return (1);
   }
   if (!strcmp(pszOption, "--all-drives") || !strcasecmp(pszOption, "-a")) {
      opt.usSwitches |= ALLDRIVES;
      opt.usSwitches &= ~FROM_CURDIR;
   } else if (!strcmp(pszOption, "--current") || !strcasecmp(pszOption, "-c")) {
      opt.usSwitches |= FROM_CURDIR;
      opt.usSwitches &= ~ALLDRIVES;
   } else if (!strcmp(pszOption, "--bare") || !strcasecmp(pszOption, "-b"))
      opt.usSwitches |= NOFILEINFO;
   else if (!strcmp(pszOption, "--archives") || !strcasecmp(pszOption, "-cf")) {
      opt.usSwitches |= COMPRESSED;
      opt.usSwitches &= ~COMPRESSED_ONLY;
   } else if (!strcmp(pszOption, "--archives-only") || !strcasecmp(pszOption, "-co")) {
      opt.usSwitches |= COMPRESSED_ONLY;
      opt.usSwitches &= ~COMPRESSED;
   } else if (!strcmp(pszOption, "--pause") || !strcasecmp(pszOption, "-p"))
      opt.usSwitches |= PAUSE;
   else if (!strcmp(pszOption, "--totals") || !strcasecmp(pszOption, "-t"))
      opt.usSwitches |= SUBDIRTOT;
   else if (!strcmp(pszOption, "--ignore-case") || !strcasecmp(pszOption, "-i"))
      opt.bIgnoreCase = TRUE;
   else if (!strcmp(pszOption, "--dos-match"))
      opt.bDOSMatch = TRUE;
   else if (!strcmp(pszOption, "--cross-filesystems"))
      opt.bCrossFilesystems = TRUE;
   else {
      ReportError(pszOption, "unknown option");
      return (-1);
   }
   return (0);
}

static BOOL ParseEnvironmentOptions(void)
{
   const char *pszEnvironment = getenv("QF_OPT");
   PSTR pszCopy, pszToken, pszNext;
   BOOL bValid = TRUE;

   if (!pszEnvironment)
      return (TRUE);
   pszCopy = strdup(pszEnvironment);
   if (!pszCopy) {
      ReportError("QF_OPT", "out of memory");
      return (FALSE);
   }
   for (pszToken = strtok_r(pszCopy, " \t\r\n", &pszNext); pszToken;
        pszToken = strtok_r(NULL, " \t\r\n", &pszNext)) {
      if (*pszToken != '-' || !strcmp(pszToken, "--") ||
          ParseOption(pszToken, TRUE) != 0) {
         if (*pszToken != '-' || !strcmp(pszToken, "--"))
            ReportError("QF_OPT", "only options are accepted");
         bValid = FALSE;
         break;
      }
   }
   free(pszCopy);
   return (bValid);
}

static void c_break(int iSignal)
{
   bInterrupted = iSignal;       // Cleanup and output happen outside the handler.
}

static void ShowDirectory(const char *pszDirectory, BOOL *pbPrinted)
{
   if (!*pbPrinted) {
      putchar('\n');
      PrintText(pszDirectory);
      putchar('\n');
      PauseOutput();
      *pbPrinted = TRUE;
   }
}

void FoundArchiveEntry(const char *pszDirectory, const char *pszArchive,
                       const struct stat *pstArchive, const char *pszEntry,
                       uint64_t ullSize, time_t tModified,
                       TOTALS *ptotalsDirectory, BOOL *pbPrinted, BOOL bFirst)
{
   FILESTUFF fs;

   ShowDirectory(pszDirectory, pbPrinted);
   fs.pszName = pszArchive;
   fs.pszArchiveName = pszArchive;
   fs.ullSize = (uint64_t)pstArchive->st_size;
   fs.tModified = pstArchive->st_mtime;
   fs.byType = FS_MAINARCHIVE;
   if (bFirst)
      ShowFileInfo(&fs);
   fs.pszName = pszEntry;
   fs.ullSize = ullSize;
   fs.tModified = tModified;
   fs.byType = FS_ARCHIVE;
   ShowFileInfo(&fs);
   AddTotal(&ptotalsDirectory->ullFiles, 1);
   AddTotal(&ptotalsDirectory->ullBytes, ullSize);
}

static void filefind(WALKFRAME *pframe)
{
   struct dirent *pde;
   struct stat stFile;
   TOTALS totalsDirectory = { 0, 0, 0 };
   BOOL bPrinted = FALSE;
   int iDirectory = pframe->iDirectory, iArchive;
   PSTR pszPath;
   FILESTUFF fs;

   BeginFileListing();
   while (!bInterrupted) {
      errno = 0;
      pde = readdir(pframe->pdir);
      if (!pde) {
         if (errno)
            ReportError(pframe->pszPath, strerror(errno));
         break;
      }
      if (!strcmp(pde->d_name, ".") || !strcmp(pde->d_name, ".."))
         continue;
      pszPath = JoinPath(pframe->pszPath, pde->d_name);
      if (!pszPath)
         break;
      if (fstatat(iDirectory, pde->d_name, &stFile, AT_SYMLINK_NOFOLLOW)) {
         ReportError(pszPath, strerror(errno));
         free(pszPath);
         continue;
      }
      iArchive = ((opt.usSwitches & (COMPRESSED | COMPRESSED_ONLY)) &&
                  S_ISREG(stFile.st_mode)) ? GetArchiveSearchFunction(pde->d_name) : -1;

      if (!(opt.usSwitches & COMPRESSED_ONLY) &&
          Match(opt.pszPattern, pde->d_name)) {
         ShowDirectory(pframe->pszPath, &bPrinted);
         fs.pszName = pde->d_name;
         fs.pszArchiveName = NULL;
         fs.ullSize = stFile.st_size < 0 ? 0 : (uint64_t)stFile.st_size;
         fs.tModified = stFile.st_mtime;
         fs.byType = S_ISDIR(stFile.st_mode) ? FS_DIRECTORY :
                      (iArchive >= 0 ? FS_MAINARCHIVE : 0);
         ShowFileInfo(&fs);
         if (S_ISDIR(stFile.st_mode))
            AddTotal(&totalsDirectory.ullDirectories, 1);
         else {
            AddTotal(&totalsDirectory.ullFiles, 1);
            AddTotal(&totalsDirectory.ullBytes, fs.ullSize);
         }
      }

      if (iArchive >= 0 && !bInterrupted) {
         int iFile = openat(iDirectory, pde->d_name,
                            O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
         struct stat stOpened;

         if (iFile < 0) {
            ReportError(pszPath, strerror(errno));
         } else {
            if (fstat(iFile, &stOpened) || !S_ISREG(stOpened.st_mode) ||
                stOpened.st_dev != stFile.st_dev || stOpened.st_ino != stFile.st_ino)
               ReportError(pszPath, "archive changed since search");
            else {
               FILE *pfArchive = fdopen(iFile, "rb");
               if (!pfArchive)
                  ReportError(pszPath, strerror(errno));
               else {
                  // ReadArchive borrows the stream while it is called.
                  ReadArchive(iArchive, pfArchive, pframe->pszPath, pde->d_name,
                              &stOpened, &totalsDirectory, &bPrinted);
                  fclose(pfArchive);
                  iFile = -1;
               }
            }
            if (iFile >= 0)
               close(iFile);
         }
      }
      free(pszPath);
   }

   EndFileListing();
   if ((opt.usSwitches & SUBDIRTOT) && !(opt.usSwitches & NOFILEINFO) && bPrinted)
      ShowTotals(&totalsDirectory, "in this directory");
   AddTotal(&totals.ullFiles, totalsDirectory.ullFiles);
   AddTotal(&totals.ullDirectories, totalsDirectory.ullDirectories);
   AddTotal(&totals.ullBytes, totalsDirectory.ullBytes);
   rewinddir(pframe->pdir);
}

static WALKFRAME *OpenDirectory(int iParent, const char *pszName,
                                PSTR pszPath, WALKFRAME *pframeParent,
                                const struct stat *pstExpected)
{
   int iDirectory = openat(iParent, pszName,
                           O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
   struct stat stDirectory;
   WALKFRAME *pframe;
   DIR *pdir;

   if (iDirectory < 0) {
      ReportError(pszPath, strerror(errno));
      free(pszPath);
      return (NULL);
   }
   if (fstat(iDirectory, &stDirectory) ||
       (pstExpected && (stDirectory.st_dev != pstExpected->st_dev ||
                        stDirectory.st_ino != pstExpected->st_ino))) {
      ReportError(pszPath, "directory changed since search");
      close(iDirectory);
      free(pszPath);
      return (NULL);
   }
   pdir = fdopendir(iDirectory);
   pframe = pdir ? malloc(sizeof(WALKFRAME)) : NULL;
   if (!pframe) {
      ReportError(pszPath, pdir ? "out of memory" : strerror(errno));
      if (pdir)
         closedir(pdir);
      else
         close(iDirectory);
      free(pszPath);
      return (NULL);
   }
   pframe->pdir = pdir;
   pframe->iDirectory = iDirectory;
   pframe->devDirectory = stDirectory.st_dev;
   pframe->inoDirectory = stDirectory.st_ino;
   pframe->pszPath = pszPath;
   pframe->pframeParent = pframeParent;
   return (pframe);
}

static void walkdirs(const char *pszRoot)
{
   WALKFRAME *pframe, *pframeChild, *pframeParent;
   struct dirent *pde;
   struct stat stRoot, stDirectory;
   PSTR pszPath = strdup(pszRoot);

   if (!pszPath) {
      ReportError(pszRoot, "out of memory");
      return;
   }
   pframe = OpenDirectory(AT_FDCWD, pszRoot, pszPath, NULL, NULL);
   if (!pframe)
      return;
   if (fstat(pframe->iDirectory, &stRoot)) {
      ReportError(pszRoot, strerror(errno));
      closedir(pframe->pdir);
      free(pframe->pszPath);
      free(pframe);
      return;
   }
   filefind(pframe);

   // An explicit stack avoids exhausting the C stack on a deep tree.
   while (pframe) {
      errno = 0;
      pde = bInterrupted ? NULL : readdir(pframe->pdir);
      if (!pde) {
         if (errno)
            ReportError(pframe->pszPath, strerror(errno));
         pframeParent = pframe->pframeParent;
         closedir(pframe->pdir);
         free(pframe->pszPath);
         free(pframe);
         pframe = pframeParent;
         continue;
      }
      if (!strcmp(pde->d_name, ".") || !strcmp(pde->d_name, ".."))
         continue;
      if (fstatat(pframe->iDirectory, pde->d_name, &stDirectory,
                  AT_SYMLINK_NOFOLLOW)) {
         ReportError(pframe->pszPath, strerror(errno));
         continue;
      }
      if (!S_ISDIR(stDirectory.st_mode) ||
          (!opt.bCrossFilesystems && stDirectory.st_dev != stRoot.st_dev))
         continue;
      // Bind mounts can form cycles even when no symlinks are followed.
      for (pframeParent = pframe; pframeParent;
           pframeParent = pframeParent->pframeParent)
         if (pframeParent->devDirectory == stDirectory.st_dev &&
             pframeParent->inoDirectory == stDirectory.st_ino)
            break;
      if (pframeParent)
         continue;
      pszPath = JoinPath(pframe->pszPath, pde->d_name);
      if (!pszPath)
         continue;
      pframeChild = OpenDirectory(pframe->iDirectory, pde->d_name,
                                  pszPath, pframe, &stDirectory);
      if (pframeChild) {
         pframe = pframeChild;
         filefind(pframe);
      }
   }
}

int main(int argc, char **argv)
{
   int iIndex, iResult;
   BOOL bOptions = TRUE;
   PSTR pszRoot;
   struct sigaction saBreak;

   memset(&saBreak, 0, sizeof(saBreak));
   saBreak.sa_handler = c_break;
   sigemptyset(&saBreak.sa_mask);
   sigaction(SIGINT, &saBreak, NULL);
   sigaction(SIGTERM, &saBreak, NULL);
   if (!ParseEnvironmentOptions())
      return (2);
   for (iIndex = 1; iIndex < argc; iIndex++) {
      if (bOptions && !strcmp(argv[iIndex], "--")) {
         bOptions = FALSE;
         continue;
      }
      if (bOptions && argv[iIndex][0] == '-' && argv[iIndex][1]) {
         iResult = ParseOption(argv[iIndex], FALSE);
         if (iResult)
            return (iResult > 0 ? 0 : 2);
      } else if (!opt.pszPattern)
         opt.pszPattern = argv[iIndex];
      else if (!opt.pszRoot)
         opt.pszRoot = argv[iIndex];
      else {
         ReportError(argv[iIndex], "too many arguments");
         return (2);
      }
   }
   if (!opt.pszPattern) {
      usage();
      return (argc == 1 ? 0 : 2);
   }
   if (strchr(opt.pszPattern, '/')) {
      ReportError(opt.pszPattern, "supply a filename pattern and a separate directory");
      return (2);
   }
   pszRoot = strdup(opt.pszRoot ? opt.pszRoot :
                    ((opt.usSwitches & ALLDRIVES) ? "/" : "."));
   if (!pszRoot) {
      ReportError("directory", "out of memory");
      return (1);
   }
   // A trailing slash must not bypass O_NOFOLLOW on the root directory.
   while (strlen(pszRoot) > 1 && pszRoot[strlen(pszRoot) - 1] == '/')
      pszRoot[strlen(pszRoot) - 1] = 0;
   if (!(opt.usSwitches & NOFILEINFO))
      printf("Quick File Finder for Linux version %s\n"
             "Copyright 1989-95 by Ronald C. Bieber; MIT License (see LICENSE).\n", VERSION);
   walkdirs(pszRoot);
   free(pszRoot);
   if (!(opt.usSwitches & NOFILEINFO)) {
      putchar('\n');
      ShowTotals(&totals, "found");
   }
   CloseTerminal();
   if (fflush(stdout) || ferror(stdout))
      bError = TRUE;
   return (bInterrupted ? 128 + bInterrupted : (bError ? 1 : 0));
}
