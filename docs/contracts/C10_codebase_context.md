# C10 — Codebase context & session working memory contract

**Authority:** Local desktop runtime / SQLite indexer.  
**Classification:** Advisory context provider; strictly non-authoritative over C01 core state.

---

## 1. Required Concepts

* `WorkspaceContext`:
  * `workspace_root`: absolute local filesystem path.
  * `active_file`: optional relative path to the currently focused editor file.
  * `detection_source`: `WINDOW_TITLE`, `LOCKFILE_HEURISTIC`, or `USER_SELECTED`.
* `SymbolHit`:
  * `name`: identifier name (e.g. `NormalizedAnalysis`).
  * `kind`: `CLASS`, `STRUCT`, `FUNCTION`, `INTERFACE`, `ENUM`, `TYPE_ALIAS`.
  * `signature`: concise text definition (e.g. `struct Version { int major; ... };`).
  * `file_path`: relative file path.
  * `line_start`: 1-based start line.
* `SessionWorkingSet`:
  * Graph of active symbols and files touched during the current user session.
  * Each entry holds an attention weight $\in [0.0, 1.0]$.
  * Decays by factor $0.7$ on each non-referencing prompt turn.
  * Evicted when weight $< 0.2$.

---

## 2. Privacy & Air-Gapped Boundary

* **`AIR_GAPPED_LOCAL_ONLY`**:
  * Code symbols, signatures, file paths, and project names are strictly restricted to the local device.
  * Codebase context is fed **only** to the local in-process Granite 3B model (`llama.cpp`).
* **Zero Transmission to Cloud**:
  * No `SymbolHit`, `WorkspaceContext`, or code snippet may be included in network requests to cloud Jev or external telemetry under C07.
  * Jev receives only generalized, anonymized candidate choice groups.

---

## 3. Surgical Context Budget

* Granite 3B input budgets for codebase context must not exceed **350 tokens**.
* Full implementation bodies of functions/classes are excluded; only signatures and type definitions are formatted.
* If a prompt matches more than 5 symbols, the working set prioritizes:
  1. Symbols declared in the active open file.
  2. Symbols touched in the immediate previous turn.
  3. Highest BM25 trigram match rank.

---

## 4. Non-Destructive Invariant

* The codebase indexer is strictly **read-only**.
* It never writes to, modifies, or reformats the user's project files.
* SQLite indexing failures or Tree-sitter parse errors degrade gracefully to empty context without blocking prompt analysis or widget invocation.
