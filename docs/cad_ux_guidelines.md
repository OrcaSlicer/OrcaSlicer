# Orca-CAD — UX guidelines and design charter

Status: proposed, v1. Owner: design working group. Applies to the Design tab —
the parametric CAD environment inside OrcaSlicer.

This document is a **review instrument**, not an essay. Sections 3–9 are written
so that a reviewer can hold a pull request against them and get a yes or a no.
If a rule here cannot be failed, it is badly written and should be rewritten.

---

## 1. Why this exists

A CAD tool acquires its interface by accretion. Every feature arrives needing
"just one more field", the side panel is the cheapest place to put it, and after
forty features the product is FreeCAD: complete, respected, and abandoned by
almost everyone who opens it once. That end state is not a failure of any single
decision. It is the sum of forty locally reasonable ones taken without a written
rule to violate.

So we write the rule down first, and we make additions argue against it.

## 2. Product thesis

**Orca-CAD is a modelling space for people who want a part, inside the tool that
prints it.**

Two audiences, one interface:

- The maker who has an idea and a printer, and who has bounced off FreeCAD.
  They should be modelling something real within ten minutes of first opening
  the tab, without a tutorial, without knowing the word "constraint".
- The mechanical designer who needs assemblies, mates, exploded views,
  variables, and a feature history they can edit six months later. They should
  not have to leave for SolidWorks the moment the work gets serious.

The reference for *how it feels* is Shapr3D: direct, gestural, quiet, almost no
chrome, depth revealed by what you touch rather than by what is on screen. The
anti-references are Blender (a modal keyboard language you must learn before the
first success) and FreeCAD (a workbench-and-dialog architecture where the
geometry is a preview of a form you fill in elsewhere).

We are not cloning Shapr3D's feature set. We are adopting its *interaction
economy*: the smallest number of visible controls that still makes an expert
fast.

**And one thing neither reference has:** Orca-CAD lives inside a slicer. The
plate, the nozzle, the material and the print constraints are known to the
application at design time. Designing for print is not a plugin here, it is the
home advantage. Where a rule below trades generality for print-awareness, it
trades in favour of print-awareness.

## 3. The laws

Non-negotiable. A change that breaks one of these does not get merged on the
grounds that it was easier, that the alternative is more work, or that another
CAD does it that way. Each law carries a test — the question a reviewer asks.

### L1 — Geometry first: you point, then you act

Controls live **on the geometry**: handles, arrows, points, small circles and
boxes, with an inline label tab for typed values. Not in a side panel of combos
and spin fields.

The canonical gesture: **select a face or plane in the viewport, then click the
sketch tool.** Never: click the sketch tool, then choose a plane from a list.
The tool consumes what you pointed at.

> **Test.** Can the operation be performed start to finish without the pointer
> leaving the viewport, except to press the tool itself? If a control had to be
> added to a panel to make it work, the design is not finished.

This is the law the others serve. It was stated after two proposals in a row
reached for a dropdown, and the failure mode it names is real and recurrent: a
fix that "adds a row to the plane combo" is the side-panel pattern wearing a
different hat.

### L2 — Everything draggable is typable, and everything typable is draggable

Any value produced by direct manipulation (a fillet radius, an extrude depth, a
pattern spacing, a plane offset) shows a live label on the geometry, and that
label is an editable field. Any value entered numerically has a corresponding
handle in the viewport.

Dragging is for finding the answer. Typing is for committing to it. A tool that
offers only one of the two is half a tool.

> **Test.** Point at the number the tool produces. Can you drag it? Can you
> click it and type? Both must be yes.

### L3 — Noun then verb, always the same way round

Selection precedes action, without exception, across sketch tools, features,
dress-up, booleans and mates. There is no tool in the product that is armed
first and asks for its input afterwards.

> **Test.** Does this tool work if the user has already selected the thing they
> want it applied to? Does it work *only* that way?

### L4 — No modal dialog in the modelling loop

Dialogs belong to document-level actions: open, save, import, export, preferences.
Modelling never opens one. A feature that needs three values gets three labels on
the geometry, not a form; a feature that needs a confirmation gets a ghost preview
and a click in empty space.

> **Test.** Between starting an operation and seeing its result, does a window
> appear that must be dismissed? If yes, redesign.

### L5 — One click, one visible change

