#!/usr/bin/env python3
"""Keep engine/ in step with upstream Xonotic without losing Touch changes.

The pristine upstream files live on the upstream/vendor branch. upstream.lock
records which upstream commits that branch holds, and which vendor commit main
was last merged from. A sync is a three-way merge with an explicit base:

    base   = vendor commit named in upstream.lock on main   (old upstream)
    ours   = main                                           (upstream + Touch)
    theirs = new vendor commit                              (new upstream)

Nothing here touches the working tree or the index except `resolve`, so the
check, vendor, merge and hooks commands are safe in a checkout other agents
are editing.

Commands:
  check    compare upstream.lock with the upstream branch heads
  vendor   build a vendor commit from upstream (or from the lock's commits)
  merge    merge a vendor commit into a revision, write the result to a branch
  resolve  start the same merge in a clean checkout, conflicts staged
  hooks    verify every token Touch added to a shared file survived a merge
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LOCK_NAME = "upstream.lock"
VENDOR_REF = "refs/heads/upstream/vendor"
ZERO = "0" * 40
TRAILER = "Upstream-{}"


# --------------------------------------------------------------------------
# git helpers


def git(*args, cwd=ROOT, env=None, input=None, check=True, binary=False):
    full_env = dict(os.environ)
    if env:
        full_env.update(env)
    res = subprocess.run(
        ["git", *args],
        cwd=cwd,
        env=full_env,
        input=input,
        capture_output=True,
        text=not binary,
    )
    if check and res.returncode != 0:
        err = res.stderr if not binary else res.stderr.decode(errors="replace")
        sys.exit(f"git {' '.join(args[:3])} failed ({res.returncode}): {err.strip()}")
    return res


def out(*args, **kw):
    return git(*args, **kw).stdout.strip()


def rev_exists(rev):
    return git("rev-parse", "--verify", "-q", rev + "^{commit}", check=False).returncode == 0


def show(rev, path):
    res = git("show", f"{rev}:{path}", check=False)
    return res.stdout if res.returncode == 0 else None


def ls_tree(rev, cwd=ROOT):
    """Blob entries of rev as {path: (mode, sha)}."""
    res = git("ls-tree", "-r", "-z", "--full-tree", rev, cwd=cwd)
    entries = {}
    for item in res.stdout.split("\0"):
        if not item:
            continue
        meta, path = item.split("\t", 1)
        mode, typ, sha = meta.split()
        if typ == "blob":
            entries[path] = (mode, sha)
    return entries


# --------------------------------------------------------------------------
# upstream.lock


class Component:
    def __init__(self, name, path, branch, commit, url):
        self.name, self.path, self.branch, self.commit, self.url = name, path, branch, commit, url

    def owns(self, path, components):
        """True when path belongs to this component and not to a nested one."""
        if not under(path, self.path):
            return False
        for other in components:
            if other is not self and len(other.path) > len(self.path) and under(path, other.path):
                return False
        return True


def under(path, prefix):
    return prefix == "" or path == prefix or path.startswith(prefix + "/")


class Lock:
    def __init__(self, text):
        self.lines = text.splitlines()
        self.components = []
        self.vendor = ZERO
        for line in self.lines:
            fields = line.split("#", 1)[0].split()
            if not fields:
                continue
            if fields[0] == "vendor" and len(fields) == 2:
                self.vendor = fields[1]
            elif len(fields) == 5:
                self.components.append(Component(*fields))
            else:
                sys.exit(f"{LOCK_NAME}: cannot parse line: {line}")
        if not self.components:
            sys.exit(f"{LOCK_NAME}: no components")

    def get(self, name):
        for c in self.components:
            if c.name == name:
                return c
        sys.exit(f"{LOCK_NAME}: no component {name}")

    def render(self, commits=None, vendor=None):
        """The lock text with new commits and vendor line; layout kept."""
        commits = commits or {}
        result = []
        for line in self.lines:
            fields = line.split("#", 1)[0].split()
            if fields and fields[0] == "vendor" and len(fields) == 2 and vendor:
                line = line.replace(fields[1], vendor)
            elif len(fields) == 5 and fields[0] in commits:
                line = line.replace(fields[3], commits[fields[0]])
            result.append(line)
        return "\n".join(result) + "\n"


def read_lock(rev=None):
    if rev is None:
        path = ROOT / LOCK_NAME
        if not path.exists():
            sys.exit(f"missing {path}")
        return Lock(path.read_text())
    text = show(rev, LOCK_NAME)
    if text is None:
        sys.exit(f"{rev} has no {LOCK_NAME}")
    return Lock(text)


def vendor_commits(rev):
    """Component commits recorded in a vendor commit's message."""
    msg = out("log", "-1", "--format=%B", rev)
    return dict(re.findall(r"^Upstream-([\w.-]+): ([0-9a-f]{40})$", msg, re.M))


