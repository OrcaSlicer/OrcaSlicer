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

Three audiences, one interface:

- **The fourteen-year-old on a school laptop.** Free software, on the machine
  they already have, with no account, no subscription, no licence and no
  tutorial. They open the tab because they want a bracket for a bike light, and
  an hour later it is printing. This is not the charity case at the bottom of
  the list — it is the reason the project is worth doing. A CAD tool that only
  the equipped can run is a tool for people who were already going to design
  something; this one has to be a creative instrument in the hands of someone
  who did not yet know they could make things. Everything in §6.1 exists to
  keep that door open, and nothing gets to close it for the convenience of the
  other two audiences.
- **The maker** who has an idea and a printer, and who has bounced off FreeCAD.
  They should be modelling something real within ten minutes of first opening
  the tab, without a tutorial, without knowing the word "constraint".
- **The mechanical designer** who needs assemblies, mates, exploded views,
  variables, and a feature history they can edit six months later. They should
  not have to leave for SolidWorks the moment the work gets serious.

The order matters. When a decision helps one audience and hurts another, the
earlier one wins unless there is a written argument for why not.

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
The tool consumes what you pointed at — and, better still, the thing you pointed
at offers the tool itself (§4).

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
the geometry, not a form; a feature that needs confirming gets a ghost preview and
a confirm/cancel puck in the scene beside it (§4.2) — an object, not a window: the
camera still orbits, the values are still editable, nothing is blocked.

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

The floor, applied to every new interaction (details in §6.2): full keyboard
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

### L11 — The floor is a school laptop, and nothing is behind a door

The product runs, completely, on a low-end laptop with integrated graphics and a
small screen, offline, with no account, no subscription and no feature withheld.
No capability in this document is reserved for a paid tier, a cloud service, a
plugin, or a machine with a discrete GPU — there is one product and everybody
gets all of it.

> **Test.** On the reference low-end machine (§6.1), at 1366×768, with the
> network cable pulled and no account ever created: does this feature work, and
> is it usable at an honest frame rate? Any "no" is a defect, not a limitation.

## 4. Interaction grammar — object-driven

The rules above compose into one sentence the whole product obeys:

> **Point at geometry → the geometry offers what can be done to it → choose the
> tool → manipulate handles and type exact values → confirm or cancel.**

The selection does not merely feed the tool. **The selection determines which
tools exist.** Pick a planar face and the product shows you the small set of
things a planar face can become — sketch on it, extrude it, hole it, shell it,
put a datum on it. Pick an edge and that set is fillet, chamfer, and the sketch
tools that can use it as a reference. Nothing else is offered, because nothing
else is possible.

This is the single largest thing we can do for a first-time user, and it is
worth stating as the reason: a beginner's difficulty is not operating a tool,
it is **not knowing which tools apply to what they are looking at**. A palette
of sixty icons answers a question they cannot yet ask. A face that offers its
own five verbs teaches the model of the product by using it. It also removes an
entire class of failure — a tool that silently does nothing because the
selection was wrong can no longer be reached.

### 4.1 The offer, and the one thing that makes it work

The flow, in full:

> **click a geometric object → an offer whose tools are always in the same
> place, each with its icon and its shortcut → click.**

- **It appears at the selection**, in the viewport, as a compact radial of icons
  — not a list in a panel, not a dropdown, not a menu bar. Radial because a
  *direction from where you clicked* is an absolute spatial address that
  survives the offer appearing anywhere on screen, while "third item down"
  does not.
- **Position is fixed and it is the whole point.** A tool occupies one permanent
  address in the offer, and that address is the same in every selection where
  the tool appears. Fillet is down-right on an edge, on a face, on a body, on
  the day the product ships and two years later. The hand learns the place; the
  eye stops being needed.
- **It is an accelerator, not a toll gate.** The toolbar and the single-letter
  shortcuts keep working exactly as they do now, and pressing a tool directly
  consumes the same selection (L3). An expert never has to look at the offer;
  a beginner never has to know the toolbar exists. Both routes land in the same
  place — this is the only way one interface serves §2's three audiences.
- **Every slot shows its keyboard shortcut**, next to the icon and the
  drawing-office word (L10). This is deliberate: the offer is the path by which
  a user stops needing the offer. You reach for fillet in its place, the place
  says "F", and one day your hand types F before the ring has finished drawing.
  A menu that teaches its own shortcut is how a beginner becomes the power user
  who never opens it — the same interface, at two speeds, with no "advanced
  mode" between them (§7).
- **It is a teaching surface** in the other direction too: an empty slot says
  *this cannot be done to this thing*, which is an answer, where a tool that
  silently does nothing is not.
- **It never blocks the view of what it acts on** — the centre is a hole, the
  picked geometry stays visible through it — and it dismisses the moment the
  selection changes.
- **Click to open, click to choose.** A flick gesture may be added later as an
  accelerator for people who want it, but nothing is ever *only* reachable by
  drag or by timing (L9, §6.2).

#### The slot-constancy invariant

This is the rule that has to survive every future feature, so it is written as
an invariant rather than as advice:

