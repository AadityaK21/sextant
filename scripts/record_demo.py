#!/usr/bin/env python3
"""Record the CLI demo as an animated SVG.

WHY A GENERATED SVG RATHER THAN A SCREEN CAPTURE

A GIF recorded by hand is a screenshot of a moment. It drifts from the code the
day after it is made, and nothing detects that it has - the numbers in it stay
whatever they were on the afternoon someone hit record.

This runs the real commands against a real database and renders their real
output. Regenerating it is one command, so the asset in the README cannot
quietly stop being true. If the round trip breaks, the demo shows it breaking.

It also produces something a GIF cannot: text that can be selected, searched and
diffed. A reviewer reading the README on GitHub sees the actual numbers as
characters, not as pixels.

WHAT THIS DOES NOT COVER

The UI. A frame-by-frame reconstruction of a browser session would be a drawing
of the frontend rather than a recording of it, which is worse than not having
one. The README says plainly that the UI screenshots are captured by hand.

    python3 scripts/record_demo.py --db sextant-db --out docs/demo.svg
"""

import argparse
import html
import pathlib
import subprocess
import sys
import time

# Colours match web/src/index.css, so the recording and the UI look like one
# product rather than two.
BG = "#0b0f14"
FG = "#d7e0ea"
DIM = "#7d8ba1"
ACCENT = "#4aa8ff"
GOOD = "#3ecf8e"
WARN = "#f0b429"

CHAR_W = 8.4
LINE_H = 19
PAD = 18
FONT = ("ui-monospace, 'SF Mono', 'Cascadia Mono', Menlo, Consolas, "
        "'DejaVu Sans Mono', monospace")


def run(binary, args, db, schema):
    """Run one sextant subcommand and capture its output."""
    cmd = [binary] + args + ["--db", db, "--schema", schema]
    started = time.time()
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    elapsed = time.time() - started
    output = (result.stdout or "") + (result.stderr or "")
    return cmd, output.rstrip("\n").split("\n"), elapsed, result.returncode