# --------------------------------------------------------------------------
# check


def remote_head(c):
    res = git("ls-remote", c.url, f"refs/heads/{c.branch}", check=False)
    if res.returncode != 0 or not res.stdout.strip():
        sys.exit(f"cannot read {c.branch} of {c.url}: {res.stderr.strip()}")
    return res.stdout.split()[0]


def cmd_check(args):
    lock = read_lock(args.rev)
    updates = {}
    for c in lock.components:
        head = remote_head(c)
        state = "up to date" if head == c.commit else "UPDATE"
        print(f"{c.name:12} {c.commit[:12]} -> {head[:12]}  {state}")
        if head != c.commit:
            updates[c.name] = head
    gh = os.environ.get("GITHUB_OUTPUT")
    if gh:
        with open(gh, "a") as fh:
            fh.write(f"updates={'true' if updates else 'false'}\n")
            fh.write(f"components={' '.join(sorted(updates))}\n")
    return 0


# --------------------------------------------------------------------------
# vendor


def escape_sparse(path):
    path = re.sub(r"([\\*?\[\]])", r"\\\1", path)
    return "/" + path


def fetch_component(c, commit, components, workdir, keep=None):
    """Import the vendored files of one upstream commit; returns {path: (mode, sha)}.

    keep, when given, filters paths relative to the component root.
    """
    repo = workdir / f"{c.name}-{commit[:12]}"
    git("init", "-q", str(repo), cwd=workdir)
    git("remote", "add", "origin", c.url, cwd=repo)
    print(f"  fetching {c.name} {commit[:12]}", flush=True)
    git("fetch", "-q", "--depth", "1", "--filter=blob:none", "origin", commit, cwd=repo)

    entries = {}
    for path, (mode, sha) in ls_tree(commit, cwd=repo).items():
        if mode == "160000" or (keep and not keep(path)):
            continue
        full = f"{c.path}/{path}" if c.path else path
        if c.owns(full, components):
            entries[full] = (mode, sha)

    # Never vendor what this repo ignores (textures, models, sounds, ...).
    paths = sorted(entries)
    res = git("check-ignore", "--no-index", "-z", "--stdin", input="\0".join(paths) + "\0", check=False)
    ignored = set(p for p in res.stdout.split("\0") if p)
    for p in ignored:
        del entries[p]
    print(f"  {c.name}: {len(entries)} files vendored, {len(ignored)} ignored", flush=True)

    # A sparse checkout of just those files pulls their blobs in one batch.
    strip = len(c.path) + 1 if c.path else 0
    patterns = "\n".join(escape_sparse(p[strip:]) for p in sorted(entries)) + "\n"
    git("sparse-checkout", "set", "--no-cone", "--stdin", cwd=repo, input=patterns)
    git("-c", "advice.detachedHead=false", "checkout", "-q", commit, cwd=repo)

    shas = "\n".join(sorted(set(sha for _, sha in entries.values()))) + "\n"
    pack = git("pack-objects", "-q", "--stdout", cwd=repo, input=shas.encode(), binary=True).stdout
    git("index-pack", "--stdin", input=pack, binary=True)
    return entries


