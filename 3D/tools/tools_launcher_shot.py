"""跑一次真实的启动动画（全屏），中途截两张图。

用法： python tools/tools_launcher_shot.py [时长秒]
"""
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
PY = sys.executable

seconds = sys.argv[1] if len(sys.argv) > 1 else "9"


def shot(name):
    subprocess.run([PY, os.path.join(HERE, "tools_screenshot.py"),
                    os.path.join(ROOT, "shots", name)], check=True)


exe = os.path.join(ROOT, "AuroraLauncher.exe")
p = subprocess.Popen([exe, "--seconds", seconds])

time.sleep(3.2)          # 淡入 + 推近结束
shot("live_a.png")
time.sleep(2.6)
shot("live_b.png")

p.wait(timeout=30)
print("done")
