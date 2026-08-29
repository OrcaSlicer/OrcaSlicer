# Mate connectors: aligning with the mainstream CAD systems

Research date: 2026-08-05. Written against `orca_cad` / `snaporca` at the M8 state
(`CadDocument.{hpp,cpp}`, `apply_mate`, `datum_frame`, the `Mate` card in `DesignPanel.cpp`).

**Brief:** align with the mate-connector concept as the main CAD programs actually implement it,
and be simple, unequivocal, unconfusing. Alignment is the organising principle of this document:
every recommendation is labelled either **[INDUSTRY]** — do what they all do — or **[DEVIATION]** —
we would be departing, here is why and what it costs.

---

## 0. The answer in ten lines

1. Seven systems surveyed. **Five of the seven use the same model**; two are the old world.
2. The model: a joint is defined between **two local coordinate frames**, one rigidly attached to
   each part, plus **one type** naming which DOF stay free.
3. The frame is called a mate connector (Onshape), a **joint origin** (Fusion, Inventor), a joint
   connector (FreeCAD 1.0). Same object, three names.
4. **Every one of them expresses every DOF about the frame's Z axis.** One axis, one convention.
5. **Five types appear in every frame-based system with identical names and identical DOF**:
   Fastened/Rigid, Revolute, Slider, Cylindrical, Planar. Ball is in four of five.
6. That is not fashion — those are the classical **lower kinematic pairs**. The vocabulary converged
   because the mechanics converged.
7. Our kernel is already on the right side of the line: frame-based, five types, Z-relative,
   superimpose-then-relax. **The architecture needs no revisiting.**
8. Where we are out of step: connectors that are not attached to a body; an origin that can only be
   a face centroid; no live preview of the two Z arrows; a mate card of abstract dropdowns.
9. Where we would knowingly deviate: refusing a second mate per body (no vendor does this — it is
   forced on us by having no solver) and possibly inverting the default mate direction.
10. Biggest single win for the stated goal, and it costs no kernel work: **draw both frames and
    ghost the result before Confirm.** The convention stops needing to be remembered.

---

## 1. The two families

**Constraint-based ("old CAD").** The user states pairwise *geometric relations* between raw
topology — this face coincident with that face, this axis concentric with that axis, this plane
parallel at 12 mm. Each relation removes some DOF; a numerical solver satisfies all of them at once.
Fully positioning one part typically takes **three or more mates**, and the set can be
over-constrained, under-constrained, or satisfiable in several configurations.

**Frame-based ("mate connectors").** The user places a *local coordinate system* on each part and
states **one** relation between the two frames. The relation is not "these surfaces touch" but
"these frames coincide, except for the following DOF, which stay free."

Onshape's help page opens by drawing exactly this line:

> *"Mates in Onshape are different than mates in old CAD systems. Many assemblies require only one
> Onshape Mate between any two instances, as the movement (degrees of freedom) between those two
> instances is embedded in the Mate."*

The frame-based model won for three reasons, all of which matter here:

- **One mate per pair.** No mental arithmetic about which three constraints add up to a hinge.
- **The DOF are declared, not deduced.** A revolute mate *is* one rotation. You do not discover the
  remaining freedom by dragging.
- **It needs no simultaneous solver for the common case.** Frame-to-frame alignment is a matrix
  composition — precisely what `apply_mate` already does.

> **Caveat — several vendors ship both, and "align with X" is therefore ambiguous.** **Inventor**
> kept its legacy constraints *and* added frame-based Joints in 2012; many Inventor users still build
> assemblies entirely with the old constraint stack. **Creo** has placement constraints *and*
> Mechanism connections. **FreeCAD** had constraint-based Assembly2/3 add-ons before the frame-based
> Assembly workbench shipped in 1.0. So copying "what Inventor does" means copying **one of two
> coexisting workflows**. **Onshape and Fusion 360 are the only pure frame-based examples**, and they
> are the ones to weight most heavily when the evidence conflicts.

---

## 2. Field survey — seven systems

| | Onshape | Fusion 360 | Inventor | FreeCAD 1.0 | Creo | Siemens NX | SOLIDWORKS |
|---|---|---|---|---|---|---|---|
| **Family** | Frame | Frame | Frame (+ legacy constraints) | Frame (+ legacy add-ons) | Both | Constraint | Constraint |
| **Frame object** | Mate connector | Joint origin | Joint origin | Joint connector (`Placement1/2`) | CSYS on `Weld`/`6DOF` | — | — (nearest: **mate reference**) |
| **Where it lives** | Part Studio **and** Assembly; in the feature list | Component, inside the joint | Component / inside the joint | Inside the Joint object | Part | — | Part (up to 3 named entities) |
| **Origin placement** | Inferred family on hover; `Shift` locks | Discrete **snap points**; `Ctrl` cycles | Snap points + explicit origins | Inferred, previewed on hover | Picked CSYS | Picked entities | Picked entities |
| **Orientation control** | Primary axis (Z) + secondary axis; flip + 90° reorient | Flip, angle, offsets | Flip, angle, offsets | `Placement1/2` + `Offset1/2` | CSYS + offset | — | — |
| **Type inference** | No — explicit | No — explicit | **Yes — "Automatic"** from picked geometry | No | No | No | Partial (mate reference type) |
| **Solver** | Yes, simultaneous — *"order won't affect a Mate"* | Yes | Yes | Yes (Ondsel) | Yes | Yes | Yes |
| **Reuse across instances** | **Yes** — a Part Studio connector exists on every instance | Weak | Partial | Per-joint | Interfaces | Product Interface | Mate references auto-mate on insert |

Three observations that shape everything below.

- **Every frame-based system reduced the type list by an order of magnitude** relative to SOLIDWORKS
  (7–13 vs ~25) and lost nothing. That is not simplification-by-omission; it is what happens when the
  DOF live in the mate instead of being assembled from constraints.
- **Every one of them defines its types relative to a single axis.** Slider translates along Z,
  Revolute rotates about Z, Cylindrical does both, Planar translates in X/Y and rotates about Z.
  One axis carries the whole vocabulary.
- **Onshape alone treats the connector as a first-class, reusable, named object** — and that is also
  where its worst usability complaints come from (§4).

---

## 3. The type vocabulary — cross-system table

DOF = degrees of freedom left **free**, stated about/along the connector Z.

