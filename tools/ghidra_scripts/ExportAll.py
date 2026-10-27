# Ghidra headless script: export decompiled C for every function, plus string xrefs.
# @runtime Jython
from ghidra.app.decompiler import DecompInterface, DecompileOptions
from ghidra.util.task import ConsoleTaskMonitor
import os, codecs

args = getScriptArgs()
outdir = args[0] if args else '.'
if not os.path.isdir(outdir):
    os.makedirs(outdir)

prog = currentProgram
listing = prog.getListing()
fm = prog.getFunctionManager()
mon = ConsoleTaskMonitor()

# ---- strings with xrefs
refm = prog.getReferenceManager()
with codecs.open(os.path.join(outdir, 'strings.txt'), 'w', 'utf-8') as f:
    for d in listing.getDefinedData(True):
        t = d.getDataType().getName().lower()
        if 'string' not in t and 'unicode' not in t:
            continue
        try:
            v = d.getValue()
        except:
            continue
        if v is None:
            continue
        xs = [r.getFromAddress() for r in refm.getReferencesTo(d.getAddress())]
        fns = []
        for a in xs:
            fn = fm.getFunctionContaining(a)
            fns.append('%s@%s' % (fn.getName() if fn else '?', a))
        f.write(u'%s\t%r\t%s\n' % (d.getAddress(), v, ' '.join(fns)))

# ---- decompile all
di = DecompInterface()
opts = DecompileOptions()
di.setOptions(opts)
di.openProgram(prog)
with codecs.open(os.path.join(outdir, 'decomp.c'), 'w', 'utf-8') as f:
    n = 0
    for fn in fm.getFunctions(True):
        res = di.decompileFunction(fn, 60, mon)
        f.write(u'\n// ==== %s @ %s\n' % (fn.getName(), fn.getEntryPoint()))
        if res and res.decompileCompleted():
            f.write(res.getDecompiledFunction().getC())
        else:
            f.write(u'// decompile failed\n')
        n += 1
print('exported %d functions' % n)
