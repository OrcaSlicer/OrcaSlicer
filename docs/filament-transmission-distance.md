# Transmission distance

How deep light gets into a filament, in millimetres: the thickness at which it stops showing what is
printed underneath it. About 1 for an opaque filament, 4 to 7 for a usefully translucent one, 8 and
above for a nearly clear one. It is the number every translucent-layer colour tool is built on.

Nothing in slicing reads it. The option exists so that the value has somewhere to live.

## Why the values are a table and not preset defaults

Every published transmission distance is **per colour**. OrcaSlicer's filament presets are **per
product line**: `Panchroma PLA Matte @base` covers every colour Polymaker sells under that name, and
their own published values for that one line run from about 0.1 to about 4.1. There is no colour to
attach a number to, so a default on the preset would mean choosing a representative colour - an
editorial guess wearing the clothes of a measurement, and wrong for most spools of that line.

So the values ship as a table keyed by colour, one file per vendor under
`resources/filament_td/`, and
`filament_td_lookup()` consults it for a filament whose preset is still at the option's default. It
needs both halves to agree: at least two words in common with a line the table knows, and a colour
within 6 dE of one of that line's colours. That threshold is measured against the table rather than
chosen - inside a line the colours are well separated, so at 12 the worst pair it holds could be
confused (Cotton White at 4.1 against Pastel Candy at 2.9), while at 6 the worst remaining confusion
is 0.1 mm. No match is the right answer when in doubt: a white filament handed the number measured
for a grey one is worse than a default, because it is wrong and it looks measured.

A value somebody typed, or measured, always wins over the table.

The table is small - 125 colours across three brands - and it is a seed rather than coverage. No
source states the conditions either: not a layer height, not an instrument, not an illuminant. A
transmission distance without its method is approximate however good its provenance.

## Adding values

A file holds one vendor's colours, so two vendors never edit the same file and never collide.

```json
{
  "brand": "polymaker",
  "vendor": "Polymaker",
  "source": "where these numbers came from",
  "license": "the terms they came under",
  "filaments": [
    { "name": "Panchroma Matte PLA Cotton White", "type": "PLA", "color": "#f0ece4", "td": 4.1 }
  ]
}
```

`brand` is what the filament preset's `filament_vendor` reduces to, lower case and without spaces -
the lookup knows a few aliases, such as Prusa Polymers for Prusament. `name` is the line and the
colour together, because the line is matched by its words and the colour by its value. `td` is in
millimetres.

- **A vendor** adds `resources/filament_td/<Vendor>.json` in a pull request, and touches nothing
  else. Measured values for your own filament are the best thing this file can hold; say in `source`
  how they were measured.
- **Anyone** can put the same file in `filament_td/` inside OrcaSlicer's data directory. Those are
  read first, so a value you measured for your own spool wins over the one that shipped. Nothing is
  installed and nothing is overwritten by an update.

## Getting a value

- **Measure it.** Calibration -> Transmission distance prints a staircase of the filament over one it
  has to hide; the step where the one underneath stops showing through is the measurement. No second
  tool, no instrument.
- **A meter.** The BIQU AJAX TD-1S and the open-hardware Td-Free measure it directly.
- **A spool tag.** OpenPrintTag carries it as main field 27, `transmission_distance`, so a tagged
  spool can hand over its own number.

## Where published values exist

Recorded here so the question does not have to be asked again, and so that anyone filling in a
profile knows what is safe to copy.

| Source | Licence | Shape |
| --- | --- | --- |
| [OpenPrintTag material database](https://github.com/OpenPrintTag/openprinttag-database) | MIT, Copyright 2025 Prusa Research a.s. | ~125 per-colour values across Prusament, Polymaker and Printed Solid. The only openly licensed set whose numbers trace back to the brand that published them, and the one this ships. |
| [Prusa's HueForge TD and hex table](https://help.prusa3d.com/article/hueforge-filament-transparency-values-and-hexcodes_762314) | none stated | ~78 per-colour values for Prusament, measured by Prusa for its own filament. |
| [Polymaker's hex codes and transmission distances](https://wiki.polymaker.com/polymaker-products/more-about-our-products/hex-codes-and-transmission-distances) | none stated | ~400 per-colour values, the largest manufacturer table. |

A manufacturer publishing a measurement of its own product is the clearest case there is, but neither
of the last two grants anything in writing - ask before copying a table wholesale.

**Do not take values from HueForge's bundled database**, and do not take them from an aggregator that
cannot say where its numbers came from: an open licence on a collection does not clean the provenance
of what was collected into it. Some of the large openly licensed datasets say outright that they do
not record where each value came from, and ship importers for HueForge's own library.
