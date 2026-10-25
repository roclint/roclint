"""Resident Julia-1 CPU scorer. Versioned bounded UTF-8 NDJSON with independent control I/O.

Model code and weights come from a pinned SupersonicLabs release. Downloads are
explicit; load/evaluate work entirely from local files. stdout is protocol-only.
"""

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import queue
import sys
import threading
import time

MODEL_ID = "SupersonicLabs/Julia-1"
REVISION = "a85b127321d580d65176c89ced8273f305745d85"
WEIGHTS_SHA256 = "df853bf7fe424420011f3d0c47a05d7341aa9eefa7fb9f203ea4aada4ad95b72"
MAX_FRAME = 256 * 1024
PROTOCOL_OUT = sys.stdout
PROTOCOL_VERSION = 1
CONFIGURATION_ID = "julia.auto.bf16-fp32.v2"
MAX_PENDING = 8
MAX_ID = 9007199254740991
TRIAGE_PLAN_VERSION = 2
TRIAGE_TEMPLATE_ID = "julia.prompt_triage.categories.v2"
TRIAGE_CATEGORIES = ("ACTION", "TARGET", "RULE", "RULE_CONFLICT")
TRIAGE_HEADS = (
    ("ACTION",
     "Is the intended action or requested outcome materially ambiguous?",
     ["The request identifies the intended action or outcome; ordinary implementation choices do not change that intent.",
      "Materially different active actions or outcomes would fit the wording; the user has not chosen between them."]),
    ("TARGET",
     "Is a target or scope needed for this request missing or materially ambiguous?",
     ["The request identifies its target and scope clearly enough, or no specific target is needed.",
      "The active task requires modifying a target file, but which file cannot be determined."]),
    ("RULE",
     "Is a requirement or success criterion materially ambiguous?",
     ["The request fixes its requirements and limits; ordinary implementation choices do not change that intent.",
      "Materially different requirements, criteria, or goals would fit the wording; the user has not chosen between them."]),
    ("RULE_CONFLICT",
     "Does the request contain incompatible actual requirements?",
     ["The request instructions are compatible, or the task has no conflicting requirements.",
      "Two or more active requirements directly contradict each other and cannot both be satisfied."])
)
OUTPUT_LOCK = threading.Lock()


def emit(value):
    frame = json.dumps(value, ensure_ascii=False, allow_nan=False).encode("utf-8") + b"\n"
    if len(frame) > MAX_FRAME:
        raise ValueError("Julia response exceeded the frame limit")
    with OUTPUT_LOCK:
        PROTOCOL_OUT.write(frame.decode("utf-8"))
        PROTOCOL_OUT.flush()


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def install(root):
    from huggingface_hub import snapshot_download

    snapshot_download(
        MODEL_ID, revision=REVISION, local_dir=root,
        allow_patterns=["README.md", "config.json", "julia_config.json", "inference-policy.json",
                        "model.safetensors", "encoder/*.json", "tokenizer/*.json",
                        "julia/*.py", "julia/router/*.py"],
    )
    if digest(root / "model.safetensors") != WEIGHTS_SHA256:
        raise ValueError("Julia weights failed their SHA-256 integrity check")
    manifest = {"model_id": MODEL_ID, "revision": REVISION, "weights_sha256": WEIGHTS_SHA256, "files": {}}
    for path in sorted(root.rglob("*")):
        if path.is_file() and ".cache" not in path.parts and "__pycache__" not in path.parts and path.name != "roclint-julia.json":
            manifest["files"][path.relative_to(root).as_posix()] = digest(path)
    (root / "roclint-julia.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")


