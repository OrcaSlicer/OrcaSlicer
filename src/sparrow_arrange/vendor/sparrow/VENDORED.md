# Vendored sparrow

Upstream: https://github.com/JeroenGar/sparrow @ `50690c4eed08db111921ca0af5fa1845b8b9dcbf` (MIT, see LICENSE).

Kept: the separator, samplers, evaluators, collision tracker, listener, terminator and consts.
Removed: both binaries, the TUI, SVG/JSON/ctrl-c plumbing, the SIMD overlap proxy, and the
strip-packing driver (`optimize()`, exploration, compression, the LBF builder) with the config,
consts and listener variants that only served it -- plus the dependencies they needed
(clap, ratatui, crossterm, fern, jiff, svg, serde, serde_json, num_cpus, ctrlc, test-case,
anyhow, getrandom).

Local changes are marked `SPARROW_ARRANGE PATCH`: pinned obstacles in the collision tracker,
item eviction on the separator, and terminator polling inside the separation loop.