def build_vendor(lock, wanted, parent, outside=None):
    """Commit a vendor tree holding the wanted upstream commit of each component.

    outside maps a component name to (subdir, sha): files of that component
    outside subdir come from sha instead. Only the first vendor commit needs
    this, to describe a tree that was synced one directory at a time.
    """
    outside = outside or {}
    parent_commits = vendor_commits(parent) if parent else {}
    stale = [c for c in lock.components if parent_commits.get(c.name) != wanted[c.name]]
    if parent and not stale:
        print("vendor branch already holds these upstream commits")
        return parent

    with tempfile.TemporaryDirectory(prefix="upstream-vendor-") as tmp:
        tmp = Path(tmp)
        env = {"GIT_INDEX_FILE": str(tmp / "index")}
        if parent:
            git("read-tree", parent, env=env)
        else:
            git("read-tree", "--empty", env=env)
        for c in stale:
            if c.name in outside:
                sub, other = outside[c.name]
                inside = lambda rel, sub=sub: under(rel, sub)
                entries = fetch_component(c, wanted[c.name], lock.components, tmp, keep=inside)
                entries.update(fetch_component(c, other, lock.components, tmp,
                                               keep=lambda rel, f=inside: not f(rel)))
            else:
                entries = fetch_component(c, wanted[c.name], lock.components, tmp)
            old = [p for p in out("ls-files", "-z", env=env).split("\0")
                   if p and c.owns(p, lock.components)]
            if old:
                git("update-index", "-z", "--force-remove", "--stdin", env=env,
                    input="\0".join(old) + "\0")
            info = "".join(f"{mode} {sha}\t{path}\0" for path, (mode, sha) in sorted(entries.items()))
            git("update-index", "-z", "--index-info", env=env, input=info)
        tree = out("write-tree", env=env)

    if parent and out("rev-parse", parent + "^{tree}") == tree:
        print("upstream changes touch no vendored file; vendor tree unchanged")
    lines = ["Vendor upstream Xonotic sources [skip ci]", ""]
    changed = [c.name for c in stale]
    lines.append(f"Updated: {', '.join(changed)}." if parent else "Initial vendor tree.")
    for name, (sub, other) in sorted(outside.items()):
        lines += ["", f"{name}: only {sub}/ is at the commit below; every other file of",
                  f"{name} is from {other}."]
    lines.append("")
    lines += [f"{TRAILER.format(c.name)}: {wanted[c.name]}" for c in lock.components]
    parents = ["-p", parent] if parent else []
    return out("commit-tree", tree, *parents, input="\n".join(lines) + "\n")


def cmd_vendor(args):
    lock = read_lock(args.lock_rev)
    if args.at == "lock":
        wanted = {c.name: c.commit for c in lock.components}
    else:
        wanted = {c.name: remote_head(c) for c in lock.components}
    for item in args.set or []:
        name, _, sha = item.partition("=")
        lock.get(name)
        wanted[name] = sha

    old = out("rev-parse", "--verify", "-q", args.ref, check=False) if args.ref else ""
    parent = None if args.root else (old or None)
    outside = {}
    for item in args.outside or []:
        spec, _, sha = item.partition("=")
        name, _, sub = spec.partition(":")
        lock.get(name)
        if not sub or not re.fullmatch(r"[0-9a-f]{40}", sha):
            sys.exit(f"--outside wants NAME:SUBDIR=SHA40, got {item}")
        outside[name] = (sub.strip("/"), sha)
    commit = build_vendor(lock, wanted, parent, outside)
    if args.ref:
        git("update-ref", "-m", "upstream-sync vendor", args.ref, commit, old or ZERO)
        print(f"{args.ref} -> {commit}")
    print(commit)
    return 0


# --------------------------------------------------------------------------
# merge


def merge_tree(base, ours, theirs):
    """Returns (tree, conflict_entries, messages, clean)."""
    res = git("merge-tree", "--write-tree", "-z", f"--merge-base={base}", ours, theirs, check=False)
    if res.returncode not in (0, 1):
        sys.exit(f"git merge-tree failed: {res.stderr.strip()}")
    parts = res.stdout.split("\0")
    tree = parts[0]
    entries = []
    for item in parts[1:]:
        if not item:
            break
        meta, path = item.split("\t", 1)
        mode, sha, stage = meta.split()
        entries.append((mode, sha, int(stage), path))
    messages = ""
    if res.returncode == 1:
        plain = git("merge-tree", "--write-tree", f"--merge-base={base}", ours, theirs, check=False).stdout
        messages = plain.split("\n\n", 1)[1].strip() if "\n\n" in plain else ""
    return tree, entries, messages, res.returncode == 0


def compare_url(c, old, new):
    base = c.url[:-4] if c.url.endswith(".git") else c.url
    return f"{base}/-/compare/{old}...{new}"


def report(lock, new_commits, conflicts, messages, path):
    lines = ["| Component | From | To | Changes |", "|---|---|---|---|"]
    for c in lock.components:
        new = new_commits.get(c.name, c.commit)
        if new == c.commit:
            lines.append(f"| {c.name} | `{c.commit[:12]}` | unchanged | |")
        else:
            lines.append(f"| {c.name} | `{c.commit[:12]}` | `{new[:12]}` | [compare]({compare_url(c, c.commit, new)}) |")
    if conflicts:
        lines += ["", f"### {len(conflicts)} conflicted files", ""]
        lines += [f"- `{p}`" for p in conflicts]
        if messages:
            lines += ["", "<details><summary>git messages</summary>", "", "```", messages, "```", "</details>"]
    text = "\n".join(lines) + "\n"
    if path:
        Path(path).write_text(text)
    return text


