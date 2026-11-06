"""Bounded batch/protocol tests using a deterministic engine; no model downloads or imports."""
import copy
import hashlib
import json
import os
import math
import queue
import threading
import importlib.util
from pathlib import Path
import unittest
from types import SimpleNamespace
from unittest import mock
import contextlib
import io
import sys

spec = importlib.util.spec_from_file_location("julia_worker", Path(__file__).resolve().parents[1] / "app/runtime/julia_worker.py")
worker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(worker)


class Engine:
    def __init__(self):
        self.rows = []
        self.calls = 0

    def logits(self, rows):
        self.calls += 1
        self.rows = copy.deepcopy(rows)
        return [[float(index) for index in range(len(row["options"]))] for row in rows]


def request(groups=1, assumptions=1):
    return {"plan_version": 2, "snapshot_id": "normalized-input", "prompt": "Update the service.",
            "groups": [{"id": f"grp_{index}", "question": "Which service?", "alternatives": [
                {"id": f"alt_{index}_0", "label": "Billing"}, {"id": f"alt_{index}_1", "label": "Shipping"}]}
                for index in range(groups)],
            "assumptions": [{"id": f"asm_{index}", "statement": f"Premise {index}"} for index in range(assumptions)]}


class JuliaPromptTriageTests(unittest.TestCase):
    def test_focused_goal_requires_independent_broad_agreement(self):
        class GoalEngine(Engine):
            def __init__(self, action, goal):
                super().__init__()
                self.action, self.goal = action, goal
            def logits(self, rows):
                self.rows = rows
                return [[math.log(1-p), math.log(p)] for p in
                        [self.action if i == 0 else self.goal if row['question'] == worker.TRIAGE_GOAL_HEAD[1] else .1
                         for i, row in enumerate(rows)]]
        for broad, goal, expected in ((.589, .98, .98), (.499, .99, .499), (.50, .95, .95), (.60, .69, .60)):
            result = worker.triage(GoalEngine(broad, goal), {'triage_plan_version':2, 'prompt':'Make Reader::open better.'})
            self.assertAlmostEqual(result['category_scores'][0]['score'], expected)
            self.assertAlmostEqual(result['atomic_scores']['ACTION'], broad)
            self.assertAlmostEqual(result['atomic_scores']['GOAL_CHOICE'], goal)
        self.assertNotIn('route', result)

    def test_triage_is_one_independent_real_engine_call(self):
        engine = Engine()
        result = worker.triage(engine, {"triage_plan_version": 2, "prompt": "Review this request."})
        self.assertEqual(engine.calls, 1)
        self.assertEqual(len(engine.rows), 5)
        self.assertTrue(all(row["state"] == "Review this request." for row in engine.rows))
        self.assertTrue(all(row["type"] == "noul" for row in engine.rows))
        self.assertEqual(result["template_id"], "julia.prompt_triage.categories.v3")
        self.assertEqual([item["category"] for item in result["category_scores"]], list(worker.TRIAGE_CATEGORIES))
        for item in result["category_scores"]:
            self.assertAlmostEqual(item["score"], worker.softmax([0, 1])[1])
        self.assertNotIn("ambiguity_score", result)
        self.assertNotIn("rewritten_prompt", result)
        self.assertNotIn("route", result)  # The core owns the threshold and routing decision.

    def test_triage_with_structured_state_and_extended_checklist_heads(self):
        engine = Engine()
        state_dict = {
            "prompt": "Update the card clamping logic so it stays inside the screen margin...",
            "workspace_matches": ["FloatingCard.qml", "ScreenPlacement.cpp"],
            "symbol_fanout_callers": 4
        }
        result = worker.triage(engine, {
            "triage_plan_version": 2,
            "state": state_dict,
            "extended_heads": True
        })
        self.assertEqual(engine.calls, 1)
        self.assertEqual(len(engine.rows), 7)  # six previous checks plus focused goal
        expected_state = (
            "Update the card clamping logic so it stays inside the screen margin...\n"
            "[Workspace AST Context: Target collision across: FloatingCard.qml, ScreenPlacement.cpp]"
        )
        self.assertTrue(all(row["state"] == expected_state for row in engine.rows))
        self.assertTrue(all(row["type"] == "noul" for row in engine.rows))
        self.assertIn("atomic_scores", result)
        self.assertEqual(len(result["atomic_scores"]), 7)
        self.assertIn("NEGATIVE_ONLY", result["atomic_scores"])
        self.assertIn("SECURITY_BYPASS", result["atomic_scores"])
        # Canonical categories are always 4
        self.assertEqual(len(result["category_scores"]), 4)

    def test_triage_with_candidate_choices_computes_entropy_confidence(self):
        engine = Engine()
        candidates = ["FloatingCard.qml", "ScreenPlacement.cpp"]
        result = worker.triage(engine, {
            "triage_plan_version": 2,
            "prompt": "Fix clamping bounds.",
            "candidate_choices": candidates
        })
        self.assertEqual(engine.calls, 1)
        self.assertEqual(len(engine.rows), 6)  # five checks plus one choice
        self.assertEqual(engine.rows[-1]["type"], "choice")
        self.assertEqual(engine.rows[-1]["options"], candidates)
        self.assertIn("choice_decision", result)
        self.assertIn(result["choice_decision"]["recommended_option"], candidates)
        self.assertTrue(0.0 <= result["choice_decision"]["confidence"] <= 1.0)

    def test_ast_context_formatting_and_state_injection(self):
        engine = Engine()
        # 1. Single exact target file
        worker.triage(engine, {
            "triage_plan_version": 2,
            "state": {
                "prompt": "Increase the hero lede font size to 1.25rem and line-height to 1.7 in styles.css.",
                "matched_files": ["styles.css"]
            }
        })
        self.assertIn("Single exact target: styles.css", engine.rows[0]["state"])

        # 2. Target collision across multiple files
        worker.triage(engine, {
            "triage_plan_version": 2,
            "state": {
                "prompt": "Add a Blog link in the navigation.",
                "matched_files": ["index.html", "about.html", "work.html", "contact.html"]
            }
        })
        self.assertIn("Target collision across: index.html, about.html, work.html, contact.html", engine.rows[0]["state"])

        # 3. Subjective action modifier
        worker.triage(engine, {
            "triage_plan_version": 2,
            "state": {
                "prompt": "Make the cards look much nicer and more modern.",
                "unbounded_action": True
            }
        })
        self.assertIn("Action is subjective without concrete styling parameters", engine.rows[0]["state"])

        # 4. Negative constraint violations
        worker.triage(engine, {
            "triage_plan_version": 2,
            "state": {
                "prompt": "Change the background color without modifying any CSS custom properties.",
                "constraint_violations": [":root CSS custom properties"]
            }
        })
        self.assertIn("Rule conflict: negative constraint forbids modifying :root CSS custom properties", engine.rows[0]["state"])

        # 5. Missing backend sink in static workspace
        worker.triage(engine, {
            "triage_plan_version": 2,
            "state": {
                "prompt": "Wire up the contact form so submissions get sent and saved.",
                "missing_backend_sink": True
            }
        })
        self.assertIn("Missing target sink: static repository has no backend service or form endpoint", engine.rows[0]["state"])

    def test_complete_prompt_tail_is_classified_without_mutating_the_request(self):
        engine = Engine()
        prompt = "Background. " * 400 + "Return only JSON. Also add an explanation outside the JSON."
        payload = {"triage_plan_version": 2, "prompt": prompt}
        before = copy.deepcopy(payload)
        worker.triage(engine, payload)
        self.assertGreater(len(prompt), 3000)
        self.assertTrue(all(row["state"] == prompt for row in engine.rows))
        self.assertEqual(payload, before)

    def test_lossless_overflow_fails_before_inference_without_a_clear_result(self):
        class Overflow(Engine):
            def encoding_info(self, rows):
                raise ValueError("Game state exceeds lossless context budget")
        engine = Overflow()
        payload = {"triage_plan_version": 2, "prompt": "Full request. " * 900}
        before = copy.deepcopy(payload)
        with self.assertRaises(ValueError): worker.triage(engine, payload)
        self.assertEqual(engine.calls, 0)
        self.assertEqual(payload, before)


