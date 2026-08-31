#!/usr/bin/env python3
"""Align this platform with the newest BtbN FFmpeg build.

1. downloads/refreshes the prebuilt package declared in UPSTREAM.toml
   ([windows] or [linux] asset, depending on the running platform),
2. adopts its source commit into [upstream] commit,
3. syncs the vendored fftools snapshot to that commit and regenerates shim/.

The prebuilt tree is the authority for the commit: when its commit differs
from the manifest the rolling asset is re-fetched and the manifest moves to
it — run this script on the other platform right after to keep the pair
aligned. The vendored fftools/*.c|h files must stay byte-identical to
upstream; every local adaptation lives in shim/ (regenerated here), so a
sync is a plain overwrite with no merging.

Requires Python 3.12+. Standard library only.
"""
import argparse
import json
import os
import re
import shutil
import sys
import tarfile
import urllib.request
import urllib.error
import zipfile
from datetime import date
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MANIFEST = ROOT / "UPSTREAM.toml"
PLATFORM = "windows" if sys.platform == "win32" else "linux"
UA = {"User-Agent": "ffplay-standalone"}
FFTOOLS = ["ffplay.c", "ffplay_renderer.c", "ffplay_renderer.h", "cmdutils.c",
           "cmdutils.h", "opt_common.c", "opt_common.h", "fopen_utf8.h"]
INCLUDE_RE = re.compile(r'#include\s+"((?:compat|libavutil)/[^"]+)"')
RELINC_RE = re.compile(r'#include\s+"([a-z0-9_]+\.h)"')


def get_proxy_handler(proxy_url):
    """Create proxy handler from given proxy URL."""
    if proxy_url:
        return urllib.request.ProxyHandler({"http": proxy_url, "https": proxy_url})
    return None


STUBS = {  # headers replaced with hand-written equivalents, not upstream copies
    "libavutil/libm.h": """\
/**
 * The prebuilt dev packages omit libavutil/libm.h. Upstream's version keys
 * off configure-time HAVE_* math macros that a synthesized config.h cannot
 * reproduce and collides with UCRT/glibc declarations, so this stub just
 * exposes the standard C math API, which both platforms provide.
 */

#ifndef AVUTIL_LIBM_H
#define AVUTIL_LIBM_H

#include <math.h>

#endif /* AVUTIL_LIBM_H */
""",
}


def toml_get(section, key):
    sec = None
    for line in MANIFEST.read_text().splitlines():
        if m := re.match(r"\s*\[([^\]]+)\]", line):
            sec = m.group(1)
        elif sec == section and (m := re.match(rf'\s*{key}\s*=\s*"([^"]*)"', line)):
            return m.group(1)
    return ""


def toml_set(section, key, value):
    pat = re.compile(rf"(\[{re.escape(section)}\](?:(?!\n\[).)*?\n\s*{key}\s*=\s*)\"[^\"]*\"", re.S)
    # newline parameter not available in older Python versions
    if sys.version_info >= (3, 10):
        MANIFEST.write_text(pat.sub(rf'\1"{value}"', MANIFEST.read_text(), count=1), newline="\n")
    else:
        MANIFEST.write_text(pat.sub(rf'\1"{value}"', MANIFEST.read_text(), count=1))


def download(url, dest):
    print("Downloading", url)
    with urllib.request.urlopen(urllib.request.Request(url, headers=UA)) as r, \
         open(dest, "wb") as f:
        expected = r.headers.get("Content-Length")
        shutil.copyfileobj(r, f)
    if expected and Path(dest).stat().st_size != int(expected):
        Path(dest).unlink()
        sys.exit(f"error: download truncated (expected {expected} bytes): {url}")


def extract(archive, dest):
    if archive.suffix == ".zip":
        with zipfile.ZipFile(archive) as z:
            z.extractall(dest)
    else:
        with tarfile.open(archive) as t:
            # filter parameter is only available in Python 3.12+
            if sys.version_info >= (3, 12):
                t.extractall(dest, filter="data")
            else:
                t.extractall(dest)


