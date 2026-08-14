//============================================================================
//  injector.exe  (V66: 手动映射注入, 无 LoadLibrary / 无模块链表条目)
//
//  把 MCCombatStatusJni.dll (XOR 加密后嵌入本 exe, 见 src/payload.h) 手动
//  映射进 Minecraft (java/javaw) 进程:
//    * 不调用 LoadLibrary -> 无 LoadImage 回调、无 PEB 模块条目、不落盘
//    * 远程 VirtualAllocEx(RW) -> 拷贝节区 -> 重定位 -> 本地解析导入地址
//      (系统 DLL 基址在系统范围内一致) -> 写入 IAT -> 入口存根线程执行
//      DllMain(base, DLL_PROCESS_ATTACH) -> 收尾改 RX
//    * OpenProcess 仅最小权限 (CREATE_THREAD|QUERY_INFO|VM_*)
//    * 反重复注入: 检查共享内存 Local\MCCombatStatus_<pid> 是否健康
//
//  用法:
//    injector.exe                       自动查找 Minecraft 窗口并注入 (嵌入载荷)
//    injector.exe -pid <PID>            手动指定进程
//    injector.exe -title <子串>         按窗口标题子串查找 (默认 "Minecraft")
//    injector.exe -dll <路径>           用指定明文 DLL 文件替代嵌入载荷
//============================================================================

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <string.h>
#include <vector>
#include <algorithm>

#include "payload.h"   // kPayload[] / kPayloadLen (XOR 0x5A 加密, 由 build.bat 生成)

static const char* g_titleSub = "Minecraft";
static char        g_dllPath[MAX_PATH] = ""; // 非空时使用指定 DLL 文件
static bool        g_localDebug = false;

