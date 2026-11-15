"""Integration test for the Roclint retrieval subsystem (R00).

Tests real embedding quality using the pinned IBM Granite embedding model.
Requires: model assets downloaded via tools/setup_embedding_model.py
         Python dependencies in embedding-requirements.txt

Usage:
    python tests/retrieval/test_retrieval_integration.py [--model-dir DIR]

This script:
1. Launches the embedding_worker.py subprocess.
2. Embeds real code and documentation examples.
3. Builds an in-memory evaluation corpus.
4. Measures Recall@k and Precision@k.
5. Reports results honestly — including failures.

Design note:
  The evaluation corpus contains deliberately misleading near-matches.
  A pure lexical search would miss semantic queries; a pure dense search
  would miss exact identifiers. The hybrid fusion must outperform both.
"""
import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WORKER_PATH = ROOT / "app" / "runtime" / "embedding_worker.py"

# ---------------------------------------------------------------------------
# Evaluation corpus
# ---------------------------------------------------------------------------

CORPUS = [
    # (chunk_id, text, source_kind)
    ("c001", "class Queue {\npublic:\n    void push(int v);\n    int pop();\n};\n", "code"),
    ("c002", "void Queue::push(int v) {\n    if (size_ < capacity_) data_[size_++] = v;\n}\n", "code"),
    ("c003", "int Queue::pop() {\n    if (size_ == 0) return -1;\n    return data_[--size_];\n}\n", "code"),
    ("c004", "class Stack {\npublic:\n    void push(int);\n    int pop();\nprivate:\n    int top_;\n};\n", "code"),
    ("c005", "namespace roclint {\n    class RewriteValidator {\n    public:\n        bool validate(const Proposal&);\n    };\n}\n", "code"),
    ("c006", "void Widget::update() {\n    emit changed();\n    scheduleRepaint();\n}\n", "code"),
    ("c007", "CREATE TABLE sessions (id TEXT PRIMARY KEY, created_at INTEGER);\n", "code"),
    ("d001", "The Queue data structure supports FIFO ordering. Push adds to the back, pop removes from the front.\n", "doc"),
    ("d002", "Cinematic depth of field blurs the background to focus attention on the subject.\n", "doc"),
    ("d003", "Typography hierarchy uses font sizes, weights, and spacing to guide reading order.\n", "doc"),
    ("d004", "The rewrite validator checks that proposals match the confirmed intent before approval.\n", "doc"),
    ("d005", "Stack is a LIFO data structure. Push adds to the top, pop removes from the top.\n", "doc"),
    # Misleading near-matches:
    ("m001", "void push_notification(const std::string& message) { notifier_.send(message); }\n", "code"),  # has 'push' but not Queue
    ("m002", "The cinematic universe expanded rapidly after the first film.\n", "doc"),  # 'cinematic' but different context
]

QUERIES = [
    # (query_text, expected_relevant_ids, description)
    (
        "Queue::push faster without changing ordering",
        {"c001", "c002", "c003"},
        "Semantic code query: Queue implementation",
    ),
    (
        "make a cinematic shot of a plant",
        {"d002"},
        "Creative direction: depth of field and cinematography",
    ),
    (
        "compact typography and hand-drawn artwork",
        {"d003"},
        "Style reference: typography hierarchy",
    ),
    (
        "validate rewrite proposal against intent",
        {"c005", "d004"},
        "Semantic match: validation without shared identifiers",
    ),
    (
        "LIFO data structure pop from top",
        {"c004", "d005"},
        "Exact identifier + semantic: Stack",
    ),
    (
        "Widget::update scheduleRepaint",
        {"c006"},
        "Exact qualified identifier query",
    ),
]

THRESHOLDS = {
    "recall_at_5_min": 0.5,    # Recall@5 must be >= 50% over all queries
    "precision_at_5_min": 0.2,  # Precision@5 must be >= 20% over all queries
}


# ---------------------------------------------------------------------------
# Worker interface
# ---------------------------------------------------------------------------