Every click either changes what is on screen or tells the user why it did not.
A click that opens something invisible, arms an invisible state, or requires a
second identical click to have any effect is a defect, not a design.

This law exists because we shipped its violation twice. Sketch-tool family
buttons were flyouts whose first click only rendered a pressed state — three
separate sessions filed bugs against tools that were working. Solid picking used
a click *cycle* (first click selects the body, second refines to the face), so
sketching on a face appeared broken to anyone who clicked a face once, the way
every human does.

> **Test.** Perform the gesture exactly once, as a first-time user would. Take a
> screenshot. Is the state visibly different, and is the difference the one the
> user intended?

### L6 — The default is the answer four times out of five

Every option that has a default must have the *common* answer as its default,
measured against real parts, not against generality. "New body" as the default
result of an extrude is wrong: most extrudes join. Radius as the input for a
circle is wrong: drawings give diameter.

> **Test.** Take ten real parts. In how many is the default correct? Below eight,
> change the default or infer it from context.

### L7 — Errors are caught before the commit, in the user's words

A self-intersecting profile, a cut that removes no material, a wall thinner than
the nozzle: these are reported at the moment they become knowable, on the
geometry that is wrong, phrased as what happened and what to do — not as a kernel
exception after the fact, and never silently.

> **Test.** Is the failure detectable before the user commits? Then it must be
> reported before the user commits. Read the message aloud: does it name a thing
> the user can see and an action they can take?

### L8 — The camera is the application's job

Selecting a sketch plane orients the view to it. Committing a feature does not
throw the camera away. Zoom-to-fit exists and is one keystroke. The user is never
required to fight the view in order to reach the geometry, and orbit is bound to
the gesture people actually try.

> **Test.** Count camera manipulations in a representative modelling session.
> Any camera action the application could have performed for the user is a bug.

### L9 — Accessible by construction, not by retrofit

The floor, applied to every new interaction (details in §6): full keyboard
reach, no meaning carried by colour alone, hit targets that survive a shaky hand
and a HiDPI screen, legible labels over an arbitrary 3D background, no gesture
that depends on timing.

> **Test.** Drive the whole interaction from the keyboard. Then drive it in
> greyscale. Both must work.

### L10 — Vocabulary from the drawing office

