# Quick Find for Linux

The Linux build uses CMake and has no archive-library dependency. Its source
follows the original Hungarian notation, three-space indentation, and brace
style. Historical source and documentation are retained; fixes described here
apply to the native Linux executable. The DOS makefile has been moved to
`Makefile.dos`, and the DOS/OS/2/Windows builds have not been revalidated.

## Building and testing

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

CMake 3.20+, a C99 compiler, and Linux are required. Tests use Python 3.9+ and
generate their data in temporary directories. To omit tests and the Python
dependency, configure with `-DBUILD_TESTING=OFF`.

An optional installation uses the usual CMake prefix:

```sh
cmake --install build --prefix "$HOME/.local"
```

Address and undefined-behavior checks:

```sh
cmake -S . -B build-sanitize -DCMAKE_C_COMPILER=clang \
      -DCMAKE_BUILD_TYPE=Debug -DQF_ENABLE_SANITIZERS=ON
cmake --build build-sanitize
ctest --test-dir build-sanitize --output-on-failure
```

GCC also supports this option. Environments that prohibit LeakSanitizer's process
inspection can run the address/undefined-behavior tests with
`ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build-sanitize --output-on-failure`.
This disables leak detection only; it does not disable address checking.

## Command line

```text
qf [options] pattern [directory]
```

Quote patterns to prevent the shell from expanding them. The pattern is a
filename, not a pathname: use `qf '*.c' /tmp`, rather than `qf '/tmp/*.c'`.
Options can appear before or after the pattern. `--` ends option processing,
allowing filenames that start with `-`. Slash-prefixed DOS switches are not
accepted because `/` starts an absolute Linux pathname.

| Options | Behavior |
| --- | --- |
| `-a`, `--all-drives` | Start at `/` |
| `-c`, `--current` | Start at `.`; this is the default |
| `-b`, `--bare` | Omit banner, metadata, and totals; retain directory/archive headings |
| `-cf`, `--archives` | Search ordinary filenames and archive member filenames |
| `-co`, `--archives-only` | Search archive members only |
| `-e`, `--formats` | List supported archive formats and their limits |
| `-p`, `--pause` | Pause terminal output every 24 lines; ESC quits |
| `-t`, `--totals` | Print per-directory totals as well as overall totals |
| `-i`, `--ignore-case` | Ignore ASCII case during matching |
| `--dos-match` | Use the original DOS name/extension and star rules |
| `--cross-filesystems` | Descend into directories on other filesystem devices |
| `-h`, `--help` | Display help |

Original short options are case-insensitive. An explicit directory overrides
`-a` and `-c`. Without an explicit directory, the last of `-a`/`-c` wins.
Likewise, the last of `-cf`/`-co` wins.

`QF_OPT` supplies whitespace-separated default options. It is copied before
parsing, accepts tabs and newlines as separators, and is never evaluated by a
shell. Quoting, positional arguments, help, archive-format displays, and `--`
are not accepted there.
Command-line options are processed afterward.

## Matching and traversal

The default matcher is case-sensitive and supports `*` and `?`. A star matches
zero or more bytes, including in the middle of a pattern; a question mark
matches exactly one byte. Bracket expressions and shell escaping are not
implemented. UTF-8 names are preserved, but matching is byte-oriented and
`-i` does not perform Unicode case folding.

DOS matching is case-insensitive, compares name and extension separately,
defaults an omitted pattern extension to `*`, and makes `*` accept the remainder
of that name/extension component. Consequently, `*.*` also matches extensionless
names, and `a*b.txt` matches `abc.txt`. A question mark still requires a byte;
it cannot read past the end of a short name.

Directories whose names start with `.` are searched. Directory symlinks
encountered in the search are never followed; matching symlinks can still be
listed. The selected root itself must not be a symlink. Ancestor
device/inode checks prevent bind-mount cycles. By default, descent stays on the
root's filesystem device; use `--cross-filesystems` when needed. This device
check does not distinguish separate bind mounts on the same device.

Traversal uses an explicit stack and directory-relative operations, without
changing the process's current directory. Paths are dynamically allocated,
including display paths longer than 4096 bytes reached through directory file
descriptors. One directory descriptor is held per active level; exhaustion is
reported as an error rather than silently skipping the tree.

Listings put the full filename first, followed by a right-aligned size (or
`<DIR>`), the `bytes` unit, and the modification timestamp. Each directory's
columns fit its longest displayed filename and size, instead of reserving a
large fixed field. Names are never truncated.

Disk entries and archive headings start with three literal spaces. When archive
searching is enabled, archives appear as directory-like headings ending in `/`,
with their matching members indented another three spaces underneath and marked
with an asterisk before the filename. Each
archive's member columns are sized independently, so long stored paths do not
widen the surrounding disk listing. Bare mode keeps the same hierarchy and
omits metadata. Tabs are not used for padding.

Matching rows are buffered for one directory while column widths are measured.
Widths account for escaped characters and Unicode terminal-cell widths.

The display preserves filename spelling and escapes backslashes, ASCII control
characters, DEL, invalid UTF-8 bytes, and nonprinting characters so names cannot
inject newlines or terminal controls.
Sizes and totals are unsigned 64-bit quantities with checked addition.
Directories do not contribute bytes to totals. Matching archive members count
as files and contribute their uncompressed sizes; their archive heading does
not contribute to totals. If the archive itself also matches in `-cf` mode, its
disk-file size is counted separately.

## Archives

