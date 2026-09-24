#!/usr/bin/env python3
import csv, glob, json, math, os, sys

def pct(xs, p):
    xs=sorted(xs)
    if not xs: return float("nan")
    x=(len(xs)-1)*p/100.0
    lo=int(math.floor(x)); hi=int(math.ceil(x))
    if lo==hi: return xs[lo]
    return xs[lo]*(hi-x)+xs[hi]*(x-lo)

def read(path):
    rows=list(csv.DictReader(open(path,newline="")))
    exp=[float(r["experienced_ms"]) for r in rows]
    return {
        "file": path,
        "profile": rows[0]["profile"] if rows else "?",
        "interactions": len(rows),
        "p50_ms": pct(exp,50),
        "p95_ms": pct(exp,95),
        "p99_ms": pct(exp,99),
        "over_10ms_pct": 100.0*sum(x>10 for x in exp)/len(exp),
        "over_50ms_pct": 100.0*sum(x>50 for x in exp)/len(exp),
        "rss_peak_mb": max(float(r["rss_mb"]) for r in rows),
        "tracked_raw_peak_mb": max(float(r["tracked_raw_mb"]) for r in rows),
        "missing_faults": sum(int(r["missing_faults"]) for r in rows),
        "write_faults": sum(int(r["write_faults"]) for r in rows),
    }

raw=read("stateram13_raw_12g_reference.csv")
runs=[]
for path in sorted(glob.glob("qual_*.csv")):
    r=read(path)
    r["capacity_gate"] = (
        r["rss_peak_mb"] < 4096.0 and
        r["tracked_raw_peak_mb"] <= 3000.5 and
        r["missing_faults"] > 0 and
        r["write_faults"] > 0
    )
    r["interactive_gate"] = (
        r["p95_ms"] <= 2.0 and
        r["p99_ms"] <= 10.0 and
        r["over_10ms_pct"] <= 1.0 and
        r["over_50ms_pct"] == 0.0
    )
    r["pass"] = r["capacity_gate"] and r["interactive_gate"]
    runs.append(r)

expected=12
profiles={}
for r in runs:
    profiles.setdefault(r["profile"], []).append(r)

summary={
    "raw_reference": raw,
    "expected_runs": expected,
    "actual_runs": len(runs),
    "profiles_seen": sorted(profiles),
    "runs": runs,
    "worst_case": {
        "p95_ms": max((r["p95_ms"] for r in runs), default=None),
        "p99_ms": max((r["p99_ms"] for r in runs), default=None),
        "over_10ms_pct": max((r["over_10ms_pct"] for r in runs), default=None),
        "over_50ms_pct": max((r["over_50ms_pct"] for r in runs), default=None),
        "rss_peak_mb": max((r["rss_peak_mb"] for r in runs), default=None),
    }
}
summary["release_qualified"] = (
    len(runs)==expected and
    set(profiles)=={"normal","bursty","high-switch","low-locality"} and
    all(r["pass"] for r in runs)
)
open("stateram13_release_qualification.json","w").write(json.dumps(summary,indent=2)+"\n")
print(json.dumps(summary,indent=2))
print("\nSTATERAM13_RELEASE_QUALIFICATION=" + ("PASS" if summary["release_qualified"] else "FAIL"))
sys.exit(0 if summary["release_qualified"] else 10)
