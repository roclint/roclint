"""Check a complete local patch against upstream without changing the working index or refs."""
import argparse
import datetime
import json
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--upstream", default="origin/main")
    parser.add_argument("--fetch", action="store_true", help="Fetch origin before taking the upstream snapshot")
    parser.add_argument("--output", type=Path, default=Path("build/merge-preview/latest.json"))
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]

    def git(*arguments, env=None, check=True):
        return subprocess.run(["git", *arguments], cwd=root, env=env, check=check, capture_output=True)

    if args.fetch:
        fetch = git("fetch", "origin", check=False)
        if fetch.returncode:
            sys_error = fetch.stderr.decode("utf-8", errors="replace")
            raise SystemExit("Could not refresh upstream; merge status is not established.\n" + sys_error)
    base = git("rev-parse", "HEAD").stdout.decode().strip()
    upstream = git("rev-parse", "--verify", args.upstream + "^{commit}").stdout.decode().strip()
    with tempfile.TemporaryDirectory(prefix="roclint-merge-preview-") as scratch:
        environment = os.environ.copy()
        environment["GIT_INDEX_FILE"] = str(Path(scratch) / "index")
        environment.update(GIT_AUTHOR_NAME="Roclint merge preview", GIT_AUTHOR_EMAIL="merge-preview@localhost",
                           GIT_COMMITTER_NAME="Roclint merge preview", GIT_COMMITTER_EMAIL="merge-preview@localhost")
        git("read-tree", "HEAD", env=environment)
        git("add", "-A", "--", ".", env=environment)
        tree = git("write-tree", env=environment).stdout.decode().strip()
        # An unnamed temporary commit object provides ancestry to Git's actual merge algorithm.
        snapshot = git("commit-tree", tree, "-p", base, "-m", "Temporary complete-working-tree merge preview",
                       env=environment).stdout.decode().strip()
        merge = git("merge-tree", "--write-tree", "--messages", "--name-only", upstream, snapshot, check=False)
        if merge.returncode not in (0, 1):
            raise SystemExit("Git could not evaluate this merge:\n" + merge.stderr.decode("utf-8", errors="replace"))
        changed = git("diff", "--name-only", "-z", upstream, tree).stdout.decode("utf-8").rstrip("\0").split("\0")
        changed = [name for name in changed if name]
        patch = git("diff", "--binary", upstream, tree).stdout
        patch_path = Path(scratch) / "complete.patch"
        patch_path.write_bytes(patch)
        git("read-tree", upstream, env=environment)
        apply_check = git("apply", "--cached", "--check", str(patch_path), env=environment, check=False) if patch else None
        result = {"checked_at_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
                  "upstream_ref": args.upstream, "upstream_commit": upstream, "local_head": base,
                  "working_tree": tree, "temporary_snapshot": snapshot,
                  "clean_merge": merge.returncode == 0, "changed_file_count": len(changed),
                  "applies_to_upstream": apply_check is None or apply_check.returncode == 0,
                  "changed_files": changed, "git_merge_output": merge.stdout.decode("utf-8", errors="replace"),
                  "scope": "Git merge mechanics only; does not establish feature parity, runtime correctness or future mergeability"}
    output = args.output if args.output.is_absolute() else root / args.output
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    delivery = output.with_name("complete.patch")
    delivery.write_bytes(patch)
    print(f"Upstream: {upstream}")
    print(f"Complete patch: {len(changed)} changed/new files")
    print("Merge: " + ("CLEAN at this upstream revision" if result["clean_merge"] else "CONFLICTS; inspect report"))
    print(f"Report: {output}")
    print(f"Complete patch: {delivery}")
    return merge.returncode if result["applies_to_upstream"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