Use `qf -e` or `qf --formats` to list supported formats and their limits without
starting a search. The same list appears in `qf --help` and comes from the
archive dispatch table used by the readers.

Readers inspect metadata only. They neither decompress data nor recursively
search archives inside archives. Matching uses the member's basename, and the
listing retains stored member paths where available. Extension selection is
case-insensitive, as in the original program.

| Extensions | Supported metadata |
| --- | --- |
| `.ARC`, `.PAK` | Traditional ARC records, including the shorter version-1 header |
| `.ZIP` | Single-disk, non-ZIP64 central directories, including long names and archive comments |
| `.ZOO` | Old and extended archive headers; type-1/type-2 directory entries; deleted entries omitted |
| `.DWC` | Traditional trailer/directory format |
| `.LZH`, `.LZS` | Level-0 headers, including long names that fit that header |
| `.ARJ` | Single-volume ARJ; basic and extended header CRCs; DOS/Unix timestamps |
| `.CAB` | Single-cabinet Microsoft Cabinet file tables, including reserve areas |
| `.RAR` | RAR 1.5-4.x headers, including Unicode names and 64-bit size fields |
| `.TAR` | Uncompressed V7/USTAR, GNU long names, and PAX path/size/mtime overrides |

ZIP64, split archives, newer LZH header levels, RAR5, encrypted RAR headers,
ARJ dual-name headers, compressed TAR (`.tar.gz`, `.tar.xz`, etc.), and sparse
TAR entries are unsupported. Encryption of file data does not prevent listing
ARJ/RAR names when their headers remain readable. No passwords are requested.
CAB folder compression does not affect filename searching; folder data is
neither decompressed nor extracted. CAB names are limited to 65535 bytes, and
each GNU/PAX TAR metadata payload is limited to 16 MiB. PAX records support
path, size, and modification-time overrides; empty-value deletions for these
keys are not implemented. Other PAX metadata is ignored, except sparse-file
metadata, which is rejected explicitly.

Archive variants outside the supported layouts produce errors. ZIP end records are
searched within the format's bounded comment area; ordinary ZIP central
directories must end immediately before the end record. Self-extracting
wrappers with adjusted/relative offsets are not specially recognized.

The readers decode little-endian fields explicitly. Lengths, file offsets,
partial records, embedded NULs, and ZOO directory cycles are checked. LZH
level-0, ARJ basic/extended, RAR4, and TAR header checksums are verified.
Compressed-data CRCs and other archive checksums are not verified, so successful
listing is not an archive integrity test. Members displayed before a later bad record remain in the output and
totals, and the process returns an error.

ZIP tests use Python's `zipfile` writer, and TAR tests use `tarfile` in USTAR,
GNU, and PAX modes, plus generated V7 and malformed headers. Other formats are
checked with generated fixtures and malformed/mutated variants. Independent
ARJ samples from Apache Commons Compress and CAB/RAR4 samples from libarchive
were also checked manually. Generated CAB/RAR4 listings were compared with
`bsdtar`. These checks list metadata only; no archive contents are extracted.
Broader compatibility with historical archivers still benefits from real-world
archive samples.

Format references used to check the original layouts:

- [Zoo's canonical header and directory offsets](https://sources.debian.org/src/zoo/2.10-7/zoo.h/).
- [LHa's header documentation](https://github.com/jca02266/lha/blob/master/header.doc.md).
- [ARJ's header reader](https://github.com/joncampbell123/arj/blob/master/arjsfxjr.c).
- [Microsoft's Cabinet format specification](https://download.microsoft.com/download/4/D/A/4DA14F27-B4EF-4170-A6E6-5B1EF85B1BAA/%5BMS-CAB%5D.pdf).
- [RAR 4.0 technical notes](https://sources.debian.org/src/rar/2%3A4.0.b3-1/technote.txt/).
- [GNU tar format documentation](https://www.gnu.org/software/tar/manual/html_node/Standard.html).
- `COMPRESS.H` in this repository for the original ARC, DWC, ZIP, ZOO, and LZH layouts.

## Terminals and exit status

The Linux executable only searches and lists files. Deletion options `-d` and
`-n` (including `--delete` and `--no-prompt`) are not supported and are rejected
as invalid options, including in `QF_OPT`. The historical platform source still
contains its original deletion features.

Pagination is disabled when stdout is not a terminal. Ctrl-C and SIGTERM set a
flag; cleanup and terminal-mode restoration run outside the signal handler.
Shell redirection replaces the old printer/redirection option.

Exit status is 0 for a completed search, including no matches; 1 for search,
archive, or output errors; 2 for invalid arguments; and 128 plus the signal
number for interruption (130 for
Ctrl-C or pagination ESC, 143 for SIGTERM).

## Source layout

- `QFLINUX.C`: option processing, traversal, and matches.
- `QFUTIL.C`: matching, formatting, totals, and terminal input.
- `QFARCH.C`: archive dispatch and readers.
- `QFARCH.H`: private shared archive-reader context and helpers.
- `QFLEGACY.C`: ARJ, CAB, and RAR4 header readers.
- `QFTAR.C`: uncompressed TAR and extended-metadata reader.
- `QFLINUX.H`: native runtime structures and interfaces.
- `tests/test_quickfind.py`: filesystem, archive, and pseudo-terminal regressions.

The native modules retain the program's familiar functions and output, while
avoiding DOS drive emulation and segmented-pointer types in the Linux runtime.