def prepare_merge(args):
    ours = out("rev-parse", "--verify", args.onto + "^{commit}")
    lock = read_lock(ours) if show(ours, LOCK_NAME) is not None else read_lock()
    base = args.base or lock.vendor
    if base == ZERO or not rev_exists(base):
        sys.exit(f"merge base {base} is not available; fetch {VENDOR_REF} first")
    theirs = out("rev-parse", "--verify", args.vendor + "^{commit}")
    new_commits = vendor_commits(theirs)
    new_lock = lock.render(new_commits, vendor=theirs)
    return ours, lock, base, theirs, new_commits, new_lock


def cmd_merge(args):
    ours, lock, base, theirs, new_commits, new_lock = prepare_merge(args)
    if base == theirs:
        print("already merged: upstream.lock names this vendor commit")
        return 0
    tree, entries, messages, clean = merge_tree(base, ours, theirs)
    conflicts = sorted(set(p for _, _, _, p in entries))
    print(report(lock, new_commits, conflicts, messages, args.report))
    if not clean:
        print(f"{len(conflicts)} conflicted files; run `{Path(__file__).name} resolve` in a clean clone")
        return 1

    with tempfile.TemporaryDirectory(prefix="upstream-merge-") as tmp:
        env = {"GIT_INDEX_FILE": str(Path(tmp) / "index")}
        git("read-tree", tree, env=env)
        blob = out("hash-object", "-w", "--stdin", input=new_lock)
        git("update-index", "--add", "--cacheinfo", f"100644,{blob},{LOCK_NAME}", env=env)
        tree = out("write-tree", env=env)
    msg = merge_message(lock, new_commits)
    commit = out("commit-tree", tree, "-p", ours, "-p", theirs, input=msg)
    if args.branch:
        ref = args.branch if args.branch.startswith("refs/") else f"refs/heads/{args.branch}"
        git("update-ref", "-m", "upstream-sync merge", ref, commit)
        print(f"{ref} -> {commit}")
    print(commit)
    return 0


def merge_message(lock, new_commits):
    changed = [c for c in lock.components if new_commits.get(c.name, c.commit) != c.commit]
    names = ", ".join(c.name for c in changed) or "nothing"
    lines = [f"Merge upstream Xonotic ({names})", ""]
    for c in changed:
        lines.append(f"{c.name}: {c.commit[:12]} -> {new_commits[c.name][:12]}")
    return "\n".join(lines) + "\n"


def cmd_resolve(args):
    if git("status", "--porcelain", "--untracked-files=no").stdout.strip():
        sys.exit("working tree or index has changes; resolve needs a clean checkout "
                 "(use a separate clone, not a git worktree, in this repo)")
    args.onto = "HEAD"
    ours, lock, base, theirs, new_commits, new_lock = prepare_merge(args)
    if base == theirs:
        print("already merged")
        return 0
    tree, entries, messages, clean = merge_tree(base, ours, theirs)
    git("read-tree", "-u", "-m", "HEAD", tree)
    conflicts = sorted(set(p for _, _, _, p in entries))
    if conflicts:
        git("update-index", "-z", "--force-remove", "--stdin", input="\0".join(conflicts) + "\0")
        info = "".join(f"{mode} {sha} {stage}\t{path}\0" for mode, sha, stage, path in entries)
        git("update-index", "-z", "--index-info", input=info)
    (ROOT / LOCK_NAME).write_text(new_lock)
    git("add", LOCK_NAME)
    gitdir = Path(out("rev-parse", "--absolute-git-dir"))
    (gitdir / "MERGE_HEAD").write_text(theirs + "\n")
    (gitdir / "MERGE_MSG").write_text(merge_message(lock, new_commits))
    print(report(lock, new_commits, conflicts, messages, None))
    if conflicts:
        print("Resolve the files above (keep every Touch hook), `git add` them, then `git commit`.")
    else:
        print("Merged cleanly; review and `git commit`.")
    print(f"Afterwards: python3 {Path(__file__).relative_to(ROOT)} hooks")
    return 0


# --------------------------------------------------------------------------
# hooks

TOKEN = re.compile(r"[A-Za-z_][A-Za-z0-9_]{3,}")
TEXT_EXT = {".c", ".h", ".qc", ".qh", ".inc", ".cfg", ".txt", ".sh", ".cmake"}
TEXT_NAMES = {"makefile", "Makefile", "BSDmakefile"}


def blob_text(sha):
    if sha is None:
        return ""
    return git("cat-file", "blob", sha).stdout


