#!/usr/bin/env python3
import argparse, csv, json, math, statistics

def pct(xs,p):
    xs=sorted(xs)
    if not xs:return 0.0
    k=(len(xs)-1)*p/100
    lo=math.floor(k);hi=math.ceil(k)
    if lo==hi:return xs[lo]
    return xs[lo]*(hi-k)+xs[hi]*(k-lo)

def load(path):
    rows=[]
    with open(path,newline="") as f:
        for r in csv.DictReader(f):
            rows.append(r)
    return rows

def summarize(path):
    rows=load(path)
    exp=[float(r["experienced_ms"]) for r in rows]
    fg=[float(r["foreground_ms"]) for r in rows]
    late=[float(r["lateness_ms"]) for r in rows]
    rss=[float(r["rss_mb"]) for r in rows]
    raw=[float(r["tracked_raw_mb"]) for r in rows]
    miss=[int(r["missing_faults"]) for r in rows]
    write=[int(r["write_faults"]) for r in rows]
    return {
        "file":path,
        "interactions":len(rows),
        "experienced_mean_ms":statistics.mean(exp) if exp else 0,
        "experienced_p50_ms":pct(exp,50),
        "experienced_p95_ms":pct(exp,95),
        "experienced_p99_ms":pct(exp,99),
        "over_10ms_pct":100*sum(x>10 for x in exp)/max(1,len(exp)),
        "over_50ms_pct":100*sum(x>50 for x in exp)/max(1,len(exp)),
        "foreground_p95_ms":pct(fg,95),
        "lateness_p95_ms":pct(late,95),
        "rss_peak_mb":max(rss) if rss else 0,
        "tracked_raw_peak_mb":max(raw) if raw else 0,
        "missing_faults":sum(miss),
        "write_faults":sum(write),
    }

ap=argparse.ArgumentParser()
ap.add_argument("csv",nargs="+")
args=ap.parse_args()
out=[summarize(p) for p in args.csv]
print(json.dumps(out,indent=2))
