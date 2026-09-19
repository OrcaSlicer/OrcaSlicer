from __future__ import annotations

from typing import Optional

from fontTools.annotations import KerningPair, KerningDict, KerningGroups, IntFloat

StrDict = dict[str, str]


def lookupKerningValue(
    pair: KerningPair,
    kerning: KerningDict,
    groups: KerningGroups,
    fallback: IntFloat = 0,
    glyphToFirstGroup: Optional[StrDict] = None,
    glyphToSecondGroup: Optional[StrDict] = None,
) -> IntFloat:
    """Retrieve the kerning value (if any) between a pair of elements.

    The elements can be either individual glyphs (by name) or kerning
    groups (by name), or any combination of the two.

    Providing the two glyph-to-group mappings accelerates group-lookups.
    These can be built manually, or with :py:func:`glyphsToGroups`.

    Args:
      pair:
          A tuple, in logical order (first, second) with respect
          to the reading direction, to query the font for kerning
          information on. Each element in the tuple can be either
          a glyph name or a kerning group name.
      kerning:
          A dictionary of kerning pairs.
      groups:
          A dictionary mapping each group name to its glyph names.
      fallback:
          The fallback value to return if no kern is found between
          the elements in ``pair``. Defaults to 0.
      glyphToFirstGroup:
          A dictionary mapping glyph names to the first-glyph kerning
          groups to which they belong. Defaults to ``None``.
      glyphToSecondGroup:
          A dictionary mapping glyph names to the second-glyph kerning
          groups to which they belong. Defaults to ``None``.

    Returns:
      The kerning value between the element pair. If no kerning for
      the pair is found, the fallback value is returned.

    Note: This function expects the ``kerning`` argument to be a flat
    dictionary of kerning pairs, not the nested structure used in a
    kerning.plist file.

    Examples::

      >>> groups = {
      ...     "public.kern1.O" : ["O", "D", "Q"],
      ...     "public.kern2.E" : ["E", "F"]
      ... }
      >>> kerning = {
      ...     ("public.kern1.O", "public.kern2.E") : -100,
      ...     ("public.kern1.O", "F") : -200,
      ...     ("D", "F") : -300
      ... }
      >>> lookupKerningValue(("D", "F"), kerning, groups)
      -300
      >>> lookupKerningValue(("O", "F"), kerning, groups)
      -200
      >>> lookupKerningValue(("O", "E"), kerning, groups)
      -100
      >>> lookupKerningValue(("O", "O"), kerning, groups)
      0
      >>> lookupKerningValue(("E", "E"), kerning, groups)
      0
      >>> lookupKerningValue(("E", "O"), kerning, groups)
      0
      >>> lookupKerningValue(("X", "X"), kerning, groups)
      0
      >>> lookupKerningValue(("public.kern1.O", "public.kern2.E"),
      ...     kerning, groups)
      -100
      >>> lookupKerningValue(("public.kern1.O", "F"), kerning, groups)
      -200
      >>> lookupKerningValue(("O", "public.kern2.E"), kerning, groups)
      -100
      >>> lookupKerningValue(("public.kern1.X", "public.kern2.X"), kerning, groups)
      0
      >>> glyphToFirstGroup, glyphToSecondGroup = glyphsToGroups(groups)
      >>> lookupKerningValue(("O", "E"), kerning, groups, glyphToFirstGroup=glyphToFirstGroup, glyphToSecondGroup=glyphToSecondGroup)
      -100
    """
    # quickly check to see if the pair is in the kerning dictionary
    if pair in kerning:
        return kerning[pair]
    # ensure both or no glyph-to-group mappings are provided
    if (glyphToFirstGroup is None) != (glyphToSecondGroup is None):
        raise ValueError(
            "Must provide both 'glyphToFirstGroup' and 'glyphToSecondGroup', or neither."
        )
    # create glyph to group mapping
    if glyphToFirstGroup is None or glyphToSecondGroup is None:
        glyphToFirstGroup, glyphToSecondGroup = glyphsToGroups(groups)
    # get group names and make sure first and second are glyph names
    first, second = pair
    firstGroup = secondGroup = None
    if first.startswith("public.kern1."):
        firstGroup = first
        firstGlyph = None
    else:
        firstGroup = glyphToFirstGroup.get(first)
        firstGlyph = first
    if second.startswith("public.kern2."):
        secondGroup = second
        secondGlyph = None
    else:
        secondGroup = glyphToSecondGroup.get(second)
        secondGlyph = second
    # make an ordered list of pairs to look up
    pairs = [
        (a, b)
        for a in (firstGlyph, firstGroup)
        for b in (secondGlyph, secondGroup)
        if a is not None and b is not None
    ]
    # look up the pairs and return any matches
    for pair in pairs:
        if pair in kerning:
            return kerning[pair]
    # use the fallback value
    return fallback


def glyphsToGroups(groups: KerningGroups) -> tuple[StrDict, StrDict]:
    """Build dictionaries mapping glyph names to their kerning groups.

    This is a reverse-mapping of the UFO kerning groups data structure.
    Groups are assumed to be compliant with the UFO specification; with
    the current implementation, if glyphs appear in more than one group
    anyway, the last group wins.

    Args:
      groups:
          A dictionary mapping each group name to its glyph names.

    Returns:
      A tuple of two dictionaries, respectively mapping glyph names to
      the first- and second-glyph kerning groups to which they belong.

    Examples::

      >>> groups = {
      ...     "public.kern1.O" : ["O", "D", "Q"],
      ...     "public.kern2.E" : ["E", "F"]
      ... }
      >>> glyphsToGroups(groups)
      ({'O': 'public.kern1.O', 'D': 'public.kern1.O', 'Q': 'public.kern1.O'}, {'E': 'public.kern2.E', 'F': 'public.kern2.E'})
    """
    glyphToFirstGroup = {}
    glyphToSecondGroup = {}
    for group, groupMembers in groups.items():
        if group.startswith("public.kern1."):
            for glyph in groupMembers:
                glyphToFirstGroup[glyph] = group
        elif group.startswith("public.kern2."):
            for glyph in groupMembers:
                glyphToSecondGroup[glyph] = group
    return (glyphToFirstGroup, glyphToSecondGroup)


if __name__ == "__main__":
    import doctest

    doctest.testmod()