| DOF | Onshape | Fusion 360 | Inventor | FreeCAD 1.0 | Creo | **Ours today** |
|---|---|---|---|---|---|---|
| 0 | Fastened | Rigid | Rigid | Fixed | Rigid / Weld | **Fastened** ✅ |
| 1 — rot Z | Revolute | Revolute | Rotational | Revolute | Pin | **Revolute** ✅ |
| 1 — trans Z | Slider | Slider | Slider | Slider | Slider | **Slider** ✅ |
| 2 — rot + trans Z | Cylindrical | Cylindrical | Cylindrical | Cylindrical | Cylinder | **Cylindrical** ✅ |
| 3 — trans XY + rot Z | Planar | Planar | Planar | *(Parallel+Distance)* | Planar | **Planar** ✅ |
| 3 — rot XYZ | Ball | Ball | Ball | Ball | Ball | — |
| 2 — different axes | Pin slot | Pin-Slot | — | — | Slot / Bearing | — |
| 1 — coupled | Screw | — | — | Screw | — | — |
| 4 | Parallel | — | — | Parallel | — | — |
| other | Tangent, Width, Group | As-built | Automatic | Perpendicular, Angle, Distance, Gears, Belt, RackPinion | General, 6DOF | — |

**Five types appear in every frame-based system, with the same name and the same DOF.** Those five
are the industry's common denominator, and they are exactly `mate_kind` 0–4 as already implemented.
Ball is in four of five. Everything past that is a long tail no two vendors agree on.

### Why the convergence is a fact, not a fashion

A rigid-body placement is an element of SE(3). A mate leaves some set of relative motions free. For
the mate to behave the same throughout its range — for a hinge to be a hinge at every angle — that
free set must be **closed under composition**: two allowed motions must compose to an allowed motion.
A closed set of motions is a **subgroup** of SE(3).

The subgroups corresponding to physical surface-on-surface contact are the classical **six lower
pairs** (Reuleaux):

| Pair | Free motion relative to Z | DOF |
|---|---|---|
| Revolute (R) | rotation about Z | 1 |
| Prismatic / slider (P) | translation along Z | 1 |
| Helical / screw (H) | coupled rotation + translation | 1 |
| Cylindrical (C) | rotation about **and** translation along Z | 2 |
| Planar (E/G) | translation in X,Y + rotation about Z | 3 |
| Spherical / ball (S) | rotation about X, Y, Z | 3 |

Plus the two trivial ends: identity (0 DOF — **fastened**) and all of SE(3) (6 DOF — floating, i.e.
no mate). Hervé's Lie-subgroup analysis of the displacement group is the standard reference for
treating these as the algebraic building blocks of mechanism synthesis.

**Consequence.** Anything outside this table is either (a) a *composition* needing a solver, or
(b) not a joint at all but a *measurement*:

- Onshape's **Parallel** (4 DOF), **Tangent**, **Width**, **Pin slot**, and FreeCAD's **Distance /
  Angle / Perpendicular** are constraints, not pairs — their free set is not a subgroup, so they only
  make sense alongside a simultaneous solver.
- **Gear, Belt, Rack-and-pinion** are *relations between two mates*, a different object entirely.
- **Screw (H)** is a legitimate lower pair but needs a pitch parameter and is rare in printed parts.

So the vendors' shared five, the lower pairs, and our `mate_kind` 0–4 are the same list arrived at
three ways. **[INDUSTRY] Stop looking for missing types and spend the budget on the connector.**

---

## 4. What they all agree on — adopt verbatim

Deviating from any of these makes an experienced user's intuition *wrong*, which is the operational
definition of "confusing".

**A1 [INDUSTRY] — The connector is a full right-handed frame.**
Origin + Z (primary) + X (secondary). Onshape and Fusion expose exactly these two axis controls and
nothing else. A point cannot express spin; an axis cannot express clocking.
*Status: we comply* — `DatumCoordSys` carries origin/x/y and derives Z.

**A2 [INDUSTRY] — Z is the joint axis; every DOF is about or along Z.**
Revolute rotates about Z. Slider translates along Z. Planar's free plane is normal to Z. Offsets run
along Z. This single rule is what makes the system learnable: **one axis to look at, and its meaning
never changes.**
*Status: we comply* — `mate_offset` along A's z, `mate_angle` about A's z.

**A3 [INDUSTRY] — Mating superimposes the two frames; the type then relaxes specific DOF.**
FreeCAD states it most plainly: *"the second connector is superimposed on the first connector by
default and may change its position according to the joint type."* Fastened is not a special case —
it is the base case with nothing relaxed.
*Status: we comply* — `T = M_A · Rz · Tz · F · M_B⁻¹`, looser kinds relaxing from there.

**A4 [INDUSTRY] — The connector belongs to a part and moves with it.**
Onshape: a connector defined in a Part Studio *"is available for reuse on every instance of that part
in every assembly in which it is instanced."* It is part geometry, not assembly geometry.
*Status: **violated**.* `CoordSysType::PointWorld` is a bare world XYZ with `X = world X` and no
`coordsys_body`. Such a connector does not follow its part. See §6 G1.

**A5 [INDUSTRY] — Selection order is meaningful and must be visible.**
One connector is the reference; the other is driven onto it. Onshape spells out that offsets are
measured *"from the second Mate connector selected to the first"*, and that reversing the order
flips the sign.
*Status: complied with in the data model* (`mate_cs_a` fixed, `mate_cs_b` moves) *but not in the UI* —
two dropdowns labelled A and B do not tell the user which part is about to jump.

**A6 [INDUSTRY] — Flip and re-clock live in the mate dialog, always.**
Onshape: *"Click the arrow icon to flip the direction of the primary axis. Click the Reorient
secondary axis icon to rotate the secondary axis in 90-degree increments."*
*Status: partial.* We have `mate_flip` (Z reversal). We have `mate_angle` as a free number — strictly
more powerful than 90° steps, and much worse to *use*: the common case is "it came in a quarter turn
out", and typing 90 is a worse gesture than pressing a button.

**A7 [INDUSTRY] — DOF are shown, not inferred by the user.**
Onshape animates each mate's remaining DOF on demand; Fusion and Inventor name the DOF in the type
list. Our dropdown text already does this in words ("free spin + axial slide"). Keep it.

**A8 [INDUSTRY] — Free DOF are preserved from the current placement, not zeroed.**
Onshape: a Planar mate aligns the frames *"but they are not restricted to this location with respect
to their degrees of freedom."*
*Status: we comply* — and it must be *said*, because a Planar mate that leaves the part where it was
looks like a mate that did nothing.

---

## 5. Where they diverge — who to copy, and why

### D1 — Where the connector's origin comes from

| | Behaviour |
|---|---|
| **Fusion 360** | Discrete **snap points** only: vertex, edge midpoint, face centre, arc centre. `Ctrl` cycles the candidates under the cursor. A circle icon denotes a vertex, a triangle a midpoint. "Between two faces" is a separate explicit option. |
| **Onshape** | Infers a *family* on hover — centroid, every vertex, every edge midpoint, every arc centre, the centroids of interior regions (holes, slots), and the virtual sharps of conical faces. `Shift` locks the current candidate. |
| **Inventor** | Snap points, plus explicit joint origins for awkward cases. |
| **FreeCAD 1.0** | Hovering previews where the connector will land before you commit. |
| **Ours** | Always the **face centroid**. No alternative exists. |

