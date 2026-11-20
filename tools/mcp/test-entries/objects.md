# Objects

The design of this area's sweep tests. The rules every entry follows, the grades and the table of all tests are
in [TESTS.md](../TESTS.md).

Two tests, split where their checks do: `test_objects` measures where objects stand and how many there are (places in
mm, instances, arrangements, the print's copies), and `test_object_structure` reads what objects are made of and how
the list orders them (parts and their types, names, order, meshes and the files they come from).

## `test_objects`
`test_objects.py` · Python, 544 checks · in `sweep.json` as `{"path": "test_objects.py"}`

**Tests:** Where objects stand and how many there are: adding a shape at a size, a place, a plate and a name; moving,
turning and scaling an instance, moving it to another plate, and what Auto Drop does to a move; moving and scaling a
part; what the selection says; Mirror, Drop to Bed, Center, Scale to Fit and Auto-Orient; the number of instances and
Set as Individual Objects; Arrange and Fill bed; Clone; and leaving instances out of the print. Each command is called
by index and, where its menu item acts on the selection, with no target on the selection. It runs on the Generic
Klipper Printer 0.4 nozzle: a bed 250 mm square and 250 mm high with no excluded area (`bed_exclude_area`), and
plates laid out 300 mm apart (the bed and a fifth of it), two to a row.

**Starts from:** Every scenario starts with `start(client)` in the Prepare view and checks that `presets()` is
`{printer: KLIPPER, process: KLIPPER_STD, filaments: [GENERIC_PLA]}`, that `plates()` is `"" 0 *` and that there are
no objects; a `project_new` within a scenario is followed by the same check of the plates and objects. Before its
first scenario the test stores the arrange options its arrangements are checked under, the ones a new data folder has:
spacing 0 (automatic) and rotation off, with `object_arrange {spacing: 0, rotate: false}` of a 20 mm cube after a
`start()`, in a project the first scenario's `start()` discards, and checks that the reply's `settings` give `spacing`
0 and `rotate` false. The Arrange options panel keeps them for every arrange after, one set for each print sequence:
these are the by-layer ones, which the Klipper process prints with. An earlier test may leave rotation on
(`test_worker_admission` does), and then the turn an arrange gives a cube is the arranger's choice, while the tables
fix it at `[0, 0, 0]` after an arrange without options (arranging, step 4; cloning, steps 3 and 6). The Clone dialog's
Auto arrange box starts from the `auto_arrange` preference, which `test_preferences` changes; the test does not set
it, as cloning step 6 holds whether the dialog arranges or not.

**Leaves:** The by-layer arrange options its last arrange with options stored: spacing 10 and rotation off. The
Klipper printer selected, with one slot of `GENERIC_PLA`. `kept.3mf` at the top of the recent projects, and the folder
the file dialogs open in, both in `W`, which is removed when the test ends. On a data folder in Simple mode, Advanced
mode: an undo in Simple mode to a step whose model has several instances or a part other than a model part switches
the app to Advanced and keeps it (`Plater::priv::update_after_undo_redo()`, the app's own code), as the undo of the
number of instances, step 4, does.

**Files:** The test writes into a folder of its own, `W` (a `tempfile.TemporaryDirectory()`, removed when the test
ends), the project it saves and the G-code files it exports. Undo steps are counted by undoing them and reading the state back, not from the log.

**Reads:** `plates()` as `test_plates` reads it. New readers in `checks.py`:
- `scene()`: each object of `object_list` in index order, its entry whole (`name`, `id`, `filament`, `auto_drop`,
  `bbox`, `mesh_errors`, `parts`, and `layer_config_ranges` and `layer_height_profile` when it has them), with its
  `instances` gathered from every plate and from `outside`, each given its `plate` (−1 outside). A part is its entry
  whole: `name`, `id`, `type`, `subtype`, `filament`, `position`, `rotation_deg`, `scale`, `mirrored`, `triangles`,
  `mesh_volume`, `scaled_volume`, `bbox`, `painted`, `svg`. It sits beside `test_plates`' `objects()`, which gives only
  each object's name and plate.
- `selection()`: `app_state`'s `selection` as `{objects, instances, parts}`, `parts` absent when whole instances are
  selected.
- `corner(p)`: plate p's `origin` from `plate_list`, and `rect(p)`: its `origin` and `size`.
- `gcode_prints(path)`: the `; printing object <name> id:<n> copy <c>` comments of a G-code file, as the names in the
  order they first print, each with the number of different `id`, `copy` pairs printed under it: its copies
  (`gcode_label_objects` is on in `0.20mm Standard @MyKlipper`, as the app's default has it).
- Geometry on `bbox`: an instance's square is its `bbox` in x and y; two squares are apart when they do not overlap;
  a square is inside plate p when it lies within `rect(p)`; the gap between two squares is the distance between
  them: along the axis on which they are apart, corner to corner when they are apart on both.

In the tables `plates()` is written as in `test_plates`; a table without a plates column says in its introduction what
`plates()` is after every step, and one without an objects column says what its objects are. The objects are listed in index order between `·`, each as
its name, `@` and the plate its instances stand on, and `×n` when it has n instances: `a@0×3` · `b@0` is object 0
named a with three instances on plate 0, then object 1 named b with one. An object the tables call by a name of its
own (first, block, a, kept and so on) is added with `name` set to it; Cube, a shape's own name, is added without one.
After every step the names, plates and
instance counts of `scene()` are compared with the row whole. A place is written `+(x, y)`: an instance's `offset`
x and y measured from the corner of the plate it stands on, checked to 0.001 mm (a part's place is its `position`,
written the same way with its z). An instance keeps the place the table last gave it until a row gives it another, or
says that the app places it, after which the row says what that place must hold. Unless a row says otherwise, an
instance has `rotation_deg` `[0, 0, 0]`, `scale` `[1, 1, 1]`, `mirrored` false and `printable` true and rests on the
bed (`bbox.min` z 0 to 0.001), and its object has `auto_drop` true; what a row changes holds until a later row changes
it again. After every step these values and every place the table fixes are compared for every instance.

**Scenario: adding a shape**

| # | Step | Plates after | Objects after | Also checked |
|---|---|---|---|---|
| 1 | `plate_add`, twice | `"" 0` · `"" 0` · `"" 0 *` | none | |
| 2 | `object_add_primitive {shape: "cube", size: [20, 20, 20], plate: 1, name: "first"}` | `"" 0` · `"" 1 *` · `"" 0` | first@1 | the reply's `object` 0, `plate` 1 and `objects` naming first; the app places first, its square inside plate 1; `bbox.size` `[20, 20, 20]`; `selection()` objects `[0]` |
| 3 | the same with `name: "second"` and `position: corner(1) + (112.8, 110, 0)` in place of `plate`, first still selected | `"" 0` · `"" 2 *` · `"" 0` | first@1 · second@1 | the reply's `object` 1, `plate` 1 and `offset` `corner(1) + (112.8, 110, 10)`; second at +(112.8, 110); `selection()` objects `[1]`: a call with a size adds the shape whatever is selected, and selects it |
| 4 | `undo` | `"" 0` · `"" 1 *` · `"" 0` | first@1 | adding, naming and placing second were one undo step |
| 5 | `redo` | as after 3 | as after 3 | second at +(112.8, 110) |
| 6 | `object_select {none: true}`, then `object_add_primitive {shape: "cube", size: [40, 30, 10]}` | `"" 0` · `"" 3 *` · `"" 0` | first@1 · second@1 · Cube@1 | named after its shape; `bbox.size` `[40, 30, 10]`; the app places it, its square inside plate 1 |
| 7 | `object_add_primitive {shape: "cylinder", size: [1, 2, 3]}` | as after 6 | as after 6 | refused, saying that a cylinder's size is `[diameter, height]` |

**Scenario: moving an instance, and Auto Drop**

block and other are 20 mm cubes added with `object_add_primitive` at a `position`, with `object_select {none: true}`
between them.

| # | Step | Plates after | Objects after | Also checked |
|---|---|---|---|---|
| 1 | block at `corner(0) + (100, 100, 0)`, other at `corner(0) + (200, 200, 0)` | `"" 2 *` | block@0 · other@0 | block at +(100, 100), other at +(200, 200) |
| 2 | `object_transform {object: 0, scale: 1.5}` | as after 1 | as after 1 | block's `scale` `[1.5, 1.5, 1.5]` and `bbox.size` `[30, 30, 30]`; it rests on the bed: a scale that grows it drops it back; still at +(100, 100) |
| 3 | `object_transform {object: 0, size: [45, null, null]}` | as after 1 | as after 1 | `scale` `[2.25, 2.25, 2.25]`, `bbox.size` `[45, 45, 45]`: one size and two nulls keep the proportions |
| 4 | `object_transform {object: 0, size: [30, 20, 10]}` | as after 1 | as after 1 | `scale` `[1.5, 1, 0.5]`, `bbox.size` `[30, 20, 10]` |
| 5 | `object_transform {object: 0, rotate: [0, 0, 30], mode: "absolute"}` | as after 1 | as after 1 | `rotation_deg` `[0, 0, 30]`, `bbox.size` `[35.98, 32.32, 10]` to 0.01 |
| 6 | `object_transform {object: 0, rotate: [0, 0, 15]}` | as after 1 | as after 1 | `rotation_deg` `[0, 0, 45]`: a call is relative by default; `bbox.size` `[35.36, 35.36, 10]` to 0.01 |
| 7 | `object_transform {object: 0, translate: [10, -20, 0]}` | as after 1 | as after 1 | block at +(110, 80) |
| 8 | `object_transform {object: 0, position: corner(0) + (50, 60, 0)}` | as after 1 | as after 1 | block at +(50, 60): `position` is where it goes, whatever `mode` says |
| 9 | `plate_add` | `"" 2` · `"" 0 *` | as after 1 | |
| 10 | `object_transform {object: 0, plate: 1}` | `"" 1` · `"" 1 *` | block@1 · other@0 | the reply's `plate` 1; block at +(50, 60) on plate 1; `object_list {plate: 1}` lists block alone, with its instance 0 |
| 11 | `object_transform {object: 0, plate: 0}` | `"" 2` · `"" 0 *` | as after 1 | block at +(50, 60) on plate 0 again; moving an instance leaves the current plate |
| 12 | `object_transform {object: 0, translate: [0, 0, 15]}` | as after 11 | as after 1 | block rests on the bed: with Auto Drop on, a lifted object drops |
| 13 | `object_set_auto_drop {objects: [0], auto_drop: false}` | as after 11 | as after 1 | the reply's `objects` naming block alone and `auto_drop` false; block's `auto_drop` false, other's true |
| 14 | `object_transform {object: 1, translate: [0, 0, 15]}` | as after 11 | as after 1 | other rests on the bed: its Auto Drop is on |
| 15 | `object_transform {object: 0, translate: [0, 0, 15]}` | as after 11 | as after 1 | block's `bbox.min` z 15 and `bbox.max` z 25: with Auto Drop off it stays where it is moved |
| 16 | `undo` | as after 11 | as after 1 | block rests on the bed: the move was one undo step |

**Scenario: moving a part, the selection, and Mirror**

base is a 30 × 30 × 10 mm cube at `corner(0) + (100, 100, 0)`, single a 20 mm cube at `corner(0) + (200, 200, 0)`.
Parts are read from `scene()`; part 0 of base, the cube itself, is at +(100, 100, 5) after every step but 2. After
every step the objects are base@0 · single@0, base at +(100, 100) (but in step 2, as the row says) and single at
+(200, 200), and `plates()` is `"" 2 *`.

| # | Step | Part 1 of base after | Also checked |
|---|---|---|---|
| 1 | base and single, then `part_add {object: 0, type: "part", shape: "cube", size: [10, 10, 10], position: corner(0) + (120, 100, 15)}` | named `Generic-Cube`, `type` part, at +(120, 100, 15) | the reply's `part` 1 and `part_count` 2; part 1's `filament` 1, `triangles` 12, `mesh_volume` 1000, `scaled_volume` 1000; base's `bbox.size` `[40, 30, 20]` |
| 2 | `object_transform {object: 0, size: [60, null, null]}` | at +(130, 100, 22.5) | base's `scale` `[1.5, 1.5, 1.5]` and `bbox.size` `[60, 45, 30]`: the size of an object of two parts scales both in proportion, about the instance's place; it rests on the bed, part 0 at +(100, 100, 7.5) |
| 3 | `undo` | at +(120, 100, 15) | base's `scale` `[1, 1, 1]` |
| 4 | `object_transform {object: 0, part: 1, translate: corner(0) + (110, 100, 15), mode: "absolute"}` | at +(110, 100, 15) | the reply's `part_info.position` the same |
| 5 | `object_transform {object: 0, part: 1, translate: [5, 0, 0]}` | at +(115, 100, 15) | |
| 6 | `undo` | at +(110, 100, 15) | each part move is one undo step |
| 7 | `object_transform {object: 0, part: 1, scale: 2, mode: "absolute"}` | `scale` `[2, 2, 2]`, at +(110, 100, 15) | the reply's `size` `[20, 20, 20]`; `scaled_volume` 8000; base still rests on the bed |
| 8 | `object_transform {object: 0, part: 1, size: [null, null, 3]}` | `scale` `[0.3, 0.3, 0.3]` | the reply's `size` `[3, 3, 3]`: one size keeps the proportions; `mesh_volume` 1000, `scaled_volume` 27 |
| 9 | `object_transform {object: 0, part: 1, rotate: [0, 0, 45]}` | `rotation_deg` `[0, 0, 45]`, at +(110, 100, 15) | |
| 10 | `object_transform {object: 0, part: 1, rotate: [0, 0, 0], mode: "absolute"}` | as after 9 | refused, saying a part rotates relative to its current rotation |
| 11 | `object_transform {object: 0, part: 1, plate: 1}` | as after 9 | refused, saying `plate` moves an instance |
| 12 | `object_transform {object: 1, part: 0, translate: [1, 0, 0]}` | as after 9 | refused, saying object 1 has one part; single at +(200, 200) |
| 13 | `object_select {parts: [[0, 1]]}` | as after 9 | the reply is `{canvas: "3d", empty: false, objects: [0], instances: [[0, 0]], parts: [[0, 1]]}`, and `selection()` the same |
| 14 | `object_select {objects: [0]}` | as after 9 | the reply's `objects` `[0]` and `instances` `[[0, 0]]` and no `parts`: a whole instance lists none; `selection()` the same |
| 15 | `object_mirror {parts: [[0, 1]], axis: "x"}` | `mirrored` true, at +(110, 100, 15) | the reply names part 1 of base; a part mirrors in its place; base's instance not mirrored. From here part 1's `rotation_deg` is not compared (see Not checked) |
| 16 | `object_mirror {objects: [0], axis: "x"}` | at +(90, 100, 15) | base's instance `mirrored` true; an instance mirrors about the centre of the selection's box, here x 100, as part 1 lies within the cube's width: part 1 goes to the other side and part 0 stays at +(100, 100, 5); single not mirrored |
| 17 | `object_mirror {objects: [0], axis: "y"}` | at +(90, 100, 15) | base's instance `mirrored` false and `rotation_deg` `[0, 0, 180]`: a mirror along X and one along Y make a half turn; the box's centre is y 100, so the places stay |
| 18 | `object_select {none: true}`, then `object_mirror {axis: "x"}` | as after 17 | refused, `nothing_selected`; base's instance and single not mirrored, part 1 `mirrored` true |

**Scenario: the number of instances**

a and b are 20 mm cubes at `corner(0) + (60, 60, 0)` and `corner(0) + (180, 60, 0)`; b has a modifier, added with
`part_add {object: 1, type: "modifier", shape: "cube", size: [5, 5, 5]}`. An instance a call adds goes 12.5 mm
(0.05 of the bed's 250 mm) further along x and y than the object's last instance. The plate is `"" 2 *` after
every step but these: `"" 3 *` after 18 and 20, and `"" 5 *` after 21.

| # | Step | Objects after | Also checked |
|---|---|---|---|
| 1 | a and b | a@0 · b@0 | a at +(60, 60), b at +(180, 60) |
| 2 | `object_select {none: true}`, then `object_set_instances {count: 3}` | as after 1 | refused, `nothing_selected` |
| 3 | `object_set_instances {object: 0, count: 3}` | a@0×3 · b@0 | the reply's `object` 0, `instances` 3 and `objects` naming a; a at +(60, 60), +(72.5, 72.5), +(85, 85); `selection()` objects `[0]` |
| 4 | `undo` | as after 1 | the count was one undo step |
| 5 | `object_set_instances {object: 0, count: " 2 "}` | a@0×2 · b@0 | a count written as text is read as the number; a at +(60, 60), +(72.5, 72.5) |
| 6 | `object_set_instances {object: 0, delta: 1}` | a@0×3 · b@0 | a's third at +(85, 85) |
| 7 | `object_set_instances {object: 0, delta: -1}` | a@0×2 · b@0 | the last instance goes |
| 8 | `object_set_instances {object: 0, count: 1}` | as after 1 | a at +(60, 60) |
| 9 | `object_set_instances {object: 0, count: c}` for c 0, −2, 1001, 2.5 and `"three"` | as after 1 after each | each refused: 0 and −2 by the schema, saying `count must be >= 1`; 1001 as `bad_instance_count`; 2.5 and `"three"` by the schema, saying `count must be of type integer` |
| 10 | `object_select {objects: [0, 1]}`, then `object_set_instances {count: 2}` | as after 1 | refused, `one_object_only` |
| 11 | `object_set_instances {object: 0, count: 2}`, then `object_select {instances: [[0, 1]]}` | a@0×2 · b@0 | the reply's `objects` `[0]` and `instances` `[[0, 1]]`, and `selection()` the same |
| 12 | `object_select {objects: [0]}` | as after 11 | the reply's `instances` `[[0, 0], [0, 1]]`: the selection names the instances it is in |
| 13 | `object_select {instances: [[0, 1]]}`, then `object_set_instances {count: 4}` | a@0×4 · b@0 | the selected instance stands for its object: a at +(60, 60), +(72.5, 72.5), +(85, 85), +(97.5, 97.5) |
| 14 | `object_select {parts: [[1, 1]]}`, then `object_set_instances {count: 2}` | a@0×4 · b@0×2 | the selected part stands for its object: b at +(180, 60), +(192.5, 72.5) |
| 15 | `object_set_printable {instances: [[1, 0]], printable: false}`, then `object_set_instances {object: 1, count: 3}` | as after 14 | refused, `unprintable_instance`; b's instances `printable` false, true |
| 16 | `object_set_instances {object: 1, count: 1}` | a@0×4 · b@0 | fewer instances need no printable rule; b's instance at +(180, 60), `printable` false |
| 17 | `object_set_printable {objects: [1], printable: true}`, `object_select {objects: [1]}`, then the Set Number of Instances action (`action_run` of `obj_set_instances`), its dialog answered Cancel | as after 16 | the call replies `blocked_by_dialog`, the dialog open titled `Copies of the selected object`; after Cancel, `app_wait {for: "job"}` on its job is met and gives `object` 1 and `instances` 1; b `printable` true |
| 18 | `object_set_instances {object: 1, count: 2}`, then `object_separate_instances {object: 1}` | a@0×4 · b@0 · b@0 | the reply's `objects` names b at 1 and 2; object 2 at +(192.5, 72.5) with parts of `type` part and modifier: each instance's object keeps the parts |
| 19 | `undo` | a@0×4 · b@0×2 | Set as Individual Objects was one undo step |
| 20 | `object_separate_instances {instances: [[0, 3]]}` | a@0×3 · b@0×2 · a@0 | the reply's `objects` names a at 2; object 2 at +(97.5, 97.5), where the instance stood |
| 21 | `object_select {objects: [0]}`, then `object_separate_instances {}` | a@0 · b@0×2 · a@0 · a@0 · a@0 | the reply's `objects` names a at 0, 3 and 4; a at +(60, 60), object 3 at +(85, 85), object 4 at +(72.5, 72.5): each instance but the first becomes an object, the last instance first |
| 22 | `project_new`, a 20 mm cube named cut, cut in two with a connector through the Cut gizmo (as `test_menu_items_refusals` cuts it), then `object_set_instances {object: 0, count: 2}` | cut@0 · cut@0 | refused, `cut_object`; each half one instance; `plates()` `"" 2 *` |

**Scenario: placing objects**

first, second, third, fourth, fit and fit2 are 20 mm cubes added with `object_add_primitive {shape: "cube", size:
[20, 20, 20]}` at the `plate`, `position` and `name` the row gives; second has a 5 mm cube part, added with
`part_add {object: 1, type: "part", shape: "cube", size: [5, 5, 5], position: corner(1) + (115, 60, 2.5)}`, 15 mm to
the right of its centre.

| # | Step | Plates after | Objects after | Also checked |
|---|---|---|---|---|
| 1 | `plate_add`, twice; first at plate 1 +(40, 60), second at plate 1 +(100, 60) with its part, third at plate 2 +(60, 60) | `"" 0` · `"" 2` · `"" 1 *` | first@1 · second@1 · third@2 | the places as added; second's part at +(115, 60, 2.5) |
| 2 | `object_mirror {objects: [1], axis: "x"}` | as after 1 | as after 1 | the reply names second; second `mirrored` true, first and third false. An instance mirrors about the centre of the selection's box, x +103.75 (the cube from +90 to +110, its part to +117.5): second at +(107.5, 60), its part at +(92.5, 60, 2.5) |
| 3 | `object_transform {object: 1, translate: [0, 0, 15], drop_to_bed: false}` | as after 1 | as after 1 | second's `bbox.min` z 15: an object of two parts stays above the bed |
| 4 | `object_transform {object: 0, translate: [0, 0, 15], drop_to_bed: false}` | as after 1 | as after 1 | first rests on the bed: an object of one part left above it drops anyway |
| 5 | `object_drop {objects: [1]}` | as after 1 | as after 1 | the reply's `moved` true and `objects` naming second; second rests on the bed |
| 6 | `object_drop {objects: [1]}` | as after 1 | as after 1 | not refused: the reply's `moved` false |
| 7 | step 3 again, `object_select {objects: [1]}`, then `object_drop {}` | as after 1 | as after 1 | the reply's `moved` true; second rests on the bed: no target drops the selection |
| 8 | `object_center {objects: [0, 2]}` | as after 1 | as after 1 | the reply's `plates` `[{plate: 1, objects: [0]}, {plate: 2, objects: [2]}]`: each goes to the centre of its own plate; first at +(125, 125), third at +(125, 125) |
| 9 | the same | as after 1 | as after 1 | the places as after 8 |
| 10 | fourth at plate 1 +(125, 60) | `"" 0` · `"" 3 *` · `"" 1` | first@1 · second@1 · third@2 · fourth@1 | |
| 11 | `object_center {objects: [0, 3], plate: 0}` | `"" 2` · `"" 1 *` · `"" 1` | first@0 · second@1 · third@2 · fourth@0 | the reply's `plates` `[{plate: 0, objects: [0, 3]}]`; the two move as one group, its box to plate 0's centre: first at +(125, 157.5), fourth at +(125, 92.5) |
| 12 | `object_select {objects: [2]}`, then `object_center {}` | `"" 2` · `"" 2 *` · `"" 0` | first@0 · second@1 · third@1 · fourth@0 | the selection goes to the centre of the current plate, plate 1, not its own: third at +(125, 125) on plate 1; the reply's `plates` `[{plate: 1, objects: [2]}]` |
| 13 | `object_transform {object: 2, rotate: [45, 0, 0], mode: "absolute"}` | as after 12 | as after 12 | third's `rotation_deg` `[45, 0, 0]`, `bbox.size` z 28.28 to 0.01 |
| 14 | `object_auto_orient {objects: [1, 2]}` | as after 12 | as after 12 | the reply's `objects` naming second and third, and `rotations` an entry for instance 0 of each; third lies on a face: `rotation_deg` x 90 to 0.1, `bbox.size` z 20 to 0.01, resting on the bed; first and fourth keep `rotation_deg` `[0, 0, 0]`: naming objects turns no other; `selection()` objects `[1, 2]`. The app turns second: its rotation is not fixed, and it rests on the bed |
| 15 | `object_select {none: true}`, `object_transform {object: 0, rotate: [0, 45, 0], mode: "absolute"}`, then `object_auto_orient {}`, the Auto-Orient action's arguments | as after 12 | as after 12 | with nothing selected every object is oriented: the reply's `rotations` one entry for each of objects 0 to 3, and each entry's `rotation_deg` is that instance's in `scene()`; first lies on a face, `bbox.size` z 20 to 0.01; which turn second, third and fourth get is not fixed |
| 16 | fit at plate 1 +(60, 200), then `object_scale_to_fit {objects: [4]}` | `"" 2` · `"" 3 *` · `"" 0` | as after 12 · fit@1 | the reply's `scales` `[{object: 4, instance: 0, scale: [s, s, s]}]`, and fit's `scale` the same, with s 12.488 to 0.001; fit's `bbox.size` `[249.75, 249.75, 249.75]` to 0.02, at +(125, 125) to 0.01, resting on the bed: it fills the bed, its centre on the bed's |
| 17 | fit2 at plate 1 +(200, 60), `object_select {objects: [5]}`, then `object_scale_to_fit {}`, the Scale to Fit action's arguments | `"" 2` · `"" 4 *` · `"" 0` | as after 16 · fit2@1 | no target scales the selection: the reply's `scales` names object 5 with s, fit2's `scale` s and at +(125, 125) |

**Scenario: arranging and filling the bed**

A, B and C are 20 mm cubes added with `object_add_primitive` at a `position`, with `object_select {none: true}`
between them. After an arrange the app places the objects it arranged; the row says what their places hold. An arrange
may reorder objects, so in this scenario the objects are found by name, and a row's objects are compared as names,
plates and instance counts in any order.

| # | Step | Plates after | Objects after | Also checked |
|---|---|---|---|---|
| 1 | `object_arrange {}` | `"" 0 *` | none | refused, `no_objects_to_arrange` |
| 2 | `object_arrange {plates: 0}` | `"" 0 *` | none | refused, `no_objects_to_arrange` |
| 3 | A and B both at `corner(0) + (128, 128, 0)` | `"" 2 *` | A@0 · B@0 | A and B at +(128, 128): one on the other |
| 4 | `view_set {tab: "preview"}`, then `object_arrange {}` | as after 3 | A@0 · B@0 | the reply's `off_plate` `[]` and `plates` 1; `app_state` `tab` `prepare`: an arrange shows the Prepare view; A's and B's squares apart and inside plate 0 |
| 5 | `undo` | as after 3 | as after 3 | A and B at +(128, 128): the arrange was one undo step |
| 6 | `object_arrange {plates: 0, spacing: 30, rotate: false, align_y: false}` | as after 3 | as after 3 | the reply's `settings` `spacing` 30, `rotate` false, `align_y` false, and `off_plate` `[]`; the gap between A's and B's squares at least 29.9; both inside plate 0, `rotation_deg` `[0, 0, 0]` |
| 7 | A and B back to `corner(0) + (128, 128, 0)` with `object_transform {position}`, then `object_arrange {}` | as after 3 | as after 3 | the reply's `settings` `spacing` 30: the arrange options panel kept it; the gap at least 29.9 |
| 8 | `object_arrange {plates: 0, spacing: 10}` | as after 3 | as after 3 | the reply's `settings` `spacing` 10; the gap at least 9.9 and under 29.9: the spacing decides how far apart they go |
| 9 | `plate_add`, then `object_arrange {plates: 1}` | `"" 2` · `"" 0 *` | as after 3 | refused, saying that plate 1 has no objects to arrange |
| 10 | A to `corner(0) + (60, 60, 0)` and B to `+ (180, 180, 0)` with `object_transform`, C at `plate: 1`, `position: corner(1) + (128, 128, 0)` | `"" 2` · `"" 1 *` | A@0 · B@0 · C@1 | A at +(60, 60), B at +(180, 180), C at +(128, 128) |
| 11 | `object_arrange {plates: 7}` | as after 10 | as after 10 | refused, saying `Plate 7 does not exist (the project has plates 0..1)`; the places as after 10 |
| 12 | `object_arrange {fill_object: 0, plates: 0}` | as after 10 | as after 10 | refused, saying `fill_object` fills the object's own plate; the places as after 10 |
| 13 | `object_set_instances {object: <C>, count: 3}`, then `object_arrange {plates: 1, spacing: 10, rotate: false}` | as after 10 | A@0 · B@0 · C@1×3 | the reply's `settings` `spacing` 10 and `rotate` false; C's three squares apart and inside plate 1, each `rotation_deg` `[0, 0, 0]`; A at +(60, 60), B at +(180, 180): arranging one plate leaves the other plates' objects |
| 14 | `object_arrange {plates: 0}` | `"" 2 *` · `"" 1` | as after 13 | the plate arranged is made current; A's and B's squares apart and inside plate 0; C's three inside plate 1 |
| 15 | A to +(60, 60) and B to +(180, 180) again, then `object_arrange {fill_object: <A>}` | as after 14 | A@0×n · B@0 · C@1×3, n at least 2 | the reply's `object` A's index, `filled_with` `instances`, `added` n − 1, `instances` n, `plate` 0, `off_plate` `[]`; B at +(180, 180): Fill bed adds to its object alone, and moves no other; every square on plate 0 apart from every other and inside the plate |
| 16 | `undo` | as after 14 | as after 13 | A at +(60, 60), B at +(180, 180): Fill bed was one undo step |
| 17 | `object_arrange {fill_object: <C>}` | `"" 2` · `"" 1 *` | A@0 · B@0 · C@1×m, m at least 4 | the reply's `plate` 1, `added` m − 3 and `instances` m: an object on another plate than the current one fills its own plate, which is made current; C's squares apart and inside plate 1; A at +(60, 60), B at +(180, 180) |

**Scenario: cloning**

cube is a 20 mm cube at `corner(0) + (60, 60, 0)`, added with nothing selected. The plate is `"" n *` throughout, n
the number of objects.

| # | Step | Objects after | Also checked |
|---|---|---|---|
| 1 | cube, then `object_clone {objects: [0], arrange: false}` | cube@0 · cube@0 | the reply's `objects` names cube at 1, `copied.objects` names cube at 0 and nothing else, `object_count` 2, `arranged` false; object 0 at +(60, 60): without an arrange the source stays; the app places the copy, its square apart from object 0's and inside plate 0; `selection()` objects `[1]` |
| 2 | `undo` | cube@0 | object 0 at +(60, 60) |
| 3 | `object_clone {objects: [0], count: 3, arrange: true}` | cube@0 four times | the reply's `objects` at 1, 2 and 3, `object_count` 4, `arranged` true; every square apart from every other and inside plate 0, none outside |
| 4 | `undo` | cube@0 four times | object 0 at +(60, 60): the arrangement is an undo step of its own |
| 5 | `undo` | cube@0 | object 0 at +(60, 60): the copies were one undo step |
| 6 | `view_set {camera: "iso"}`, `view_set {camera: "fit_objects"}`, `object_select {objects: [0]}`, `app_wait {for: "idle"}`, then a right click on object 0's top (`mouse_gesture`) picking `Clone`, and the dialog answered OK | cube@0 · cube@0 | the gesture's reply `ran` `Clone`; the waits are met; the dialog that opens is titled `Clone`; afterwards no dialog is open, and the two squares are apart and inside plate 0 |
| 7 | `object_set_instances {object: 0, count: 2}`, `object_select {instances: [[0, 1]]}`, then `object_clone {arrange: false}` | cube@0×2 · cube@0 · cube@0 | the reply's `copied.instances` `[{object: 0, instance: 1}]` and no `copied.objects`, `objects` naming cube at 2, `object_count` 3: a selected instance is copied as a new object of one instance |

**Scenario: leaving instances out of the print**

a is a 10 mm cube at `corner(0) + (60, 60, 0)` with three instances (`object_set_instances {object: 0, count: 3}`),
at +(60, 60), +(72.5, 72.5) and +(85, 85); b a 15 mm cube at `corner(0) + (180, 60, 0)`. A slice is
`plate_slice {plates: 0}`, which must reply `outcome` `ok`, then `project_export {format: "gcode", path}` into `W`.
After every step the objects are a@0×3 · b@0 at those places, and `plates()` is `"" 2 *`.

| # | Step | `printable` after | Also checked |
|---|---|---|---|
| 1 | a and b, then a slice | a true, true, true; b true | `gcode_prints()` prints a in 3 copies and b in 1 |
| 2 | `object_set_printable {instances: [[0, 1]], printable: false}` | a true, false, true; b true | the reply's `instances` `[{object: 0, instance: 1, printable: false}]` and no `objects` |
| 3 | a slice | as after 2 | `gcode_prints()` prints a in 2 copies and b in 1: an instance that is not printable is left out of the G-code |
| 4 | `undo` | as after 1 | Printable was one undo step |
| 5 | `object_set_printable {objects: [0], printable: false}` | a false, false, false; b true | the reply's `instances` instances 0, 1 and 2 of a, each `printable` false, and `objects` naming a: an object sets every instance of its own and no other's |

**Scenario: what the project keeps**

`plates()` is `"" 1 *` · `"" 0` after step 1, `"" 1` · `"" 1 *` after step 2, and `"" 1 *` · `"" 1` after step 3:
opening a project makes plate 0 current.

| # | Step | Objects after | Also checked |
|---|---|---|---|
| 1 | `plate_add`; kept, a 20 mm cube at `plate: 0`, `position: corner(0) + (60, 60, 0)`; `object_set_instances {object: 0, count: 2}`; `object_transform {object: 0, instance: 0, rotate: [0, 0, 30], mode: "absolute"}`; `object_transform {object: 0, instance: 1, scale: 1.5}`; `object_select {instances: [[0, 1]]}`, then `object_mirror {axis: "x"}`; `object_set_printable {instances: [[0, 1]], printable: false}` | kept@0×2 | instance 0 at +(60, 60), `mirrored` true, turned 30 degrees: `bbox.size` `[27.32, 27.32, 20]` to 0.01; instance 1 at +(72.5, 72.5), `scale` `[1.5, 1.5, 1.5]`, `mirrored` true, `printable` false: Mirror of the selected instance mirrors the object's other instance with it. From here instance 0's `rotation_deg` is not compared (see Not checked) |
| 2 | lifted, a 20 mm cube at `plate: 1`, `position: corner(1) + (60, 60, 0)`; `object_set_auto_drop {objects: [1], auto_drop: false}`; `object_transform {object: 1, translate: [0, 0, 15]}` | kept@0×2 · lifted@1 | lifted at +(60, 60), `auto_drop` false, `bbox.min` z 15 |
| 3 | `project_save` to a new file in `W`, `project_new`, then `project_open` of the file | as after 2 | every value as after 2: the places, turns, scales, mirrors, printable flags and Auto Drop of the instances are kept in the project file |

**To settle in review:**
- Scale to Fit (placing, steps 16 and 17). The fit is into the plate's free print volumes, the bed less its excluded
  area. The Klipper printer's bed has none, so its one volume is the whole bed, 250 mm wide, 250 mm deep and 250 mm
  high. The selection's box, grown by 0.01 mm on every side (20.02 mm), scales by the least of 250 / 20.02 on each
  axis, so s = 12.488, and its centre goes to the volume's centre, +(125, 125). The replay checked only s > 5.
- The spacing's effect (arranging, step 8). With spacing d each item is grown by d / 2, so two cubes end at least d
  apart; that they end less than 29.9 mm apart at spacing 10 rests on the arranger packing them as close as that lets
  it. Without that bound the step shows only that the spacing was taken.
- The plate an arrange of one plate or a Fill bed leaves current (arranging, steps 14 and 17): `Ops::start_arrange()`
  selects the plate it arranges, and `FillBedJob` the plate of the object it fills.
- The refusals of a count by the schema (instances, step 9) carry no code, so they are matched by their words; 1001
  passes the schema, which sets no maximum, and is refused by the operation as `bad_instance_count`. A count written
  as text, `" 2 "`, is read as the number by the argument reader.
- The order of the objects Set as Individual Objects makes of a whole object (instances, step 21): it takes the
  instances from the last down, so the new objects hold the last instance first.
- An object lifted with Auto Drop off reopens where it was saved, 15 mm above the bed (what the project keeps,
  step 3): the project file stores `auto_drop`, and opening a project puts its objects on the bed through
  `ModelObject::ensure_on_bed()`, which leaves an object with Auto Drop off where it is.
- The rotation Auto-Orient gives a cube on its edge (placing, step 14): x 90. That it lies on a face, z 20, holds whichever face it picks.
- Mirror of one instance (what the project keeps, step 1). `object_mirror` names objects and parts, and mirrors one
  instance as Mirror does, on the selection. The app's Mirror mirrors the other instances of the selected instance's
  object with it (`Selection::scale_and_translate()` synchronises the unselected instances, the app's own code), so
  both of kept's instances are mirrored.
- The names of a cut's halves (instances, step 22): a cut by a plane names both halves after the object
  (`Cut::perform_with_plane()`, the app's own code); only a cut along a contour or with a groove adds `_A` and `_B`.
- A scale that grows instance 1 (what the project keeps, step 1) leaves it resting on the bed: `object_transform`
  drops the instance it moved by that instance's own lowest point, as the Scale gizmo drops what it scaled
  (`GLCanvas3D::do_scale()`), not by instance 0's, which `ModelObject::ensure_on_bed()` measures.

**Not checked:**
- A person's pick of the Speed Dial actions of these commands (Center, Drop to Bed, Scale to Fit, Auto-Orient,
  Mirror, the instance actions, Set as Individual Objects, Auto-Arrange) and Add Primitive's shape actions with their
  size dialog: `test_every_action` checks each action's effect. This test calls the commands with no target on the
  selection, which for Center, Drop to Bed, Scale to Fit and Auto-Orient are the actions' own arguments (`{}`), and
  reads their results whole, the Auto-Orient rotations among them. The Set Number of Instances action is run here only
  to show its dialog cancelled.
- Auto-Arrange's reply, journal step and replay against its action, and `object_select {all: true}`:
  `test_person_forms`. An arrange from the assemble view and with the Move gizmo open: `test_operation_routes`. What a lock does to Arrange and Auto-Orient: `test_plates`.
- The refusals `test_menu_items_refusals` checks with the state after them: Set as Individual Objects on an object of
  one instance, Add Primitive's pick with an object selected, `object_mirror` with no axis,
  `object_center` and `object_add_primitive` on a plate the project lacks, a pick of Mirror with two objects selected,
  a pick of Drop to Bed on an object on the bed, and Auto-Orient of an empty plate or of objects that are not printable.
- What `rotate: true` does to an arrangement: which turn the arranger picks is its own. Only that `rotate: false` turns
  nothing is checked.
- Fill bed with copies (`fill_with: "copies"`) and Fill bed refused for a cut object: no replaced replay exercised them.
- A bed with an excluded area, where Scale to Fit fills the widest free part beside it and Arrange and Fill bed keep
  clear of it: the Klipper printer's bed has none.
- The rotations Auto-Orient gives the objects it may turn without a fixed result (placing, steps 14 and 15): which
  face a cube resting on one already ends on is the job's choice; step 15 checks them against the reply. The rotation
  of a shape both turned and mirrored (moving a part, part 1 from step 15; what the project keeps, kept's instance 0):
  a mirror of a turned shape can be written as another turn and a mirror, so only `mirrored` and the places are
  compared, and kept's instance 0's turn by its box. A mirrored shape that is not turned reads `rotation_deg`
  `[0, 0, 0]`, and is compared so (moving a part, step 16; what the project keeps, instance 1).
- How the objects look: a greyed instance that is not printable, the selection's highlight. No screenshot is taken.
- `app_state`'s `python` and `design_tab`: they are not object state.
  `test_command_line` checks the unrestricted-scripts switch.
- An end-to-end run of all the commands on one scene. Each of its steps is checked in the scenario of its command: the size
  of an object of several parts (moving a part, step 2), the instances of an object of parts made objects (the number
  of instances, step 18), one plate arranged with options (arranging, step 13), Assemble and ordering by name across
  plates (`test_object_structure`, splitting and ordering), Fill bed on another plate (arranging, step 17); its
  closing slice is replaced by the G-code checks of leaving instances out of the print.
- `object_list {screen: true}` and paging with `limit` and `offset`: no replaced replay used them, and they change how
  `object_list` reports the objects, not what the objects are.

**Strength:** Strong — after every step it reads the plates and every instance back through `plate_list` and
`object_list` and compares them whole with values written out here: names, plates, instance counts, places to
0.001 mm, turns, scales, mirrors, printable flags and Auto Drop. It asserts each refusal by its code or its words,
counts undo steps by undoing them, holds what the arranger and Scale to Fit work out to stated properties and
tolerances, reads the copies a print leaves out in the G-code, and checks what the project keeps through the file.

## `test_object_structure`
`test_object_structure.py` · Python, 509 checks · in `sweep.json` as `{"path": "test_object_structure.py"}`

**Tests:** What objects are made of and how the list orders them: renaming and deleting objects, and the layer commands
acting on the selected object; Split to Objects and to Parts, and Assemble; adding parts of every type, changing a
part's type and renaming a part; ordering a plate's objects by a property or in a given order, and the print order that
sets; Fix Model by index and through the object's menu; Reload from disk and Replace 3D file; and the keep_painting
preference, shown against its control. Each command is called by index and, where its menu item acts on the selection,
with no target on the selection. It runs on the Generic Klipper Printer 0.4 nozzle, whose by-object printing lets
objects lower than its rod (36 mm, `extruder_clearance_height_to_rod`) print in the object list's order when they
stand more than its clearance radius (65 mm, `extruder_clearance_radius`) apart.

**Starts from:** Every scenario starts with `start(client)` in the Prepare view and checks that `presets()` is
`{printer: KLIPPER, process: KLIPPER_STD, filaments: [GENERIC_PLA]}`, that `plates()` is `"" 0 *` and that there
are no objects; a `project_new` within a scenario is followed by the same check of the plates and objects. The one
stored setting a check depends on, the keep_painting preference, is set by the painting scenario before each step
whose result depends on it (painting, steps 2 and 4); no other step reloads or replaces a painted part.

**Leaves:** keep_painting on, as painting step 4 sets it. The Klipper printer selected, with one slot of
`GENERIC_PLA`. `keeps.3mf` at the top of the recent projects, and the folder the file dialogs open in, both in `W`,
which is removed when the test ends. On a data folder in Simple mode, Advanced mode, as `test_objects` leaves it: the
undo of parts, step 14, goes back to a step with a negative part.

**Files:** The test writes into a folder of its own, `W` (a `tempfile.TemporaryDirectory()`), with an ASCII STL writer
of its own: `cube20.stl` (a 20 mm cube from the origin, 12 triangles), `cube30.stl` (the same at 30 mm), `holed.stl`
(`cube20.stl` less one triangle, which leaves three open edges) and `top4.stl` (a 20 mm cube from the origin whose top
is four triangles about its centre, 14 triangles). It reads `tests/data/two_hollow_squares.obj` (two square rings
10.67 mm wide and 2.8 mm high, their centres 58 mm apart along x), `resources/handy_models/OrcaCube_v2.drc` and
`tools/mcp/tests/fixtures/star.svg`, `tools/mcp/tests/fixtures/heightmap.png`. The project it saves and the G-code it
exports go to `W` as well. It does not use `tests/fixtures/objects_files.py` or `tests/fixtures/ops_undo.py`: undo
steps are counted by undoing them and reading the state back.

**Reads:** `plates()`, and `scene()`, `selection()`, `gcode_prints(path)` and `corner(p)` as `test_objects` reads
them. Also:
- `names()`: the objects' names in index order; `types(o)`: object o's parts' `type`s in order; both from one
  `scene()`.
- `notices()`: the texts of `app_state`'s `notifications`; `dialogs()`: the titles of its `dialogs`.

Places are written as in `test_objects`, from plate 0's corner. An object the tables call by a name of its own is
added with `name` set to it; Cube is a shape's own name, and an imported object is named after its file. In the
tables the objects are written by their names in index order, and an object's parts by their types, after `:`
(`Assembly: part, modifier`). After every step `names()` is compared with its row whole, and the parts of every object
the row writes them for; a table without an objects column says in its introduction what the names are after every
step. `plates()` is `"" n *` after every step, n the number of objects, but where the ordering scenario says otherwise.

**Scenario: renaming and deleting**

Three 20 mm cubes are added with `object_add_primitive` at `corner(0) + (40, 60, 0)`, `+ (100, 60, 0)` and
`+ (160, 60, 0)`, with `object_select {none: true}` between them. Each is named Cube after its shape.

| # | Step | Objects after | Also checked |
|---|---|---|---|
| 1 | the three cubes | Cube · Cube · Cube | at +(40, 60), +(100, 60), +(160, 60) |
| 2 | `object_rename {object: 1, name: "middle"}` | Cube · middle · Cube | the reply's `objects` `[{index: 1, name: "middle"}]` |
| 3 | `object_delete {objects: [0]}` | middle · Cube | the reply's `objects` `[{index: 0, name: "Cube"}]` and `object_count` 2; middle at +(100, 60), Cube at +(160, 60): the objects after the deleted one move down |
| 4 | `undo` | Cube · middle · Cube | the delete was one undo step |
| 5 | `redo` | as after 3 | |
| 6 | `object_select {objects: [0]}`, then `object_delete {}` | Cube | the reply's `objects` `[{index: 0, name: "middle"}]` and `object_count` 1; Cube at +(160, 60) |
| 7 | `object_rename {object: 0, name: "a/b"}`, then `object_rename {object: 0, name: ""}` | Cube | refused, `invalid_name`, then `empty_name` |
| 8 | layered, a 20 mm cube at `+ (60, 160, 0)`, `object_select {objects: [1]}`, then `object_set_height_ranges {ranges: [{min_z: 0, max_z: 5}]}` | Cube · layered | the reply's `object` 1, `objects` `[{index: 1, name: "layered"}]` and `layer_config_ranges` one range, `min_z` 0 and `max_z` 5; `object_list` gives layered that range and Cube no `layer_config_ranges` |
| 9 | `object_set_variable_layer_height {action: "adaptive"}` | Cube · layered | the reply's `object` 1 and `objects` naming layered, with a `layer_height_profile`; `object_list` gives layered a `layer_height_profile` with `points` and Cube none |

**Scenario: splitting and assembling**

Each import is `project_import {paths: [two_hollow_squares.obj]}`, whose reply's `added` is 1; the object is named
after the file. A ring's box is `bbox.size` `[10.67, 10.67, 2.8]` to 0.01; two rings stand side by side when their
boxes' centres are 58 apart along x and level along y, to 0.01.

| # | Step | Objects after | Also checked |
|---|---|---|---|
| 1 | an import | two_hollow_squares.obj: part | its `bbox.size` `[68.67, 10.67, 2.8]` to 0.01 |
| 2 | `object_split {object: 0, into: "objects"}` | two_hollow_squares.obj_1: part · two_hollow_squares.obj_2: part | the reply's `objects` at 0 and 1 and `object_count` 2; each a ring's box, the two side by side; `selection()` objects `[0, 1]`: the pieces are selected |
| 3 | an import, then `object_split {object: 2, into: "parts"}` | as after 2 · two_hollow_squares.obj: part, part | the reply's `parts` `[{object: 2, part: 0}, {object: 2, part: 1}]` and `object_count` 3; the parts named `two_hollow_squares.obj_1` and `two_hollow_squares.obj_2`, in that order; each part a ring's box, the two side by side |
| 4 | `object_assemble {objects: [0, 1]}` | two_hollow_squares.obj: part, part · Assembly: part, part | the reply's `object` 1 and `name` `Assembly`: the two go and the assembly comes last; its `bbox.size` `[68.67, 10.67, 2.8]` to 0.01: the pieces keep their places |
| 5 | `part_change_type {parts: [[1, 0]], type: "modifier"}` | two_hollow_squares.obj: part, part · Assembly: part, modifier | the reply's `parts` `[{object: 1, part: 1, type: "modifier"}]`: parts are listed by type; Assembly's `bbox.size` `[10.67, 10.67, 2.8]`: a modifier is left out of the object's box |
| 6 | `undo` | as after 4 | Assembly's `bbox.size` x 68.67 again: the change was one undo step |
| 7 | `object_select {objects: [0]}`, then `object_split {into: "objects"}` | Assembly: part, part · two_hollow_squares.obj_1: part · two_hollow_squares.obj_2: part | no target splits the selected object, by its parts, each piece named after its part; the reply's `objects` at 1 and 2 and `object_count` 3; the two pieces' boxes rings', side by side |
| 8 | `object_select {objects: [1, 2]}`, then `object_assemble {}` | Assembly: part, part · Assembly: part, part | no target assembles the selected objects; the reply's `object` 1 and `name` `Assembly` |

**Scenario: parts**

block is a 40 × 40 × 10 mm cube at `corner(0) + (100, 100, 0)`. The positions are of the parts, from plate 0's corner,
to 0.001 unless a tolerance is given. The parts are of the scenario's one object: `names()` is `[block]` after steps 1
to 9, `[scaled]` after 10, `[plain]` after 11 to 20 and `[base]` after 21 and 22.

| # | Step | The object's parts after | Also checked |
|---|---|---|---|
| 1 | block, then `part_add {object: 0, type: "negative", shape: "cylinder", size: [6.2, 3.2], position: corner(0) + (100, 100, 1.6)}` | part, negative | the reply's `part` 1 and `part_count` 2; the negative part at +(100, 100, 1.6), `bbox.min` z 0 and `bbox.size` z 3.2 to 0.01, `bbox.size` x 6.1 to 6.21; block's `bbox.size` z 10: a negative part is left out of its box |
| 2 | `undo` | part | adding a part is one undo step |
| 3 | `redo` | part, negative | |
| 4 | `part_add {object: 0, type: "modifier", path: OrcaCube_v2.drc}` | part, negative, modifier | the reply's `part` 2 and `part_count` 3; the modifier named `OrcaCube_v2.drc` |
| 5 | `part_add {object: 0, type: "part", shape: "cube", size: [10, 10, 20], position: corner(0) + (125, 100, 10)}` | part, part, negative, modifier | the reply's `part` 1: parts are listed by type; the new part named `Generic-Cube`, `filament` 1, `bbox.size` z 20 to 0.01; block's `bbox.size` z 20 |
| 6 | `part_add {object: 0, type: "support_blocker", shape: "sphere", position: corner(0) + (110, 110, 10)}` | part, part, negative, modifier, support_blocker | the blocker at +(110, 110, 10) |
| 7 | `part_add {object: 0, type: "precise_seam", shape: "cube", size: [2, 2, 2]}` | part, part, negative, modifier, support_blocker, precise_seam | the seam's `subtype` `center` |
| 8 | `part_add` with `{type: "modifier", shape: "cube", path: OrcaCube_v2.drc}`, `{type: "modifier", shape: "cylinder", size: [1, 2, 3]}`, `{type: "part", path: star.svg}` and `{type: "modifier", path: heightmap.png}`, each on object 0 | as after 7 after each | refused, saying in turn `Give shape or path`, `A cylinder's size is [Diameter, Height]`, `SVG parts come from object_add_svg` and `The file could not be loaded` |
| 9 | `object_set_instances {object: 0, count: 2}`, then `part_add {object: 0, type: "modifier", shape: "cube"}` | as after 7 | refused, saying `object 0 has 2 instances` |
| 10 | `project_new`; scaled, a 20 mm cube, `object_transform {object: 0, scale: 1.5}`, then `part_add {object: 0, type: "modifier", shape: "cube"}` | part, modifier | the reply's `part` 1; the instance's `scale` `[1, 1, 1]` and part 0's `[1.5, 1.5, 1.5]`, and the object's `bbox.size` `[30, 30, 30]`: adding a shape folds the instance's scale into the parts |
| 11 | `project_new`; plain, a 30 × 30 × 10 mm cube, then `part_add {object: 0, type: "modifier", shape: "cube", size: [10, 10, 10]}` | part, modifier | part 0 named plain, part 1 `Generic-Cube` |
| 12 | `object_select {parts: [[0, 1]]}`, then `part_change_type {type: "negative"}` | part, negative | no target changes the selected part; the reply's `parts` `[{object: 0, part: 1, type: "negative"}]` |
| 13 | `part_change_type {parts: [[0, 1]], type: "support_blocker"}` | part, support_blocker | |
| 14 | `undo` | part, negative | each change is one undo step |
| 15 | `part_change_type {parts: [[0, 1]], type: "precise_seam"}` | part, precise_seam | `subtype` `center` |
| 16 | `part_change_type {parts: [[0, 1]], type: "part"}`, then `part_change_type {parts: [[0, 1]], type: "modifier"}` | part, part, then part, modifier | a seam made a part again is listed after the first part |
| 17 | `part_change_type {parts: [[0, 0]], type: "modifier"}` | part, modifier | refused, `last_solid_part` |
| 18 | `object_select {objects: [0]}`, then `part_change_type {type: "part"}` | part, modifier | refused, `wrong_selection`: an object's row is not a part |
| 19 | `object_select {objects: [0]}`, `object_add_svg {path: star.svg}`, `app_wait {for: "idle"}`, then `object_select {none: true}` | part, part, modifier | the met wait; part 1's `svg.path` ends in `star.svg`; no gizmo open afterwards |
| 20 | `part_change_type {parts: [[0, 1]], type: "support_enforcer"}` | part, part, modifier | refused, `text_part` |
| 21 | `object_select {objects: [0]}`, `object_rename {name: "base"}`, `object_select {parts: [[0, 2]]}`, then `object_rename {name: "infill zone"}` | part, part, modifier | no target renames the selected object, then the selected part: the object named base (it was plain); part 0 still named plain, as the object has three parts; part 2 named `infill zone` (it was `Generic-Cube`) |
| 22 | `object_rename {name: "a/b"}` | part, part, modifier | refused, `invalid_name`; the object named base, part 2 `infill zone` |

**Scenario: ordering**

tall, short and mid are 20 × 20 mm cubes 30, 10 and 20 mm high, added with `object_add_primitive` at
`corner(0) + (30, 60, 0)`, `+ (128, 60, 0)` and `+ (226, 60, 0)` with those names, nothing selected between them.
Each keeps its place throughout; an order reply is written as its names, every entry on plate 0 and its `object` the
entry's place in the list. `plates()` is `"" 3 *` after steps 1 to 6, `"" 3` · `"" 0 *` after steps 7 to 10, and
`"" 3 *` · `"" 0` from step 11: `plate_set_settings` makes the plate it sets current, as its Plate Settings dialog
works on the current plate.

| # | Step | Objects after | Also checked |
|---|---|---|---|
| 1 | the three cubes | tall · short · mid | their `bbox.size` z 30, 10 and 20 |
| 2 | `object_order {by: "height", plate: 0}` | short · mid · tall | the reply's `descending` false and `order` short, mid, tall |
| 3 | the same | short · mid · tall | `descending` false: a call that gives `plate` sorts ascending, whatever order they have |
| 4 | `object_order {by: "height", descending: true}` | tall · mid · short | `descending` true |
| 5 | `object_order {by: "height"}`, the Order By action's arguments | short · mid · tall | `descending` false: from a descending order it sorts ascending |
| 6 | the same | tall · mid · short | `descending` true: an order they already have is reversed |
| 7 | `plate_add`, `plate_select {plate: 1}`, then `object_order {by: "size"}` | tall · mid · short | refused, `no_objects_to_order`: the current plate is empty |
| 8 | `object_order {by: "size", all: "every"}` | tall · mid · short | refused by the schema, saying `all must be of type boolean` |
| 9 | `object_order {by: "size", all: true}` | short · mid · tall | the reply's `descending` false and `order` short, mid, tall and no more: every plate's objects, by volume (4000, 8000, 12000 mm³) |
| 10 | `object_order {by: "name", all: true}` | mid · short · tall | the reply's `descending` false and `order` mid, short, tall: with the current plate empty, `all` orders plate 0's objects, by name |
| 11 | `plate_set_settings {plate: 0, values: {print_sequence: "by object"}}` | mid · short · tall | `plate_list` gives plate 0 `print_sequence` `by object` |
| 12 | `object_order {plate: 0, order: [2, 0, 1]}` | tall · mid · short | the reply's `order` tall, mid, short and no `descending` |
| 13 | `object_order {plate: 0, order: [0, 1, 2]}` | tall · mid · short | the reply's `order` tall, mid, short: the order they have changes nothing |
| 14 | `undo` | mid · short · tall | step 12 was one undo step and step 13 took none |
| 15 | `redo` | tall · mid · short | |
| 16 | `plate_slice {plates: 0}`, then `project_export {format: "gcode", plates: 0, path}` into `W` | tall · mid · short | `outcome` `ok`; `gcode_prints()` prints tall, mid, short, in that order: the list's order is the print order |

**Scenario: repairing a mesh**

`names()` is `[holed.stl]` after steps 1 to 4 and `[holed.stl, holed.stl]` after 5 and 6; object 0 at +(60, 60)
throughout, object 1 at +(128, 128) from step 5.

| # | Step | `open_edges` after | Also checked |
|---|---|---|---|
| 1 | `project_import {paths: [holed.stl]}`, then `object_transform {object: 0, position: corner(0) + (60, 60, 0)}` | 3 | `mesh_errors.manifold` false |
| 2 | `object_fix {objects: [0]}` | 0 | the reply has no `status`: the call waited for the repair; its `objects` names object 0 and its `repairs` one entry, `object` 0, `changed` true, `mesh_errors_before.open_edges` 3, `mesh_errors_after.open_edges` 0, `painting_removed` false; `mesh_errors.manifold` true; `dialogs()` empty, and no notice holds any of the endings of Fix Model's notices (`has been repaired`, `have been repaired`, `needed no repair`, `Failed to repair`, `Repairing was canceled`) |
| 3 | `undo` | 3 | the repair was one undo step |
| 4 | `redo` | 0 | |
| 5 | another import of `holed.stl`, `object_transform {object: 1, position: corner(0) + (128, 128, 0)}`, `view_set {camera: "iso"}`, `view_set {camera: "fit_objects"}`, `object_select {objects: [1]}`, `app_wait {for: "idle"}`, then a right click on object 1's top picking `Modify>Fix Model` | object 0: 0; object 1: 0 | the gesture's reply `ran` `Modify>Fix Model`; a wait of up to 30 s, which fails when it runs out, for object 1's open edges to be 0 and for a notice holding `has been repaired`: the person's path ends in its notice, which shows the reader of step 2 can see one |
| 6 | `undo` | object 0: 0; object 1: 3 | the menu item's repair was one undo step |

**Scenario: the files a part comes from**

`names()` is `[reload.stl]` after steps 1 to 6 and `[cube20.stl]` after 7 to 11, the object at +(100, 100)
throughout; it has one part after steps 1 to 6 and two after 7 to 11.

| # | Step | Parts after | Also checked |
|---|---|---|---|
| 1 | `cube20.stl` copied to `W/reload.stl`, `project_import` of it, `object_transform {object: 0, position: corner(0) + (100, 100, 0)}` | part 0 `mesh_volume` 8000 | |
| 2 | `cube30.stl` copied over `reload.stl`, then `object_reload_from_disk {objects: [0]}` | part 0 `mesh_volume` 27000 | the reply's `reloaded` `[{object: 0, part: 0, path}]`, the path ending in `reload.stl`, `missing` and `unmatched` `[]`, and no `unreadable`; the object rests on the bed |
| 3 | `undo` | part 0 `mesh_volume` 8000 | the reload was one undo step |
| 4 | `redo` | part 0 `mesh_volume` 27000 | |
| 5 | `reload.stl` deleted, then `object_reload_from_disk {objects: [0]}` | as after 4 | the reply's `missing` `[{object: 0, part: 0, path}]`, the path ending in `reload.stl`, and `reloaded` `[]`; `dialogs()` empty: no file dialog asks for it |
| 6 | `undo` | part 0 `mesh_volume` 8000 | the call that reloaded nothing took no undo step, so undo takes back step 4's |
| 7 | `project_new`; `filament_add`; `project_import` of `W/cube20.stl`, moved with `object_transform {object: 0, position: corner(0) + (100, 100, 0)}`; `part_add {object: 0, type: "modifier", shape: "cylinder", size: [10, 10], position: corner(0) + (100, 100, 10)}`; `settings_set {type: "process", target: "part", object: 0, part: 0, values: {extruder: 2}}` | part 0: part, `filament` 2, 12 `triangles`, at +(100, 100, 10); part 1: modifier, no `filament`, `mesh_volume` under 785.4, at +(100, 100, 10) | |
| 8 | `part_replace {object: 0, part: 0, path: W/top4.stl}` | part 0: part, `filament` 2, 14 `triangles`, `mesh_volume` 8000, at +(100, 100, 10); part 1 as after 7 | the reply's `parts` `[{object: 0, part: 0}]` named `top4.stl`, with a `part_id` other than the one part 0 had and the one `object_list` gives it: the part keeps its type, filament and place, its mesh the file's |
| 9 | `part_replace {object: 0, part: 1, path: W/top4.stl}` | part 1: modifier, no `filament`, 14 `triangles`, `mesh_volume` 8000, at +(110, 110, 15) to 0.05 | the mesh keeps its place in its file: the cylinder's centre was at (0, 0, 5) in its own frame and the cube's is at (10, 10, 10) in `top4.stl`'s, so the modifier moves by (10, 10, 5) |
| 10 | `undo` | part 1 as after 7 | |
| 11 | `undo` | part 0: 12 `triangles` | each replace was one undo step |

**Scenario: painting kept through a mesh change**

The supports are painted on a cube's front face as a person paints them: `view_set {camera: "front"}`, `view_set
{camera: "fit_objects"}`, `object_select {objects: [0]}`, `app_wait {for: "idle"}`, then a click on the Paint-on
supports gizmo (`widget_click {gizmo: "FdmSupports"}`, whose reply's `active` is true), on its `Fill` tool, at the
part's front (`mouse_gesture`), and on `Done`. The preference is set with `preferences_set`, and the test leaves it
as step 4 sets it. The object has one part throughout; `names()` is `[painted.stl]` after steps 1 to 4 and
`[top4.stl]` after 5, as replacing an object's only part names the object after the new file
(`replace_part_with_file()`).

| # | Step | part 0's painted enforcers after | Also checked |
|---|---|---|---|
| 1 | `project_import` of `W/cube20.stl` copied to `W/painted.stl`, then the painting | more than 0 | no gizmo open afterwards |
| 2 | `preferences_set {values: {keep_painting: false}}`, then `object_reload_from_disk {objects: [0]}` | none: no `painted` | the reply's `reloaded` names part 0; `preferences_get` gives `keep_painting` false: with the preference off a reload drops the painting |
| 3 | `undo` | more than 0 | |
| 4 | `preferences_set {values: {keep_painting: true}}`, then `object_reload_from_disk {objects: [0]}` | more than 0 | `preferences_get` gives `keep_painting` true: with it on the same call keeps the painting |
| 5 | `part_replace {object: 0, part: 0, path: W/top4.stl}` | more than 0 | part 0's `triangles` 14: with the preference on, the painting is carried onto a mesh of other triangles |

**Scenario: what the project keeps**

| # | Step | Objects after | Also checked |
|---|---|---|---|
| 1 | base, a 30 × 30 × 10 mm cube at `corner(0) + (60, 60, 0)`; `part_add {object: 0, type: "negative", shape: "cylinder", size: [6, 3], position: corner(0) + (60, 60, 1.5)}`; `part_add {object: 0, type: "modifier", shape: "cube", size: [10, 10, 10], position: corner(0) + (60, 60, 5)}`; `part_add {object: 0, type: "precise_seam", shape: "cube", size: [2, 2, 2], position: corner(0) + (75, 60, 5)}`; `object_rename {object: 0, part: 2, name: "infill zone"}`; `part_change_type {parts: [[0, 3]], type: "precise_seam", subtype: "left"}`; other, a 20 mm cube at `+ (180, 180, 0)`; `object_order {plate: 0, order: [1, 0]}` | other: part · base: part, negative, modifier, precise_seam | base's parts named `base`, `Generic-Cylinder`, `infill zone`, `Generic-Cube`, at +(60, 60, 5), +(60, 60, 1.5), +(60, 60, 5), +(75, 60, 5); the seam's `subtype` `left` |
| 2 | `project_save` to a new file in `W`, `project_new`, then `project_open` of the file | as after 1 | every value as after 1: names, part types, the seam's subtype, the parts' places and the order are kept in the project file |

**To settle in review:**
- The names a split gives (splitting, steps 2, 3 and 7). Split to Objects of an object of one part names the pieces
  after the object with `_1` and `_2` in the order it adds them (`ModelObject::split()`); Split to Parts names the
  parts after the part the same way (`ModelVolume::split()`); Split to Objects of an object of several parts names each
  piece after its part. The object is named after the OBJ file it came from, whose path the test gives in the system's
  own form (`os.path.normpath`), as the loader takes the name after its last separator. Which ring is `_1` is not
  fixed, so the rings are checked as a pair by their boxes.
- That Assemble keeps the pieces where they stand (splitting, step 4), so the assembly's box is the two rings' box:
  `ObjectList::merge()` gives each part its object's instance placement times its own.
- The open edges of `holed.stl` once imported (repairing, step 1): the loader's repair matches the edges it can and
  leaves the hole, three edges.
- Fix Model's notice through the menu (repairing, step 5) is matched by the words `has been repaired`.
- The modifier's move on Replace 3D file (files, step 9). The new mesh is moved by the difference between the two
  meshes' centres in their files, (10, 10, 5) here, for an instance that is neither turned nor scaled. The cylinder's
  centre is (0, 0, 5) only to its facets, hence 0.05.
- A reload that finds only missing files takes no undo step (files, step 6): `Ops::reload_parts()` reloads nothing,
  so it takes no snapshot. `object_reload_from_disk`'s description (`register_object_tools()` in
  `src/slic3r/GUI/Mcp/McpToolsScene.cpp`) and the comment on `reload_parts()` in
  `src/slic3r/GUI/Operations/ObjectOps.hpp` say so: one undo step, none when every part's file is missing.
- An order the objects already have takes no undo step (ordering, steps 13 and 14): `Ops::reorder_objects()` takes its
  snapshot only when it moves an object, as `ObjectList::order_objects()`, the app's own code, takes one only when the
  order changes. `object_order`'s description (`register_object_menu_commands()` in
  `src/slic3r/GUI/Mcp/McpToolsScene.cpp`) and the comments on `order_objects()` and `reorder_objects()` in
  `src/slic3r/GUI/Operations/ObjectOps.hpp` say so: one undo step, none when the order stays as it was.
- The words of `part_add`'s refusals (parts, steps 8 and 9) carry no code, so they are matched by their text. A
  shape's size names the fields of Add Primitive's size dialog (`primitive_size_names()`), so a cylinder's is
  `[Diameter, Height]`.
- The names of parts (parts, steps 11 and 21; what the project keeps, step 1): a part added as a shape is named
  `Generic-` and its shape (`ObjectList::load_generic_subobject()`), and naming an object of one part names its part
  too (`ObjectList::update_name_in_model()`), so the first part is named after the object while it has no other.

**Not checked:**
- The refusals `test_menu_items_refusals` checks with the state after them: Delete of an object's last solid part and
  of a cut object, Split of a mesh of one shell and a split that would ask the Auto-Drop question, the picks of Split,
  Assemble and Change Type on rows they refuse, Rename with two objects or an instance selected.
  `test_additions_refusals` checks an `object_order` that leaves an object out.
- The Speed Dial actions of these commands (Delete, Delete All, Split to Objects and to Parts, Assemble, the Change Type
  actions, Rename, the four Order By actions): `test_every_action` checks each action's effect. This test checks the
  commands' forms with no target on the selection, and the Order By action's arguments as a call.
- Deleting instances, parts, a plate's objects (`plate`) and every object (`all`): no replaced replay deleted them but
  by index and on the selection; the parts are deleted in `test_menu_items_refusals`.
- The layer heights a height range or an adaptive profile gives: the layers entry checks them. Here only which object
  the selection names.
- What `part_replace` does with keep_painting off. It then copies the painting by triangle (`replace_part_with_file()`
  in `Plater.cpp`), so on a mesh of other triangles the painted count stays and only where the painting lies changes,
  which no reader gives; on the same mesh off and on give the same painting. The preference's effect is shown on
  Reload from disk, which drops the painting with it off (painting, steps 2 and 4).
- Painting other than supports under keep_painting (seam, colour, fuzzy skin): `ModelVolume::save_painting()` carries
  every kind together, so the supports stand for them. keep_painting with Fix Model: no replaced replay repaired a
  painted mesh; `ModelVolume::repair()` keeps painting through the same `save_painting()`.
- Fix Model by plate or `all`, and its cancel: no replaced replay used them, and a cancel needs a repair slow enough to
  stop, which a 12-triangle cube is not. Reload from disk with `all`: no replaced replay used it; it reloads every
  object but the cut ones through the same `Ops::reload_parts()`. A STEP file's mesh options on Replace 3D file:
  `test_files` (the files entry) checks them.
- The order of objects of the same height, such as two rings beside cubes:
  which of two equal objects comes first is the sort's. The ordering scenario's objects differ in height, size and
  name, so the whole order is read at every step.

**Strength:** Strong — after every step it reads the objects back through `object_list` and compares their names, and
their parts' types where a row writes them, whole with values written out here; it asserts each refusal by its code or
its words, counts undo steps by undoing them, checks the print order in the G-code and what the project keeps through
the file, and shows keep_painting's effect against the preference turned off.
