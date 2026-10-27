"""Install a release (or a local test build) into the player's games, keeping their settings.

  python tools/install.py                 latest GitHub release into both games
  python tools/install.py v2026.10.15-1   that release
  python tools/install.py --dist          the test build in dist/ (make_aim1.py / make_cw.py)
  python tools/install.py --test [n ...]  build origin/main + open PRs n... (default: all open
                                          PRs) and install it; prints a code id both PCs compare
  options: --aim1 <folder> / --cw <folder> (default: the folders in install.local.ini; a missing one
           is asked for and saved there), --only aim1|cw

Copies everything from AIM-Coop.zip / ClanWars-Coop.zip into the install folder except the mod's
ini (AIMCoop.ini / ClanWarsCoop.ini) when one is already there: those are the player's settings,
missing keys take defaults. Refuses to touch a folder a game is running from.
"""
import json, os, shutil, subprocess, sys, tempfile, zipfile

DEV = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from release import stamp

GAMES = {   # key (also the install.local.ini key): (zip, ini, game name)
    'aim1': ('AIM-Coop.zip', 'AIMCoop.ini', 'A.I.M.'),
    'cw': ('ClanWars-Coop.zip', 'ClanWarsCoop.ini', 'Clan Wars'),
}
LOCAL = os.path.join(DEV, 'install.local.ini')   # this PC's game folders: aim1=<folder>, cw=<folder>


def local_folders():
    out = {}
    if os.path.exists(LOCAL):
        for l in open(LOCAL, encoding='utf-8-sig', errors='replace'):
            k, _, v = l.partition('=')
            if k.strip() in GAMES and v.strip():
                out[k.strip()] = v.strip().strip('"')
    return out


def ask_folder(g):
    """Asks for a game folder (the one with the game's exe) and remembers it in install.local.ini."""
    name = GAMES[g][2]
    while True:
        try:
            d = input('Where is your %s game? Paste its folder (the one with the .exe): ' % name)
        except EOFError:                                  # no one to ask (NUL counts as a tty on Windows)
            sys.exit('\nno %s folder: add "%s=<folder>" to install.local.ini or pass --%s <folder>' % (name, g, g))
        d = d.strip().strip('"')
        if os.path.isdir(d):
            break
        print('  no such folder:', d)
    lines = open(LOCAL, encoding='utf-8-sig').read().splitlines() if os.path.exists(LOCAL) else []
    lines = [l for l in lines if l.partition('=')[0].strip() != g] + ['%s=%s' % (g, d)]
    with open(LOCAL, 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines) + '\n')
    print('  saved in install.local.ini')
    return d


def running_from(folder):
    """Paths of processes whose exe lies in the folder (the game must be closed first)."""
    ps = "Get-Process | Where-Object { $_.Path -like '%s\\*' } | ForEach-Object { $_.Path }" % \
         os.path.abspath(folder).replace("'", "''")
    r = subprocess.run(['powershell', '-NoProfile', '-Command', ps], capture_output=True, text=True)
    return [l for l in r.stdout.splitlines() if l.strip()]


def install(zpath, dst, ini):
    n = 0
    with zipfile.ZipFile(zpath) as z:
        for i in z.infolist():
            if i.is_dir():
                continue
            target = os.path.join(dst, *i.filename.split('/'))
            if i.filename == ini and os.path.exists(target):
                print('  kept your', ini)
                continue
            os.makedirs(os.path.dirname(target), exist_ok=True)
            with z.open(i) as s, open(target, 'wb') as d:
                shutil.copyfileobj(s, d)
            n += 1
    print('  copied %d files, mod built %s' % (n, stamp(os.path.join(dst, 'dinput8.dll'))))


def git(*cmd, check=True):
    r = subprocess.run(('git',) + cmd, cwd=DEV, capture_output=True, text=True)
    if check and r.returncode:
        sys.exit('failed: git %s\n%s' % (' '.join(cmd), r.stderr))
    return r


