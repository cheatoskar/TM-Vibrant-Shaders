"""Cross-checks docs/settings.md against the settings table (src/config.cpp TMVS_FIELD) and the
defaults in src/settings.h: every key has a row, the row's label is the menu's, its range is the
slider's and a default it states (in brackets at the end of the range) is the code's.

Run:  python tools/check_settings_doc.py      (exit code 1 on a mismatch)

The range column reads "a – b", "on / off", "colour" or the menu's choices ("Middle, Car"; a list
may also give its ini values, "0 – 2", with the names in the text); a
default goes last in brackets: "0 – 1 (0.7)", "on / off (on)", "Middle, Car (Middle)". A row may
hold several keys ("`GodRays`, `GodRayDecay`"): then the labels are joined by " / " and the
ranges by ", " in the same order. Machine settings (not part of a preset) must state a default.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def read(path):
    with open(os.path.join(ROOT, path), encoding="utf-8") as f:
        return f.read()


def fields():
    out = []
    pattern = re.compile(r'TMVS_FIELD\("(\w+)", "([^"]*)", "([^"]*)", (\w+), (\w+), ([-\d.]+)f, ([-\d.]+)f\)')
    for m in pattern.finditer(read("src/config.cpp")):
        key, label, cat, kind, member, mn, mx = m.groups()
        out.append(dict(key=key, label=label, cat=cat, kind=kind, member=member, min=float(mn), max=float(mx)))
    return out


def defaults():
    out = {}
    pattern = re.compile(r"^\s*(bool|int|float)\s+(\w+)(\[3\])?\s*=\s*([^;]+);", re.M)
    for m in pattern.finditer(read("src/settings.h")):
        kind, member, array, value = m.groups()
        value = value.strip()
        if array:
            out[member] = value
        elif kind == "bool":
            out[member] = value == "true"
        else:
            out[member] = float(value.rstrip("f"))
    return out


def machine_keys():
    body = re.search(r"bool isSystemField\(const Field& f\) \{(.*?)\n\}", read("src/config.cpp"), re.S).group(1)
    return set(re.findall(r'strcmp\(f\.key, "(\w+)"\)', body)), "Trail" if 'strncmp(f.key, "Trail", 5)' in body else None


def choices():
    """The names the menu shows for an Int setting drawn as a list (src/menu.cpp drawField)."""
    menu = read("src/menu.cpp")
    lists = {name: re.findall(r'"([^"]*)"', body) for name, body in re.findall(r"const char\* (k\w+)\[\] = \{([^}]*)\}", menu)}
    out = {}
    for key, body in re.findall(r'strcmp\(f\.key, "(\w+)"\)\) \{(.*?)\} else', menu, re.S):
        used = re.search(r"Combo\(label, (?:value|&choice), (k\w+)", body)
        if used and used.group(1) in lists:
            out[key] = lists[used.group(1)]
    return out


def doc_rows():
    text = read("docs/settings.md")
    text = text[text.index("## All settings"):]
    rows = {}
    for line in text.splitlines():
        if not line.startswith("| ") or line.startswith("| Setting") or line.startswith("|---"):
            continue
        cells = [c.strip() for c in line.strip().strip("|").split(" | ")]
        if len(cells) < 4:
            continue
        label, keys, rng = cells[0], re.findall(r"`(\w+)`", cells[1]), cells[2]
        if not keys or "maniaplanet.ini" in cells[1]:
            continue
        labels = label.split(" / ") if len(keys) > 1 else [label]
        ranges = rng.split(", ") if len(keys) > 1 and rng.count(", ") == len(keys) - 1 else [rng] * len(keys)
        for i, key in enumerate(keys):
            rows[key] = dict(label=labels[i] if i < len(labels) else None, range=ranges[i], line=line[:80])
    return rows


def number(text):
    m = re.search(r"-?\d+(?:\.\d+)?", text.replace("−", "-"))
    return float(m.group()) if m else None


def main():
    problems = []
    defs = defaults()
    machine, prefix = machine_keys()
    names = choices()
    rows = doc_rows()
    known = set()
    for f in fields():
        key = f["key"]
        known.add(key)
        row = rows.get(key)
        if not row:
            problems.append(f"{key}: no row in docs/settings.md")
            continue
        if row["label"] is not None and row["label"] != f["label"]:
            problems.append(f"{key}: label '{row['label']}', the menu says '{f['label']}'")
        rng = row["range"]
        bracket = re.search(r"\(([^()]*)\)\s*$", rng)
        body = rng[: bracket.start()].strip() if bracket else rng
        stated = bracket.group(1).strip() if bracket else None
        default = defs.get(f["member"])
        kind = f["kind"]
        if kind == "Bool":
            if body != "on / off":
                problems.append(f"{key}: range '{body}', expected 'on / off'")
            if stated is not None and stated not in ("on", "off"):
                problems.append(f"{key}: default '({stated})' is not on or off")
            elif stated is not None and (stated == "on") != default:
                problems.append(f"{key}: docs say {stated}, settings.h says {'on' if default else 'off'}")
        elif kind == "Color":
            if body != "colour":
                problems.append(f"{key}: range '{body}', expected 'colour'")
        elif kind == "Int" and key in names and number(body) is None:
            listed = [n.strip() for n in body.split(", ")]
            if listed != names[key]:
                problems.append(f"{key}: choices '{body}', the menu has '{', '.join(names[key])}'")
            if stated is not None:
                want = names[key][int(default)] if default is not None and 0 <= int(default) < len(names[key]) else None
                if stated != want:
                    problems.append(f"{key}: default '{stated}', settings.h says '{want}'")
        else:
            # A number range (an Int shown as a list may give its ini values, the names in the text).
            nums = [float(x) for x in re.findall(r"-?\d+(?:\.\d+)?", body.replace("−", "-").replace(" – ", " ").replace(" - ", " "))]
            if len(nums) < 2 or nums[0] != f["min"] or nums[1] != f["max"]:
                problems.append(f"{key}: range '{body}', the slider is {f['min']:g} – {f['max']:g}")
            if stated is not None:
                value = number(stated)
                if value is None or abs(value - default) > 1e-6:
                    problems.append(f"{key}: default '({stated})', settings.h says {default:g}")
        if stated is None and kind != "Color" and (key in machine or (prefix and key.startswith(prefix))):
            problems.append(f"{key}: machine setting without a default in docs/settings.md")
    for key in rows:
        if key not in known:
            problems.append(f"{key}: in docs/settings.md but not in the settings table")
    for p in problems:
        print(p)
    print(f"{len(known)} settings, {len(rows)} documented, {len(problems)} problem(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
