"""Behavioral tests; every file and deletion target lives in a temporary tree."""

import errno
import io
import os
from pathlib import Path
import pty
import random
import re
import select
import signal
import struct
import subprocess
import sys
import tempfile
import tarfile
import termios
import time
import unittest
import unicodedata
import zipfile
import zlib


szExecutable = str(Path(sys.argv.pop(1)).resolve())
usDate = ((2024 - 1980) << 9) | (6 << 5) | 15
usTime = (13 << 11) | (42 << 5) | 15


def CreateARC(szName=b"FOUND.TXT", bOld=False):
    byHeader = struct.pack("<BB13sIHHH", 26, 1 if bOld else 2,
                           szName, 3, usDate, usTime, 0)
    if not bOld:
        byHeader += struct.pack("<I", 3)
    return byHeader + b"abc" + b"\x1a\0"


def CreateLZH(szName=b"FOUND.TXT", byLevel=0):
    byHeader = bytearray(struct.pack("<BB5sIIHHBBB", 0, 0, b"-lh0-",
                                     3, 3, usTime, usDate, 32,
                                     byLevel, len(szName)) + szName + b"\0\0")
    byHeader[0] = len(byHeader) - 2
    byHeader[1] = sum(byHeader[2:]) & 255
    return bytes(byHeader) + b"abc\0"


def CreateDWC():
    byEntry = struct.pack("<13sIIII BB B H", b"FOUND.TXT", 3, 1718458920,
                          3, 0, 0, 0, 0, 0)
    byTrailer = struct.pack("<HB13sII3s", 27, len(byEntry), b"", 0, 1, b"DWC")
    return b"abc" + byEntry + byTrailer


def CreateZOO(bCycle=False, bDeleted=False, bLong=False, bOld=False, byDirectory=b""):
    ulStart = 34 if bOld else 42
    byHeader = struct.pack("<20sIII BB", b"ZOO 2.10 Archive.\x1a",
                           0xFDC4A7DC, ulStart, (-ulStart) & 0xFFFFFFFF, 2, 0)
    if not bOld:
        byHeader += struct.pack("<BIHB", 1, 0, 0, 0)
    byLongName = b"LongFilename.TXT\0"
    byVariable = (struct.pack("<HBHBB", 2 + len(byLongName) + len(byDirectory), 127, 0,
                              len(byLongName), len(byDirectory)) + byLongName +
                  byDirectory) if bLong else b""
    ulData = ulStart + 51 + len(byVariable)
    ulNext = ulStart if bCycle else ulData + 3
    byEntry = struct.pack("<IBBIIHHHII BBBBIH13s", 0xFDC4A7DC,
                          2 if bLong else 1, 0, ulNext, ulData,
                          usDate, usTime, 0, 3, 3, 1, 0,
                          int(bDeleted), 0, 0, 0, b"FOUND.TXT")
    byEnd = struct.pack("<IBBI", 0xFDC4A7DC, 1, 0, 0) + bytes(41)
    return byHeader + byEntry + byVariable + b"abc" + byEnd


def ARJHeader(byName, byType, byData=b"", usFlags=0, byExtra=b""):
    byHeader = struct.pack("<8B4I3H", 30, 3, 1, 0, usFlags, 0, byType, 0,
                           (usDate << 16) | usTime, len(byData), len(byData),
                           zlib.crc32(byData), 0, 32, 0) + byName + b"\0\0"
    byRecord = (struct.pack("<HH", 0xEA60, len(byHeader)) + byHeader +
                struct.pack("<I", zlib.crc32(byHeader)))
    if byExtra:
        byRecord += struct.pack("<H", len(byExtra)) + byExtra + struct.pack("<I", zlib.crc32(byExtra))
    return byRecord + b"\0\0" + byData


def CreateARJ(aEntries=None, usFlags=0, byExtra=b""):
    if aEntries is None:
        aEntries = [(b"folder/FOUND.TXT", b"abc")]
    byArchive = ARJHeader(b"sample.arj", 2, usFlags=usFlags, byExtra=byExtra)
    for byName, byData in aEntries:
        byArchive += ARJHeader(byName, 3 if byName.endswith(b"/") else 0, byData)
    return byArchive + struct.pack("<HH", 0xEA60, 0)


def CreateCAB(aEntries=None, bReserve=False):
    if aEntries is None:
        aEntries = [(b"folder\\FOUND.TXT", b"abc")]
    byFiles = b""
    byPayload = b""
    for byName, byData in aEntries:
        byFiles += struct.pack("<IIHHHH", len(byData), len(byPayload), 0,
                               usDate, usTime, 32) + byName + b"\0"
        byPayload += byData
    byReserve = struct.pack("<HBB", 3, 2, 0) + b"abc" if bReserve else b""
    byFolderReserve = b"xy" if bReserve else b""
    ulFiles = 36 + len(byReserve) + 8 + len(byFolderReserve)
    ulData = ulFiles + len(byFiles)
    byHeader = struct.pack("<4sIIIII BB HHHHH", b"MSCF", 0,
                           ulData + 8 + len(byPayload), 0, ulFiles, 0,
                           3, 1, 1, len(aEntries), 4 if bReserve else 0, 0, 0)
    return (byHeader + byReserve + struct.pack("<IHH", ulData, 1, 0) + byFolderReserve +
            byFiles + struct.pack("<IHH", 0, len(byPayload), len(byPayload)) + byPayload)


def RARBlock(byType, usFlags, byHeader=b"", byData=b""):
    byBody = struct.pack("<BHH", byType, usFlags, len(byHeader) + 7) + byHeader
    return struct.pack("<H", zlib.crc32(byBody) & 65535) + byBody + byData


