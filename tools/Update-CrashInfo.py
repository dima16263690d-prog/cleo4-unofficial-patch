#!/usr/bin/env python3
"""Update the bundled CrashInfo databases from JuniorDjjr/CrashInfo.

Upstream keeps the canonical lists per game and language under Lists/:

    Lists/GTA-SA-10US/EN-CrashList.txt
    Lists/GTA-SA-10US/ES-CrashList.txt
    Lists/GTA-SA-10US/PT-CrashList.txt
    Lists/GTA-VC-10/EN-CrashList.txt
    ...

DebugUtils bundles the GTA SA 1.0 US English list. That list is the only one the
parser reads as VERIFIED, but the other languages/versions are fetched too so a
human can diff them or build a localized overlay from real upstream text instead
of retyping it.

The bundled file is not a raw copy: it carries a DebugUtils header (version
marker, source, update date) and a local-notes footer that must survive updates.
This script regenerates the header, swaps the upstream body in, and reappends the
existing footer verbatim.

Run from the repository root (no third-party dependencies, uses urllib):

    python tools/Update-CrashInfo.py
    python tools/Update-CrashInfo.py --dry-run
    python tools/Update-CrashInfo.py --source <path-to-EN-CrashList.txt>
"""

import argparse
import datetime
import os
import re
import sys
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CRASHINFO_DIR = os.path.join(ROOT, "Plugins", "DebugUtils", "CrashInfo")

RAW_BASE = "https://raw.githubusercontent.com/JuniorDjjr/CrashInfo/main/Lists"
TARGET_VERSION = "GTA-SA-10US"
TARGET_LANG = "EN"

TARGET_FILE = os.path.join(CRASHINFO_DIR, "CLEO-CrashList.txt")

# Mirror of the upstream tree we care about. The SA 1.0 US EN list is what ships
# inside DebugUtils.cleo; the rest are kept alongside for localization/diffing.
SATELLITES = [
    ("GTA-SA-10US", "ES", "GTA-SA-10US-ES-CrashList.txt"),
    ("GTA-SA-10US", "PT", "GTA-SA-10US-PT-CrashList.txt"),
    ("GTA-VC-10", "EN", "GTA-VC-10-EN-CrashList.txt"),
    ("GTA-3-10", "EN", "GTA-3-10-EN-CrashList.txt"),
]

HEADER_MARKER = "# DebugUtils-Database-Version:"
FOOTER_START = "# CLEO DebugUtils LOCAL NOTES"
HEADER_END = "# " + "=" * 76


def fetch(url):
    request = urllib.request.Request(url, headers={"User-Agent": "DebugUtils-updater"})
    with urllib.request.urlopen(request, timeout=30) as response:
        return response.read().decode("utf-8", errors="replace")


def split_existing(text):
    """Return (footer, error_count) from the current bundled file."""
    lines = text.splitlines()

    footer = ""
    for index, line in enumerate(lines):
        if line.strip() == FOOTER_START:
            # Include the separator line that precedes the footer, if present.
            start = index
            if start > 0 and lines[start - 1].strip().startswith("# ==="):
                start -= 1
            footer = "\n".join(lines[start:])
            break

    error_count = sum(1 for line in lines if line.startswith("Error: "))
    return footer, error_count


def build_header(version, lang, source_url, date_text):
    return "\n".join(
        [
            "# CLEO DebugUtils Crash Database",
            f"{HEADER_MARKER} 2",
            f"# Baseline: {version} CrashInfo list (local copy)",
            f"# Source: JuniorDjjr/CrashInfo — {lang}-CrashList.txt",
            f"# Upstream: {source_url}",
            f"# Updated: {date_text}",
            "# Runtime AUTO-DISCOVERED observations are written by DebugUtils to the installed",
            "# local database and are not part of this repository source until verified.",
            HEADER_END,
        ]
    )


def normalize_newlines(text):
    return text.replace("\r\n", "\n").replace("\r", "\n").strip("\n")


def main():
    parser = argparse.ArgumentParser(description="Update bundled CrashInfo databases.")
    parser.add_argument("--dry-run", action="store_true", help="report without writing")
    parser.add_argument(
        "--source",
        help="use a local EN-CrashList.txt instead of downloading (offline refresh)",
    )
    parser.add_argument(
        "--with-satellites",
        action="store_true",
        help="also fetch the other languages/versions into CrashInfo/upstream/",
    )
    args = parser.parse_args()

    if not os.path.isdir(CRASHINFO_DIR):
        print(f"error: {CRASHINFO_DIR} not found; run from the repository", file=sys.stderr)
        return 1

    source_url = f"{RAW_BASE}/{TARGET_VERSION}/{TARGET_LANG}-CrashList.txt"

    try:
        if args.source:
            with open(args.source, encoding="utf-8", errors="replace") as handle:
                body = handle.read()
        else:
            body = fetch(source_url)
    except Exception as exc:  # noqa: BLE001 - report any download/read failure plainly
        print(f"error: could not read the upstream list: {exc}", file=sys.stderr)
        return 1

    body = normalize_newlines(body)
    if "Error: " not in body:
        print("error: upstream list does not look like a CrashInfo list", file=sys.stderr)
        return 1

    with open(TARGET_FILE, encoding="utf-8", errors="replace") as handle:
        existing = handle.read()

    footer, _ = split_existing(existing)
    if not footer:
        print("warning: local-notes footer not found; writing header + upstream only")

    today = datetime.date.today().isoformat()
    header = build_header(TARGET_VERSION, TARGET_LANG, source_url, today)

    parts = [header, "", body]
    if footer:
        parts += ["", footer]
    output = "\n".join(parts).rstrip("\n") + "\n"

    error_count = body.count("\nError: ") + (1 if body.startswith("Error: ") else 0)

    if args.dry_run:
        print(f"[dry-run] {TARGET_FILE}: {error_count} Error entries, {len(output)} bytes")
    else:
        with open(TARGET_FILE, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(output)
        print(f"updated {os.path.relpath(TARGET_FILE, ROOT)}: {error_count} Error entries")

    # Fetch the satellites for localization/diffing only when asked. These are
    # best effort: a network hiccup must not fail the primary update.
    if args.with_satellites:
        satellite_dir = os.path.join(CRASHINFO_DIR, "upstream")
        os.makedirs(satellite_dir, exist_ok=True)
        for version, lang, filename in SATELLITES:
            url = f"{RAW_BASE}/{version}/{lang}-CrashList.txt"
            target = os.path.join(satellite_dir, filename)
            try:
                data = normalize_newlines(fetch(url))
            except Exception as exc:  # noqa: BLE001
                print(f"  skip {filename}: {exc}")
                continue

            if args.dry_run:
                print(f"  [dry-run] upstream/{filename}: {len(data)} bytes")
            else:
                with open(target, "w", encoding="utf-8", newline="\n") as handle:
                    handle.write(data + "\n")
                print(f"  updated upstream/{filename}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