> **Every tool has exactly one address in the offer. That address is identical
> for every selection type in which the tool appears. Slots for tools that do
> not apply to the current selection are left EMPTY — the offer is never
> compacted, re-sorted or re-ordered. Adding a tool never changes the address of
> an existing one.**

Two consequences the group must accept together with the invariant:

- **No adaptive ordering. Ever.** Not most-used-first, not recently-used-first,
  not per-selection frequency. An offer that rearranges itself to be helpful
  destroys the only thing that made it fast, and it does so precisely for the
  user who has just started to learn it. (Office 2000's adaptive menus are the
  textbook case; they were removed.)
- **Empty slots are the price, and they are worth it.** A compacted offer is
  denser and unlearnable. A sparse one wastes a few hundred square pixels and
  is memorised in a week.

#### Proposed slot map — for the group to ratify

The invariant is not negotiable. The specific assignment below is a first
proposal, and the group should argue about it *once*, then never again — every
later change re-addresses somebody's muscle memory.

Eight compass positions, grouped so the map itself has a logic (material grows,
then is refined, then is repeated, then is moved):

| Position | Family | On a face | On an edge | On a body | On text/art |
|---|---|---|---|---|---|
| **N** | Create | Sketch on it | — | — | Edit text |
| **NE** | Add material | Extrude, boss | — | Thicken | Extrude |
| **E** | Remove material | Hole, pocket | — | Shell, cut | — |
| **SE** | Dress-up | — | Fillet, chamfer | Draft | — |
| **S** | Repeat | Pattern | Pattern along it | Mirror, pattern | Pattern |
| **SW** | Transform | Align to | — | Move, rotate, scale | Move, size |
| **W** | Reference | Datum plane, measure | Datum axis, measure | Measure | — |
| **NW** | Modify | Delete face, replace | — | Edit feature, delete | Replace art |

Where a family holds more than one verb for a selection, the slot opens a
secondary ring **with its own fixed addresses** — the same rule, one level down.
A dash means the slot is drawn empty for that selection.

The centre of the ring names what is selected ("Top face · Body 1"), because a
user who mis-picked should find that out before choosing a verb, not after.

### 4.2 Confirm and cancel are objects, not gestures

The old rule — click empty space to commit — is withdrawn. It was an invisible
gesture with a destructive meaning: nothing on screen said it, and a stray click
committed a feature the user was still adjusting. That is exactly what L5
forbids, and it is hostile to the audience §6.1 exists for.

- **A pending feature carries a confirm/cancel puck**, attached to the geometry
  it is editing, next to its handles: ✓ commits, ✗ discards. Enter and Escape
  mirror them for the keyboard (L9). It is drawn where the user's attention
  already is, and it is the only thing in the viewport that commits.
- **Empty space now means "clear the selection"** — the safe meaning, and the
  same meaning everywhere.
- **This is not a dialog** (L4). It is two objects in the scene, on the
  geometry, non-modal: the camera still orbits, the tree is still there, the
  values are still editable while it waits.
- **Continuous tools do not ask.** Drawing a line, a rectangle, a circle commits
  each entity as its own gesture completes — a ✓ per line would destroy the
  inner loop. The puck belongs to *features* (extrude, fillet, hole, pattern,
  mate) and to sketch edits that hold a pending state. Enter/Escape end a
  continuous tool rather than confirming an entity.
- **Ambiguity resolves toward keeping work, never toward losing it.** Starting
  another operation while a valid feature is pending commits it rather than
  discarding it; if it is not valid, the product says why (L7) and keeps it
  pending. Since undo reaches everything (§6.1), the recoverable direction is
  always the right default.

### 4.3 The rest of the grammar

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
  created it — including its offer and its puck.

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
- **The budget is set by the smallest screen we serve**, 1366×768 (§6.1) — not
  by the reviewer's monitor. Chrome that fits a 27-inch display and swallows a
  laptop's has not fitted, it has just failed somewhere the author cannot see.

## 6. Accessibility — reach first, then the assistive floor

"Accessible" means two different things and the product owes both. §6.1 is about
**who can get in at all**; §6.2 is about **who can operate it once inside**.
Neither is a phase. Both are merge requirements.

### 6.1 Reach — the door has to be open

The premise of the whole project: someone with no money, no licence, no account,
no fast machine and no teacher can open this and make a real thing. Free
software on a school laptop is the only path to a CAD tool that reaches people
who were never going to be handed one. If a design decision quietly raises the
cost of entry, it has broken the premise, however elegant it is.

- **The reference machine.** A 5-year-old laptop: dual/quad-core CPU,
  **integrated graphics**, 8 GB RAM, **1366×768** screen, no discrete GPU. The
  Design tab must be usable there, and any interaction that needs more is a
  design failure to be solved, not a requirement to be documented. The GPU path
  degrades gracefully to software rendering rather than refusing to start; the
  viewport stays interactive while the kernel thinks.
- **1366×768 is the layout target, not the stretch case.** A form-heavy side
  panel is not merely inelegant on that screen — it takes the model off it.
  This is the second, independent argument for the whole of L1 and §5.
