"""Exercise real host SIGKILL cleanup with isolated, deliberately uncooperative children."""
import argparse
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parent.parent
HELPER = ROOT / "build/roclint_ipc_hardening_tests"


def alive(pid):
    result = subprocess.run(["ps", "-p", str(pid), "-o", "stat="], capture_output=True, text=True, check=False)
    return bool(result.stdout.strip()) and not result.stdout.lstrip().startswith("Z")


class GuardianTests(unittest.TestCase):
    @unittest.skipUnless(os.name == "posix", "POSIX guardian; Windows uses Job Objects")
    def test_host_sigkill_reaps_worker_and_kills_descendant(self):
        if not HELPER.is_file():
            self.skipTest("Build the IPC hardening fixture first")
        with tempfile.TemporaryDirectory(prefix="roclint-guardian-") as temporary:
            environment = dict(os.environ, QT_QPA_PLATFORM="offscreen")
            parent = subprocess.Popen([str(HELPER), "--owned-parent", temporary], stdout=subprocess.PIPE,
                                      stderr=subprocess.PIPE, env=environment, start_new_session=True)
            pids = []
            try:
                paths = [Path(temporary) / name for name in ("worker.pid", "descendant.pid")]
                deadline = time.monotonic() + 8
                while time.monotonic() < deadline and (not all(p.exists() and p.stat().st_size for p in paths)):
                    if parent.poll() is not None:
                        self.fail("Fixture parent exited before starting its owned worker")
                    time.sleep(0.02)
                self.assertTrue(all(p.exists() for p in paths), "Owned worker did not start")
                pids = [int(p.read_text()) for p in paths]
                self.assertTrue(all(alive(pid) for pid in pids))
                parent.kill()
                parent.wait(timeout=3)
                deadline = time.monotonic() + 6
                while time.monotonic() < deadline and (any(alive(pid) for pid in pids) or subprocess.run(
                        ["ps", "-p", str(pids[0]), "-o", "stat="], capture_output=True, text=True, check=False).stdout.strip()):
                    time.sleep(0.025)
                self.assertFalse(any(alive(pid) for pid in pids), "Owned processes survived host SIGKILL")
                self.assertFalse(subprocess.run(["ps", "-p", str(pids[0]), "-o", "stat="],
                                               capture_output=True, text=True, check=False).stdout.strip(), "Worker was not reaped")
            finally:
                if parent.poll() is None:
                    parent.kill()
                parent.wait(timeout=3)
                # Only kill still-live fixture processes whose executable identity is verified.
                for pid in pids:
                    command = subprocess.run(["ps", "-p", str(pid), "-o", "command="], capture_output=True, text=True, check=False)
                    if str(HELPER) in command.stdout:
                        try:
                            os.kill(pid, signal.SIGKILL)
                        except ProcessLookupError:
                            pass
                parent.communicate(timeout=3)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--helper", type=Path)
    options, remainder = parser.parse_known_args()
    if options.helper:
        HELPER = options.helper
    unittest.main(argv=[sys.argv[0], *remainder])