class JuliaDeviceTests(unittest.TestCase):
    def runtime(self, available=True, bf16=True, fail_gpu=False, override=None, mps_available=False, fail_mps=False):
        calls, released = [], []
        torch = SimpleNamespace(float32="float32",
            cuda=SimpleNamespace(
                is_available=lambda: available, is_bf16_supported=lambda: bf16,
                empty_cache=lambda: released.append("cuda")),
            backends=SimpleNamespace(mps=SimpleNamespace(is_available=lambda: mps_available)),
            mps=SimpleNamespace(empty_cache=lambda: released.append("mps")))
        def loader(root, **kwargs):
            device = kwargs["device"]; calls.append(device)
            if device == "cuda" and fail_gpu: raise RuntimeError("GPU unavailable during warm-up")
            if device == "mps" and fail_mps: raise RuntimeError("MPS unavailable during warm-up")
            engine = Engine()
            engine.device = SimpleNamespace(type=device)
            engine.model = SimpleNamespace(parameters=lambda: iter([SimpleNamespace(dtype="float32")]))
            return engine
        return worker.create_engine(loader, torch, Path("fixture"), 4, override), calls, released

    def test_auto_uses_supported_bf16_and_reports_its_profile(self):
        engine, calls, _ = self.runtime()
        self.assertEqual(calls, ["cuda"])
        self.assertEqual(engine.roclint_runtime["configuration_id"], "julia.cuda.bf16.v2")

    def test_auto_uses_cpu_if_bf16_is_unsupported(self):
        engine, calls, _ = self.runtime(bf16=False)
        self.assertEqual(calls, ["cpu"])
        self.assertEqual(engine.roclint_runtime["compute_dtype"], "float32")

    def test_auto_falls_back_to_cpu_after_gpu_failure(self):
        engine, calls, released = self.runtime(fail_gpu=True)
        self.assertEqual(calls, ["cuda", "cpu"])
        self.assertEqual(released, ["cuda"])
        self.assertEqual(engine.roclint_runtime["configuration_id"], "julia.cpu.fp32.v2")

    def test_auto_uses_supported_mps_and_reports_its_profile(self):
        engine, calls, _ = self.runtime(available=False, mps_available=True)
        self.assertEqual(calls, ["mps"])
        self.assertEqual(engine.roclint_runtime["configuration_id"], "julia.mps.fp16.v2")
        self.assertEqual(engine.roclint_runtime["compute_dtype"], "float16")

    def test_auto_falls_back_to_cpu_after_mps_failure(self):
        engine, calls, released = self.runtime(available=False, mps_available=True, fail_mps=True)
        self.assertEqual(calls, ["mps", "cpu"])
        self.assertEqual(released, ["mps"])
        self.assertEqual(engine.roclint_runtime["configuration_id"], "julia.cpu.fp32.v2")

    def test_explicit_device_never_misreports_a_fallback(self):
        engine, calls, _ = self.runtime(override="cpu")
        self.assertEqual(calls, ["cpu"])
        self.assertEqual(engine.roclint_runtime["device"], "cpu")
        with self.assertRaises(ValueError): self.runtime(bf16=False, override="cuda")
        with self.assertRaises(RuntimeError): self.runtime(fail_gpu=True, override="cuda")
        with self.assertRaises(ValueError): self.runtime(available=False, mps_available=False, override="mps")
        with self.assertRaises(RuntimeError): self.runtime(available=False, mps_available=True, fail_mps=True, override="mps")

    def test_explicit_mps_device(self):
        engine, calls, _ = self.runtime(available=False, mps_available=True, override="mps")
        self.assertEqual(calls, ["mps"])
        self.assertEqual(engine.roclint_runtime["device"], "mps")
        self.assertEqual(engine.roclint_runtime["compute_dtype"], "float16")


