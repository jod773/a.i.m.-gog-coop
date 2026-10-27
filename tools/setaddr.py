"""Set #define values in an address header: setaddr.py <header> NAME=VALUE [...]
(adds the define if missing; keeps the comment, drops a trailing TODO)."""
import re, sys

path = sys.argv[1]
c = open(path, encoding='utf-8').read()
for kv in sys.argv[2:]:
    name, _, value = kv.partition('=')
    rx = re.compile(r'^#define\s+%s\s+.*?(\s+//.*)?$' % re.escape(name), re.M)
    m = rx.search(c)
    if m:
        cm = (m.group(1) or '').replace(' TODO', '').replace(' (unverified)', '')
        c = c[:m.start()] + '#define %-22s %s%s' % (name, value, cm) + c[m.end():]
    else:
        c = c.rstrip('\n') + '\n#define %-22s %s\n' % (name, value)
    print(name, '=', value)
open(path, 'w', encoding='utf-8', newline='\n').write(c)
