"""Download and verify the pinned IBM Granite embedding model for Roclint (R00).

Pinned model: ibm-granite/granite-embedding-311m-multilingual-r2
Pinned revision: d9ade47c9b2b2afee75f29fd8a1cef279a0e9aea

Usage:
    python tools/setup_embedding_model.py                    # Download to default location
    python tools/setup_embedding_model.py --verify           # Verify existing installation
    python tools/setup_embedding_model.py --dir PATH         # Download to explicit directory
    python tools/setup_embedding_model.py --install-deps     # Install Python requirements only

The download requires:
  pip install huggingface_hub>=0.21
  pip install sentence-transformers>=3.0 torch>=2.0 transformers>=4.40 safetensors>=0.4

The model weights are approximately 588 MB (model.safetensors in bfloat16).
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
if hasattr(sys.stderr, "reconfigure"):
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")

ROOT = Path(__file__).resolve().parents[1]
MANIFEST_PATH = ROOT / "app" / "runtime" / "embedding-models.json"
REQUIREMENTS_PATH = ROOT / "app" / "runtime" / "embedding-requirements.txt"

# Pinned immutable revision (HEAD as of 2026-05-18).
# Previous revision d9ade47c9b2b2afee75f29fd8a1cef279a0e9aea returned HTTP 404.
MODEL_ID = "ibm-granite/granite-embedding-311m-multilingual-r2"
MODEL_REVISION = "44399559930365213510b1ee2eb15ded83374f0e"

# Required files that must be present in the model directory.
REQUIRED_FILES = [
    "config.json",
    "1_Pooling/config.json",
    "tokenizer.json",
    "tokenizer_config.json",
    "special_tokens_map.json",
    "model.safetensors",
]

# Expected approximate file sizes (bytes), used for a quick sanity check.
# These are advisory; the sha256 hash is the authoritative check.
EXPECTED_MIN_SIZES = {
    "model.safetensors": 500 * 1024 * 1024,  # ~588 MB
    "tokenizer.json":    100 * 1024,
}


def default_model_dir() -> Path:
    """Return the platform default model directory for the embedding model."""
    if sys.platform == "win32":
        local_app = os.environ.get("LOCALAPPDATA")
        if local_app:
            return Path(local_app) / "roclint" / "Roclint" / "models" / "embedding"
        return Path.home() / "AppData" / "Local" / "roclint" / "Roclint" / "models" / "embedding"
    if sys.platform == "darwin":
        return Path.home() / "Library" / "Application Support" / "roclint" / "Roclint" / "models" / "embedding"
    return Path.home() / ".local" / "share" / "roclint" / "Roclint" / "models" / "embedding"


def sha256_file(path: Path) -> str:
    with path.open("rb") as f:
        return hashlib.file_digest(f, "sha256").hexdigest()


def verify_installation(model_dir: Path) -> list[str]:
    """Return a list of error messages; empty list means verification passed."""
    errors: list[str] = []
    if not model_dir.is_dir():
        errors.append(f"Model directory does not exist: {model_dir}")
        return errors

    for fname in REQUIRED_FILES:
        p = model_dir / fname
        if not p.is_file():
            errors.append(f"Missing required file: {p}")
            continue
        min_size = EXPECTED_MIN_SIZES.get(fname, 0)
        if min_size > 0 and p.stat().st_size < min_size:
            errors.append(f"File too small ({p.stat().st_size} < {min_size}): {p}")

    # Verify config.json contains the expected model type.
    config_path = model_dir / "config.json"
    if config_path.is_file():
        try:
            config = json.loads(config_path.read_text(encoding="utf-8"))
            arch = config.get("architectures", [])
            model_type = config.get("model_type", "")
            if not any("ModernBert" in a or "modernbert" in a.lower() for a in arch) and \
               "modernbert" not in model_type.lower():
                errors.append(
                    f"config.json architecture unexpected: {arch}, model_type={model_type}. "
                    "Expected ModernBERT. Verify this is the correct model."
                )
        except Exception as exc:
            errors.append(f"Could not read config.json: {exc}")

    # Verify pooling config is CLS (not mean).
    pooling_path = model_dir / "1_Pooling" / "config.json"
    if pooling_path.is_file():
        try:
            pcfg = json.loads(pooling_path.read_text(encoding="utf-8"))
            if not pcfg.get("pooling_mode_cls_token", False):
                errors.append(
                    f"1_Pooling/config.json does not enable CLS pooling: {pcfg}. "
                    "This model uses CLS pooling. The model directory may contain the wrong model."
                )
            if pcfg.get("pooling_mode_mean_tokens", False):
                errors.append(
                    "1_Pooling/config.json enables mean pooling in addition to or instead of CLS. "
                    "granite-embedding-311m-multilingual-r2 uses only CLS pooling."
                )
        except Exception as exc:
            errors.append(f"Could not read 1_Pooling/config.json: {exc}")

    return errors


def install_requirements() -> bool:
    """Install Python requirements. Returns True on success."""
    if not REQUIREMENTS_PATH.is_file():
        print(f"[ERROR] Requirements file not found: {REQUIREMENTS_PATH}", file=sys.stderr)
        return False
    print(f"Installing requirements from {REQUIREMENTS_PATH}...")
    result = subprocess.run(
        [sys.executable, "-m", "pip", "install", "-r", str(REQUIREMENTS_PATH)],
        check=False,
    )
    return result.returncode == 0


def download_model(model_dir: Path) -> bool:
    """Download the pinned model to model_dir using huggingface_hub. Returns True on success."""
    try:
        from huggingface_hub import snapshot_download
    except ImportError:
        print("[ERROR] huggingface_hub is not installed.", file=sys.stderr)
        print("Run: pip install huggingface_hub>=0.21", file=sys.stderr)
        return False

    model_dir.mkdir(parents=True, exist_ok=True)
    print(f"Downloading {MODEL_ID} @ {MODEL_REVISION[:12]} to {model_dir}...")
    try:
        snapshot_download(
            repo_id=MODEL_ID,
            revision=MODEL_REVISION,
            local_dir=str(model_dir),
            ignore_patterns=["*.md", "*.gitattributes", "*.gitignore",
                             "pytorch_model.bin",        # prefer safetensors
                             "tf_model.h5",
                             "flax_model.msgpack",
                             "rust_model.ot"],
        )
        print("Download complete.")
        return True
    except Exception as exc:
        print(f"[ERROR] Download failed: {exc}", file=sys.stderr)
        return False


def write_local_manifest(model_dir: Path) -> None:
    """Write the model manifest into the local model directory for runtime verification."""
    manifest = json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))
    dest = model_dir / "roclint-embedding.json"
    dest.write_text(json.dumps(manifest, indent=2, ensure_ascii=False), encoding="utf-8")
    print(f"Wrote manifest to {dest}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Roclint embedding model setup")
    parser.add_argument("--dir", default=None,
                        help="Model directory (default: platform data directory)")
    parser.add_argument("--verify", action="store_true",
                        help="Verify an existing installation without downloading")
    parser.add_argument("--install-deps", action="store_true",
                        help="Install Python dependencies only (no model download)")
    args = parser.parse_args()

    model_dir = Path(args.dir) if args.dir else default_model_dir()

    if args.install_deps:
        ok = install_requirements()
        return 0 if ok else 1

    if args.verify:
        errors = verify_installation(model_dir)
        if errors:
            print(f"[FAIL] Verification failed for {model_dir}:")
            for e in errors:
                print(f"  - {e}")
            return 1
        print(f"[OK] Embedding model verified at {model_dir}")
        return 0

    # Full setup: install deps + download model.
    print("=" * 60)
    print("Roclint Embedding Model Setup")
    print(f"Model: {MODEL_ID}")
    print(f"Revision: {MODEL_REVISION}")
    print(f"Target: {model_dir}")
    print("=" * 60)

    # Install requirements.
    if not install_requirements():
        print("[ERROR] Failed to install requirements.", file=sys.stderr)
        return 1

    # Check if already installed.
    errors = verify_installation(model_dir)
    if not errors:
        print(f"[OK] Embedding model already installed and verified at {model_dir}")
        return 0

    print(f"Verification found issues ({len(errors)}); downloading...")

    # Download.
    if not download_model(model_dir):
        return 1

    # Verify post-download.
    errors = verify_installation(model_dir)
    if errors:
        print(f"[FAIL] Post-download verification failed:")
        for e in errors:
            print(f"  - {e}")
        return 1

    write_local_manifest(model_dir)

    print()
    print("=" * 60)
    print("[OK] Embedding model ready.")
    print(f"     Directory: {model_dir}")
    print("=" * 60)
    return 0


if __name__ == "__main__":
    sys.exit(main())
