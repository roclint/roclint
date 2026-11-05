"""Download and verify all pinned Roclint models (Julia-1 S1 and Granite-4.2-3B S2) on Windows/macOS/Linux.

This provisions the exact pinned model weights required for Roclint's decoupled
two-model architecture:
- S1: Julia-1 Triage Classifier (ModernBERT FP32 safetensors)
- S2: Granite-4.2-3B (GGUF Q8_0 Vulkan/Metal runtime)

Use ``--verify`` to validate an existing installation without downloading or
creating a virtual environment.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
if hasattr(sys.stderr, "reconfigure"):
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")

ROOT = Path(__file__).resolve().parents[1]
GRANITE_MANIFEST = ROOT / "app" / "runtime" / "granite-models.json"

# Pinned Julia-1 Checkpoint
JULIA_MODEL_ID = "SupersonicLabs/Julia-1"
JULIA_REVISION = "a85b127321d580d65176c89ced8273f305745d85"
JULIA_WEIGHTS_SHA256 = "df853bf7fe424420011f3d0c47a05d7341aa9eefa7fb9f203ea4aada4ad95b72"


def sha256_file(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def get_default_models_dir() -> Path:
    if sys.platform == "win32":
        local_app_data = os.environ.get("LOCALAPPDATA")
        if local_app_data:
            return Path(local_app_data) / "roclint" / "Roclint" / "models"
        return Path.home() / "AppData" / "Local" / "roclint" / "Roclint" / "models"
    if sys.platform == "darwin":
        return Path.home() / "Library" / "Application Support" / "roclint" / "Roclint" / "models"
    return Path.home() / ".local" / "share" / "roclint" / "Roclint" / "models"


def choose_models_dir(explicit: Path | None) -> Path:
    if explicit is not None:
        return explicit
    local = ROOT / "models"
    if (local / "granite-4.2-3b").is_dir() or (local / "julia-1").is_dir() or (local / "s2").is_dir():
        return local
    return get_default_models_dir()


def model_directories(models_dir: Path) -> tuple[Path, Path]:
    julia_dir = models_dir / "julia-1"
    if not julia_dir.is_dir():
        julia_dir = models_dir / "s1"
    granite_dir = models_dir / "granite-4.2-3b"
    if not granite_dir.is_dir():
        granite_dir = models_dir / "s2"
    return julia_dir, granite_dir


def load_granite_manifest(target_dir: Path) -> dict:
    manifest_path = target_dir / "roclint-granite.json"
    if not manifest_path.is_file():
        manifest_path = GRANITE_MANIFEST
    return json.loads(manifest_path.read_text(encoding="utf-8"))


def verify_granite(target_dir: Path, quantization: str) -> list[str]:
    errors: list[str] = []
    manifest = load_granite_manifest(target_dir)
    selected = [candidate for candidate in manifest["candidates"]
                if quantization == "all" or candidate["quantization"] == quantization]
    if not selected:
        return [f"No pinned Granite candidate matches quantization {quantization!r}"]
    for candidate in selected:
        path = target_dir / candidate["filename"]
        if not path.is_file():
            errors.append(f"Missing Granite candidate: {path}")
            continue
        size = path.stat().st_size
        if size != candidate["size_bytes"]:
            errors.append(f"Granite size mismatch for {path.name}: {size} != {candidate['size_bytes']}")
            continue
        digest = sha256_file(path)
        if digest != candidate["sha256"]:
            errors.append(f"Granite SHA-256 mismatch for {path.name}")
            continue
        print(f"[OK] Granite {candidate['quantization']} checksum {digest} ({path})")
    return errors


def verify_julia(target_dir: Path) -> list[str]:
    errors: list[str] = []
    manifest_path = target_dir / "roclint-julia.json"
    files: dict[str, str]
    if manifest_path.is_file():
        files = json.loads(manifest_path.read_text(encoding="utf-8")).get("files", {})
    else:
        files = {"model.safetensors": JULIA_WEIGHTS_SHA256}
    if "model.safetensors" not in files:
        files["model.safetensors"] = JULIA_WEIGHTS_SHA256
    for relative, expected in sorted(files.items()):
        path = target_dir / relative
        if not path.is_file():
            errors.append(f"Missing Julia file: {path}")
            continue
        digest = sha256_file(path)
        if digest != expected:
            errors.append(f"Julia SHA-256 mismatch for {relative}")
            continue
        print(f"[OK] Julia checksum {digest} ({path})")
    return errors


def get_or_create_julia_venv(build_dir: Path) -> Path:
    """Ensure build/runtime/julia-python exists and has required dependencies."""
    venv_dir = build_dir / "runtime" / "julia-python"
    python_exe = venv_dir / ("Scripts" if sys.platform == "win32" else "bin") / (
        "python.exe" if sys.platform == "win32" else "python3")

    if not python_exe.exists():
        print(f"[*] Setting up Julia Python virtual environment in {venv_dir}...")
        setup_script = ROOT / "tools" / "setup_julia.py"
        subprocess.run([sys.executable, str(setup_script), "--build-dir", str(build_dir)], check=True)

    if not python_exe.exists():
        raise RuntimeError(f"Failed to create or locate Julia Python at {python_exe}")

    return python_exe


def download_julia(target_dir: Path, venv_python: Path):
    """Download and verify Julia-1 using the prepared virtual environment."""
    target_dir.mkdir(parents=True, exist_ok=True)
    safetensors = target_dir / "model.safetensors"
    manifest_file = target_dir / "roclint-julia.json"

    if safetensors.exists() and manifest_file.exists():
        if not verify_julia(target_dir):
            print(f"[OK] Julia-1 model is already installed and verified at {target_dir}")
            return
        print("[!] Existing Julia weights failed hash check; re-downloading...")

    print(f"[*] Downloading Julia-1 classifier to {target_dir} (about 600 MB)...")
    download_code = f"""
