//============================================================================
//  hooktest.exe —— 独立验证 SwapBuffers 钩子链路 (带逐步打点文件日志)
//============================================================================
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <string.h>

static HANDLE g_log = NULL;
static void LOG(const char* s)
{
    if (!g_log) {
        g_log = CreateFileA("verify\\hooktest_trace.log", GENERIC_WRITE, FILE_SHARE_READ,
                            NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    }
    if (g_log != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        WriteFile(g_log, s, (DWORD)strlen(s), &w, NULL);
        WriteFile(g_log, "\r\n", 2, &w, NULL);
        FlushFileBuffers(g_log);
    }
}

static bool FitsRel32(void* from, void* to)
{
    long long d = (long long)((unsigned char*)to - (unsigned char*)from);
    return d >= -0x80000000LL && d <= 0x7FFFFFFFLL;
}
static bool WriteRelJmp(unsigned char* dst, void* to)
{
    if (!FitsRel32(dst, to)) return false;
    dst[0] = 0xE9;
    int off = (int)((unsigned char*)to - (dst + 5));
    memcpy(dst + 1, &off, 4);
    return true;
}
static void WriteAbsJmp(unsigned char* dst, void* to)
{
    dst[0] = 0x48; dst[1] = 0xB8;
    memcpy(dst + 2, &to, 8);
    dst[10] = 0xFF; dst[11] = 0xE0;
}
static int ModRmLen(const unsigned char* p, int i)
{
    unsigned char modrm = p[i];
    int mod = modrm >> 6, rm = modrm & 7;
    int len = 1;
    if (mod == 0 && rm == 5) len += 4;
    else if (mod == 1) len += 1;
    else if (mod == 2) len += 4;
    else if (mod == 0 && rm == 4) {
        len += 1;
        if ((p[i + 1] & 7) == 5) len += 4;
    }
    return len;
}
static int InsnLen64(const unsigned char* p)
{
    int i = 0;
    bool rexW = false;
    while (i < 15) {
        unsigned char b = p[i];
        if (b >= 0x40 && b <= 0x4F) { rexW = (b & 8) != 0; i++; continue; }
        if (b == 0x66 || b == 0x67 || b == 0xF0 || b == 0xF2 || b == 0xF3) { i++; continue; }
        break;
    }
    if (i >= 15) return 0;
    unsigned char op = p[i];
    if (op == 0x0F) {
        unsigned char op2 = p[i + 1];
        if (op2 >= 0x80 && op2 <= 0x8F) return i + 6;
        if (op2 == 0x1E || op2 == 0x1F) return i + 2 + ModRmLen(p, i + 2);
        if (op2 == 0x05 || op2 == 0x34) return i + 2;
        return 0;
    }
    if (op >= 0x50 && op <= 0x5F) return i + 1;
    if (op >= 0x70 && op <= 0x7F) return i + 2;
    if (op == 0xEB) return i + 2;
    if (op == 0xE9) return i + 5;
    if (op == 0xE8) return i + 5;
    if (op >= 0xB8 && op <= 0xBF) return i + 1 + (rexW ? 8 : 4);
    if (op == 0x68) return i + 5;
    if (op == 0x6A) return i + 2;
    if (op == 0x80) return i + 2 + ModRmLen(p, i + 1) + 1;
    if (op == 0x81) return i + 2 + ModRmLen(p, i + 1) + 4;
    if (op == 0x83) return i + 2 + ModRmLen(p, i + 1) + 1;
    if (op == 0xC7) return i + 2 + ModRmLen(p, i + 1) + 4;
    if (op == 0x89 || op == 0x8B || op == 0x8D || op == 0x03 || op == 0x0B ||
        op == 0x2B || op == 0x33 || op == 0x3B || op == 0x01 || op == 0x09 ||
        op == 0x85 || op == 0x39 || op == 0x31 || op == 0x29 || op == 0x23 ||
        op == 0x63 || op == 0x8F || op == 0x21 || op == 0x87 || op == 0x86)
        return i + 1 + ModRmLen(p, i + 1);
    if (op == 0xFF) return i + 1 + ModRmLen(p, i + 1);
    if (op == 0xC3) return i + 1;
    if (op == 0xC2) return i + 3;
    if (op == 0xCC) return i + 1;
    if (op == 0x90) return i + 1;
    return 0;
}

static unsigned char* ResolveRealEntry(unsigned char* entry)
{
    for (int hop = 0; hop < 8 && entry; ++hop) {
        if (entry[0] == 0xFF && entry[1] == 0x25) {
            int disp;
            memcpy(&disp, entry + 2, 4);
            unsigned char** slot = (unsigned char**)(entry + 6 + disp);
            entry = *slot;
            continue;
        }
        if (entry[0] == 0xE9) {
            int disp;
            memcpy(&disp, entry + 1, 4);
            entry = entry + 5 + disp;
            continue;
        }
        return entry;
    }
    return NULL;
}

static DWORD ModuleSizeOf(HMODULE m)
{
    unsigned char* base = (unsigned char*)m;
    if (!base) return 0;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    return nt->OptionalHeader.SizeOfImage;
}

typedef BOOL (WINAPI* SwapBuffersFn)(HDC);
static SwapBuffersFn g_orig = NULL;
static volatile LONG g_hits = 0;

static BOOL WINAPI MyHook(HDC hdc)
{
    InterlockedIncrement(&g_hits);
    return g_orig(hdc);
}

int main()
{
    char line[256];
    setvbuf(stdout, NULL, _IONBF, 0);

    LOG("A:start");
    HMODULE gdi = GetModuleHandleA("gdi32.dll");
    if (!gdi) gdi = LoadLibraryA("gdi32.dll");
    snprintf(line, sizeof(line), "B:gdi=%p", gdi); LOG(line);

    unsigned char* entry = (unsigned char*)(void*)GetProcAddress(gdi, "SwapBuffers");
    snprintf(line, sizeof(line), "C:stub=%p", entry); LOG(line);
    unsigned char* t = ResolveRealEntry(entry);
    snprintf(line, sizeof(line), "D:real=%p", t); LOG(line);
    if (!t) { LOG("E:chase-fail"); return 1; }
    snprintf(line, sizeof(line), "E:prologue %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
             t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7], t[8], t[9], t[10], t[11]);
    LOG(line);

    // 路径 A: lea reg,[rip+slot]; jmp rel32 -> 解析槽位后原子替换指针
    if (t[1] == 0x8D && (t[2] & 0xC7) == 0x05 && t[7] == 0xE9) {
        LOG("F:path-A detected");
        int disp;
        memcpy(&disp, t + 3, 4);
        unsigned char** slot = (unsigned char**)(t + 7 + disp);
        void* val = *slot;
        unsigned char* gdiBase = (unsigned char*)gdi;
        DWORD gdiSize = ModuleSizeOf(gdi);
        snprintf(line, sizeof(line), "G:slot=%p val=%p inGdi=%d", slot, val,
                 (unsigned char*)val >= gdiBase && (unsigned char*)val < gdiBase + gdiSize);
        LOG(line);
        if ((unsigned char*)val >= gdiBase && (unsigned char*)val < gdiBase + gdiSize) {
            HMODULE full = GetModuleHandleA("gdi32full.dll");
            if (!full) { LOG("H:gdi32full-missing"); return 1; }
            val = (void*)GetProcAddress(full, "SwapBuffers");
            snprintf(line, sizeof(line), "H:fallback gdi32full!SwapBuffers=%p", val); LOG(line);
        }
        void* real = (void*)ResolveRealEntry((unsigned char*)val);
        snprintf(line, sizeof(line), "I:real=%p", real); LOG(line);
        if (!real) { LOG("I:real-null"); return 1; }
        DWORD old = 0;
        if (!VirtualProtect(slot, 8, PAGE_READWRITE, &old)) { LOG("J:vprotect-fail"); return 1; }
        *slot = (unsigned char*)(void*)&MyHook;
        VirtualProtect(slot, 8, old, &old);
        g_orig = (SwapBuffersFn)real;
        LOG("J:slot-patched, calling");
        BOOL r = ((SwapBuffersFn)entry)(NULL);
        snprintf(line, sizeof(line), "K:call1=%d hits=%ld", r, (long)g_hits); LOG(line);
        r = ((SwapBuffersFn)entry)(NULL);
        snprintf(line, sizeof(line), "L:call2=%d hits=%ld", r, (long)g_hits); LOG(line);
        LOG((g_hits == 2) ? "M:PASS path-A" : "M:FAIL hits");
        return (g_hits == 2) ? 0 : 1;
    }

    // 路径 B: 内联钩子
    LOG("F:path-B");
    bool absNeeded = !FitsRel32(t, (void*)&MyHook);
    int need = absNeeded ? 12 : 5;
    int total = 0;
    while (total < need) {
        int l = InsnLen64(t + total);
        if (l <= 0) { LOG("G:decode-fail"); return 1; }
        total += l;
    }
    unsigned char* tramp = NULL;
    long long base = (long long)t;
    for (int k = 0; k < 64 && !tramp; ++k) {
        long long addr = base - 0x40000000LL + (long long)k * 0x08000000LL;
        unsigned char* m = (unsigned char*)VirtualAlloc((void*)addr, 64,
            MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (m && FitsRel32(m + total, t + total)) tramp = m;
        else if (m) VirtualFree(m, 0, MEM_RELEASE);
    }
    if (!tramp) { LOG("H:tramp-fail"); return 1; }
    memcpy(tramp, t, total);
    if (!WriteRelJmp(tramp + total, t + total)) { LOG("I:trampjmp-fail"); return 1; }
    g_orig = (SwapBuffersFn)tramp;

    HANDLE hs[1024]; int nh = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te; te.dwSize = sizeof(te);
        DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
        if (Thread32First(snap, &te)) {
            do {
                if (te.th32OwnerProcessID == pid && te.th32ThreadID != self && nh < 1024) {
                    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
                    if (h) { SuspendThread(h); hs[nh++] = h; }
                }
            } while (Thread32Next(snap, &te));
        }
    }
    DWORD old = 0;
    VirtualProtect(t, total, PAGE_EXECUTE_READWRITE, &old);
    if (!WriteRelJmp(t, (void*)&MyHook)) WriteAbsJmp(t, (void*)&MyHook);
    VirtualProtect(t, total, old, &old);
    FlushInstructionCache(GetCurrentProcess(), t, total);
    FlushInstructionCache(GetCurrentProcess(), tramp, total + 5);
    for (int i = 0; i < nh; ++i) { ResumeThread(hs[i]); CloseHandle(hs[i]); }
    if (snap != INVALID_HANDLE_VALUE) CloseHandle(snap);

    LOG("J:patched, calling");
    BOOL r = ((SwapBuffersFn)t)(NULL);
    snprintf(line, sizeof(line), "K:call1=%d hits=%ld", r, (long)g_hits); LOG(line);
    r = ((SwapBuffersFn)t)(NULL);
    snprintf(line, sizeof(line), "L:call2=%d hits=%ld", r, (long)g_hits); LOG(line);
    LOG((g_hits == 2) ? "M:PASS path-B" : "M:FAIL hits");
    return (g_hits == 2) ? 0 : 1;
}
