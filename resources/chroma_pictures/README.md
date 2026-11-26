# Built-in ChromaStack pictures

The pictures the ChromaStack picker lists under "Built-in pictures". A ChromaStack print reproduces a
picture's colours by stacking thin layers of a few filaments, so what belongs here is a picture to
look at, not a height map: `resources/textures/displacement` is the separate library the texture
displacement gizmo reads, and neither picker offers the other's files.

## What a file here has to be

- `.png`, `.jpg`, `.jpeg`, `.bmp` or `.webp`, in colour. The solve reads RGB; a grayscale image works
  but only ever asks for one filament's tonal ramp.
- Named for what it is, in English, capitalised as a title: the file's stem is the name the picker
  shows, with no lookup table behind it (`Sunset over water.jpg`, not `sunset_01.jpg`).
- Around 1000 px on the long side. The solve runs per pixel and the relief is carried by mesh
  vertices, so anything much larger costs time and memory without reaching the print.
- Clear at a glance in a 92 px tile, and still readable in a dozen filament-blended colours: strong
  shapes and separated tones carry, fine texture and near-identical hues do not.
- Redistributable with OrcaSlicer, under a licence that allows it, with the source and licence
  recorded in the pull request that adds it.

## What a file here must not be

- A height map, a normal map or anything else meant to be read as displacement.
- A photograph of a person who did not agree to ship in a slicer.
- A file whose name a translator would have to guess at.

The user's own pictures are never stored here. They live in `<data_dir>/chroma/pictures`, which an
app update cannot overwrite; the picker lists them in their own section and only they can be removed
from it.

## What is here, and where it came from

Every file is public domain or CC0, so it can ship with the slicer and be reprinted by anyone who
installs it. The set is chosen to cover the subjects a filament stack handles well: flat graphic
work, strong tonal separation, and a few large shapes rather than fine texture. Pictures from paid
or closed tools, including HueForge's gallery and its art marketplace, cannot go here: they are the
artists' own work and their licence does not allow redistribution.

| Picture | Work | Licence | Source |
|---|---|---|---|
| Art nouveau poster | Alphonse Mucha, Job cigarettes, 1896 | Public domain | [Wikimedia Commons](https://commons.wikimedia.org/wiki/File:Alphonse_Mucha_-_Job_Cigarettes_1.jpg) |
| Earth from Apollo 17 | NASA, Apollo 17 crew, 1972 | Public domain | [Wikimedia Commons](https://commons.wikimedia.org/wiki/File:The_Earth_seen_from_Apollo_17.jpg) |
| Girl with a pearl earring | Johannes Vermeer, c. 1665 | Public domain | [Wikimedia Commons](https://commons.wikimedia.org/wiki/File:Meisje_met_de_parel.jpg) |
| Kingfisher and lotus | Ohara Koson, c. 1900-1930 | CC0 | [Wikimedia Commons](https://commons.wikimedia.org/wiki/File:IJsvogel_bij_lotusbloem,_RP-P-1999-435.jpg) |
| National park poster | Works Progress Administration, Zion National Park, 1938 | Public domain | [Wikimedia Commons](https://commons.wikimedia.org/wiki/File:Zion_National_Park,_Ranger_Naturalist_Service_LCCN2007676134.jpg) |
| Pillars of creation | NASA, ESA, CSA, STScI (JWST), 2022 | Public domain | [Wikimedia Commons](https://commons.wikimedia.org/wiki/File:Pillars_of_Creation_(NIRCam_Image).jpg) |
| Red Fuji | Katsushika Hokusai, c. 1830 | Public domain | [Wikimedia Commons](https://commons.wikimedia.org/wiki/File:South_Wind,_Clear_Dawn_LACMA_M.81.91.1.jpg) |
| Starry night over the Rhone | Vincent van Gogh, 1888 | Public domain | [Wikimedia Commons](https://commons.wikimedia.org/wiki/File:Vincent_van_Gogh_-_Starry_Night_-_Google_Art_Project.jpg) |
| Sudden shower on the bridge | Utagawa Hiroshige, 1857 | Public domain | [Wikimedia Commons](https://commons.wikimedia.org/wiki/File:Hiroshige,_Sudden_shower_over_Shin-%C5%8Chashi_bridge_and_Atake,_1857.jpg) |
| The great wave off Kanagawa | Katsushika Hokusai, c. 1831, this printing c. 1930 | Public domain | [Wikimedia Commons](https://commons.wikimedia.org/wiki/File:The_Great_Wave_off_Kanagawa.jpg) |

Each one was downsized to 1000 px on the long side, converted to sRGB and saved without metadata.