def load(root):
    manifest = json.loads((root / "roclint-julia.json").read_text(encoding="utf-8"))
    if manifest.get("revision") != REVISION or manifest.get("weights_sha256") != WEIGHTS_SHA256:
        raise ValueError("This Julia folder is not the pinned Roclint checkpoint. Download or repair it first.")
    required = {"model.safetensors", "encoder/config.json", "tokenizer/tokenizer.json", "tokenizer/tokenizer_config.json",
                "julia_config.json", "julia/__init__.py", "julia/inference.py", "julia/model.py", "julia/data.py",
                "julia/router/engine.py", "julia/router/encoder.py"}
    if not required.issubset(manifest.get("files", {})):
        raise ValueError("Julia's integrity manifest is incomplete. Download or repair it first.")
    for name, expected in manifest["files"].items():
        path = (root / name).resolve()
        if not path.is_relative_to(root.resolve()) or digest(path) != expected:
            raise ValueError("Julia's local files failed an integrity check. Download or repair the model.")
    if digest(root / "model.safetensors") != WEIGHTS_SHA256:
        raise ValueError("Julia weights failed their pinned SHA-256 integrity check")
    os.environ["HF_HUB_OFFLINE"] = "1"
    os.environ["TRANSFORMERS_OFFLINE"] = "1"
    os.environ["TOKENIZERS_PARALLELISM"] = "false"
    os.environ.setdefault("JULIA_CPU_THREADS", str(min(8, os.cpu_count() or 4)))
    import torch
    from importlib.metadata import version

    if not version("transformers").startswith("5.0."):
        raise ValueError("Julia needs transformers 5.0.x. Select a Python runtime with the Julia requirements installed.")
    threads = int(os.environ["JULIA_CPU_THREADS"])
    if not 1 <= threads <= 256:
        raise ValueError("JULIA_CPU_THREADS must be between 1 and 256")
    torch.set_num_threads(threads)
    sys.path.insert(0, str(root))
    try:
        from julia.router.engine import FastEngine
        _orig_pack = FastEngine._pack

        def _mps_pack(self, encoded):
            batch = _orig_pack(self, encoded)
            if self.device.type == "mps":
                for k, v in batch.items():
                    if isinstance(v, torch.Tensor) and v.device.type != "mps":
                        batch[k] = v.to(self.device)
            return batch

        FastEngine._pack = _mps_pack
    except Exception:
        pass
    from julia import load_model

    return create_engine(load_model, torch, root, threads, os.environ.get("JULIA_DEVICE"))


