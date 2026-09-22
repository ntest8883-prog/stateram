#!/usr/bin/env python3
import argparse, os, pathlib, shutil, sys

def size_value(s):
    s=s.strip().lower()
    if s in ("max","0"): return s
    mult=1
    if s.endswith("k"): mult=1024; s=s[:-1]
    elif s.endswith("m"): mult=1024**2; s=s[:-1]
    elif s.endswith("g"): mult=1024**3; s=s[:-1]
    return str(int(float(s)*mult))

ap=argparse.ArgumentParser(
    description="Run a command in a temporary cgroup v2 memory limit."
)
ap.add_argument("--memory",required=True,help="e.g. 4G")
ap.add_argument("--swap",default="0",help="0, max, or size")
ap.add_argument("cmd",nargs=argparse.REMAINDER)
args=ap.parse_args()

if args.cmd and args.cmd[0]=="--":
    args.cmd=args.cmd[1:]
if not args.cmd:
    ap.error("command required after --")
if os.geteuid()!=0:
    print("ERROR: run this wrapper with sudo/root.",file=sys.stderr)
    sys.exit(2)

base=pathlib.Path("/sys/fs/cgroup")
if not (base/"cgroup.controllers").exists():
    print("ERROR: cgroup v2 not mounted at /sys/fs/cgroup",file=sys.stderr)
    sys.exit(2)

cg=base/f"stateram12-{os.getpid()}"
cg.mkdir()
try:
    (cg/"memory.max").write_text(size_value(args.memory))
    if (cg/"memory.swap.max").exists():
        (cg/"memory.swap.max").write_text(size_value(args.swap))

    pid=os.fork()
    if pid==0:
        try:
            (cg/"cgroup.procs").write_text(str(os.getpid()))
            # If invoked through sudo, return to the original user before exec.
            if "SUDO_GID" in os.environ:
                os.setgid(int(os.environ["SUDO_GID"]))
            if "SUDO_UID" in os.environ:
                os.setuid(int(os.environ["SUDO_UID"]))
            os.execvp(args.cmd[0],args.cmd)
        except Exception as e:
            print(f"child setup failed: {e}",file=sys.stderr)
            os._exit(127)

    _,status=os.waitpid(pid,0)
    code=os.waitstatus_to_exitcode(status)
finally:
    try:
        # Process should be gone; cgroup can now be removed.
        cg.rmdir()
    except OSError:
        pass

sys.exit(code)
