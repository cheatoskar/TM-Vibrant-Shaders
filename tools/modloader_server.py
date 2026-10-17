#!/usr/bin/env python3
"""ModLoader (TMLoader) update server for TM Vibrant Shaders.

A ModLoader server is a folder URL whose index.json lists products and their versions
(format of https://fullspeed.mooo.com/modloader/index.json, api 1). For every version the
ModLoader downloads `download_link` (a zip), checks its SHA-256 against `hash_sha256`, unpacks it
into database/TmForever/products/<product>/<version>/ and writes the version entry next to it
as description.yaml. So the zip holds only the version folder's files (here: the DLL), flat.

  zip       TMVibrantShaders.dll -> TMVS-modloader.zip (byte-identical for the same DLL)
  index     index.json (stable releases) and beta/index.json (stable + pre-releases) from the
            GitHub releases that carry TMVS-modloader.zip; changelogs from CHANGELOG.md
  missing   tags of releases without TMVS-modloader.zip but with TMVibrantShaders.dll (backfill)
  check     validate index.json files and zips against the format and TMLoader's own field list
  selftest  zip + index + check from a local DLL and fake releases (no network needed)

Standard library only (runs on the GitHub runners and with `python -I`).
"""
import argparse
import hashlib
import json
import os
import re
import sys
import tempfile
import urllib.request
import zipfile

REPO = "cheatoskar/TM-Vibrant-Shaders"
PRODUCT = "TM Vibrant Shaders"  # the product folder name; the setup installs into the same folder
ASSET = "TMVS-modloader.zip"
DLL = "TMVibrantShaders.dll"
GAME = "TmForever"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CHANGELOG_LIMIT = 6000  # characters per version; the ModLoader shows it as plain text
SEMVER = re.compile(r"^(\d+)\.(\d+)\.(\d+)(?:-([0-9A-Za-z.-]+))?(?:\+[0-9A-Za-z.-]+)?$")
SHA256 = re.compile(r"^[0-9a-f]{64}$")


# ---------------------------------------------------------------------------------------------
# packaging/*.yaml (flat files written by us; no YAML library on the runners' system Python)

def _yaml_scalar(text):
    text = text.strip()
    if len(text) >= 2 and text[0] == text[-1] == "'":
        return text[1:-1].replace("''", "'")
    if len(text) >= 2 and text[0] == text[-1] == '"':
        return json.loads(text)
    return text


def read_product_yaml(path):
    """packaging/description.yaml: name, author, type, homepage, description, icon_link."""
    meta = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            m = re.match(r"^([a-z_]+):\s*(.*)$", line.rstrip("\r\n"))
            if m and m.group(2):
                meta[m.group(1)] = _yaml_scalar(m.group(2))
    return meta


def read_version_yaml(path):
    """packaging/version.yaml: executable and dependencies (the changelog comes from CHANGELOG.md)."""
    text = open(path, encoding="utf-8").read()
    executable = re.search(r"^executable:\s*(.+)$", text, re.M)
    deps = []
    for m in re.finditer(r"^\s*-\s*id:\s*(.+)$((?:\n\s+version:\s*.+$)?)", text, re.M):
        dep = {"id": _yaml_scalar(m.group(1))}
        v = re.search(r"version:\s*(.+)$", m.group(2), re.M)
        if v:
            dep["version"] = _yaml_scalar(v.group(1))
        deps.append(dep)
    if not executable:
        raise SystemExit(f"{path}: no executable")
    return {"executable": _yaml_scalar(executable.group(1)), "dependencies": deps}


# ---------------------------------------------------------------------------------------------
# zip

def make_zip(dll, out):
    """The DLL alone at the zip's root, fixed timestamp: the same DLL gives the same bytes."""
    info = zipfile.ZipInfo(DLL, date_time=(2020, 1, 1, 0, 0, 0))
    info.compress_type = zipfile.ZIP_DEFLATED
    info.external_attr = 0o644 << 16
    info.create_system = 0
    with open(dll, "rb") as f:
        data = f.read()
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with zipfile.ZipFile(out, "w") as z:
        z.writestr(info, data, compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)
    return sha256_file(out)


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


# ---------------------------------------------------------------------------------------------
# changelog

