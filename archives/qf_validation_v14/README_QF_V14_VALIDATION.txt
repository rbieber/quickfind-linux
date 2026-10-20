QF structural validation set - v14

This ZIP is stored with no compression. It is only a shipping box.
Extract it first, then run QF inside the extracted directory.

Suggested tests:

    qf '*' --cf
    qf '*' --co
    qf '*' --cf --t
    qf '*.txt' --cf

Included working structural samples:

    good/basic_zip.zip
    good/basic_arc.arc
    good/basic_pak.pak      same structure as ARC; extension/factory mapping only
    good/basic_dwc.dwc
    good/basic_lzh.lzh
    good/basic_lzs.lzs
    good/basic_zoo.zoo
    edge/basic_lha_needs_factory.lha
    edge/nested_zip.zip
    edge/long_names_zip.zip
    edge/oldver1_arc.arc
    new_formats/basic_tar.tar
    new_formats/basic_cab.cab

Included bad samples for error reporting:

    bad/broken_zip.zip
    bad/broken_arc.arc
    bad/broken_zoo.zoo

Still missing reference samples:

    ARJ
    RAR4
    7Z

Implementation status in v14:

    TAR: implemented, metadata-only
    CAB: implemented, metadata-only
    ARJ: experimental parser present, needs real reference archives
    RAR: RAR4 experimental parser present, needs real reference archives
    7Z: recognized by factory, reports '7Z metadata parser not implemented yet'

Rule preserved:

    QF only reads archive directory/header metadata. It does not decompress member payloads.
