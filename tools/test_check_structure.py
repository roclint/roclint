"""Exercise structural failures in disposable source copies, without changing the repository."""
from pathlib import Path
import shutil
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import check_structure


class StructureGuardTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = Path(__file__).resolve().parents[1]
        cls.names = subprocess.run(
            ["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"],
            cwd=cls.source, capture_output=True, check=True,
        ).stdout.decode("utf-8").split("\0")

    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory(prefix="roclint-structure-test-")
        self.addCleanup(self.scratch.cleanup)
        self.root = Path(self.scratch.name)
        for name in self.names:
            source = self.source / name
            if name and source.is_file():
                target = self.root / name
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(source, target)

    def check(self, **kwargs):
        names = [path.relative_to(self.root).as_posix() for path in self.root.rglob("*") if path.is_file()]
        response = SimpleNamespace(stdout=("\0".join(names) + "\0").encode("utf-8"))
        with patch.object(check_structure.subprocess, "run", return_value=response):
            return check_structure.check(self.root, **kwargs)

    def test_current_source_passes(self):
        self.assertTrue(self.check()["passed"])

    def test_detailed_inventory_partitions_original_and_added_files(self):
        result = self.check(details=True)
        comparison = result["file_comparison"]
        self.assertEqual(result["original_paths"], result["modified_original_files"]
                         + result["unchanged_original_files"] + len(comparison["missing_original_paths"]))
        self.assertEqual(result["source_files"], result["preserved_original_paths"] + result["additions"])
        self.assertEqual(result["additions"], len(comparison["added_paths"]))
        self.assertIn("app/runtime", result["added_directories"])
        self.assertIn("app/host/JuliaRuntime.cpp", comparison["added_paths"])
        self.assertNotIn("file_comparison", self.check())

    def test_original_content_comparison_normalizes_checkout_line_endings(self):
        path = self.root / "LICENSE"
        before = self.check(details=True)["file_comparison"]
        self.assertIn("LICENSE", before["unchanged_original_paths"])
        path.write_bytes(path.read_bytes().replace(b"\r\n", b"\n").replace(b"\n", b"\r\n"))
        self.assertIn("LICENSE", self.check(details=True)["file_comparison"]["unchanged_original_paths"])
        path.write_bytes(path.read_bytes() + b"\r\nChanged in disposable fixture\r\n")
        result = self.check(details=True)
        self.assertTrue(result["passed"])
        self.assertIn("LICENSE", result["file_comparison"]["modified_original_paths"])

    def test_missing_original_path_fails(self):
        (self.root / "app/qml/shell/FloatingCard.qml").unlink()
        result = self.check()
        self.assertFalse(result["passed"])
        self.assertIn("Missing original path: app/qml/shell/FloatingCard.qml", result["errors"])

    def test_parallel_application_root_fails(self):
        path = self.root / "replacement_app/main.cpp"
        path.parent.mkdir()
        path.write_text("int main() {}", encoding="utf-8")
        self.assertTrue(any("Unreviewed top-level addition" in error for error in self.check()["errors"]))

    def test_unregistered_cpp_and_qml_fail(self):
        (self.root / "src/core/Unregistered.cpp").write_text("", encoding="utf-8")
        (self.root / "app/qml/shell/Unregistered.qml").write_text("", encoding="utf-8")
        errors = self.check()["errors"]
        self.assertIn("Source absent from CMakeLists.txt: src/core/Unregistered.cpp", errors)
        self.assertIn("QML absent from qml.qrc: app/qml/shell/Unregistered.qml", errors)

    def test_runtime_test_and_tool_sources_require_registration(self):
        paths = ("app/runtime/Unregistered.cpp", "tests/bridge/Unregistered.cpp", "tools/Unregistered.cpp")
        for name in paths:
            (self.root / name).write_text("", encoding="utf-8")
        errors = self.check()["errors"]
        for name in paths:
            self.assertIn(f"Source absent from CMakeLists.txt: {name}", errors)

    def test_unreviewed_directory_inside_existing_root_fails(self):
        path = self.root / "app/replacement_ui/README.md"
        path.parent.mkdir()
        path.write_text("replacement", encoding="utf-8")
        self.assertTrue(any("Unreviewed directory addition: app/replacement_ui" in error
                            for error in self.check()["errors"]))

    def test_changed_contract_fails(self):
        (self.root / "docs/contracts/C03_analysis_semantics.md").write_text("changed", encoding="utf-8")
        self.assertTrue(any("Preserved contract differs" in error for error in self.check()["errors"]))

    def test_checkout_line_endings_do_not_change_contract_content(self):
        path = self.root / "docs/contracts/C03_analysis_semantics.md"
        path.write_bytes(path.read_bytes().replace(b"\r\n", b"\n").replace(b"\n", b"\r\n"))
        self.assertTrue(self.check()["passed"])

    def test_changed_external_reference_fails(self):
        spec = self.root / "fixture-spec"
        spec.mkdir()
        # All expected source files are deliberately absent. No real specification is edited.
        result = self.check(spec_dir=spec)
        self.assertFalse(result["passed"])
        self.assertTrue(any("Specification source changed" in error for error in result["errors"]))


if __name__ == "__main__":
    unittest.main()