def semver_key(version):
    m = SEMVER.match(version)
    if not m:
        return (0, 0, 0, 0, ())
    pre = m.group(4)
    ids = ()
    if pre:
        ids = tuple((0, int(p), "") if p.isdigit() else (1, 0, p) for p in pre.split("."))
    return (int(m.group(1)), int(m.group(2)), int(m.group(3)), 0 if pre else 1, ids)


def changelog_sections(markdown):
    """'## 1.4.0 (2026-10-18)' -> {'1.4.0': body}."""
    sections, current, lines = {}, None, []
    for line in markdown.splitlines():
        m = re.match(r"^##\s+v?(\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?)\b", line)
        if m or line.startswith("# "):
            if current:
                sections[current] = "\n".join(lines).strip()
            current, lines = (m.group(1) if m else None), []
        elif current:
            lines.append(line)
    if current:
        sections[current] = "\n".join(lines).strip()
    return sections


_TYPO = {"→": "->", "←": "<-", "–": "-", "—": "-", "‘": "'", "’": "'",
         "“": '"', "”": '"', "…": "...", " ": " ", "−": "-", "×": "x"}


def markdown_to_text(md, limit=CHANGELOG_LIMIT):
    """Markdown -> the plain text the ModLoader shows (white-space: pre-wrap, no markup)."""
    out = []
    for line in md.splitlines():
        s = line.rstrip()
        if re.match(r"^\s*!\[.*\]\(.*\)\s*$", s) or re.match(r"^\s*(---+|\*\*\*+)\s*$", s):
            continue
        if s.strip().lower().startswith(("<details", "</details", "<summary")):
            continue
        s = re.sub(r"!\[[^\]]*\]\([^)]*\)", "", s)
        s = re.sub(r"\[([^\]]+)\]\([^)]*\)", r"\1", s)
        heading = re.match(r"^#{1,6}\s+(.*)$", s)
        if heading:
            s = heading.group(1).rstrip(":") + ":"
        bold_line = re.match(r"^\*\*(.+?)\*\*:?$", s.strip())
        if bold_line:
            s = bold_line.group(1).rstrip(":") + ":"
        s = s.replace("**", "").replace("__", "").replace("`", "")
        s = re.sub(r"(?<![\w*])\*(?!\s)([^*\n]+?)(?<!\s)\*(?![\w*])", r"\1", s)
        s = re.sub(r"^(\s*)\* ", r"\1- ", s)
        for k, v in _TYPO.items():
            s = s.replace(k, v)
        out.append(s)
    text = re.sub(r"\n{3,}", "\n\n", "\n".join(out)).strip()
    if len(text) > limit:
        cut = text.rfind("\n", 0, limit)
        text = text[: cut if cut > 0 else limit].rstrip() + "\n..."
    return text


def changelog_for(version, sections, release):
    # Its own CHANGELOG.md section; for a pre-release without one its release notes (they
    # describe that build), else the section of the version it leads to.
    base = version.split("-")[0]
    notes = (release.get("body") or "").split("<details>")[0].strip()
    body = sections.get(version)
    if body is None and notes and base != version:
        body = notes
    if body is None:
        body = sections.get(base, notes)
    text = markdown_to_text(body)
    url = release.get("html_url") or f"https://github.com/{REPO}/releases/tag/v{version}"
    return (text + "\n\n" if text else "") + "Release notes: " + url


# ---------------------------------------------------------------------------------------------
# GitHub releases

def _get(url, token=None, accept="application/vnd.github+json"):
    req = urllib.request.Request(url, headers={"User-Agent": "tmvs-modloader-index", "Accept": accept})
    if token:
        req.add_header("Authorization", f"Bearer {token}")
    return urllib.request.urlopen(req, timeout=120)


def fetch_releases(repo, token=None):
    releases, url = [], f"https://api.github.com/repos/{repo}/releases?per_page=100"
    while url:
        with _get(url, token) as r:
            releases += json.load(r)
            m = re.search(r'<([^>]+)>;\s*rel="next"', r.headers.get("Link") or "")
            url = m.group(1) if m else None
    return releases


