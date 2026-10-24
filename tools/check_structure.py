"""Check upstream layout continuity and recorded specification sources without editing files."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import subprocess
import xml.etree.ElementTree as ET


def digest(path, normalize=False):
    data = path.read_bytes()
    if normalize:
        data = data.replace(b"\r\n", b"\n")
    return hashlib.sha256(data).hexdigest()


def check(root, baseline_dir=None, spec_dir=None, details=False):
    manifest = json.loads((root / "docs/alignment_baseline.json").read_text(encoding="utf-8"))
    files = subprocess.run(
        ["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"],
        cwd=root, check=True, capture_output=True,
    ).stdout.decode("utf-8").split("\0")
    current = {name for name in files if name and (root / name).is_file()}
    original = manifest["upstream"]["files_lf_sha256"]
    errors = []
    missing = sorted(set(original) - current)
    errors.extend(f"Missing original path: {name}" for name in missing)
    modified = sorted(name for name in set(original) & current
                      if digest(root / name, normalize=True) != original[name])
    unchanged = sorted((set(original) & current) - set(modified))
    added = sorted(current - set(original))

    allowed_roots = set(manifest["allowed_top_level_directories"])
    allowed_files = set(manifest["allowed_top_level_files"])
    allowed_directories = set(manifest["allowed_directories"])
    for name in sorted(current - set(original)):
        parts = PurePosixPath(name).parts
        if (len(parts) == 1 and name not in allowed_files) or (len(parts) > 1 and parts[0] not in allowed_roots):
            errors.append(f"Unreviewed top-level addition: {name}; keep additions within the existing module layout")
    directories = {parent.as_posix() for name in current for parent in PurePosixPath(name).parents if str(parent) != "."}
    original_directories = {parent.as_posix() for name in original for parent in PurePosixPath(name).parents
                            if str(parent) != "."}
    added_directories = sorted(directories - original_directories)
    errors.extend(f"Unreviewed directory addition: {name}; review the module boundary before updating the manifest"
                  for name in sorted(directories - allowed_directories))

    # These are source-registration checks, not a substitute for compiling CMake/QML.
    cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    for name in sorted(current):
        if name.startswith(("src/", "app/host/", "app/runtime/", "tests/", "tools/")) and Path(name).suffix in {".cpp", ".mm"}:
            if name not in cmake:
                errors.append(f"Source absent from CMakeLists.txt: {name}")
    qrc_path = root / "app/qml/shell/qml.qrc"
    resources = {node.text for node in ET.parse(qrc_path).iter("file")}
    for name in sorted(current):
        path = PurePosixPath(name)
        if path.parent == PurePosixPath("app/qml/shell") and path.suffix == ".qml" and path.name not in resources:
            errors.append(f"QML absent from qml.qrc: {name}")
    for resource in resources:
        if not resource or not (qrc_path.parent / resource).is_file():
            errors.append(f"Missing QML resource: {resource}")

    contracts = json.loads((root / "docs/contracts/SOURCE.json").read_text(encoding="utf-8"))["files"]
    for name, expected in contracts.items():
        path = root / "docs/contracts" / name
        # Git may convert the preserved LF source cards to CRLF on Windows checkout.
        if not path.is_file() or digest(path, normalize=True) != expected:
            errors.append(f"Preserved contract differs from its recorded source: {name}")

    if baseline_dir is not None:
        actual = {path.relative_to(baseline_dir).as_posix() for path in baseline_dir.rglob("*") if path.is_file()}
        if actual != set(original):
            errors.append("Supplied upstream snapshot path inventory differs from the recorded baseline")
        for name, expected in original.items():
            path = baseline_dir / name
            if not path.is_file() or digest(path, normalize=True) != expected:
                errors.append(f"Supplied upstream snapshot differs: {name}")
    if spec_dir is not None:
        for name, expected in manifest["vision"]["files_sha256"].items():
            path = spec_dir / name
            if not path.is_file() or digest(path) != expected:
                errors.append(f"Specification source changed; repeat vision review: {name}")

    result = {
        "passed": not errors,
        "original_paths": len(original),
        "preserved_original_paths": len(original) - len(missing),
        "modified_original_files": len(modified),
        "unchanged_original_files": len(unchanged),
        "source_files": len(current),
        "additions": len(added),
        "added_directories": added_directories,
        "upstream_commit": manifest["upstream"]["commit"],
        "external_snapshot_checked": baseline_dir is not None,
        "external_vision_sources_checked": spec_dir is not None,
        "errors": errors,
        "scope": "Path continuity, source registration and specification integrity; not semantic compliance or runtime correctness",
    }
    if details:
        result["file_comparison"] = {
            "modified_original_paths": modified,
            "unchanged_original_paths": unchanged,
            "missing_original_paths": missing,
            "added_paths": added,
        }
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--baseline-dir", type=Path, help="Extracted original roclint-main snapshot")
    parser.add_argument("--spec-dir", type=Path, help="Extracted Stardust_v4_2_with_RAG directory")
    parser.add_argument("--details", action="store_true", help="Include the file-by-file comparison with the original snapshot")
    args = parser.parse_args()
    try:
        result = check(args.root.resolve(), args.baseline_dir, args.spec_dir, details=args.details)
    except (OSError, ValueError, ET.ParseError, subprocess.CalledProcessError) as error:
        print(json.dumps({"passed": False, "errors": [str(error)]}, indent=2))
        return 2
    print(json.dumps(result, indent=2))
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
