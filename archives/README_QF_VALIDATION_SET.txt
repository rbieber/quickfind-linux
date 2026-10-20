QF structural archive validation set
====================================

Purpose
-------
This is a practical validation set for the Go/C++ QF ports.
These files are structurally valid enough for QF's metadata-only archive search.
They are not intended to validate decompression. QF should never touch payload data.

Rule being tested
-----------------
QF searches archive directory metadata only:
  - filenames
  - sizes
  - dates/times
  - container identity
  - wildcard matching

Compression method and payload correctness are intentionally irrelevant.

Good samples
------------
  good/basic_zip.zip   - normal stored ZIP; validates EOCD and central directory search.
  good/basic_arc.arc   - ARC structure from COMPRESS.H.
  good/basic_pak.pak   - same ARC structure, different extension.
  good/basic_dwc.dwc   - DWC directory/trailer layout.
  good/basic_lzh.lzh   - LZH/LHARC-style level-0 headers.
  good/basic_lzs.lzs   - LZS extension using the LZH reader.
  good/basic_zoo.zoo   - ZOO archive header and directory entries.

Edge samples
------------
  edge/nested_zip.zip              - ZIP entries with path-like names.
  edge/long_names_zip.zip          - ZIP entries with long modern filenames.
  edge/oldver1_arc.arc             - ARC header-version-1 case.
  edge/basic_lha_needs_factory.lha - Structurally same as LZH; useful if .LHA is added to the factory.

Bad samples
-----------
  bad/* are intentionally malformed. They are for future error-reporting tests.
  A good QF should report which container failed and keep going.

Outer ZIP packaging
-------------------
The outer ZIP should be created using STORE/no compression, equivalent to:
    zip -0 -r qf_structural_archive_corpus_store.zip qf_structural_archive_corpus
