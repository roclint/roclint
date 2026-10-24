"""Prepare a reproducible-location Julia environment without changing global Python packages."""
import argparse
from pathlib import Path
import subprocess
import sys
import venv


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    parser.add_argument("--reuse-system-packages", action="store_true",
                        help="Explicitly reuse installed packages; default is an isolated environment")
    args = parser.parse_args()
    if sys.version_info < (3, 11):
        parser.error("Julia's integrity helper requires Python 3.11 or newer")
    root = Path(__file__).resolve().parents[1]
    build = args.build_dir if args.build_dir.is_absolute() else root / args.build_dir
    environment = build / "runtime" / "julia-python"
    if not (environment / "pyvenv.cfg").exists():
        venv.EnvBuilder(with_pip=True, system_site_packages=args.reuse_system_packages).create(environment)
    python = environment / ("Scripts/python.exe" if sys.platform == "win32" else "bin/python3")
    subprocess.run([str(python), "-m", "pip", "install", "--upgrade", "pip"], check=True)
    # CPU wheels avoid an unnecessary GPU toolkit download for this CPU-only scorer.
    subprocess.run([str(python), "-m", "pip", "install", "torch>=2.6", "--index-url",
                    "https://download.pytorch.org/whl/cpu"], check=True)
    subprocess.run([str(python), "-m", "pip", "install", "-r",
                    str(root / "app/runtime/julia-requirements.txt")], check=True)
    print(f"Julia Python environment: {python}")
    print("Build the host, then select this Python executable in Julia settings if it is not detected automatically.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