Onshape's richness has a cost its own documentation admits: *"The suggested locations are based on
the underlying geometry of the part and changing the geometry will change the location of the Mate.
This can be undesirable in certain situations."* On the forum this shows up as connectors that move
or break on edit — the classic topological-naming failure. Fusion's discrete set is poorer and far
more predictable.

> **[INDUSTRY] Copy Fusion's candidate *set*.** A small, closed, enumerable set — **face centroid,
> vertex, edge midpoint, arc/circle centre** — each drawn before commit, with the card naming which is
> in use ("Origin: edge midpoint"). This is our largest expressiveness gap: a face centroid alone
> cannot place a hinge pin on a corner boss. It is also the one place where copying the *simpler*
> vendor is clearly right.
>
> **Open sub-choice — how the candidate is chosen.** Three options, in increasing order of magic:
> (1) **explicit dropdown** in the card after picking the face — no hover behaviour at all;
> (2) **Fusion's `Ctrl` cycling** through candidates under the cursor; (3) **Onshape's hover
> inference**. Kimi's independent review argued for (1) on the grounds that hover is exactly where
> both vendors' instability complaints originate, and that a dropdown gets ~90% of the expressiveness
> with none of the hover-guess debugging. That is a fair reading and (1) is the cheapest to build and
> the easiest to make unequivocal. **Recommendation: build (1) first; if hover is added later, let it
> *pre-fill the dropdown* rather than silently create an implicit connector** — which also keeps R2
> (one kind of connector) intact.

### D2 — Explicit type, or inferred from the geometry?

Inventor is the only surveyed system that infers: *"Rotational is selected if the two selected
origins are circular. Cylindrical if the two selected origins are points on a cylinder. Ball if
points on a sphere. Rigid for all other origin selections."* Onshape and Fusion require an explicit
choice.

> **[INDUSTRY, Inventor] Do both, in Inventor's order.** Infer a *default* type from what was picked,
> then show it in an editable control. Inference is what makes the tool feel like it understands the
> geometry; the visible, editable result is what keeps it unequivocal. Pure inference with no visible
> type is the confusing option; a pure dropdown with no default is the tedious one. This also fits
> the Design tab's geometry-first charter exactly: point at a bore, get Revolute offered.

### D3 — How the Z-direction ambiguity is resolved

This is the specific failure the brief is aimed at. A former IT trainer stated it precisely on the
Onshape forum:

> *"There is always the risk that users will build their own conceptual models of how software works
> which may not match the designer's concept. The result is usually a poor user experience and many
> mistakes… for a good (say) Fixed mate to occur do the Z axes of the two mates have to be pointing
> in the same direction… Alternatively, should they be facing each other?"*

He is asking the right question and **no vendor's documentation answers it.** Onshape's own advice —
*"if the behavior is not what you expected, try flipping the primary and/or secondary axis"* — is
trial and error. This is a gap in the industry, not a convention to copy.

> **[INDUSTRY, method] Resolve it with live preview, not documentation.** FreeCAD previews the
> connector on hover; Onshape and Fusion both draw the frames. Draw **both** Z arrows the moment the
> second connector is picked, and ghost the resulting placement *before* Confirm. The convention then
> never has to be remembered because it is on screen.
>
> **[DEVIATION, optional] Name the two cases in the user's words** rather than in axis-speak:
> "the two faces come together" vs "the axes run the same way". No surveyed vendor does this — they
> all ship a flip arrow. It is a small, low-risk improvement on the state of the art, and it is
> separable from the default-direction question in §8 D1.

### D4 — Named, reusable connectors on the part

Onshape: connectors created in the Part Studio are reused on every instance in every assembly.
SOLIDWORKS' **mate reference** reaches the same end by another route: up to three named entities
(primary/secondary/tertiary) baked into the part so it auto-mates on drag-and-drop — and a *named*
mate reference seeks out a matching name on insertion. That naming trick is how a library of
fasteners assembles itself.

> **[INDUSTRY] Out of scope now, but do not preclude it.** Give connectors a stable, user-visible
> name at creation. One string today; expensive to add once documents exist in the wild.

---

## 6. Confusion catalogue

Documented ways real implementations confuse people. Each is a requirement in disguise.

**C1 — Which way does Z point?** See D3. If a user has to ask once, they will mis-predict a hundred
times.

**C2 — The roll is unspecified.** Aligning Z leaves one rotation about Z undetermined. Something must
pin it, and if that something is world-derived, the frame does not rotate with its part. **This
codebase shipped exactly this bug** (`snaporca-en4`): a face-only connector took Z from the face
normal but X from `coordsys_x_hint`, a world constant, so Fastened and Slider claimed to lock an
orientation the frame could not see. Fixed 2026-07-26 by deriving X from the face's own first usable
edge — but note the fix's own caveat: *"replaying an older document whose face-only connector fed a
mate can now place that body differently."* Roll conventions are load-bearing, and changing one is a
document-format change.

**C3 — The origin drifts.** See D1.

**C4 — Implicit and explicit connectors are not the same thing.** On the Onshape forum, implicit
connectors are reported to change their query structure when a feature is edited and re-accepted, and
are unusable in places explicit ones work. Two things called by one name that behave differently is a
permanent tax.

**C5 — Which part moves?** A frame alignment is asymmetric. If the UI does not say which frame is
driven, the user finds out by watching the wrong part jump.

**C6 — Which direction is a positive offset?** Onshape measures *"from the second Mate connector
selected to the first"* — the sign depends on pick order, and swapping the picks flips it. Documented
behaviour, documented surprise.

**C7 — One intent, several mates.** The SOLIDWORKS failure: expressing "this shaft is in this hole,
resting on this shoulder" as three constraints, then discovering the solver picked the mirror
configuration. Frame-based systems fix this by construction; the requirement is not to reintroduce it.

**C8 — Degenerate frames.** A circular face has no usable in-plane edge direction; a cylinder seam
projects to nothing; a picked edge parallel to Z gives a zero cross product. `datum_frame` handles all
three with fallbacks — the requirement is that a fallback be *visible*, because a silent fallback is
C2 wearing a different hat.

**C9 — Order dependence without a solver.** Onshape can say *"Onshape solves Mates simultaneously so
order won't affect a Mate."* A system that composes transforms in tree order cannot say that. Two
mates driving one body means the second wins and the first is a lie on screen.

**C10 — Mirrors and patterns.** A mirrored instance has a left-handed frame. Blindly mirroring a
connector gives a frame whose Z still points "out" but whose handedness flipped, so every rotation
runs backwards. Cheap to handle now, miserable to retrofit.

---

## 7. Requirements

Labelled **[INDUSTRY]** (what the frame-based systems do) or **[DEVIATION]** (we would depart).

### Definition

**R1 [INDUSTRY] — A mate connector is a frame attached to exactly one body.** No body, no connector.
*Test:* creating a connector without a body is rejected at creation, not at mate time.
→ **`CoordSysType::PointWorld` violates this.** It is a datum wearing a connector's name.

