"""Send window messages to a test instance's game window (only that window).

  sendmsg.py <test folder name> <msg,wparam,lparam> ...
"""
import sys, ctypes, subprocess, time

u = ctypes.windll.user32
name, msgs = sys.argv[1], sys.argv[2:]
ps = "Get-Process AIM,AIMII -ErrorAction SilentlyContinue | Where-Object { $_.Path -like '*\\NAME\\*' } | ForEach-Object { $_.MainWindowHandle }"
r = subprocess.run(['powershell', '-NoProfile', '-Command', ps.replace('NAME', name)], capture_output=True, text=True)
h = int(r.stdout.split()[0])
for m in msgs:
    msg, wp, lp = [int(x, 0) for x in m.split(',')]
    u.SendMessageW(h, msg, wp, lp)
    time.sleep(0.3)
print('sent to', hex(h))
