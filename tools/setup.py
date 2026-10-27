"""One-time setup of a development / test workspace on a new PC.

  python tools/setup.py --aim1 "<A.I.M. folder>" --cw "<A.I.M. 2 Clan Wars folder>"

1. downloads the llvm-mingw toolchain into ext/ (about 175 MB zip, 1.7 GB unpacked)
2. builds the mod (src/build.ps1) and the packages (tools/make_aim1.py, tools/make_cw.py)
3. makes disposable test copies of the games: aim1game, aim1game2, cwgame, cwgame2
   (big read-only data is hard-linked when the game is on the same drive, else copied)
4. installs the mod into them with test settings: windowed, test harness on (virtual input,
   screenshots, commands through rr_cmd.txt), every video skipped, loopback co-op

Either game can be left out. Run again with --rebuild to only rebuild and refresh the mod files
in existing test copies.
"""
import argparse, io, os, shutil, subprocess, sys, urllib.request, zipfile

DEV = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
TOOLCHAIN = 'llvm-mingw-20261006-msvcrt-x86_64'
TOOLCHAIN_URL = 'https://github.com/mstorsjo/llvm-mingw/releases/download/20261006/' + TOOLCHAIN + '.zip'
RO_EXT = {'.pak', '.bik', '.wav', '.ogg', '.mp3', '.tm', '.tga', '.dds', '.mdl', '.res', '.pdf'}

TEST_INI = {
    'aim1': ('AIMCoop.ini', 'AIM.exe'),
    'cw': ('ClanWarsCoop.ini', 'AIMII.exe'),
}


def toolchain():
    dst = os.path.join(DEV, 'ext', TOOLCHAIN)
    if os.path.exists(os.path.join(dst, 'bin')):
        print('toolchain: present')
        return
    os.makedirs(os.path.join(DEV, 'ext'), exist_ok=True)
    print('toolchain: downloading', TOOLCHAIN_URL)
    data = urllib.request.urlopen(TOOLCHAIN_URL).read()
    print('toolchain: unpacking')
    zipfile.ZipFile(io.BytesIO(data)).extractall(os.path.join(DEV, 'ext'))


def build():
    r = subprocess.run(['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
                        os.path.join(DEV, 'src', 'build.ps1')])
    if r.returncode:
        sys.exit('build failed')
    for m in ('make_aim1.py', 'make_cw.py'):
        subprocess.run([sys.executable, os.path.join(DEV, 'tools', m)], check=True)


def copy_game(src, dst):
    if os.path.exists(dst):
        print(dst, 'exists: kept')
        return
    same_drive = os.path.splitdrive(os.path.abspath(src))[0].lower() == os.path.splitdrive(dst)[0].lower()
    nl = nc = 0
    for root, dirs, files in os.walk(src):
        rel = os.path.relpath(root, src)
        os.makedirs(os.path.join(dst, rel), exist_ok=True)
        in_data = rel.lower().startswith('data')
        for f in files:
            s, d = os.path.join(root, f), os.path.join(dst, rel, f)
            ext = os.path.splitext(f)[1].lower()
            big = in_data and (ext in RO_EXT or os.path.getsize(s) > 4 << 20)
            if same_drive and big:
                try:
                    os.link(s, d); nl += 1
                    continue
                except OSError:
                    pass
            shutil.copy2(s, d); nc += 1
    print(dst, 'linked', nl, 'copied', nc)


def ini_set(path, values, force=True):
    """Writes {section: {key: value}} into an ini in place. Windows reads only the first section
    of a name and the first key in it, so keys go into that first section (appending a second
    [Video] would be ignored). force=False keeps keys that are already there."""
    lines = open(path, encoding='latin-1', newline='').read().splitlines(True) if os.path.exists(path) else []
    nl = '\r\n' if any(l.endswith('\r\n') for l in lines) else '\n'
    if lines and not lines[-1].endswith('\n'):
        lines[-1] += nl
    for sec, keys in values.items():
        start = end = None                                # first [sec]: header line, end of body
        for i, l in enumerate(lines):
            s = l.strip()
            if start is None:
                if s.lower() == '[%s]' % sec.lower():
                    start = end = i + 1
            elif s.startswith('['):
                break
            elif s:
                end = i + 1
        if start is None:
            lines += ([nl] if lines and lines[-1].strip() else []) + ['[%s]%s' % (sec, nl)]
            start = end = len(lines)
        for k, v in keys.items():
            for i in range(start, end):
                if lines[i].split('=', 1)[0].strip().lower() == k.lower() and '=' in lines[i]:
                    if force:
                        lines[i] = '%s=%s%s' % (k, v, nl)
                    break
            else:
                lines.insert(end, '%s=%s%s' % (k, v, nl))
                end += 1
    with open(path, 'w', encoding='latin-1', newline='') as f:
        f.writelines(lines)


def install_mod(game, dst, name):
    pkg = os.path.join(DEV, 'dist', 'AIM-Coop' if game == 'aim1' else 'ClanWars-Coop')
    ini_name, _ = TEST_INI[game]
    for root, _, files in os.walk(pkg):
        rel = os.path.relpath(root, pkg)
        os.makedirs(os.path.join(dst, rel), exist_ok=True)
        for f in files:
            if f == ini_name and os.path.exists(os.path.join(dst, f)):
                continue                                  # keep the test settings
            shutil.copy2(os.path.join(root, f), os.path.join(dst, rel, f))
    ini = os.path.join(dst, ini_name)
    # the test rules always win; the rest is only filled in when missing
    ini_set(ini, {'Dev': {'Harness': '1', 'SkipVideos': '1'},
                  'Video': {'DisplayMode': 'windowed', 'IntroVideos': '0'},
                  'Coop': {'Name': name, 'Loopback': '1'}})
    ini_set(ini, {'Dev': {'WindowX': '0', 'WindowY': '0'}}, force=False)
    # Clan Wars: the GOG folder's d3d8.dll is a wrapper the mod bypasses anyway; keep it out of the way
    wrapper = os.path.join(dst, 'd3d8.dll')
    if game == 'cw' and os.path.exists(wrapper):
        os.replace(wrapper, wrapper + '.off')
    print(dst, 'mod installed')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--aim1', help='A.I.M. game folder (with AIM.exe)')
    ap.add_argument('--cw', help='A.I.M. 2 Clan Wars game folder (with AIMII.exe)')
    ap.add_argument('--rebuild', action='store_true', help='only rebuild and refresh existing test copies')
    a = ap.parse_args()
    toolchain()
    build()
    games = []
    if a.rebuild:
        games = [g for g in ('aim1', 'cw') if os.path.exists(os.path.join(DEV, g.replace('cw', 'cwgame').replace('aim1', 'aim1game')))]
    else:
        for g, path in (('aim1', a.aim1), ('cw', a.cw)):
            if not path:
                continue
            if not os.path.exists(os.path.join(path, TEST_INI[g][1])):
                sys.exit('%s not found in %s' % (TEST_INI[g][1], path))
            games.append(g)
            base = 'aim1game' if g == 'aim1' else 'cwgame'
            for suffix in ('', '2'):
                copy_game(path, os.path.join(DEV, base + suffix))
    for g in games:
        base = 'aim1game' if g == 'aim1' else 'cwgame'
        install_mod(g, os.path.join(DEV, base), 'Host')
        install_mod(g, os.path.join(DEV, base + '2'), 'Guest')
    print('done. Drive the test copies with tools/h.py, tools/coop1.py (A.I.M.) and tools/cw2.py (Clan Wars).')


if __name__ == '__main__':
    main()