**R2 [INDUSTRY] — One kind of connector, not two.** No "implicit" connector that behaves differently
from an explicit one. If hover inference is offered, hovering *creates* an ordinary connector.
*Why:* C4. *Test:* everything that accepts a connector accepts any connector.

**R3 [INDUSTRY] — A mate names exactly one subgroup of free motion.** Fastened (0), Revolute (1),
Slider (1), Cylindrical (2), Planar (3), optionally Ball (3). *Why:* §3. *Test:* every type's free
set is closed; no type is "A and also B".

### Orientation

**R4 [INDUSTRY] — Everything is about Z. Say so once, in the UI.** *Test:* no mate parameter refers
to any other axis.

**R5 [DEVIATION] — Z is the outward material direction, and mates default to FACING.**
A mate would drive B's Z onto **−A's Z** by default, so picking two faces that should touch makes
them touch with no options changed. *Why:* it is the whole of C1.
**Cost and caveat:** this inverts today's default (`mate_flip=false` currently *aligns*), and I could
not establish from any vendor's documentation what their default actually is — the forum question in
D3 went unanswered precisely because it is undocumented. So this is marked a deviation on the honest
grounds that **I cannot prove the industry agrees with it.** If D3's live preview lands first, the
default matters much less, because the user sees the outcome before committing. See §9 D1.

**R6 [DEVIATION] — Name the two directions; do not ship a boolean called "flip".**
`Direction: Facing | Aligned`. Every surveyed vendor ships a flip arrow instead. A boolean requires
remembering what unticked means; two named values do not. Low risk, small improvement on the state of
the art.

**R7 [INDUSTRY] — Roll is picked, or a stored quarter turn. Never world-derived.**
X from a referenced edge or in-plane direction; failing that, a deterministic body-attached seed, with
**Rotate 90°** offered as a stored integer 0–3 on top (this is Onshape's "reorient secondary axis",
A6). *Why:* C2 and the world-constant bug this project already shipped. *Test:* rotate the parent
body by any angle; the connector's X rotates with it — *this test already exists* ("a face-only frame
rotates with its body").

**R8 [INDUSTRY] — A degenerate roll is reported, not absorbed.** *Test:* a connector on a full
cylindrical face reports "roll undefined — pick a direction" rather than silently taking a fallback.

### Placement

**R9 [INDUSTRY, Fusion] — Origin comes from a small closed set of named candidates.**
**Face centroid, arc/circle centre, edge midpoint, vertex.** Four. Each stored as
`(kind, topological reference)` and resolved at rebuild. *Why:* D1. *Test:* the stored kind is visible
in the card; a rebuild either resolves it or raises an error.

**R10 [INDUSTRY] — An unresolvable reference is an error, never a silent relocation.**
*Test:* delete the referenced face; the mate reports "connector A: face not found" and the body stays
where it was.

### Semantics without a solver

**R11 [DEVIATION] — A body is driven by at most one mate. The second is refused.**
**No surveyed system does this** — they all have solvers and all accept many mates per body. It is
forced on us by tree-order composition: a second mate on the same body silently overrides the first
and the screen shows a configuration satisfying only one stated intent (C9). *Test:* creating a
second mate whose moving body already has one is rejected, naming the existing mate.
This is the single largest departure in this document. See §9 D4.

> **A tempting misreading, checked and rejected.** It is easy to find the claim that Onshape mandates
> *"exactly one Mate between any two instances"*, which would make R11 an industry agreement rather
> than a deviation. **The Onshape page does not say that.** It says *"**Many assemblies require only**
> one Onshape Mate between any two instances"* and then lists, as an explicit remedy, *"**Use more
> than one Mate if necessary.**"* One mate per pair is Onshape's *typical case*, not its rule. R11
> remains a deviation and must be justified on our own architecture, not on theirs.

**R11a [DEVIATION] — The refusal list.** With no solver, these are unsupportable and must be refused
rather than half-done: a second mate on an already-driven body; cycles (A→B, B→A); closed loops
(A→B, A→C, B→C); relations *between* mates (gear, belt, rack-and-pinion, screw coupling); **joint
limits**, which nothing can enforce without a solver; and **dragging a body to exercise a free DOF**,
which requires keeping the body on the allowed manifold. Motion analysis and animation follow from the
same lack. *Requirement:* none of these may appear in the UI as something that half-works.

**R12 [DEVIATION] — The mate graph is an acyclic forest rooted at fixed bodies.** A body reached by
no mate is fixed; cycles are refused. Same root cause as R11. *Test:* A→B, B→A rejected at creation.

**R13 [INDUSTRY] — Free DOF are preserved from the current placement, and the user is told.**
Behaviour already matches Onshape (A8); the telling does not. *Test:* the card for any type with
DOF > 0 says which motions remain and that dragging exercises them.

**R14 [INDUSTRY] — State what mirroring does to a connector.**
*Checked in the code:* `datum_frame` ends with a Gram-Schmidt forcing a right-handed frame
(`ds.x = Y.cross(Z)`), so a connector resolved on a mirrored body comes out **right-handed, not
mirror-imaged**. Z follows the mirrored face's outward normal, X follows a mirrored edge, handedness
is re-imposed. Defensible — a mate on the mirrored part still turns the way its type says — but it
means a mirrored sub-assembly is *not* the mirror image of the original in its rotation sense.
*Requirement:* document it and pin it with a test. *Why:* C10.

### Feedback — the part that actually removes confusion

**R15 [INDUSTRY] — Before Confirm, the card answers four questions in words.** Which body moves;
which way Z points on each connector; how many DOF remain; what the offset is measured from.

**R16 [INDUSTRY] — Draw both frames live, with Z distinguishable, and ghost the result.**
Two triads with Z rendered differently from X/Y (length, arrowhead, colour). *Why:* D3 — the fastest
way to make a convention unequivocal is to show it. *Test:* both Z directions are readable in a
screenshot.

**R17 [INDUSTRY] — Show the DOF budget per body.** "Body 2: 1 of 6 DOF free (rotation about Z)."
The most educational readout in any assembly system, and free to compute here — the type *is* the DOF
count. *Test:* the number changes when the type changes.

**R18 [DEVIATION] — Refuse loudly and name the alternative.** Where something is out of scope (a
second mate, a tangency, a gear ratio), say what is unsupported and what to do instead. Vendors do not
need this because their solvers accept the input. *Test:* no refusal message ends without a suggested
next action.

---

## 8. Minimal specification, and gap analysis

### The connector

```
MateConnector
    body            int          required, ≥ 0                                   (R1)
    origin_kind     enum         FaceCentroid | ArcCentre | EdgeMidpoint | Vertex (R9)
    origin_ref      topo ref     face / edge / vertex index on that body
    z_source        implied by origin_kind: face normal, arc axis, edge tangent
    roll_ref        topo ref     optional in-plane edge; else deterministic seed  (R7)
    roll_quarters   int 0..3     stored quarter turns on top of the seed          (R7, A6)
    flip_z          bool         reverse Z at the connector
    name            string       stable, user-visible                             (D4)
```