class BenchmarkAcceptanceTests(unittest.TestCase):
    def test_single_device_probe_exits_unsuccessfully_when_any_required_gate_fails(self):
        path = Path(__file__).resolve().parent / "benchmark_julia_triage.py"
        spec = importlib.util.spec_from_file_location("benchmark", path)
        benchmark = importlib.util.module_from_spec(spec); spec.loader.exec_module(benchmark)
        for semantic, latency, expected in [(False, True, 1), (True, False, 1), (True, True, 0)]:
            with mock.patch.object(benchmark, "run_benchmark", return_value={"semantic_acceptance": semantic, "latency_acceptance": latency}), \
                 mock.patch.object(sys, "argv", [str(path), "--device", "cpu", "--json-out"]), \
                 contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(benchmark.main(), expected)

    def test_bad_triage_input_is_rejected_before_inference(self):
        for payload in ({"triage_plan_version": True, "prompt": "request"},
                        {"triage_plan_version": 1, "prompt": "request"},
                        {"triage_plan_version": 2, "prompt": " "},
                        {"triage_plan_version": 2, "prompt": "a\rb"},
                        {"triage_plan_version": 2, "prompt": "x" * (64 * 1024 + 1)}):
            engine = Engine()
            with self.assertRaises(ValueError):
                worker.triage(engine, payload)
            self.assertEqual(engine.calls, 0)

    def test_malformed_triage_heads_fail_without_replacement_scores(self):
        class MalformedEngine:
            def __init__(self, logits): self.value = logits
            def logits(self, rows): return self.value
        for logits in ([], [[0, 1]], [[0], [0, 1, 2, 3]], [[0, 1], [0, 1]], [[float("nan"), 0], [0, 1, 2, 3]]):
            with self.assertRaises(ValueError):
                worker.triage(MalformedEngine(logits), {"triage_plan_version": 2, "prompt": "request"})