def create_engine(load_model, torch, root, threads, override=None):
    """The pinned SDK uses BF16 autocast on CUDA and FP16 on Apple MPS; unsupported devices use CPU FP32."""
    # Detect optional DirectML backend (Windows AMD/Intel/Qualcomm).
    # torch-directml is an optional dependency; degrade silently if absent.
    directml_supported = False
    try:
        import torch_directml  # type: ignore[import]
        directml_supported = torch_directml.is_available()
    except Exception:  # ImportError or any init failure
        pass
    if override not in (None, "auto", "cpu", "cuda", "mps", "directml"):
        raise ValueError("JULIA_DEVICE must be auto, cpu, cuda, mps or directml")
    if override == "directml" and not directml_supported:
        raise ValueError("Explicit DirectML requires torch-directml to be installed and available")
    cuda_supported = torch.cuda.is_available() and torch.cuda.is_bf16_supported()
    mps_supported = hasattr(torch, "backends") and hasattr(torch.backends, "mps") and torch.backends.mps.is_available()
    if override == "cuda" and not cuda_supported:
        raise ValueError("Explicit CUDA requires BF16 support in the pinned Julia runtime")
    if override == "mps" and not mps_supported:
        raise ValueError("Explicit MPS requires Apple Metal support in the pinned Julia runtime")
    if override == "cuda":
        devices = ["cuda"]
    elif override == "directml":
        devices = ["directml"]
    elif override == "mps":
        devices = ["mps"]
    elif override == "cpu":
        devices = ["cpu"]
    elif cuda_supported:
        devices = ["cuda", "directml", "cpu"] if directml_supported else ["cuda", "cpu"]
    elif directml_supported:
        devices = ["directml", "cpu"]
    elif mps_supported:
        devices = ["mps", "cpu"]
    else:
        devices = ["cpu"]
    for device in devices:
        engine = None
        try:
            sdk_device = device
            if device == "directml":
                sdk_device = torch_directml.device(0)  # torch_directml guaranteed imported above
            engine = load_model(str(root), device=sdk_device, backend="torch", max_length=8192, head_length=512,
                                strict_encoding=True, marker_only_head=True, batch_size=4, compile_model=False)
            engine.logits([{"state": "The user asks for a refund.", "question": "Which team?",
                            "options": ["Billing", "Shipping"], "type": "choice"}])
            # torch-directml exposes its device as type "privateuseone" internally.
            expected_type = "privateuseone" if device == "directml" else device
            if engine.device.type != expected_type:
                raise RuntimeError("Julia loaded on an unexpected device")
            if device == "cpu" and next(engine.model.parameters()).dtype != torch.float32:
                raise RuntimeError("Julia CPU execution requires FP32 weights")
            config_id = (
                "julia.cuda.bf16.v2"      if device == "cuda"      else
                "julia.directml.fp16.v2"  if device == "directml"  else
                "julia.mps.fp16.v2"       if device == "mps"        else
                "julia.cpu.fp32.v2"
            )
            compute_dtype = (
                "bfloat16" if device == "cuda"                      else
                "float16"  if device in ("directml", "mps")         else
                "float32"
            )
            engine.roclint_runtime = {
                "configuration_id": config_id,
                "device": device,
                "compute_dtype": compute_dtype,
                "cpu_threads": threads
            }
            return engine
        except (RuntimeError, MemoryError) as exc:
            oom = (device == "cuda" and hasattr(torch, "cuda") and
                   hasattr(torch.cuda, "OutOfMemoryError") and
                   isinstance(exc, torch.cuda.OutOfMemoryError))
            if oom:
                print(f"[Julia] CUDA out-of-memory ('{device}'); retrying on next device. ({exc})",
                      file=sys.stderr)
            if device not in ("cuda", "mps", "directml") or len(devices) == 1:
                raise
            del engine
        # Leave the exception handler first so its traceback cannot retain the failed model.
        # Releases only this process's unused accelerator allocations; never touches other processes.
        if device == "cuda" and hasattr(torch, "cuda") and hasattr(torch.cuda, "empty_cache"):
            torch.cuda.empty_cache()
        elif device == "mps" and hasattr(torch, "mps") and hasattr(torch.mps, "empty_cache"):
            torch.mps.empty_cache()
    raise RuntimeError("Julia could not load an execution profile")


def softmax(values):
    if not values or not all(math.isfinite(float(value)) for value in values):
        raise ValueError("Julia returned invalid logits")
    top = max(values)
    exps = [math.exp(value - top) for value in values]
    total = sum(exps)
    return [value / total for value in exps]


def triage(engine, request):
    """Classify the prompt before Granite; never generate options or rewritten text."""
    state = request.get("prompt")
    if type(request.get("triage_plan_version")) is not int or request["triage_plan_version"] != TRIAGE_PLAN_VERSION:
        raise ValueError("Unsupported Julia prompt-triage plan")
    if (not isinstance(state, str) or not state.strip() or "\r" in state or
            len(state.encode("utf-8")) > 64 * 1024):
        raise ValueError("Julia triage needs a nonempty bounded canonical LF prompt")
    # Independently score the four user-requested categories; the core owns max/threshold routing.
    rows = [{"state": state, "question": question, "options": list(options), "type": "noul"}
            for _, question, options in TRIAGE_HEADS]
    try:
        # Audit the exact rows used by inference. Over-budget prompts fail without a route;
        # an unchanged caller buffer does not justify classifying a silently shortened prefix.
        if hasattr(engine, "encoding_info"):
            audit = engine.encoding_info(rows)
            if len(audit) != 4 or any(item.get("stateTruncated") is not False or
                                     item.get("optionsTruncated") is not False for item in audit):
                raise ValueError("Julia could not encode the complete prompt losslessly")
        logits = engine.logits(rows)
    except Exception:
        raise ValueError("Julia could not classify the bounded prompt.") from None
    if len(logits) != len(TRIAGE_CATEGORIES):
        raise ValueError("Julia returned the wrong number of triage heads")
    scores = []
    for category, values in zip(TRIAGE_CATEGORIES, logits):
        probabilities = softmax(values)
        if len(probabilities) != 2:
            raise ValueError("Julia returned a malformed Boolean triage head")
        scores.append({"category": category, "score": probabilities[1]})
    # These independent scores need not sum to one. Do not invent a global probability.
    result = {"triage_plan_version": TRIAGE_PLAN_VERSION, "template_id": TRIAGE_TEMPLATE_ID, "category_scores": scores}
    if hasattr(engine, "roclint_runtime"):
        result["execution_configuration_id"] = engine.roclint_runtime["configuration_id"]
    return result


