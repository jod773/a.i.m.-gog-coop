"""List the top-level windows of the AIMII.exe running from a folder: wins.py <folder-substring>"""
import sys, ctypes, ctypes.wintypes as w, subprocess

sub = sys.argv[1].lower()
r = subprocess.run(['powershell', '-NoProfile', '-Command',
                    "Get-CimInstance Win32_Process -Filter \"Name='AIMII.exe'\" | % { \"$($_.ProcessId)|$($_.ExecutablePath)\" }"],
                   capture_output=True, text=True)
pids = [int(l.split('|')[0]) for l in r.stdout.splitlines() if '|' in l and sub in l.lower()]
u = ctypes.windll.user32
res = []


@ctypes.WINFUNCTYPE(ctypes.c_bool, w.HWND, w.LPARAM)
def cb(hw, l):
    pid = w.DWORD()
    u.GetWindowThreadProcessId(hw, ctypes.byref(pid))
    if pid.value in pids:
        b = ctypes.create_unicode_buffer(256)
        u.GetWindowTextW(hw, b, 256)
        c = ctypes.create_unicode_buffer(256)
        u.GetClassNameW(hw, c, 256)
        res.append((pid.value, b.value, c.value, u.IsWindowVisible(hw)))
    return True


u.EnumWindows(cb, 0)
for x in res:
    print(x)