// 临时调试: 记录崩溃地址
static LONG WINAPI VectoredHandler(PEXCEPTION_POINTERS ep)
{
    MEMORY_BASIC_INFORMATION mbi = {};
    VirtualQuery(ep->ExceptionRecord->ExceptionAddress, &mbi, sizeof(mbi));
    fprintf(stderr, "[!!] exception code=%08X at %p (page=%p prot=0x%lX)\n",
            ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress,
            mbi.BaseAddress, mbi.Protect);
    if (ep->ExceptionRecord->NumberParameters >= 2)
        fprintf(stderr, "[!!] numParam=%d info0=%llx faultAddr=%p rip=%p rax=%p rcx=%p\n",
                ep->ExceptionRecord->NumberParameters,
                ep->ExceptionRecord->ExceptionInformation[0],
                (void*)ep->ExceptionRecord->ExceptionInformation[1],
                (void*)ep->ContextRecord->Rip, (void*)ep->ContextRecord->Rax,
                (void*)ep->ContextRecord->Rcx);
    fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

//--------------------------------------------------------------------------
// 解密载荷 (XOR 0x5A); 失败时回退 -dll 文件读取
//--------------------------------------------------------------------------
static BYTE* LoadPayload(size_t* outLen)
{
    *outLen = 0;
    if (g_dllPath[0]) {
        HANDLE f = CreateFileA(g_dllPath, GENERIC_READ, FILE_SHARE_READ,
                               NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f == INVALID_HANDLE_VALUE) {
            printf("[!] 无法打开 %s\n", g_dllPath);
            return NULL;
        }
        DWORD sz = GetFileSize(f, NULL);
        BYTE* buf = (BYTE*)VirtualAlloc(NULL, sz, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!buf) { CloseHandle(f); return NULL; }
        DWORD rd = 0;
        ReadFile(f, buf, sz, &rd, NULL);
        CloseHandle(f);
        *outLen = sz;
        return buf;   // -dll 路径按明文处理
    }
    size_t n = kPayloadLen;
    BYTE* buf = (BYTE*)VirtualAlloc(NULL, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!buf) return NULL;
    for (size_t i = 0; i < n; i++) buf[i] = (BYTE)(kPayload[i] ^ 0x5A);
    *outLen = n;
    return buf;
}

//--------------------------------------------------------------------------
// 进程名判断 / 窗口搜索 (与旧版一致)
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
        return FALSE;
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
// 反重复注入: 共享内存 Local\MCCombatStatus_<pid> 存在
//--------------------------------------------------------------------------
static bool AlreadyInjected(DWORD pid)
{
    char name[64];
    snprintf(name, sizeof(name), "Local\\MCCombatStatus_%lu", pid);
    fflush(stderr);
    HANDLE m = OpenFileMappingA(FILE_MAP_READ, FALSE, name);
    fflush(stderr);
    if (!m) return false;
    CloseHandle(m);
    return true;
}

//--------------------------------------------------------------------------
// MinGW 伪重定位 (pseudo-reloc) 应用。
// 列表位置通过 COFF 符号表里的 ___RUNTIME_PSEUDO_RELOC_LIST__ /
// ___RUNTIME_PSEUDO_RELOC_LIST_END__ 符号定位 (链接时 --retain-symbols-file
// 仅保留这两个符号; 符号表在文件中、不进入运行时映像, 无隐蔽性损失)。
// v2 条目 (24 字节): {DWORD_PTR sym, target, addend}, 应用语义 (与
// mingw-w64 运行时一致): *(base+target) = base + sym + addend
//--------------------------------------------------------------------------
struct CoffSym {
    char   name[8];     // 或 {4 字节 0 + 4 字节字符串表偏移}
    DWORD  value;
    SHORT  section;
    WORD   type;
    BYTE   sclass;
    BYTE   naux;
};

static const char* CoffName(const CoffSym* s, const char* strtab)
{
    // 前 4 字节全 0 = 字符串表引用 (偏移在 4..7)
    if (s->name[0] == 0 && s->name[1] == 0 && s->name[2] == 0 && s->name[3] == 0) {
        DWORD off = *(DWORD*)(s->name + 4);
        return strtab + off;
    }
    // 短名: 8 字节内可能无终止符, 复制到静态缓冲
    static char buf[9];
    memcpy(buf, s->name, 8);
    buf[8] = 0;
    return buf;
}

static bool ApplyPseudoRelocs(HANDLE proc, BYTE* base, const BYTE* workImg, const BYTE* imgFile, size_t imgSize)
{
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)imgFile;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return true;   // 无符号表 -> 无伪重定位可应用
    IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(imgFile + dos->e_lfanew);
    DWORD symOff = nt->FileHeader.PointerToSymbolTable;
    DWORD symNum = nt->FileHeader.NumberOfSymbols;
    if (!symOff || !symNum) return true;
    if (symOff + (size_t)symNum * 18 + 4 > imgSize) return true;
    const CoffSym* syms = (const CoffSym*)(imgFile + symOff);
    const char* strtab = (const char*)(imgFile + symOff + (size_t)symNum * 18);

    DWORD listRva = 0, endRva = 0;
    for (DWORD i = 0; i < symNum; i++) {
        const CoffSym* s = &syms[i];
        const char* nm = CoffName(s, strtab);
        if (!strcmp(nm, "___RUNTIME_PSEUDO_RELOC_LIST__"))
            listRva = s->value;
        else if (!strcmp(nm, "___RUNTIME_PSEUDO_RELOC_LIST_END__"))
            endRva = s->value;
        i += s->naux;
    }
    if (!listRva || !endRva || endRva <= listRva) return true;   // 无条目

    // v2: 24 字节条目 (从 RVA 布局的 workImg 读, 条目值是 RVA)
    for (DWORD rva = listRva; rva + 24 <= endRva; rva += 24) {
        DWORD_PTR sym, target, addend;
        memcpy(&sym,    workImg + rva,      8);
        memcpy(&target, workImg + rva + 8,  8);
        memcpy(&addend, workImg + rva + 16, 8);
        DWORD_PTR val = (DWORD_PTR)base + sym + addend;
        if (!WriteProcessMemory(proc, base + target, &val, 8, NULL))
            return false;
    }
    return true;
}