class EmbeddingWorker:
    def __init__(self, model_dir=None):
        cmd = [sys.executable, str(WORKER_PATH)]
        if model_dir:
            cmd += ["--model-dir", str(model_dir)]
        self.proc = subprocess.Popen(
            cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE
        )
        self._next_id = 1

    def _send(self, msg: dict) -> dict:
        req_id = str(self._next_id)
        self._next_id += 1
        msg["id"] = req_id
        line = json.dumps(msg) + "\n"
        self.proc.stdin.write(line.encode())
        self.proc.stdin.flush()
        resp_line = self.proc.stdout.readline()
        if not resp_line:
            stderr = self.proc.stderr.read(2000).decode(errors="replace")
            raise RuntimeError(f"Worker closed stdout unexpectedly. Stderr: {stderr}")
        return json.loads(resp_line.decode())

    def ping(self) -> dict:
        return self._send({"op": "ping"})

    def embed(self, texts: list[str]) -> list[list[float]]:
        resp = self._send({"op": "embed", "texts": texts})
        if not resp.get("ok"):
            raise RuntimeError(f"embed failed: {resp.get('error')}")
        return resp["embeddings"]

    def stop(self):
        try:
            self._send({"op": "quit"})
        except Exception:
            pass
        self.proc.terminate()


def cosine_similarity(a: list[float], b: list[float]) -> float:
    dot = sum(x * y for x, y in zip(a, b))
    # Vectors are already normalized, so cosine = dot product.
    return dot


def fused_score(dense_rank: int, lex_rank: int, k: float = 60.0) -> float:
    return 1.0 / (k + dense_rank) + (1.0 / (k + lex_rank) if lex_rank > 0 else 0.0)