def tree_commit_of(include):
    fv = include / "libavutil" / "ffversion.h"
    if not fv.is_file():
        return ""
    version = re.search(r'FFMPEG_VERSION "([^"]*)"', fv.read_text())[1]
    m = re.search(r"g([0-9a-f]{7,40})", version)  # git-describe: n9.0.1-11-ge47273f4d9
    return m.group(1) if m else ""


def same_commit(tree, manifest):
    # the tree only carries the abbreviated describe hash; compare by prefix
    return bool(tree) and bool(manifest) and manifest.startswith(tree)


def expand_sha(repository, sha):
    if re.fullmatch("[0-9a-f]{40}", sha or ""):
        return sha
    api = repository.replace("https://github.com", "https://api.github.com/repos")
    try:
        with urllib.request.urlopen(urllib.request.Request(f"{api}/commits/{sha}", headers=UA)) as r:
            return json.load(r)["sha"]
    except OSError:
        print(f"WARNING: could not expand commit '{sha}' via GitHub API")
        return ""


def internal_includes(paths):
    found = set()
    for p in paths:
        if p.is_file():
            found |= set(INCLUDE_RE.findall(p.read_text(errors="replace")))
    return sorted(found)


def regenerate_shim(include, tree_root):
    """Rebuild shim/ as the closure of internal headers the snapshot includes.

    Only headers the prebuilt package does not install are vendored; upstream
    copies with in-tree relative includes rewritten to their public
    libavutil/ location. libm.h is the exception: upstream's version keys off
    configure-time HAVE_* math macros a synthesized config.h cannot reproduce
    and collides with UCRT/glibc declarations, so a stub exposing <math.h> is
    used instead (both platforms provide the full C math API).
    """
    shutil.rmtree(ROOT / "shim", ignore_errors=True)
    (ROOT / "shim" / "compat").mkdir(parents=True)
    (ROOT / "shim" / "libavutil").mkdir()

    for _ in range(4):  # iterate to a fixpoint: shim headers reference more shim headers
        paths = [*(ROOT / "fftools").glob("*.[ch]"), *(ROOT / "shim").rglob("*.h")]
        added = 0
        for h in internal_includes(paths):
            dst = ROOT / "shim" / h
            if dst.exists() or (include / h).exists():  # prebuilt provides it
                continue
            if h in STUBS:
                dst.parent.mkdir(parents=True, exist_ok=True)
                # newline parameter not available in older Python versions
                if sys.version_info >= (3, 10):
                    dst.write_text(STUBS[h], newline="\n")
                else:
                    dst.write_text(STUBS[h])
            elif (tree_root / h).is_file():
                dst.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(tree_root / h, dst)
            else:
                sys.exit(f"error: referenced internal header not found upstream: {h}")
            print(f"  shim/{h}")
            added += 1
        if not added:
            break

    for f in (ROOT / "shim" / "libavutil").rglob("*.h"):
        text = f.read_text()
        fixed = RELINC_RE.sub(
            lambda m: m.group(0) if m.group(1) in ("config.h", "config_components.h")
            else f'#include "libavutil/{m.group(1)}"', text)
        if fixed != text:
            # newline parameter not available in older Python versions
            if sys.version_info >= (3, 10):
                f.write_text(fixed, newline="\n")
            else:
                f.write_text(fixed)
            print(f"  rewrote relative includes in shim/libavutil/{f.name}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("-r", "--fftools-ref",
                    help="sync fftools from this ref instead of the manifest commit (set a reason in UPSTREAM.toml)")
    ap.add_argument("-s", "--source-dir",
                    help="sync fftools from a local FFmpeg checkout instead of the network")
    ap.add_argument("-p", "--proxy",
                    help="use proxy for network requests (e.g., http://127.0.0.1:10808)")
    args = ap.parse_args()

    # Set up proxy if provided
    if args.proxy:
        proxy_handler = get_proxy_handler(args.proxy)
        if proxy_handler:
            opener = urllib.request.build_opener(proxy_handler)
            urllib.request.install_opener(opener)
            print(f"Using proxy: {args.proxy}")

    asset = toml_get(PLATFORM, "asset")
    repository = toml_get("upstream", "repository")
    if not asset or not repository:
        sys.exit(f"error: UPSTREAM.toml is missing [{PLATFORM}] asset or [upstream] repository")
    tree = ROOT / "third_party" / re.sub(r"\.(zip|tar\.xz|tar\.gz)$", "", asset)
    include = tree / "include"
    cache = ROOT / "third_party" / ".cache"
    cache.mkdir(parents=True, exist_ok=True)

    # 1. prebuilt package: the tree is the authority for the commit
    manifest_commit = toml_get("upstream", "commit")
    manifest_set = manifest_commit not in ("", "TBD")
    commit = tree_commit_of(include)
    if commit and manifest_set and not same_commit(commit, manifest_commit):
        print(f"Prebuilt tree at {commit} differs from manifest ({manifest_commit}); refreshing...")
        shutil.rmtree(tree, ignore_errors=True)
        (cache / asset).unlink(missing_ok=True)
        commit = ""
    if not (include / "libavutil" / "avutil.h").is_file():
        download(f"https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/{asset}", cache / asset)
        extract(cache / asset, tree.parent)
        commit = tree_commit_of(include)
    if not commit:
        sys.exit(f"error: could not read the source commit from {include / 'libavutil' / 'ffversion.h'}")
    if not same_commit(commit, manifest_commit):
        if manifest_set:
            print(f"WARNING: prebuilt commit changed ({manifest_commit} -> {commit}); "
                  "re-run this script on the other platform to keep the pair aligned.")
        manifest_commit = expand_sha(repository, commit) or commit
        toml_set("upstream", "commit", manifest_commit)
    print(f"Prebuilt {asset} at commit {manifest_commit}")

    # 2. resolve the fftools ref to sync from
    ref = args.fftools_ref or toml_get("sync", "fftools_ref")
    if ref and not args.fftools_ref and not toml_get("sync", "reason"):
        print("WARNING: UPSTREAM.toml sets fftools_ref without a reason; the alignment policy expects one.")
    ref = expand_sha(repository, ref or manifest_commit)

    # 3. stage the source snapshot (extracted once per commit into the cache)
    if args.source_dir:
        tree_root = Path(args.source_dir)
        if not (tree_root / "fftools" / "ffplay.c").is_file():
            sys.exit(f"error: fftools/ffplay.c not found under --source-dir '{args.source_dir}'")
        print(f"Using local source tree: {tree_root} (expected to be at {ref})")
    else:
        # github archive tarballs unpack to <repo-name>-<ref>/
        src_tree = cache / f"{repository.rsplit('/', 1)[-1]}-{ref}"
        if not (src_tree / "fftools" / "ffplay.c").is_file():
            src = cache / f"ffmpeg-src-{ref[:12]}.tar.gz"
            if not src.is_file():
                download(f"{repository}/archive/{ref}.tar.gz", src)
            extract(src, cache)
        if not (src_tree / "fftools" / "ffplay.c").is_file():
            sys.exit(f"error: unexpected archive layout for {ref}")
        tree_root = src_tree

    # 4. copy the fftools whitelist + regenerate shim/
    for f in FFTOOLS:
        if (tree_root / "fftools" / f).is_file():
            shutil.copyfile(tree_root / "fftools" / f, ROOT / "fftools" / f)
        else:
            print(f"  skip fftools/{f} (not present at {ref})")
    regenerate_shim(include, tree_root)

    toml_set("sync", "last_synced", date.today().isoformat())
    print(f"\nSynced fftools from {ref}")


if __name__ == "__main__":
    main()
