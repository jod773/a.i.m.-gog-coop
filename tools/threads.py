"""Dump every thread of a (32-bit) AIMII.exe: eip and the code addresses on its stack.

  threads.py <folder-substring>
Addresses inside AIMII.exe are shown unrelocated (as in the disassembly), others as module+offset.
"""
import sys, ctypes, ctypes.wintypes as w, subprocess, struct

k = ctypes.WinDLL('kernel32', use_last_error=True)
psapi = ctypes.WinDLL('psapi')
sub = sys.argv[1].lower()
EXE = sys.argv[2] if len(sys.argv) > 2 else 'AIMII.exe'
r = subprocess.run(['powershell', '-NoProfile', '-Command',
                    "Get-CimInstance Win32_Process -Filter \"Name='" + EXE + "'\" | % { \"$($_.ProcessId)|$($_.ExecutablePath)\" }"],
                   capture_output=True, text=True)
pid = [int(l.split('|')[0]) for l in r.stdout.splitlines() if '|' in l and sub in l.lower()][0]
PROCESS_ALL = 0x1F0FFF
hp = k.OpenProcess(PROCESS_ALL, False, pid)

# modules
mods = []
hm = (ctypes.c_void_p * 1024)()
need = w.DWORD()
psapi.EnumProcessModulesEx(hp, hm, ctypes.sizeof(hm), ctypes.byref(need), 3)
for i in range(need.value // ctypes.sizeof(ctypes.c_void_p)):
    name = ctypes.create_unicode_buffer(260)
    psapi.GetModuleBaseNameW(hp, ctypes.c_void_p(hm[i]), name, 260)

    class MI(ctypes.Structure):
        _fields_ = [('base', ctypes.c_void_p), ('size', w.DWORD), ('entry', ctypes.c_void_p)]
    mi = MI()
    psapi.GetModuleInformation(hp, ctypes.c_void_p(hm[i]), ctypes.byref(mi), ctypes.sizeof(mi))
    mods.append((mi.base or 0, mi.size, name.value))
exe = [m for m in mods if m[2].lower() == EXE.lower()][0]
delta = exe[0] - 0x400000


def desc(a):
    for b, s, n in mods:
        if b <= a < b + s:
            if n.lower() == EXE.lower():
                return '%08x' % (a - delta)
            return '%s+%x' % (n, a - b)
    return None


class WOW64_CONTEXT(ctypes.Structure):
    _fields_ = [('ContextFlags', w.DWORD), ('Dr', w.DWORD * 6), ('FloatSave', ctypes.c_byte * 112),
                ('SegGs', w.DWORD), ('SegFs', w.DWORD), ('SegEs', w.DWORD), ('SegDs', w.DWORD),
                ('Edi', w.DWORD), ('Esi', w.DWORD), ('Ebx', w.DWORD), ('Edx', w.DWORD), ('Ecx', w.DWORD), ('Eax', w.DWORD),
                ('Ebp', w.DWORD), ('Eip', w.DWORD), ('SegCs', w.DWORD), ('EFlags', w.DWORD), ('Esp', w.DWORD),
                ('SegSs', w.DWORD), ('Ext', ctypes.c_byte * 512)]


class TE(ctypes.Structure):
    _fields_ = [('dwSize', w.DWORD), ('cntUsage', w.DWORD), ('th32ThreadID', w.DWORD), ('th32OwnerProcessID', w.DWORD),
                ('tpBasePri', w.LONG), ('tpDeltaPri', w.LONG), ('dwFlags', w.DWORD)]


snap = k.CreateToolhelp32Snapshot(4, 0)
te = TE()
te.dwSize = ctypes.sizeof(te)
ok = k.Thread32First(snap, ctypes.byref(te))
while ok:
    if te.th32OwnerProcessID == pid:
        ht = k.OpenThread(0x1FFFFF, False, te.th32ThreadID)
        k.Wow64SuspendThread(ht)
        ctx = WOW64_CONTEXT()
        ctx.ContextFlags = 0x10007
        if k.Wow64GetThreadContext(ht, ctypes.byref(ctx)):
            buf = ctypes.create_string_buffer(0x8000)
            got = ctypes.c_size_t()
            k.ReadProcessMemory(hp, ctypes.c_void_p(ctx.Esp), buf, 0x8000, ctypes.byref(got))
            frames = []
            for i in range(0, got.value - 3, 4):
                v = struct.unpack_from('<I', buf.raw, i)[0]
                d = desc(v)
                if d and not d.startswith(('ntdll', 'KERNEL', 'kernel', 'wow64')) or (d and len(frames) < 3):
                    frames.append(d)
                if len(frames) >= (200 if len(sys.argv) > 3 else 24):
                    break
            if len(sys.argv) > 3:
                frames = [f for f in frames if not f.startswith(('nv', 'ucrt', 'shcore', 'combase'))]
            if len(sys.argv) <= 3 or str(te.th32ThreadID) in sys.argv[3].split(','):
                print('thread %d eip %s: %s' % (te.th32ThreadID, desc(ctx.Eip), ' '.join(frames)))
        k.ResumeThread(ht)
        k.CloseHandle(ht)
    ok = k.Thread32Next(snap, ctypes.byref(te))
