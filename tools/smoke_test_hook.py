# -*- coding: utf-8 -*-
"""冒烟测试 (V65 hook 版):
   假客户端 "Client thread" 每 10ms 调真实 gdi32!SwapBuffers
   -> injector 注入 V65 DLL
   -> DLL 在渲染帧钩子内 GetEnv 复用 JNIEnv 完成解析/采样
   -> 校验共享内存 (reader_v7.exe) + UDP 35785 转换点序列

   判定预期 (TestSwapClient 阶段表):
     canAttack: 阶段1=0 阶段2/3/4=1 阶段5=0
     canPlace : 阶段1=0 阶段2=1 阶段3=0 阶段4=0 阶段5=1
"""
import subprocess, socket, time, os, re, sys

MC = r"D:\VibeCoding\MCCombatStatusJni"
SRC = os.path.join(MC, "test", "swapclient")
OUT = os.path.join(SRC, "out")
STUB = os.path.join(SRC, "native", "swapstub.dll")
DEV = subprocess.DEVNULL

def die(msg):
    print("FAIL: " + msg)
    try:
        if 'java' in dir() and java.poll() is None:
            java.kill()
    except Exception:
        pass
    sys.exit(1)

# 0. 清理历史冒烟测试残留 (同名假客户端)
subprocess.run(["taskkill", "/F", "/IM", "java.exe"], capture_output=True)
time.sleep(0.5)

# 1. 编译假客户端
os.makedirs(OUT, exist_ok=True)
r = subprocess.run(["javac", "-encoding", "UTF-8", "-sourcepath", SRC, "-d", OUT,
                    os.path.join(SRC, "TestSwapClient.java")],
                   capture_output=True, text=True, errors="replace")
if r.returncode != 0:
    die("javac:\n" + r.stderr)
if not os.path.exists(STUB):
    die("swapstub.dll 不存在, 请先运行 build.bat")

# 2. 启动假客户端 (classpath 带伪造 versions 目录 -> classpath 版本检测定位 1.20.1)
cp = OUT + os.pathsep + os.path.join(SRC, "versions", "1.20.1", "fake.jar")
java = subprocess.Popen(["java", "-cp", cp, "TestSwapClient"], cwd=MC, stdout=DEV, stderr=DEV)
time.sleep(2.0)

# 3. UDP 监听先绑好 (注入前绑定, 不漏包)
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("127.0.0.1", 35785))
s.settimeout(0.3)

# 4. 注入
inj = subprocess.run([os.path.join(MC, "injector.exe")],
                     capture_output=True, text=True, errors="replace")
m = re.search(r"PID\s*=\s*(\d+)", inj.stdout + inj.stderr)
if not m:
    die("injector 未找到进程:\n" + inj.stdout + inj.stderr)
pid = int(m.group(1))
print("injected PID =", pid)

# 5. 等解析完成, 读共享内存 (此时应处于阶段2: canAttack=1 canPlace=1)
time.sleep(2.5)
r = subprocess.run([os.path.join(MC, "verify", "reader_v7.exe"), str(pid)],
                   capture_output=True, text=True, errors="replace")
print("=== reader @ ~4.5s ===\n%s" % r.stdout, flush=True)
if "ready=1" not in r.stdout:
    die("DLL 未解析成功 (ready != 1)")
if "canAttack=1 canPlace=1" not in r.stdout:
    die("阶段2 期望 canAttack=1 canPlace=1")
if "tick=0" in r.stdout:
    die("tick 未递增 (帧驱动采样未工作)")
mp = re.search(r"map=\[([^\]]*)\]", r.stdout)
if not mp or "1201" not in mp.group(1):
    die("映射表未命中 1.20.1: " + (mp.group(1) if mp else "?"))

# 6. UDP 转换点 (t=0 是注入时刻)
t0 = time.time()
last = None
trans = []
while time.time() - t0 < 15.0:
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

# 7. 序列校验: 依次出现 00 -> 11 -> 10 -> 01
seq = ["%s%s" % (t[1], t[2]) for t in trans]
expect = ["00", "11", "10", "01"]
idx = 0
for v in seq:
    if v == expect[idx]:
        idx += 1
        if idx >= len(expect):
            break
if idx < len(expect):
    die("UDP 转换序列不完整: %s (期望依次出现 %s)" % (seq, expect))

print("PASS: 共享内存 ready=1 + 映射命中 + UDP 转换序列 %s" % expect)