from pathlib import Path
import json, hashlib
from huggingface_hub import snapshot_download

root = Path(r'{target_dir}')
snapshot_download(
    '{JULIA_MODEL_ID}',
    revision='{JULIA_REVISION}',
    local_dir=str(root),
    allow_patterns=[
        'README.md', 'config.json', 'julia_config.json', 'inference-policy.json',
        'model.safetensors', 'encoder/*.json', 'tokenizer/*.json',
        'julia/*.py', 'julia/router/*.py'
    ]
)

weights_file = root / 'model.safetensors'
with weights_file.open('rb') as f:
    digest = hashlib.file_digest(f, 'sha256').hexdigest()

if digest != '{JULIA_WEIGHTS_SHA256}':
    raise ValueError('Julia weights failed SHA-256 integrity check')

manifest = {{'model_id': '{JULIA_MODEL_ID}', 'revision': '{JULIA_REVISION}', 'weights_sha256': '{JULIA_WEIGHTS_SHA256}', 'files': {{}}}}
for path in sorted(root.rglob('*')):
    if path.is_file() and '.cache' not in path.parts and '__pycache__' not in path.parts and path.name != 'roclint-julia.json':
        with path.open('rb') as f:
            manifest['files'][path.relative_to(root).as_posix()] = hashlib.file_digest(f, 'sha256').hexdigest()

(root / 'roclint-julia.json').write_text(json.dumps(manifest, indent=2) + '\\n', encoding='utf-8')
print('[OK] Julia-1 download and SHA-256 verification complete.')
"""
    subprocess.run([str(venv_python), "-c", download_code], check=True)


def download_granite(target_dir: Path, quantization: str):
    """Download and verify native Granite GGUF candidate using setup_granite.py."""
    target_dir.mkdir(parents=True, exist_ok=True)
    setup_script = ROOT / "tools" / "setup_granite.py"
    print(f"[*] Downloading / verifying native Granite 4.2-3B ({quantization}) to {target_dir}...")
    cmd = [
        sys.executable,
        str(setup_script),
        "--model-dir", str(target_dir),
        "--quantization", quantization
    ]
    subprocess.run(cmd, check=True)


def update_windows_registry(models_base: Path, julia_dir: Path, granite_dir: Path):
    if sys.platform != "win32":
        return
    import winreg
    try:
        models_str = str(models_base).replace("\\", "/")
        julia_str = str(julia_dir).replace("\\", "/")
        granite_str = str(granite_dir).replace("\\", "/")

        with winreg.CreateKey(winreg.HKEY_CURRENT_USER, r"Software\roclint\Roclint\models") as key:
            winreg.SetValueEx(key, "baseDirectory", 0, winreg.REG_SZ, models_str)
            winreg.SetValueEx(key, "nativeDirectory", 0, winreg.REG_SZ, granite_str)
            winreg.SetValueEx(key, "active", 0, winreg.REG_SZ, "native-granite:Q8_0")

        with winreg.CreateKey(winreg.HKEY_CURRENT_USER, r"Software\roclint\Roclint\scoring") as key:
            winreg.SetValueEx(key, "juliaDirectory", 0, winreg.REG_SZ, julia_str)

        print("[OK] Updated Windows Registry model paths successfully.")
    except Exception as e:
        print(f"[!] Note: Could not update registry automatically: {e}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--models-dir", type=Path, default=None,
                        help="Root directory for models (defaults to the local models/ fallback, then the system AppData directory)")
    parser.add_argument("--quantization", choices=["Q8_0", "Q5_K_M", "Q4_K_M", "all"], default="Q8_0",
                        help="Granite quantization to download or verify (default: Q8_0)")
    parser.add_argument("--skip-julia", action="store_true", help="Skip Julia-1 download")
    parser.add_argument("--skip-granite", action="store_true", help="Skip Granite download")
    parser.add_argument("--verify", action="store_true",
                        help="Only verify existing local weights and checksums; never download or create a venv")
    args = parser.parse_args()

    models_dir = choose_models_dir(args.models_dir)
    julia_dir, granite_dir = model_directories(models_dir)
    build_dir = ROOT / "build"

    print("==================================================")
    print("  Roclint Model Downloader & Verification (v3)")
    print("==================================================")
    print(f"Models Directory : {models_dir}")
    print(f"S1 (Julia-1)     : {julia_dir}")
    print(f"S2 (Granite-3B)  : {granite_dir} ({args.quantization})")
    print("==================================================")

    if args.verify:
        errors: list[str] = []
        if not args.skip_julia:
            errors.extend(verify_julia(julia_dir))
        if not args.skip_granite:
            errors.extend(verify_granite(granite_dir, args.quantization))
        if errors:
            print("\n[FAIL] Model verification failed:")
            for error in errors:
                print("  - " + error)
            return 1
        print("\n[OK] All selected model weights are present and match their pinned SHA-256 digests.")
        return 0

    if not args.skip_julia:
        venv_python = get_or_create_julia_venv(build_dir)
        download_julia(julia_dir, venv_python)

    if not args.skip_granite:
        download_granite(granite_dir, args.quantization)

    update_windows_registry(models_dir, julia_dir, granite_dir)

    print("\n[OK] All required models are provisioned and verified.")
    print("    You can now launch Roclint with start_roclint.bat.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