//--------------------------------------------------------------------------
// 手动映射 (反射加载)
//--------------------------------------------------------------------------
static bool ManualMap(HANDLE proc, const BYTE* img, size_t imgSize)
{
    const BYTE* imgFile = img;   // 原始文件字节 (COFF 符号表等仅在文件中)
    // 变量统一前置声明 (goto fail 不得跨越初始化)
    BYTE* base = NULL;
    IMAGE_SECTION_HEADER* sec = NULL;
    ULONGLONG delta = 0;
    ULONGLONG entry = 0;
    DWORD sn = 0;
    void* stubAddr = NULL;
    HANDLE th = NULL;
    DWORD exitCode = 0;

    if (imgSize < sizeof(IMAGE_DOS_HEADER)) return false;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)img;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(img + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    DWORD sizeOfImage = nt->OptionalHeader.SizeOfImage;
    DWORD sizeOfHeaders = nt->OptionalHeader.SizeOfHeaders;
    ULONGLONG prefBase = nt->OptionalHeader.ImageBase;

    // 关键: 本地解析缓冲必须按 RVA 布局 (节区文件偏移 ≠ RVA),
    // 否则 .reloc/.idata 解析会读到错误内容。按节区表逐节拷贝, 其余零填充。
    BYTE* work = (BYTE*)VirtualAlloc(NULL, sizeOfImage, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!work) return false;
    memset(work, 0, sizeOfImage);
    {
        size_t hdrN = imgSize < sizeOfHeaders ? imgSize : sizeOfHeaders;
        memcpy(work, img, hdrN);
        IMAGE_SECTION_HEADER* secW = IMAGE_FIRST_SECTION(nt);
        for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
            if (secW[i].SizeOfRawData && secW[i].VirtualAddress < sizeOfImage &&
                secW[i].PointerToRawData < imgSize) {
                size_t n = secW[i].SizeOfRawData;
                if (secW[i].PointerToRawData + n > imgSize) n = imgSize - secW[i].PointerToRawData;
                if (secW[i].VirtualAddress + n > sizeOfImage) n = sizeOfImage - secW[i].VirtualAddress;
                memcpy(work + secW[i].VirtualAddress, img + secW[i].PointerToRawData, n);
            }
        }
    }
    img = work;
    dos = (IMAGE_DOS_HEADER*)img;
    nt  = (IMAGE_NT_HEADERS64*)(img + dos->e_lfanew);

    // 1. 远程分配 RW
    base = (BYTE*)VirtualAllocEx(proc, NULL, sizeOfImage,
                                 MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!base) { printf("[!] VirtualAllocEx 失败 (%lu)\n", GetLastError()); return false; }

    // 2. 拷贝头 + 节区 (从文件缓冲按文件偏移读; work 是 RVA 布局不可用于此)
    if (!WriteProcessMemory(proc, base, imgFile, sizeOfHeaders, NULL)) goto fail;
    sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (sec[i].SizeOfRawData) {
            if (!WriteProcessMemory(proc, base + sec[i].VirtualAddress,
                                    imgFile + sec[i].PointerToRawData,
                                    sec[i].SizeOfRawData, NULL)) goto fail;
        }
    }

    // 3. 重定位
    delta = (ULONGLONG)base - prefBase;
    if (delta) {
        IMAGE_DATA_DIRECTORY& reloc = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
        DWORD off = 0;
        while (off + sizeof(IMAGE_BASE_RELOCATION) <= reloc.Size) {
            DWORD blkFile = reloc.VirtualAddress + off;
            if (blkFile + sizeof(IMAGE_BASE_RELOCATION) > sizeOfImage) break;
            IMAGE_BASE_RELOCATION* blk = (IMAGE_BASE_RELOCATION*)(img + blkFile);
            if (blk->SizeOfBlock < sizeof(IMAGE_BASE_RELOCATION)) {
                break;
            }
            if ((ULONGLONG)reloc.VirtualAddress + off + blk->SizeOfBlock > sizeOfImage) {
                break;
            }
            DWORD count = (blk->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / 2;
            WORD* items = (WORD*)((BYTE*)blk + sizeof(IMAGE_BASE_RELOCATION));
            for (DWORD i = 0; i < count; i++) {
                if ((items[i] >> 12) == IMAGE_REL_BASED_DIR64) {
                    DWORD rva = blk->VirtualAddress + (items[i] & 0xFFF);
                    if (rva + 8 > sizeOfImage) continue;
                    ULONGLONG* slot = (ULONGLONG*)(img + rva);
                    ULONGLONG val = *slot + delta;
                    if (!WriteProcessMemory(proc, base + rva, &val, 8, NULL)) goto fail;
                }
            }
            off += blk->SizeOfBlock;
        }
    }

    // 3b. MinGW 对 .rdata/.data 中少数绝对指针不生成标准 reloc 条目
    // (期望装载于首选基址)。手动映射基址不同, 需补修: 扫描数据节中
    // 位于 [prefBase, prefBase+SizeOfImage) 的 8 字节值, 且槽位未被
    // 标准 reloc 覆盖 -> 加 delta。误报概率极低 (值域仅 155KB/2^64)。
    {
        std::vector<DWORD> covered;
        covered.reserve(2048);
        {
            IMAGE_DATA_DIRECTORY& reloc2 = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
            DWORD off2 = 0;
            while (off2 + sizeof(IMAGE_BASE_RELOCATION) <= reloc2.Size) {
                IMAGE_BASE_RELOCATION* blk = (IMAGE_BASE_RELOCATION*)(img + reloc2.VirtualAddress + off2);
                if (!blk->SizeOfBlock || blk->SizeOfBlock < sizeof(IMAGE_BASE_RELOCATION)) break;
                DWORD count = (blk->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / 2;
                WORD* items = (WORD*)((BYTE*)blk + sizeof(IMAGE_BASE_RELOCATION));
                for (DWORD i = 0; i < count; i++)
                    if ((items[i] >> 12) == IMAGE_REL_BASED_DIR64)
                        covered.push_back(blk->VirtualAddress + (items[i] & 0xFFF));
                off2 += blk->SizeOfBlock;
            }
        }
        std::sort(covered.begin(), covered.end());
        for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
            DWORD ch = sec[i].Characteristics;
            if (!(ch & IMAGE_SCN_MEM_READ) || (ch & IMAGE_SCN_CNT_CODE)) continue;
            DWORD va = sec[i].VirtualAddress;
            DWORD vs = sec[i].Misc.VirtualSize ? sec[i].Misc.VirtualSize : sec[i].SizeOfRawData;
            if (va + vs > sizeOfImage) vs = sizeOfImage - va;
            for (DWORD o = 0; o + 8 <= vs; o++) {
                if (std::binary_search(covered.begin(), covered.end(), va + o)) continue;
                ULONGLONG v = *(ULONGLONG*)(img + va + o);
                if (v >= prefBase && v < prefBase + sizeOfImage) {
                    ULONGLONG nv = v + delta;
                    WriteProcessMemory(proc, base + va + o, &nv, 8, NULL);
                }
            }
        }
    }

    // 4. 导入解析: 系统 DLL (KERNEL32/msvcrt/WS2_32) 基址在系统范围内一致,
    //    且目标进程 (javaw) 必然已加载 -> 本地 GetProcAddress 直接填 IAT
    {
        IMAGE_DATA_DIRECTORY& imp = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        DWORD off = 0;
        while (off + sizeof(IMAGE_IMPORT_DESCRIPTOR) <= imp.Size) {
            IMAGE_IMPORT_DESCRIPTOR* desc = (IMAGE_IMPORT_DESCRIPTOR*)(img + imp.VirtualAddress + off);
            if (!desc->Name && !desc->FirstThunk) break;
            const char* dllName = (const char*)(img + desc->Name);
            HMODULE mod = GetModuleHandleA(dllName);
            if (!mod) mod = LoadLibraryA(dllName);   // 注入器侧加载 (系统 DLL 基址全局一致)
            if (!mod) {
                printf("[!] 目标依赖模块未加载: %s\n", dllName);
                goto fail;
            }
            ULONGLONG* origThunk = (ULONGLONG*)(img + (desc->OriginalFirstThunk
                                                        ? desc->OriginalFirstThunk : desc->FirstThunk));
            ULONGLONG* iat = (ULONGLONG*)(img + desc->FirstThunk);
            for (int i = 0; origThunk[i]; i++) {
                ULONGLONG val = 0;
                if (origThunk[i] & 0x8000000000000000ULL) {
                    val = (ULONGLONG)(ULONG_PTR)GetProcAddress(mod, (LPCSTR)(origThunk[i] & 0xFFFF));
                } else {
                    IMAGE_IMPORT_BY_NAME* ibn = (IMAGE_IMPORT_BY_NAME*)(img + origThunk[i]);
                    val = (ULONGLONG)(ULONG_PTR)GetProcAddress(mod, ibn->Name);
                }
                if (!val) {
                    printf("[!] 导入解析失败: %s\n", dllName);
                    goto fail;
                }
                if (!WriteProcessMemory(proc, base + desc->FirstThunk + i * 8, &val, 8, NULL)) goto fail;
            }
            off += sizeof(IMAGE_IMPORT_DESCRIPTOR);
        }
    }

    // 4.5 伪重定位 (MinGW .refptr 等绝对引用; 符号表来自文件, 条目来自 RVA 布局)
    if (!ApplyPseudoRelocs(proc, base, img, imgFile, imgSize)) goto fail;

    // 5. 入口存根: mov rcx,base; mov edx,1; xor r8,r8; mov rax,entry; jmp rax
    entry = (ULONGLONG)base + nt->OptionalHeader.AddressOfEntryPoint;
    {
        BYTE stub[64];
        sn = 0;
        stub[sn++] = 0x48; stub[sn++] = 0xB9;
        memcpy(stub + sn, &base, 8);  sn += 8;      // mov rcx, base
        stub[sn++] = 0xBA; stub[sn++] = 0x01; stub[sn++] = 0x00;
        stub[sn++] = 0x00; stub[sn++] = 0x00;       // mov edx, 1 (DLL_PROCESS_ATTACH)
        stub[sn++] = 0x4D; stub[sn++] = 0x31; stub[sn++] = 0xC0;   // xor r8, r8
        stub[sn++] = 0x48; stub[sn++] = 0xB8;
        memcpy(stub + sn, &entry, 8); sn += 8;      // mov rax, entry
        stub[sn++] = 0xFF; stub[sn++] = 0xE0;       // jmp rax

        stubAddr = VirtualAllocEx(proc, NULL, sizeof(stub),
                                  MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!stubAddr) goto fail;
        if (!WriteProcessMemory(proc, stubAddr, stub, sn, NULL)) goto fail;
        FlushInstructionCache(proc, stubAddr, sn);
    }
    FlushInstructionCache(proc, base, sizeOfImage);

    // 5.5 按节区属性设置保护 (代码 RX / 数据 RW), 否则 NX 会杀掉执行线程
    {
        sec = IMAGE_FIRST_SECTION(nt);
        for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
            DWORD ch = sec[i].Characteristics;
            DWORD prot = PAGE_READONLY;
            if (ch & IMAGE_SCN_MEM_EXECUTE) prot = PAGE_EXECUTE_READ;
            if (ch & IMAGE_SCN_MEM_WRITE)   prot = (ch & IMAGE_SCN_MEM_EXECUTE)
                                                   ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
            DWORD old = 0;
            VirtualProtectEx(proc, base + sec[i].VirtualAddress,
                             sec[i].Misc.VirtualSize ? sec[i].Misc.VirtualSize : 1,
                             prot, &old);
        }
    }

    // 6. 执行 DllMain (新线程; 入口点非 LoadLibrary, 无模块加载)
    if (g_localDebug) {
        // -local 调试: 映射进自身进程 (CreateRemoteThread 不支持本进程)
        th = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)stubAddr, NULL, 0, NULL);
    } else {
        th = CreateRemoteThread(proc, NULL, 0,
                                (LPTHREAD_START_ROUTINE)stubAddr, NULL, 0, NULL);
    }
    if (!th) { printf("[!] CreateRemoteThread 失败 (%lu)\n", GetLastError()); goto fail; }
    WaitForSingleObject(th, 10000);
    GetExitCodeThread(th, &exitCode);
    CloseHandle(th);
    th = NULL;
    printf("[*] DllMain 返回 %lu\n", exitCode);

    // 7. 收尾: 保护已在执行前按节区设好 (DllMain 内自行抹除 PE 头)
    VirtualFreeEx(proc, stubAddr, 0, MEM_RELEASE);
    VirtualFree(work, 0, MEM_RELEASE);
    return true;

