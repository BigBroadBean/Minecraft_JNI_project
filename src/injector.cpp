//============================================================================
//  injector.exe
//  把 MCCanAttackJni.dll 注入到 Minecraft 1.8.9 (java/javaw) 进程,
//  然后实时显示共享内存中的 "能否攻击" 状态。
//
//  用法:
//    injector.exe                      自动查找 Minecraft 窗口并注入
//    injector.exe -pid <PID>           注入到指定 PID
//    injector.exe -title <子串>        按窗口标题子串查找 (默认 "Minecraft")
//    injector.exe -once                注入后打印一次状态即退出 (不循环)
//
//  注意: injector.exe 与 DLL 位数必须和游戏 Java 一致 (默认 64 位)。
//============================================================================

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <string.h>

//--------------------------------------------------------------------------
// 与 MCCanAttackJni.dll 一致的状态结构
//--------------------------------------------------------------------------
#pragma pack(push, 1)
struct CanAttackStatus {
    DWORD        magic;
    DWORD        version;
    DWORD        pid;
    volatile LONG ready;
    volatile LONG inGame;
    volatile LONG canAttack;
    volatile LONG hitType;
    volatile LONG targetLiving;
    volatile LONG targetAlive;
    volatile LONG targetIsPlayer;
    char          targetName[128];
    char          mappingName[32];
    char          envName[48];
    char          loaderName[48];
    char          errMsg[96];
    char          failLog[160];
    volatile LONG mcNull;
    volatile LONG tick;
    volatile LONG lastError;
};
#pragma pack(pop)

static const char* kDllName   = "MCCanAttackJni.dll";
static const char* kMapFmt    = "Local\\MCCanAttackStatus_%lu";
static const DWORD kMagic     = 0x4D43414B;

static const char* g_titleSub = "Minecraft";
static char        g_dllPath[MAX_PATH] = ""; // 非空时使用指定 DLL 路径

//--------------------------------------------------------------------------
// 进程名判断 (不区分大小写)
//--------------------------------------------------------------------------
static bool NameIs(const char* name, const char* target)
{
    return _stricmp(name, target) == 0;
}

static bool IsJavaProcess(DWORD pid)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;

    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(pe);
    bool isJava = false;
    if (Process32First(snap, &pe)) {
        do {
            if (pe.th32ProcessID == pid &&
                (NameIs(pe.szExeFile, "java.exe") || NameIs(pe.szExeFile, "javaw.exe"))) {
                isJava = true;
                break;
            }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return isJava;
}

//--------------------------------------------------------------------------
// 按窗口标题查找 Minecraft 进程
//--------------------------------------------------------------------------
struct FindCtx { const char* sub; DWORD pid; };

static BOOL CALLBACK EnumWinProc(HWND hwnd, LPARAM lp)
{
    FindCtx* ctx = (FindCtx*)lp;
    if (!IsWindowVisible(hwnd)) return TRUE;

    char title[256];
    if (GetWindowTextA(hwnd, title, sizeof(title)) == 0) return TRUE;
    if (!strstr(title, ctx->sub)) return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid && IsJavaProcess(pid)) {
        ctx->pid = pid;
        return FALSE; // 找到
    }
    return TRUE;
}

static DWORD FindMinecraftPid(void)
{
    FindCtx ctx = { g_titleSub, 0 };
    EnumWindows(EnumWinProc, (LPARAM)&ctx);
    return ctx.pid;
}

//--------------------------------------------------------------------------
// 注入 DLL (CreateRemoteThread + LoadLibraryA)
//--------------------------------------------------------------------------
static bool InjectDll(DWORD pid, const char* dllPath)
{
    HANDLE proc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!proc) {
        printf("[!] OpenProcess(%lu) 失败, 错误码 %lu (试试以管理员身份运行)\n",
               pid, GetLastError());
        return false;
    }

    // 位数检查
    BOOL isWow64 = FALSE;
    if (sizeof(void*) == 8 && IsWow64Process(proc, &isWow64) && isWow64) {
        printf("[!] 目标进程是 32 位, 而本工具是 64 位, 无法注入。\n");
        printf("    请使用 32 位版本 (gcc -m32 重新编译) 或改用 64 位 Java。\n");
        CloseHandle(proc);
        return false;
    }

    size_t len = strlen(dllPath) + 1;
    void* mem = VirtualAllocEx(proc, NULL, len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!mem) {
        printf("[!] VirtualAllocEx 失败, 错误码 %lu\n", GetLastError());
        CloseHandle(proc);
        return false;
    }
    if (!WriteProcessMemory(proc, mem, dllPath, len, NULL)) {
        printf("[!] WriteProcessMemory 失败, 错误码 %lu\n", GetLastError());
        VirtualFreeEx(proc, mem, 0, MEM_RELEASE);
        CloseHandle(proc);
        return false;
    }

    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    FARPROC loadLib = GetProcAddress(k32, "LoadLibraryA");
    HANDLE thread = CreateRemoteThread(proc, NULL, 0,
                                       (LPTHREAD_START_ROUTINE)loadLib, mem, 0, NULL);
    if (!thread) {
        printf("[!] CreateRemoteThread 失败, 错误码 %lu\n", GetLastError());
        VirtualFreeEx(proc, mem, 0, MEM_RELEASE);
        CloseHandle(proc);
        return false;
    }

    WaitForSingleObject(thread, 10000);
    CloseHandle(thread);
    VirtualFreeEx(proc, mem, 0, MEM_RELEASE);
    CloseHandle(proc);
    return true;
}

