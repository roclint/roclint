"""Explicitly provision pinned native Granite benchmark candidates; never run inference or change user settings."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import time
import urllib.request


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def prepare_runtime(path, manifest):
    repository = manifest["runtime_repository"]
    revision = manifest["runtime_revision"]
    if (path / ".git").exists():
        def git(*args):
            return subprocess.check_output(["git", "-C", str(path), *args], text=True).strip()
        if git("remote", "get-url", "origin") != repository or git("rev-parse", "HEAD") != revision or git("status", "--porcelain"):
            raise RuntimeError("Existing llama.cpp checkout differs from the pinned clean source; it was left unchanged")
        return
    if path.exists() and any(path.iterdir()):
        raise RuntimeError("Runtime source directory is not empty; it was left unchanged")
    path.mkdir(parents=True, exist_ok=True)
    for args in [["init", str(path)], ["-C", str(path), "remote", "add", "origin", repository],
                 ["-C", str(path), "fetch", "--depth", "1", "origin", revision],
                 ["-C", str(path), "checkout", "--detach", "FETCH_HEAD"]]:
        subprocess.run(["git", *args], check=True)


def download(directory, manifest, candidate):
    target = directory / candidate["filename"]
    size, expected = candidate["size_bytes"], candidate["sha256"]
    if target.exists():
        if target.stat().st_size == size and digest(target) == expected:
            print("Verified existing " + target.name, flush=True)
            return
        raise RuntimeError("Existing model failed integrity verification; choose another destination or explicitly repair it before retrying")
    partial = target.with_suffix(target.suffix + ".part")
    offset = partial.stat().st_size if partial.exists() else 0
    if offset > size:
        raise RuntimeError("Partial model is larger than its pinned size; it was left unchanged")
    if shutil.disk_usage(directory).free < size - offset + 256 * 1024 * 1024:
        raise RuntimeError("Not enough free space for the pinned model and download margin")
    if offset != size:
        url = "https://huggingface.co/{}/resolve/{}/{}".format(manifest["model_family"], manifest["model_revision"], candidate["filename"])
        headers = {"Range": "bytes=%d-" % offset} if offset else {}
        with urllib.request.urlopen(urllib.request.Request(url, headers=headers), timeout=60) as reply:
            if offset and reply.status == 206:
                if not reply.headers.get("Content-Range", "").startswith("bytes %d-" % offset):
                    raise RuntimeError("Download resume offset did not match the partial file")
            elif offset:
                offset = 0  # Server returned the complete object; restart only the owned partial file.
            announced = reply.headers.get("Content-Length")
            if announced and int(announced) != size - offset:
                raise RuntimeError("Download size differs from the pinned artifact")
            received, previous = offset, time.monotonic()
            with partial.open("ab" if offset else "wb") as stream:
                while chunk := reply.read(1024 * 1024):
                    received += len(chunk)
                    if received > size:
                        raise RuntimeError("Download exceeded its pinned size")
                    stream.write(chunk)
                    if time.monotonic() - previous >= 5:
                        print("%s: %.1f / %.1f MiB" % (target.name, received / 2**20, size / 2**20), flush=True)
                        previous = time.monotonic()
    if partial.stat().st_size != size or digest(partial) != expected:
        raise RuntimeError("Downloaded model failed its pinned size/SHA-256 check; the partial file was retained")
    partial.replace(target)
    print("Verified and installed " + target.name, flush=True)


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model-dir", type=Path)
    parser.add_argument("--runtime-only", action="store_true", help="Fetch/verify build sources without downloading weights")
    parser.add_argument("--quantization", choices=["Q4_K_M", "Q5_K_M", "Q8_0", "all"], help="Defaults to the pinned manifest's Q8_0 selection")
    parser.add_argument("--fetch-runtime", action="store_true")
    parser.add_argument("--runtime-source", type=Path, default=root / "build/deps/llama.cpp")
    args = parser.parse_args()
    manifest = json.loads((root / "app/runtime/granite-models.json").read_text(encoding="utf-8"))
    args.quantization = args.quantization or manifest["default_quantization"]
    if args.fetch_runtime or args.runtime_only:
        prepare_runtime(args.runtime_source.resolve(), manifest)
    if args.runtime_only:
        return
    if args.model_dir is None:
        parser.error("--model-dir is required when provisioning weights")
    directory = args.model_dir.resolve()
    directory.mkdir(parents=True, exist_ok=True)
    for candidate in manifest["candidates"]:
        if args.quantization in {"all", candidate["quantization"]}:
            download(directory, manifest, candidate)
    (directory / "roclint-granite.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print("Provisioned pinned candidates. Quantization selection still requires the recorded benchmark.", flush=True)


if __name__ == "__main__":
    main()