def build_test(prs):
    """Builds origin/main + the given open PRs (all open ones if none given) into dist/, on a
    throw-away local branch, then goes back to the branch you were on."""
    if git('status', '--porcelain', '--untracked-files=no').stdout.strip():
        sys.exit('uncommitted changes in the repository: commit or stash them first')
    back = git('rev-parse', '--abbrev-ref', 'HEAD').stdout.strip()
    git('fetch', '--quiet', '--prune', 'origin')
    if not prs:                                           # all open PRs (needs gh)
        r = subprocess.run(['gh', 'pr', 'list', '--state', 'open', '--json', 'number,title'],
                           cwd=DEV, capture_output=True, text=True, encoding='utf-8')
        if r.returncode:
            sys.exit('gh pr list failed (pass the PR numbers instead):\n' + r.stderr)
        pick = [[str(p['number']), p['title']] for p in json.loads(r.stdout)]
    else:
        pick = [[n, ''] for n in prs]
    pick.sort(key=lambda p: int(p[0]))
    for n, _ in pick:                                     # GitHub keeps every PR's head at pull/<n>/head
        if git('fetch', '--quiet', 'origin', '+pull/%s/head:refs/remotes/origin/pr/%s' % (n, n), check=False).returncode:
            sys.exit('no PR #%s on GitHub' % n)
    try:
        git('checkout', '--quiet', '-B', 'test/combined', 'origin/main')
        for n, title in pick:
            print('  + #%s %s' % (n, title))
            if git('merge', '--quiet', '--no-edit', 'origin/pr/' + n, check=False).returncode:
                git('merge', '--abort', check=False)
                sys.exit('#%s does not merge cleanly with the others - ask its author to update it' % n)
        for cmd in (['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', os.path.join('src', 'build.ps1')],
                    [sys.executable, os.path.join('tools', 'make_aim1.py')],
                    [sys.executable, os.path.join('tools', 'make_cw.py')]):
            if subprocess.run(cmd, cwd=DEV, stdout=subprocess.DEVNULL).returncode:
                sys.exit('build failed: ' + ' '.join(cmd))
        # same code on both PCs <=> same tree id (the build stamps always differ)
        code = git('rev-parse', '--short', 'HEAD^{tree}').stdout.strip()
        print('test build: origin/main + %s, code id %s' % (' + '.join('#' + p[0] for p in pick) or 'nothing', code))
        return code
    finally:
        git('checkout', '--quiet', back, check=False)


def main():
    args = sys.argv[1:]
    folders = local_folders()
    only, tag, dist, test = None, None, False, None
    while args:
        a = args.pop(0)
        if a in ('--aim1', '--cw') and args:
            folders[a[2:]] = args.pop(0)
        elif a == '--only' and args:
            only = args.pop(0)
        elif a == '--dist':
            dist = True
        elif a == '--test':
            test = []
            while args and args[0].lstrip('#').isdigit():
                test.append(args.pop(0).lstrip('#'))
        elif a.startswith('v'):
            tag = a
        else:
            sys.exit(__doc__)
    games = [g for g in GAMES if (only is None or g == only)]
    for g in games:
        if g not in folders or not os.path.isdir(folders[g]):
            folders[g] = ask_folder(g)
        if not os.path.isdir(folders[g]):
            sys.exit('%s does not exist' % folders[g])
        busy = running_from(folders[g])
        if busy:
            sys.exit('close the game first - running from that folder:\n  ' + '\n  '.join(busy))

    code = None
    if test is not None:
        code = build_test(test)
        dist = True
    tmp = tempfile.mkdtemp(prefix='coop-install-')
    try:
        if dist:
            src = os.path.join(DEV, 'dist')
            print('installing the test build from', src)
        else:
            src = tmp
            cmd = ['gh', 'release', 'download'] + ([tag] if tag else []) + \
                  ['-R', 'jod773/a.i.m.-gog-coop', '-p', '*.zip', '-D', tmp]
            if subprocess.run(cmd, cwd=DEV).returncode:
                sys.exit('download failed: ' + ' '.join(cmd))
            label = tag or subprocess.run(['gh', 'release', 'view', '-R', 'jod773/a.i.m.-gog-coop', '--json',
                                           'tagName', '--jq', '.tagName'], capture_output=True, text=True).stdout.strip()
            print('installing release', label)
        for g in games:
            zname, ini, _ = GAMES[g]
            print('%s -> %s' % (zname, folders[g]))
            install(os.path.join(src, zname), folders[g], ini)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print('done - start the game; the log header shows the same `mod built` stamp')
    if code:
        print('tell your friend: code id %s - theirs must be the same' % code)


if __name__ == '__main__':
    main()