def evaluate(engine, request):
    state = request.get("prompt")
    groups = request.get("groups")
    assumptions = request.get("assumptions")
    if request.get("plan_version") != 2:
        raise ValueError("Unsupported Julia scoring plan; expected version 2")
    if not isinstance(state, str) or len(state.encode("utf-8")) > 64 * 1024 or "\r" in state:
        raise ValueError("Julia needs a bounded canonical LF prompt")
    if not isinstance(groups, list) or len(groups) > 3:
        raise ValueError("Julia accepts at most three material choice groups")
    if not isinstance(assumptions, list) or len(assumptions) > 4:
        raise ValueError("Julia accepts at most four material assumption checks")
    if not groups and not assumptions:
        raise ValueError("The scoring batch is empty")
    rows = []
    seen = set()

    def bounded_text(value, budget=4096):
        return isinstance(value, str) and bool(value.strip()) and len(value.encode("utf-8")) <= budget

    for group in groups:
        if not isinstance(group, dict):
            raise ValueError("Malformed choice group")
        gid = group.get("id")
        alternatives = group.get("alternatives")
        if not bounded_text(gid, 256) or gid in seen:
            raise ValueError("Invalid or duplicate choice group ID")
        seen.add(gid)
        if not isinstance(alternatives, list) or not 2 <= len(alternatives) <= 3 or not all(isinstance(item, dict) for item in alternatives):
            raise ValueError("Julia requires two to three supplied alternatives")
        ids = [item.get("id") for item in alternatives]
        labels = [item.get("label") for item in alternatives]
        if not all(bounded_text(value, 256) for value in ids) or len(set(ids)) != len(ids) or not all(bounded_text(value) for value in labels):
            raise ValueError("Invalid alternative IDs or labels")
        question = group.get("question")
        if not bounded_text(question):
            raise ValueError("A choice group needs a bounded question")
        rows.append({"state": state, "question": question, "options": labels, "type": "choice"})
        rendered = "; ".join(labels)
        rows.append({"state": state, "question": f"Is the following decision materially ambiguous in the user's request? {question} Interpretations: {rendered}",
                     "options": ["The user's request clearly resolves this decision to one interpretation.",
                                 "The user's request leaves multiple materially different interpretations unresolved."], "type": "noul"})
    for assumption in assumptions:
        if not isinstance(assumption, dict):
            raise ValueError("Malformed assumption check")
        aid = assumption.get("id")
        statement = assumption.get("statement")
        if not bounded_text(aid, 256) or aid in seen or not bounded_text(statement):
            raise ValueError("Invalid assumption ID or statement")
        seen.add(aid)
        rows.append({"state": state, "question": f"Does the user's request explicitly establish this material premise? {statement}",
                     "options": ["The user's request explicitly establishes this premise.",
                                 "The user's request leaves this material premise uncertain or unsupported."], "type": "noul"})
    # One independent batch; no row sees answers to any other row. No truncation.
    try:
        logits = engine.logits(rows)
    except Exception:
        # An SDK error may contain input fragments; do not persist or echo those through the host.
        raise ValueError("Julia could not evaluate the bounded batch. Check model compatibility and input length.") from None
    if len(logits) != len(rows):
        raise ValueError("Julia returned the wrong number of decisions")
    scores = []
    for index, group in enumerate(groups):
        probabilities = softmax(logits[index * 2])
        ambiguity = softmax(logits[index * 2 + 1])
        if len(probabilities) != len(group["alternatives"]) or len(ambiguity) != 2:
            raise ValueError("Julia returned a malformed decision")
        winner = max(range(len(probabilities)), key=probabilities.__getitem__)
        scores.append({"group_id": group["id"], "winner": group["alternatives"][winner]["id"],
                       "probabilities": [{"id": item["id"], "probability": probability}
                                         for item, probability in zip(group["alternatives"], probabilities)],
                       "ambiguity": ambiguity[1], "noul_certainty": max(ambiguity) >= 0.70})
    checks = []
    for index, assumption in enumerate(assumptions):
        probabilities = softmax(logits[len(groups) * 2 + index])
        if len(probabilities) != 2:
            raise ValueError("Julia returned a malformed assumption check")
        checks.append({"assumption_id": assumption["id"], "uncertainty": probabilities[1],
                       "noul_certainty": max(probabilities) >= 0.70})
    return {"snapshot_id": request["snapshot_id"], "plan_version": 2, "groups": scores, "assumptions": checks,
            "model_id": MODEL_ID, "revision": REVISION, "weights_sha256": WEIGHTS_SHA256}