`flip_z` is a property of the **connector**, chosen once when it is made — not a per-mate
afterthought. Keeping connector-flip and mate-direction separate is what stops the "which flip do I
tick?" question.

### The mate

```
Mate
    kind        enum      Fastened | Revolute | Slider | Cylindrical | Planar [| Ball]  (R3)
    fixed       connector A — its body does not move
    moving      connector B — its body is driven                                        (A5, C5)
    direction   enum      Facing | Aligned                                              (R5, R6)
    offset      mm along A's Z, measured A → B — state this in the label                (C6)
    angle       deg about A's Z                                                         (R4)
```

Within one field of what exists.

### Gaps against today

Source of record: `CadDocument.hpp:26,247-252,298-310`; `CadDocument.cpp:1669` (`datum_frame`),
`:2961` (`apply_mate`), `:1302` (`add_mate`); `DesignPanel.cpp:2671-2709` (the Mate card).

| # | Gap | Severity | Ref |
|---|---|---|---|
| G1 | `PointWorld` connectors are not attached to a body and their X is a world constant | **High — data model** | A4/R1 |
| G2 | Origin is always the face centroid; no vertex / edge-midpoint / arc-centre snap | **High — expressiveness** | D1/R9 |
| G3 | No live preview of the two Z arrows or of the resulting placement | **High — this is the brief** | D3/R16 |
| G4 | Mate card is two abstract dropdowns; nothing says which body moves | High — charter + A5 | R15 |
| G5 | No joint-type inference from the picked geometry | Medium — feel | D2 |
| G6 | `add_mate` validates nothing — no one-mate-per-body, no cycle check | Medium | R11/R12 |
| G7 | No `Ball` type | Low | §3 |
| G8 | Re-clocking needs a typed angle; no 90° step control | Low, cheap | A6/R7 |
| G9 | Degenerate roll falls back silently | Low | C8/R8 |
| G10 | Connectors have no stable user-facing name | Low now, expensive later | D4 |

**Already aligned — do not "fix" these:** the five types and their DOF; the frame definition (A1);
Z as the joint axis (A2); superimpose-then-relax (A3); the fixed/moving asymmetry in the data model
(A5); DOF wording in the type list (A7); free-DOF preservation (A8); right-handed frames under mirror
(R14); and `snaporca-en4`'s fix, which put roll derivation on the body where it belongs (C2).

**The pattern worth naming: the kernel is in good shape and the concept is under-explained.** Half the
requirements here are wording and drawing, not geometry. The two real engineering items are R9 (origin
candidates) and R11/R12 (the mate-graph rules).

### Expensive-to-retrofit decisions — get these right in the data model now

Changing any of these after documents exist in the wild costs a migration, not an edit.

1. **Topological reference stability.** Storing raw face/edge indices is brittle — editing a body
   renumbers faces. Either persistent topology IDs, or store the named origin *kind* plus a
   deterministic search that re-finds the same geometric intent on rebuild. The latter is cheaper and
   probably sufficient here; it is also what makes R10's "error, never silent relocation" enforceable.
2. **Connector ownership** (R1). Remove `PointWorld` or bind it to a body. Do this first.
3. **Mate direction semantics** (R5/D1). Inverting the default rewrites the meaning of every saved
   mate.
4. **Roll representation** (R7). "First usable edge" is better than world-X but still fragile. Store
   an explicit roll reference plus quarter turns.
5. **Coordinate convention** — Z = joint axis, X = roll reference. Changing this after release
   invalidates every mate.
6. **Units** — offset in mm, angle in degrees. Never change.
7. **Mirror handedness** (R14) — document the decision, do not let it stay an accident.
8. **Flat body index vs. a component tree.** Mates currently reference bodies in a flat vector. If
   **sub-assemblies** are ever in scope, mates must reference nodes in a tree instead. Retrofitting
   this is painful and it is the one item on this list not already implied elsewhere in the document —
   **decide now whether nested assemblies are in scope.**
9. **Serialization field semantics.** Adding fields is easy; redefining `mate_flip` or
   `coordsys_x_hint` is not.
10. **The one-mate-per-body rule** (R11). Enforce at creation. Relaxing it later by adding a solver is
    straightforward; allowing many mates now and discovering later that they silently conflict is not.

---

## 8b. The visual shape of the connector — polarity and verse

Researched separately (2026-08-05) by downloading and **looking at** the vendors' own figures, not
by reading their prose. Files kept alongside this document in `doc/design/mate-connectors/`.

### What the systems actually draw

**Onshape** — verified from `planarfacemateconnectors.png`, `cylindricalmateconnectors.png`,
`linearedgemateconnectors.png`, `mateconnector-planarpoints.png`, `matepointiconLG.png`:

> **A small circle with one quadrant filled, plus three short coloured axis arms (X red, Y green,
> Z blue).**

Three parts, each doing one job:

| Element | What it says |
|---|---|
| The **circle** | "I am a frame, and this is my XY plane." |
| The **filled quadrant** | **The roll.** The shaded sector is the +X/+Y quadrant. |
| The **coloured arms** | The three axis directions, Z distinguished by colour. |

The quadrant is the cleverest part of the whole design and it is easy to miss. The figure
`matepointreorientsecondaryaxis.png` shows three connectors side by side with the quadrant in three
different rotations — **it is the live readout of "reorient secondary axis in 90° increments" (A6).**
One glyph element makes the otherwise-invisible clocking visible, and makes the 90° button's effect
legible before you commit. The toolbar icon `matepointiconLG.png` is that same circle-with-a-quadrant,
so the symbol is consistent from toolbar to viewport.

Candidate snap points, before you choose one, are drawn as **plain small white dots** on the model
(clear in `mateconnector-planarpoints.png`: dots at every corner and edge midpoint). Candidate and
committed are deliberately different weights — dots propose, the circle-and-triad commits.

**FreeCAD 1.0** — verbatim from the wiki: *"Connectors are local coordinate systems and are marked by
a symbol with three axes (X, Y, Z) and a circle representing the XY-plane."* Same core as Onshape —
circle plus triad — **without** the quadrant.

**Fusion 360** — the joint origin glyph, plus a documented icon language for *candidates*: *"A circle
denotes a vertex, and a triangle denotes a midpoint."* Shape encodes what kind of point it is.

**Convergent core:** *circle for the XY plane + coloured triad*. Onshape alone adds the roll quadrant.

### What none of them draw — and it is exactly what was asked for

**Nothing in any vendor's glyph says which connector is the reference and which one is about to
move.** Both ends of a mate are drawn identically. That is confusion C5 ("which part moves?") left
unsolved in the visual language, and it is why the honest recommendation earlier was a live ghost —
the ghost compensates for a glyph that does not carry the information.

So the two things asked for split cleanly, and only one of them is solved upstream:

