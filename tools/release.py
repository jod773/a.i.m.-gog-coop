"""Build and publish a release both players install (co-op needs the same build on every PC).

  python tools/release.py ["notes"]

Only from an up-to-date, clean `main`: builds the DLLs, packages dist/AIM-Coop.zip and
dist/ClanWars-Coop.zip, tags vYYYY.MM.DD-N and creates a GitHub release with both zips and the
merged pull requests since the last release as notes.
"""
import datetime, os, re, subprocess, sys

DEV = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))


def run(*cmd, capture=False):
    r = subprocess.run(cmd, cwd=DEV, capture_output=capture, text=True)
    if r.returncode:
        sys.exit('failed (exit %d): %s%s' % (r.returncode, ' '.join(cmd), '\n' + (r.stderr or '') if capture else ''))
    return (r.stdout or '').strip()


def stamp(dll):
    """The __DATE__ __TIME__ compiled into the DLL (what the log header prints as `mod built ...`)."""
    data = open(dll, 'rb').read()
    d = re.search(rb'\0([A-Z][a-z]{2} [ \d]\d \d{4})\0', data)
    t = re.search(rb'\0(\d\d:\d\d:\d\d)\0', data)
    return (d.group(1) + b' ' + t.group(1)).decode() if d and t else '?'


def main():
    if run('git', 'rev-parse', '--abbrev-ref', 'HEAD', capture=True) != 'main':
        sys.exit('releases are made from main only')
    run('git', 'fetch', 'origin', capture=True)
    if run('git', 'status', '--porcelain', '--untracked-files=no', capture=True):
        sys.exit('uncommitted changes: commit or stash them first')
    if run('git', 'rev-parse', 'HEAD', capture=True) != run('git', 'rev-parse', 'origin/main', capture=True):
        sys.exit('main is not the same as origin/main: pull / push first')
    tags = run('git', 'tag', '--list', 'v*', capture=True).split()
    day = datetime.date.today().strftime('v%Y.%m.%d')
    n = 1 + sum(1 for t in tags if t.startswith(day + '-'))
    tag = '%s-%d' % (day, n)
    os.environ['MOD_VERSION'] = tag          # shown in the games' main menus (src/build.ps1)
    run('powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', os.path.join('src', 'build.ps1'))
    run(sys.executable, os.path.join('tools', 'make_aim1.py'))
    run(sys.executable, os.path.join('tools', 'make_cw.py'))
    last = sorted(tags)[-1] if tags else None
    log = run('git', 'log', '--merges', '--pretty=- %s', (last + '..HEAD') if last else 'HEAD', capture=True)
    notes = (sys.argv[1] + '\n\n' if len(sys.argv) > 1 else '') + ('Merged since %s:\n%s' % (last, log) if last else log)
    notes += '\n\nBuild stamps (the `mod built` line of a log session): ' + ', '.join(
        '%s %s' % (g, stamp(os.path.join(DEV, 'build', d, 'dinput8.dll'))) for g, d in (('A.I.M.', 'aim1'), ('Clan Wars', 'aim2cw')))
    run('git', 'tag', tag)
    run('git', 'push', 'origin', tag)
    zips = [os.path.join('dist', 'AIM-Coop.zip'), os.path.join('dist', 'ClanWars-Coop.zip')]
    if subprocess.run(['gh', 'auth', 'status'], cwd=DEV, capture_output=True).returncode:
        # a packaged Python (Python install manager / Store) can't read gh's login from the Windows
        # credential store: leave the last step to the shell
        p = os.path.join(DEV, 'dist', 'release-notes.md')
        open(p, 'w', encoding='utf-8').write(notes or 'Release ' + tag)
        print('tag %s pushed, but gh is not logged in from Python. Run this from the shell to publish:' % tag)
        print('gh release create %s %s --title %s --notes-file %s' % (tag, ' '.join(zips), tag, os.path.relpath(p, DEV)))
        return
    run('gh', 'release', 'create', tag, *zips, '--title', tag, '--notes', notes or 'Release ' + tag)
    print('released', tag, '- players: download both zips from the release page and install them')


if __name__ == '__main__':
    main()
