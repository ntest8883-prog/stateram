import json
import os
import struct
import subprocess
import sys

if len(sys.argv) != 2:
    raise SystemExit("usage: browser_bridge_smoke.py <native-host-exe>")

exe = sys.argv[1]
message = {
    "type": "status",
    "reclaimable": 7,
    "oldestIdleSeconds": 3600,
}
payload = json.dumps(message, separators=(",", ":")).encode("utf-8")
framed = struct.pack("<I", len(payload)) + payload

proc = subprocess.run(
    [exe],
    input=framed,
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
    timeout=15,
)

if proc.returncode != 0:
    sys.stderr.write(proc.stderr.decode("utf-8", errors="replace"))
    raise SystemExit(f"native host exited {proc.returncode}")

if len(proc.stdout) < 4:
    raise SystemExit("native host returned no framed message")

length = struct.unpack("<I", proc.stdout[:4])[0]
body = proc.stdout[4:4 + length]

if len(body) != length:
    raise SystemExit("native host returned truncated JSON frame")

reply = json.loads(body.decode("utf-8"))

if reply.get("ok") is not True:
    raise SystemExit(f"runtime/native bridge did not connect: {reply}")

pressure = int(reply.get("pressure", -1))
discard = int(reply.get("discard", -1))
minimum_idle = int(reply.get("minimumIdleSeconds", -1))

if pressure not in (0, 1, 2, 3):
    raise SystemExit(f"invalid pressure level: {pressure}")

if discard < 0 or discard > 4:
    raise SystemExit(f"invalid discard decision: {discard}")

if minimum_idle < 0:
    raise SystemExit(f"invalid idle threshold: {minimum_idle}")

print("STATERAM_BROWSER_NATIVE_MESSAGING=PASS")
print(json.dumps(reply, indent=2))
