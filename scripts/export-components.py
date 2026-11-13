#!/usr/bin/env python3
"""Import, verify and export the Godnotic engine/data components.

The engine (engine/darkplaces) and the game code (engine/data/xonotic-data.pk3dir)
live in their own repositories; this checkout carries a copy of each at the commit
pinned in components.json. --import and --check bring a commit in and compare with
it; --export writes this checkout's copy into a clone of the component repository
(scripts/component-push.sh commits, pushes and pins it).
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
from typing import BinaryIO
from urllib.error import URLError
from urllib.request import Request, urlopen


COMPONENTS_FILE = "components.json"
ALLOWED = {
    "darkplaces": ("dixonSolutions/Godnotic-darkplaces", "engine/darkplaces"),
    "data": ("dixonSolutions/Godnotic-data", "engine/data/xonotic-data.pk3dir"),
}
REVISION_RE = re.compile(r"^[0-9a-fA-F]{40}$")
PLACEHOLDER = "0" * 40
EXCLUDED_ROOTS = {".github", ".godnotic"}
MAX_ARCHIVE_BYTES = 256 * 1024 * 1024
MAX_SNAPSHOT_BYTES = 512 * 1024 * 1024


class ComponentError(Exception):
    """An invalid pin, archive, or local checkout state."""


def git(root: Path, *args: str) -> bytes:
    try:
        return subprocess.run(
            ["git", "-C", str(root), *args], check=True,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        ).stdout
    except subprocess.CalledProcessError as exc:
        detail = exc.stderr.decode("utf-8", "replace").strip()
        raise ComponentError(f"git {' '.join(args)} failed: {detail}") from exc


def load_config(root: Path) -> dict:
    path = root / COMPONENTS_FILE
    try:
        config = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ComponentError(f"cannot read {path}: {exc}") from exc
    if config.get("format") != 1 or not isinstance(config.get("components"), dict):
        raise ComponentError(f"{COMPONENTS_FILE} has an unsupported format")
    for name, (repository, target) in ALLOWED.items():
        item = config["components"].get(name)
        if not isinstance(item, dict):
            raise ComponentError(f"{COMPONENTS_FILE} is missing component {name}")
        if item.get("repository") != repository or item.get("path") != target:
            raise ComponentError(f"{name} must use the trusted source {repository} at {target}")
        revision = item.get("revision")
        if not isinstance(revision, str) or (revision != PLACEHOLDER and not REVISION_RE.fullmatch(revision)):
            raise ComponentError(f"{name} has an invalid pinned revision")
    if set(config["components"]) != set(ALLOWED):
        raise ComponentError("components.json has unknown component entries")
    return config


def validate_revision(revision: str) -> str:
    if not REVISION_RE.fullmatch(revision):
        raise ComponentError("revision must be a full 40-hex commit SHA")
    return revision.lower()


def tracked_files(root: Path, target: str) -> set[str]:
    output = git(root, "ls-files", "-z", "--", target)
    prefix = target.rstrip("/") + "/"
    # .github, .godnotic and GODNOTIC.md belong to each repository on its own:
    # an import never brings them in, so it must not compare or remove them.
    return {
        path[len(prefix):]
        for path in output.decode("utf-8", "surrogateescape").split("\0")
        if path.startswith(prefix) and not excluded(PurePosixPath(path[len(prefix):]))
    }


def refuse_dirty_tracked(root: Path, target: str) -> None:
    dirty = git(root, "status", "--porcelain=v1", "--untracked-files=no", "--", target)
    if dirty:
        listing = dirty.decode("utf-8", "replace").strip().replace("\n", ", ")
        raise ComponentError(f"refusing to replace dirty tracked files under {target}: {listing}")


def excluded(relative: PurePosixPath) -> bool:
    if not relative.parts:
        return True
    return (
        relative.parts[0] in EXCLUDED_ROOTS
        or relative.parts[0] == ".git"
        or relative.as_posix() == "GODNOTIC.md"
    )


def read_snapshot(archive: BinaryIO) -> dict[str, tuple[bytes, int]]:
    """Read a GitHub tarball without extractall; reject unsafe entries."""
    files: dict[str, tuple[bytes, int]] = {}
    root_prefix: str | None = None
    seen: set[str] = set()
    total_size = 0
    try:
        tar = tarfile.open(fileobj=archive, mode="r:gz")
    except (tarfile.TarError, OSError) as exc:
        raise ComponentError(f"invalid source archive: {exc}") from exc
    with tar:
        for member in tar:
            name = member.name
            if "\\" in name or "\x00" in name:
                raise ComponentError(f"unsafe archive path {name!r}")
            raw = PurePosixPath(name)
            if not raw.parts or raw.is_absolute() or any(p in {"", ".", ".."} for p in raw.parts):
                raise ComponentError(f"unsafe archive path {name!r}")
            if root_prefix is None:
                root_prefix = raw.parts[0]
            if raw.parts[0] != root_prefix:
                raise ComponentError("archive contains more than one top-level directory")
            if len(raw.parts) == 1:
                if member.isdir():
                    continue
                raise ComponentError(f"archive has no component path: {name!r}")
            relative = PurePosixPath(*raw.parts[1:])
            if member.issym() or member.islnk():
                raise ComponentError(f"source archive contains a symlink or hardlink: {name}")
            if not (member.isfile() or member.isdir()):
                raise ComponentError(f"source archive contains a special file: {name}")
            if member.isfile():
                total_size += member.size
                if total_size > MAX_SNAPSHOT_BYTES:
                    raise ComponentError(f"source archive expands beyond {MAX_SNAPSHOT_BYTES} bytes")
            normalized = relative.as_posix()
            if normalized in seen:
                raise ComponentError(f"duplicate archive path {normalized}")
            seen.add(normalized)
            if excluded(relative) or member.isdir():
                continue
            source = tar.extractfile(member)
            if source is None:
                raise ComponentError(f"cannot read archive file {name}")
            files[normalized] = (source.read(), stat.S_IMODE(member.mode) & 0o777)
    if not files:
        raise ComponentError("source archive contains no importable files")
    return files


def fetch_snapshot(repository: str, revision: str) -> dict[str, tuple[bytes, int]]:
    request = Request(
        f"https://api.github.com/repos/{repository}/tarball/{revision}",
        headers={"User-Agent": "Godnotic-component-importer", "Accept": "application/vnd.github+json"},
    )
    try:
        with urlopen(request, timeout=120) as response:
            final_url = response.geturl()
            if not (final_url.startswith("https://codeload.github.com/") or final_url.startswith("https://api.github.com/")):
                raise ComponentError(f"unexpected archive redirect host: {final_url}")
            with tempfile.TemporaryDirectory(prefix="godnotic-component-") as temporary:
                archive_path = Path(temporary) / "source.tar.gz"
                length = response.headers.get("Content-Length")
                if length and int(length) > MAX_ARCHIVE_BYTES:
                    raise ComponentError(f"source archive exceeds {MAX_ARCHIVE_BYTES} bytes")
                with archive_path.open("wb") as archive:
                    received = 0
                    while True:
                        chunk = response.read(1024 * 1024)
                        if not chunk:
                            break
                        received += len(chunk)
                        if received > MAX_ARCHIVE_BYTES:
                            raise ComponentError(f"source archive exceeds {MAX_ARCHIVE_BYTES} bytes")
                        archive.write(chunk)
                with archive_path.open("rb") as archive:
                    return read_snapshot(archive)
    except (URLError, TimeoutError, OSError) as exc:
        raise ComponentError(f"could not fetch {repository} at {revision}: {exc}") from exc


def destination(root: Path, target: str, relative: str) -> Path:
    base = root / target
    if base.is_symlink():
        raise ComponentError(f"refusing to write through symlink {base}")
    path = base.joinpath(*PurePosixPath(relative).parts)
    # Reject symlinked parents in the existing checkout: an untracked path must
    # never redirect an import outside its component directory.
    parent = base
    for part in PurePosixPath(relative).parts[:-1]:
        parent = parent / part
        if parent.is_symlink():
            raise ComponentError(f"refusing to write through symlink {parent}")
    if not path.resolve(strict=False).is_relative_to(base.resolve(strict=False)):
        raise ComponentError(f"import path escapes component target: {relative}")
    return path


def apply_snapshot(root: Path, name: str, revision: str, files: dict[str, tuple[bytes, int]]) -> None:
    repository, target = ALLOWED[name]
    config = load_config(root)
    target_dir = root / target
    refuse_dirty_tracked(root, target)
    tracked = tracked_files(root, target)
    for relative in files:
        path = destination(root, target, relative)
        if path.exists() and relative not in tracked:
            raise ComponentError(f"refusing to overwrite untracked file {target}/{relative}")

    # Install new content first, so a write failure cannot remove old files.
    for relative, (content, mode) in sorted(files.items()):
        path = destination(root, target, relative)
        path.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.NamedTemporaryFile(dir=path.parent, prefix=".godnotic-import-", delete=False) as tmp:
            temporary = Path(tmp.name)
            tmp.write(content)
        os.chmod(temporary, mode)
        os.replace(temporary, path)

    # Only remove paths that Git already tracks inside this component. Any
    # ignored or untracked build output remains in place.
    for relative in sorted(tracked - files.keys(), reverse=True):
        path = destination(root, target, relative)
        if path.is_file() or path.is_symlink():
            path.unlink()
        parent = path.parent
        while parent != target_dir and parent.is_relative_to(target_dir):
            try:
                parent.rmdir()
            except OSError:
                break
            parent = parent.parent

    config["components"][name]["revision"] = revision
    config_path = root / COMPONENTS_FILE
    temporary_config = config_path.with_suffix(".json.tmp")
    temporary_config.write_text(json.dumps(config, indent=2) + "\n", encoding="utf-8")
    os.replace(temporary_config, config_path)
    print(f"Imported {name} {revision} from {repository} ({len(files)} files)")


def check_snapshot(root: Path, name: str, revision: str, files: dict[str, tuple[bytes, int]]) -> None:
    _, target = ALLOWED[name]
    config = load_config(root)
    pinned = config["components"][name]["revision"]
    if pinned != revision:
        raise ComponentError(f"{name} pin is {pinned}, requested {revision}")
    indexed = tracked_files(root, target)
    expected = set(files)
    expected_indexed = set()
    for relative in expected:
        path = f"{target}/{relative}"
        ignored = subprocess.run(
            ["git", "-C", str(root), "check-ignore", "--no-index", "-q", "--", path],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        ).returncode == 0
        if relative in indexed or not ignored:
            expected_indexed.add(relative)
    if indexed != expected_indexed:
        missing = sorted(expected_indexed - indexed)[:5]
        extra = sorted(indexed - expected_indexed)[:5]
        raise ComponentError(f"{name} tracked file list differs from {revision}; missing={missing}, extra={extra}")
    for relative, (content, mode) in files.items():
        path = destination(root, target, relative)
        try:
            actual = path.read_bytes()
        except OSError as exc:
            raise ComponentError(f"{name} is missing {relative}: {exc}") from exc
        if actual != content:
            raise ComponentError(f"{name} differs from {revision}: {relative}")
        executable = bool(mode & 0o111)
        actual_executable = bool(stat.S_IMODE(path.stat().st_mode) & 0o111)
        if actual_executable != executable:
            raise ComponentError(f"{name} has the wrong executable bit: {relative}")
    print(f"Checked {name} {revision} ({len(files)} files)")


def export_tree(root: Path, name: str, clone: Path) -> None:
    """Copy this checkout's tracked component files into a clone of its repository.

    The clone ends up with exactly the tracked files of the component directory
    (working tree content), plus whatever it keeps for itself under .github,
    .godnotic and GODNOTIC.md. Nothing is committed or pushed here.
    """
    _, target = ALLOWED[name]
    if not (clone / ".git").exists():
        raise ComponentError(f"{clone} is not a git checkout")
    source = tracked_files(root, target)
    if not source:
        raise ComponentError(f"no tracked files under {target}")
    base = root / target
    for relative in sorted(source):
        src = base.joinpath(*PurePosixPath(relative).parts)
        if src.is_symlink():
            raise ComponentError(f"refusing to export symlink {target}/{relative}")
        if not src.is_file():
            # Tracked but deleted in the working tree: the removal is exported too.
            source = source - {relative}
            continue
        dst = clone.joinpath(*PurePosixPath(relative).parts)
        dst.parent.mkdir(parents=True, exist_ok=True)
        if dst.is_symlink() or dst.is_dir():
            raise ComponentError(f"refusing to replace {dst}")
        shutil.copyfile(src, dst)
        os.chmod(dst, 0o755 if os.access(src, os.X_OK) else 0o644)
    present = git(clone, "ls-files", "-z").decode("utf-8", "surrogateescape").split("\0")
    removed = 0
    for relative in present:
        if not relative or excluded(PurePosixPath(relative)) or relative in source:
            continue
        path = clone.joinpath(*PurePosixPath(relative).parts)
        if path.is_file() or path.is_symlink():
            path.unlink()
            removed += 1
    print(f"Exported {name}: {len(source)} files into {clone} ({removed} removed)")


def set_pin(root: Path, name: str, revision: str) -> None:
    revision = validate_revision(revision)
    config = load_config(root)
    config["components"][name]["revision"] = revision
    config_path = root / COMPONENTS_FILE
    temporary_config = config_path.with_suffix(".json.tmp")
    temporary_config.write_text(json.dumps(config, indent=2) + "\n", encoding="utf-8")
    os.replace(temporary_config, config_path)
    print(f"Pinned {name} at {revision}")


def run(root: Path, action: str, name: str, revision: str, archive: BinaryIO | None = None) -> None:
    if name not in ALLOWED:
        raise ComponentError(f"unknown component {name!r}; choose one of: {', '.join(ALLOWED)}")
    if action == "export":
        export_tree(root, name, Path(revision).resolve())
        return
    if action == "pin":
        set_pin(root, name, revision)
        return
    if action == "show-pin":
        print(load_config(root)["components"][name]["revision"])
        return
    revision = validate_revision(revision)
    repository, _ = ALLOWED[name]
    config = load_config(root)
    if archive is None:
        files = fetch_snapshot(repository, revision)
    else:
        files = read_snapshot(archive)
    if action == "import":
        apply_snapshot(root, name, revision, files)
    else:
        check_snapshot(root, name, revision, files)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    actions = parser.add_mutually_exclusive_group(required=True)
    actions.add_argument("--import", dest="action", action="store_const", const="import", help="import a pinned component snapshot")
    actions.add_argument("--check", dest="action", action="store_const", const="check", help="verify tracked files match a pinned snapshot")
    actions.add_argument("--export", dest="action", action="store_const", const="export", help="copy the tracked component files into a clone of its repository (second argument: the clone)")
    actions.add_argument("--pin", dest="action", action="store_const", const="pin", help="record a component commit in components.json without importing")
    actions.add_argument("--show-pin", dest="action", action="store_const", const="show-pin", help="print the pinned commit")
    parser.add_argument("component", help="darkplaces or data")
    parser.add_argument("revision", nargs="?", default="", help="full 40-hex commit SHA (for --export: path of the component clone)")
    args = parser.parse_args()
    try:
        run(Path(__file__).resolve().parent.parent, args.action, args.component, args.revision)
    except ComponentError as exc:
        print(f"export-components: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