def load_releases(args):
    if args.releases:
        with open(args.releases, encoding="utf-8") as f:
            return json.load(f)
    return fetch_releases(args.repo, os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN"))


def asset_hash(asset, cache_dir, local):
    """SHA-256 of the asset as the ModLoader will download it (via the release redirect)."""
    name = asset["browser_download_url"]
    if name in local:
        return sha256_file(local[name])
    os.makedirs(cache_dir, exist_ok=True)
    path = os.path.join(cache_dir, hashlib.sha1(name.encode()).hexdigest() + ".zip")
    if not os.path.exists(path):
        with _get(name, accept="application/octet-stream") as r, open(path + ".part", "wb") as f:
            while True:
                chunk = r.read(1 << 20)
                if not chunk:
                    break
                f.write(chunk)
        os.replace(path + ".part", path)
    digest = sha256_file(path)
    # The API also lists a digest for newer assets: both must agree.
    listed = (asset.get("digest") or "").removeprefix("sha256:")
    if listed and listed != digest:
        raise SystemExit(f"{name}: downloaded sha256 {digest} != listed {listed}")
    return digest


def build_index(releases, product_meta, version_meta, sections, hash_of):
    """-> (stable index, beta index, skipped tags)."""
    entries, skipped = [], []
    for rel in releases:
        tag = rel.get("tag_name", "")
        if rel.get("draft") or not tag.startswith("v") or not SEMVER.match(tag[1:]):
            continue
        version = tag[1:]
        asset = next((a for a in rel.get("assets", []) if a.get("name") == ASSET), None)
        if not asset:
            skipped.append(tag)
            continue
        entry = {
            "executable": version_meta["executable"],
            "dependencies": version_meta["dependencies"],
            "download_link": asset["browser_download_url"],
            "hash_sha256": hash_of(asset),
            "changelog": changelog_for(version, sections, rel),
        }
        entries.append((version, bool(rel.get("prerelease")), entry))
    entries.sort(key=lambda e: semver_key(e[0]))

    def index(include_pre):
        product = {k: product_meta[k] for k in ("name", "author", "type", "description", "icon_link", "homepage")
                   if product_meta.get(k)}
        product["versions"] = {v: e for v, pre, e in entries if include_pre or not pre}
        return {"api": 1, "game": GAME, "products": {PRODUCT: product}}

    return index(False), index(True), skipped


def write_json(path, data):
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(data, f, indent=2, ensure_ascii=True)
        f.write("\n")


# ---------------------------------------------------------------------------------------------
# check

def tmloader_fields(loader_d_ts):
    """Field names of ProductDescription / ProductVersionDescription in TMLoader's ui/layouts/loader.d.ts."""
    text = open(loader_d_ts, encoding="utf-8").read()
    fields = {}
    for cls in ("ProductDescription", "ProductVersionDescription", "Selector"):
        m = re.search(r"declare class " + cls + r"\b[^{]*\{(.*?)\n\}", text, re.S)
        if m:
            fields[cls] = set(re.findall(r"^\s*(?:readonly\s+)?([a-z_0-9]+)\s*:", m.group(1), re.M)) - {
                "product", "productversion"}
    return fields


def check_index(path, reference=None, fields=None, zips=None, installed=None):
    errors, notes = [], []
    data = json.load(open(path, encoding="utf-8"))
    def err(msg):
        errors.append(f"{path}: {msg}")
    if set(data) != {"api", "game", "products"}:
        err(f"top-level keys {sorted(data)}")
    if data.get("api") != 1:
        err("api is not 1")
    if data.get("game") != GAME:
        err(f"game is not {GAME}")
    products = data.get("products") or {}
    if not isinstance(products, dict) or not products:
        err("no products")
    ref_product_keys = ref_version_keys = None
    if reference:
        rp = next(iter(reference["products"].values()))
        ref_product_keys = set(rp)
        ref_version_keys = set(next(iter(rp["versions"].values())))
    for pid, prod in products.items():
        keys = set(prod)
        for k in ("name", "author", "type", "versions"):
            if k not in prod:
                err(f"{pid}: missing {k}")
        if prod.get("type") not in ("modification", "program"):
            err(f"{pid}: type {prod.get('type')!r}")
        if fields and "ProductDescription" in fields and keys - {"versions"} - fields["ProductDescription"]:
            err(f"{pid}: fields unknown to TMLoader {sorted(keys - {'versions'} - fields['ProductDescription'])}")
        if ref_product_keys is not None and keys != ref_product_keys:
            notes.append(f"{pid}: product keys differ from reference: ours-ref {sorted(keys - ref_product_keys)}, "
                         f"ref-ours {sorted(ref_product_keys - keys)}")
        for link in ("icon_link", "homepage"):
            if link in prod and not str(prod[link]).startswith("https://"):
                err(f"{pid}: {link} is not https")
        versions = prod.get("versions") or {}
        if not versions:
            notes.append(f"{pid}: no versions")
        for ver, e in versions.items():
            where = f"{pid} {ver}"
            if not SEMVER.match(ver):
                err(f"{where}: version is not semver")
            vkeys = set(e)
            for k in ("executable", "dependencies", "download_link", "hash_sha256"):
                if k not in e:
                    err(f"{where}: missing {k}")
            if fields and "ProductVersionDescription" in fields and vkeys - fields["ProductVersionDescription"]:
                err(f"{where}: fields unknown to TMLoader {sorted(vkeys - fields['ProductVersionDescription'])}")
            if ref_version_keys is not None and vkeys != ref_version_keys:
                err(f"{where}: version keys {sorted(vkeys)} != reference {sorted(ref_version_keys)}")
            if not SHA256.match(str(e.get("hash_sha256", ""))):
                err(f"{where}: hash_sha256 is not 64 lowercase hex digits")
            if not str(e.get("download_link", "")).startswith("https://"):
                err(f"{where}: download_link is not https")
            deps = e.get("dependencies")
            if not isinstance(deps, list) or not all(isinstance(d, dict) and isinstance(d.get("id"), str) for d in deps):
                err(f"{where}: dependencies must be a list of {{id, version}}")
            else:
                for d in deps:
                    extra = set(d) - (fields.get("Selector") if fields and "Selector" in fields else {"id", "version"})
                    if extra:
                        err(f"{where}: dependency fields {sorted(extra)}")
            if not isinstance(e.get("changelog", ""), str):
                err(f"{where}: changelog is not a string")
            if zips and e.get("download_link") in zips:
                zp = zips[e["download_link"]]
                if sha256_file(zp) != e.get("hash_sha256"):
                    err(f"{where}: zip hash differs from hash_sha256")
                with zipfile.ZipFile(zp) as z:
                    names = z.namelist()
                    if z.testzip() is not None:
                        err(f"{where}: zip is corrupt")
                if e.get("executable") not in names:
                    err(f"{where}: {e.get('executable')} is not at the zip's root ({names})")
                if any("/" in n or "\\" in n for n in names):
                    err(f"{where}: the zip has folders; the ModLoader unpacks it into the version folder")
                if "description.yaml" in names:
                    err(f"{where}: description.yaml in the zip (the ModLoader writes it from the index)")
                if installed:
                    want = sorted(set(installed) - {"description.yaml"})
                    notes.append(f"{where}: zip {sorted(names)}; installed reference version folder {want}")
    return errors, notes


def installed_layout(product_dir):
    """Files of the newest version folder of an installed ModLoader product (read only)."""
    if not product_dir or not os.path.isdir(product_dir):
        return None
    versions = [d for d in os.listdir(product_dir) if os.path.isdir(os.path.join(product_dir, d)) and SEMVER.match(d)]
    if not versions:
        return None
    newest = max(versions, key=semver_key)
    return [f for f in os.listdir(os.path.join(product_dir, newest)) if "." in f and not re.search(r"\.dll\.", f)]


def default_loader_d_ts():
    base = os.environ.get("LOCALAPPDATA")
    path = os.path.join(base, "TMLoader", "ui", "layouts", "loader.d.ts") if base else ""
    return path if path and os.path.exists(path) else None


def load_reference(arg):
    if not arg:
        return None
    if re.match(r"^https?://", arg):
        with _get(arg, accept="application/json") as r:
            return json.load(r)
    return json.load(open(arg, encoding="utf-8"))


# ---------------------------------------------------------------------------------------------
# commands

def cmd_zip(args):
    digest = make_zip(args.dll, args.out)
    print(f"{digest}  {os.path.basename(args.out)}")


def cmd_index(args):
    releases = load_releases(args)
    product_meta = read_product_yaml(args.description)
    version_meta = read_version_yaml(args.version_yaml)
    sections = changelog_sections(open(args.changelog, encoding="utf-8").read())
    local = dict(kv.split("=", 1) for kv in (args.local or []))
    cache = args.cache or os.path.join(tempfile.gettempdir(), "tmvs-modloader-cache")
    stable, beta, skipped = build_index(releases, product_meta, version_meta, sections,
                                        lambda a: asset_hash(a, cache, local))
    write_json(os.path.join(args.out_dir, "index.json"), stable)
    write_json(os.path.join(args.out_dir, "beta", "index.json"), beta)
    for name, idx in (("index.json", stable), ("beta/index.json", beta)):
        print(f"{name}: {', '.join(idx['products'][PRODUCT]['versions']) or '(no versions)'}")
    if skipped:
        print(f"without {ASSET} (not listed): {', '.join(skipped)}")


def cmd_missing(args):
    for rel in load_releases(args):
        names = {a.get("name") for a in rel.get("assets", [])}
        if not rel.get("draft") and ASSET not in names and DLL in names:
            print(rel["tag_name"])


def cmd_check(args):
    fields = tmloader_fields(args.loader_d_ts) if args.loader_d_ts else None
    reference = load_reference(args.reference)
    zips = dict(kv.split("=", 1) for kv in (args.zip or []))
    installed = installed_layout(args.installed)
    all_errors = []
    for path in args.index:
        errors, notes = check_index(path, reference, fields, zips, installed)
        for n in notes:
            print("note:", n)
        all_errors += errors
    for e in all_errors:
        print("ERROR:", e)
    print("OK" if not all_errors else f"{len(all_errors)} error(s)")
    return 1 if all_errors else 0


def cmd_selftest(args):
    """A fake dist from a local DLL, fake releases (one stable, one pre-release, one without the
    ModLoader zip), the index from them, then every check."""
    work = args.work or tempfile.mkdtemp(prefix="tmvs-mlserver-")
    dist = os.path.join(work, "dist")
    os.makedirs(dist, exist_ok=True)
    dll = args.dll
    if not dll:
        for cand in ("build/Release/TMVibrantShaders.dll", "dist/TMVibrantShaders.dll"):
            if os.path.exists(os.path.join(ROOT, cand)):
                dll = os.path.join(ROOT, cand)
                break
    if not dll:
        dll = os.path.join(work, DLL)
        with open(dll, "wb") as f:
            f.write(b"MZ" + bytes(4094))
        print("no built DLL found: using a dummy file")
    zip_path = os.path.join(dist, ASSET)
    digest = make_zip(dll, zip_path)
    if make_zip(dll, zip_path + ".again") != digest:
        print("ERROR: the zip is not reproducible")
        return 1
    os.remove(zip_path + ".again")
    print(f"zip: {zip_path} ({os.path.getsize(zip_path)} bytes, sha256 {digest})")

    def release(tag, pre, with_zip):
        assets = [{"name": DLL, "browser_download_url": f"https://github.com/{REPO}/releases/download/{tag}/{DLL}"}]
        if with_zip:
            assets.append({"name": ASSET, "browser_download_url": f"https://github.com/{REPO}/releases/download/{tag}/{ASSET}"})
        return {"tag_name": tag, "prerelease": pre, "draft": False, "body": "Notes for " + tag + "\n<details>x</details>",
                "html_url": f"https://github.com/{REPO}/releases/tag/{tag}", "assets": assets}

    releases = [release("v1.4.1-beta.1", True, True), release("v1.4.0", False, True), release("v1.3.0", False, False),
                {"tag_name": "v1.5.0", "draft": True, "assets": []}]
    rel_path = os.path.join(work, "releases.json")
    write_json(rel_path, releases)
    local = [f"{a['browser_download_url']}={zip_path}" for r in releases for a in r["assets"] if a["name"] == ASSET]
    site = os.path.join(work, "site", "modloader")
    ns = argparse.Namespace(releases=rel_path, repo=REPO, description=os.path.join(ROOT, "packaging", "description.yaml"),
                            version_yaml=os.path.join(ROOT, "packaging", "version.yaml"),
                            changelog=os.path.join(ROOT, "CHANGELOG.md"), local=local, cache=None, out_dir=site)
    cmd_index(ns)
    stable = json.load(open(os.path.join(site, "index.json"), encoding="utf-8"))
    beta = json.load(open(os.path.join(site, "beta", "index.json"), encoding="utf-8"))
    problems = []
    if list(stable["products"][PRODUCT]["versions"]) != ["1.4.0"]:
        problems.append("stable index must list only 1.4.0")
    if list(beta["products"][PRODUCT]["versions"]) != ["1.4.0", "1.4.1-beta.1"]:
        problems.append("beta index must list 1.4.0 and 1.4.1-beta.1, ascending")
    cl = stable["products"][PRODUCT]["versions"]["1.4.0"]["changelog"]
    if "**" in cl or "`" in cl or "Release notes: https://github.com/" not in cl:
        problems.append("changelog is not plain text with the release link")
    for p in problems:
        print("ERROR:", p)
    zips = [f"{u.split('=', 1)[0]}={zip_path}" for u in local]
    loader = args.loader_d_ts or default_loader_d_ts()
    installed = args.installed
    if installed is None and os.environ.get("LOCALAPPDATA"):
        installed = os.path.join(os.environ["LOCALAPPDATA"], "TMLoader", "database", GAME, "products", "CenterSpeed")
    print(f"TMLoader field list: {loader or '(not found, skipped)'}; reference: {args.reference or '(none)'}")
    check = argparse.Namespace(index=[os.path.join(site, "index.json"), os.path.join(site, "beta", "index.json")],
                               reference=args.reference, loader_d_ts=loader, zip=zips, installed=installed)
    rc = cmd_check(check)
    print("\n--- index.json (changelog shortened) ---")
    shown = json.loads(json.dumps(stable))
    for e in shown["products"][PRODUCT]["versions"].values():
        e["changelog"] = e["changelog"][:160] + " [...]"
    print(json.dumps(shown, indent=2))
    print(f"\nwork folder: {work}")
    return 1 if (rc or problems) else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("zip")
    p.add_argument("--dll", required=True)
    p.add_argument("--out", required=True)

    def releases_args(p):
        p.add_argument("--releases", help="releases JSON (GitHub API format); default: fetch from the API")
        p.add_argument("--repo", default=os.environ.get("GITHUB_REPOSITORY") or REPO)

    p = sub.add_parser("index")
    releases_args(p)
    p.add_argument("--out-dir", required=True, help="writes <dir>/index.json and <dir>/beta/index.json")
    p.add_argument("--description", default=os.path.join(ROOT, "packaging", "description.yaml"))
    p.add_argument("--version-yaml", default=os.path.join(ROOT, "packaging", "version.yaml"))
    p.add_argument("--changelog", default=os.path.join(ROOT, "CHANGELOG.md"))
    p.add_argument("--local", action="append", help="<download_link>=<local zip>: hash this file instead of downloading")
    p.add_argument("--cache", help="download cache folder")

    p = sub.add_parser("missing")
    releases_args(p)

    p = sub.add_parser("check")
    p.add_argument("index", nargs="+")
    p.add_argument("--reference", help="a working server's index.json (file or URL) to compare the keys with")
    p.add_argument("--loader-d-ts", default=default_loader_d_ts(), help="TMLoader's ui/layouts/loader.d.ts")
    p.add_argument("--zip", action="append", help="<download_link>=<local zip> to check")
    p.add_argument("--installed", help="an installed ModLoader product folder to compare the zip layout with")

    p = sub.add_parser("selftest")
    p.add_argument("--dll", help="default: build/Release/TMVibrantShaders.dll, else a dummy file")
    p.add_argument("--work", help="work folder (default: a new temp folder)")
    p.add_argument("--reference", help="e.g. https://fullspeed.mooo.com/modloader/index.json")
    p.add_argument("--loader-d-ts")
    p.add_argument("--installed")

    args = ap.parse_args()
    rc = {"zip": cmd_zip, "index": cmd_index, "missing": cmd_missing, "check": cmd_check, "selftest": cmd_selftest}[args.cmd](args)
    sys.exit(rc or 0)


if __name__ == "__main__":
    main()
