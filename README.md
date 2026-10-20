Linux build
===========

This branch adds a native Linux version of Quick Find, preserving the original
Hungarian notation, three-space indentation, brace style, and archive-searching
command-line interface. The historical DOS, OS/2, and Windows source files remain
intact. The original DOS makefile is now `Makefile.dos`.

Build with CMake 3.20 or later and a C99 compiler. The tests require Python 3.9
or later; use `-DBUILD_TESTING=OFF` to build without Python.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure

./build/qf '*.c' ~/Projects
./build/qf -i -cf '*.txt' ~/Downloads
./build/qf --dos-match -co '*.DOC' ~/Archives
./build/qf --formats
./build/qf --help
```

Searches start in the current directory unless a directory is supplied. Quote
wildcards. Linux options start with `-` or `--`; absolute directory paths are
accepted without being mistaken for DOS switches. Matching preserves case by
default; `-i` ignores ASCII case, and `--dos-match` selects the old DOS matching
rules. Archive members are listed without extraction.

Supported extensions are ARC, PAK, ZIP, ZOO, DWC, LZH, LZS, ARJ, CAB, RAR
(RAR4 format), and TAR. TAR support covers uncompressed V7/USTAR, GNU long
names, and PAX metadata. Use `--formats` to see format-specific limits.

See [the Linux notes](DOC/LINUX.md) for behavior, archive limitations, development
checks, and the layout of the new source files.

Original project notes
======================

This is a program that I wrote about 37 years ago and was my first "Shareware" type of program.

The really cool thing about this program is that this was the one I learned C with - and subsequently
learned every other language that I've learned by taking pieces of this one and rewriting it because
I knew it so well.

I hate the bracing style I used in this.   This is probably the 14th reformat overall, and was when I
was in my "Petzold, Programming Windows 3.0" stage.   The bracing style is awful.   I should have 
stuck with K&R style bracing, which is what I do now.

I wound up donating this to the [FreeDOS](http://www.freedos.org) project in July of 2001.   Unfortunately, the only 
record I can find of this now is this [spanish version of the FreeDOS log](http://www.fdos.info/es/Jul2001.htm).

Aside from the bracing style - I hope you enjoy looking at this.   I found it while rummaging through the house
over the last week and thought I would publish it on Github - not because its useful, but because it is historical.

The time this was written was a _great_ time, when someone like me could just start publishing software on bulletin boards.

Trust me, at the time it was cool.

See the [DOCS](https://github.com/rbieber/quickfind/tree/master/DOC) directory for some other cool historical documentation, including a software review.

Cool thing is, I made $10 off of all the hours that I put into this.   Yes, $10.

UPDATE
======
This now builds with the [OpenWatcom C Compiler](http://www.openwatcom.org/index.php/Downloads) for DOS.   The default
makefile (`Makefile.dos` on this branch) will build the DOS executable.

Building this software also requires the [A86 assembler](http://www.eji.com/a86/) by Eric Isaacson.  Install the software 
in a directory that appears in your path.
 
After installing the above and the Watcom C compiler and setting up your environment,
type <CODE>wmake -f Makefile.dos</CODE>.