def CreateRAR(aEntries=None, usMainFlags=0, bUnicode=False, ullSize=None):
    if aEntries is None:
        aEntries = [("folder/FOUND.TXT", b"abc")]
    byArchive = b"Rar!\x1a\x07\0" + RARBlock(0x73, usMainFlags, bytes(6))
    for szName, byData in aEntries:
        byName = szName.encode()
        usFlags = 0x8000 | (0xE0 if szName.endswith("/") else 0)
        byHigh = b""
        if bUnicode:
            byUnits = szName.encode("utf-16le")
            byEncoded = b"\0"
            for iIndex in range(0, len(byUnits), 8):
                byEncoded += b"\xaa" + byUnits[iIndex:iIndex + 8]
            byName = szName.encode("ascii", "replace") + b"\0" + byEncoded
            usFlags |= 0x200
        ullOriginal = len(byData) if ullSize is None else ullSize
        if ullOriginal > 0xFFFFFFFF:
            usFlags |= 0x100
            byHigh = struct.pack("<II", 0, ullOriginal >> 32)
        byHeader = struct.pack("<IIBIIBBHI", len(byData), ullOriginal & 0xFFFFFFFF,
                               0, zlib.crc32(byData), (usDate << 16) | usTime,
                               20, 0x30 if ullSize is None else 0x33, len(byName), 32)
        byArchive += RARBlock(0x74, usFlags, byHeader + byHigh + byName, byData)
    return byArchive + RARBlock(0x7B, 0)


def PAXRecord(szKey, szValue):
    byRecord = (szKey + "=" + szValue + "\n").encode()
    cbLength = len(byRecord) + 2
    while len(str(cbLength)) + 1 + len(byRecord) != cbLength:
        cbLength = len(str(cbLength)) + 1 + len(byRecord)
    return str(cbLength).encode() + b" " + byRecord


class QuickFindTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="qf-test-")
        self.pathRoot = Path(self.tmp.name)
        self.env = dict(os.environ, LC_ALL="C", TZ="UTC", QF_OPT="")

    def tearDown(self):
        self.tmp.cleanup()

    def RunQF(self, *aszArguments, iExpected=0, envExtra=None, byInput=None):
        env = self.env.copy()
        if envExtra:
            env.update(envExtra)
        proc = subprocess.run([szExecutable, *map(str, aszArguments)],
                              cwd=self.pathRoot, env=env,
                              stdin=subprocess.DEVNULL if byInput is None else None,
                              input=byInput,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              start_new_session=True, timeout=10)
        self.assertEqual(proc.returncode, iExpected, proc.stdout + proc.stderr)
        self.assertNotIn(b"Sanitizer", proc.stderr)
        self.assertNotIn(b"runtime error:", proc.stderr)
        return proc

    def RunTerminal(self, aszArguments, aActions, iExpected=0):
        iPID, iTerminal = pty.fork()
        if not iPID:
            os.chdir(self.pathRoot)
            os.execve(szExecutable, [szExecutable, *map(str, aszArguments)], self.env)
        byOutput = b""
        iAction = 0
        iStatus = None
        tDeadline = time.monotonic() + 10
        try:
            while time.monotonic() < tDeadline:
                if select.select([iTerminal], [], [], 0.05)[0]:
                    try:
                        byData = os.read(iTerminal, 65536)
                    except OSError as ex:
                        if ex.errno != errno.EIO:
                            raise
                        byData = b""
                    byOutput += byData
                if iAction < len(aActions) and aActions[iAction][0] in byOutput:
                    aActions[iAction][1](iPID, iTerminal)
                    iAction += 1
                iWaited, iNewStatus = os.waitpid(iPID, os.WNOHANG)
                if iWaited:
                    iStatus = iNewStatus
                    break
            self.assertIsNotNone(iStatus, byOutput)
            self.assertEqual(os.waitstatus_to_exitcode(iStatus), iExpected, byOutput)
            self.assertEqual(iAction, len(aActions), byOutput)
            self.assertNotIn(b"Sanitizer", byOutput)
            self.assertNotIn(b"runtime error:", byOutput)
            usFlags = termios.tcgetattr(iTerminal)[3]
            self.assertTrue(usFlags & termios.ICANON, byOutput)
            self.assertTrue(usFlags & termios.ECHO, byOutput)
            return byOutput
        finally:
            if iStatus is None:
                os.kill(iPID, signal.SIGKILL)
                os.waitpid(iPID, 0)
            os.close(iTerminal)

    def test_recursive_case_and_hidden_directories(self):
        (self.pathRoot / ".hidden").mkdir()
        (self.pathRoot / ".hidden" / "Mixed.C").write_text("abc")
        (self.pathRoot / "lower.c").write_text("ab")
        proc = self.RunQF("*.C")
        self.assertIn(b"Mixed.C", proc.stdout)
        self.assertNotIn(b"lower.c", proc.stdout)
        proc = self.RunQF("-i", "*.c", "-t", self.pathRoot)
        self.assertIn(b"2 file(s), 0 directory(s), 5 bytes found", proc.stdout)

    def test_default_root_and_option_position(self):
        (self.pathRoot / "One.txt").touch()
        self.assertIn(b"One.txt", self.RunQF("*.txt", "-b", self.pathRoot).stdout)
        self.assertIn(b"One.txt", self.RunQF("-b", "*.txt").stdout)
        self.assertIn(b"One.txt", self.RunQF("-A", "-C", "*.txt", "-b").stdout)

    def test_percent_newline_and_terminal_control_names(self):
        (self.pathRoot / "name%n%s\n\x1b.txt").touch()
        proc = self.RunQF("*.txt")
        self.assertIn(b"name%n%s\\x0a\\x1b.txt", proc.stdout)
        self.assertNotIn(b"\x1b", proc.stdout)

    def test_dash_name_and_absolute_directory(self):
        (self.pathRoot / "-notes.txt").touch()
        self.assertIn(b"-notes.txt", self.RunQF("--", "-notes.txt", self.pathRoot).stdout)

    def test_invalid_arguments(self):
        for aszArgs in [("--wat", "*"), ("*", ".", "."),
                        ("--delete",), ("/tmp/*",), ("/T",)]:
            with self.subTest(aszArgs=aszArgs):
                self.RunQF(*aszArgs, iExpected=2)
        self.RunQF("--help")

    def test_environment_options_are_copied_and_validated(self):
        (self.pathRoot / "MATCH.TXT").touch()
        proc = self.RunQF("*.txt", envExtra={"QF_OPT": "-b\t-i\n-t"})
        self.assertIn(b"MATCH.TXT", proc.stdout)
        self.assertNotIn(b"bytes", proc.stdout)
        self.RunQF("*", envExtra={"QF_OPT": "filename"}, iExpected=2)
        self.RunQF("*", envExtra={"QF_OPT": "--help"}, iExpected=2)

    def test_linux_star_and_question_mark(self):
        for szName in ["abc", "axxb", "ac", "a", "extensionless"]:
            (self.pathRoot / szName).touch()
        proc = self.RunQF("-b", "a*b")
        self.assertIn(b"axxb", proc.stdout)
        self.assertNotIn(b"abc", proc.stdout)
        proc = self.RunQF("-b", "a?")
        self.assertIn(b"   ac\n", proc.stdout)
        self.assertNotIn(b"   a\n", proc.stdout)

    def test_dos_matching_is_explicit(self):
        for szName in ["ABC.TXT", "extensionless", "axxb.txt"]:
            (self.pathRoot / szName).touch()
        proc = self.RunQF("-b", "--dos-match", "a*b.txt")
        self.assertIn(b"ABC.TXT", proc.stdout)
        self.assertIn(b"axxb.txt", proc.stdout)
        self.assertIn(b"extensionless", self.RunQF("--dos-match", "*.*").stdout)
        self.assertIn(b"ABC.TXT", self.RunQF("--dos-match", ".TXT").stdout)
        self.assertNotIn(b"extensionless", self.RunQF("*.*").stdout)

    def test_symlink_cycles_and_external_directories(self):
        (self.pathRoot / "child").mkdir()
        (self.pathRoot / "child" / "found.txt").touch()
        (self.pathRoot / "child" / "cycle").symlink_to(self.pathRoot)
        (self.pathRoot / "broken").symlink_to("nowhere")
        proc = self.RunQF("*.txt")
        self.assertEqual(proc.stdout.count(b"found.txt"), 1)
        self.RunQF("*", self.pathRoot / "child" / "cycle", iExpected=1)
        self.RunQF("*", str(self.pathRoot / "child" / "cycle") + "/", iExpected=1)

    def test_large_sparse_files_and_totals(self):
        with (self.pathRoot / "large.bin").open("wb") as pf:
            pf.truncate(5 * 1024**3)
        proc = self.RunQF("*.bin", "-t")
        self.assertIn(b"5,368,709,120 bytes", proc.stdout)
        self.assertIn(b"1 file(s), 0 directory(s), 5,368,709,120 bytes found", proc.stdout)

    def test_paths_beyond_old_limits(self):
        pathDirectory = self.pathRoot
        for iIndex in range(15):
            pathDirectory /= "directory" + str(iIndex) + "x" * 20
            pathDirectory.mkdir()
        (pathDirectory / ("n" * 240 + ".txt")).touch()
        proc = self.RunQF("*.txt")
        self.assertIn(b"n" * 240 + b".txt", proc.stdout)

    def test_missing_directory(self):
        self.RunQF("*", self.pathRoot / "missing", iExpected=1)

    def test_permission_failure_is_reported(self):
        if os.geteuid() == 0:
            self.skipTest("root bypasses directory access permissions")
        pathDirectory = self.pathRoot / "private"
        pathDirectory.mkdir()
        pathDirectory.chmod(0)
        try:
            proc = self.RunQF("*", iExpected=1)
            self.assertIn(b"Permission denied", proc.stderr)
        finally:
            pathDirectory.chmod(0o700)

    def test_directory_traversal_beyond_kernel_path_limit(self):
        iDirectory = os.open(self.pathRoot, os.O_RDONLY | os.O_DIRECTORY)
        try:
            for iIndex in range(110):
                szName = "d" * 40 + str(iIndex)
                os.mkdir(szName, dir_fd=iDirectory)
                iChild = os.open(szName, os.O_RDONLY | os.O_DIRECTORY,
                                 dir_fd=iDirectory)
                os.close(iDirectory)
                iDirectory = iChild
            iFile = os.open("deep.txt", os.O_WRONLY | os.O_CREAT,
                            0o600, dir_fd=iDirectory)
            os.close(iFile)
        finally:
            os.close(iDirectory)
        proc = self.RunQF("*.txt")
        self.assertIn(b"deep.txt", proc.stdout)
        self.assertGreater(len(proc.stdout), 4096)

    def test_error_paths_escape_terminal_controls(self):
        proc = self.RunQF("*", "missing\x1b", iExpected=1)
        self.assertIn(b"missing\\x1b", proc.stderr)
        self.assertNotIn(b"\x1b", proc.stderr)

    def test_zip_nested_names_and_totals(self):
        with zipfile.ZipFile(self.pathRoot / "sample.ZIP", "w") as zf:
            zf.writestr("inside/Found.txt", "abc")
            zf.writestr("another/Found.txt", "defg")
            zf.writestr("folder/", "")
        proc = self.RunQF("-co", "*.txt", "-t")
        self.assertIn(b"inside/Found.txt", proc.stdout)
        self.assertIn(b"another/Found.txt", proc.stdout)
        self.assertIn(b"2 file(s), 0 directory(s), 7 bytes found", proc.stdout)
        self.assertEqual(sum(byLine == b"   sample.ZIP/"
                             for byLine in proc.stdout.splitlines()), 1)

    def test_zip_empty_and_maximum_comment(self):
        with zipfile.ZipFile(self.pathRoot / "empty.zip", "w"):
            pass
        with zipfile.ZipFile(self.pathRoot / "comment.zip", "w") as zf:
            zf.writestr("found.txt", "abc")
            zf.comment = b"z" * 65535
        proc = self.RunQF("-co", "*.txt")
        self.assertIn(b"found.txt", proc.stdout)

    def test_matching_archive_is_listed_once(self):
        pathArchive = self.pathRoot / "sample.zip"
        with zipfile.ZipFile(pathArchive, "w") as zf:
            zf.writestr("found.txt", "abc")
        proc = self.RunQF("-cf", "*")
        self.assertEqual(sum(byLine == b"   sample.zip/"
                             for byLine in proc.stdout.splitlines()), 1)
        self.assertIn(f"2 file(s), 0 directory(s), {pathArchive.stat().st_size + 3:,} bytes found".encode(),
                      proc.stdout)

    def test_zip_long_member_names(self):
        szName = "folder/" + "x" * 4000 + ".txt"
        with zipfile.ZipFile(self.pathRoot / "long.zip", "w") as zf:
            zf.writestr(szName, "abc")
        self.assertIn(szName.encode(), self.RunQF("-co", "*.txt").stdout)

    def test_zip_comment_containing_a_false_end_record(self):
        with zipfile.ZipFile(self.pathRoot / "comment.zip", "w") as zf:
            zf.writestr("found.txt", "abc")
            zf.comment = b"PK\x05\x06" + bytes(18)
        self.assertIn(b"found.txt", self.RunQF("-co", "*.txt").stdout)

    def test_zip_record_at_old_buffer_boundary(self):
        with zipfile.ZipFile(self.pathRoot / "boundary.zip", "w") as zf:
            zf.writestr("found.txt", "abc")
            zf.comment = b"z" * (8192 - 22)
        self.assertIn(b"found.txt", self.RunQF("-co", "*.txt").stdout)

    def test_zip_truncated_invalid_lengths_and_zip64(self):
        pathArchive = self.pathRoot / "bad.zip"
        with zipfile.ZipFile(pathArchive, "w") as zf:
            zf.writestr("found.txt", "abc")
        byOriginal = pathArchive.read_bytes()
        for szKind in ["truncated", "filename", "zip64", "multidisk", "offset", "nul"]:
            with self.subTest(szKind=szKind):
                byData = bytearray(byOriginal)
                iCentral = byData.index(b"PK\x01\x02")
                iEnd = byData.index(b"PK\x05\x06")
                if szKind == "truncated":
                    byData = byData[:-1]
                elif szKind == "filename":
                    struct.pack_into("<H", byData, iCentral + 28, 65535)
                elif szKind == "zip64":
                    struct.pack_into("<I", byData, iEnd + 16, 0xFFFFFFFF)
                elif szKind == "multidisk":
                    struct.pack_into("<H", byData, iEnd + 4, 1)
                elif szKind == "offset":
                    struct.pack_into("<I", byData, iCentral + 42, 0xFFFFFFFE)
                else:
                    byData[iCentral + 46] = 0
                pathArchive.write_bytes(byData)
                self.RunQF("-co", "*", iExpected=1)

    def test_all_legacy_archive_extensions(self):
        for szExtension, byData in [("ARC", CreateARC()), ("PAK", CreateARC()),
                                     ("LZH", CreateLZH()), ("LZS", CreateLZH()),
                                     ("DWC", CreateDWC()), ("ZOO", CreateZOO())]:
            (self.pathRoot / ("sample." + szExtension)).write_bytes(byData)
        proc = self.RunQF("-co", "*.TXT", "-t")
        self.assertEqual(proc.stdout.count(b"      *FOUND.TXT "), 6)
        self.assertIn(b"6 file(s), 0 directory(s), 18 bytes found", proc.stdout)
        self.assertIn(b"01:42 PM  Jun 15 2024", proc.stdout)

    def test_arc_old_header_and_partial_record(self):
        pathArchive = self.pathRoot / "old.arc"
        pathArchive.write_bytes(CreateARC(bOld=True))
        self.assertIn(b"3 bytes", self.RunQF("-co", "*").stdout)
        pathArchive.write_bytes(CreateARC()[:20])
        self.RunQF("-co", "*", iExpected=1)

    def test_lzh_long_names_and_bad_headers(self):
        pathArchive = self.pathRoot / "long.lzh"
        pathArchive.write_bytes(CreateLZH(b"a" * 200 + b".TXT"))
        self.assertIn(b"a" * 200, self.RunQF("-co", "*.TXT").stdout)
        for byData in [CreateLZH(byLevel=1), CreateLZH()[:10],
                       CreateLZH()[:2] + b"X" + CreateLZH()[3:]]:
            pathArchive.write_bytes(byData)
            self.RunQF("-co", "*", iExpected=1)

    def test_validation_lzh_lzs_samples(self):
        pathSource = Path(__file__).resolve().parent.parent / "archives"
        for pathDirectory in [pathSource / "good", pathSource / "qf_validation_v14" / "good"]:
            for szName in ["basic_lzh.lzh", "basic_lzs.lzs"]:
                with self.subTest(pathArchive=pathDirectory / szName):
                    pathArchive = self.pathRoot / szName
                    pathArchive.write_bytes((pathDirectory / szName).read_bytes())
                    proc = self.RunQF("-co", "*")
                    for byName in [b"README.TXT", b"MAIN.C", b"EMPTY.DAT"]:
                        self.assertIn(b"      *" + byName, proc.stdout)
                    self.assertIn(b"3 file(s), 0 directory(s), 66 bytes found", proc.stdout)
                    byBad = bytearray(pathArchive.read_bytes())
                    byBad[1] = 0
                    pathArchive.write_bytes(byBad)
                    self.assertIn(b"invalid LZH header checksum",
                                  self.RunQF("-co", "*", iExpected=1).stderr)
                    pathArchive.unlink()

    def test_zoo_deleted_long_old_and_cycle(self):
        pathArchive = self.pathRoot / "sample.zoo"
        pathArchive.write_bytes(CreateZOO(bDeleted=True))
        self.assertNotIn(b"FOUND.TXT", self.RunQF("-co", "*").stdout)
        pathArchive.write_bytes(CreateZOO(bLong=True))
        self.assertIn(b"LongFilename.TXT", self.RunQF("-co", "*.TXT").stdout)
        pathArchive.write_bytes(CreateZOO(bLong=True, byDirectory=b"stored/path/\0"))
        self.assertIn(b"stored/path/LongFilename.TXT", self.RunQF("-co", "*.TXT").stdout)
        pathArchive.write_bytes(CreateZOO(bOld=True))
        self.assertIn(b"FOUND.TXT", self.RunQF("-co", "*.TXT").stdout)
        pathArchive.write_bytes(CreateZOO(bCycle=True))
        self.RunQF("-co", "*", iExpected=1)

    def test_dwc_bad_counts(self):
        byData = bytearray(CreateDWC())
        struct.pack_into("<I", byData, len(byData) - 7, 0xFFFFFFFF)
        (self.pathRoot / "bad.dwc").write_bytes(byData)
        self.RunQF("-co", "*", iExpected=1)

    def test_malformed_archive_corpus(self):
        rng = random.Random(89)
        for iIndex in range(50):
            for szExtension in ["arc", "zip", "zoo", "dwc", "lzh", "arj", "cab", "rar", "tar"]:
                (self.pathRoot / (str(iIndex) + "." + szExtension)).write_bytes(
                    rng.randbytes(rng.randrange(0, 512)))
        self.RunQF("-co", "*", iExpected=1)

    def test_mutated_archive_headers(self):
        rng = random.Random(1995)
        with zipfile.ZipFile(self.pathRoot / "original.zip", "w") as zf:
            zf.writestr("found.txt", "abc")
        aArchives = [("arc", CreateARC()), ("zoo", CreateZOO()),
                     ("dwc", CreateDWC()), ("lzh", CreateLZH()),
                     ("zip", (self.pathRoot / "original.zip").read_bytes()),
                     ("arj", CreateARJ()), ("cab", CreateCAB()), ("rar", CreateRAR()),
                     ("tar", tarfile.TarInfo("empty.txt").tobuf() + bytes(1024))]
        for szExtension, byOriginal in aArchives:
            for iIndex in range(60):
                byData = bytearray(byOriginal)
                iOffset = rng.randrange(len(byData))
                byData[iOffset] ^= 1 << rng.randrange(8)
                (self.pathRoot / (str(iIndex) + "." + szExtension)).write_bytes(byData)
        self.RunQF("-co", "*", iExpected=1)

    def test_archive_symlink_and_fifo_are_not_opened(self):
        (self.pathRoot / "linked.zip").symlink_to("missing.zip")
        os.mkfifo(self.pathRoot / "pipe.zip")
        self.RunQF("-co", "*")

    def test_new_native_archive_formats(self):
        aArchives = [("ARJ", CreateARJ()), ("CAB", CreateCAB()), ("RAR", CreateRAR())]
        for szExtension, byData in aArchives:
            (self.pathRoot / ("sample." + szExtension)).write_bytes(byData)
        proc = self.RunQF("-co", "*.TXT", "-t")
        self.assertIn(b"3 file(s), 0 directory(s), 9 bytes found", proc.stdout)
        self.assertEqual(proc.stdout.count(b"FOUND.TXT"), 3)
        self.assertIn(b"01:42 PM  Jun 15 2024", proc.stdout)

    def test_new_formats_multiple_members_and_directories(self):
        aEntries = [("folder/", b""), ("folder/first.txt", b"abc"),
                    ("another/second.txt", b"defg"), ("other.bin", b"h")]
        for szExtension, Create in [("arj", CreateARJ), ("cab", CreateCAB), ("rar", CreateRAR)]:
            with self.subTest(szExtension=szExtension):
                aEncoded = aEntries if szExtension == "rar" else [(sz.encode(), byData) for sz, byData in aEntries]
                pathArchive = self.pathRoot / ("sample." + szExtension)
                pathArchive.write_bytes(Create(aEncoded))
                proc = self.RunQF("-co", "*.txt")
                self.assertIn(b"2 file(s), 0 directory(s), 7 bytes found", proc.stdout)
                self.assertNotIn(b"other.bin", proc.stdout)
                pathArchive.unlink()

    def test_arj_extended_headers_and_corruption(self):
        pathArchive = self.pathRoot / "sample.arj"
        byOriginal = CreateARJ(byExtra=b"extra metadata")
        pathArchive.write_bytes(byOriginal)
        self.assertIn(b"FOUND.TXT", self.RunQF("-co", "*").stdout)
        for byData in [byOriginal[:-3], byOriginal[:20],
                       byOriginal[:10] + bytes([byOriginal[10] ^ 1]) + byOriginal[11:]]:
            pathArchive.write_bytes(byData)
            self.RunQF("-co", "*", iExpected=1)
        pathArchive.write_bytes(CreateARJ(usFlags=4))
        self.assertIn(b"split ARJ", self.RunQF("-co", "*", iExpected=1).stderr)

    def test_arj_unix_timestamps(self):
        for byOS in [2, 8]:
            byData = bytearray(CreateARJ())
            iFile = len(ARJHeader(b"sample.arj", 2))
            cbHeader = struct.unpack_from("<H", byData, iFile + 2)[0]
            byData[iFile + 7] = byOS
            struct.pack_into("<I", byData, iFile + 12, 1718458920)
            struct.pack_into("<I", byData, iFile + 4 + cbHeader,
                             zlib.crc32(byData[iFile + 4:iFile + 4 + cbHeader]))
            (self.pathRoot / "unix.arj").write_bytes(byData)
            self.assertIn(b"01:42 PM  Jun 15 2024", self.RunQF("-co", "*.TXT").stdout)

    def test_cab_reserve_area_and_long_names(self):
        byName = ("folder/" + "long" * 100 + ".txt").encode()
        (self.pathRoot / "sample.cab").write_bytes(CreateCAB([(byName, b"abc")], bReserve=True))
        self.assertIn(byName, self.RunQF("-co", "*.txt").stdout)

    def test_cab_bad_offsets_lengths_and_split(self):
        pathArchive = self.pathRoot / "bad.cab"
        byOriginal = CreateCAB()
        for szKind in ["offset", "size", "folder", "unterminated", "split", "truncated"]:
            with self.subTest(szKind=szKind):
                byData = bytearray(byOriginal)
                if szKind == "offset":
                    struct.pack_into("<I", byData, 16, len(byData) + 1)
                elif szKind == "size":
                    struct.pack_into("<I", byData, 8, len(byData) + 1)
                elif szKind == "folder":
                    struct.pack_into("<H", byData, 44 + 8, 100)
                elif szKind == "unterminated":
                    byData[60:len(byData) - 11] = b"X" * (len(byData) - 71)
                elif szKind == "split":
                    struct.pack_into("<H", byData, 30, 1)
                else:
                    byData = byData[:-1]
                pathArchive.write_bytes(byData)
                self.RunQF("-co", "*", iExpected=1)

    def test_rar4_unicode_names_and_large_metadata(self):
        pathArchive = self.pathRoot / "sample.rar"
        szName = "folder/café-\U0001f596.txt"
        pathArchive.write_bytes(CreateRAR([(szName, b"abc")], bUnicode=True))
        self.assertIn(szName.encode(), self.RunQF("-co", "*.txt").stdout)
        pathArchive.write_bytes(CreateRAR(ullSize=5 * 1024**3))
        self.assertIn(b"5,368,709,120 bytes", self.RunQF("-co", "*").stdout)

    def test_rar4_rejects_rar5_split_encrypted_and_bad_headers(self):
        pathArchive = self.pathRoot / "bad.rar"
        for byData in [b"Rar!\x1a\x07\x01\0", CreateRAR(usMainFlags=1),
                       CreateRAR(usMainFlags=0x80), CreateRAR()[:25],
                       CreateRAR()[:30] + bytes([CreateRAR()[30] ^ 1]) + CreateRAR()[31:]]:
            pathArchive.write_bytes(byData)
            self.RunQF("-co", "*", iExpected=1)

    def test_rar4_invalid_repeated_main_and_unicode_runs(self):
        pathArchive = self.pathRoot / "bad.rar"
        byMain = b"Rar!\x1a\x07\0" + RARBlock(0x73, 0, bytes(6))
        pathArchive.write_bytes(byMain + RARBlock(0x73, 2))
        self.RunQF("-co", "*", iExpected=1)
        # Recompute the checksum so invalid encoded-name lengths reach the decoder.
        byBadName = b"a\0\0\xc0\x7f"
        byHeader = struct.pack("<IIBIIBBHI", 0, 0, 0, 0,
                               (usDate << 16) | usTime, 20, 0x30, len(byBadName), 32)
        pathArchive.write_bytes(byMain + RARBlock(0x74, 0x8200, byHeader + byBadName))
        self.RunQF("-co", "*", iExpected=1)

    def test_rar4_unicode_copy_runs(self):
        byName = b"folder/found.txt"
        byEncoded = byName + b"\0\0\xc0" + bytes([len(byName) - 2])
        byHeader = struct.pack("<IIBIIBBHI", 0, 0, 0, 0,
                               (usDate << 16) | usTime, 20, 0x30, len(byEncoded), 32)
        byMain = b"Rar!\x1a\x07\0" + RARBlock(0x73, 0, bytes(6))
        (self.pathRoot / "unicode.rar").write_bytes(byMain + RARBlock(0x74, 0x8200, byHeader + byEncoded))
        self.assertIn(byName, self.RunQF("-co", "*.txt").stdout)

    def test_tar_v7_ustar_gnu_and_pax(self):
        for szKind, iFormat in [("ustar", tarfile.USTAR_FORMAT), ("gnu", tarfile.GNU_FORMAT),
                               ("pax", tarfile.PAX_FORMAT)]:
            with self.subTest(szKind=szKind):
                pathArchive = self.pathRoot / "sample.tar"
                with tarfile.open(pathArchive, "w", format=iFormat) as tf:
                    ti = tarfile.TarInfo("directory/")
                    ti.type = tarfile.DIRTYPE
                    tf.addfile(ti)
                    for szName, byData in [("folder/first.txt", b"abc"),
                                            ("another/second.txt", b"defg")]:
                        ti = tarfile.TarInfo(szName)
                        ti.size = len(byData)
                        ti.mtime = 1718458920
                        tf.addfile(ti, io.BytesIO(byData))
                proc = self.RunQF("-co", "*.txt", "-t")
                self.assertIn(b"2 file(s), 0 directory(s), 7 bytes found", proc.stdout)
                self.assertIn(b"01:42 PM  Jun 15 2024", proc.stdout)
                if szKind == "ustar":
                    byData = bytearray(pathArchive.read_bytes())
                    # Produce a traditional header by clearing the optional USTAR fields.
                    for iOffset in [0, 512, 1536]:
                        byData[iOffset + 257:iOffset + 512] = bytes(255)
                        byData[iOffset + 148:iOffset + 156] = b" " * 8
                        ulSum = sum(byData[iOffset:iOffset + 512])
                        byData[iOffset + 148:iOffset + 156] = f"{ulSum:06o}\0 ".encode()
                    pathArchive.write_bytes(byData)
                    self.assertIn(b"first.txt", self.RunQF("-co", "*.txt").stdout)
                pathArchive.unlink()

    def test_tar_long_names_and_ustar_prefixes(self):
        for iFormat in [tarfile.USTAR_FORMAT, tarfile.GNU_FORMAT, tarfile.PAX_FORMAT]:
            pathArchive = self.pathRoot / "long.tar"
            szName = "directory" * 14 + "/" + "long" * 20 + ".txt"
            with tarfile.open(pathArchive, "w", format=iFormat) as tf:
                ti = tarfile.TarInfo(szName)
                ti.size = 3
                tf.addfile(ti, io.BytesIO(b"abc"))
            self.assertIn(szName.encode(), self.RunQF("-co", "*.txt").stdout)
            pathArchive.unlink()

    def test_tar_pax_size_and_global_time(self):
        byGlobal = PAXRecord("mtime", "1718458920.25")
        byLocal = PAXRecord("path", "folder/PAX.txt") + PAXRecord("size", "3")
        byArchive = b""
        for byType, byData in [(tarfile.XGLTYPE, byGlobal), (tarfile.XHDTYPE, byLocal)]:
            ti = tarfile.TarInfo("metadata")
            ti.type = byType
            ti.size = len(byData)
            byArchive += ti.tobuf() + byData + bytes((-len(byData)) % 512)
        ti = tarfile.TarInfo("placeholder")
        byArchive += ti.tobuf() + b"abc" + bytes(509) + bytes(1024)
        (self.pathRoot / "sample.tar").write_bytes(byArchive)
        proc = self.RunQF("-co", "*.txt")
        self.assertIn(b"folder/PAX.txt", proc.stdout)
        self.assertIn(b"3 bytes", proc.stdout)
        self.assertIn(b"01:42 PM  Jun 15 2024", proc.stdout)

    def test_tar_preserves_literal_backslashes_in_names(self):
        with tarfile.open(self.pathRoot / "names.tar", "w") as tf:
            tf.addfile(tarfile.TarInfo("literal\\name.txt"))
            tf.addfile(tarfile.TarInfo("literal-end\\"))
        proc = self.RunQF("-co", "literal*")
        self.assertIn(b"literal\\\\name.txt", proc.stdout)
        self.assertIn(b"literal-end\\\\", proc.stdout)
        self.assertIn(b"2 file(s)", proc.stdout)

    def test_tar_base256_large_sparse_payload(self):
        pathArchive = self.pathRoot / "large.tar"
        ti = tarfile.TarInfo("large.bin")
        ti.size = 9 * 1024**3
        with pathArchive.open("wb") as pf:
            pf.write(ti.tobuf(format=tarfile.GNU_FORMAT))
            pf.seek(512 + ti.size)
            pf.write(bytes(1024))
        self.assertIn(b"9,663,676,416 bytes", self.RunQF("-co", "*.bin").stdout)

    def test_tar_invalid_checksums_truncation_pax_and_sparse(self):
        pathArchive = self.pathRoot / "bad.tar"
        ti = tarfile.TarInfo("found.txt")
        ti.size = 3
        byOriginal = ti.tobuf() + b"abc" + bytes(509) + bytes(1024)
        byBad = bytearray(byOriginal)
        byBad[0] ^= 1
        for byData in [bytes(byBad), byOriginal[:512], byOriginal[:1536]]:
            pathArchive.write_bytes(byData)
            self.RunQF("-co", "*", iExpected=1)
        for byType, byPayload in [(tarfile.XHDTYPE, b"0 path=x\n"),
                                   (tarfile.XHDTYPE, PAXRecord("GNU.sparse.size", "3")),
                                   (tarfile.XHDTYPE, PAXRecord("SCHILY.realsize", "3")),
                                   (tarfile.GNUTYPE_SPARSE, b"")]:
            ti = tarfile.TarInfo("metadata")
            ti.type = byType
            ti.size = len(byPayload)
            pathArchive.write_bytes(ti.tobuf(format=tarfile.GNU_FORMAT) + byPayload +
                                     bytes((-len(byPayload)) % 512) + bytes(1024))
            self.RunQF("-co", "*", iExpected=1)

    def test_empty_new_archives(self):
        for szExtension, byData in [("arj", CreateARJ([])), ("cab", CreateCAB([])),
                                     ("rar", CreateRAR([])), ("tar", bytes(1024))]:
            (self.pathRoot / ("empty." + szExtension)).write_bytes(byData)
        self.assertIn(b"0 file(s)", self.RunQF("-co", "*").stdout)

    def test_removed_deletion_options_are_rejected(self):
        pathFile = self.pathRoot / "keep.txt"
        pathFile.write_text("keep")
        for szOption in ["-d", "-D", "-n", "-N", "--delete", "--no-prompt"]:
            with self.subTest(szOption=szOption):
                self.RunQF(szOption, "*.txt", byInput=b"y\n", iExpected=2)
                self.RunQF("*.txt", envExtra={"QF_OPT": szOption}, iExpected=2)
                self.assertEqual(pathFile.read_text(), "keep")
        byHelp = self.RunQF("--help").stdout
        self.assertNotIn(b"--delete", byHelp)
        self.assertNotIn(b"--no-prompt", byHelp)

    def test_supported_formats_are_shown_in_help_and_standalone(self):
        byFormats = self.RunQF("--formats").stdout
        self.assertEqual(self.RunQF("-e").stdout, byFormats)
        self.assertEqual(self.RunQF("-E").stdout, byFormats)
        self.assertIn(byFormats, self.RunQF("--help").stdout)
        for byExtension in [b".ARC", b".PAK", b".ZIP", b".ZOO", b".DWC", b".LZH", b".LZS",
                            b".ARJ", b".CAB", b".RAR", b".TAR"]:
            self.assertIn(byExtension, byFormats)
        self.assertIn(b"no ZIP64", byFormats)
        self.assertIn(b"Level-0", byFormats)
        self.RunQF("*", envExtra={"QF_OPT": "--formats"}, iExpected=2)

    def test_archive_search_preserves_disk_files(self):
        pathArchive = self.pathRoot / "sample.zip"
        pathFile = self.pathRoot / "found.txt"
        pathFile.write_text("keep")
        with zipfile.ZipFile(pathArchive, "w") as zf:
            zf.writestr("found.txt", "abc")
        byBefore = pathArchive.read_bytes()
        self.RunQF("-co", "*.txt")
        self.RunQF("-cf", "*.txt")
        self.assertEqual(pathArchive.read_bytes(), byBefore)
        self.assertEqual(pathFile.read_text(), "keep")

    def test_name_first_columns_fit_the_directory(self):
        aFiles = [("short.txt", 1), ("long" * 45 + ".txt", 1234),
                  ("control\nname.txt", 12), ("café.txt", 0), ("界.txt", 15),
                  ("cafe\u0301.txt", 21), ("back\\slash.txt", 2)]
        dExpected = {}
        for szName, cbSize in aFiles:
            pathFile = self.pathRoot / szName
            pathFile.write_bytes(b"x" * cbSize)
            os.utime(pathFile, (1718458920, 1718458920))
            szRendered = szName.replace("\\", "\\\\").replace("\n", "\\x0a")
            dExpected[szRendered] = (f"{cbSize:,}", "bytes")
        pathDirectory = self.pathRoot / "directory"
        pathDirectory.mkdir()
        os.utime(pathDirectory, (1718458920, 1718458920))
        dExpected["directory"] = ("<DIR>", "")
        byPath = os.fsencode(self.pathRoot) + b"/invalid-\xff.txt"
        iFile = os.open(byPath, os.O_WRONLY | os.O_CREAT, 0o600)
        os.write(iFile, b"x")
        os.close(iFile)
        os.utime(byPath, (1718458920, 1718458920))
        dExpected["invalid-\\xff.txt"] = ("1", "bytes")
        proc = self.RunQF("*")
        cNameWidth = max(sum(0 if unicodedata.combining(c) else
                            2 if unicodedata.east_asian_width(c) in "WF" else 1
                            for c in szName) for szName in dExpected)
        cSizeWidth = max(len(aInfo[0]) for aInfo in dExpected.values())
        aRows = proc.stdout.decode().splitlines()
        for szName, (szSize, szUnit) in dExpected.items():
            cName = sum(0 if unicodedata.combining(c) else
                        2 if unicodedata.east_asian_width(c) in "WF" else 1 for c in szName)
            szExpected = ("   " + szName + " " * (cNameWidth - cName + 2) +
                          szSize.rjust(cSizeWidth) + " " + szUnit.ljust(5) +
                          "  01:42 PM  Jun 15 2024")
            self.assertIn(szExpected, aRows)
            self.assertNotIn("\t", szExpected)

    def test_name_columns_reset_for_each_directory(self):
        (self.pathRoot / ("long" * 30 + ".txt")).touch()
        pathChild = self.pathRoot / "child"
        pathChild.mkdir()
        (pathChild / "a.txt").touch()
        (pathChild / "b.txt").touch()
        proc = self.RunQF("*.txt")
        self.assertIn(b"   a.txt  0 bytes", proc.stdout)
        self.assertIn(b"   b.txt  0 bytes", proc.stdout)

    def test_archives_are_headings_with_indented_member_columns(self):
        szLongName = "folder/" + "name" * 125 + ".txt"
        with zipfile.ZipFile(self.pathRoot / "sample.zip", "w") as zf:
            zf.writestr("short.txt", "abc")
            zf.writestr(szLongName, "abcd")
        (self.pathRoot / "disk.txt").touch()
        proc = self.RunQF("-cf", "*.txt")
        aRows = proc.stdout.splitlines()
        self.assertIn(b"   sample.zip/", aRows)
        iHeader = aRows.index(b"   sample.zip/")
        self.assertTrue(aRows[iHeader + 1].startswith(b"      *short.txt  "))
        self.assertTrue(aRows[iHeader + 2].startswith(b"      *" + szLongName.encode() + b"  "))
        self.assertEqual(aRows[iHeader + 1].index(b"bytes"), aRows[iHeader + 2].index(b"bytes"))
        self.assertIn(b"   disk.txt  0 bytes", proc.stdout)
        self.assertIn(b"      *short.txt", proc.stdout)

    def test_each_archive_has_its_own_columns_including_bare_mode(self):
        for szArchive, aMembers in [("first.zip", [("a.txt", "abc"), ("long-name.txt", "x")]),
                                     ("second.zip", [("b.txt", "ab"), ("c.txt", "abc")])]:
            with zipfile.ZipFile(self.pathRoot / szArchive, "w") as zf:
                for szName, szData in aMembers:
                    zf.writestr(szName, szData)
        proc = self.RunQF("-co", "*.txt")
        self.assertIn(b"      *b.txt  2 bytes", proc.stdout)
        self.assertIn(b"      *c.txt  3 bytes", proc.stdout)
        for szOption in ["-co", "-cf"]:
            proc = self.RunQF("-b", szOption, "*")
            self.assertIn(b"   first.zip/\n      *a.txt\n      *long-name.txt\n", proc.stdout)
            self.assertIn(b"   second.zip/\n      *b.txt\n      *c.txt\n", proc.stdout)
            self.assertNotIn(b"bytes", proc.stdout)
            self.assertEqual(proc.stdout.count(b"   first.zip/\n"), 1)
            self.assertEqual(proc.stdout.count(b"   second.zip/\n"), 1)

    def test_pause_is_disabled_for_pipes(self):
        for iIndex in range(30):
            (self.pathRoot / (str(iIndex) + ".txt")).touch()
        proc = self.RunQF("-p", "*.txt")
        self.assertNotIn(b"Program paused", proc.stdout)

    def test_pause_escape_and_terminal_restore(self):
        for iIndex in range(30):
            (self.pathRoot / (str(iIndex) + ".txt")).touch()
        self.RunTerminal(["-p", "*.txt"],
                         [(b"Program paused", lambda iPID, iFD: os.write(iFD, b"\x1b"))],
                         iExpected=130)

    def test_signal_during_pause_restores_terminal(self):
        for iIndex in range(30):
            (self.pathRoot / (str(iIndex) + ".txt")).touch()
        self.RunTerminal(["-p", "*.txt"],
                         [(b"Program paused", lambda iPID, iFD: os.kill(iPID, signal.SIGTERM))],
                         iExpected=143)


if __name__ == "__main__":
    unittest.main(verbosity=2)