class JuliaBatchTests(unittest.TestCase):
    def test_maximum_batch_is_one_independent_call(self):
        engine = Engine()
        result = worker.evaluate(engine, request(3, 4))
        self.assertEqual(engine.calls, 1)
        self.assertEqual(len(engine.rows), 10)
        self.assertEqual(len(result["groups"]), 3)
        self.assertEqual(len(result["assumptions"]), 4)
        self.assertEqual(result["plan_version"], 2)
        self.assertTrue(all(row["state"] == "Update the service." for row in engine.rows))
        self.assertTrue(all(row["type"] == "noul" for row in engine.rows[-4:]))
        self.assertEqual(result["groups"][0]["winner"], "alt_0_1")
        self.assertAlmostEqual(sum(item["probability"] for item in result["groups"][0]["probabilities"]), 1)
        self.assertGreater(result["assumptions"][0]["uncertainty"], 0.5)

    def test_assumption_only_and_group_only_batches(self):
        self.assertEqual(len(worker.evaluate(Engine(), request(0, 4))["assumptions"]), 4)
        self.assertEqual(worker.evaluate(Engine(), request(1, 0))["assumptions"], [])

    def test_limits_are_rejected_before_inference(self):
        invalid = [request(4, 0), request(0, 5), request(0, 0)]
        too_many_alternatives = request()
        too_many_alternatives["groups"][0]["alternatives"] += [{"id": "a", "label": "A"}, {"id": "b", "label": "B"}]
        invalid.append(too_many_alternatives)
        for value in invalid:
            with self.subTest(value=value):
                engine = Engine()
                with self.assertRaises(ValueError):
                    worker.evaluate(engine, value)
                self.assertEqual(engine.calls, 0)

    def test_ids_text_and_plan_are_validated(self):
        invalid = []
        for key, value in [("plan_version", 1), ("prompt", "a\rb"), ("prompt", "x" * (64 * 1024 + 1))]:
            item = request()
            item[key] = value
            invalid.append(item)
        duplicate = request(0, 2)
        duplicate["assumptions"][1]["id"] = duplicate["assumptions"][0]["id"]
        invalid.append(duplicate)
        oversized = request()
        oversized["assumptions"][0]["statement"] = "é" * 2049
        invalid.append(oversized)
        malformed = request()
        malformed["groups"][0]["alternatives"] = [False, 1]
        invalid.append(malformed)
        for item in invalid:
            with self.subTest(item=item), self.assertRaises(ValueError):
                worker.evaluate(Engine(), item)

    def test_malformed_logits_fail_the_whole_batch(self):
        class WrongCount:
            def logits(self, _):
                return []
        class InvalidLogit:
            def logits(self, rows):
                return [[float("nan"), 1] for _ in rows]
        class WrongArity:
            def logits(self, rows):
                return [[1] for _ in rows]
        for engine in [WrongCount(), InvalidLogit(), WrongArity()]:
            with self.subTest(engine=engine), self.assertRaises(ValueError):
                worker.evaluate(engine, request())

    def test_engine_errors_do_not_echo_prompt_fragments(self):
        class EchoingFailure:
            def logits(self, _):
                raise RuntimeError("private prompt fragment")
        with self.assertRaises(ValueError) as result:
            worker.evaluate(EchoingFailure(), request())
        self.assertNotIn("private prompt fragment", str(result.exception))
        self.assertIn("bounded batch", str(result.exception))