# The reader stays responsive while the single inference thread loads/scores. A
# cancelled torch kernel may finish, but its result is never published or reused.
def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("Duplicate JSON key")
        result[key] = value
    return result


class WindowsPipeReader:
    """Read only available bytes, never holding a CRT stdin lock while idle.

    Native library DLL initialization can flush/lock CRT streams. A background
    blocking stdin read can deadlock that initialization on Windows. PeekNamedPipe
    bounds each os.read to bytes already present; the idle wait holds no CRT lock.
    """
    def __init__(self, stream):
        import ctypes
        from ctypes import wintypes
        import msvcrt
        self.ctypes = ctypes
        self.wintypes = wintypes
        self.fd = stream.fileno()
        msvcrt.setmode(self.fd, os.O_BINARY)
        self.handle = msvcrt.get_osfhandle(self.fd)
        self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        self.peek = self.kernel.PeekNamedPipe
        pointer = ctypes.POINTER(wintypes.DWORD)
        self.peek.argtypes = [wintypes.HANDLE, wintypes.LPVOID, wintypes.DWORD, pointer, pointer, pointer]
        self.peek.restype = wintypes.BOOL
        self.buffer = bytearray()

    def readline(self, limit):
        while True:
            end = self.buffer.find(b"\n")
            count = min(end + 1, limit) if end >= 0 else limit if len(self.buffer) >= limit else 0
            if count:
                result = bytes(self.buffer[:count])
                del self.buffer[:count]
                return result
            available = self.wintypes.DWORD()
            if not self.peek(self.handle, None, 0, None, self.ctypes.byref(available), None):
                code = self.ctypes.get_last_error()
                if code in {109, 233}:  # Closed anonymous pipe / disconnected pipe.
                    result = bytes(self.buffer)
                    self.buffer.clear()
                    return result
                raise OSError(code, "Julia's input pipe could not be read")
            if available.value:
                self.buffer.extend(os.read(self.fd, min(available.value, limit - len(self.buffer))))
            else:
                time.sleep(0.01)