- **Verse** (*verso* — which way it points): **solved**. Z has a colour and a direction.
- **Polarity** (which end receives, which end inserts; who is anchored, who travels): **unsolved
  everywhere.** This is open ground, and getting it right is a genuine improvement rather than a
  deviation to justify.

### Our starting point

**We draw nothing.** `resolve_datum_coordsys()` (`CadDocument.cpp:1749`) has exactly one consumer in
the entire tree — `McpControl.cpp:1310`, the agent socket. A mate connector is today visible only to
a program. The glyph is unbuilt, so there is no migration cost to designing it properly now.

### Proposed glyph: the magnet

Adopt Onshape's proven core, then add the missing polarity with a metaphor that carries its own
instructions.

```
            ▲  solid cone on +Z ONLY          ← verse
            |
        ────●────                              ← the disc = XY plane, ● = exact origin
        ▨ quadrant filled                      ← roll / clocking, steps 90°
```

**Rule 1 — verse: draw +Z and never −Z.** A single stem with a cone head, on the positive side only.
No stem below the disc. A double-headed axis is the one thing that guarantees the question gets asked;
an arrow that exists on one side only cannot be misread. Length is asymmetric on purpose.

**Rule 2 — roll: keep Onshape's quadrant.** Filled sector = the +X/+Y quadrant. It rotates in 90°
steps with the reorient control (A6/R7). This is aligned *and* it is the only in-glyph answer to
"where is X?", which matters because Fastened and Slider lock the clocking.

**Rule 3 — polarity: solid cone travels, open collar receives.**
- The **driven** connector (B, on the body that will move) draws a **solid filled cone** — the plug.
- The **fixed** connector (A) draws an **open ring / hollow cone outline** — the socket.

Same silhouette, so they read as a matched pair; opposite fill, so which one is about to jump is
answerable at a glance and without a legend. Plug-into-socket is the one mechanical metaphor every
user of this tool already has in their hands.

**Rule 4 — the pair reads as a magnet.** Draw a dashed line joining the two origins the moment both
are picked. Two poles, one field line. And because a magnet's north seeks a south, **"facing" becomes
the self-evident default** — which quietly settles open decision D1 (§9) on visual grounds rather than
on a convention nobody can look up. If the glyph looks like a magnet, nobody has to be told that two
faces which touch have opposed normals.

**Rule 5 — three states, three weights.**

| State | Drawing |
|---|---|
| **Candidate** (hover) | small dot only — Onshape's white dots; shape may encode kind, Fusion-style |
| **Picked** | full glyph: disc + quadrant + cone |
| **Degenerate roll** (C8/R8) | the quadrant is drawn **hollow/hatched** — "roll undefined, pick a direction" |

That last row is worth the trouble: it turns R8 from a message nobody reads into a mark you cannot
miss, and it costs one branch in the renderer.

**Rule 6 — do not reuse the existing triad.** The bed-centre world triad
(`DesignCanvas.cpp:65`, `set_axes_at_bed_center`) and the move gizmo are already three-coloured arrows.
The connector must not be a fourth set of RGB arrows or the viewport becomes unreadable. The disc and
the quadrant are what distinguish it; keep the arms short, and consider drawing only Z on the
committed glyph, with X/Y implied by the quadrant.

### Built and judged in the viewport, not in a mock

The browser mock that first accompanied this section was the wrong instrument and its proportions
were meaningless: **every gizmo in this codebase is sized in SCREEN PIXELS** via `upp = 1/zoom`
(`render_shell_gizmo` uses `15.0 * upp`, `render_hole_gizmo` `9.0 * upp` for its cube). A connector
is a symbol, not a part — it must not shrink with the model. Nothing about that is visible in SVG.

The glyph was therefore implemented and driven on the rig. Screenshots: `g-0*.png`, left in the workspace `artifacts/shots/` and not moved into the repo.
Five findings, none of which a mock could have produced:

**F1 — Three axis arms lose to one.** Rendered side by side (`SNAPORCA_GLYPH=A` vs default), the
Onshape-style RGB trio crowds a 22 px disc: the arrowheads are as large as the disc, they bury the
gold quadrant, and at an oblique angle the three heads pile into a coloured smudge. Worse, **it is
indistinguishable from the move gizmo and the bed triad**, which are already RGB arrow trios in this
viewport. One-sided Z wins on evidence, not taste. (`g-01-zoom.png` vs `g-02-zoom.png`.)

**F2 — Polarity works, and colour does more of the work than fill.** A filled blue head against an
open grey outline head is readable instantly at 22 px (`g-03-zoom.png`). But the fill difference is
the *second* cue; the colour split carries it. Keep both — fill survives greyscale and colour-blind
palettes, colour survives small size.

**F3 — Depth off floats, depth on tears.** With `GL_DEPTH_TEST` off, connectors on faces pointing
*away* from the camera still drew their discs over the solid, so the part looked covered in frames
that were really on its back. Turning depth on fixed that and immediately caused **z-fighting**: the
disc is exactly coplanar with its face, and came out as a broken dotted arc. The fix is depth **on**
plus a sub-pixel lift along Z (`0.7 * upp`), scaled by `upp` so it never becomes a visible gap on
zoom-in. Both failure modes are in the images (`g-03` torn, `g-04` clean).

**F4 — The quadrant is the first thing to die at a grazing angle.** On a face seen nearly edge-on the
disc foreshortens to a sliver and the fan collapses into a blob (`g-01-zoom.png`, lower-right glyph).
The roll is exactly the information that is hardest to read when you most need it. Not yet solved —
see the open item below.

**F5 — Roll-undefined in red is too loud.** It works, but it makes the *least* important connector
the most eye-catching thing on screen. Amber, or the same grey with a hatched quadrant, is enough.

Also surfaced while testing, and unrelated to the glyph: `add_mate` accepted a mate between two
connectors **on the same body**, which is meaningless, and duly transformed the body relative to
itself. Concrete instance of gap G6.

**Still untested:** a true grazing view (the view-cube click missed), a connector on a curved face,
and behaviour when a connector overlaps the move gizmo. F4 is the open design question — the disc may
need to billboard its *quadrant* while keeping the disc in-plane, which is a compromise no surveyed
vendor makes and which should be tried before being adopted.

### What this costs

A renderer for `resolve_datum_coordsys()` — which does not exist and has to be written whatever glyph
is chosen — plus one dashed line and three fill states. No kernel work. It is the same piece of work
as G3 (live preview), and doing them together is what makes the mate card honest.

---

## 8c. The "faceted ridge dome" proposal — built, rendered, judged

A colleague proposed replacing the flat disc with an **asymmetric low-poly solid**: a faceted
prismatic wedge with a dominant longitudinal ridge that **slopes** from a tall steep back to a long
shallow front, plus a male protrusion / female pocket pair with a 0.2 mm clearance.

It was built rather than discussed. `faceted_ridge_key.scad` (this folder) (6 vertices, 7 faces),
verified as a closed manifold, exported through OpenSCAD, and flat-shaded from five directions with
`render_key.py` / `render_stl.py`. Sheets: `rk-sheet.png`, `cmp-sheet.png`.