def envelope(identifier=1, operation="hello", payload=None):
    payload = payload or {}
    return {"protocol_version": 1, "type": "request", "request_id": str(identifier), "op": operation,
        "request_group_id": "group", "attempt_id": f"attempt_{identifier}", "session_id": "session",
        "draft_revision_id": "revision", "state_version": 5, "provider": "julia_local",
        "model_id": worker.MODEL_ID, "revision": worker.REVISION, "weights_sha256": worker.WEIGHTS_SHA256,
        "configuration_id": worker.CONFIGURATION_ID,
        "input_hash": hashlib.sha256(payload.get("prompt", "").encode("utf-8")).hexdigest(), "payload": payload}


def encoded(value):
    return json.dumps(value, ensure_ascii=False).encode("utf-8") + b"\n"


class RuntimeSession:
    def __init__(self, loader=lambda _: Engine(), evaluator=worker.evaluate, bootstrap=lambda: None):
        read_fd, write_fd = os.pipe()
        self.reader = os.fdopen(read_fd, "rb", buffering=0)
        self.writer = os.fdopen(write_fd, "wb", buffering=0)
        self.responses = queue.Queue()
        self.exit_code = None
        def run():
            self.exit_code = worker.serve(Path("unused"), self.reader, self.responses.put,
                loader=loader, installer=lambda _: None, evaluator=evaluator, bootstrap=bootstrap)
        self.thread = threading.Thread(target=run, daemon=True)
        self.thread.start()

    def send(self, value):
        self.writer.write(encoded(value))

    def response(self):
        return self.responses.get(timeout=2)

    def close(self):
        self.writer.close()
        self.thread.join(timeout=2)
        if self.thread.is_alive():
            raise AssertionError("Control reader did not stop")
        self.reader.close()


