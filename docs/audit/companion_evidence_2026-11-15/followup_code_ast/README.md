# Code AST follow-up — PARTIAL

This follow-up was created because the primary code row's `getProjectFiles` idle
check returned quickly and did not prove codebase-index readiness.

Observed:

```
t=180515 project_indexed idle=True ready=False
t=180563 analysis_posted posted=True
```

`HostController::codebaseIndexReadyForTesting()` remained false for the full
180 s wait. The probe then started Julia analysis. The harness server restarted
during Julia load, so no question/rewrite result exists for this run.

Consequence: this baseline does **not** establish AST-grounded behavior for the
code scenario. The primary code row is a text-level target/preservation result
only. A later run should use the production index/consent path (or a documented
test seam that actually publishes readiness) and retain
`codebaseIndexReadyForTesting`, `groundingStatus`, `groundedEvidence` and the
attempt's workspace/AST fields. Do not treat this partial file as a model
result.