def decode_request(line):
    if len(line) > MAX_FRAME or not line.endswith(b"\n"):
        raise ValueError("Invalid frame size or missing newline")
    request = json.loads(line.decode("utf-8"), object_pairs_hook=_unique_object,
                         parse_constant=lambda _: (_ for _ in ()).throw(ValueError("Nonfinite JSON")))
    stack = [(request, 0)]
    while stack:
        value, depth = stack.pop()
        if depth > 32:
            raise ValueError("JSON nesting exceeded the protocol limit")
        if isinstance(value, float) and not math.isfinite(value):
            raise ValueError("Nonfinite JSON number")
        if isinstance(value, dict):
            stack.extend((item, depth + 1) for item in value.values())
        elif isinstance(value, list):
            stack.extend((item, depth + 1) for item in value)
    if not isinstance(request, dict) or type(request.get("protocol_version")) is not int or request["protocol_version"] != PROTOCOL_VERSION:
        raise ValueError("Unsupported protocol")
    if request.get("type") != "request" or request.get("op") not in {"hello", "load", "install", "triage", "evaluate", "runtime_status", "cancel", "shutdown"}:
        raise ValueError("Unsupported message")
    identifier = request.get("request_id")
    if not isinstance(identifier, str) or not identifier.isascii() or not identifier.isdecimal() or not 1 <= int(identifier) <= MAX_ID or str(int(identifier)) != identifier:
        raise ValueError("Invalid request ID")
    for key in ["request_group_id", "attempt_id", "session_id", "draft_revision_id"]:
        value = request.get(key)
        if not isinstance(value, str) or not value or len(value.encode("utf-8")) > 256:
            raise ValueError("Invalid request binding")
    version = request.get("state_version")
    if type(version) is not int or not 0 <= version <= MAX_ID:
        raise ValueError("Invalid state version")
    identity = {"provider": "julia_local", "model_id": MODEL_ID, "revision": REVISION,
                "weights_sha256": WEIGHTS_SHA256, "configuration_id": CONFIGURATION_ID}
    if any(request.get(key) != value for key, value in identity.items()):
        raise ValueError("Incompatible model configuration")
    input_hash = request.get("input_hash")
    if not isinstance(input_hash, str) or len(input_hash) != 64 or any(c not in "0123456789abcdef" for c in input_hash):
        raise ValueError("Invalid input hash")
    if not isinstance(request.get("payload"), dict):
        raise ValueError("Invalid operation payload")
    if request["op"] in {"evaluate", "triage"}:
        prompt = request["payload"].get("prompt")
        if not isinstance(prompt, str) or hashlib.sha256(prompt.encode("utf-8")).hexdigest() != input_hash:
            raise ValueError("Input hash mismatch")
    return request


def response_for(request, result=None, error=None, latency=0):
    envelope = {key: value for key, value in request.items() if key not in {"payload", "type"}}
    envelope.update(type="error" if error else "hello_ack" if request["op"] == "hello" else
                    "runtime_status" if request["op"] == "runtime_status" else "result",
                    ok=error is None, latency_ms=latency)
    envelope["error" if error else "result"] = error if error else result
    return envelope


