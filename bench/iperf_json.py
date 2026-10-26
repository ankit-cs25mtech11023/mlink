# SPDX-License-Identifier: GPL-2.0-only
"""Reduce one `iperf3 -J` result (stdin) to a CSV fragment on stdout.

TCP: "<receiver Mbit/s>,<retransmits>"     UDP: "<receiver Mbit/s>,<lost %>"
An iperf3 error yields "nan,nan" so a failed repetition stays visible in the CSV.
"""
import json
import sys

try:
    r = json.load(sys.stdin)
    end = r["end"]
    if "sum_received" in end:  # TCP
        mbps = end["sum_received"]["bits_per_second"] / 1e6
        extra = end.get("sum_sent", {}).get("retransmits", 0)
    else:  # UDP
        s = end["sum"]
        mbps = s["bits_per_second"] / 1e6
        extra = s.get("lost_percent", 0.0)
    print(f"{mbps:.1f},{extra}")
except Exception:  # noqa: BLE001 - any parse failure is a failed run
    print("nan,nan")
