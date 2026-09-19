from __future__ import annotations

from typing import Any

from fontTools.misc import sstruct
from fontTools.misc.textTools import bytesjoin, safeEval
from fontTools.misc.xmlWriter import XMLWriter
from fontTools.ttLib import TTFont, TTLibError
from fontTools.ttLib.tables import DefaultTable
from fontTools.ttLib.tables.DefaultTable import XMLAttrs, XMLContent
from fontTools.ttLib.tables.E_B_L_C_ import sbitLineMetricsFormat, SbitLineMetrics

ebscHeaderFormat = """
    > # big endian
    version:  16.16F
    numSizes: I
"""

bitmapScaleTableFormatPart2 = """
    > # big endian
    ppemX:           B
    ppemY:           B
    substitutePpemX: B
    substitutePpemY: B
"""


class table_E_B_S_C_(DefaultTable.DefaultTable):
    """
    Embedded Bitmap Scaling Table

    The ``EBSC`` table provides a mechanism for describing embedded bitmaps which are created by
    scaling other embedded bitmaps. While this is the sort of thing that outline font technologies
    were invented to avoid, there are cases (small sizes of Kanji, for example) where scaling a bitmap
    produces more legible glyphs than scan-converting an outline. For this reason, the EBSC table
    allows a font to define a bitmap strike as a scaled version of another strike.

    The ``EBSC`` table is used together with the ``EBDT`` table, which provides embedded monochrome or
    grayscale bitmap data, and the ``EBLC`` table, which provides embedded bitmap locators.

    See also https://learn.microsoft.com/en-us/typography/opentype/spec/ebsc
    """

    dependencies = ["EBDT", "EBLC"]

    version: float
    numSizes: int
    bitmapScaleTables: list[BitmapScaleTable]

    def decompile(self, data: bytes, ttFont: TTFont):
        _, data = sstruct.unpack2(ebscHeaderFormat, data, self)
        self.bitmapScaleTables = []
        for _ in range(self.numSizes):
            bitmapScaleTable = BitmapScaleTable()
            data = bitmapScaleTable.decompile(data)
            self.bitmapScaleTables.append(bitmapScaleTable)

    def compile(self, ttFont: TTFont) -> bytes:
        self.numSizes = len(self.bitmapScaleTables)
        dataList = [sstruct.pack(ebscHeaderFormat, self)]
        for bitmapScaleTable in self.bitmapScaleTables:
            dataList.append(bitmapScaleTable.compile())
        return bytesjoin(dataList)

    def toXML(self, writer: XMLWriter, ttFont: TTFont, **kwargs: Any):
        writer.simpletag(
            "header",
            [("version", self.version), ("numSizes", len(self.bitmapScaleTables))],
        )
        writer.newline()
        for index, bitmapScaleTable in enumerate(self.bitmapScaleTables):
            bitmapScaleTable.toXML(index, writer, ttFont)

    def fromXML(
        self, name: str, attrs: dict[str, str], content: XMLContent, ttFont: TTFont
    ):
        if not hasattr(self, "bitmapScaleTables"):
            self.bitmapScaleTables = []
        if name == "header":
            self.version = safeEval(attrs["version"])
            self.numSizes = safeEval(attrs["numSizes"])
        elif name == "bitmapScaleTable":
            bitmapScaleTable = BitmapScaleTable()
            bitmapScaleTable.fromXML(content, ttFont)
            self.bitmapScaleTables.append(bitmapScaleTable)


class BitmapScaleTable:
    hori: SbitLineMetrics
    vert: SbitLineMetrics
    ppemX: int
    ppemY: int
    substitutePpemX: int
    substitutePpemY: int

    def __init__(
        self,
        hori: SbitLineMetrics | None = None,
        vert: SbitLineMetrics | None = None,
        ppemX: int = 0,
        ppemY: int = 0,
        substitutePpemX: int = 0,
        substitutePpemY: int = 0,
    ):
        self.hori = hori if hori is not None else SbitLineMetrics()
        self.vert = vert if vert is not None else SbitLineMetrics()
        self.ppemX = ppemX
        self.ppemY = ppemY
        self.substitutePpemX = substitutePpemX
        self.substitutePpemY = substitutePpemY

    def decompile(self, data: bytes) -> bytes:
        dummy, data = sstruct.unpack2(sbitLineMetricsFormat, data, self.hori)
        dummy, data = sstruct.unpack2(sbitLineMetricsFormat, data, self.vert)
        dummy, data = sstruct.unpack2(bitmapScaleTableFormatPart2, data, self)
        return data

    def compile(self) -> bytes:
        dataList = [
            sstruct.pack(sbitLineMetricsFormat, self.hori),
            sstruct.pack(sbitLineMetricsFormat, self.vert),
            sstruct.pack(bitmapScaleTableFormatPart2, self),
        ]
        return bytesjoin(dataList)

    def toXML(self, index: int, writer: XMLWriter, ttFont: TTFont):
        writer.begintag("bitmapScaleTable", [("index", index)])
        writer.newline()
        self.hori.toXML("hori", writer, ttFont)
        self.vert.toXML("vert", writer, ttFont)
        for name in sstruct.getformat(bitmapScaleTableFormatPart2)[1]:
            writer.simpletag(name, value=getattr(self, name))
            writer.newline()
        writer.endtag("bitmapScaleTable")
        writer.newline()

    def fromXML(self, content: XMLContent, ttFont: TTFont):
        dataNames = set(sstruct.getformat(bitmapScaleTableFormatPart2)[1])
        for element in content:
            if not isinstance(element, tuple):
                continue
            name, attrs, content = element
            if name == "sbitLineMetrics":
                direction = attrs["direction"]
                if direction not in ("hori", "vert"):
                    raise TTLibError(
                        f"invalid sbitLineMetrics direction: {direction!r}"
                    )
                metricObj = SbitLineMetrics()
                metricObj.fromXML(name, attrs, content, ttFont)
                setattr(self, direction, metricObj)
            elif name in dataNames:
                setattr(self, name, safeEval(attrs["value"]))
