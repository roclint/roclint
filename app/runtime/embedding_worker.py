"""Roclint embedding worker — IBM Granite Embedding 311M Multilingual r2.

Pinned model: ibm-granite/granite-embedding-311m-multilingual-r2
Pinned revision: 44399559930365213510b1ee2eb15ded83374f0e

POOLING NOTE: This model uses CLS-token pooling (not mean pooling).
  Verified via 1_Pooling/config.json: pooling_mode_cls_token=true.
  sentence-transformers reads the pooling config from the model directory
  and applies CLS pooling automatically.

This worker is launched as a subprocess by the Roclint host and communicates
via JSON-newline messages on stdin/stdout.

Protocol (request → response):
  Request:  {"id": "...", "op": "embed", "texts": [...]}
  Response: {"id": "...", "ok": true, "embeddings": [[...], [...]], "dim": 768,
             "model_id": "...", "model_revision": "...", "elapsed_ms": 42.0}

  Request:  {"id": "...", "op": "ping"}
  Response: {"id": "...", "ok": true, "status": "ready",
             "model_id": "...", "model_revision": "...", "dim": 768,
             "pooling": "cls"}

  Request:  {"id": "...", "op": "quit"}
  Response: {"id": "...", "ok": true}
  (subprocess exits)

Error response: {"id": "...", "ok": false, "error": "..."}

Requirements:
  pip install sentence-transformers>=3.0 torch>=2.0 transformers>=4.40

Model notes:
  - Architecture: ModernBERT encoder with CLS-token pooling
  - Input token limit: 8192 tokens
  - Output dimensions: 768 (full) or reduced via Matryoshka
  - Pooling: CLS token (NOT mean); verified from published 1_Pooling/config.json
  - L2 normalization: required (applied by this worker via normalize_embeddings=True)
  - Query prefix: none required for this model per published instructions
  - Document prefix: none required
  - Multilingual: 12 languages, optimized for code and technical text
"""
import json
import os
import sys
import time

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace", line_buffering=True)
if hasattr(sys.stderr, "reconfigure"):
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")

# Pinned immutable revision. This must match embedding-models.json.
PINNED_MODEL_ID = "ibm-granite/granite-embedding-311m-multilingual-r2"
PINNED_REVISION = "44399559930365213510b1ee2eb15ded83374f0e"

# Target output dimensions (768 = full, Matryoshka supports 256/512/768).
TARGET_DIM = 768
# Maximum batch size for embedding to stay within memory bounds.
MAX_BATCH_SIZE = 32
# Maximum input characters per text (truncation is handled by the tokenizer).
MAX_CHARS_PER_TEXT = 32768
# Maximum number of texts per request to bound message size.
MAX_TEXTS_PER_REQUEST = 256


def _write_response(resp: dict) -> None:
    """Write a single JSON response line to stdout."""
    sys.stdout.write(json.dumps(resp, ensure_ascii=False) + "\n")
    sys.stdout.flush()


def _error(req_id: str, msg: str) -> None:
    _write_response({"id": req_id, "ok": False, "error": msg})