fail:
    printf("[!] 手动映射失败 (%lu)\n", GetLastError());
    if (th) CloseHandle(th);
    if (stubAddr) VirtualFreeEx(proc, stubAddr, 0, MEM_RELEASE);
    if (base) VirtualFreeEx(proc, base, 0, MEM_RELEASE);
    VirtualFree(work, 0, MEM_RELEASE);
    return false;
}

//--------------------------------------------------------------------------
// 主流程
//--------------------------------------------------------------------------
int main(int argc, char** argv)
{
    fflush(stderr);
    AddVectoredExceptionHandler(1, VectoredHandler);

    DWORD pid = (argc > 2) ? (DWORD)strtoul(argv[2], NULL, 10) : 0;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-local") == 0) g_localDebug = true;
        else if (strcmp(argv[i], "-dll") == 0 && i + 1 < argc)
            snprintf(g_dllPath, sizeof(g_dllPath), "%s", argv[++i]);
        else if (strcmp(argv[i], "-pid") == 0 && i + 1 < argc)
            pid = (DWORD)strtoul(argv[++i], NULL, 10);
    }

    // 定位游戏进程 (-local 调试模式跳过)
    if (pid == 0 && !g_localDebug) {
        printf("[*] 正在查找窗口标题包含 \"%s\" 的 Java 进程...\n", g_titleSub);
        pid = FindMinecraftPid();
        if (pid == 0) {
            printf("[!] 未找到 Minecraft 进程。\n");
            printf("    请先启动游戏, 或手动指定: injector.exe -pid <PID>\n");
            return 1;
        }
        printf("[*] 找到 Minecraft 进程, PID = %lu\n", pid);
    } else {
        if (!IsJavaProcess(pid)) {
            printf("[!] 警告: PID %lu 不是 java/javaw 进程, 仍将尝试注入。\n", pid);
        }
    }

    fflush(stderr);
    if (AlreadyInjected(pid)) {
        printf("[*] 已注入 (共享内存存在), 跳过。\n");
        return 0;
    }

    // 载荷 (嵌入解密 / -dll 明文)
    size_t imgSize = 0;
    BYTE* img = LoadPayload(&imgSize);
    if (!img) {
        printf("[!] 载荷加载失败\n");
        return 1;
    }

    HANDLE proc;
    if (g_localDebug) {
        proc = GetCurrentProcess();
    } else {
        // 最小权限句柄
        proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                           PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                           FALSE, pid);
        if (!proc) {
            printf("[!] OpenProcess(%lu) 失败, 错误码 %lu (试试以管理员身份运行)\n",
                   pid, GetLastError());
            VirtualFree(img, 0, MEM_RELEASE);
            return 1;
        }

        // 位数检查
        BOOL isWow64 = FALSE;
        if (sizeof(void*) == 8 && IsWow64Process(proc, &isWow64) && isWow64) {
            printf("[!] 目标进程是 32 位, 而本工具是 64 位, 无法注入。\n");
            CloseHandle(proc);
            VirtualFree(img, 0, MEM_RELEASE);
            return 1;
        }
    }

    printf("[*] 手动映射注入 (无 LoadLibrary, 无模块条目)...\n");
    bool ok = ManualMap(proc, img, imgSize);
    CloseHandle(proc);
    VirtualFree(img, 0, MEM_RELEASE);
    if (!ok) return 1;
    printf("[+] 注入完成。DLL 将向本机 35785 端口 UDP 上报 2 字节 (byte0=可以攻击 1/0, byte1=手持放置物 1/0)。\n");
    return 0;
}