### The verdict: the shape is right, the male/female polarity cue is not

**It solves F4, decisively.** The grazing view — where the flat disc dies, its quadrant collapsing to
a blob — is the view where this shape is *most* legible: the tall back and long shallow front are
unmistakable in silhouette. At a grazing angle the silhouette IS the information, and this solid's
silhouette is maximally informative there. That is a real, evidence-backed win over what is currently
in the code.

**Down the mating axis (+Z) it also reads well**, which matters because that is the natural viewing
direction when you are looking at a face you intend to mate.

**One degenerate view, and it is not the one I predicted.** I expected the ±X views (along the ridge)
to be silhouette-ambiguous, resolved only by shading. Wrong: front and back are clearly *different* —
the front shows several facets, the back is a **single flat featureless triangle**. So they are not
confusable, but the view from directly behind the tall end tells you nothing about roll or slope.
A second blind spot remains untested: from below the base, where the protrusion is hidden behind its
own face.

**The female half fails, and much harder than expected.** Rendered with flat shading and no outlines —
the honest test, since a viewport draws no black edges — a recessed pocket is *invisible*: iso and
grazing show a plain block with a hairline; straight down the axis shows a **completely blank
rectangle**. The interior faces are lit almost identically to the top face and are occluded by the rim
from most angles. As a polarity cue, male/female therefore works in exactly one direction and returns
nothing in the other.

> **Conclusion: do not overload shape with all three jobs.** Let the solid carry **verse and roll**,
> where it is excellent, and carry **polarity on a second channel** — colour plus the filled/open head
> that already tested well at 22 px (F2). Drawing the fixed connector as an outline/wireframe of the
> same solid is the variant worth trying; drawing it as a pocket is not.

### Two premises in the brief are wrong

**"Avoid curved surfaces to optimise rendering computations / rapid mesh processing."** Not a reason
for a viewport glyph. There are 2–20 connectors on screen, the renderer pushes `GLModel` triangles
directly, and it performs no CSG or mesh processing at all. **The real argument for flat facets is
legibility**: hard normals give distinct value steps between adjacent facets, and the renders confirm
that is exactly what makes the shape readable from an arbitrary angle. Keep the constraint, fix the
justification. (For a *printed* part the original justification is sound for a different reason: flat
facets slice without the stair-stepping a tessellated curve produces.)

**"0.2 mm clearance for smooth mechanical mating."** Meaningless for a glyph. A symbol mates with
nothing, and every gizmo here is sized in screen pixels via `upp`, so a millimetre tolerance has no
referent. This is the strongest signal that **the brief was written for a physical printed part**,
not for a viewport symbol — as are "scannable" and "mechanical mating". See the open question below.

### Two defects the build caught that discussion would not have

1. **The flank quads are not planar.** Written as `[0,3,5,4]` and `[1,4,5,2]` the base edge and the
   ridge edge are skew, so the four corners do not share a plane — my own first draft asserted the
   opposite in a comment. Left as quads, the tessellator picks the fold direction, the "flat facet"
   promise is broken by an unspecified crease, and two exporters can disagree about the shape. Fixed
   by triangulating explicitly (7 faces, Euler 6 − 11 + 7 = 2).
2. **The pocket punched through its own plate.** A 4.5 mm key against a 3 mm demo plate gives a
   through-hole, not a pocket. Minimum stock = height + clearance + pocket depth + a wall.

Also worth recording: the first female render was misleading because the debug renderer outlined
*every* triangle, so a flat top face triangulated by CGAL looked like a faceted dome. The instrument
lied before the geometry did. Conclusions were only drawn after outlines were removed.

### Second opinion, and the one disagreement worth resolving

Kimi reviewed the proposal independently and **rejected it for the viewport**. It agreed on the two
wrong premises, agreed the female pocket is unreadable, and added the useful framing that a
screen-constant symbol and a model-constant part feature are two different design spaces that cannot
be served by one geometry. It also noted correctly that there is **no single scalar** that removes
ambiguity from every view: you need one asymmetry in the base plane (for top-down roll) and one out
of plane (the ridge slope, for front/back). Our base is scalene, so it has both.

Its central objection was numeric and testable: *"at 22 px with 6–8 facets each facet is 3–7 px wide,
that is at the aliasing limit … minimum useful size is roughly 32–48 px, which is not compatible with
a 22 px screen-constant symbol."* My own renders were ~300 px, so the claim was unaddressed by my
evidence and would have killed the concept if true.

**Rendered at 22, 32 and 48 px (`size-test.png`), it is false for this shape.** At 22 px all three
views still read: the grazing view shows the tall back and shallow front unmistakably, and the
down-axis view keeps a strong dark/light split. The reason Kimi's arithmetic does not apply is that
this solid presents only **four or five large facets with high value contrast**, not eight small ones —
the silhouette does most of the work, and silhouettes survive downsampling far better than facet
detail does.

*Honest limit on that result:* the test renderer has no anti-aliasing, no perspective, one directional
light, and no background. Readable at 22 px against white is not the same as readable at 22 px on top
of a shaded gold part next to the move gizmo. That case still needs the rig.

**Where I do not follow Kimi:** its recommendation is to **billboard** the existing flat glyph so it
never turns edge-on. That kills F4 by construction, but a billboarded frame cannot show the frame's
orientation *in place* — which is the entire reason the disc is a disc and not a dot — and it is what
no surveyed CAD system does; Onshape, Fusion and FreeCAD all draw the frame in the geometry. Worth
prototyping as an option, not worth adopting on argument.

### Open question for Tommaso

**Is this a viewport glyph or a printable alignment feature?** The vertex logic is identical either
way; only the units and the clearance change, and the `.scad` file states both readings. But the
answer decides whether `clr`/`depth` are real millimetres or meaningless, and whether the geometry
scales with the model or stays screen-constant. The brief's own language points at "physical", the
conversation it arrived in points at "glyph".

---

## 9. Decisions for you

**D1 — Invert the default direction to Facing?** [DEVIATION, R5]
It changes the meaning of every stored document containing a mate. Options: (a) invert and migrate,
writing `direction=Aligned` where `mate_flip` was false; (b) invert only for new mates and store
`direction` explicitly from now on. (b) is safer and costs one field. Note this project has taken one
such semantic hit knowingly before — the `snaporca-en4` fix — and the golden fixture survived, so the
migration path is a known quantity. **If G3 (live preview) lands first, this matters much less.**

**D2 — How far to take origin candidates?** [R9]
Four kinds is the Fusion-aligned recommendation. Two (face centroid + arc centre) would cover "sit on
a face" and "go down a hole" — most printed-part assembly — at a third of the work. Where do you want
to stop?

