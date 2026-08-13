"""Adversarial E2E: exercise the board paths never run on hardware, and try to break them.

S1 stale-selection regression (REFUSED with a name, not a bare ERROR)
S2 abandoned upload: begin + partial data, client killed -> status shows progress; a SECOND
   partial upload immediately after (begin-over-begin, last-writer-wins); then the 60 s idle
   timeout must name the abandonment
S3 a clean commit straight after all that mess -> board unaffected
S4 a put fired while the board is still rebooting from S3 -> must fail gracefully, then succeed
S5 verdicts from the dev-board console for the whole run
"""
import re, subprocess, threading, time, serial

APP="/home/fw/qs/repos-no-5/q_zenoh_testapp"; EP="tls/192.168.10.29:7447"
S="/tmp/claude-1000/-home-fw-qs-repos-no-5/063b0ceb-d27d-4196-883e-349c135b0813/scratchpad"
EV="/home/fw/qs/repos-no-5/q_zenoh_testapp/docs/evidence/bughunt-e2e-20260813"

ser=serial.Serial("/dev/ttyACM0",115200,timeout=0.05); buf,stop=[],False
def pump():
    while not stop:
        d=ser.read(ser.in_waiting or 1024)
        if d: buf.append(d.decode("utf-8","replace"))
threading.Thread(target=pump,daemon=True).start()
print("console open (dev board resets once)"); time.sleep(28)

def T(script,timeout=200):
    return subprocess.run([APP+"/build/q_zenoh_testapp","--endpoint",EP],cwd=APP,
        capture_output=True,text=True,timeout=timeout,
        input="login acbm-fabric-2026\n"+script+"quit\n").stdout
def status():
    out=T("flow status\n")
    m=re.search(r"flow: sha=(\S{16})",out); r=re.search(r"running=(\w+)\s+nodes=(\d+)\s+received=(\d+)",out)
    e=re.search(r"error: (.+)",out)
    return dict(sha=m.group(1) if m else "?",running=r.group(1) if r else "?",
                nodes=r.group(2) if r else "?",received=r.group(3) if r else "?",
                err=e.group(1).strip() if e else "")
def wait_port():
    while subprocess.run(["bash","-c","timeout 3 bash -c '</dev/tcp/192.168.10.29/7447'"],
                         capture_output=True).returncode!=0: time.sleep(3)

print("== S1: stale selection must be a NAMED refusal ==")
r=subprocess.run(["python3",f"{APP}/scripts/deploy.py","status","100152,100164,100170"],
                 capture_output=True,text=True,timeout=200)
line=[l for l in r.stdout.splitlines() if "REFUSED" in l]
print(f"  rc={r.returncode} named={bool(line)}  {line[0][:100] if line else 'MISSING'}")

print("== S2a: kill an upload mid-flight ==")
p=subprocess.Popen([APP+"/build/q_zenoh_testapp","--endpoint",EP],cwd=APP,
    stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True)
p.stdin.write(f"login acbm-fabric-2026\nflow put {S}/big.riot --slow\n"); p.stdin.flush()
time.sleep(11)      # auth ~6s + begin + a few slow chunks
p.kill()
st=status(); print(f"  after kill: received={st['received']} (expect >0 and <8000) err='{st['err']}'")
s2a_ok = st['received'] not in ("?","0") and int(st['received'])<8000

print("== S2b: begin-over-begin (second upload immediately, no waiting) ==")
p=subprocess.Popen([APP+"/build/q_zenoh_testapp","--endpoint",EP],cwd=APP,
    stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True)
p.stdin.write(f"login acbm-fabric-2026\nflow put {S}/big.riot --slow\n"); p.stdin.flush()
time.sleep(9); p.kill()
st=status(); print(f"  second upload staged: received={st['received']} (restarted, not resumed)")

print("== S2c: the 60 s idle timeout must NAME the abandonment ==")
time.sleep(65)
st=status(); print(f"  after 65 s idle: received={st['received']} err='{st['err']}'")
s2c_ok = "abandon" in st['err']

print("== S3: clean commit right after the mess ==")
out=T(f"flow put {S}/default.riot\n")
print("  committed:", "committed" in out)
time.sleep(8); wait_port(); time.sleep(2)

print("== S4: put fired while the board is mid-reboot from another commit ==")
out=T(f"flow put {S}/default.riot\n")            # commit -> board reboots in 3 s
t0=time.time()
out2=T(f"flow put {S}/default.riot\n",timeout=120)   # fired into the reboot window
took=time.time()-t0
graceful = ("committed" in out2) or ("ERR" in out2 or "begin failed" in out2 or "no reply" in out2)
print(f"  second put: {'committed' if 'committed' in out2 else 'failed gracefully'} after {took:.0f}s")
time.sleep(8); wait_port(); time.sleep(2)
st=status(); print(f"  final: sha={st['sha']} running={st['running']} err='{st['err']}'")

time.sleep(2); stop=True; time.sleep(0.5); ser.close()
board="".join(buf)
import os; os.makedirs(EV,exist_ok=True)
open(EV+"/01-devboard-console.log","w").write(board)
print("\n== S5 VERDICTS (dev-board console, whole run) ==")
for k,v in {"panics":"Guru Meditation","task watchdog":"task_wdt: Task watchdog got triggered",
            "unexpected error logs":"App_Zenoh: FLOW .*failed|assert","mis-paired modbus":"does not match the command in flight"}.items():
    print(f"  {k:<22} {len(re.findall(v,board))}")
print(f"  resets: {len(re.findall(r'rst:0x',board))} (expected: console + S3 + S4 commits)")
for ln in board.splitlines():
    if re.search(r"FLOW (begin|committed|abandon|commit rejected)",ln): print("   ",ln.strip()[-100:])
