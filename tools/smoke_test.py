# -*- coding: utf-8 -*-
"""临时冒烟测试: 假客户端 vanilla1201 + 注入 + 共享内存/UDP 转换点采样。"""
import subprocess, socket, time, os, re

MC = r"D:\VibeCoding\MCCombatStatusJni"
out = os.path.join(MC, "test", "vanilla1201", "out")
DEV = subprocess.DEVNULL

java = subprocess.Popen(["java", "-cp", out, "TestMC1201V"], cwd=MC,
                        stdout=DEV, stderr=DEV)
time.sleep(1.8)
inj = subprocess.run([os.path.join(MC, "injector.exe")],
                     capture_output=True, text=True, errors="replace")
real_pid = int(re.search(r"PID\s*=\s*(\d+)", inj.stdout + inj.stderr).group(1))

# 等解析完成, 读一次共享内存
time.sleep(2.5)
r = subprocess.run([os.path.join(MC, "verify", "reader_v7.exe"), str(real_pid)],
                   capture_output=True, text=True, errors="replace")
print("=== reader @ ~4s ===\n%s" % r.stdout, flush=True)

# UDP 只记录值变化点
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("127.0.0.1", 35785))
s.settimeout(0.3)
t0 = time.time()
last = None
trans = []
while time.time() - t0 < 14.0:
    try:
        data, _ = s.recvfrom(64)
        v = (data[0:1], data[1:2])
        if v != last:
            trans.append((round(time.time() - t0, 1), v[0].decode(), v[1].decode()))
            last = v
    except socket.timeout:
        if java.poll() is not None:
            break
s.close()
java.wait()

print("=== UDP transitions (t=0 是注入后) ===")
for t, a, p in trans:
    print("  t=%5.1fs  canAttack=%s canPlace=%s" % (t, a, p))
