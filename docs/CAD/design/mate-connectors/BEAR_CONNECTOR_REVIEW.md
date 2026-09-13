# BearConnector.step — examination

> **Scope.** One file was supplied and it contains **one object: the male.** Everything below is
> measured from that single solid. Earlier drafts of this note reasoned about a female pocket and a
> mating pair — those objects were never supplied, so any statement about them was speculation and
> has been removed. The clearance, the fit, and the pocket's legibility are all **unassessed**.

Measured, not eyeballed. Imported into the Design tab's own OpenCascade kernel
(`import_step` → one valid closed solid), topology queried, geometry checked numerically.
Flat drawing: `artifacts/shots/bear-flat.png`. Viewport: `artifacts/shots/bear-02-zoom.png`.

**File:** AP242 Edition 2, ST-Developer. 1 `MANIFOLD_SOLID_BREP`, 1 `CLOSED_SHELL`.
**Size:** 83.06 × 66.69 × 17.27 mm. **Faces:** 30 — 24 planar + 6 cylindrical.
**Curves:** 69 lines + 12 circles. **No** splines, spheres, tori or cones.
**Relief:** only four Z levels — 0, 3.00, 10.66, 17.27.

---

## What is right, and precisely so

**The sloping ridge is implemented exactly as briefed.** From (0.00, 18.40, 17.27) to
(0.00, 46.72, 10.66): 28.3 mm long, 6.61 mm drop, **13.1° slope**, and both ends sit dead on
x = 0.00. It breaks 180° rotation on its own.

**20.0° uniform draft on all four snout flanks**, identical to within 0.1°:
`(0,−0.94,0.342) (0.936,0.08,0.342) (0,0.94,0.342) (−0.936,0.08,0.342)`. That is a real,
deliberate lead-in — it self-centres into a matching pocket, and it demoulds and prints.

**The eyes are exactly symmetric**: Ø9.87 at x = ±16.43, y = 48.01, matching to 0.01 mm.
Someone mirrored those on purpose.

**The mating feature is extremely economical**: only **five edges** exist above the 3 mm plate —
the ridge plus two flank edges at each end. Base plate is exactly 3.00 mm.

The low-poly constraint is honoured. All six cylinders are outline rounds and eye holes; none of
them is a mating surface.

---

## The asymmetry is deliberate, and it is complete

**Correction.** A first pass read the left/right differences as an unfinished mirror. That was wrong:
the asymmetry is intentional. Tested properly — every candidate self-symmetry, in the part's own
centred frame, with a generous 0.1 mm tolerance:

| operation | edges mapped onto the part |
|---|---|
| identity | 81 / 81 — 100 % |
| mirror about x = 0 (left/right) | **0 / 81** |
| mirror about y = 0 (top/bottom) | **0 / 81** |
| rotate 180° about Z | **0 / 81** |
| rotate 90° about Z | **0 / 81** |
| mirror about the diagonal | **0 / 81** |

**The symmetry group is trivial.** No rigid motion or reflection maps this part onto itself, so
**every partial view determines the orientation uniquely** — you never need to see the whole face to
know which way round it goes. That is the strongest possible result for a keying interface and it is
exactly what the earlier abstract glyph work kept failing to achieve: a symmetric shape seen at a
grazing angle, or half-occluded, gives an ambiguous read.

### Does it let you GRASP the orientation? Measured, not asserted.

Unique-in-principle and graspable-at-a-glance are different claims. The symmetry table proves the
first. For the second, the front-on picture (outline + eyes + mouth, filled) was rasterised and
compared against its own mirror and its own 180° rotation — the two ways a person can get it wrong.

**By size** (percentage of pixels that differ):

| width | vs mirror | vs rotated 180° |
|---|---|---|
| 16 px | 20.7 % | 26.0 % |
| 24 px | 21.9 % | 30.9 % |
| 32 px | 23.0 % | 28.1 % |
| 48 px | 22.4 % | 30.6 % |
| 80 px | 24.7 % | 31.0 % |
| 160 px | 23.6 % | 31.0 % |

**The curve is flat.** The full signal is already there at 16 pixels and more resolution adds
nothing. That is the whole result: **the orientation cue lives at low spatial frequency**, carried by
the overall shape rather than by any detail. It therefore survives distance, blur, poor light,
peripheral vision, a small print and a low-resolution screen. It is the exact opposite of the abstract
disc glyph, whose roll cue was a small high-frequency feature and died at a grazing angle.

