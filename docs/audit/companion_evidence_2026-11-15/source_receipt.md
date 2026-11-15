# Source receipt — 2026-11-15 companion measurement

All commands were run in `D:/Stardust/roclint-wt-eval` on branch
`topic/eval-2026-11-15`.

```
$ git rev-parse HEAD
d5c8101e42b45d5d3b0796c97f01024214fd5d57

$ git -C D:/Stardust/roclint rev-parse HEAD
d5c8101e42b45d5d3b0796c97f01024214fd5d57

$ git status --porcelain
?? docs/audit/companion_evidence_2026-11-15/
?? docs/audit/companion_quality_measurement_2026-11-15.md
?? tests/fixtures/
?? tools/companion_eval/
?? tools/companion_probe/

$ git status --porcelain -- src include app CMakeLists.txt tests/bridge tests/viewmodel
(no output)

$ git diff --stat
(no output)
```

The worktree is dirty only because this mission's new evaluation files are
untracked. No production source, existing test, CMake file, model setting or
installed application was modified. The probe was linked against the main
checkout's existing Release libraries read-only.