def colour_for(line):
    """Pick a colour from the shape of the line, not from parsing it.

    Deliberately crude. The alternative is teaching this script the output
    format of every subcommand, which is a second place for that format to be
    written down and a second place for it to go stale.
    """
    stripped = line.strip()
    if not stripped:
        return FG
    if "100.00%" in line or " ok" in line or "verified" in line:
        return GOOD
    if stripped.startswith("!") or "warning" in line.lower() or "refused" in line:
        return WARN
    if any(k in line for k in ("F1", "P 1.0000", "ratio", "TIDX", "keys_scanned")):
        return ACCENT
    if stripped.startswith(("source", "clustering", "-", "=")):
        return DIM
    return FG


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", default="build/src/cli/sextant")
    parser.add_argument("--db", default="sextant-db")
    parser.add_argument("--schema", default="schema")
    parser.add_argument("--out", default="docs/demo.svg")
    parser.add_argument("--cols", type=int, default=96)
    parser.add_argument("--max-lines-per-step", type=int, default=14)
    parser.add_argument(
        "--animate",
        action="store_true",
        help="reveal each step in turn with SMIL. OFF BY DEFAULT: SMIL is not "
        "run by static renderers, so an animated version converts to a "
        "completely blank PNG and renders empty in some viewers. The first "
        "version of this script defaulted it on and produced exactly that.",
    )
    args = parser.parse_args()

    binary = pathlib.Path(args.binary)
    if not binary.exists():
        sys.exit(f"no binary at {binary}. Run `make build` first.")

    # The steps, in the order they tell the story: the schema is declarative,
    # the data is real, resolution is measured, lineage is verified, and the
    # query is a range scan.
    steps = [
        (["stats"], "what was ingested"),
        (["eval"], "entity resolution, on a held-out split"),
        (["resolve"], "cluster, fuse, write entities with provenance"),
        (["explain"], "the lineage round trip: the headline result"),
        (["query", "--type", "Port", "--where", "locode=NLRTM",
          "--link", "arrivals",
          "--from", "2026-04-01T00:00:00Z", "--to", "2026-07-01T00:00:00Z",
          "--show", "3"], "the quarter query"),
    ]

    frames = []
    for cmd_args, caption in steps:
        cmd, output, elapsed, code = run(str(binary), cmd_args, args.db, args.schema)
        if code != 0:
            sys.exit(f"`{' '.join(cmd)}` exited {code}:\n" + "\n".join(output))

        # Blank lines are for a terminal, not for a fixed-height figure. Dropping
        # them buys several real lines of content per step, which is the
        # difference between the held-out F1 being visible and being inside a
        # "... N lines ..." marker.
        output = [line for line in output if line.strip()]

        # Weighted toward the TAIL, because every one of these commands puts its
        # summary last and the summary is the part worth showing. An even split
        # cut the held-out F1 line out of the eval step, which was the single
        # most important line in it.
        if len(output) > args.max_lines_per_step:
            head = 2
            tail = args.max_lines_per_step - head - 1
            hidden = len(output) - head - tail
            output = (
                output[:head]
                + [f"    ... {hidden} lines ..."]
                + output[-tail:]
            )

        # The query command is long enough to run off the edge. Wrap rather than
        # clip: the flags are the point of showing it.
        display = "sextant " + " ".join(cmd_args)
        if len(display) > args.cols - 2:
            wrapped = []
            current = ""
            for token in display.split(" "):
                if current and len(current) + len(token) + 1 > args.cols - 4:
                    wrapped.append(current + " \\")
                    current = "    " + token
                else:
                    current = f"{current} {token}".strip()
            wrapped.append(current)
            display = wrapped
        else:
            display = [display]
        frames.append((display, caption, output, elapsed))

    # Layout.
    total_lines = sum(len(f[0]) + len(f[2]) + 2 for f in frames) + 2
    width = int(args.cols * CHAR_W + PAD * 2)
    height = int(total_lines * LINE_H + PAD * 2)

    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {height}" '
        f'width="{width}" height="{height}" font-family="{FONT}" font-size="13">',
        f'<rect width="{width}" height="{height}" rx="8" fill="{BG}"/>',
    ]

    # One <g> per step. With --animate each is revealed in turn; without it,
    # everything is drawn immediately.
    #
    # The default is NOT animated, and that is a deliberate correctness call.
    # SMIL only runs in a live renderer: convert an animated version to PNG, or
    # open it somewhere that does not execute animation, and every group is
    # still at its starting opacity of zero. The image is blank. A transcript
    # that renders reliably beats one that fades in prettily and sometimes shows
    # nothing at all.
    y = PAD + LINE_H
    delay = 0.0
    for display, caption, output, elapsed in frames:
        if args.animate:
            parts.append('<g opacity="0">')
            parts.append(
                f'<animate attributeName="opacity" from="0" to="1" dur="0.35s" '
                f'begin="{delay:.2f}s" fill="freeze"/>'
            )
        else:
            parts.append("<g>")

        parts.append(
            f'<text x="{PAD}" y="{y}" fill="{DIM}"># {html.escape(caption)}</text>'
        )
        y += LINE_H
        for i, part in enumerate(display):
            prompt = (f'<tspan fill="{GOOD}">$</tspan> ' if i == 0 else "  ")
            parts.append(
                f'<text x="{PAD}" y="{y}" xml:space="preserve">{prompt}'
                f'<tspan fill="{FG}">{html.escape(part)}</tspan></text>'
            )
            y += LINE_H

        for line in output:
            # Mark a clip rather than just stopping. A line that ends "geo_proxi"
            # looks like a rendering bug; one that ends "geo_proxi..." reads as
            # a deliberate cut.
            clipped = (
                line[: args.cols - 3] + "..." if len(line) > args.cols else line
            )
            parts.append(
                f'<text x="{PAD}" y="{y}" fill="{colour_for(line)}" '
                f'xml:space="preserve">{html.escape(clipped)}</text>'
            )
            y += LINE_H

        parts.append(
            f'<text x="{PAD}" y="{y}" fill="{DIM}">  ({elapsed:.2f}s)</text>'
        )
        y += LINE_H
        parts.append("</g>")
        delay += 1.4

    parts.append("</svg>")

    out = pathlib.Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text("\n".join(parts), encoding="utf-8")
    print(f"wrote {out} ({out.stat().st_size // 1024} KB, {len(frames)} steps)")


if __name__ == "__main__":
    main()
