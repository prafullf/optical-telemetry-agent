#!/usr/bin/env python3
"""Offline analysis of ota-agent JSON-lines logs.

* Alarm report: raise/clear counts per alarm type and time-to-clear per port.
* Early warning: a rolling z-score detector over the raw telemetry that flags a
  port whose readings are drifting away from their own recent baseline --
  before the fixed-threshold alarm in the agent fires. The report shows the
  lead time (in samples) the detector gained over the threshold alarm.

Standard library only, so it runs anywhere the agent runs.
"""

from __future__ import annotations

import argparse
import json
import math
import statistics
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass, asdict
from typing import Dict, Iterable, List, Optional

# metric -> (function applied to the raw value, alarm types it predicts)
METRICS = {
    "pre_fec_ber": (lambda v: math.log10(max(v, 1e-15)), {"PRE_FEC_BER_HIGH", "OSNR_LOW", "LOS"}),
    "temp_c": (lambda v: v, {"TEMP_HIGH"}),
    "rx_dbm": (lambda v: v, {"RX_POWER_LOW", "LOS"}),
    "osnr_db": (lambda v: v, {"OSNR_LOW", "PRE_FEC_BER_HIGH", "LOS"}),
    "tx_dbm": (lambda v: v, {"TX_POWER_LOW"}),
}


@dataclass
class EarlyWarning:
    port: int
    metric: str
    warning_seq: int
    alarm: Optional[str]
    alarm_seq: Optional[int]
    lead_samples: Optional[int]


def load_jsonl(path: str) -> List[dict]:
    with open(path, encoding="utf-8") as fh:
        return [json.loads(line) for line in fh if line.strip()]


def alarm_report(alarms: List[dict]) -> dict:
    raised = Counter(a["alarm"] for a in alarms if a["state"] == "RAISED")
    masked = sum(1 for a in alarms if a.get("masked"))
    open_at: Dict[tuple, int] = {}
    time_to_clear: Dict[str, List[int]] = defaultdict(list)
    for a in alarms:
        key = (a["port"], a["alarm"])
        if a["state"] == "RAISED":
            open_at[key] = a["seq"]
        elif key in open_at:
            time_to_clear[a["alarm"]].append(a["seq"] - open_at.pop(key))
    return {
        "raised_by_type": dict(raised),
        "masked_clears": masked,
        "still_active": len(open_at),
        "mean_samples_to_clear": {k: round(statistics.mean(v), 1) for k, v in time_to_clear.items()},
    }


def detect_drift(values: List[float], baseline: int = 30, gap: int = 5,
                 z_limit: float = 4.0, consecutive: int = 3) -> Optional[int]:
    """Index of the first sample where |z| > z_limit for `consecutive` samples in
    a row, z being computed against the window [i-gap-baseline, i-gap)."""
    run = 0
    for i in range(baseline + gap, len(values)):
        window = values[i - gap - baseline:i - gap]
        mu = statistics.fmean(window)
        sd = statistics.pstdev(window) or 1e-9
        if abs(values[i] - mu) / sd > z_limit:
            run += 1
            if run >= consecutive:
                return i - consecutive + 1
        else:
            run = 0
    return None


def early_warnings(telemetry: List[dict], alarms: List[dict], **kw) -> List[EarlyWarning]:
    by_port: Dict[int, List[dict]] = defaultdict(list)
    for s in telemetry:
        by_port[s["port"]].append(s)
    first_raise: Dict[tuple, int] = {}
    for a in alarms:
        if a["state"] == "RAISED":
            first_raise.setdefault((a["port"], a["alarm"]), a["seq"])

    out: List[EarlyWarning] = []
    for port, samples in sorted(by_port.items()):
        samples.sort(key=lambda s: s["seq"])
        for metric, (transform, predicts) in METRICS.items():
            values = [transform(s[metric]) for s in samples]
            idx = detect_drift(values, **kw)
            if idx is None:
                continue
            warn_seq = samples[idx]["seq"]
            later = sorted((seq, name) for (p, name), seq in first_raise.items()
                           if p == port and name in predicts and seq >= warn_seq)
            alarm_seq, alarm = later[0] if later else (None, None)
            lead = alarm_seq - warn_seq if alarm_seq is not None else None
            out.append(EarlyWarning(port, metric, warn_seq, alarm, alarm_seq, lead))
    return out


def main(argv: Iterable[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--alarms", required=True, help="alarm JSON-lines log from ota-agent")
    ap.add_argument("--telemetry", help="telemetry JSON-lines log from ota-agent")
    ap.add_argument("--json", action="store_true", help="print machine-readable JSON")
    args = ap.parse_args(list(argv) if argv is not None else None)

    alarms = load_jsonl(args.alarms)
    report = {"alarms": alarm_report(alarms)}
    if args.telemetry:
        report["early_warnings"] = [asdict(w) for w in early_warnings(load_jsonl(args.telemetry), alarms)]

    if args.json:
        json.dump(report, sys.stdout, indent=2)
        print()
        return 0

    a = report["alarms"]
    print("Alarms raised by type:")
    for name, n in sorted(a["raised_by_type"].items()):
        print(f"  {name:<18} {n}")
    print(f"Masked clears (root-cause correlation): {a['masked_clears']}")
    print(f"Still active at end of log: {a['still_active']}")
    for name, v in sorted(a["mean_samples_to_clear"].items()):
        print(f"  mean samples to clear {name:<18} {v}")
    if "early_warnings" in report:
        print("\nEarly warnings (rolling z-score drift detector):")
        for w in report["early_warnings"]:
            lead = f"{w['lead_samples']} samples before {w['alarm']}" if w["alarm"] else "no threshold alarm followed"
            print(f"  port {w['port']:>3}  {w['metric']:<12} at seq {w['warning_seq']:<6} -> {lead}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
