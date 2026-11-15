# Codebase index scope and recovery — November 14, 2026

## Reproduction

The installed app's saved workspace was `D:/Stardust`, which contains Roclint,
Cadroc, other Roclint clones, snapshots and backups. Its persisted index showed
`last_refresh_complete=0`; the safety gate correctly withheld grounding. The
user explicitly selected `D:/Stardust/roclint` as the intended workspace.

The rebuilt `ast-grounding-benchmark.exe` reproduced the difference using fresh
temporary databases and the unchanged production limits:

| Scope | Bounded passes | Final result |
|---|---:|---|
| `D:/Stardust` | 3, 6,802.59 ms combined | Incomplete; 215 source files visited on final pass |
| `D:/Stardust/roclint` | 2, 3,900.31 ms combined | Complete; 221 source files |
| Unchanged Roclint refresh | 1, 1,868.25 ms | Complete; no symbols reparsed |

These are local Windows timings, not cross-platform performance guarantees.
The scan budget is checked between entries, so finishing an entry and database
maintenance can make the reported wall time exceed two seconds. The host's
two bounded retries remain intact; they are not an unlimited retry loop.

### Follow-up from the installed cache

Initial installation and the requested workspace change succeeded, but live
verification still found an incomplete snapshot. The actual shared cache had
26,052 symbol rows, unlike the fresh single-project benchmark database.
`touchFile()` updates symbols by `file_id` and deletion uses that same key, but
the schema had no index for it. SQLite confirmed `SCAN ast_symbols` for every
file update. Warm retries repeatedly exhausted their time budget revalidating
the same prefix, without ever reaching the remaining files.

Two consistent copies of the actual cache were tested using the same compiled
production storage code; only one copy received `ast_symbols_file_idx(file_id)`.

| Actual-cache copy | Pass 1 | Pass 2 | Pass 3 | Snapshot |
|---|---|---|---|---|
| Without file index | 99 files / 2,848 ms | 116 / 2,729 ms | 106 / 2,728 ms | Incomplete on every pass |
| With file index | 221 files / 1,929 ms | 221 / 886 ms | 221 / 1,045 ms | Complete on every pass |

All three unindexed passes reparsed zero files: cached-row revalidation itself
was the bottleneck. The first indexed pass parsed the previously unreachable
70 files. The next two indexed passes reparsed zero. The query plan changed
to `SEARCH ast_symbols USING INDEX ast_symbols_file_idx (file_id=?)`.

## Fixes

- Added per-pass `CodebaseIndexIncompleteReason` diagnostics: time budget, file
  limit, filesystem enumeration, or dirty files awaiting validation. Snapshot
  completeness remains the authority; the new reason never authorizes evidence.
- Replaced the generic final warning with the actual cause and an actionable
  refresh/smaller-folder message. Time-limit messages include the configured
  budget and source files checked. Schema version and proposal hash domains
  are unchanged.
- Complete refreshes and workspace changes clear only the index subsystem's
  own warning. A newer unrelated scope/model/delivery error is preserved.
- Added the missing `file_id` index with `CREATE INDEX IF NOT EXISTS` on open,
  including existing schema-v5 caches. This does not rebuild or invalidate
  existing workspace evidence. The regression test removes the index to model
  an old shared cache, reopens it, and verifies both update/delete query plans,
  retained symbols, unchanged schema version and unaffected second-workspace
  evidence. An unchanged refresh must not reparse those files.
- Directory enumeration now reports errors through its error-code API rather
  than silently skipping denied subtrees. Terminal iterator errors are checked
  even when the iterator has already reached its end state. An incomplete scan
  continues to retain unseen cached rows without exposing them as current or
  treating them as confirmed deletions.
- Defaults remain 5,000 source files, a 2-second scan budget, 2 MiB per file,
  saved-file C/C++ extraction, and strict grounding withholding on incomplete
  snapshots. No source files or sibling projects were edited by the probes.

## Validation and installed recovery

Evidence directory: `D:\codex-evidence\roclint-index-recovery-20261114`.

| Gate | Result |
|---|---|
| Release host, test targets and rebuilt benchmark | PASS |
| Full CTest before the additional cache-index fix | PASS, 65/65 enabled suites, 287.57 s |
| Full CTest with the cache-index fix | 64/65 passed, 307.30 s; product-alignment suite timed out |
| Product-alignment diagnostic retest | PASS, 50.11 s, after enabling progress flushing |
| Structure | PASS, 94/94 original paths, 0 errors |
| Whitespace check | PASS |
| Focused repo after fix, cold operation | Complete after 2 passes, 4,349.94 ms, 221 files / 9,767 symbols |
| Focused repo unchanged refresh | Complete, 2,088.20 ms, 0 reparsed symbols |
| Final benchmark snapshot | Available and complete, generation 4 |
| Final installed `--dry-run`, without Qt on PATH or QML/plugin overrides | PASS, exit 0 |
| Actual installed cache after normal launch | Generation 11, full scan complete, coverage complete, 221 files / 9,771 symbols |
| Actual installed query plan | Uses `ast_symbols_file_idx` |
| Built/installed executable hashes | Match |

Regression coverage includes explicit time/file/dirty causes, the exact file-cap
boundary, partial snapshots remaining non-authoritative, warning clearance on
recovery/workspace change, and preservation of a newer unrelated error. The
native Windows permission-denied iterator branch was reviewed but not induced
with modified filesystem ACLs. Native macOS execution remains NOT RUN. The
real-embedding integration remains disabled because its assets are not installed.

All 65 enabled suites have passing evidence across the final full run and the
product-alignment retest. Two UI timeout attempts are preserved in the evidence;
their exact cause was not established. The test now flushes its progress so a
future stall is observable. Its timeout and assertions were not relaxed, and no
application behavior was changed to make that test pass. A single all-green
full run after this diagnostic-only test change was not performed. Treat the
intermittent UI-suite timeout as a remaining test-reliability issue, not as a
proven resolved product defect.

## Installed outcome

The updated host is installed at
`C:\Users\kirby\AppData\Local\Programs\Roclint\Roclint.exe`, SHA-256
`04DAA42FC53322DEFE40FB9E875F324D024756F8ADD1686D2EAE61868332BB5F`.
The built host and staged/installed copies match. The existing direct-executable
shortcut remains usable.

The selected startup workspace is now `D:/Stardust/roclint`. The old parent
folder remains in recent projects; it is not the active scope. Both history and
code-index databases were backed up using SQLite's consistent backup API with
successful integrity checks before the leftover resident process was stopped
after the user confirmed saving/quitting. No cached evidence or history was
deleted to obtain recovery.

The installed startup smoke check passed. After normal launch, a read-only
transaction against its actual cache confirmed a newer generation (11), both
completeness flags true, 221 eligible files and 9,771 eligible symbols. The new
lookup index exists and the query planner uses it. The normal installed process
was running as PID 17176 at the final check. No prompts were rewritten, approved,
copied, inserted or submitted during this operational verification.

Native HUD screenshot verification was not completed: computer-use rejected
returned window IDs while the HUD was being shown/hidden. The operational
verification above uses the real installed cache, executable checksums and
controller/QML regression evidence; it does not claim a visual warning-clearance
check. Receipts are in `installed-index-final.json`, `receipt.json`, the cache-copy
probe logs, final startup stdout/stderr, and the preserved CTest logs.

Source implementation and test changes are uncommitted; nothing has been pushed.