**Partial views — a claim I made and then withdrew.** I ran a masked-window test and concluded that
a single quarter of the face was enough to read the orientation. **That test was invalid and the
conclusion is wrong.** It compared a window of the original against *the same window* of the mirrored
and rotated versions — which silently hands the observer the registration. It assumes you already
know that the patch you are looking at is the top-left quarter, which is exactly the thing you would
not know if you could only see a quarter.

**You need to see the whole face.** The cues here are *relational*: the big ear only means something
next to the small ear, and the mouth offset only means something relative to the centreline. None of
them is self-locating. Whole-face is the operating condition, and the design should be judged and
used on that basis.

That does not weaken the size result above, which always used the complete silhouette: the whole face
reads at 16 px. Needing all of it, and needing very little resolution of it, are compatible — and for
a part held in a hand, seeing all of it is the normal case.

**The signal is allocated to the right risks.** The strongest cue (up to 41.7 %) guards against
inserting it upside down — the mistake people actually make. The weakest (~23 %) guards the mirror
case, which needs the part flipped over and which the protrusion already prevents mechanically.

It also does mechanical work beyond the ridge. The ridge alone breaks 180° rotation; the asymmetric
outline additionally defeats the **mirrored-part** case — a mirror-image copy will not fit, so a
modelling or printing mirror is caught at assembly rather than three steps later.

And for children specifically, a symmetric cartoon face reads as a mask; illustrators asymmetrise
deliberately so a face reads as a *character*. The asymmetry is earning its keep three ways at once.

### What is worth keeping in mind anyway

**The ears differ by 42 %** — left 8.33 mm wide (top y 65.68), right 11.81 mm (top y 66.69). Both
start at the same y = 60.79, so they read as a deliberate pair rather than an error. 42 % is well
above the perceptual threshold: you see it instantly. Good cue.

**The mouth is a smirk** — x −21.93 … 0.00, centred at x = −10.96, stopping on the centreline. A
classic character device and a strong asymmetry.

**The rounds are the best cue and the one safety question.** All four are on the left — Ø11.71 at
(−40.82, 7.38), Ø11.71 at (−34.76, 0.58), Ø10.00 at (−29.85, 60.83), Ø2.90 at (−26.70, 65.95) — and
the right side is entirely sharp. This is the *most locally readable* cue in the design: the ears
differ only by comparison (you must see both to know which is which), whereas a rounded corner tells
you "this is the left" from that corner alone, by eye **or by fingertip**. For children assembling by
feel that is the cue doing the real work.

The tension is that "sharp" on a children's part is a hazard, and the obvious safety fix — round
everything — destroys the cue. The resolution is not round-vs-sharp but **large-vs-small radius**:
keep R≈6 on the left and give the right R≈1. R1 still reads and feels sharp locally, so the cue
survives, and the actual edge hazard goes away. That is the one recommendation that outlives the
correction.

**One measurement that does not fit the story:** the outline is off-centre by **0.54 mm** (left reach
40.99, right reach 42.07). A deliberate cue should be unmissable; 0.54 mm is invisible. It is
probably a by-product of the other features rather than intent — worth a look, not a defect.

---

## Two judgement calls, not defects

**The snout is highest at the nose tip and slopes down toward the brow** — a real bear's muzzle
does the opposite. Anatomically it reads more like a beak or a horn than a snout. But mechanically
it is the better choice: the nose tip enters the pocket first and does the finding. Keep it if the
lead-in matters more than the likeness; flip it if "it must look like a bear" wins.

**Only the male was supplied**, so the clearance, the fit and the pocket are unassessed. Nothing in
this note should be read as a judgement on them.

---

## The strategic point, which is the real reason this design is good

It gives orientation **a name**. "Ears up, nose down" needs no legend, no convention and no
documentation. Face recognition is the most robust pattern-matching humans have: it survives low
resolution, poor light, partial occlusion and peripheral vision. That is exactly the robustness the
abstract ridge key was reaching for, and here it comes for free.

**One earlier objection does not transfer — noting it only so it is not carried over by mistake.**
In §8c of the design doc a female *pocket* measured as visually invisible — flat-shaded, a recess
reads as a blank rectangle — and I concluded male/female
is the wrong polarity cue. **That was a viewport finding, and it does not apply to a physical part.**
Nobody looks into the pocket of a toy; they feel it. For a part in a child's hands, male/female is
exactly the right polarity language. The earlier conclusion stands for the on-screen glyph and must
not be carried over to this.

**The one rule to write down now:** the face and the key must never be allowed to disagree. People
will trust the face over the mechanics every time. Here they agree — ridge on the centreline, ears
up. If the face is ever restyled independently of the key, a user will orient by the bear and be
wrong. Tie them permanently, in the model and in whatever generates it.
