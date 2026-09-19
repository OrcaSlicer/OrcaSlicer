from fontTools.ttLib.ttFont import TTFont
from io import BytesIO
import struct
import logging
from contextlib import contextmanager
from fontTools.ttLib.sfnt import TTC_V1, TTC_V2, readTTCHeader, writeTTCHeader
from fontTools.ttLib.tables.D_S_I_G_ import table_D_S_I_G_
from fontTools.misc.timeTools import timestampNow

log = logging.getLogger(__name__)


@contextmanager
def _sharedModifiedTimestamp(fonts):
    """Pin a single ``head.modified`` timestamp across an iterable of TTFonts.

    When several fonts are saved together with ``recalcTimestamp=True``, each
    one restamps its own ``head`` table via ``timestampNow()``, whose
    granularity is one second. If the wall clock ticks between two fonts'
    ``head.compile()`` calls they get different ``modified`` values, which
    defeats byte-identical table sharing (``shareTables=True``) and makes the
    saved size non-deterministic.

    This reads ``timestampNow()`` once (so ``SOURCE_DATE_EPOCH`` is still
    honored) and applies it to every font that would have recalculated its own
    timestamp, suppressing the per-font restamp for the duration. Each font's
    ``recalcTimestamp`` flag is restored on exit, even if the body raises; the
    pinned ``modified`` value is left in place, matching a normal recalc save.
    Fonts with ``recalcTimestamp`` falsy, or without a ``head`` table, are left
    untouched (their ``head`` is not even loaded).

    See https://github.com/fonttools/fonttools/issues/4110.
    """
    now = timestampNow()
    restore = []
    try:
        for font in fonts:
            if font.recalcTimestamp and "head" in font:
                # record the original flag *before* mutating, so the finally
                # block restores it even if loading 'head' below raises
                restore.append((font, font.recalcTimestamp))
                font["head"].modified = now
                font.recalcTimestamp = False
        yield now
    finally:
        for font, recalcTimestamp in restore:
            font.recalcTimestamp = recalcTimestamp


class TTCollection(object):
    """Object representing a TrueType Collection / OpenType Collection.
    The main API is self.fonts being a list of TTFont instances.

    If shareTables is True, then different fonts in the collection
    might point to the same table object if the data for the table was
    the same in the font file.  Note, however, that this might result
    in suprises and incorrect behavior if the different fonts involved
    have different GlyphOrder.  Use only if you know what you are doing.
    """

    def __init__(self, file=None, shareTables=False, **kwargs):
        fonts = self.fonts = []
        if file is None:
            return

        assert "fontNumber" not in kwargs, kwargs

        closeStream = False
        if not hasattr(file, "read"):
            file = open(file, "rb")
            closeStream = True

        tableCache = {} if shareTables else None

        header = readTTCHeader(file)
        for i in range(header.numFonts):
            font = TTFont(file, fontNumber=i, _tableCache=tableCache, **kwargs)
            fonts.append(font)

        if header.Version == TTC_V2:
            self.dsig: table_D_S_I_G_ | None = None
            if header.ulDsigOffset != 0:
                self.dsig = table_D_S_I_G_("DSIG")
                file.seek(header.ulDsigOffset, 0)
                self.dsig.data = file.read(header.ulDsigLength)

        # don't close file if lazy=True, as the TTFont hold a reference to the original
        # file; the file will be closed once the TTFonts are closed in the
        # TTCollection.close(). We still want to close the file if lazy is None or
        # False, because in that case the TTFont no longer need the original file
        # and we want to avoid 'ResourceWarning: unclosed file'.
        if not kwargs.get("lazy") and closeStream:
            file.close()

    def __enter__(self):
        return self

    def __exit__(self, type, value, traceback):
        self.close()

    def close(self):
        for font in self.fonts:
            font.close()

    def save(self, file, shareTables=True):
        """Save the font to disk. Similarly to the constructor,
        the 'file' argument can be either a pathname or a writable
        file object.
        """
        if not hasattr(file, "write"):
            final = None
            file = open(file, "wb")
        else:
            # assume "file" is a writable file object
            # write to a temporary stream to allow saving to unseekable streams
            final = file
            file = BytesIO()

        tableCache = {} if shareTables else None

        # A V2 TTC will be saved if self.dsig is present, even if it is None
        version = TTC_V2 if hasattr(self, "dsig") else TTC_V1

        # Pin one 'modified' timestamp so the fonts' 'head' tables can be shared.
        with _sharedModifiedTimestamp(self.fonts):
            offsets_offset = writeTTCHeader(file, len(self.fonts), version=version)
            offsets = []
            for font in self.fonts:
                offsets.append(file.tell())
                font._save(file, tableCache=tableCache)
                file.seek(0, 2)

        file.seek(offsets_offset)
        file.write(struct.pack(">%dL" % len(self.fonts), *offsets))

        if version == TTC_V2 and self.dsig is not None:
            # Compile the DSIG if necessary
            if hasattr(self.dsig, "data"):
                data = self.dsig.data
            else:
                data = self.dsig.compile(None)
            # Write the DSIG tag, length, and offset
            # We are at the offset where the DSIG header starts
            dsig_header_fields_offset = file.tell()
            # The DSIG data will be written to the end of the file, go there
            file.seek(0, 2)
            dsig_offset = file.tell()
            # Write the actual data
            file.write(data)
            # Go back to the DSIG header
            file.seek(dsig_header_fields_offset)
            # Write the tag
            file.write(b"DSIG")
            # Write the length and offset
            file.write(struct.pack(">2L", len(data), dsig_offset))

        if final:
            final.write(file.getvalue())
        file.close()

    def saveXML(self, fileOrPath, newlinestr="\n", writeVersion=True, **kwargs):
        from fontTools.misc import xmlWriter

        writer = xmlWriter.XMLWriter(fileOrPath, newlinestr=newlinestr)

        if writeVersion:
            from fontTools import version

            version = ".".join(version.split(".")[:2])
            writer.begintag("ttCollection", ttLibVersion=version)
        else:
            writer.begintag("ttCollection")
        writer.newline()
        writer.newline()

        for font in self.fonts:
            font._saveXML(writer, writeVersion=False, **kwargs)
            writer.newline()

        writer.endtag("ttCollection")
        writer.newline()

        writer.close()

    def __getitem__(self, item):
        return self.fonts[item]

    def __setitem__(self, item, value):
        self.fonts[item] = value

    def __delitem__(self, item):
        return self.fonts[item]

    def __len__(self):
        return len(self.fonts)

    def __iter__(self):
        return iter(self.fonts)