class JuliaProtocolTests(unittest.TestCase):
    def session(self, **kwargs):
        session = RuntimeSession(**kwargs)
        self.addCleanup(session.close)
        return session

    def test_prompt_triage_negotiates_and_echoes_the_frozen_context(self):
        session = self.session()
        session.send(envelope())
        self.assertIn("triage", session.response()["result"]["operations"])
        session.send(envelope(2, "load"))
        self.assertTrue(session.response()["ok"])
        source = envelope(3, "triage", {"triage_plan_version": 2, "prompt": "Review this request."})
        session.send(source)
        response = session.response()
        self.assertTrue(response["ok"])
        self.assertTrue(all(response[key] == value for key, value in source.items() if key not in {"type", "payload"}))
        self.assertEqual(len(response["result"]["category_scores"]), 4)
        self.assertNotIn("rewritten_prompt", response["result"])
        corrupt = copy.deepcopy(source)
        corrupt["input_hash"] = "0" * 64
        with self.assertRaises(ValueError):
            worker.decode_request(encoded(corrupt))

    def test_strict_envelope_utf8_types_hash_and_exact_frame_boundary(self):
        source = envelope(2, "evaluate", request())
        self.assertEqual(worker.decode_request(encoded(source)), source)
        invalid = [b'{"protocol_version":1,"protocol_version":1}\n', b'\xff\n', b'{}', b'NaN\n']
        for key, value in [("protocol_version", 2), ("protocol_version", True), ("request_id", "01"),
                           ("request_id", 2), ("state_version", True), ("state_version", worker.MAX_ID + 1),
                           ("session_id", ""), ("input_hash", "0" * 64), ("configuration_id", "other"),
                           ("op", "other"), ("payload", []), ("type", "result")]:
            item = copy.deepcopy(source)
            item[key] = value
            invalid.append(encoded(item))
        deep = envelope()
        deep["payload"] = {"nested": [[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[0]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]}
        invalid.append(encoded(deep))
        invalid.append(encoded(envelope()).replace(b'"payload": {}', b'"payload": {"x": 1e999}'))
        for value in invalid:
            with self.subTest(value=value[:80]), self.assertRaises((ValueError, UnicodeError)):
                worker.decode_request(value)
        source = envelope()
        source["padding"] = ""
        source["padding"] = "x" * (worker.MAX_FRAME - len(encoded(source)))
        self.assertEqual(len(encoded(source)), worker.MAX_FRAME)
        worker.decode_request(encoded(source))
        with self.assertRaises(ValueError):
            worker.decode_request(encoded(source) + b" ")

    def test_handshake_required_and_request_ids_are_unique(self):
        session = self.session()
        session.send(envelope(1, "load"))
        self.assertFalse(session.response()["ok"])
        session.thread.join(timeout=1)
        self.assertEqual(session.exit_code, 2)
        other = self.session()
        hello = envelope()
        other.send(hello)
        ack = other.response()
        self.assertEqual(ack["type"], "hello_ack")
        self.assertTrue(all(ack[key] == value for key, value in hello.items() if key not in {"type", "payload"}))
        other.send(envelope(1, "runtime_status"))
        self.assertFalse(other.response()["ok"])

    def test_partial_and_multiple_frames_control_status_and_shutdown(self):
        session = self.session()
        hello = encoded(envelope())
        session.writer.write(hello[:10])
        with self.assertRaises(queue.Empty):
            session.responses.get(timeout=0.03)
        session.writer.write(hello[10:] + encoded(envelope(2, "runtime_status")))
        self.assertEqual(session.response()["type"], "hello_ack")
        self.assertEqual(session.response()["result"]["state"], "unloaded")
        session.send(envelope(3, "load"))
        self.assertEqual(session.response()["result"]["state"], "ready")
        session.send(envelope(4, "shutdown"))
        self.assertEqual(session.response()["result"]["state"], "stopping")
        session.thread.join(timeout=1)
        self.assertEqual(session.exit_code, 0)

    def test_cancel_is_read_during_inference_and_result_is_suppressed(self):
        entered, release, finished = threading.Event(), threading.Event(), threading.Event()
        self.addCleanup(release.set)
        def slow(engine, payload):
            entered.set()
            release.wait(timeout=2)
            result = worker.evaluate(engine, payload)
            finished.set()
            return result
        session = self.session(evaluator=slow)
        session.send(envelope())
        session.response()
        session.send(envelope(2, "load"))
        session.response()
        session.send(envelope(3, "evaluate", request()))
        self.assertTrue(entered.wait(timeout=1))
        session.send(envelope(4, "cancel", {"target_request_id": "3"}))
        self.assertEqual(session.response()["result"], {"cancelled": True})
        session.send(envelope(5, "runtime_status"))
        self.assertEqual(session.response()["result"]["state"], "busy")
        release.set()
        self.assertTrue(finished.wait(timeout=1))
        with self.assertRaises(queue.Empty):
            session.responses.get(timeout=0.05)
        session.send(envelope(6, "runtime_status"))
        self.assertEqual(session.response()["result"]["state"], "ready")

    def test_shutdown_control_does_not_wait_for_a_blocked_model(self):
        entered, release = threading.Event(), threading.Event()
        self.addCleanup(release.set)
        def slow_load(_):
            entered.set()
            release.wait(timeout=3)
            return Engine()
        session = self.session(loader=slow_load)
        session.send(envelope())
        session.response()
        session.send(envelope(2, "load"))
        self.assertTrue(entered.wait(timeout=1))
        session.send(envelope(3, "shutdown"))
        self.assertEqual(session.response()["result"]["state"], "stopping")
        release.set()
        session.thread.join(timeout=0.5)
        self.assertFalse(session.thread.is_alive())

    def test_native_bootstrap_follows_ack_before_background_pipe_read(self):
        entered, release = threading.Event(), threading.Event()
        self.addCleanup(release.set)
        def bootstrap():
            entered.set()
            release.wait(timeout=2)
        session = self.session(bootstrap=bootstrap)
        self.assertFalse(entered.is_set())
        session.send(envelope())
        self.assertEqual(session.response()["type"], "hello_ack")
        self.assertTrue(entered.wait(timeout=1))
        session.send(envelope(2, "runtime_status"))
        with self.assertRaises(queue.Empty):
            session.responses.get(timeout=0.03)
        release.set()
        self.assertEqual(session.response()["result"]["state"], "unloaded")

    def test_sdk_errors_are_redacted_in_protocol(self):
        def broken(_):
            raise RuntimeError("private input and file path")
        session = self.session(loader=broken)
        session.send(envelope())
        session.response()
        session.send(envelope(2, "load"))
        response = session.response()
        self.assertFalse(response["ok"])
        self.assertNotIn("private input", response["error"])
        self.assertEqual(response["request_id"], "2")


if __name__ == "__main__":
    unittest.main()
