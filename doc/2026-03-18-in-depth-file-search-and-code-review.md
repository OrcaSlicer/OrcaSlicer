# In-Depth File Search and Code Review (2026-03-18)

## Scope

This review focused on first-party OrcaSlicer files and intentionally excluded vendored directories:

- Excluded: `deps/`, `deps_src/`, `.git/`, `build/`
- Primary areas searched: `src/`, `tests/`, `scripts/`, `cmake/`, `doc/`, root documentation

## File Search Snapshot

The repository currently contains a very large number of tracked assets and web artifacts, so this snapshot is useful for prioritizing future audits:

- First-party file count (with vendored trees excluded): **14,816**
- Markdown files (same exclusions): **21**
- Most common file suffixes:
  - `.json`: 10,059
  - `.svg`: 1,003
  - `.hpp`: 845
  - `.png`: 786
  - `.cpp`: 757

## Code Review Findings

### 1) Duplicate-automation scripts do not explicitly exclude pull requests

**Severity:** Medium  
**Files:** `scripts/auto-close-duplicates.ts`, `scripts/backfill-duplicate-comments.ts`

Both scripts query GitHub via `/issues` endpoints, which return both issues and pull requests unless PR entries are explicitly filtered out in code. The current issue interfaces do not include a PR marker and there is no guard before processing. This can lead to unexpected automation behavior on PRs if the heuristics match.  

Relevant code:

- Issue listing without PR filtering in the auto-close script.
- Issue listing without PR filtering in the backfill script.

### 2) Auto-close flow may unintentionally overwrite existing labels

**Severity:** Medium  
**File:** `scripts/auto-close-duplicates.ts`

When closing a duplicate, the script sends a `PATCH` payload with `labels: ['duplicate']`. Depending on GitHub API semantics for issue update, this can replace the entire label set with only `duplicate`, potentially dropping triage or workflow labels that should be preserved.

### 3) Backfill script hard-codes repository owner/repo values

**Severity:** Low  
**File:** `scripts/backfill-duplicate-comments.ts`

Unlike the auto-close script, the backfill script does not honor `GITHUB_REPOSITORY_OWNER` or `GITHUB_REPOSITORY_NAME` and always targets `OrcaSlicer/OrcaSlicer`. This reduces portability for forks and dry-run validation in test repos.

## Suggested Follow-Ups

1. Add PR filtering:
   - Extend issue type to include optional `pull_request` field.
   - Skip entries where `pull_request` is present.
2. Preserve labels safely:
   - Fetch current labels and append `duplicate` if missing, or use dedicated labels API with merge behavior.
3. Normalize repo targeting:
   - Use environment-driven owner/repo in both scripts consistently.

## Reproduction Commands Used in This Review

```bash
rg --files -g 'AGENTS.md'
rg --files -g '*.md' | head -n 200
rg -n "repos/.*/issues\\?" scripts/*.ts
sed -n '1,260p' scripts/auto-close-duplicates.ts
sed -n '1,260p' scripts/backfill-duplicate-comments.ts
python - <<'PY'
from pathlib import Path
from collections import Counter
root=Path('.')
exclude={'.git','build','deps','deps_src'}
files=[]
for p in root.rglob('*'):
    if p.is_file() and not (set(p.parts) & exclude):
        files.append(p)
print('file_count',len(files))
ext=Counter(p.suffix.lower() or '<no_ext>' for p in files)
for k,v in ext.most_common(15):
    print(k,v)
print('md_count',len([p for p in files if p.suffix.lower()=='.md']))
PY
```
