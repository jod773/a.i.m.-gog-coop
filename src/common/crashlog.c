// Crash logging: a first-chance exception handler that writes the faulting
// address (as an address of the unrelocated executable, or module+offset),
// the registers and the code addresses found on the stack to the mod's log.
// The game catches access violations in its own catch(...) blocks and then
// quits with a generic message, so this runs before those.
#include "util.h"
#include <stdio.h>
#include <string.h>

extern IMAGE_DOS_HEADER __ImageBase;
static int g_count;

static void describe(uintptr_t a, char *out, int n)
{
    HMODULE m;
    char name[MAX_PATH];
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)a, &m)) {
        if (m == GetModuleHandleA(NULL)) { snprintf(out, n, "%08lx", (unsigned long)(a - g_exe_delta)); return; }
        if (m == (HMODULE)&__ImageBase) { snprintf(out, n, "mod+%lx", (unsigned long)(a - (uintptr_t)m)); return; }
        GetModuleFileNameA(m, name, sizeof name);
        const char *b = strrchr(name, '\\');
        snprintf(out, n, "%s+%lx", b ? b + 1 : name, (unsigned long)(a - (uintptr_t)m));
        return;
    }
    snprintf(out, n, "%08lx", (unsigned long)a);
}

static int is_code(uintptr_t a)
{
    MEMORY_BASIC_INFORMATION mi;
    if (a < 0x10000 || !VirtualQuery((void *)a, &mi, sizeof mi) || mi.State != MEM_COMMIT) return 0;
    return (mi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

static LONG CALLBACK handler(EXCEPTION_POINTERS *ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_PRIV_INSTRUCTION)
        return EXCEPTION_CONTINUE_SEARCH;
    if (g_count++ >= 8) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT *c = ep->ContextRecord;
    char where[MAX_PATH + 32];
    describe(c->Eip, where, sizeof where);
    ULONG_PTR *info = ep->ExceptionRecord->ExceptionInformation;
    mlog("CRASH: exception %08lx at %s (%s %08lx) thread %lu", (unsigned long)code, where,
         code == EXCEPTION_ACCESS_VIOLATION ? (info[0] ? "writing" : "reading") : "-",
         (unsigned long)(code == EXCEPTION_ACCESS_VIOLATION ? info[1] : 0), (unsigned long)GetCurrentThreadId());
    mlog("CRASH: eax=%08lx ebx=%08lx ecx=%08lx edx=%08lx esi=%08lx edi=%08lx ebp=%08lx esp=%08lx",
         (unsigned long)c->Eax, (unsigned long)c->Ebx, (unsigned long)c->Ecx, (unsigned long)c->Edx,
         (unsigned long)c->Esi, (unsigned long)c->Edi, (unsigned long)c->Ebp, (unsigned long)c->Esp);
    char buf[1400];
    int n = 0, k = 0;
    MEMORY_BASIC_INFORMATION mi;
    uintptr_t *p = (uintptr_t *)c->Esp;
    if (VirtualQuery(p, &mi, sizeof mi)) {
        uintptr_t *end = (uintptr_t *)((uint8_t *)mi.BaseAddress + mi.RegionSize);
        for (; p < end && k < 48 && (uint8_t *)p < (uint8_t *)c->Esp + 0x4000; p++) {
            if (!is_code(*p)) continue;
            char d[MAX_PATH + 32];
            describe(*p, d, sizeof d);
            n += snprintf(buf + n, sizeof buf - n, " %s", d);
            k++;
            if (n > (int)sizeof buf - 64) break;
        }
    }
    buf[n] = 0;
    mlog("CRASH: stack:%s", buf);
    return EXCEPTION_CONTINUE_SEARCH;
}

void crashlog_install(void)
{
    AddVectoredExceptionHandler(1, handler);
}

// After an error the games log in error.log, their main thread can end without ending the
// process: it stays behind without a window (and keeps the co-op port). If the main thread
// is gone and the process is still there a few seconds later, it is ended.
static DWORD WINAPI main_watch(void *h)
{
    WaitForSingleObject((HANDLE)h, INFINITE);
    Sleep(3000);
    mlog("the game's main thread has ended without closing the game: closing it");
    TerminateProcess(GetCurrentProcess(), 0);
    return 0;
}

void crashlog_watch_main_thread(void)       // call from the main thread
{
    HANDLE h = NULL;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h, SYNCHRONIZE, FALSE, 0)) return;
    HANDLE t = CreateThread(NULL, 0, main_watch, h, 0, NULL);
    if (t) CloseHandle(t);
}
