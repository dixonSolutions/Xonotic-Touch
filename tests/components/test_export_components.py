import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("export_components", ROOT / "scripts/export-components.py")
exporter = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(exporter)


OLD = "1" * 40
NEW = "2" * 40


def archive(files, special=None):
    """Build an in-memory GitHub-shaped tarball fixture."""
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w:gz") as tar:
        top = tarfile.TarInfo("Godnotic-darkplaces-commit/")
        top.type = tarfile.DIRTYPE
        tar.addfile(top)
        for name, content in files.items():
            info = tarfile.TarInfo(f"Godnotic-darkplaces-commit/{name}")
            info.mode = 0o755 if name.endswith(".sh") else 0o644
            info.size = len(content)
            tar.addfile(info, io.BytesIO(content))
        for info in special or []:
            info.name = "Godnotic-darkplaces-commit/" + info.name
            payload = io.BytesIO(b"x") if info.isfile() and info.size else None
            tar.addfile(info, payload)
    buf.seek(0)
    return buf


class ExportComponentsTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.darkplaces = self.root / "engine/darkplaces"
        self.darkplaces.mkdir(parents=True)
        (self.darkplaces / "old.c").write_text("old\n")
        config = {
            "format": 1,
            "components": {
                "darkplaces": {
                    "repository": "dixonSolutions/Godnotic-darkplaces",
                    "path": "engine/darkplaces",
                    "revision": OLD,
                },
                "data": {
                    "repository": "dixonSolutions/Godnotic-data",
                    "path": "engine/data/xonotic-data.pk3dir",
                    "revision": OLD,
                },
            },
        }
        (self.root / "components.json").write_text(json.dumps(config) + "\n")
        subprocess.run(["git", "init", "-q", str(self.root)], check=True)
        subprocess.run(["git", "-C", str(self.root), "config", "user.name", "Component Test"], check=True)
        subprocess.run(["git", "-C", str(self.root), "config", "user.email", "test@example.invalid"], check=True)
        subprocess.run(["git", "-C", str(self.root), "add", "components.json", "engine/darkplaces/old.c"], check=True)
        subprocess.run(["git", "-C", str(self.root), "commit", "-qm", "baseline"], check=True)

    def tearDown(self):
        self.temp.cleanup()

    def import_fixture(self, files, revision=NEW):
        exporter.run(self.root, "import", "darkplaces", revision, archive(files))

    def test_import_removes_only_removed_tracked_files_and_preserves_untracked_outputs(self):
        (self.darkplaces / "build-obj").mkdir()
        output = self.darkplaces / "build-obj/renderer.o"
        output.write_bytes(b"local build output")
        self.import_fixture({"new.c": b"new source\n", "tools/run.sh": b"#!/bin/sh\nexit 0\n"})
        self.assertFalse((self.darkplaces / "old.c").exists())
        self.assertEqual((self.darkplaces / "new.c").read_bytes(), b"new source\n")
        self.assertTrue(os.access(self.darkplaces / "tools/run.sh", os.X_OK))
        self.assertEqual(output.read_bytes(), b"local build output")
        pin = json.loads((self.root / "components.json").read_text())
        self.assertEqual(pin["components"]["darkplaces"]["revision"], NEW)

    def test_import_refuses_dirty_tracked_files_without_touching_them(self):
        (self.darkplaces / "old.c").write_text("local edit\n")
        with self.assertRaisesRegex(exporter.ComponentError, "dirty tracked files"):
            self.import_fixture({"new.c": b"new\n"})
        self.assertEqual((self.darkplaces / "old.c").read_text(), "local edit\n")
        pin = json.loads((self.root / "components.json").read_text())
        self.assertEqual(pin["components"]["darkplaces"]["revision"], OLD)

    def test_import_refuses_to_overwrite_an_untracked_collision(self):
        output = self.darkplaces / "new.c"
        output.write_bytes(b"keep local file")
        with self.assertRaisesRegex(exporter.ComponentError, "untracked file"):
            self.import_fixture({"new.c": b"source file"})
        self.assertEqual(output.read_bytes(), b"keep local file")

    def test_import_preserves_untracked_file_that_does_not_collide(self):
        output = self.darkplaces / "generated.dat"
        output.write_bytes(b"build output")
        self.import_fixture({"new.c": b"new\n"})
        self.assertEqual(output.read_bytes(), b"build output")

    def test_check_compares_pin_and_imported_tree_to_source_archive(self):
        snapshot = {"new.c": b"new source\n"}
        self.import_fixture(snapshot)
        subprocess.run(["git", "-C", str(self.root), "add", "components.json", "engine/darkplaces"], check=True)
        exporter.run(self.root, "check", "darkplaces", NEW, archive(snapshot))
        (self.darkplaces / "new.c").write_text("changed\n")
        with self.assertRaisesRegex(exporter.ComponentError, "differs from"):
            exporter.run(self.root, "check", "darkplaces", NEW, archive(snapshot))

    def test_archive_excludes_repo_metadata_and_rejects_traversal(self):
        snapshot = exporter.read_snapshot(archive({
            ".github/workflows/build.yml": b"not imported",
            ".godnotic/config.json": b"not imported",
            "GODNOTIC.md": b"not imported",
            "src/file.c": b"source",
        }))
        self.assertEqual(set(snapshot), {"src/file.c"})

        escaped = tarfile.TarInfo("../../outside.c")
        escaped.size = 1
        with self.assertRaisesRegex(exporter.ComponentError, "unsafe archive path"):
            exporter.read_snapshot(archive({}, [escaped]))

    def test_archive_rejects_symlinks_even_under_excluded_roots(self):
        link = tarfile.TarInfo(".github/escape")
        link.type = tarfile.SYMTYPE
        link.linkname = "../../outside"
        with self.assertRaisesRegex(exporter.ComponentError, "symlink or hardlink"):
            exporter.read_snapshot(archive({}, [link]))

    def test_only_full_sha_and_allowlisted_components_are_accepted(self):
        with self.assertRaisesRegex(exporter.ComponentError, "40-hex"):
            exporter.run(self.root, "import", "darkplaces", "abc", archive({"x.c": b"x"}))
        with self.assertRaisesRegex(exporter.ComponentError, "unknown component"):
            exporter.run(self.root, "import", "other", NEW, archive({"x.c": b"x"}))


if __name__ == "__main__":
    unittest.main()