//--------------------------------------------------------------------------
// 打印一行状态
//--------------------------------------------------------------------------
static const char* HitName(long hit)
{
    switch (hit) {
        case 0:  return "MISS  ";
        case 1:  return "BLOCK ";
        case 2:  return "ENTITY";
        default: return "?     ";
    }
}

static void PrintStatus(const CanAttackStatus* s)
{
    printf("\r[%s] env=%-18s map=%-11s loader=%-30s tick=%-6lu inGame=%d mcNull=%d hit=%s "
           "canAttack=%d target=%-24s living=%d alive=%d err=%ld %s   ",
           s->ready ? "READY " : "WAIT  ",
           s->envName[0] ? s->envName : "(unknown)",
           s->mappingName[0] ? s->mappingName : "(none)",
           s->loaderName[0] ? s->loaderName : "(null)",
           (unsigned long)s->tick,
           (int)s->inGame,
           (int)s->mcNull,
           HitName(s->hitType),
           (int)s->canAttack,
           s->targetName[0] ? s->targetName : "(none)",
           (int)s->targetLiving,
           (int)s->targetAlive,
           (long)s->lastError,
           s->errMsg[0] ? s->errMsg : "");
    if (s->failLog[0]) {
        printf("\n      failLog: %s", s->failLog);
    }
}

//--------------------------------------------------------------------------
// 主流程
//--------------------------------------------------------------------------
int main(int argc, char** argv)
{
    SetConsoleOutputCP(CP_UTF8); // 中文输出

    DWORD pid = 0;
    bool  once = false;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-pid") == 0 && i + 1 < argc) {
            pid = (DWORD)strtoul(argv[++i], NULL, 10);
        }
        else if (strcmp(argv[i], "-title") == 0 && i + 1 < argc) {
            g_titleSub = argv[++i];
        }
        else if (strcmp(argv[i], "-dll") == 0 && i + 1 < argc) {
            snprintf(g_dllPath, sizeof(g_dllPath), "%s", argv[++i]);
        }
        else if (strcmp(argv[i], "-once") == 0) {
            once = true;
        }
        else {
            printf("用法: injector.exe [-pid <PID>] [-title <窗口标题子串>] [-dll <DLL路径>] [-once]\n");
            return 1;
        }
    }

    // 定位游戏进程
    if (pid == 0) {
        printf("[*] 正在查找窗口标题包含 \"%s\" 的 Java 进程...\n", g_titleSub);
        pid = FindMinecraftPid();
        if (pid == 0) {
            printf("[!] 未找到 Minecraft 进程。\n");
            printf("    请先启动游戏, 或手动指定: injector.exe -pid <PID>\n");
            printf("    可用命令查看: tasklist | findstr /i \"java\"\n");
            return 1;
        }
        printf("[*] 找到 Minecraft 进程, PID = %lu\n", pid);
    } else {
        if (!IsJavaProcess(pid)) {
            printf("[!] 警告: PID %lu 不是 java/javaw 进程, 仍将尝试注入。\n", pid);
        }
    }

    // 计算 DLL 路径 (与 injector.exe 同目录; 或 -dll 指定)
    char dllPath[MAX_PATH];
    if (g_dllPath[0]) {
        snprintf(dllPath, sizeof(dllPath), "%s", g_dllPath);
    } else {
        char exePath[MAX_PATH];
        DWORD got = GetModuleFileNameA(NULL, exePath, MAX_PATH);
        if (got == 0 || got >= MAX_PATH) {
            printf("[!] 无法获取自身路径\n");
            return 1;
        }
        char* slash = strrchr(exePath, '\\');
        if (slash) *(slash + 1) = 0;
        snprintf(dllPath, sizeof(dllPath), "%s%s", exePath, kDllName);
    }

    if (GetFileAttributesA(dllPath) == INVALID_FILE_ATTRIBUTES) {
        printf("[!] 找不到 %s (应与 injector.exe 在同一目录)\n", dllPath);
        return 1;
    }

    // 注入
    printf("[*] 注入 %s ...\n", dllPath);
    if (!InjectDll(pid, dllPath)) return 1;
    printf("[+] 注入完成。\n");

    // 等待共享内存出现 (DLL 工作线程创建)
    char mapName[64];
    snprintf(mapName, sizeof(mapName), kMapFmt, (unsigned long)pid);

    HANDLE map = NULL;
    for (int i = 0; i < 100; ++i) {
        map = OpenFileMappingA(FILE_MAP_READ, FALSE, mapName);
        if (map) break;
        Sleep(100);
    }
    if (!map) {
        printf("[!] 等待共享内存超时 (10 秒)。\n");
        return 1;
    }

    const CanAttackStatus* s = (const CanAttackStatus*)MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0);
    if (!s) {
        printf("[!] MapViewOfFile 失败\n");
        CloseHandle(map);
        return 1;
    }

    printf("[*] 已连接共享内存, 开始监视 (Ctrl+C 退出)...\n\n");

    if (once) {
        // 等待 JNI 就绪后打印一次状态 (最多等 10 秒)
        for (int i = 0; i < 100; ++i) {
            if (s->ready && s->tick > 0) break;
            Sleep(100);
        }
        PrintStatus(s);
        printf("\n");
    } else {
        while (true) {
            PrintStatus(s);
            fflush(stdout);
            Sleep(100);
        }
    }

    UnmapViewOfFile(s);
    CloseHandle(map);
    return 0;
}