Names come from the language of people who make parts: fillet, chamfer, boss,
rib, counterbore, mate, exploded view. Not from the kernel (no "boolean
subtract", no "B-rep"), not from invented product-speak. Where the drawing-office
word and the beginner's word differ, use the drawing-office word and make the
tooltip teach it — an approachable tool that leaves the user unable to talk to a
machinist has failed them.

> **Test.** Would a shop-floor engineer recognise this word? Would a first-time
> user be able to look it up and find a real definition?

## 4. Interaction grammar

The rules above compose into one sentence the whole product obeys:

> **Point at geometry → press a tool → manipulate handles → type exact values →
> click empty space to commit.**

Consequences worth stating explicitly:

- **Empty space commits.** Escape cancels. These two never change meaning
  between tools.
- **The status line is one imperative sentence** naming what the tool wants
  next, and it names the target when the target came from a selection
  ("Circle — click centre, then radius · on the picked face"). It is the
  authoritative feedback surface for the armed tool; the toolbar is not.
- **Hover previews, click commits.** A hover shows the ghost of what a click
  would do wherever this is cheap to compute.
- **Selection is persistent and visible** until consumed or cleared. A tool that
  consumes a selection clears it, so the next feature cannot silently inherit it.
- **Every gesture is undoable**, and the feature tree is editable history, not a
  log. Re-editing a feature re-enters the same on-geometry interaction that
  created it.

## 5. Layout and screen budget

The viewport is the application. Chrome is a tax on it.

- **One toolbar**, contextual to the mode (model / sketch). Tools are grouped by
  what they make, not by which subsystem implements them.
- **A left rail for the document, not for parameters**: feature tree, bodies,
  variables. It answers "what exists", never "what value should this be".
- **No parameter panel.** Where one exists today it is technical debt with a
  scheduled removal (§10).
- **Print context is ambient**, not a panel: the plate is visible in the design
  space, and print-domain warnings appear on the geometry that will fail.
- **Nothing is added to permanent chrome without removing something**, or
  demonstrating that the addition is used in the majority of sessions.

## 6. Accessibility floor

Not a phase. A merge requirement.

- **Keyboard**: every operation reachable and completable without a pointer.
  Single-letter shortcuts for sketch tools, documented in the tool's own
  tooltip. A visible focus state on every focusable element. No shortcut that
  only works while the pointer happens to be over the canvas.
- **Colour**: never the sole carrier of meaning. Selection is colour *and*
  outline; an error is colour *and* an icon *and* text. Verify in greyscale.
- **Contrast**: labels over the 3D viewport get a scrim or halo so 4.5:1 holds
  against any background the model can produce, including a white body under a
  white plate.
- **Targets**: handles and grips no smaller than 32 px at 100 % scale, scaling
  with the OS factor; the grab tolerance is larger than the drawn glyph.
- **Timing**: no double-click-to-mean-something-else, no press-and-hold as the
  only route to a function, no cycle that depends on repeated clicks
  (see L5).
- **Motion**: animation is functional (showing where a thing went), never
  decorative, and it respects the reduced-motion preference.
- **Text**: no fixed-width assumptions; the UI holds together in German and in
  Chinese, at 125 % and 200 % scale. Every string routed through the normal
  translation path.

## 7. Depth without clutter — the three tiers

Power for experts is delivered by **progressive disclosure of tools, never by
relocation of tools**. A tool that appears in a later tier is in the same place
it will always be; it is simply not shown yet.

| Tier | Who | What appears |
|---|---|---|
| **Make** | first hour | Sketch, extrude, revolve, hole, fillet/chamfer, move, commit to plate |
| **Model** | competent user | Patterns, shell, draft, sweep/loft, booleans, reference geometry, variables, import/export |
| **Mechanism** | mechanical designer | Assemblies and mates, exploded views, interference detection, surfaces, feature-level editing of imported solids |

Rules that keep this honest:

1. **Tiers are non-modal.** No mode switch, no workbench selector, no "advanced
   mode" toggle that changes the meaning of anything. The tier only governs what
   is *offered*.
2. **A tier reveals itself by use.** Using a body reveals boolean tools; adding
   a second body reveals assembly tools. The product notices what you are doing.
3. **Nothing moves when a tier appears.** A user who learned where fillet lives
   finds it in the same place forever.
4. **An expert tool obeys the same grammar** as a beginner tool. Mates are
   picked in 3D like everything else, not configured in a table.
5. **Exploded views are a view state**, not a document mode — reversible,
   draggable along mate axes, and never a separate file.

## 8. Designing for print — the home advantage

Design-time knowledge the application already has, and must use:

- **The plate is present** in the design space, at the real size, with the real
  origin. Committing a body to the plate is one action and preserves placement.
- **Print-domain checks run on the model, on the geometry, before slicing**:
  walls thinner than the nozzle, unsupported overhangs beyond the material's
  angle, features smaller than the layer height, a part that does not fit the
  build volume.
- **These are warnings on the geometry, never a report.** The thin wall glows;
  the tooltip says how thin and what the nozzle is.
- **Material and machine context is inherited** from the active slicer profile,
  not re-entered in the Design tab.
- **The round trip is preserved**: editing a design after slicing returns to the
  feature history, not to a mesh.

## 9. The review gate

Every pull request that touches the Design tab UI answers these, in the PR body.
A "no" that is not accompanied by an argument is a request for changes.

1. Which law (L1–L10) does the change most directly serve?
2. Can the whole operation be completed without the pointer leaving the
   viewport? If not, why is this the exception?
3. Are the values draggable *and* typable?
4. Screenshot of the state after **exactly one** click of the new gesture,
   performed as a first-time user.
5. Keyboard-only walkthrough: does it complete?
6. Greyscale screenshot: is every state still distinguishable?
7. What was **removed**? (Net additions to permanent chrome require an argument.)
8. Which tier does it belong to, and does it appear without moving anything else?
9. What does it do when the geometry is invalid, and is that reported before the
   commit?
10. Interaction cost: actions required for the canonical task it addresses,
    before and after.

## 10. Where we stand today — honest inventory

Complying with the laws already:

- Sketch inline editors — draw an entity and its dimension tab opens on the
  geometry; Tab walks Length → Width → Angle.
- Fillet/chamfer draggable radius arrow with an editable value label.
- Extrude depth arrow; move-body three-axis arrows.
- Datum-plane resize handles and offset arrow; ghost reference planes picked in
  3D.
- Imported-art place/size gizmo.
- Sketch plane taken from the picked face, with the target named in the status
  line, and the sketch-plane dropdown deleted outright.

Violating them, with removal scheduled:

- **Every tool card is a two-column form** of combos and spin fields in the left
  panel. This is the single largest debt in the product and the reason this
  document exists. Tracked as an epic; each card is replaced by its on-geometry
  equivalent, not improved in place.
- Seven remaining plane pickers still populate a combo instead of consuming a
  viewport selection.
- Pattern has no on-geometry spacing arrow or count badge.
- Hole is positioned by X/Y fields rather than by a point on a face.
- Booleans and cuts pick their operands from lists rather than in 3D.
- Fillet/chamfer edge selection still requires the click cycle L5 forbids.

Nothing on the violating list is defended. The only open question for each is
what its on-geometry replacement should be.

## 11. How the group works

**Roles.** Product/UX lead (owns this document and casts the tie-break vote on
interaction questions); kernel maintainer; GUI maintainer; a print-domain
reviewer; a mechanical-design reviewer who uses the product on real work; an
accessibility reviewer. One person may hold more than one role; the UX lead and
the mechanical-design reviewer should not be the same person.

**Cadence.** A short weekly review of open interaction proposals. A monthly pass
over the violating inventory in §10 — anything that has not moved in two months
is either scheduled or explicitly accepted as permanent, with a reason written
into this document.

**How a change moves.**

1. *Problem* — a described user difficulty, ideally with an interaction-cost
   measurement, never a solution in disguise.
2. *Sketch* — one or two on-geometry interaction proposals, drawn or described
   as a gesture sequence. Reviewed against §3 before any code.
3. *Prototype* — built behind whatever the smallest safe path is, driven end to
   end on a real display, and screenshotted at each state.
4. *Gate* — §9 answered in the PR.
5. *Merge*, then update §10.

**Decisions are written down.** Any resolution that constrains future work is
appended to this document as a numbered law or as an accepted exception with its
reasoning. A decision that lives only in a call is not a decision.

**How disagreements resolve.** Against the laws first. If the laws do not decide
it, the tie-break is the interaction cost measured on the canonical tasks in
§12; if that does not decide it, the UX lead chooses and records why.

## 12. Canonical tasks — the benchmark

The measure of every UX change is the cost of these five tasks. Each is timed and
counted (clicks, keystrokes, camera actions, mode switches) on the headless rig
and, periodically, with real users who have not seen the product.

| # | Task | What it exercises |
|---|---|---|
| **B1** | Bracket: sketch an L, extrude, two holes, fillet the inside corner, send to plate | The inner loop |
| **B2** | Change a hole diameter and the plate thickness, six features deep, and rebuild | Parametric editability |
| **B3** | Take an imported STEP, delete a boss, close the face, thicken a wall to nozzle width | Direct editing + print awareness |
| **B4** | Two parts, one revolute mate, check interference, produce an exploded view | The Mechanism tier |
| **B5** | First-run: from opening the Design tab to a printed-ready solid, no documentation | Approachability |

Targets are set once each task has been measured on the current build; B5's
target is expressed in minutes-to-first-solid, and it is the number this project
is ultimately judged by.

---

### Appendix — anti-patterns we have already paid for

Kept because each cost real time and each is easy to reintroduce.

- **The dropdown that grew a row.** Fixing "cannot sketch on a face" by adding a
  "Face of Body 1" entry to a plane combo. It reads as a small fix and it is the
  side-panel architecture reproducing itself.
- **The invisible first click.** Flyout buttons and pick cycles whose first click
  changes nothing meaningful. Filed as bugs three separate times against working
  code, and made a real bug look fixed when it was not.
- **The fix verified through a path the user will never take.** A face-sketch fix
  confirmed by double-clicking to reach face level. Users click once. A fix
  reachable only by an undiscoverable gesture is indistinguishable from no fix.
- **The wrong feedback surface.** Measuring an armed tool by the toolbar, which
  never renders keyboard-armed state. The status line is the surface that
  answers.
- **The silent success.** A cut that removed no material, reported as done. Now
  an error naming the likely cause.
