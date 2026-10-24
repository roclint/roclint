# C07 — Telemetry and study contract

## Hierarchy

```text
ParticipantCode (pseudonymous)
  ↓
Study session
  ↓
StudyAttempt / trial (assigned condition + trial index/block)
  ↓
Preparation
  ↓
ExecutionRequest (exact final text/hash actually sent)
  ↓
ModelAttempt(s)
  ↓
TaskOutcome
```

`attempt_id` is reserved for provider/model attempts. Task outcomes reference `study_attempt_id`.

## ExecutionRequest

Every downstream execution persists an immutable artifact identifying exact bytes/text, hash, assigned condition, source revision/intent/proposal when applicable, downstream model/config, and creation time.

## Assigned vs realized path

`assigned_condition` never changes after task start. Provider outages remain in that denominator. `realized_path` and failure/degradation fields describe what actually occurred.

## Protocol status vs task quality

Workflow terminal status uses values such as `COMPLETED`, `ABANDONED`, `INTERRUPTED`, `TIMED_OUT`; task success is scored separately.

## Generic clarification fairness

Clarification/preparation produces one exact final ExecutionRequest. **All conditions execute in a fresh downstream context** with only that final request and the same downstream configuration. This avoids giving one condition extra hidden conversation context.

## Participants

Store pseudonymous `participant_code`, `trial_index`, and sequence/block ID sufficient to inspect order/learning effects. Real recruitment identity stays outside the research DB.