**D3 — Ball mate: in or out?**
In four of five frame-based systems, so including it is the aligned choice. Out is defensible for
printable mechanical parts. Cheap either way — align origins, leave orientation free. Kimi's review
argued **out**: a true ball joint is hard to print and hard to use without a roll reference, and a
Fastened connector at the ball centre approximates it.

**D3a — Should Planar be dropped?** [dissent worth recording]
Kimi's independent review recommended **removing Planar** and shipping four types, on the grounds that
"slide on a flat surface" is rarely how printed mechanisms work — you usually want a rail or a hinge —
and that Planar is the type most likely to confuse a user who expected "put this flat on that" and got
a part free to slide. It further ranked the honest minimum as **three**: Fastened, Revolute, Slider,
with Cylindrical useful and decomposable.
**I do not agree, and the reason is alignment.** Planar appears in every frame-based system surveyed,
it is a genuine lower pair, it is already implemented and tested, and removing it is a document-format
change made in exchange for nothing. The confusion Kimi names is real but it is a *feedback* problem —
it is exactly what R17 (show the DOF budget) and R13 (say that free DOF are preserved) exist to fix.
Recorded here because it is a legitimate reading of the same evidence and the call is yours.

**D4 — Is refusing a second mate per body acceptable?** [DEVIATION, R11 — the big one]
It is the honest consequence of having no solver, and it is what makes the tool predictable. But **no
mainstream system behaves this way**, so it is the point where an experienced user's intuition will
break. It means a part cannot be constrained by two independent relationships — "in this hole *and*
resting on this shoulder" must be expressed by placing one connector correctly rather than by two
mates. If that trade is unacceptable, the answer is a solver, and the scope of this document changes
entirely.

There is a strong argument that the trade is not merely acceptable but *correct for this product*:
the Design tab lives inside a slicer, and most of its users are positioning parts for printing rather
than building working mechanisms. For layout-and-export, tree-order composition is genuinely enough,
and adding a solver to look like Onshape would buy complexity nobody asked for. The rule to publish is
then simple and defensible: **one mate per moving body, acyclic, no relations between mates** — with
R18's loud refusals carrying the honesty.

---

## Sources

**Onshape** — [Mate Connector](https://cad.onshape.com/help/Content/PartStudio/mate_connector.htm) ·
[Mates](https://cad.onshape.com/help/Content/Assembly/mates.htm) ·
[Fastened](https://cad.onshape.com/help/Content/Assembly/fastened_mate.htm) ·
[Revolute](https://cad.onshape.com/help/Content/Assembly/revolute_mate.htm) ·
[Slider](https://cad.onshape.com/help/Content/Assembly/slider_mate.htm) ·
[Cylindrical](https://cad.onshape.com/help/Content/Assembly/cylindrical_mate.htm) ·
[Planar](https://cad.onshape.com/help/Content/Assembly/planar_mate.htm) ·
[Ball](https://cad.onshape.com/help/Content/Assembly/ball_mate.htm) ·
[Parallel](https://cad.onshape.com/help/Content/Assembly/parallel_mate.htm) ·
[Tangent](https://cad.onshape.com/help/Content/Assembly/tangent_mate.htm) ·
[Pin Slot](https://cad.onshape.com/help/Content/Assembly/pin_slot_mate.htm) ·
[5 things you can do with mate connectors in Part Studios](https://www.onshape.com/en/resource-center/tech-tips/tech-tip-5-things-you-can-do-with-mate-connectors-in-onshape-part-studios)

**Onshape forum** — [The concept behind Mates Z Axes](https://forum.onshape.com/discussion/22828/the-concept-behind-mates-z-axes) (C1/D3) ·
[Implicit mate connectors act differently than explicit ones](https://forum.onshape.com/discussion/15736/implicit-mate-connectors-act-differently-than-explicit-ones) (C4) ·
[Efficiently set mate connectors](https://forum.onshape.com/discussion/13133/efficiently-set-mate-connectors)

**Fusion 360** — [Joint types](https://help.autodesk.com/cloudhelp/ENU/Fusion-Assemble/files/GUID-8818AE31-958A-4A59-989B-9875A174C67A.htm) ·
[Joint origins](https://help.autodesk.com/view/fusion360/ENU/?guid=ASM-JOINT-ORIGIN) ·
[Joints vs. Mates in Fusion](https://www.autodesk.com/products/fusion-360/blog/joints-mates-moving-fusion/) ·
[Joint tips — snap points and Ctrl cycling](https://mgfx.co.za/blog/engineering-manufacturing-design/fusion-360-joint-tips/)

**Inventor** — [Create Joints Reference](https://help.autodesk.com/cloudhelp/2026/ENU/Inventor-Help/files/GUID-6AA68E8F-7C97-4806-8483-3941DE915E70.htm) ·
[Use Joint to define and manage relationships](https://knowledge.autodesk.com/support/inventor-products/learn-explore/caas/CloudHelp/cloudhelp/2014/ENU/Inventor/files/GUID-21DC3336-5C51-42C1-90FB-4299CD66E0C6-htm.html) (type inference, D2)

**FreeCAD 1.0** — [Assembly Workbench](https://wiki.freecad.org/Assembly_Workbench) ·
[Fixed Joint properties](https://wiki.freecad.org/Assembly_CreateJointFixed)

**Creo** — [About Predefined Constraint Sets](https://support.ptc.com/help/creo/creo_pma/r12/usascii/assembly/asm/About_Predefined_Constraint_Sets.html)

**Siemens NX** — [Assembly constraints](https://learnnx.com/lesson/siemens-nx-assemblies-assembly-constraints/)

**SOLIDWORKS** — [Mate References](https://help.solidworks.com/2025/English/SolidWorks/sldworks/c_Mate_References_Overview_SWassy.htm) ·
[Creating and using mate references](https://blogs.solidworks.com/tech/2019/07/creating-and-using-mate-references.html)

**Theory** — [Hervé, The Lie group of rigid body displacements, a fundamental tool for mechanism design](https://www.sciencedirect.com/science/article/abs/pii/S0094114X98000512) ·
[Joint kinematics — the six lower pairs and their DOF](https://erc-bpgc.github.io/handbook/mechanical/Joint%20Kinematics/) ·
[ISO 10303-105 — Kinematics (STEP integrated resource)](https://www.iso.org/standard/78589.html)

**Internal** — `snaporca-en4` (closed 2026-07-26, fixes C2 here) · `CadDocument.cpp:1669`
`datum_frame` · `CadDocument.cpp:2961` `apply_mate` · `CadDocument.cpp:1302` `add_mate`

**Second opinion** — an independent review by Kimi Code (2026-08-05) contributed the
vendors-ship-both caveat (§1), the explicit-dropdown option for origin choice (D1), the expanded
refusal list (R11a), the retrofit list (§8), and the dissents recorded at D3/D3a. One of its claims —
that Onshape mandates *"exactly one Mate between any two instances"* — **was checked against the
source and is wrong**; the correction is recorded at R11 because it is a misreading that would
otherwise turn our largest deviation into a false agreement.
