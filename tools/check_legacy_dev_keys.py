"""Checks that the 2006-engine dev.ini key (LegacyCensus) can never act on another game.

Documents\\TrackMania\\TMVS\\dev.ini is shared by every TrackMania the mod runs in (Forever, United,
ESWC, Sunrise, Original). The key is read into g_legacyCensusWanted; only installLegacy, after it
recognised a 2006-engine game, copies it into the flag that acts (g_legacyCensus), and install()
clears that flag for every other game. (LegacyMp4 is gone: the MP4 export is a normal feature since
1.6.0.) This script fails when code anywhere under src/ uses the two names outside the functions
allowed below, or when
installLegacy copies them before the game is recognised.

Run:  python tools/check_legacy_dev_keys.py      (exit code 1 on a violation; runs with every build)
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# name -> {function: what it may do}; "decl" is the global definition (outside any function).
ALLOWED = {
    "g_legacyCensusWanted": {"legacyCensus": "write", "installLegacy": "read", "legacyDevKeysIgnored": "read"},
    # The acting flags: set only where the game is known (installLegacy) or cleared (legacyDevKeysIgnored),
    # read only in code that runs on a recognised 2006-engine game.
    "g_legacyCensus": {"installLegacy": "write", "legacyDevKeysIgnored": "clear", "installLegacyCensus": "read",
                       "legacyPoll": "read", "legacySetChallengeDetour": "read"},
}
# Uses that must exist, so a rename can't make the check pass by finding nothing.
REQUIRED = [("g_legacyCensus", "installLegacyCensus"), ("g_legacyCensus", "installLegacy"),
            ("g_legacyCensusWanted", "legacyCensus"), ("g_legacyCensus", "legacyDevKeysIgnored")]

NAME = re.compile(r"\b(g_legacyCensusWanted|g_legacyCensus)\b")
HEADER = re.compile(r"^[A-Za-z_][\w:<>,\s\*&]*?\b(\w+)\s*\(")


def strip_comment(line):
    i = line.find("//")
    return line if i < 0 else line[:i]


def uses(path):
    """(line number, function or None, name, kind) for every use in a source file."""
    out = []
    function = None
    with open(path, encoding="utf-8", errors="replace") as f:
        for n, raw in enumerate(f, 1):
            line = strip_comment(raw.rstrip("\r\n"))
            if line.startswith("}"):
                function = None
                continue
            if line and not line[0].isspace() and not line.startswith("#"):
                m = HEADER.match(line)
                if m and not line.rstrip().endswith(";"):
                    function = m.group(1)
            for m in NAME.finditer(line):
                name = m.group(1)
                after = line[m.end():].lstrip()
                if after.startswith("=") and not after.startswith("=="):
                    value = after[1:].strip().rstrip(";").strip()
                    kind = "clear" if value == "false" else "write"
                else:
                    kind = "read"
                if function is None:
                    kind = "decl"
                out.append((n, function, name, kind, line.strip()))
    return out


def main():
    errors = []
    seen = set()
    for folder, _, files in os.walk(os.path.join(ROOT, "src")):
        for file in files:
            if not file.endswith((".cpp", ".h")):
                continue
            path = os.path.join(folder, file)
            rel = os.path.relpath(path, ROOT)
            for n, function, name, kind, text in uses(path):
                where = "%s:%d" % (rel, n)
                if kind == "decl":
                    if rel != os.path.join("src", "engine.cpp") or not re.match(r"^bool %s = false;" % name, text):
                        errors.append("%s: %s outside a function (only its definition `bool %s = false;` in src/engine.cpp)" % (where, name, name))
                    continue
                allowed = ALLOWED[name].get(function)
                if not (allowed == kind or (allowed == "write" and kind == "clear")):
                    errors.append("%s: %s %s in %s() (allowed: %s)" % (where, kind, name, function,
                                  ", ".join("%s in %s()" % (k, f) for f, k in ALLOWED[name].items())))
                seen.add((name, function))
    for name, function in REQUIRED:
        if (name, function) not in seen:
            errors.append("expected a use of %s in %s(): not found (renamed? update this check)" % (name, function))

    # installLegacy copies the keys only after the game was recognised (`if (!found) return false;`).
    with open(os.path.join(ROOT, "src", "engine.cpp"), encoding="utf-8", errors="replace") as f:
        text = f.read()
    body = re.search(r"^bool installLegacy\(\) \{\n(.*?)^\}", text.replace("\r\n", "\n"), re.S | re.M)
    if not body:
        errors.append("src/engine.cpp: installLegacy() not found")
    else:
        b = body.group(1)
        recognised = b.find("if (!found) return false;")
        for name in ("g_legacyCensus = g_legacyCensusWanted;",):
            at = b.find(name)
            if recognised < 0 or at < 0 or at < recognised:
                errors.append("src/engine.cpp: installLegacy() must copy `%s` after `if (!found) return false;`" % name)
    # install() clears them for every game that is not a 2006-engine game, before anything else.
    body = re.search(r"^bool install\(const Callbacks& callbacks\) \{\n(.*?)^\}", text.replace("\r\n", "\n"), re.S | re.M)
    if not body or not re.search(r"if \(installLegacy\(\)\) \{.*?\n    \}\n    legacyDevKeysIgnored\(\);", body.group(1), re.S):
        errors.append("src/engine.cpp: install() must call legacyDevKeysIgnored() right after `if (installLegacy()) { ... }`")

    if errors:
        for e in errors:
            print("check_legacy_dev_keys: " + e)
        return 1
    print("check_legacy_dev_keys: OK (%d uses, all in 2006-engine-only code)" % len(seen))
    return 0


if __name__ == "__main__":
    sys.exit(main())
