# Profile authoring guide — `setting_id`

Every **instantiated** system preset (filament / process / machine) carries a
`setting_id`. These must be **globally unique** and live inside their vendor's
own prefix namespace. This page explains what to do when you add a new vendor or
add profiles to an existing vendor.

> **TL;DR** — don't hand-pick `setting_id`s. Add your profiles (you can leave
> `setting_id` out entirely), then run:
>
> ```bash
> python3 scripts/assign_vendor_setting_ids.py
> ```
>
> It assigns the correct ids, registers a prefix for new vendors, and never
> renumbers existing presets. Then bump your vendor's `version` and run the
> checker.

## How ids are namespaced

- **Bambu (`BBL`)** owns the `G*` id space and **OrcaFilamentLibrary** owns the
  `O*` space. These are reserved — never copy their ids.
- Every other vendor gets a **2-letter prefix** (first + last letter of the
  vendor name, collision-resolved), recorded in
  [`vendor_prefixes.json`](vendor_prefixes.json). Its presets are numbered
  `<PREFIX><NNNN>`, e.g. `QI0001`, `EO0326`.
- `filament_id` is a **different field** (a per-material id shared across a
  filament's nozzle variants). It is **not** managed by this workflow — leave it
  as authored.

## Adding a NEW vendor

1. Create the `resources/profiles/<Vendor>/` tree and the
   `resources/profiles/<Vendor>.json` index as usual.
2. In your instantiated presets you may leave `setting_id` **out** — the script
   fills it in. (If you copied another profile, delete any inherited
   `setting_id` value; don't keep a Bambu `G*` id.)
3. Run `python3 scripts/assign_vendor_setting_ids.py`. It will:
   - pick a free 2-letter prefix for your vendor and add it to
     `vendor_prefixes.json`, and
   - assign `<PREFIX>0001`, `0002`, … to every instantiated preset.
4. Bump `"version"` in `<Vendor>.json` (increment the last component, e.g.
   `02.04.00.00` → `02.04.00.01`).

> A brand-new vendor that already has its **own** clean, unique, non-`G*` ids is
> "grandfathered" and left as-is (no prefix entry). If you're not sure, just run
> the script.

## Adding profiles to an EXISTING vendor

1. Add your new profile files. Leave `setting_id` out (or, if present, it will be
   normalized).
2. Run `python3 scripts/assign_vendor_setting_ids.py`. Existing ids are
   **frozen** — only your new presets receive the next free numbers in the
   vendor's namespace. Nothing else changes.
3. Bump `"version"` in the vendor's `<Vendor>.json`.

## Rules (enforced by CI)

`scripts/orca_extra_profile_check.py` (run automatically by
[`.github/workflows/check_profiles.yml`](../../.github/workflows/check_profiles.yml))
will fail the build if any of these are violated:

1. **Unique** — a `setting_id` used by a non-reserved vendor must not appear in
   any other file.
2. **In-namespace** — a registered vendor's `setting_id`s must start with its
   prefix from `vendor_prefixes.json`.
3. **No id on base profiles** — a base/template profile (`"instantiation"` is
   not `"true"`) must **not** have a `setting_id`.
4. **No gaps** — in a registered vendor, every instantiated preset must have a
   `setting_id`.
5. **No typos** — the key is `setting_id`, never `settings_id`.

Run it locally before pushing:

```bash
python3 scripts/orca_extra_profile_check.py
```

## Notes

- The migration is **one-time and idempotent**: re-running over an unchanged
  tree produces no diff, and it never renumbers an already-valid preset — so it
  is always safe to run.
- Do not edit `vendor_prefixes.json` by hand; let the script maintain it.
- Base/template profiles intentionally have **no** `setting_id`; only
  user-selectable (`"instantiation": "true"`) presets get one.
