"""Markdown from result.json files: one run, the results matrix, A/B comparison."""

import json
import os

RUN_COLUMNS = ["probe", "link", "target", "host", "firmware", "libxr", "core", "tools", "log"]


def run_row(data, out_dir):
    m = data["meta"]
    probe = m.get("probe") or {}
    build = m.get("build") or {}
    tools = ", ".join(f"{k} {v}" for k, v in (m.get("tools") or {}).items() if v)
    cells = [f"{probe.get('product')} {probe.get('serial', '')[-6:]}", m["link"], m["target"] or "-",
             m["host"]["os"].split("-")[0], build.get("fw") or probe.get("firmware") or "?",
             build.get("libxr", "?"), build.get("core", "?"), tools or "-", out_dir]
    return "| " + " | ".join(str(c) for c in cells) + " |"


def run_markdown(data, out_dir):
    lines = ["| " + " | ".join(RUN_COLUMNS) + " |", "|" + "---|" * len(RUN_COLUMNS),
             run_row(data, out_dir), "", "| case | status | result |", "|---|---|---|"]
    for c in data["cases"]:
        lines.append(f"| {c['id']} {c['title']} | {c['status']} | {c['summary']} |")
    return "\n".join(lines) + "\n"


def load(paths):
    runs = []
    for p in paths:
        path = os.path.join(p, "result.json") if os.path.isdir(p) else p
        with open(path, encoding="utf-8") as f:
            runs.append((os.path.dirname(path), json.load(f)))
    return runs


def matrix(paths):
    """Runs table plus one row per case and one column per run."""
    runs = load(paths)
    out = ["| run | " + " | ".join(RUN_COLUMNS) + " |", "|---|" + "---|" * len(RUN_COLUMNS)]
    for k, (d, data) in enumerate(runs, 1):
        out.append(f"| R{k} " + run_row(data, d))
    out += ["", "| case | " + " | ".join(f"R{k}" for k in range(1, len(runs) + 1)) + " |",
            "|---|" + "---|" * len(runs)]
    ids = []
    for _, data in runs:
        ids += [c["id"] for c in data["cases"] if c["id"] not in ids]
    for cid in ids:
        cells = []
        for _, data in runs:
            c = next((c for c in data["cases"] if c["id"] == cid), None)
            cells.append(f"{c['status']}: {c['summary']}" if c else "")
        out.append(f"| {cid} | " + " | ".join(cells) + " |")
    return "\n".join(out) + "\n"


def flatten(prefix, value, out):
    if isinstance(value, dict):
        for k, v in value.items():
            flatten(f"{prefix}.{k}" if prefix else str(k), v, out)
    elif isinstance(value, (int, float)) and not isinstance(value, bool):
        out[prefix] = value


def compare(a_paths, b_paths):
    """A/B table: mean of every numeric metric over the A runs and the B runs."""
    def means(paths):
        acc = {}
        for _, data in load(paths):
            for c in data["cases"]:
                flat = {}
                flatten("", c["metrics"], flat)
                for k, v in flat.items():
                    acc.setdefault((c["id"], k), []).append(v)
        return {k: sum(v) / len(v) for k, v in acc.items()}

    a, b = means(a_paths), means(b_paths)
    out = ["| case | metric | A | B | B/A |", "|---|---|---|---|---|"]
    for key in sorted(set(a) & set(b)):
        ratio = f"{b[key] / a[key]:.3f}" if a[key] else "-"
        out.append(f"| {key[0]} | {key[1]} | {a[key]:.4g} | {b[key]:.4g} | {ratio} |")
    return "\n".join(out) + "\n"
