"""Make a disposable test copy of a game install.

Large read-only assets (archives, videos, textures, >4 MB files under Data)
are hardlinked, everything else is copied, so the game can't write through
to the original.
  python mk_testcopy.py <dest> [source]     (source: "cw" = GOG A.I.M. 2 Clan Wars (default), "aim1" = GOG A.I.M., or a folder)
"""
import os, sys, shutil

SOURCES = {
    'cw': r'D:\GOG Galaxy\Games\AIM 2',
    'aim1': r'D:\GOG Galaxy\Games\A.I.M. Artificial Intelligence Machines',
}
SKIP = set()
RO_EXT = {'.pak', '.bik', '.wav', '.ogg', '.mp3', '.tm', '.tga', '.dds', '.mdl', '.res', '.pdf'}


def main(dst, src):
    if os.path.exists(dst):
        sys.exit('destination exists: ' + dst)
    nl = nc = 0
    for root, dirs, files in os.walk(src):
        dirs[:] = [d for d in dirs if d not in SKIP]
        rel = os.path.relpath(root, src)
        os.makedirs(os.path.join(dst, rel), exist_ok=True)
        in_data = rel.lower().startswith('data')
        for f in files:
            s, d = os.path.join(root, f), os.path.join(dst, rel, f)
            ext = os.path.splitext(f)[1].lower()
            if (in_data and (ext in RO_EXT or os.path.getsize(s) > 4 << 20)) or ext == '.pdf':
                os.link(s, d); nl += 1
            else:
                shutil.copy2(s, d); nc += 1
    print(dst, 'linked', nl, 'copied', nc)


if __name__ == '__main__':
    src = SOURCES.get(sys.argv[2], sys.argv[2]) if len(sys.argv) > 2 else SOURCES['cw']
    main(sys.argv[1], src)
