#!/usr/bin/env python3
"""Summarise or render a printer communication capture.

The capture is written to <data_dir>/log/printer_comm/*.jsonl by a build configured with
-DORCA_PRINTER_COMM_LOG=ON. Each line is one record; see PrinterCommLogger.hpp.

  printer_comm_report.py capture.jsonl              # inventory: what channels appeared
  printer_comm_report.py capture.jsonl --md out.md  # human-readable transcript
  printer_comm_report.py capture.jsonl --cmd print.push_status --md -
"""

import argparse
import collections
import json
import sys


def load(path):
    records = []
    with open(path, encoding="utf-8") as handle:
        for lineno, line in enumerate(handle, 1):
            line = line.strip()
            if not line:
                continue
            try:
                records.append(json.loads(line))
            except json.JSONDecodeError:
                print(f"{path}:{lineno}: skipping unparseable line", file=sys.stderr)
    return records


def label(rec):
    if rec.get("kind") == "event":
        return f'{rec.get("cat", "?")}.{rec.get("name", "?")}'
    return rec.get("cmd", "?")


def inventory(records):
    """What the capture actually contains -- the first thing to check before writing prose."""
    by_channel = collections.Counter()
    by_message = collections.Counter()
    suppressed = collections.Counter()
    devices = set()

    for rec in records:
        if rec.get("kind") == "event":
            channel = rec.get("cat", "?")
        else:
            channel = f'{rec.get("transport", "?")} {rec.get("dir", "?")}'
        by_channel[channel] += 1
        by_message[(channel, label(rec))] += 1
        suppressed[label(rec)] += rec.get("rate_limited", 0)
        if rec.get("dev"):
            devices.add(rec["dev"])

    print(f"{len(records)} records, {len(devices)} device(s): {', '.join(sorted(devices)) or 'none'}")
    if records:
        print(f"{records[0].get('ts')}  ..  {records[-1].get('ts')}")
    print()
    for channel, count in by_channel.most_common():
        print(f"{count:>7}  {channel}")
        for (chan, name), n in by_message.most_common():
            if chan != channel:
                continue
            extra = f"   (+{suppressed[name]} rate-limited)" if suppressed[name] else ""
            print(f"         {n:>6}  {name}{extra}")
        print()


def render(records, out):
    for rec in records:
        head = f'## {rec.get("ts", "")}  {label(rec)}'
        meta = []
        for key in ("dir", "transport", "dev", "qos", "rc", "msg_kind", "consumed", "http_status"):
            if key in rec:
                meta.append(f"{key}={rec[key]}")
        out.write(head + "\n")
        if meta:
            out.write("`" + "  ".join(meta) + "`\n\n")
        body = rec.get("payload", rec.get("fields", rec.get("raw")))
        if body is not None:
            out.write("```json\n")
            out.write(body if isinstance(body, str) else json.dumps(body, indent=2, ensure_ascii=False))
            out.write("\n```\n\n")
        if rec.get("rate_limited"):
            out.write(f'_{rec["rate_limited"]} similar frames rate-limited before this one._\n\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("capture", help="a printer_comm_*.jsonl file")
    parser.add_argument("--md", metavar="FILE", help="render a markdown transcript ('-' for stdout)")
    parser.add_argument("--cmd", help="only records whose command / event name contains this")
    parser.add_argument("--dev", help="only this device pseudonym")
    args = parser.parse_args()

    records = load(args.capture)
    if args.cmd:
        records = [r for r in records if args.cmd in label(r)]
    if args.dev:
        records = [r for r in records if r.get("dev") == args.dev]

    if args.md:
        if args.md == "-":
            render(records, sys.stdout)
        else:
            with open(args.md, "w", encoding="utf-8") as handle:
                render(records, handle)
            print(f"wrote {args.md} ({len(records)} records)")
    else:
        inventory(records)


if __name__ == "__main__":
    main()