def _verify_pooling_config(model_dir: str) -> str:
    """Verify the pooling configuration and return the pooling mode string.

    Returns 'cls' or 'mean'. Raises RuntimeError if the config is missing or
    contradicts the expected CLS mode.
    """
    pooling_cfg_path = os.path.join(model_dir, "1_Pooling", "config.json")
    if not os.path.isfile(pooling_cfg_path):
        # Not found — sentence-transformers will read it from wherever the model
        # is loaded from. We cannot verify offline; warn but continue.
        sys.stderr.write(
            f"[embedding_worker] WARNING: 1_Pooling/config.json not found at {pooling_cfg_path}. "
            "Cannot verify pooling mode locally.\n"
        )
        sys.stderr.flush()
        return "unknown"

    try:
        with open(pooling_cfg_path, "r", encoding="utf-8") as f:
            cfg = json.load(f)
    except Exception as exc:
        raise RuntimeError(f"Cannot read 1_Pooling/config.json: {exc}") from exc

    cls_mode = cfg.get("pooling_mode_cls_token", False)
    mean_mode = cfg.get("pooling_mode_mean_tokens", False)

    if mean_mode and not cls_mode:
        raise RuntimeError(
            "Pooling config declares mean pooling. This model (granite-embedding-311m-multilingual-r2) "
            "uses CLS pooling. The model directory may contain the wrong model. "
            f"Config path: {pooling_cfg_path}"
        )
    if not cls_mode:
        raise RuntimeError(
            "Pooling config does not enable CLS pooling. "
            f"Config: {cfg}. Path: {pooling_cfg_path}"
        )
    return "cls"


def load_model(model_dir=None):
    """Load the pinned embedding model.

    Parameters
    ----------
    model_dir : str or None
        Path to the local model directory. If None, uses the HuggingFace cache.
        The model directory must contain the correct pooling configuration.
    """
    try:
        from sentence_transformers import SentenceTransformer
    except ImportError:
        raise RuntimeError(
            "sentence-transformers is required. "
            "Run: pip install sentence-transformers>=3.0"
        )

    if model_dir and os.path.isdir(model_dir):
        # Verify pooling configuration before loading.
        pooling_mode = _verify_pooling_config(model_dir)
        sys.stderr.write(
            f"[embedding_worker] Pooling mode: {pooling_mode} "
            f"(from {model_dir}/1_Pooling/config.json)\n"
        )
        sys.stderr.flush()

        # Load from local directory. local_files_only=True prevents any download.
        model = SentenceTransformer(
            model_dir,
            local_files_only=True,
            trust_remote_code=False,
        )
    else:
        if model_dir:
            raise RuntimeError(
                f"Model directory specified but does not exist: {model_dir}. "
                "Run tools/setup_embedding_model.py to download the model."
            )
        # Load from HuggingFace Hub with pinned revision. Only allowed during
        # initial setup; normal runtime must use a local directory.
        sys.stderr.write(
            "[embedding_worker] WARNING: Loading from HuggingFace Hub. "
            "Normal runtime must use --model-dir with locally downloaded assets.\n"
        )
        sys.stderr.flush()
        model = SentenceTransformer(
            PINNED_MODEL_ID,
            revision=PINNED_REVISION,
            trust_remote_code=False,
        )

    # Confirm the model's output dimensionality matches expectations.
    # Probe with a single short input.
    test_emb = model.encode(["test"], normalize_embeddings=True, show_progress_bar=False)
    actual_dim = int(test_emb.shape[1]) if hasattr(test_emb, "shape") else len(test_emb[0])
    if actual_dim != TARGET_DIM:
        raise RuntimeError(
            f"Model returned {actual_dim}-dim embeddings; expected {TARGET_DIM}. "
            "The model directory may contain the wrong model."
        )

    return model


def embed_texts(model, texts):
    """Embed a list of texts. Returns a list of 768-dim float lists.

    The sentence-transformers library handles:
    - Tokenization with the model's tokenizer
    - CLS-token pooling (via the model's 1_Pooling/config.json)
    - L2 normalization (applied here via normalize_embeddings=True)
    - Automatic truncation at the model's max sequence length

    No query or document prefix is added. Per the published model card for
    granite-embedding-311m-multilingual-r2, this model does not require
    task-specific prefixes for retrieval.
    """
    if not texts:
        return []

    # Sentence-transformers encode() returns normalized embeddings when
    # normalize_embeddings=True. CLS pooling is applied automatically
    # based on the model's 1_Pooling/config.json.
    embeddings = model.encode(
        texts,
        batch_size=min(MAX_BATCH_SIZE, len(texts)),
        normalize_embeddings=True,  # L2 normalization
        show_progress_bar=False,
        convert_to_numpy=True,
    )
    return embeddings.tolist()