def cmd_hooks(args):
    merged = out("rev-parse", "--verify", args.merged + "^{commit}")
    ours = out("rev-parse", "--verify", (args.ours or merged + "^1") + "^{commit}")
    old_base = args.old_base or read_lock(ours).vendor
    new_base = args.new_base or read_lock(merged).vendor
    for name, rev in (("old base", old_base), ("new base", new_base)):
        if not rev_exists(rev):
            sys.exit(f"{name} {rev} not available; fetch {VENDOR_REF}")
    lock = read_lock(merged)

    trees = {k: ls_tree(r) for k, r in
             (("old", old_base), ("ours", ours), ("new", new_base), ("merged", merged))}
    problems = []
    checked = 0
    for path, (_, sha) in sorted(trees["ours"].items()):
        if not any(under(path, c.path) for c in lock.components):
            continue
        old = trees["old"].get(path)
        if old and old[1] == sha:
            continue
        if old is None:
            # Touch-only file: it must still be there.
            if path not in trees["merged"]:
                problems.append(f"{path}: Touch-only file is missing after the merge")
            continue
        if Path(path).suffix not in TEXT_EXT and Path(path).name not in TEXT_NAMES:
            continue
        merged_entry = trees["merged"].get(path)
        if merged_entry is None:
            problems.append(f"{path}: Touch-patched file was deleted (upstream removed or renamed it)")
            continue
        added = Counter(TOKEN.findall(blob_text(sha))) - Counter(TOKEN.findall(blob_text(old[1])))
        if not added:
            continue
        checked += 1
        new = trees["new"].get(path)
        kept = (Counter(TOKEN.findall(blob_text(merged_entry[1])))
                - Counter(TOKEN.findall(blob_text(new[1] if new else None))))
        lost = {t: n - kept[t] for t, n in added.items() if kept[t] < n}
        if lost:
            shown = ", ".join(f"{t}" + (f" x{n}" if n > 1 else "") for t, n in sorted(lost.items())[:12])
            more = f" (+{len(lost) - 12} more)" if len(lost) > 12 else ""
            problems.append(f"{path}: Touch additions missing: {shown}{more}")

    print(f"checked {checked} Touch-patched upstream files between {ours[:12]} and {merged[:12]}")
    if problems:
        print("\n".join(problems))
        print(f"\n{len(problems)} problems: a Touch change did not survive the merge")
        return 1
    print("every Touch addition survived")
    return 0


# --------------------------------------------------------------------------


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("check", help="compare upstream.lock with upstream branch heads")
    p.add_argument("--rev", help="read upstream.lock from this revision (default: working tree)")
    p.set_defaults(func=cmd_check)

    p = sub.add_parser("vendor", help="build a vendor commit")
    p.add_argument("--at", choices=["upstream", "lock"], default="upstream",
                   help="take upstream branch heads (default) or the commits in upstream.lock")
    p.add_argument("--set", action="append", metavar="NAME=SHA", help="pin one component")
    p.add_argument("--lock-rev", help="read upstream.lock from this revision")
    p.add_argument("--ref", default=VENDOR_REF, help=f"ref to advance (default {VENDOR_REF}; '' for none)")
    p.add_argument("--root", action="store_true", help="start a new history instead of building on --ref")
    p.add_argument("--outside", action="append", metavar="NAME:SUBDIR=SHA",
                   help="take files of NAME outside SUBDIR from SHA (first vendor commit only)")
    p.set_defaults(func=cmd_vendor)

    for name, func in (("merge", cmd_merge), ("resolve", cmd_resolve)):
        p = sub.add_parser(name, help="merge a vendor commit" if name == "merge"
                           else "start the merge in this (clean) checkout, conflicts staged")
        if name == "merge":
            p.add_argument("--onto", default="HEAD", help="revision to merge into (default HEAD)")
            p.add_argument("--branch", default="upstream-sync", help="branch to write ('' for none)")
            p.add_argument("--report", help="write a markdown summary here")
        p.add_argument("--vendor", default=VENDOR_REF, help="vendor commit to merge")
        p.add_argument("--base", help="override the merge base (default: vendor line of upstream.lock)")
        p.set_defaults(func=func)

    p = sub.add_parser("hooks", help="check that Touch additions survived a merge")
    p.add_argument("--merged", default="HEAD", help="the merge result (default HEAD)")
    p.add_argument("--ours", help="Touch side before the merge (default MERGED^1)")
    p.add_argument("--old-base", help="vendor commit ours was based on (default: its upstream.lock)")
    p.add_argument("--new-base", help="vendor commit merged in (default: MERGED's upstream.lock)")
    p.set_defaults(func=cmd_hooks)

    args = ap.parse_args()
    sys.exit(args.func(args))


if __name__ == "__main__":
    main()