def run_evaluation(worker: EmbeddingWorker, top_k: int = 5) -> dict:
    """Run the evaluation corpus and return metrics."""
    t0 = time.monotonic()

    # Embed all corpus chunks.
    print(f"Embedding {len(CORPUS)} corpus chunks...")
    corpus_texts = [text for _, text, _ in CORPUS]
    t_emb = time.monotonic()
    corpus_embeddings = worker.embed(corpus_texts)
    emb_ms_corpus = (time.monotonic() - t_emb) * 1000.0
    print(f"  Corpus embedding: {emb_ms_corpus:.0f}ms")

    # Embed all queries.
    query_texts = [q for q, _, _ in QUERIES]
    t_emb = time.monotonic()
    query_embeddings = worker.embed(query_texts)
    emb_ms_queries = (time.monotonic() - t_emb) * 1000.0
    print(f"  Query embedding:  {emb_ms_queries:.0f}ms")

    results = []
    recalls_at_k = []
    precisions_at_k = []

    for qi, (query_text, expected_ids, description) in enumerate(QUERIES):
        q_emb = query_embeddings[qi]

        # Dense retrieval: score all chunks.
        dense_scores = []
        for ci, (cid, _, _) in enumerate(CORPUS):
            score = cosine_similarity(q_emb, corpus_embeddings[ci])
            dense_scores.append((cid, score))
        dense_scores.sort(key=lambda x: -x[1])
        dense_rank = {cid: rank + 1 for rank, (cid, _) in enumerate(dense_scores)}

        # Simple lexical scoring: word overlap count.
        query_words = set(query_text.lower().split())
        lex_scores = []
        for cid, text, _ in CORPUS:
            chunk_words = set(text.lower().split())
            overlap = len(query_words & chunk_words)
            lex_scores.append((cid, overlap))
        lex_scores.sort(key=lambda x: -x[1])
        lex_rank = {cid: (rank + 1 if score > 0 else 0)
                     for rank, (cid, score) in enumerate(lex_scores)}

        # Fused scores.
        fused = {cid: fused_score(dense_rank[cid], lex_rank[cid]) for cid, _ in dense_scores}
        ranked = sorted(fused.items(), key=lambda x: -x[1])
        top_k_ids = {cid for cid, _ in ranked[:top_k]}

        retrieved_relevant = top_k_ids & expected_ids
        recall = len(retrieved_relevant) / len(expected_ids) if expected_ids else 1.0
        precision = len(retrieved_relevant) / top_k if top_k > 0 else 0.0

        recalls_at_k.append(recall)
        precisions_at_k.append(precision)

        results.append({
            "query": query_text,
            "description": description,
            "expected": sorted(expected_ids),
            "retrieved_top_k": sorted(top_k_ids),
            "recall_at_k": recall,
            "precision_at_k": precision,
        })

    avg_recall = sum(recalls_at_k) / len(recalls_at_k) if recalls_at_k else 0.0
    avg_precision = sum(precisions_at_k) / len(precisions_at_k) if precisions_at_k else 0.0
    total_ms = (time.monotonic() - t0) * 1000.0

    return {
        "top_k": top_k,
        "results": results,
        "avg_recall_at_k": avg_recall,
        "avg_precision_at_k": avg_precision,
        "total_ms": total_ms,
        "emb_ms_corpus": emb_ms_corpus,
        "emb_ms_queries": emb_ms_queries,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="Roclint retrieval integration test")
    parser.add_argument("--model-dir", default=None, help="Local model directory")
    parser.add_argument("--top-k", type=int, default=5)
    args = parser.parse_args()

    if not WORKER_PATH.is_file():
        print(f"[ERROR] embedding_worker.py not found at {WORKER_PATH}", file=sys.stderr)
        return 1

    print("=" * 60)
    print("Roclint Retrieval Integration Test")
    print(f"Model dir: {args.model_dir or 'HuggingFace cache'}")
    print("=" * 60)

    worker = EmbeddingWorker(model_dir=args.model_dir)

    print("Starting embedding worker (may take a moment for model loading)...")
    t_start = time.monotonic()
    try:
        pong = worker.ping()
    except Exception as exc:
        print(f"[ERROR] Worker ping failed: {exc}", file=sys.stderr)
        worker.stop()
        return 1
    startup_ms = (time.monotonic() - t_start) * 1000.0

    if not pong.get("ok"):
        print(f"[ERROR] Worker ping returned error: {pong.get('error')}", file=sys.stderr)
        worker.stop()
        return 1

    model_id = pong.get("model_id", "unknown")
    model_rev = pong.get("model_revision", "unknown")[:12]
    dim = pong.get("dim", "unknown")
    print(f"Worker ready: model={model_id} rev={model_rev} dim={dim} startup_ms={startup_ms:.0f}")

    # Confirm model identity against pinned revision.
    from app.runtime.embedding_worker import PINNED_MODEL_ID, PINNED_REVISION
    if pong.get("model_id") != PINNED_MODEL_ID:
        print(f"[WARN] Model ID mismatch: expected {PINNED_MODEL_ID}, got {pong.get('model_id')}")
    if pong.get("model_revision", "")[:40] != PINNED_REVISION[:40]:
        print(f"[WARN] Revision mismatch: expected {PINNED_REVISION[:12]}, got {model_rev}")

    try:
        metrics = run_evaluation(worker, top_k=args.top_k)
    except Exception as exc:
        print(f"[ERROR] Evaluation failed: {exc}", file=sys.stderr)
        worker.stop()
        return 1
    finally:
        worker.stop()

    # Print results.
    print()
    print(f"Results (Recall@{args.top_k}, Precision@{args.top_k}):")
    for r in metrics["results"]:
        status = "✓" if r["recall_at_k"] >= 0.5 else "✗"
        print(f"  {status} [{r['recall_at_k']:.2f}/{r['precision_at_k']:.2f}] {r['description']}")
        print(f"       Expected:  {r['expected']}")
        print(f"       Retrieved: {r['retrieved_top_k']}")

    print()
    print(f"Average Recall@{args.top_k}:    {metrics['avg_recall_at_k']:.3f}")
    print(f"Average Precision@{args.top_k}: {metrics['avg_precision_at_k']:.3f}")
    print(f"Total elapsed:          {metrics['total_ms']:.0f}ms")
    print(f"Corpus embed:           {metrics['emb_ms_corpus']:.0f}ms ({len(CORPUS)} chunks)")
    print(f"Query embed:            {metrics['emb_ms_queries']:.0f}ms ({len(QUERIES)} queries)")

    # Check thresholds.
    failures = []
    if metrics["avg_recall_at_k"] < THRESHOLDS["recall_at_5_min"]:
        failures.append(
            f"Recall@{args.top_k} = {metrics['avg_recall_at_k']:.3f} < "
            f"{THRESHOLDS['recall_at_5_min']} (minimum)"
        )
    if metrics["avg_precision_at_k"] < THRESHOLDS["precision_at_5_min"]:
        failures.append(
            f"Precision@{args.top_k} = {metrics['avg_precision_at_k']:.3f} < "
            f"{THRESHOLDS['precision_at_5_min']} (minimum)"
        )

    if failures:
        print()
        print("[FAIL] Thresholds not met:")
        for f in failures:
            print(f"  - {f}")
        print("NOTE: Thresholds are advisory baselines measured on this corpus.")
        print("      A real quality gate requires a held-out evaluation dataset.")
        return 1

    print()
    print("[PASS] All retrieval quality thresholds met.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