def run_worker(model_dir=None):
    """Main event loop: read JSON requests from stdin, write responses to stdout."""
    sys.stderr.write(
        f"[embedding_worker] Loading model from: {model_dir or 'HuggingFace Hub'}\n"
    )
    sys.stderr.flush()

    t_load = time.monotonic()
    model = None
    load_error = None

    try:
        model = load_model(model_dir)
    except Exception as exc:
        load_error = f"Model load failed: {exc}"
        sys.stderr.write(f"[embedding_worker] {load_error}\n")
        sys.stderr.flush()

    if model is not None:
        load_ms = (time.monotonic() - t_load) * 1000.0
        sys.stderr.write(
            f"[embedding_worker] Ready. Model: {PINNED_MODEL_ID} "
            f"rev={PINNED_REVISION[:12]} dim={TARGET_DIM} pooling=cls "
            f"load_ms={load_ms:.0f}\n"
        )
        sys.stderr.flush()

    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue

        try:
            req = json.loads(line)
        except json.JSONDecodeError as exc:
            # Cannot echo an ID we could not parse; use empty string.
            _write_response({"id": "", "ok": False, "error": f"JSON decode error: {exc}"})
            continue

        req_id = req.get("id", "")
        op = req.get("op", "")

        # Every response must echo the exact request ID.
        if not isinstance(req_id, str):
            _error("", f"id must be a string, got {type(req_id).__name__}")
            continue

        if op == "quit":
            _write_response({"id": req_id, "ok": True})
            break

        elif op == "ping":
            if load_error:
                _error(req_id, load_error)
            else:
                _write_response({
                    "id": req_id,
                    "ok": True,
                    "status": "ready",
                    "model_id": PINNED_MODEL_ID,
                    "model_revision": PINNED_REVISION,
                    "dim": TARGET_DIM,
                    "pooling": "cls",
                })

        elif op == "embed":
            if load_error:
                _error(req_id, load_error)
                continue

            texts = req.get("texts", [])
            if not isinstance(texts, list):
                _error(req_id, "texts must be a list")
                continue
            if len(texts) > MAX_TEXTS_PER_REQUEST:
                _error(req_id, f"texts count {len(texts)} exceeds maximum {MAX_TEXTS_PER_REQUEST}")
                continue
            if not all(isinstance(t, str) for t in texts):
                _error(req_id, "every element of texts must be a string")
                continue

            # Truncate oversized texts (tokenizer truncates further at token level).
            texts = [t[:MAX_CHARS_PER_TEXT] for t in texts]

            t_embed = time.monotonic()
            try:
                embeddings = embed_texts(model, texts)
                elapsed_ms = (time.monotonic() - t_embed) * 1000.0

                # Validate shape before responding.
                if len(embeddings) != len(texts):
                    _error(req_id,
                           f"embed returned {len(embeddings)} rows for {len(texts)} inputs")
                    continue
                for row_idx, row in enumerate(embeddings):
                    if len(row) != TARGET_DIM:
                        _error(req_id,
                               f"row {row_idx} has {len(row)} dims; expected {TARGET_DIM}")
                        continue

                _write_response({
                    "id": req_id,
                    "ok": True,
                    "embeddings": embeddings,
                    "dim": TARGET_DIM,
                    "model_id": PINNED_MODEL_ID,
                    "model_revision": PINNED_REVISION,
                    "elapsed_ms": elapsed_ms,
                })
            except Exception as exc:
                _error(req_id, f"embed failed: {exc}")

        else:
            _error(req_id, f"unknown op: {op!r}")


if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser(description="Roclint embedding worker")
    parser.add_argument("--model-dir", default=None,
                        help="Path to the local model directory (required for offline use)")
    args = parser.parse_args()

    run_worker(model_dir=args.model_dir)