- **No account, no cloud, no connection.** The product works forever with the
  network unplugged. Nothing is uploaded, no sign-in gates any feature, no
  telemetry is required to use it. A school network that blocks everything must
  not be able to block this.
- **No tier, no plugin wall, no "pro".** Every feature named in this document is
  in the product everyone downloads. Assemblies and exploded views are not the
  paid half.
- **Files belong to the user**, on their disk, in a format that outlives the
  project: the design travels inside the ordinary project file, and the geometry
  exports to STEP and mesh formats anyone can open.
- **Learnable without instruction.** The first solid comes with no
  documentation, no video and no tutorial mode — from noticing that a face can
  be clicked. Tooltips teach the vocabulary (L10) at the moment it is needed;
  nothing is explained in a manual the user will never open.
- **Plain language at the entry tier.** The Make tier speaks in words a
  thirteen-year-old reads without stopping. Precision comes with the tier that
  needs it, and everything is translated, because "accessible" in English only
  is not accessible.
- **Exploration must be free.** Undo reaches everything, work is never lost to a
  wrong click, and no dialog ever asks the user to be sure. A tool that punishes
  experiments teaches people to stop experimenting, which is the one thing this
  audience cannot afford to learn.
- **The product never blames the user.** Failures are stated as what happened
  and what to do (L7). "Invalid input" is not an acceptable sentence anywhere.

### 6.2 Assistive floor

- **Keyboard**: every operation reachable and completable without a pointer.
  Single-letter shortcuts for sketch tools, shown in the offer itself (§4.1) as
  well as in the tooltip. The offer's eight positions are also addressable by
  arrow/numpad direction, so the spatial map works for a keyboard user too. A
  visible focus state on every focusable element. No shortcut that only works
  while the pointer happens to be over the canvas.
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

1. Which law (L1–L11) does the change most directly serve?
2. Can the whole operation be completed without the pointer leaving the
   viewport? If not, why is this the exception?
   And: does the relevant selection *offer* this tool (§4.1), or must the user
   already know it exists?
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
11. Reach (L11): screenshot at 1366×768 with the panel open — is the model still
    on screen? Does it run on integrated graphics? Does it need the network, an
    account, or a file the user cannot keep?
12. If the change adds or moves a tool in the offer: which slot, and is it that
    tool's slot in **every** selection where it appears? Did any existing tool's
    address change? (If yes, this is not a UI change, it is a breaking change to
    every user's muscle memory, and it needs the group — see §4.1.)

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
  equivalent, not improved in place. It fails L1 and it fails L11 twice over —
  on a 1366×768 screen the cards leave the model a strip.
- Seven remaining plane pickers still populate a combo instead of consuming a
  viewport selection.
- Pattern has no on-geometry spacing arrow or count badge.
- Hole is positioned by X/Y fields rather than by a point on a face.
- Booleans and cuts pick their operands from lists rather than in 3D.
- Fillet/chamfer edge selection still requires the click cycle L5 forbids.
- **Selecting geometry offers nothing.** There is no contextual offer (§4.1):
  the user faces the full toolbar whatever they have picked, and finds out that
  a tool did not apply by it doing nothing. This is the largest single item of
  new work the charter asks for, and it has a prerequisite the group owes
  itself first — ratifying the slot map, since every tool built before it lands
  will have to be addressed afterwards anyway.
- **Committing is an invisible click in empty space** rather than the
  confirm/cancel puck of §4.2 — the exact gesture that rule withdraws.

Nothing on the violating list is defended. The only open question for each is
what its on-geometry replacement should be.

## 11. How the group works

**Roles.** Product/UX lead (owns this document and casts the tie-break vote on
interaction questions); kernel maintainer; GUI maintainer; a print-domain
reviewer; a mechanical-design reviewer who uses the product on real work; an
accessibility reviewer covering both senses of §6 — reach and assistive — who
owns the reference machine and actually runs on it. One person may hold more
than one role; the UX lead and the mechanical-design reviewer should not be the
same person, and nobody reviews reach from a workstation.

**The absent audience needs a seat.** The fourteen-year-old is not in the room
and cannot file an issue. Someone in the group is accountable for B5 and B6, and
the group watches real first-timers use the product on the reference machine at
least once a quarter — school, makerspace, or a friend's kid. Everything else in
this document can be argued from principle; approachability can only be
observed.

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
| **B5** | First-run: from opening the Design tab to a print-ready solid, no documentation | Approachability |
| **B6** | B1 again, on the reference machine at 1366×768, offline, on a fresh account-less install | Reach (L11) |

Every task is run on the reference machine of §6.1, not on a workstation — a
number measured on a fast desktop describes an experience most of our users will
never have. B6 repeats the inner loop under the full entry conditions so that
reach is a measured quantity and not an intention.

Targets are set once each task has been measured on the current build. B5's
target is expressed in minutes-to-first-solid **by someone who has never seen a
CAD program**, and it is the number this project is ultimately judged by.

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