def serve(root, stream, send=emit, loader=load, installer=install, evaluator=evaluate, bootstrap=lambda: None, triager=triage):
    if os.name == "nt":
        stream = WindowsPipeReader(stream)
    # Acknowledge compatibility before loading dependencies. Windows uses the
    # nonblocking reader above to avoid native-import/CRT stream deadlocks.
    try:
        first = decode_request(stream.readline(MAX_FRAME + 1))
        if first["op"] != "hello":
            raise ValueError("Handshake required")
        send(response_for(first, {"protocol_version": PROTOCOL_VERSION, "operations":
            ["load", "install", "triage", "evaluate", "runtime_status", "cancel", "shutdown"],
            "process_id": os.getpid(), "parent_process_id": os.getppid()}))
        bootstrap()
    except Exception:
        send({"protocol_version": PROTOCOL_VERSION, "type": "error", "request_id": "0",
              "ok": False, "error": "Julia could not initialize its protocol or native dependencies."})
        return 2
    work = queue.Queue(MAX_PENDING)
    jobs = {}
    lock = threading.Lock()
    stopping = threading.Event()
    ready = threading.Event()
    active = threading.Event()

    def execute():
        engine = None
        while not stopping.is_set():
            request = work.get()
            if request is None:
                return
            identifier = request["request_id"]
            with lock:
                cancelled = jobs[identifier]
            started = time.monotonic()
            result, error = None, None
            if not cancelled.is_set():
                active.set()
                try:
                    operation = request["op"]
                    if operation == "install":
                        os.environ.pop("HF_HUB_OFFLINE", None)
                        os.environ.pop("TRANSFORMERS_OFFLINE", None)
                        ready.clear()
                        engine = None
                        installer(root)
                        if not cancelled.is_set() and not stopping.is_set():
                            engine = loader(root)
                    elif operation == "load":
                        engine = loader(root) if engine is None else engine
                    elif operation in {"evaluate", "triage"}:
                        if engine is None:
                            raise ValueError("Julia is not loaded")
                        result = triager(engine, request["payload"]) if operation == "triage" else evaluator(engine, request["payload"])
                    if operation in {"load", "install"}:
                        if engine is not None:
                            ready.set()
                        result = {"state": "ready" if engine is not None else "unloaded"}
                        if hasattr(engine, "roclint_runtime"):
                            result["runtime"] = engine.roclint_runtime
                except Exception:
                    # SDK exceptions can contain input text, paths or raw output.
                    error = "Julia could not complete this local operation. Check the installed model and Python dependencies."
                finally:
                    active.clear()
            with lock:
                if not cancelled.is_set() and not stopping.is_set():
                    send(response_for(request, result, error, (time.monotonic() - started) * 1000))
                jobs.pop(identifier, None)

    # Native NumPy/PyTorch initialization runs on the process main thread.
    # A separate reader handles cancellation/status/shutdown while it runs.
    exit_code = [0]
    def read_requests():
        last_id = int(first["request_id"])
        try:
            while True:
                line = stream.readline(MAX_FRAME + 1)
                if not line:
                    return 0
                try:
                    request = decode_request(line)
                    identifier = int(request["request_id"])
                    if identifier <= last_id:
                        raise ValueError("Repeated or reordered request ID")
                    last_id = identifier
                    operation = request["op"]
                    if operation == "hello":
                        raise ValueError("Repeated handshake")
                    elif operation == "shutdown":
                        stopping.set()
                        send(response_for(request, {"state": "stopping"}))
                        return 0
                    elif operation == "runtime_status":
                        send(response_for(request, {"state": "busy" if active.is_set() else "ready" if ready.is_set() else "unloaded"}))
                    elif operation == "cancel":
                        target = request["payload"].get("target_request_id")
                        if not isinstance(target, str) or not target.isascii() or not target.isdecimal() or int(target) >= identifier:
                            raise ValueError("Invalid cancellation target")
                        with lock:
                            event = jobs.get(target)
                            if event:
                                event.set()
                        send(response_for(request, {"cancelled": event is not None}))
                    else:
                        with lock:
                            if len(jobs) >= MAX_PENDING:
                                send(response_for(request, error="Julia's request queue is full."))
                            else:
                                jobs[request["request_id"]] = threading.Event()
                                work.put_nowait(request)
                except (ValueError, TypeError, KeyError, UnicodeError, OverflowError, RecursionError, OSError):
                    send({"protocol_version": PROTOCOL_VERSION, "type": "error", "request_id": "0",
                          "ok": False, "error": "Julia rejected a malformed or incompatible protocol frame."})
                    exit_code[0] = 2
                    return 2
        except OSError:
            exit_code[0] = 2
            send({"protocol_version": PROTOCOL_VERSION, "type": "error", "request_id": "0",
                  "ok": False, "error": "Julia's local input pipe failed."})
        finally:
            stopping.set()
            with lock:
                for event in jobs.values():
                    event.set()
            try:
                work.put_nowait(None)
            except queue.Full:
                pass

    threading.Thread(target=read_requests, daemon=True, name="julia-control").start()
    execute()
    return exit_code[0]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--model-dir", required=True)
    args = parser.parse_args()
    sys.stdin.reconfigure(encoding="utf-8")
    PROTOCOL_OUT.reconfigure(encoding="utf-8")
    # Process-wide redirection prevents dependency prints from racing protocol I/O.
    sys.stdout = sys.stderr
    return serve(Path(args.model_dir).resolve(), sys.stdin.buffer)


if __name__ == "__main__":
    raise SystemExit(main())
