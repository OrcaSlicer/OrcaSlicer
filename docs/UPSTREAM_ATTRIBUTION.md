# Upstream attribution and licensing

Extreme Slicer is a modified, independently branded build based on the
OrcaSlicer source tree. This document keeps the project relationship explicit
without presenting upstream sponsors, donation accounts, or trademarks as
Extreme Slicer endorsements.

## Covered source

- [OrcaSlicer](https://github.com/OrcaSlicer/OrcaSlicer) and the inherited
  Slic3r/PrusaSlicer-derived code are distributed under the GNU Affero General
  Public License, version 3 (`AGPL-3.0`).
- The complete license text is retained in [`LICENSE.txt`](../LICENSE.txt).
- Extreme Slicer modifications include the CUDA/Vulkan compute dispatch,
  compute-mode preferences and diagnostics, QIDI tool mapping, and Extreme
  Slicer branding. These changes are part of this AGPL-3.0 source repository.

## Distribution obligations

When distributing a binary or installer based on this tree, keep the license
and copyright notices, identify it as a modified version, and provide the
corresponding source under AGPL-3.0. Any separate dependency or plugin keeps
its own license; check the dependency notices before redistributing a package.

The optional Bambu networking component may use non-free third-party libraries
provided by Bambu Lab. It is not a license grant for those libraries and must
be handled according to their respective terms.

This is project documentation, not legal advice. For a commercial distribution
or a substantial third-party component change, obtain a license review.
