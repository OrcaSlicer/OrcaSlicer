# Fin support: intent input

[Fin support overview](../fin-support.md)

`FinIntentInput` is the private, producer-independent input to correction and generation preparation in `FinSupport.cpp`. Painted Shape publishes this input through `publish_painted_fin_intents()`. Simple and Simulated still execute their existing routes; they do not currently publish it.

The input owns regions, source nodes, ordinary line evidence, resolved ordinary points and source paths. Correction reads it by const reference and publishes `FinCorrectionResult`; `prepare_fin_candidates()` then produces `FinPreparationInput`, and `resolve_fin_paths()` resolves finite heights, roots and stems into `FinCandidateProposal`. Physical realization has separate work and results described in [generation ownership](generation.md#painted-shape-data-ownership).

## Ordinary points

`FinPointIntent` describes a selected source anchor on a particular slice of the current `PrintObject`:

| Field | Meaning |
| --- | --- |
| `point`, `layer` | XY and slice identity of the contact-top source anchor; Z is that layer's `slice_z` |
| `direction` | Interpreted outward sheet direction |
| `surface_direction` | Source contour orientation, separate from the sheet direction |
| `macro_normal` | Source surface normal used by downstream preparation |

These are source anchors, not physical hits on printed extrusion walls. The point is not an unresolved feature center. Correction retains the published location and direction; preparation applies the common ordinary-point dimensions and expansion rule. It does not run point selection again or shift the point up by half a contact height. A region containing only such points needs neither fabricated nodes nor line-correction surface evidence.

The point's index in its published region is its identity for that input. Preparation carries this as `FinCandidate::intent_point` together with `intent_island`; a reclassified line retains its edge identity instead. A producer can correlate a submitted point with its own plan without using coordinates as identity. The input's correspondence does not define a retry or result-report protocol.

Painted Shape's raw blue point is a surface center. Its adapter resolves that center using the existing source-island contour selection, top-height tracking and model-demand direction calculation before publication. `FinPointReference` explicitly distinguishes `surface_center` from `contact_top` while resolving a source target; source-edge identity no longer selects the height interpretation.

An ordinary line can become a point only after correction. That operation resolves its derived highest-node target with the same `resolve_fin_point_intent()` algorithm and `contact_top` interpretation. It publishes a new resolved point or omits an unresolved target without modifying the original line. Producer publication and derived-point resolution share the algorithm, not mutable state or diagnostic output.

## Ordinary fitted lines and topology

`FinIntentLine` owns the source center/anchor, macro normal, interpreted XY direction, point-reclassification direction evidence, source width, root-start relation and node references. Its nodes are endpoint-first followed by interior junctions, matching the correction operation's contract. A zero XY direction records the existing projected-degenerate source case; it does not invent a direction or a new generation fallback.

The width is a source measurement used by the existing axis-correction displacement test. It is not fin thickness or contact width. Line producers must supply this evidence rather than substituting a mode-specific default. An ordinary line that becomes a point also needs the source cuts in its region for source-anchor resolution. `match_fin_point_surfaces()` uses the existing contour-authorization operation on demand; its temporary matching workspace is not published into the graph.

`FinIntentNode` owns XY, source Z and its optional attachment host. Node and host indices refer to their region's node and line arrays. Shared node identity expresses connectivity; geometric coincidence alone does not join nodes. The original host relation survives derived disconnection as provenance where required by sibling cavity filling. Graph junctions and whole generation members remain distinct: an interior junction does not split activation or failure ownership.

`bind_fin_correction()` creates stable mutable entities from these values. `correct_fin_lines()` reads source width and topology through this semantic input, with no access to painted charts, skeleton measurements or fitting queues. Split nodes retain their original source-node correspondence.

## Direct paths and crease contact

`FinSurfacePathIntent` carries an already interpreted ordinary-contact surface chain. Its finite source geometry bypasses fitted-line axis correction, while sharing height/root preparation and physical generation. This geometric category is separate from the name of the producer.

`FinCreaseIntent` retains body/source points, the independent immutable contact locus, source root normals, node membership and the established full-expansion interpretation. Its source identifiers provide correspondence for diagnostics. A generated stem or clipped body cannot replace the contact locus queried at `slice_z`. Point/path body classification and ordinary/crease contact remain independent.

Both direct path categories retain their existing source order, source-height rules and independent generation groups. The shared near-vertical predicate and the two distinct consumers remain as described in [Painted Shape correction](painted-shape.md#working-graph-and-specific-corrections).

## Operation context and reporting

All source slice references belong to the current object and slicing state. Model geometry, configuration and shape demand are supplied to operations; the input contains no generated wall hits, layer-body caches, contact bands, toolpaths or replanning state.

Point-resolution and candidate-preparation reports carry counts separately from published geometry. `log_painted_fin_preparation()` combines those reports with producer diagnostics; correction and candidate preparation do not read `PaintedSides` for logging or geometry.

`generate_candidate_fin_support()` takes an explicit `FinGeometryMode`. Its caller selects the physical pipeline, and candidates must agree with that selection. It does not infer the route from the configured producer style. The current Simple caller explicitly requests projected generation; the Painted Shape caller requests the intent-generation behavior. No other mode's selection or retry policy is changed by this interface.
