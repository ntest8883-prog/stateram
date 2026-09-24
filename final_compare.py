#!/usr/bin/env python3
import csv, json, math, sys


def percentile(values, p):
    values=sorted(values)
    if not values: return float('nan')
    x=(len(values)-1)*p/100.0
    lo=math.floor(x); hi=math.ceil(x)
    if lo==hi: return values[lo]
    return values[lo]*(hi-x)+values[hi]*(x-lo)


def read(path):
    rows=list(csv.DictReader(open(path,newline='')))
    exp=[float(r['experienced_ms']) for r in rows]
    return {
        'file': path,
        'interactions': len(rows),
        'p50_ms': percentile(exp,50),
        'p95_ms': percentile(exp,95),
        'p99_ms': percentile(exp,99),
        'over_10ms_pct': 100*sum(x>10 for x in exp)/len(exp),
        'over_50ms_pct': 100*sum(x>50 for x in exp)/len(exp),
        'rss_peak_mb': max(float(r['rss_mb']) for r in rows),
        'tracked_raw_peak_mb': max(float(r['tracked_raw_mb']) for r in rows),
        'missing_faults': sum(int(r['missing_faults']) for r in rows),
        'write_faults': sum(int(r['write_faults']) for r in rows),
    }

if len(sys.argv)!=3:
    raise SystemExit('usage: final_compare.py RAW.csv STATERAM.csv')
raw=read(sys.argv[1]); sr=read(sys.argv[2])
capacity=(sr['rss_peak_mb'] < 4096 and sr['tracked_raw_peak_mb'] <= 3000.5 and
          sr['missing_faults'] > 0 and sr['write_faults'] > 0)
interactive=(sr['p95_ms'] <= 2.0 and sr['p99_ms'] <= 10.0 and
             sr['over_10ms_pct'] <= 1.0 and sr['over_50ms_pct'] == 0.0)
out={
    'raw_reference':raw,
    'stateram13':sr,
    'capacity_gate':capacity,
    'interactive_gate':interactive,
    'candidate_gate_pass':capacity and interactive,
}
print(json.dumps(out,indent=2))
open('stateram13_final_comparison.json','w').write(json.dumps(out,indent=2)+'\n')
print('\nSTATERAM13_FINAL_CANDIDATE_GATE=' + ('PASS' if capacity and interactive else 'FAIL'))
