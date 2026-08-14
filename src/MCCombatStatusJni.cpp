//============================================================================
//  MCCombatStatusJni.dll  (V65)
//  通过 JNI 注入到 Minecraft (java/javaw) 进程中的工具 DLL。
//
//  功能: 在游戏渲染帧内读取:
//      Minecraft.getMinecraft().thePlayer / player
//      Minecraft.getMinecraft().objectMouseOver / hitResult
//  判断玩家当前是否 "瞄准到了一个可以攻击的生物" (canAttack), 以及
//  "手持物品是否为放置物" (canPlace, ItemBlock/BlockItem), 并把结果
//  写入共享内存 (Local\MCCombatStatus_<pid>), 同时通过 UDP 向本机
//  35785 端口发送 2 字节:
//      byte0 = 0x31 '1'=可以攻击 / 0x30 '0'=不可以
//      byte1 = 0x31 '1'=手持放置物 / 0x30 '0'=不是/空手
//  (byte0 与旧版 1 字节协议完全一致, 旧接收端无需改动)
//
//  V65 架构 (规避网易版等反检测):
//    不再创建线程、不再 AttachCurrentThread (外来原生线程附加 JVM 会触发
//    ThreadStart 事件被游戏侧保护检测)。改为内联钩住 gdi32!SwapBuffers
//    (LWJGL2/GLFW WGL 渲染路径的汇合点, 由游戏自己的 Client thread 每帧
//    调用), 钩子内 GetEnv() 复用该线程已有的 JNIEnv, 解析/采样/上报全部
//    在该线程内分帧完成 (每帧预算 8ms, 采样 5ms 节流)。
//
//  映射表由 tools/gen_maps.py 从 mappings-extracted (54 个版本) 自动生成,
//  见 mc_maps_generated.h (kGenMaps[]/kGenMapCount, 171 张: vanilla/forge/mojang/intermediary)。
//  三种运行时形态 (由数据自动判定), 按顺序自动尝试, 其中放置物判定成员为
//  "可选解析" —— 解析失败仅 canPlace 恒为 0, 绝不拖垮 canAttack:
//    S1 (1.8.8~1.13.2)  字段式  typeOfHit/entityHit = 字段 (vanilla=混淆名 / forge=SRG func_)
//    S2 (1.14~1.16.5)   getter 式 (vanilla=混淆名 / forge=SRG func_)
//    S3 (1.17+)          getter 式 (vanilla=混淆名 / forge=Mojang类+stable m_/f_ / mojang=官方名)
//
//  导出函数:
//    BOOL GetCanAttackNow(void)       -- 直接返回当前是否能攻击
//    BOOL IsJniReady(void)            -- JNI 是否已解析成功
//    BOOL GetCombatStatus(Status*)    -- 拷贝完整状态结构 (含 canPlace)
//============================================================================

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <tlhelp32.h>
#include <jni.h>
#include <jvmti.h>
#include <string.h>
#include <stdio.h>

//--------------------------------------------------------------------------
// 与 injector.exe 共享的状态结构 (布局固定, 跨进程共享)
//--------------------------------------------------------------------------
#pragma pack(push, 1)
struct CombatStatus {
    DWORD        magic;            // 0x4D435354 = 'MCST'
    DWORD        version;          // 7
    DWORD        pid;              // 被注入进程的 PID
    volatile LONG ready;           // JNI 解析是否成功
    volatile LONG inGame;          // 是否已进入游戏 (mc && player != null)
    volatile LONG canAttack;       // 核心结果1: 当前是否能攻击
    volatile LONG canPlace;        // 核心结果2: 手持物品是否为放置物 (ItemBlock/BlockItem)
    volatile LONG placeReady;      // 放置物判定链是否解析成功 (0=未启用, canPlace 恒 0)
    volatile LONG hitType;         // 0=未命中 1=命中方块 2=命中实体
    volatile LONG targetLiving;    // 目标是否为 LivingEntity
    volatile LONG targetAlive;     // 目标是否存活
    volatile LONG targetIsPlayer;  // 目标是否是玩家自己
    volatile LONG heldItemNull;    // 1 = 手持为空 (null), 0 = 手持有物品
    char          targetName[128]; // 目标的类名 (如 EntityZombie / pr / bfj)
    char          heldItemName[64];// 手持物品的 Item 类名 (如 ItemBlock / yo / cds)
    char          mappingName[32]; // 命中的命名体系 (如 forge1201)
    char          envName[48];     // 环境探测结果 (forge/optifine/fabric/launchwrapper)
    char          loaderName[48];  // 使用的游戏类加载器类名
    char          errMsg[96];      // 最近一次错误详情 (类名/异常信息)
    char          failLog[160];    // 最近一轮 10 套映射的失败原因汇总
    volatile LONG mcNull;          // 1 = getMinecraft() 返回 null (双份类副本问题)
    volatile LONG tick;            // 更新计数
    volatile LONG lastError;       // 最近一次错误码, 0=无错误
};
#pragma pack(pop)

//--------------------------------------------------------------------------
// 敏感字符串运行时解码 (静态 XOR 0x5A 编码, 避免明文落入静态签名样本)。
// 注意: g_xbuf 为单缓冲, 解码结果须立即使用 (本 DLL 全逻辑单线程执行)。
//--------------------------------------------------------------------------
static char g_xbuf[64];
static const char* XS(const volatile unsigned char* e, size_t n)
{
    for (size_t i = 0; i < n && i + 1 < sizeof(g_xbuf); ++i)
        g_xbuf[i] = (char)(e[i] ^ 0x5A);
    g_xbuf[n] = 0;
    return g_xbuf;
}
// volatile: 阻止编译器常量折叠 XOR 解码 (否则 -O2 会把明文直接内联进代码/常量区)
static const volatile unsigned char kEnc_MCCombatStatus_fmt[] = { 0x17, 0x19, 0x19, 0x35, 0x37, 0x38, 0x3B, 0x2E, 0x09, 0x2E, 0x3B, 0x2E, 0x2F, 0x29, 0x05, 0x7F, 0x36, 0x2F };
static const volatile unsigned char kEnc_jvm_dll[] = { 0x30, 0x2C, 0x37, 0x74, 0x3E, 0x36, 0x36 };
static const volatile unsigned char kEnc_JNI_GetCreatedJavaVMs[] = { 0x10, 0x14, 0x13, 0x05, 0x1D, 0x3F, 0x2E, 0x19, 0x28, 0x3F, 0x3B, 0x2E, 0x3F, 0x3E, 0x10, 0x3B, 0x2C, 0x3B, 0x0C, 0x17, 0x29 };
static const volatile unsigned char kEnc_gdi32_dll[] = { 0x3D, 0x3E, 0x33, 0x69, 0x68, 0x74, 0x3E, 0x36, 0x36 };
static const volatile unsigned char kEnc_SwapBuffers[] = { 0x09, 0x2D, 0x3B, 0x2A, 0x18, 0x2F, 0x3C, 0x3C, 0x3F, 0x28, 0x29 };
static const volatile unsigned char kEnc_gdi32full_dll[] = { 0x3D, 0x3E, 0x33, 0x69, 0x68, 0x3C, 0x2F, 0x36, 0x36, 0x74, 0x3E, 0x36, 0x36 };
static const volatile unsigned char kEnc_loopback[] = { 0x6B, 0x68, 0x6D, 0x74, 0x6A, 0x74, 0x6A, 0x74, 0x6B };
static const volatile unsigned char kEnc_ws2_32_dll[] = { 0x2D, 0x29, 0x68, 0x05, 0x69, 0x68, 0x74, 0x3E, 0x36, 0x36 };

static const DWORD kMagic      = 0x4D435354; // 'MCST'
static const DWORD kVersion    = 7;

static HANDLE         g_map    = NULL;
static CombatStatus*  g_status = NULL;

static void CopyName(char* dst, size_t cap, const char* src); // 前向声明
static jclass FindLoadedGameClass(JNIEnv* env, jobject loader, const char* name,
                                  jclass clsCls, jmethodID forName,
                                  const char* getterName, const char* getterSig); // 前向声明

// 追加失败原因到 failLog (每轮解析开始前清空)
static void AppendFail(const char* s)
{
    if (!g_status || !s) return;
    size_t n = strlen(g_status->failLog);
    size_t m = strlen(s);
    if (n + m + 2 >= sizeof(g_status->failLog)) return;
    if (n > 0) g_status->failLog[n++] = ' ';
    memcpy(g_status->failLog + n, s, m + 1);
}

//--------------------------------------------------------------------------
// 一套命名体系下, 所有需要解析的东西叫什么。
// 字段方式: typeOfHitField / entityHitField 非空, 对应 GetFieldID
// 方法方式: typeOfHitGetter / entityHitGetter 非空, 对应 GetMethodID
// canAttackWithItem / isAttackable 为 NULL 表示该版本不存在此检查, 跳过。
// entityConstAlt: 枚举常量名的备用候选 (不同运行时可能不同)。
//--------------------------------------------------------------------------
struct JniMap {
    const char* name;              // 标识
    // Minecraft 主类
    const char* mcClass;
    const char* mcSig;             // getMinecraft 返回类型描述符
    const char* getMinecraft;
    // 玩家
    const char* thePlayerField;
    const char* playerFieldSig;
    // 准星结果
    const char* mopField;
    const char* mopFieldSig;
    const char* mopClass;
    // 命中类型 (字段或 getter)
    const char* typeOfHitField;
    const char* typeOfHitGetter;
    const char* typeOfHitSig;      // 字段描述符 或 getter 签名
    // 命中实体 (字段或 getter; getter 定义在 EntityHitResult 上)
    const char* entityHitClass;
    const char* entityHitField;
    const char* entityHitGetter;
    const char* entityHitSig;
    // 命中类型枚举
    const char* typeClass;
    const char* entityConstField;
    const char* entityConstAlt;
    const char* entityConstSig;
    // 实体通用
    const char* entityClass;
    const char* canAttackWithItem; // NULL = 跳过 (现代版本已移除)
    const char* isAliveMethod;
    const char* isAttackable;      // NULL = 跳过
    const char* livingClass;
    // ---- 放置物判定 (全部可选: 解析失败仅 canPlace=0, 不拖垮 canAttack) ----
    const char* heldItemGetter;    // 玩家类上取手持物品的方法 (1.8.9: getHeldItem/bA/func_70694_bm; 1.20.1: getMainHandItem/eO/m_21205_)
    const char* heldItemSig;       // heldItemGetter 签名 (返回 ItemStack)
    const char* itemStackClass;    // ItemStack 类 (1.8.9: net/.../ItemStack/zx; 1.20.1: net/.../ItemStack/cfz)
    const char* itemGetItem;       // ItemStack.getItem (1.8.9: getItem/b/func_77973_b; 1.20.1: getItem/d/m_41720_)
    const char* itemGetItemSig;    // itemGetItem 签名 (返回 Item)
    const char* itemBlockClass;    // ItemBlock/BlockItem 类 (1.8.9: net/.../ItemBlock/yo; 1.20.1: net/.../BlockItem/cds)
};

#include "mc_maps_generated.h"

//--------------------------------------------------------------------------
// 类加载辅助: 解决 Forge/launchwrapper 环境下的"双份类副本"问题。
//
// 问题: 游戏类由 launchwrapper 的 LaunchClassLoader (或 ModLauncher 的
//       TransformingClassLoader) 加载; 而 JNI FindClass 默认使用系统类
//       加载器, 会从 classpath 重新加载一份独立副本 (静态字段未初始化,
//       字段 ID 与游戏实例不通用), 导致 getMinecraft() 返回 null。
// 解决: 先找到游戏的类加载器, 再用 Class.forName(name, true, loader)
//       加载类, 保证与游戏使用同一份类。
//--------------------------------------------------------------------------

// findLoadedClass 按名查找已加载的类 (绕开 ModLauncher/launchwrapper
// 对 loadClass 的重写; 游戏类已加载时这是最可靠的获取方式)
static jclass FindLoadedClassByName(JNIEnv* env, jobject loader, const char* slashName)
{
    if (!loader) return NULL;
    jclass loaderCls = env->FindClass("java/lang/ClassLoader");
    if (!loaderCls) { env->ExceptionClear(); return NULL; }
    jmethodID flc = env->GetMethodID(loaderCls, "findLoadedClass",
                                     "(Ljava/lang/String;)Ljava/lang/Class;");
    if (!flc) { env->ExceptionClear(); return NULL; }
    // 斜杠名转点分名
    char dot[256];
    size_t n = strlen(slashName);
    if (n >= sizeof(dot)) n = sizeof(dot) - 1;
    for (size_t i = 0; i < n; ++i) dot[i] = (slashName[i] == '/') ? '.' : slashName[i];
    dot[n] = 0;
    jstring nm = env->NewStringUTF(dot);
    if (!nm) { env->ExceptionClear(); return NULL; }
    jclass c = (jclass)env->CallObjectMethod(loader, flc, nm);
    if (env->ExceptionCheck()) { env->ExceptionClear(); }
    env->DeleteLocalRef(nm);
    return c; // 可能为 NULL (未加载)
}

// 用指定加载器加载类: loader 非空 -> Class.forName (点分名, 初始化);
// loader 为空 -> FindClass (斜杠名)。失败时记录原因到 errMsg。
static jclass LoadClass(JNIEnv* env, const char* slashName, jobject loader,
                        jclass clsCls, jmethodID forName)
{
    if (loader) {
        // 强制清 pending exception: 防止污染 findLoadedClass (静默失败元凶)
        env->ExceptionClear();
        // 优先 findLoadedClass: 已加载的类直接拿 (绕开 loadClass 重写问题)
        jclass loaded = FindLoadedClassByName(env, loader, slashName);
        if (loaded) return loaded;
        // 诊断: findLoadedClass 未命中
        if (g_status) {
            snprintf(g_status->errMsg, sizeof(g_status->errMsg),
                     "flc(%s)=X loader=%p", slashName, (void*)loader);
            g_status->lastError = 205;
        }
    }
    if (loader && clsCls && forName) {
        // 斜杠名转点分名
        char dot[256];
        size_t n = strlen(slashName);
        if (n >= sizeof(dot)) n = sizeof(dot) - 1;
        for (size_t i = 0; i < n; ++i) dot[i] = (slashName[i] == '/') ? '.' : slashName[i];
        dot[n] = 0;
        jstring name = env->NewStringUTF(dot);
        if (!name) { env->ExceptionClear(); return NULL; }
        // 注意: initialize=false! 类通常已被游戏初始化过, 不需要 (也不能) 再次初始化;
        // initialize=true 在真机上可能触发 <clinit> 抛异常 (如主类初始化依赖未就绪)
        jclass c = (jclass)env->CallStaticObjectMethod(clsCls, forName, name, JNI_FALSE, loader);
        env->DeleteLocalRef(name);
        if (env->ExceptionCheck()) {
            // 记录异常类名 (如 ClassNotFoundException / ExceptionInInitializerError)
            jthrowable ex = env->ExceptionOccurred();
            env->ExceptionClear();
            jclass exCls = ex ? env->GetObjectClass(ex) : NULL;
            // 注意: getName 属于 java/lang/Class, 必须用 clsCls 取方法 ID!
            jmethodID getName = exCls ? env->GetMethodID(clsCls, "getName", "()Ljava/lang/String;") : NULL;
            jstring nm = getName ? (jstring)env->CallObjectMethod(exCls, getName) : NULL;
            const char* utf = nm ? env->GetStringUTFChars(nm, NULL) : NULL;
            if (g_status) {
                snprintf(g_status->errMsg, sizeof(g_status->errMsg),
                         "forName(%s) -> %s", dot, utf ? utf : "?");
                g_status->lastError = 202;
            }
            if (utf) env->ReleaseStringUTFChars(nm, utf);
            if (env->ExceptionCheck()) env->ExceptionClear(); // 必须清! 防止污染后续调用
            return NULL;
        }
        if (!c && g_status) {
            // 静默失败: CallStaticObjectMethod 返回 NULL 且无异常
            snprintf(g_status->errMsg, sizeof(g_status->errMsg),
                     "forName(%s) -> NULL(无异常)", dot);
            g_status->lastError = 204;
        }
        return c;
    }
    jclass c = env->FindClass(slashName);
    if (env->ExceptionCheck()) {
        // 记录异常类名, 精确诊断 (ClassNotFoundException / NoClassDefFoundError / VerifyError...)
        jthrowable ex = env->ExceptionOccurred();
        env->ExceptionClear();
        jclass exCls = ex ? env->GetObjectClass(ex) : NULL;
        jmethodID getName = exCls ? env->GetMethodID(env->FindClass("java/lang/Class"), "getName", "()Ljava/lang/String;") : NULL;
        jstring nm = getName ? (jstring)env->CallObjectMethod(exCls, getName) : NULL;
        const char* utf = nm ? env->GetStringUTFChars(nm, NULL) : NULL;
        if (g_status) {
            snprintf(g_status->errMsg, sizeof(g_status->errMsg),
                     "FindClass(%s) -> %s", slashName, utf ? utf : "?");
            g_status->lastError = 203;
        }
        if (utf) env->ReleaseStringUTFChars(nm, utf);
        if (env->ExceptionCheck()) env->ExceptionClear();
        return NULL;
    }
    if (!c && g_status) {
        // 静默失败: FindClass 返回 NULL 且无异常 (pending exception 污染?)
        snprintf(g_status->errMsg, sizeof(g_status->errMsg),
                 "FindClass(%s) -> NULL(无异常)", slashName);
        g_status->lastError = 204;
    }
    return c;
}

// 尝试从 launchwrapper 的 Launch.classLoader 拿游戏类加载器 (Forge 1.8.9 / 原版 1.8.x-1.12.x)
static jobject FindLaunchClassLoader(JNIEnv* env, jclass clsCls, jmethodID forName)
{
#ifdef NO_LAUNCH_LOADER
    (void)env; (void)clsCls; (void)forName;
    return NULL;
#else
    if (g_status) CopyName(g_status->errMsg, sizeof(g_status->errMsg), "L1:find-Launch");
    jclass launch = env->FindClass("net/minecraft/launchwrapper/Launch");
    if (!launch) { env->ExceptionClear(); return NULL; }
    if (g_status) CopyName(g_status->errMsg, sizeof(g_status->errMsg), "L2:Launch-ok");
    jfieldID cl = env->GetStaticFieldID(launch, "classLoader", "Lnet/minecraft/launchwrapper/LaunchClassLoader;");
    if (!cl) { env->ExceptionClear(); return NULL; }
    if (g_status) CopyName(g_status->errMsg, sizeof(g_status->errMsg), "L3:field-ok");
    jobject loader = env->GetStaticObjectField(launch, cl);
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (g_status) CopyName(g_status->errMsg, sizeof(g_status->errMsg),
             loader ? "L4:loader-ok" : "L4:loader-null");
    return loader; // 可能为 NULL (未设置)
#endif
}

// 兜底: 遍历所有 Java 线程, 找一个能加载游戏类的上下文类加载器
// (对 Forge 1.17+ / ModLauncher 环境有效: 主线程的 context loader 是
//  TransformingClassLoader, 能加载 net.minecraft.* )
static jobject FindThreadClassLoader(JNIEnv* env, jclass clsCls, jmethodID forName)
{
#ifdef NO_THREAD_LOADER
    (void)env; (void)clsCls; (void)forName;
    return NULL;
#else
    if (g_status) CopyName(g_status->errMsg, sizeof(g_status->errMsg), "T0:start");
    jclass threadCls = env->FindClass("java/lang/Thread");
    if (!threadCls) { env->ExceptionClear(); return NULL; }
    if (g_status) CopyName(g_status->errMsg, sizeof(g_status->errMsg), "T1:threadCls");
    jmethodID getAll = env->GetStaticMethodID(threadCls, "getAllStackTraces",
                                              "()Ljava/util/Map;");
    if (!getAll) { env->ExceptionClear(); return NULL; }
    if (g_status) CopyName(g_status->errMsg, sizeof(g_status->errMsg), "T2:getAll");
    jobject map = env->CallStaticObjectMethod(threadCls, getAll);
    if (env->ExceptionCheck() || !map) { env->ExceptionClear(); return NULL; }
    if (g_status) CopyName(g_status->errMsg, sizeof(g_status->errMsg), "T3:map");

    jclass mapCls = env->FindClass("java/util/Map");
    jclass setCls = env->FindClass("java/util/Set");
    jclass itCls  = env->FindClass("java/util/Iterator");
    if (!mapCls || !setCls || !itCls) { env->ExceptionClear(); return NULL; }
    jmethodID keySet = env->GetMethodID(mapCls, "keySet", "()Ljava/util/Set;");
    jmethodID iterator = env->GetMethodID(setCls, "iterator", "()Ljava/util/Iterator;");
    jmethodID hasNext = env->GetMethodID(itCls, "hasNext", "()Z");
    jmethodID next = env->GetMethodID(itCls, "next", "()Ljava/lang/Object;");
    jmethodID getCtx = env->GetMethodID(threadCls, "getContextClassLoader",
                                        "()Ljava/lang/ClassLoader;");
    if (!keySet || !iterator || !hasNext || !next || !getCtx) {
        env->ExceptionClear();
        return NULL;
    }

    jobject set = env->CallObjectMethod(map, keySet);
    if (g_status) CopyName(g_status->errMsg, sizeof(g_status->errMsg), "T4:keySet");
    jobject it  = env->CallObjectMethod(set, iterator);
    if (g_status) CopyName(g_status->errMsg, sizeof(g_status->errMsg), "T5:iterator");
    if (env->ExceptionCheck() || !it) { env->ExceptionClear(); return NULL; }

    DWORD tStart = GetTickCount();
    jobject found = NULL;
    jobject found2 = NULL; // 后备 loader (只能加载混淆名等非标准类)
    while (env->CallBooleanMethod(it, hasNext)) {
        if (GetTickCount() - tStart > 10000) break; // 10 秒超时保护
        if (g_status) CopyName(g_status->errMsg, sizeof(g_status->errMsg), "T6:hasNext");
        if (env->ExceptionCheck()) { env->ExceptionClear(); break; }
        jobject thread = env->CallObjectMethod(it, next);
        if (g_status) CopyName(g_status->errMsg, sizeof(g_status->errMsg), "T7:next");
        if (env->ExceptionCheck()) { env->ExceptionClear(); break; }
        jobject loader = env->CallObjectMethod(thread, getCtx);
        if (g_status) CopyName(g_status->errMsg, sizeof(g_status->errMsg), "T8:getCtx");
        if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
        if (!loader) continue;
        // 测试这个加载器能否加载游戏主类。
        // 注意: 验证标准不能太弱——AppClassLoader 若 classpath 混入其他版本
        // 残留 jar (如能加载 1.8.9 的 ave), 会被误选。优先要求能加载
        // MCP/Mojang 名 net/minecraft/client/Minecraft (1.8.9~1.20.1 通用);
        // 只有全部 loader 都不行时, 才接受能加载其他 mcClass 的 loader。
        jclass mcProbe = LoadClass(env, "net/minecraft/client/Minecraft",
                                   loader, clsCls, forName);
        if (mcProbe) {
            env->DeleteLocalRef(mcProbe);
            found = loader;      // 首选: 能加载标准 Minecraft 类
            break;
        }
        // 后备: 能加载其他候选 (如 1.8.9 混淆名 ave)
        for (int i = 0; i < kGenMapCount && !found2; ++i) {
            if (strcmp(kGenMaps[i].mcClass, "net/minecraft/client/Minecraft") == 0)
                continue; // 已测过
            jclass probe = LoadClass(env, kGenMaps[i].mcClass, loader, clsCls, forName);
            if (probe) {
                env->DeleteLocalRef(probe);
                found2 = loader;
                break;
            }
        }
    }
    if (!found) found = found2; // 无首选时用后备
    if (g_status) CopyName(g_status->errMsg, sizeof(g_status->errMsg), "T9:done");
    if (env->ExceptionCheck()) env->ExceptionClear();
    return found;
#endif // NO_THREAD_LOADER
}

// 获取游戏类加载器 (返回 local ref, 调用者负责 DeleteLocalRef)
// 简化策略 (吸取真机教训):
//   1. launchwrapper Launch.classLoader —— 验证能否加载游戏类 (OptiFine 环境下
//      它只含库, 加载不了 ave 等游戏类, 必须验证!)
//   2. 验证失败 -> 直接用系统类加载器 (app loader, -cp 一定有游戏 jar)
//      (线程遍历在 OptiFine 环境会卡死, 弃用)
// 双份副本问题 (app loader 的类可能与游戏实例不同) 由 mcNull 切换逻辑兜底。
static jobject FindGameClassLoader(JNIEnv* env, jclass clsCls, jmethodID forName)
{
#ifdef NO_GAME_LOADER
    (void)env; (void)clsCls; (void)forName;
    return NULL;
#else
    // 1. Launch.classLoader —— 但必须先验证它能加载游戏类!
    jobject loader = FindLaunchClassLoader(env, clsCls, forName);
    if (loader) {
        bool canLoad = false;
        for (int i = 0; i < kGenMapCount && !canLoad; ++i) {
            jclass probe = LoadClass(env, kGenMaps[i].mcClass, loader, clsCls, forName);
            if (probe) {
                env->DeleteLocalRef(probe);
                canLoad = true;
            }
        }
        if (canLoad) return loader;
        // 不能加载游戏类 -> 记录后走 app loader
        if (g_status) CopyName(g_status->errMsg, sizeof(g_status->errMsg),
                               "E4:launch-loader-cant-load-game");
        env->DeleteLocalRef(loader);
    }
    // 2. 线程遍历: 1.17+ ModLauncher 环境的主线程 context loader
    //    = TransformingClassLoader (游戏类加载器), 带 10 秒超时保护
    loader = FindThreadClassLoader(env, clsCls, forName);
    if (loader) return loader;
    // 3. 系统类加载器 (app loader, -cp 一定有游戏 jar)
    if (g_status) CopyName(g_status->errMsg, sizeof(g_status->errMsg), "E5:system-loader");
    jclass clCls = env->FindClass("java/lang/ClassLoader");
    if (!clCls) { env->ExceptionClear(); return NULL; }
    jmethodID getSys = env->GetStaticMethodID(clCls, "getSystemClassLoader",
                                              "()Ljava/lang/ClassLoader;");
    if (!getSys) { env->ExceptionClear(); return NULL; }
    jobject sys = env->CallStaticObjectMethod(clCls, getSys);
    if (env->ExceptionCheck()) env->ExceptionClear();
    return sys; // 可能为 NULL (理论不会)
#endif
}

//--------------------------------------------------------------------------
// 环境探测: 检测 forge / optifine / fabric / launchwrapper
//--------------------------------------------------------------------------
static void DetectEnv(JNIEnv* env, jobject loader, jclass clsCls, jmethodID forName,
                      char* out, size_t cap)
{
    struct Mark { const char* cls; const char* tag; };
    static const Mark marks[] = {
        { "net/minecraftforge/fml/common/FMLCommonHandler",  "forge" },
        { "net/minecraftforge/fml/loading/FMLLoader",        "forge" },
        { "net/neoforged/fml/loading/FMLLoader",             "neoforge" },
        { "optifine/OptiFineClassTransformer",               "optifine" },
        { "net/optifine/Config",                             "optifine" },
        { "net/fabricmc/loader/FabricLoader",                "fabric" },
        { "net/minecraft/launchwrapper/Launch",              "launchwrapper" },
    };
    out[0] = 0;
    for (size_t i = 0; i < sizeof(marks) / sizeof(marks[0]); ++i) {
        jclass c = LoadClass(env, marks[i].cls, loader, clsCls, forName);
        if (c) {
            env->DeleteLocalRef(c);
            if (out[0]) {
                size_t n = strlen(out);
                if (n + strlen(marks[i].tag) + 2 < cap) {
                    out[n++] = '+';
                    strcpy(out + n, marks[i].tag);
                }
            } else {
                CopyName(out, cap, marks[i].tag);
            }
        }
    }
}

//--------------------------------------------------------------------------
// 解析结果 (ID 解析一次, 永久使用)
//--------------------------------------------------------------------------
struct Resolved {
    bool       ok;
    const char* name;
    jclass     mcClass, mopClass, typeClass, entityClass, livingClass;
    jclass     entityHitCls;      // getter 方式下的 EntityHitResult 类
    jclass     javaLangClass;
    jmethodID  getMinecraft, canAttackWithItem, isAliveMethod, isAttackable, classGetName;
    jmethodID  typeOfHitGetter, entityHitGetter;
    jfieldID   thePlayerField, mopField, typeOfHitField, entityHitField, entityConstField;
    // ---- 放置物判定 (可选: 全部为 NULL 时 canPlace 恒 0, 不影响 canAttack) ----
    bool       placeOk;           // 放置物判定链是否解析成功 (诊断用)
    jclass     itemStackCls;      // ItemStack 类
    jclass     itemBlockCls;      // ItemBlock/BlockItem 类
    jmethodID  heldItemGetter;    // player.getHeldItem()/getMainHandItem()
    jmethodID  itemGetItem;       // stack.getItem()
};

//--------------------------------------------------------------------------
// 用一套映射尝试解析, 全部成功才返回 true。
// loader: 游戏类加载器 (可能为 NULL, 此时用 FindClass)
//--------------------------------------------------------------------------
static void NoteErr(const char* mapName, const char* why)
{
    if (g_status) {
        snprintf(g_status->errMsg, sizeof(g_status->errMsg), "%s: %s", mapName, why);
        g_status->lastError = 201;
    }
}

static bool ResolveWith(JNIEnv* env, const JniMap& m, Resolved& r,
                        jobject loader, jclass clsCls, jmethodID forName)
{
    memset(&r, 0, sizeof(r));

    jclass mc  = LoadClass(env, m.mcClass, loader, clsCls, forName);
    jclass mop = LoadClass(env, m.mopClass, loader, clsCls, forName);
    jclass typ = LoadClass(env, m.typeClass, loader, clsCls, forName);
    jclass ent = LoadClass(env, m.entityClass, loader, clsCls, forName);
    jclass liv = LoadClass(env, m.livingClass, loader, clsCls, forName);
    jclass jlc = env->FindClass("java/lang/Class");
    if (!mc || !mop || !typ || !ent || !liv || !jlc) {
        AppendFail(m.name);
        AppendFail(":cls ");
        if (g_status && (g_status->lastError == 202 || g_status->lastError == 203)) {
            // LoadClass 已记录详情 (forName/FindClass 异常), 加 map 名前缀
            char tmp[128];
            snprintf(tmp, sizeof(tmp), "%s: %s", m.name, g_status->errMsg);
            CopyName(g_status->errMsg, sizeof(g_status->errMsg), tmp);
        } else {
            NoteErr(m.name, "FindClass 失败");
        }
        env->ExceptionClear();
        return false;
    }

    jmethodID getMc = env->GetStaticMethodID(mc, m.getMinecraft, m.mcSig);
    jmethodID alive = env->GetMethodID(ent, m.isAliveMethod, "()Z");
    jmethodID name  = env->GetMethodID(jlc, "getName", "()Ljava/lang/String;");
    jfieldID  pl    = env->GetFieldID(mc, m.thePlayerField, m.playerFieldSig);
    jfieldID  mopF  = env->GetFieldID(mc, m.mopField, m.mopFieldSig);
    if (!getMc || !alive || !name || !pl || !mopF) {
        NoteErr(m.name, "成员 ID 解析失败");
        AppendFail(m.name);
        AppendFail(":member ");
        env->ExceptionClear();
        return false;
    }

    // 命中类型: 字段方式 或 getter 方式
    jmethodID hitG = NULL;
    jfieldID  hitF = NULL;
    if (m.typeOfHitGetter) {
        hitG = env->GetMethodID(mop, m.typeOfHitGetter, m.typeOfHitSig);
        if (!hitG) { env->ExceptionClear(); return false; }
    } else {
        hitF = env->GetFieldID(mop, m.typeOfHitField, m.typeOfHitSig);
        if (!hitF) { env->ExceptionClear(); return false; }
    }

    // 命中实体: 字段方式 或 getter 方式 (getter 定义在 EntityHitResult 上)
    jclass    ehr = NULL;
    jmethodID entG = NULL;
    jfieldID  entF = NULL;
    if (m.entityHitGetter) {
        ehr = LoadClass(env, m.entityHitClass, loader, clsCls, forName);
        if (!ehr) { env->ExceptionClear(); return false; }
        entG = env->GetMethodID(ehr, m.entityHitGetter, m.entityHitSig);
        if (!entG) { env->ExceptionClear(); return false; }
    } else {
        entF = env->GetFieldID(mop, m.entityHitField, m.entityHitSig);
        if (!entF) { env->ExceptionClear(); return false; }
    }

    // 能否攻击 (可空) 与 可否被攻击 (可空)
    jmethodID atk = NULL;
    if (m.canAttackWithItem) {
        atk = env->GetMethodID(ent, m.canAttackWithItem, "()Z");
        if (!atk) { env->ExceptionClear(); return false; }
    }
    jmethodID atkb = NULL;
    if (m.isAttackable) {
        atkb = env->GetMethodID(ent, m.isAttackable, "()Z");
        if (!atkb) { env->ExceptionClear(); return false; }
    }

    // 枚举常量 (支持备用名)
    jfieldID entC = env->GetStaticFieldID(typ, m.entityConstField, m.entityConstSig);
    if (!entC && m.entityConstAlt) {
        env->ExceptionClear();
        entC = env->GetStaticFieldID(typ, m.entityConstAlt, m.entityConstSig);
    }
    if (!entC) { env->ExceptionClear(); return false; }

    // ---- 放置物判定成员: 全部可选解析 (失败仅 canPlace=0, 不拖垮 canAttack) ----
    // 注意: 解析失败只清异常 + 留 NULL, 不 return false。
    // 需要的 ID: 玩家类上的 heldItemGetter + ItemStack 类 + stack.getItem + ItemBlock 类。
    // 判定用 IsInstanceOf(getItem() 返回值, itemBlockCls), 不需要单独的 Item 基类。
    jclass    itemStackCls = NULL;
    jclass    itemBlockCls = NULL;
    jmethodID heldGetter   = NULL;
    jmethodID getItem      = NULL;
    if (m.heldItemGetter && m.itemStackClass && m.itemGetItem && m.itemBlockClass) {
        heldGetter = env->GetMethodID(liv, m.heldItemGetter, m.heldItemSig);
        if (env->ExceptionCheck()) { env->ExceptionClear(); heldGetter = NULL; }
        itemStackCls = LoadClass(env, m.itemStackClass, loader, clsCls, forName);
        itemBlockCls = LoadClass(env, m.itemBlockClass, loader, clsCls, forName);
        if (itemStackCls) {
            getItem = env->GetMethodID(itemStackCls, m.itemGetItem, m.itemGetItemSig);
            if (env->ExceptionCheck()) { env->ExceptionClear(); getItem = NULL; }
        }
        // 任一环节失败 -> 整体放弃放置物检测 (失败时统一留 NULL)
        if (!heldGetter || !itemStackCls || !getItem || !itemBlockCls) {
            if (itemStackCls) env->DeleteLocalRef(itemStackCls);
            if (itemBlockCls) env->DeleteLocalRef(itemBlockCls);
            itemStackCls = NULL; itemBlockCls = NULL;
            heldGetter = NULL; getItem = NULL;
        }
    }

    r.ok               = true;
    r.name             = m.name;
    r.mcClass          = mc;
    r.mopClass         = mop;
    r.typeClass        = typ;
    r.entityClass      = ent;
    r.livingClass      = liv;
    r.entityHitCls     = ehr;
    r.javaLangClass    = jlc;
    r.getMinecraft     = getMc;
    r.canAttackWithItem = atk;
    r.isAliveMethod    = alive;
    r.isAttackable     = atkb;
    r.classGetName     = name;
    r.thePlayerField   = pl;
    r.mopField         = mopF;
    r.typeOfHitGetter  = hitG;
    r.typeOfHitField   = hitF;
    r.entityHitGetter  = entG;
    r.entityHitField   = entF;
    r.entityConstField = entC;
    // 放置物判定 (可选, 可能全为 NULL -> canPlace 恒 0)
    r.placeOk          = (heldGetter && itemStackCls && getItem && itemBlockCls);
    r.itemStackCls     = itemStackCls;
    r.itemBlockCls     = itemBlockCls;
    r.heldItemGetter   = heldGetter;
    r.itemGetItem      = getItem;
    return true;
}

//--------------------------------------------------------------------------
// 拷贝字符串到状态结构 (保证以 \0 结尾)
//--------------------------------------------------------------------------
static void CopyName(char* dst, size_t cap, const char* src)
{
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = 0; return; }
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

//--------------------------------------------------------------------------
// V70: APC 加载器 —— 注入器把映像映射进目标后, 用 NtQueueApcThread 在
// 目标线程上执行本函数 (不创建新线程)。执行前提: 映像已映射、IAT 可能
// 未修复、CRT 未初始化、DllMain 未运行。因此本函数必须自包含:
//   * 不调用任何 IAT 导入函数 (先手工遍历导出表解析 kernel32 再修 IAT)
//   * 不使用任何需要构造的静态对象
//   * 参数: rcx=映像基址 (NtQueueApcThread 的 SystemArgument1)
//--------------------------------------------------------------------------
static void* GetPeb(void);   // 前向声明 (定义在反检测段)
static void* LdrGetProcAddr(HMODULE mod, const char* name)
{
    BYTE* b = (BYTE*)mod;
    if (!b) return NULL;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)b;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return NULL;
    IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(b + dos->e_lfanew);
    DWORD rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
    if (!rva) return NULL;
    IMAGE_EXPORT_DIRECTORY* ed = (IMAGE_EXPORT_DIRECTORY*)(b + rva);
    DWORD* names = (DWORD*)(b + ed->AddressOfNames);
    WORD*  ords  = (WORD*)(b + ed->AddressOfNameOrdinals);
    DWORD* funcs = (DWORD*)(b + ed->AddressOfFunctions);
    for (DWORD i = 0; i < ed->NumberOfNames; i++) {
        const char* n = (const char*)(b + names[i]);
        const char* p = name;
        while (*p && *n && *p == *n) { p++; n++; }
        if (*p == 0 && *n == 0)
            return b + funcs[ords[i]];
    }
    return NULL;
}

// PEB InLoadOrder 遍历找模块基址 (按 BaseDllName 比较; 大小写不敏感)
static void* PebFindModule(const char* targetLower, int targetLen)
{
    unsigned char* peb = (unsigned char*)GetPeb();
    if (!peb) return NULL;
    void** ldr = (void**)(peb + 0x18);
    if (!ldr || !*ldr) return NULL;
    unsigned char* l = (unsigned char*)*ldr;
    unsigned char* head = l + 0x10;                    // InLoadOrderModuleList 头
    for (unsigned char* cur = *(unsigned char**)head; cur && cur != head; cur = *(unsigned char**)cur) {
        void* dllBase = *(void**)(cur + 0x30);
        unsigned short len = *(unsigned short*)(cur + 0x58);   // BaseDllName.Length
        wchar_t* nm = *(wchar_t**)(cur + 0x60);                // BaseDllName.Buffer
        if (!nm || len < 4) continue;
        int n = len / 2;
        if (n != targetLen) continue;
        bool match = true;
        for (int i = 0; i < n; i++) {
            char c = (char)nm[i];
            if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
            if (c != targetLower[i]) { match = false; break; }
        }
        if (match) return dllBase;
    }
    return NULL;
}

extern "C" void ApcLoader(void* base, void*, void*)
{
    if (!base) return;
    // 1. 手工解析 kernel32 (PEB 遍历 + 导出表遍历, 全程不碰 IAT)
    static const char k32Name[] = { 'k','e','r','n','e','l','3','2','.','d','l','l' };
    HMODULE k32 = (HMODULE)PebFindModule(k32Name, sizeof(k32Name));
    if (!k32) return;
    void* pGetProc = LdrGetProcAddr(k32, "GetProcAddress");
    void* pLoadLib = LdrGetProcAddr(k32, "LoadLibraryA");
    if (!pGetProc || !pLoadLib) return;
    typedef void* (WINAPI* GPA_t)(HMODULE, const char*);
    typedef HMODULE (WINAPI* LL_t)(const char*);
    GPA_t GPA = (GPA_t)pGetProc;
    LL_t  LL  = (LL_t)pLoadLib;

    // 2. 修复自身 IAT (GetModuleHandle 不可用, 用 LoadLibraryA: 已加载模块只加引用计数)
    BYTE* b = (BYTE*)base;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)b;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(b + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY& imp = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    DWORD off = 0;
    while (off + sizeof(IMAGE_IMPORT_DESCRIPTOR) <= imp.Size) {
        IMAGE_IMPORT_DESCRIPTOR* desc = (IMAGE_IMPORT_DESCRIPTOR*)(b + imp.VirtualAddress + off);
        if (!desc->Name && !desc->FirstThunk) break;
        const char* dllName = (const char*)(b + desc->Name);
        HMODULE mod = LL(dllName);
        if (!mod) return;
        ULONGLONG* orig = (ULONGLONG*)(b + (desc->OriginalFirstThunk
                                            ? desc->OriginalFirstThunk : desc->FirstThunk));
        ULONGLONG* iat = (ULONGLONG*)(b + desc->FirstThunk);
        for (int i = 0; orig[i]; i++) {
            if (iat[i] != 0) continue;   // 幂等: 首次 APC/映射器已修复的槽跳过 (并发安全)
            ULONGLONG val = 0;
            if (orig[i] & 0x8000000000000000ULL)
                val = (ULONGLONG)(ULONG_PTR)GPA(mod, (LPCSTR)(orig[i] & 0xFFFF));
            else {
                IMAGE_IMPORT_BY_NAME* ibn = (IMAGE_IMPORT_BY_NAME*)(b + orig[i]);
                val = (ULONGLONG)(ULONG_PTR)GPA(mod, ibn->Name);
            }
            if (!val) return;
            iat[i] = val;
        }
        off += sizeof(IMAGE_IMPORT_DESCRIPTOR);
    }

    // 3. 执行 DllMain (幂等: g_attached 防重复初始化)
    typedef BOOL (WINAPI* Entry_t)(HMODULE, DWORD, LPVOID);
    Entry_t entry = (Entry_t)(b + nt->OptionalHeader.AddressOfEntryPoint);
    entry((HMODULE)base, DLL_PROCESS_ATTACH, NULL);
}

//--------------------------------------------------------------------------
// UDP 上报已移除 (V68): 游戏进程内不再创建 socket / 不做任何网络调用。
// 状态仅通过共享内存 Local\MCCombatStatus_<pid> 对外发布。
//--------------------------------------------------------------------------

//--------------------------------------------------------------------------
// 每帧更新: 计算 "是否能攻击" 并写入共享内存
//--------------------------------------------------------------------------
static void UpdateStatus(JNIEnv* env, const Resolved& r)
{
    if (!g_status) return;

    if (env->PushLocalFrame(32) < 0) {
        g_status->lastError = 100; // 本地引用栈溢出
        return;
    }
    g_status->lastError = 0;

    jobject mc = env->CallStaticObjectMethod(r.mcClass, r.getMinecraft);
    if (env->ExceptionCheck()) {
        // 记录异常类名, 便于诊断 (如双份类副本 / 类初始化失败)
        jthrowable ex = env->ExceptionOccurred();
        env->ExceptionClear();
        jclass exCls = ex ? env->GetObjectClass(ex) : NULL;
        jstring nm = exCls ? (jstring)env->CallObjectMethod(exCls, r.classGetName) : NULL;
        const char* utf = nm ? env->GetStringUTFChars(nm, NULL) : NULL;
        CopyName(g_status->errMsg, sizeof(g_status->errMsg), utf);
        if (utf) env->ReleaseStringUTFChars(nm, utf);
        g_status->lastError = 101;
    }
    if (!mc) {
        // 游戏尚未初始化 (主类未加载 / 双份类副本问题)
        g_status->inGame    = 0;
        g_status->canAttack = 0;
        g_status->canPlace  = 0;
        g_status->placeReady = 0;
        g_status->heldItemNull = 0;
        g_status->hitType   = 0;
        g_status->mcNull    = 1;   // 标记: getMinecraft() 拿不到对象
        g_status->tick++;          // 即使拿不到主类也计数, 便于判断 worker 是否存活
        env->PopLocalFrame(NULL);
        return;
    }
    g_status->mcNull = 0;

    jobject player = env->GetObjectField(mc, r.thePlayerField);
    jobject mop    = env->GetObjectField(mc, r.mopField);
    if (env->ExceptionCheck()) { env->ExceptionClear(); g_status->lastError = 102; }

    g_status->inGame = (player != NULL);

    if (!player) {
        // 未进入游戏 (无玩家): 全部清零 (canPlace 也依赖 player, 无法计算)
        g_status->canAttack      = 0;
        g_status->canPlace       = 0;
        g_status->placeReady     = 0;
        g_status->heldItemNull   = 0;
        g_status->hitType        = 0;
        g_status->targetLiving   = 0;
        g_status->targetAlive    = 0;
        g_status->targetIsPlayer = 0;
        CopyName(g_status->targetName, sizeof(g_status->targetName), NULL);
        CopyName(g_status->heldItemName, sizeof(g_status->heldItemName), NULL);
        g_status->tick++;
        env->PopLocalFrame(NULL);
        return;
    }

    // ---- 放置物判定: 手持物品是否为 ItemBlock/BlockItem ----
    // 只依赖 player (手持), 与准星/mop 无关; 在 mop 检查之前计算。
    // 链: player.getHeldItem()/getMainHandItem() -> stack.getItem() -> instanceof itemBlockCls
    // 可选解析: 任一 ID 为 NULL (解析失败) 时 canPlace 恒 0, 不影响 canAttack。
    g_status->placeReady = r.placeOk ? 1 : 0; // 诊断: 放置物链是否解析成功
    LONG canPlace    = 0;
    LONG heldNull    = 1; // 默认空手
    CopyName(g_status->heldItemName, sizeof(g_status->heldItemName), NULL);
    if (r.heldItemGetter && r.itemStackCls && r.itemGetItem && r.itemBlockCls) {
        jobject stack = env->CallObjectMethod(player, r.heldItemGetter);
        if (env->ExceptionCheck()) { env->ExceptionClear(); g_status->lastError = 110; }
        if (stack) {
            heldNull = 0;
            jobject item = env->CallObjectMethod(stack, r.itemGetItem);
            if (env->ExceptionCheck()) { env->ExceptionClear(); g_status->lastError = 111; }
            if (item) {
                canPlace = env->IsInstanceOf(item, r.itemBlockCls) ? 1 : 0;
                // 手持物品的 Item 类名 (如 ItemBlock / yo / cds), 便于调试
                jobject cls = env->GetObjectClass(item);
                jstring nm  = (jstring)env->CallObjectMethod(cls, r.classGetName);
                if (env->ExceptionCheck()) { env->ExceptionClear(); g_status->lastError = 112; }
                const char* utf = nm ? env->GetStringUTFChars(nm, NULL) : NULL;
                if (utf) {
                    CopyName(g_status->heldItemName, sizeof(g_status->heldItemName), utf);
                    env->ReleaseStringUTFChars(nm, utf);
                }
                env->DeleteLocalRef(cls);
                if (nm) env->DeleteLocalRef(nm);
                env->DeleteLocalRef(item);
            }
            env->DeleteLocalRef(stack);
        }
    }

    if (!mop) {
        // 未瞄准: canAttack=0, 瞄准相关字段清零; canPlace/heldNull 保留 (独立检测)
        g_status->canAttack      = 0;
        g_status->hitType        = 0;
        g_status->targetLiving   = 0;
        g_status->targetAlive    = 0;
        g_status->targetIsPlayer = 0;
        CopyName(g_status->targetName, sizeof(g_status->targetName), NULL);
        g_status->canPlace       = canPlace;
        g_status->heldItemNull   = heldNull;
        g_status->tick++;
        env->PopLocalFrame(NULL);
        return;
    }

    // 命中类型: 0=miss 1=block 2=entity (字段方式或 getter 方式)
    jobject typeObj = r.typeOfHitGetter
        ? env->CallObjectMethod(mop, r.typeOfHitGetter)
        : env->GetObjectField(mop, r.typeOfHitField);
    jobject entConst = env->GetStaticObjectField(r.typeClass, r.entityConstField);
    if (env->ExceptionCheck()) { env->ExceptionClear(); g_status->lastError = 103; }

    int hit = 0;
    if (typeObj) {
        if (entConst && env->IsSameObject(typeObj, entConst)) hit = 2;
        else hit = 1; // BLOCK 或 MISS
    }
    g_status->hitType = hit;

    // 命中实体: 仅当命中实体时读取 (getter 方式下必须保证对象是 EntityHitResult)
    jobject entity = NULL;
    if (hit == 2) {
        entity = r.entityHitGetter
            ? env->CallObjectMethod(mop, r.entityHitGetter)
            : env->GetObjectField(mop, r.entityHitField);
        if (env->ExceptionCheck()) { env->ExceptionClear(); g_status->lastError = 104; }
    }

    LONG living = 0, alive = 0, isSelf = 0;
    if (entity) {
        living = env->IsInstanceOf(entity, r.livingClass) ? 1 : 0;
        alive  = env->CallBooleanMethod(entity, r.isAliveMethod) ? 1 : 0;
        isSelf = (player && env->IsSameObject(entity, player)) ? 1 : 0;
        if (env->ExceptionCheck()) { env->ExceptionClear(); g_status->lastError = 105; }

        // 目标类名 (如 EntityZombie / pr / bfj), 便于调试
        jobject cls = env->GetObjectClass(entity);
        jstring nm  = (jstring)env->CallObjectMethod(cls, r.classGetName);
        if (env->ExceptionCheck()) { env->ExceptionClear(); g_status->lastError = 106; }
        const char* utf = nm ? env->GetStringUTFChars(nm, NULL) : NULL;
        if (utf) {
            CopyName(g_status->targetName, sizeof(g_status->targetName), utf);
            env->ReleaseStringUTFChars(nm, utf);
        } else {
            CopyName(g_status->targetName, sizeof(g_status->targetName), NULL);
        }
        env->DeleteLocalRef(cls);
        if (nm) env->DeleteLocalRef(nm);
    } else {
        CopyName(g_status->targetName, sizeof(g_status->targetName), NULL);
    }

    // 当前手持物品是否允许攻击 (仅 1.8.9 体系有; 空手/武器=true, 食物=false)
    LONG canUseItem = 1;
    if (r.canAttackWithItem) {
        canUseItem = env->CallBooleanMethod(player, r.canAttackWithItem) ? 1 : 0;
        if (env->ExceptionCheck()) { env->ExceptionClear(); g_status->lastError = 107; }
    }

    // 目标是否可被攻击 (现代版本替代 canAttackWithItem 的检查)
    LONG attackable = 1;
    if (r.isAttackable && entity) {
        attackable = env->CallBooleanMethod(entity, r.isAttackable) ? 1 : 0;
        if (env->ExceptionCheck()) { env->ExceptionClear(); g_status->lastError = 108; }
    }

    g_status->targetLiving    = living;
    g_status->targetAlive     = alive;
    g_status->targetIsPlayer  = isSelf;
    g_status->canAttack       = (hit == 2 && living && alive && !isSelf && canUseItem && attackable) ? 1 : 0;
    g_status->canPlace        = canPlace;
    g_status->heldItemNull    = heldNull;
    g_status->tick++;

    env->PopLocalFrame(NULL);
}

// JVMTI 终极方案: 枚举所有已加载的类, 找到"真 Minecraft 类"
// (A()/getMinecraft 返回非 null 的那份 —— 无论它由哪个加载器加载,
//  规避所有双份类副本问题)
//--------------------------------------------------------------------------
// 终极方案: ClassLoader.findLoadedClass(name) 直接拿"游戏已加载的类"
// (protected native 方法, JNI 可调用; 绕开 sources/委托/transform 所有问题)
static jclass FindLoadedGameClass(JNIEnv* env, jobject loader, const char* name,
                                  jclass clsCls, jmethodID forName,
                                  const char* getterName, const char* getterSig)
{
    if (!loader) return NULL;
    jclass loaderCls = env->FindClass("java/lang/ClassLoader");
    if (!loaderCls) { env->ExceptionClear(); return NULL; }
    jmethodID flc = env->GetMethodID(loaderCls, "findLoadedClass",
                                     "(Ljava/lang/String;)Ljava/lang/Class;");
    if (!flc) { env->ExceptionClear(); return NULL; }
    jstring nm = env->NewStringUTF(name);
    if (!nm) { env->ExceptionClear(); return NULL; }
    jclass c = (jclass)env->CallObjectMethod(loader, flc, nm);
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(nm);
    if (!c) return NULL;
    // 验证: getter 返回非 null (是游戏真类, theMinecraft 已初始化)
    if (getterName) {
        jmethodID m = env->GetStaticMethodID(c, getterName, getterSig);
        if (env->ExceptionCheck()) env->ExceptionClear();
        jobject inst = m ? env->CallStaticObjectMethod(c, m) : NULL;
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (!inst) return NULL;
        env->DeleteLocalRef(inst);
    }
    return c;
}

static jclass FindRealMinecraft(JNIEnv* env, JavaVM* vm,
                                const char* clsSig,     // 如 "Lave;"
                                const char* getterName, // 如 "A" (可为 NULL)
                                const char* getterSig)  // 如 "()Lave;"
{
    jvmtiEnv* jvmti = NULL;
    if (vm->GetEnv((void**)&jvmti, JVMTI_VERSION_1_2) != JNI_OK || !jvmti) return NULL;

    jint count = 0;
    jclass* classes = NULL;
    if (jvmti->GetLoadedClasses(&count, &classes) != JVMTI_ERROR_NONE) return NULL;

    // 候选 getter: 用户指定的优先, 再加常见别名
    const char* gNames[8];
    const char* gSigs[8];
    int gN = 0;
    if (getterName) { gNames[gN] = getterName; gSigs[gN] = getterSig; gN++; }
    const char* extraN[] = { "A", "func_71410_x", "getMinecraft", "getInstance" };
    const char* extraS[] = { "()Lave;", "()Lave;",
                             "()Lnet/minecraft/client/Minecraft;",
                             "()Lnet/minecraft/client/Minecraft;" };
    for (int i = 0; i < 4 && gN < 8; ++i) {
        bool dup = false;
        for (int j = 0; j < gN; ++j) if (strcmp(gNames[j], extraN[i]) == 0) { dup = true; break; }
        if (!dup) { gNames[gN] = extraN[i]; gSigs[gN] = extraS[i]; gN++; }
    }

    jclass found = NULL;
    for (jint i = 0; i < count && !found; ++i) {
        char* sig = NULL;
        if (jvmti->GetClassSignature(classes[i], &sig, NULL) != JVMTI_ERROR_NONE) continue;
        bool match = (clsSig && sig && strcmp(sig, clsSig) == 0);
        if (!match && clsSig == NULL) match = (sig != NULL); // 不匹配类名时全扫
        if (sig) jvmti->Deallocate((unsigned char*)sig);
        if (!match) continue;

        // 方法 1: 调用 getter 返回非 null -> 真类
        for (int k = 0; k < gN && !found; ++k) {
            jmethodID m = env->GetStaticMethodID(classes[i], gNames[k], gSigs[k]);
            if (env->ExceptionCheck()) env->ExceptionClear();
            if (!m) continue;
            jobject inst = env->CallStaticObjectMethod(classes[i], m);
            if (env->ExceptionCheck()) env->ExceptionClear();
            if (inst) {
                env->DeleteLocalRef(inst);
                found = classes[i];
                break;
            }
        }
        // 方法 2: 静态字段 S/theMinecraft 非 null -> 真类
        if (!found) {
            const char* fNames[] = { "S", "theMinecraft" };
            for (int k = 0; k < 2 && !found; ++k) {
                char fSig[128];
                snprintf(fSig, sizeof(fSig), "L%s;", clsSig ? clsSig + 1 : "ave");
                // 去掉尾部 ';'
                size_t fl = strlen(fSig);
                if (fl > 0 && fSig[fl-1] == ';') fSig[fl-1] = 0;
                jfieldID f = env->GetStaticFieldID(classes[i], fNames[k], fSig);
                if (env->ExceptionCheck()) env->ExceptionClear();
                if (!f) continue;
                jobject v = env->GetStaticObjectField(classes[i], f);
                if (env->ExceptionCheck()) env->ExceptionClear();
                if (v) {
                    env->DeleteLocalRef(v);
                    found = classes[i];
                    break;
                }
            }
        }
    }
    jvmti->Deallocate((unsigned char*)classes);
    return found;
}

//--------------------------------------------------------------------------
// 版本指纹探测: 用每版 vanilla 表的混淆 mcClass 快速定位版本 (原版环境)。
// Forge/FML/ModLauncher 运行时无混淆类名, 探测不到 -> 返回 0, 回退到全量尝试。
// 验证 getMinecraft() 返回非 null 才采纳, 排除"双份类副本"(classpath 残留)。
//--------------------------------------------------------------------------
static int DetectVersionHint(JNIEnv* env, jobject loader, jclass clsCls, jmethodID forName,
                             DWORD deadline)
{
    for (int i = 0; i < kGenMapCount; ++i) {
        if (GetTickCount() > deadline) break;   // 帧驱动: 预算用完放弃, 回退全量尝试
        if (strncmp(kGenMaps[i].name, "vanilla", 7) != 0) continue;
        jclass c = LoadClass(env, kGenMaps[i].mcClass, loader, clsCls, forName);
        if (!c) { env->ExceptionClear(); continue; }
        jmethodID gm = env->GetStaticMethodID(c, kGenMaps[i].getMinecraft, kGenMaps[i].mcSig);
        if (env->ExceptionCheck()) env->ExceptionClear();
        jobject inst = gm ? env->CallStaticObjectMethod(c, gm) : NULL;
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (inst) {
            env->DeleteLocalRef(inst);
            env->DeleteLocalRef(c);
            return i;  // 命中: 返回该版 vanilla 表的索引
        }
        env->DeleteLocalRef(c);
    }
    return 0;  // 未命中 (Forge 或游戏类尚未加载)
}

//--------------------------------------------------------------------------
// 版本检测 (JVM classpath): 从 System.getProperty 读 classpath / library.path,
// 提取 Minecraft 版本号。不读窗口标题 (标题栏可被资源包/语言改写, 不可靠)。
// 游戏 jar 路径形如:
//   ...\versions\1.21.11\1.21.11.jar           (原版)
//   ...\versions\1.16.5-Forge_36.2.34\...      (Forge)
//   ...\versions\1.21.11-Fabric 0.18.4\...     (Fabric)
//--------------------------------------------------------------------------
static int ExtractMcVersion(const char* text, char* out, size_t cap)
{
    out[0] = 0;
    if (!text) return 0;
    const char* start = NULL;
    // 优先: "versions" 目录后的版本号 (最可靠, 各启动器通用)
    const char* p = text;
    while ((p = strstr(p, "versions")) != NULL) {
        const char* s = p + 8;
        while (*s == '\\' || *s == '/' || *s == ' ' || *s == '"') s++;
        if (s[0] == '1' && s[1] == '.') { start = s; break; }
        p += 8;
    }
    // 回退: 路径组件开头就是 "1." 的版本号
    if (!start) {
        p = text;
        while ((p = strstr(p, "1.")) != NULL) {
            const char* q = p + 2;
            if (*q >= '0' && *q <= '9' &&
                (p == text || p[-1] == ';' || p[-1] == '\\' || p[-1] == '/' ||
                 p[-1] == ' ' || p[-1] == '=' || p[-1] == ':')) {
                start = p;
                break;
            }
            p = q;
        }
    }
    if (!start) return 0;
    size_t n = 0;
    const char* q = start;
    while (*q && ((*q >= '0' && *q <= '9') || *q == '.') && n < cap - 1) {
        out[n++] = *q++;
    }
    while (n > 0 && out[n - 1] == '.') out[--n] = 0;  // 去尾点
    out[n] = 0;
    return n > 0;
}

static void GetGameVersion(JNIEnv* env, char* out, size_t cap)
{
    out[0] = 0;
    if (!env) return;
    jclass sysCls = env->FindClass("java/lang/System");
    if (!sysCls) { env->ExceptionClear(); return; }
    jmethodID getProp = env->GetStaticMethodID(sysCls, "getProperty",
                                               "(Ljava/lang/String;)Ljava/lang/String;");
    if (!getProp) { env->ExceptionClear(); return; }
    const char* props[] = { "java.class.path", "java.library.path" };
    for (int i = 0; i < 2 && out[0] == 0; ++i) {
        jstring key = env->NewStringUTF(props[i]);
        if (!key) { env->ExceptionClear(); continue; }
        jstring val = (jstring)env->CallStaticObjectMethod(sysCls, getProp, key);
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->DeleteLocalRef(key);
        if (!val) continue;
        const char* utf = env->GetStringUTFChars(val, NULL);
        if (utf) {
            ExtractMcVersion(utf, out, cap);
            env->ReleaseStringUTFChars(val, utf);
        }
        env->DeleteLocalRef(val);
    }
}

// 版本串 "1.21.11" -> 映射表起点索引 (名字末尾完整数字段 == "12111" 的第一张表);
// -1 = 未找到。必须匹配"末尾完整数字段", 否则 "121"(1.21) 会误匹配 "1121"(1.12.1)。
static int FindVersionMapIndex(const char* version)
{
    char tag[32];
    size_t n = 0;
    for (const char* p = version; *p && n < sizeof(tag) - 1; ++p) {
        if (*p >= '0' && *p <= '9') tag[n++] = *p;
    }
    tag[n] = 0;
    if (n == 0) return -1;
    for (int i = 0; i < kGenMapCount; ++i) {
        const char* name = kGenMaps[i].name;
        size_t nameLen = strlen(name);
        // 名字末尾的连续数字段长度必须 == 版本 tag 长度 (精确匹配, 防后缀撞名)
        size_t digits = 0;
        while (digits < nameLen && name[nameLen - 1 - digits] >= '0' &&
               name[nameLen - 1 - digits] <= '9') {
            digits++;
        }
        if (digits == n && strncmp(name + nameLen - n, tag, n) == 0) {
            return i;
        }
    }
    return -1;
}

//--------------------------------------------------------------------------
// 帧驱动状态机 (V65) —— 不再创建线程、不再 AttachCurrentThread。
//
// 旧实现 (V64-): DllMain 里 CreateThread 采集线程 + vm->AttachCurrentThread。
// AttachCurrentThread 会在 JVM 里注册一个"外来原生线程"并触发 JVM 的
// ThreadStart 事件; 游戏侧保护 (如网易中国版) 发现"非游戏创建的线程附加
// 进了 JVM"会直接退出游戏 —— 这个组合就是被强杀的导火索。
//
// V65 方案:
//   * DllMain 不再 CreateThread; 只做共享内存/UDP 初始化 + 安装钩子。
//   * 内联钩住 gdi32!SwapBuffers —— 游戏自己的 Client thread (Java 线程)
//     每帧渲染都会调用它 (LWJGL2 的 WindowsDisplay 与 LWJGL3/GLFW 的
//     WGL 后端最终都调 gdi32!SwapBuffers)。
//   * 钩子内用 GetEnv() 复用该线程已有的 JNIEnv: Java 线程天然 attached,
//     GetEnv 直接返回其环境, 无需 (也绝不) AttachCurrentThread, 不触发
//     ThreadStart 事件。解析/采样/上报全部在该线程内分帧完成。
//
// 帧驱动下的关键改造点:
//   1. 跨帧保留的 jclass/jobject 一律 NewGlobalRef 提升为全局引用 ——
//      本地引用在 native 帧返回时即失效 (旧实现靠"采集线程永不返回"规避)。
//   2. 每帧工作预算 kFrameBudgetMs (8ms), 解析失败下帧续跑, 绝不
//      Sleep/阻塞渲染线程; 一次性启动期探测允许较大预算。
//   3. 离开钩子前必须清空 pending exception, 否则异常会带回游戏。
//   4. 安装钩子打补丁前挂起其他线程, 防止游戏渲染线程执行到撕裂指令。
//
// 协议不变: 共享内存 Local\MCCombatStatus_<pid> (CombatStatus, magic
// 'MCST' v7) + UDP 35785 2 字节 [canAttack][canPlace]。外部程序无需改动。
//--------------------------------------------------------------------------

// JNI 环境只通过 GetEnv 从已 attached 的线程获取, 绝不 Attach。
typedef jint (JNICALL* GetCreatedVMs_t)(JavaVM**, jsize, jsize*);

// ============================ 帧驱动全局状态 ============================
static JavaVM* g_vm = NULL;

// ---- SwapBuffers 钩子 ----
typedef BOOL (WINAPI* SwapBuffersFn)(HDC);
static SwapBuffersFn g_origSwapBuffers = NULL;
static BYTE   g_patch[32];
static DWORD  g_patchLen = 0;
static LONG   g_attached = 0;      // DllMain 幂等 (防重复 LoadLibrary 二次钩)

// ---- 解析状态机 (只在渲染线程执行, 无需锁) ----
enum {
    ST_CLS = 0,        // java/lang/Class + forName
    ST_ENV,            // 环境探测 (forge/optifine/fabric/...)
    ST_VER,            // classpath 版本号提取
    ST_LAUNCH,         // Launch.classLoader
    ST_LAUNCH_V,       // 验证 launch loader 能加载游戏类 (分帧)
    ST_SCAN,           // 遍历线程找游戏类加载器 (分帧可续)
    ST_SYS,            // 系统类加载器 + 版本指纹 + loaderName
    ST_MAPS,           // 尝试映射表 (分帧)
    ST_FIX,            // findLoadedClass 终极修正 (一次性)
    ST_STEADY          // 采样 + 上报 (5ms 节流)
};
static int      g_stage     = ST_CLS;
static int      g_probeIdx  = 0;   // 探测进度 (launch 验证 / 线程后备探测共用)
static int      g_mapIdx    = 0;   // 映射表进度
static int      g_mapStart  = 0;   // 本轮全表尝试的起点 (环境感知, wrap 后回到这里)
static bool     g_mapWrap   = false;// 全表轮完, 等待重试
static DWORD    g_retryAt   = 0;   // 全表重试时间点
static DWORD    g_scanStart = 0;   // 线程遍历总超时起点
static DWORD    g_nullSince = 0;   // mcNull 连续时间起点
static DWORD    g_lastWork  = 0;   // 上次采样/上报时刻 (5ms 节流)

static Resolved   g_res;           // 解析成功结果 (jclass 全部为全局引用)
static const JniMap* g_resMap = NULL; // g_res 对应的映射表
static jclass    g_clsCls      = NULL; // java/lang/Class (全局)
static jmethodID g_forName     = NULL;
static jobject   g_launchLoader = NULL; // 全局
static jobject   g_gameLoader   = NULL; // 全局
static jobject   g_sysLoader    = NULL; // 全局
static bool      g_useGameLoader = false;
static int       g_verHint = -2;     // -2=未提取
static char      g_gameVer[32];

// ---- 线程遍历可续状态 ----
static jobject   g_scanIt = NULL;      // Iterator (全局)
static jclass    g_scanThreadCls = NULL; // java/lang/Thread (全局)
static jmethodID g_scanHasNext = NULL, g_scanNext = NULL, g_scanGetCtx = NULL;
static jmethodID g_scanGetAll = NULL;

static const DWORD kFrameBudgetMs      = 8;     // 每帧最多占用渲染线程 8ms
static const DWORD kLaunchVerifyBudgetMs = 300; // 启动期一次性验证预算
static const DWORD kScanTimeoutMs      = 10000; // 线程遍历总上限 (对齐旧版)
static const DWORD kResolveRetryMs     = 500;   // 全表轮完后的重试间隔
static const DWORD kSamplePeriodMs     = 5;     // 采样/上报节流 (对齐旧版 5ms)

//--------------------------------------------------------------------------
// 本地引用 -> 全局引用 (帧驱动下跨帧保留对象的唯一安全方式)
//--------------------------------------------------------------------------
static jobject ToGlobal(JNIEnv* env, jobject o)
{
    if (!o) return NULL;
    jobject g = env->NewGlobalRef(o);
    env->DeleteLocalRef(o);
    return g;
}

// Resolved 内的 jclass 本地引用 -> 全局引用; 任一失败则回滚并返回 false。
static bool PromoteResolved(JNIEnv* env, Resolved& r)
{
    jclass* c[] = { &r.mcClass, &r.mopClass, &r.typeClass, &r.entityClass,
                    &r.livingClass, &r.entityHitCls, &r.javaLangClass,
                    &r.itemStackCls, &r.itemBlockCls };
    for (size_t i = 0; i < sizeof(c)/sizeof(c[0]); ++i) {
        if (!*c[i]) continue;
        jobject g = env->NewGlobalRef(*c[i]);
        if (!g) {
            // 极端 OOM: 回滚已提升的引用, 调用方放弃本轮
            for (size_t j = 0; j < i; ++j) {
                if (*c[j]) { env->DeleteGlobalRef(*c[j]); *c[j] = NULL; }
            }
            return false;
        }
        env->DeleteLocalRef(*c[i]);
        *c[i] = (jclass)g;
    }
    return true;
}

// 释放 Resolved 里的全局引用并清零。
static void FreeResolved(JNIEnv* env, Resolved& r)
{
    jclass* c[] = { &r.mcClass, &r.mopClass, &r.typeClass, &r.entityClass,
                    &r.livingClass, &r.entityHitCls, &r.javaLangClass,
                    &r.itemStackCls, &r.itemBlockCls };
    for (size_t i = 0; i < sizeof(c)/sizeof(c[0]); ++i) {
        if (*c[i]) { env->DeleteGlobalRef(*c[i]); *c[i] = NULL; }
    }
    memset(&r, 0, sizeof(r));
}

// 放弃线程遍历, 释放可续状态
static void ScanAbort(JNIEnv* env)
{
    if (g_scanIt)        { env->DeleteGlobalRef(g_scanIt);        g_scanIt = NULL; }
    if (g_scanThreadCls) { env->DeleteGlobalRef(g_scanThreadCls); g_scanThreadCls = NULL; }
    g_scanHasNext = g_scanNext = g_scanGetCtx = g_scanGetAll = NULL;
}

//--------------------------------------------------------------------------
// 遍历所有 Java 线程找一个能加载游戏类的 context class loader
// (ModLauncher/Fabric 环境: 主线程的 TransformingClassLoader/KnotClassLoader)。
// 帧驱动可续: Iterator 存为全局引用, 每帧在预算内推进; 预算用完下帧继续。
// 返回: 1=已找到 (g_gameLoader 已设) / 0=遍历完未找到 / -1=本帧预算用完
//--------------------------------------------------------------------------
static int ThreadScanStep(JNIEnv* env, DWORD now, DWORD deadline)
{
    if (now - g_scanStart > kScanTimeoutMs) { ScanAbort(env); return 0; }

    if (!g_scanIt) {
        if (g_status) CopyName(g_status->errMsg, sizeof(g_status->errMsg), "T0:start");
        g_scanThreadCls = (jclass)ToGlobal(env, env->FindClass("java/lang/Thread"));
        if (!g_scanThreadCls) { env->ExceptionClear(); return -1; }
        g_scanGetAll = env->GetStaticMethodID(g_scanThreadCls, "getAllStackTraces",
                                              "()Ljava/util/Map;");
        jclass mapCls = env->FindClass("java/util/Map");
        jclass setCls = env->FindClass("java/util/Set");
        jclass itCls  = env->FindClass("java/util/Iterator");
        if (!g_scanGetAll || !mapCls || !setCls || !itCls) {
            env->ExceptionClear(); ScanAbort(env); return 0;
        }
        jmethodID keySet   = env->GetMethodID(mapCls, "keySet", "()Ljava/util/Set;");
        jmethodID iterator = env->GetMethodID(setCls, "iterator", "()Ljava/util/Iterator;");
        g_scanHasNext = env->GetMethodID(itCls, "hasNext", "()Z");
        g_scanNext    = env->GetMethodID(itCls, "next", "()Ljava/lang/Object;");
        g_scanGetCtx  = env->GetMethodID(g_scanThreadCls, "getContextClassLoader",
                                         "()Ljava/lang/ClassLoader;");
        if (!keySet || !iterator || !g_scanHasNext || !g_scanNext || !g_scanGetCtx) {
            env->ExceptionClear(); ScanAbort(env); return 0;
        }
        if (g_status) CopyName(g_status->errMsg, sizeof(g_status->errMsg), "T1:threadCls");
        jobject map = env->CallStaticObjectMethod(g_scanThreadCls, g_scanGetAll);
        if (env->ExceptionCheck() || !map) { env->ExceptionClear(); ScanAbort(env); return 0; }
        jobject set = env->CallObjectMethod(map, keySet);
        jobject it  = set ? env->CallObjectMethod(set, iterator) : NULL;
        if (env->ExceptionCheck() || !it) { env->ExceptionClear(); ScanAbort(env); return 0; }
        g_scanIt = ToGlobal(env, it);
        if (!g_scanIt) return -1;
        g_probeIdx = 0;
        if (g_status) CopyName(g_status->errMsg, sizeof(g_status->errMsg), "T4:iterator");
    }

    while (GetTickCount() < deadline) {
        if (!env->CallBooleanMethod(g_scanIt, g_scanHasNext)) {
            if (env->ExceptionCheck()) env->ExceptionClear();
            ScanAbort(env);
            return 0;   // 遍历完: 未找到
        }
        jobject thread = env->CallObjectMethod(g_scanIt, g_scanNext);
        if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
        if (!thread) continue;
        jobject loader = env->CallObjectMethod(thread, g_scanGetCtx);
        if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
        if (!loader) continue;
        // 首选: 能加载标准 Minecraft 类
        jclass mcProbe = LoadClass(env, "net/minecraft/client/Minecraft",
                                   loader, g_clsCls, g_forName);
        if (mcProbe) {
            env->DeleteLocalRef(mcProbe);
            g_gameLoader = ToGlobal(env, loader);
            g_useGameLoader = true;
            ScanAbort(env);
            return 1;
        }
        // 后备: 能加载其他 mcClass 候选 (分帧, 进度存 g_probeIdx)
        while (g_probeIdx < kGenMapCount) {
            if (GetTickCount() >= deadline) return -1;   // 下帧继续探测当前 loader
            if (strcmp(kGenMaps[g_probeIdx].mcClass, "net/minecraft/client/Minecraft") == 0) {
                g_probeIdx++;   // 首选已测过
                continue;
            }
            jclass probe = LoadClass(env, kGenMaps[g_probeIdx].mcClass,
                                     loader, g_clsCls, g_forName);
            if (probe) {
                env->DeleteLocalRef(probe);
                g_gameLoader = ToGlobal(env, loader);
                g_useGameLoader = true;
                ScanAbort(env);
                return 1;
            }
            g_probeIdx++;
        }
        g_probeIdx = 0; // 该 loader 全部候选测完, 换下一个线程
    }
    return -1;
}

//--------------------------------------------------------------------------
// 按命名空间前缀 + 版本数字 tag 找映射表索引 (找不到返回 -1)。
// 环境感知优化用: 探测到 neoforge/forge/fabric 后直接定位对应命名空间,
// 跳过逐张失败的全表轮询 (NeoForge 的 CCNFE 异常路径极慢, 全表要几十秒)。
//--------------------------------------------------------------------------
static int FindMapByNamespace(const char* ns, const char* version)
{
    char tag[32];
    size_t n = 0;
    for (const char* p = version; *p && n < sizeof(tag) - 1; ++p)
        if (*p >= '0' && *p <= '9') tag[n++] = *p;
    tag[n] = 0;
    if (n == 0) return -1;
    size_t nsLen = strlen(ns);
    for (int i = 0; i < kGenMapCount; ++i) {
        const char* name = kGenMaps[i].name;
        if (strncmp(name, ns, nsLen) != 0) continue;
        size_t nameLen = strlen(name);
        size_t digits = 0;
        while (digits < nameLen && name[nameLen - 1 - digits] >= '0' &&
               name[nameLen - 1 - digits] <= '9') digits++;
        if (digits == n && strncmp(name + nameLen - n, tag, n) == 0) return i;
    }
    return -1;
}

//--------------------------------------------------------------------------
// 每帧泵: 解析状态机 + 采样上报 (时间预算化, 绝不阻塞渲染线程)
//--------------------------------------------------------------------------
static void PumpInner(JNIEnv* env, DWORD now)
{
    if (!g_status) return;

    switch (g_stage) {
    // ---- ST_CLS: java/lang/Class + forName ----
    case ST_CLS: {
        jclass c = env->FindClass("java/lang/Class");
        if (!c) { env->ExceptionClear(); return; }
        g_clsCls = (jclass)env->NewGlobalRef(c);
        env->DeleteLocalRef(c);
        if (!g_clsCls) return;
        g_forName = env->GetStaticMethodID(g_clsCls, "forName",
            "(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;");
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (!g_forName) return;
        CopyName(g_status->errMsg, sizeof(g_status->errMsg), "H1:cls-ok");
        g_stage = ST_ENV;
    } /* fallthrough: 同帧继续 */
    // ---- ST_ENV: 环境探测 ----
    case ST_ENV: {
#ifndef NO_ENV_DETECT
        DetectEnv(env, NULL, g_clsCls, g_forName,
                  g_status->envName, sizeof(g_status->envName));
#endif
        CopyName(g_status->errMsg, sizeof(g_status->errMsg), "H2:env-ok");
        g_stage = ST_VER;
    } /* fallthrough: 同帧继续 */
    // ---- ST_VER: classpath 版本号提取 ----
    case ST_VER: {
        GetGameVersion(env, g_gameVer, sizeof(g_gameVer));
        g_verHint = g_gameVer[0] ? FindVersionMapIndex(g_gameVer) : -1;
        if (g_gameVer[0]) {
            size_t el = strlen(g_status->envName);
            if (el + 1 + strlen(g_gameVer) < sizeof(g_status->envName)) {
                if (el) g_status->envName[el++] = '+';
                strcpy(g_status->envName + el, g_gameVer);
            }
        }
        CopyName(g_status->errMsg, sizeof(g_status->errMsg), "H3:ver-ok");
        g_stage = ST_LAUNCH;
    } /* fallthrough: 同帧继续 */
    // ---- ST_LAUNCH: Launch.classLoader ----
    case ST_LAUNCH: {
        g_launchLoader = ToGlobal(env, FindLaunchClassLoader(env, g_clsCls, g_forName));
        if (g_launchLoader) {
            g_probeIdx = 0;
            g_stage = ST_LAUNCH_V;
        } else {
            g_scanStart = now;
            g_stage = ST_SCAN;
        }
        return;
    }
    // ---- ST_LAUNCH_V: 验证 launch loader 能加载游戏类 ----
    // 一次性预算较大: 只在启动期跑一次, 全部失败才放弃 (OptiFine 下它只含库)
    case ST_LAUNCH_V: {
        DWORD deadline = now + kLaunchVerifyBudgetMs;
        while (g_probeIdx < kGenMapCount) {
            if (GetTickCount() > deadline) return;
            jclass probe = LoadClass(env, kGenMaps[g_probeIdx].mcClass,
                                     g_launchLoader, g_clsCls, g_forName);
            if (probe) {
                env->DeleteLocalRef(probe);
                g_gameLoader = env->NewGlobalRef(g_launchLoader);
                g_useGameLoader = true;
                CopyName(g_status->loaderName, sizeof(g_status->loaderName),
                         "launch-loader");
                g_stage = ST_SYS;
                return;
            }
            g_probeIdx++;
        }
        CopyName(g_status->errMsg, sizeof(g_status->errMsg),
                 "E4:launch-loader-cant-load-game");
        g_scanStart = now;
        g_stage = ST_SCAN;
        return;
    }
    // ---- ST_SCAN: 线程遍历找游戏类加载器 (分帧可续) ----
    case ST_SCAN: {
        DWORD deadline = now + kFrameBudgetMs;
        int rc = ThreadScanStep(env, now, deadline);
        if (rc == 1) {
            CopyName(g_status->loaderName, sizeof(g_status->loaderName),
                     "thread-loader");
            g_stage = ST_SYS;
        } else if (rc == 0) {
            g_stage = ST_SYS;   // 未找到: 用系统类加载器
        }
        // rc == -1: 预算用完, 下帧继续
        return;
    }
    // ---- ST_SYS: 系统类加载器 + 版本指纹 + loaderName ----
    case ST_SYS: {
        jclass clCls = env->FindClass("java/lang/ClassLoader");
        jmethodID getSys = clCls ? env->GetStaticMethodID(clCls, "getSystemClassLoader",
                                                          "()Ljava/lang/ClassLoader;") : NULL;
        jobject sys = getSys ? env->CallStaticObjectMethod(clCls, getSys) : NULL;
        if (env->ExceptionCheck()) env->ExceptionClear();
        g_sysLoader = ToGlobal(env, sys);
        if (!g_gameLoader) {
            CopyName(g_status->loaderName, sizeof(g_status->loaderName),
                     g_sysLoader ? "system-loader" : "(null - 使用 FindClass)");
        }
        // 版本指纹: classpath 提取失败时, 用原版混淆 mcClass 指纹扫一遍
        int start = (g_verHint >= 0) ? g_verHint
                  : DetectVersionHint(env, g_useGameLoader ? g_gameLoader : g_sysLoader,
                                      g_clsCls, g_forName, now + kLaunchVerifyBudgetMs);
        // 环境感知优化: 按探测到的 mod 加载器直接定位命名空间表,
        // 跳过逐张失败的全表轮询 (NeoForge 的 CCNFE 异常路径极慢)。
        // 解析失败仍会向后轮询 + wrap 全表, 回退逻辑不变。
        if (g_verHint >= 0 && g_status->envName[0]) {
            const char* ns = NULL;
            if (strstr(g_status->envName, "neoforge")) ns = "mojang";          // NeoForge 1.20.2+: 全 Mojang
            else if (strstr(g_status->envName, "fabric")) ns = "intermediary"; // Fabric: intermediary
            else if (strstr(g_status->envName, "forge")) ns = "forge";         // Forge: MCP+SRG / Mojang+stable
            if (ns) {
                int hint2 = FindMapByNamespace(ns, g_gameVer);
                if (hint2 >= 0) start = hint2;
            }
        }
        g_mapStart = start;
        g_mapIdx = start;
        g_mapWrap = false;
        g_status->failLog[0] = 0;
        CopyName(g_status->errMsg, sizeof(g_status->errMsg), "H4:loader-ok");
        g_stage = ST_MAPS;
    } /* fallthrough: 同帧开始尝试映射表 */
    // ---- ST_MAPS: 尝试映射表 (分帧) ----
    case ST_MAPS: {
        if (g_mapWrap) {
            if (now < g_retryAt) return;
            g_mapWrap = false;
            g_mapIdx = g_mapStart;
            g_status->failLog[0] = 0;
        }
        DWORD deadline = now + kFrameBudgetMs;
        for (;;) {
            if (GetTickCount() >= deadline) return;
            const JniMap& m = kGenMaps[g_mapIdx];
            jobject loader = g_useGameLoader ? g_gameLoader : g_sysLoader;
            CopyName(g_status->errMsg, sizeof(g_status->errMsg), m.name);
            env->ExceptionClear();
            Resolved tr;
            bool ok = ResolveWith(env, m, tr, loader, g_clsCls, g_forName);
            if (ok) {
                // 终极验证: getMinecraft() 必须返回真实例, 排除双份类副本
                jobject inst = env->CallStaticObjectMethod(tr.mcClass, tr.getMinecraft);
                bool hasInst = (inst != NULL);
                if (env->ExceptionCheck()) env->ExceptionClear();
                if (inst) env->DeleteLocalRef(inst);
                if (hasInst) {
                    if (PromoteResolved(env, tr)) {
                        g_resMap = &m;
                        FreeResolved(env, g_res);
                        g_res = tr;
                        g_stage = ST_FIX;
                        return;
                    }
                    return; // 极端 OOM: 放弃本轮, 下帧同表重试
                }
                NoteErr(m.name, "getMinecraft()=null(副本)");
                ok = false;
            }
            // 当前加载器失败/副本时, 同表换另一个加载器再试 (TCL 与 app loader 都覆盖)
            if (!ok && g_gameLoader && g_sysLoader && g_gameLoader != g_sysLoader) {
                jobject loader2 = (loader == g_gameLoader) ? g_sysLoader : g_gameLoader;
                Resolved tr2;
                bool ok2 = ResolveWith(env, m, tr2, loader2, g_clsCls, g_forName);
                if (ok2) {
                    jobject inst2 = env->CallStaticObjectMethod(tr2.mcClass, tr2.getMinecraft);
                    bool hasInst2 = (inst2 != NULL);
                    if (env->ExceptionCheck()) env->ExceptionClear();
                    if (inst2) env->DeleteLocalRef(inst2);
                    if (hasInst2) {
                        if (PromoteResolved(env, tr2)) {
                            g_resMap = &m;
                            FreeResolved(env, g_res);
                            g_res = tr2;
                            g_useGameLoader = (loader2 == g_gameLoader);
                            g_stage = ST_FIX;
                            return;
                        }
                        return; // 极端 OOM: 放弃本轮, 下帧同表重试
                    }
                }
            }
            g_mapIdx++;
            if (g_mapIdx >= kGenMapCount) {
                g_mapIdx = 0;
                g_mapWrap = true;
                g_retryAt = now + kResolveRetryMs;
                return;
            }
        }
    }
    // ---- ST_FIX: findLoadedClass 终极修正 (一次性, 几个 JNI 调用无需分帧) ----
    case ST_FIX: {
        const JniMap& mOk = *g_resMap;
#ifndef NO_REAL_FIX
        // 终极修正 A: findLoadedClass 拿游戏已加载的真类, 用它的类加载器重新解析
        jclass realMc = FindLoadedGameClass(env, g_launchLoader, mOk.mcClass,
                                            g_clsCls, g_forName,
                                            mOk.getMinecraft, mOk.mcSig);
        if (realMc) {
            jmethodID getLdr = env->GetMethodID(g_clsCls, "getClassLoader",
                                                "()Ljava/lang/ClassLoader;");
            jobject realLoader = getLdr ? env->CallObjectMethod(realMc, getLdr) : NULL;
            if (env->ExceptionCheck()) env->ExceptionClear();
            if (realLoader) {
                Resolved resReal;
                bool okReal = ResolveWith(env, mOk, resReal, realLoader, g_clsCls, g_forName);
                if (okReal && PromoteResolved(env, resReal)) {
                    FreeResolved(env, g_res);
                    g_res = resReal;
                    CopyName(g_status->loaderName, sizeof(g_status->loaderName),
                             "findLoadedClass-real-loader");
                }
                env->DeleteLocalRef(realLoader);
            }
            env->DeleteLocalRef(realMc);
        }
#endif
        g_status->ready = 1;
        CopyName(g_status->mappingName, sizeof(g_status->mappingName), g_res.name);
        CopyName(g_status->errMsg, sizeof(g_status->errMsg), "H5:ready");
        g_stage = ST_STEADY;
        return;
    }
    // ---- ST_STEADY: 采样 + 上报 (5ms 节流) ----
    case ST_STEADY: {
        if (g_status->mcNull) {
            if (!g_nullSince) g_nullSince = now;
            else if (now - g_nullSince >= 500) {
                // 连续 0.5 秒拿不到 mc (双份类副本): 切换加载器重新解析
                g_nullSince = now;
                if (g_gameLoader && g_sysLoader) g_useGameLoader = !g_useGameLoader;
                else if (g_gameLoader) g_useGameLoader = true;
                FreeResolved(env, g_res);
                g_resMap = NULL;
                g_status->ready = 0;
                CopyName(g_status->mappingName, sizeof(g_status->mappingName), NULL);
                g_mapIdx = g_mapStart;
                g_mapWrap = false;
                g_stage = ST_MAPS;
                return;
            }
        } else {
            g_nullSince = 0;
        }
        if (now - g_lastWork >= kSamplePeriodMs) {
            g_lastWork = now;
            UpdateStatus(env, g_res);
            // V68: UDP 已移除 (游戏进程内不再创建任何 socket), 仅共享内存通道
        }
        return;
    }
    }
}

// 每帧入口: 本地引用帧保护 + 异常兜底 (绝不让异常带回游戏)
static void PumpFrame(JNIEnv* env)
{
    if (env->PushLocalFrame(512) < 0) return;
    PumpInner(env, GetTickCount());
    env->PopLocalFrame(NULL);
    if (env->ExceptionCheck()) env->ExceptionClear();
}

//--------------------------------------------------------------------------
// gdi32!SwapBuffers 钩子: 在游戏渲染线程内复用其 JNIEnv 完成全部检测。
//--------------------------------------------------------------------------
static void ReArmGuard(void);   // 前向声明 (定义在 VEH 钩子安装段)
static BOOL WINAPI HookSwapBuffers(HDC hdc)
{
    // 惰性获取 JavaVM (JNI_GetCreatedJavaVMs 不附着线程, 不触发 ThreadStart)
    if (!g_vm) {
        HMODULE jvm = GetModuleHandleA(XS(kEnc_jvm_dll, sizeof(kEnc_jvm_dll)));
        if (jvm) {
            GetCreatedVMs_t getVMs =
                (GetCreatedVMs_t)(void*)GetProcAddress(jvm,
                    XS(kEnc_JNI_GetCreatedJavaVMs, sizeof(kEnc_JNI_GetCreatedJavaVMs)));
            if (getVMs) {
                JavaVM* v = NULL;
                jsize   n = 0;
                if (getVMs(&v, 1, &n) == JNI_OK && n >= 1 && v) g_vm = v;
            }
        }
    }
    if (g_vm) {
        JNIEnv* env = NULL;
        // 复用调用线程已有的 JNIEnv (Client thread 是游戏自建的 Java 线程,
        // 天然 attached)。绝不 AttachCurrentThread —— 非 Java 线程调用时
        // GetEnv 返回 EDETACHED, 直接跳过本帧。
        jint rc = g_vm->GetEnv((void**)&env, JNI_VERSION_1_6);
        if (rc == JNI_OK && env)
            PumpFrame(env);
        else if (g_status && g_stage == ST_CLS) {
            // 诊断 (仅解析前覆盖, 不干扰后续阶段信息)
            snprintf(g_status->errMsg, sizeof(g_status->errMsg), "HG:getenv=%d", (int)rc);
        }
    } else if (g_status && g_stage == ST_CLS) {
        CopyName(g_status->errMsg, sizeof(g_status->errMsg), "HV:no-vm");
    }
    BOOL r = g_origSwapBuffers ? g_origSwapBuffers(hdc) : FALSE;
    // V68 VEH 钩子: guard 已在重定向故障时被硬件清除, 此处执行点在本模块内,
    // 重新武装保护页供下一帧拦截 (详见 InstallSwapBuffersHook)。
    ReArmGuard();
    return r;
}

//--------------------------------------------------------------------------
// 内联钩子 (x64): 5 字节 rel32 jmp (或 12 字节 mov rax,imm64;jmp rax) +
// 附近分配的 trampoline (原指令 + jmp back)。
//--------------------------------------------------------------------------
static bool FitsRel32(void* from, void* to)
{
    INT64 d = (INT64)((BYTE*)to - (BYTE*)from);
    return d >= -0x80000000LL && d <= 0x7FFFFFFFLL;
}

static bool WriteRelJmp(BYTE* dst, void* to)
{
    if (!FitsRel32(dst, to)) return false;
    dst[0] = 0xE9;
    INT32 off = (INT32)((BYTE*)to - (dst + 5));
    memcpy(dst + 1, &off, 4);
    return true;
}

static void WriteAbsJmp(BYTE* dst, void* to)
{
    dst[0] = 0x48; dst[1] = 0xB8;               // mov rax, imm64
    memcpy(dst + 2, &to, 8);
    dst[10] = 0xFF; dst[11] = 0xE0;             // jmp rax
}

// modrm 长度 (modrm 位于 p[i]); rm==4 时任何 mod 下都存在 SIB 字节
static int ModRmLen(const BYTE* p, int i)
{
    BYTE modrm = p[i];
    int mod = modrm >> 6, rm = modrm & 7;
    int len = 1;
    if (rm == 4) {                                   // SIB
        len += 1;
        if (mod == 0 && (p[i + 1] & 7) == 5) len += 4;
    }
    if (mod == 0 && rm == 5)      len += 4;          // disp32 (无 SIB)
    else if (mod == 1)            len += 1;          // disp8
    else if (mod == 2)            len += 4;          // disp32
    return len;
}

// 最小 x86-64 长度反汇编器: 只覆盖 Windows 函数序言常见指令。
// 返回指令字节数, 无法识别返回 0。
static int InsnLen64(const BYTE* p)
{
    int i = 0;
    bool rexW = false;
    while (i < 15) {
        BYTE b = p[i];
        if (b >= 0x40 && b <= 0x4F) { rexW = (b & 8) != 0; i++; continue; }
        if (b == 0x66 || b == 0x67 || b == 0xF0 || b == 0xF2 || b == 0xF3) { i++; continue; }
        break;
    }
    if (i >= 15) return 0;
    BYTE op = p[i];
    if (op == 0x0F) {
        BYTE op2 = p[i + 1];
        if (op2 >= 0x80 && op2 <= 0x8F) return i + 6;                 // jcc rel32
        if (op2 == 0x1E || op2 == 0x1F) return i + 2 + ModRmLen(p, i + 2); // nop (含 endbr64)
        if (op2 == 0x05 || op2 == 0x34) return i + 2;                 // syscall/sysenter
        return 0;
    }
    if (op >= 0x50 && op <= 0x5F) return i + 1;                       // push/pop r64
    if (op >= 0x70 && op <= 0x7F) return i + 2;                       // jcc rel8
    if (op == 0xEB) return i + 2;                                     // jmp rel8
    if (op == 0xE9) return i + 5;                                     // jmp rel32
    if (op == 0xE8) return i + 5;                                     // call rel32
    if (op >= 0xB8 && op <= 0xBF) return i + 1 + (rexW ? 8 : 4);      // mov r, imm
    if (op == 0x68) return i + 5;                                     // push imm32
    if (op == 0x6A) return i + 2;                                     // push imm8
    if (op == 0x80) return i + 2 + ModRmLen(p, i + 1) + 1;            // group1 r/m8, imm8
    if (op == 0x81) return i + 2 + ModRmLen(p, i + 1) + 4;            // group1 r/m, imm32
    if (op == 0x83) return i + 2 + ModRmLen(p, i + 1) + 1;            // group1 r/m, imm8
    if (op == 0xC7) return i + 2 + ModRmLen(p, i + 1) + 4;            // mov r/m, imm32
    if (op == 0x89 || op == 0x8B || op == 0x8D || op == 0x03 || op == 0x0B ||
        op == 0x2B || op == 0x33 || op == 0x3B || op == 0x01 || op == 0x09 ||
        op == 0x85 || op == 0x39 || op == 0x31 || op == 0x29 || op == 0x23 ||
        op == 0x63 || op == 0x8F || op == 0x21 || op == 0x87 || op == 0x86)
        return i + 1 + ModRmLen(p, i + 1);
    if (op == 0xFF) return i + 1 + ModRmLen(p, i + 1);                 // call/jmp r/m
    if (op == 0xC3) return i + 1;                                     // ret
    if (op == 0xC2) return i + 3;                                     // ret imm16
    if (op == 0xCC) return i + 1;                                     // int3
    if (op == 0x90) return i + 1;                                     // nop
    return 0;
}

// 追跳存根链到真实函数体:
//   gdi32!SwapBuffers 等系统导出常以 FF 25 disp32 (jmp [rip+disp32]) 或
//   E9 rel32 开头 (跳转存根)。若直接钩存根, trampoline 原样复制含 RIP
//   相对寻址的指令会因地址偏移而跳飞。必须追到真实函数体再打补丁。
static BYTE* ResolveRealEntry(BYTE* entry)
{
    for (int hop = 0; hop < 8 && entry; ++hop) {
        if (entry[0] == 0xFF && entry[1] == 0x25) {          // jmp [rip+disp32]
            INT32 disp;
            memcpy(&disp, entry + 2, 4);
            BYTE** slot = (BYTE**)(entry + 6 + disp);        // 槽在导出者自身数据段内
            entry = *slot;
            continue;
        }
        if (entry[0] == 0xE9) {                              // jmp rel32
            INT32 disp;
            memcpy(&disp, entry + 1, 4);
            entry = entry + 5 + disp;
            continue;
        }
        return entry;   // 真实函数体
    }
    return NULL;
}

// 检查已解码窗口内是否存在需要重定位的指令
// (相对跳转/调用, 或 mod=00 rm=101 的 RIP 相对寻址) —— 蹦床原样复制
// 这类指令会因地址偏移而跳飞, 一律拒绝内联补丁。
static bool InsnNeedsReloc(const BYTE* p, int len)
{
    if (len <= 0) return true;
    // 跳过指令前缀
    int i = 0;
    while (i < len) {
        BYTE b = p[i];
        if ((b >= 0x40 && b <= 0x4F) || b == 0x66 || b == 0x67 ||
            b == 0xF0 || b == 0xF2 || b == 0xF3) { i++; continue; }
        break;
    }
    if (i >= len) return true;
    BYTE op = p[i];
    if (op == 0xE8 || op == 0xE9 || op == 0xEB ||
        (op >= 0x70 && op <= 0x7F) ||
        (op == 0x0F && i + 1 < len && p[i+1] >= 0x80 && p[i+1] <= 0x8F) ||
        (op == 0xFF && i + 1 < len && (p[i+1] & 0xC7) == 0x25))
        return true;
    if (i + 1 < len) {
        BYTE modrm = p[i + 1];
        if ((modrm >> 6) == 0 && (modrm & 7) == 5) return true;  // [rip+disp32]
    }
    return false;
}

// 模块映像大小 (PE 头解析, 无需 psapi)
static DWORD ModuleSizeOf(HMODULE m)
{
    BYTE* base = (BYTE*)m;
    if (!base) return 0;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    return nt->OptionalHeader.SizeOfImage;
}

// 安装 gdi32!SwapBuffers 钩子 (V68: VEH 页保护钩子, gdi32/gdi32full 零修改)。
// 用 PAGE_GUARD 保护真实 SwapBuffers 入口所在页: 渲染线程每次进入函数时
// 触发 STATUS_GUARD_PAGE_VIOLATION (硬件自动清除保护), VEH 处理器把 RIP
// 重定向到 HookSwapBuffers; 钩子干完活、执行点回到本模块后重新武装保护页。
// 同页其他函数被调用时不重定向, 单步离开该页后重新武装 (见 GuardVeh/StepVeh)。
// 优点: 不写任何代码/数据字节 —— 完整性检查/前后比对看不到任何改动。
// 也彻底消除旧版"槽位未解析"分支的崩溃面 (解析器原样运行, 尾跳到真实
// 入口时才触发保护)。
static BYTE*  g_guardPage  = NULL;
static BYTE*  g_guardEntry = NULL;

static void ReArmGuard(void)
{
    if (!g_guardPage) return;
    DWORD old = 0;
    VirtualProtect(g_guardPage, 0x1000, PAGE_EXECUTE_READ | PAGE_GUARD, &old);
}

static LONG WINAPI GuardVeh(PEXCEPTION_POINTERS ep)
{
    if (ep->ExceptionRecord->ExceptionCode != STATUS_GUARD_PAGE_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;
    void* addr = ep->ExceptionRecord->ExceptionAddress;
    if (!g_guardPage || (BYTE*)addr < g_guardPage || (BYTE*)addr >= g_guardPage + 0x1000)
        return EXCEPTION_CONTINUE_SEARCH;
    if (ep->ContextRecord->Rip == (ULONGLONG)(ULONG_PTR)g_guardEntry) {
        // 进入 SwapBuffers: 重定向到钩子 (guard 已被硬件清除, 钩子结束再武装)
        ep->ContextRecord->Rip = (ULONGLONG)(ULONG_PTR)&HookSwapBuffers;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    // 同页其他函数被调用: 解除保护 + 单步, 离开该页后由 StepVeh 重新武装
    DWORD old = 0;
    VirtualProtect(g_guardPage, 0x1000, PAGE_EXECUTE_READ, &old);
    ep->ContextRecord->EFlags |= 0x100;   // TF
    return EXCEPTION_CONTINUE_EXECUTION;
}

static LONG WINAPI StepVeh(PEXCEPTION_POINTERS ep)
{
    if (ep->ExceptionRecord->ExceptionCode != STATUS_SINGLE_STEP)
        return EXCEPTION_CONTINUE_SEARCH;
    ULONGLONG rip = ep->ContextRecord->Rip;
    if (g_guardPage && rip >= (ULONGLONG)(ULONG_PTR)g_guardPage &&
        rip < (ULONGLONG)(ULONG_PTR)(g_guardPage + 0x1000)) {
        // 仍在保护页内: 继续单步直到离开
        ep->ContextRecord->EFlags |= 0x100;
    } else {
        ReArmGuard();
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

static bool InstallSwapBuffersHook(void)
{
    // VEH 只注册一次 (幂等)
    static bool vehInstalled = false;
    if (!vehInstalled) {
        AddVectoredExceptionHandler(1, GuardVeh);
        AddVectoredExceptionHandler(1, StepVeh);
        vehInstalled = true;
    }

    HMODULE gdi = GetModuleHandleA(XS(kEnc_gdi32_dll, sizeof(kEnc_gdi32_dll)));
    if (!gdi) return false;
    BYTE* entry = (BYTE*)(void*)GetProcAddress(gdi, XS(kEnc_SwapBuffers, sizeof(kEnc_SwapBuffers)));
    if (!entry) return false;

    void* val = NULL;
    if (entry[0] == 0xFF && entry[1] == 0x25) {
        // 热补丁存根: 读槽位取目标; 槽未解析(指向 gdi32 内部解析器)时直接取 gdi32full
        INT32 disp;
        memcpy(&disp, entry + 2, 4);
        val = *(void**)(entry + 6 + disp);
        BYTE* gdiBase = (BYTE*)gdi;
        DWORD gdiSize = ModuleSizeOf(gdi);
        if ((BYTE*)val >= gdiBase && (BYTE*)val < gdiBase + gdiSize) {
            HMODULE full = GetModuleHandleA(XS(kEnc_gdi32full_dll, sizeof(kEnc_gdi32full_dll)));
            if (!full) return false;
            val = (void*)GetProcAddress(full, XS(kEnc_SwapBuffers, sizeof(kEnc_SwapBuffers)));
            if (!val) return false;
        }
    } else {
        val = entry;
    }
    BYTE* real = ResolveRealEntry((BYTE*)val);
    if (!real) return false;

    g_origSwapBuffers = (SwapBuffersFn)real;
    g_guardEntry = real;
    g_guardPage  = (BYTE*)((ULONGLONG)real & ~0xFFFULL);

    DWORD old = 0;
    if (!VirtualProtect(g_guardPage, 0x1000, PAGE_EXECUTE_READ | PAGE_GUARD, &old))
        return false;
    return true;
}

//--------------------------------------------------------------------------
// 反检测: 从 PEB 模块三链表摘除自身 (枚举不到) + 抹除 PE 头。
// 手动映射时无 LDR 条目, 摘链自然为空操作; LoadLibrary 路径同样适用。
//--------------------------------------------------------------------------
// V66.1: PEB 摘链 + PE 头抹除在网易版真机上被反作弊判为"隐藏/篡改模块"
// (进黑屋局)。V65.1 的可见模块形态 (不摘链/不抹头) 真机 60s+ 无异常,
// 因此默认关闭这两项 —— 运行时行为与真机验证版完全一致。
// 手动映射路径本来就没有 PEB 条目, 关闭后行为不变。
#define HIDE_MODULE_ANTI_DETECT 0
//--------------------------------------------------------------------------
static void* GetPeb(void)
{
    void* p = NULL;
    __asm__ __volatile__("movq %%gs:0x60, %0" : "=r"(p));
    return p;
}

static void UnlinkFromPeb(void)
{
    unsigned char* peb = (unsigned char*)GetPeb();
    if (!peb) return;
    void** ldr = (void**)(peb + 0x18);                 // PEB->Ldr
    if (!ldr || !*ldr) return;
    unsigned char* l = (unsigned char*)*ldr;
    // 本模块基址: 从函数地址的分配基取
    MEMORY_BASIC_INFORMATION mbi = {};
    if (!VirtualQuery((void*)&UnlinkFromPeb, &mbi, sizeof(mbi))) return;
    unsigned char* self = (unsigned char*)mbi.AllocationBase;

    // LDR_DATA_TABLE_ENTRY: 三链表分别在 0x00/0x10/0x20, DllBase 在 0x30
    for (int off = 0; off <= 0x20; off += 0x10) {
        unsigned char* head = l + 0x10 + off;          // In*OrderModuleList 头
        unsigned char* cur = *(unsigned char**)head;
        while (cur && cur != head) {
            unsigned char* entry = cur - off;          // 条目基址
            unsigned char* base = *(unsigned char**)(entry + 0x30);
            if (base == self) {
                unsigned char* fl = *(unsigned char**)cur;
                unsigned char* bl = *(unsigned char**)(cur + 8);
                *(unsigned char**)bl = fl;
                *(unsigned char**)(fl + 8) = bl;
                // 抹掉名字 (FullDllName@0x48 / BaseDllName@0x58)
                *(unsigned short*)(entry + 0x48) = 0;         // Length
                *(unsigned long long*)(entry + 0x50) = 0;     // Buffer
                *(unsigned short*)(entry + 0x58) = 0;
                *(unsigned long long*)(entry + 0x60) = 0;
                break;
            }
            cur = *(unsigned char**)cur;
        }
    }
}

static void ErasePeHeader(void)
{
    MEMORY_BASIC_INFORMATION mbi = {};
    if (!VirtualQuery((void*)&ErasePeHeader, &mbi, sizeof(mbi))) return;
    void* base = mbi.AllocationBase;
    if (!base) return;
    DWORD old = 0;
    if (VirtualProtect(base, 0x1000, PAGE_READWRITE, &old)) {
        memset(base, 0, 0x1000);
        VirtualProtect(base, 0x1000, old, &old);
    }
}

//--------------------------------------------------------------------------
// -nostartfiles 链接时的占位: 伪重定位 (_pei386_runtime_relocator) 只在
// MinGW CRT 启动时执行; 本 DLL 入口直指 DllMain 不经过 CRT。伪重定位
// 列表由注入器的手动映射器在映射阶段直接应用 (列表位于 .rdata 开头)。
//--------------------------------------------------------------------------
extern "C" void _pei386_runtime_relocator(void) {}

//--------------------------------------------------------------------------
// DLL 入口 (V65): 不创建任何线程, 只装钩子。
// 全部检测工作在游戏渲染线程 (Client thread) 内由 SwapBuffers 钩子驱动。
// 注意: 链接时 -nostartfiles -Wl,-e,DllMain 直接把入口指向本函数 (跳过
// MinGW CRT 启动), 使手动映射无需伪重定位运行时; 因此本函数必须是
// extern "C" 且不自依赖任何 CRT 初始化状态。
//--------------------------------------------------------------------------
extern "C" BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        // V68: 不再调用 DisableThreadLibraryCalls —— 线程劫持执行时目标线程
        // 可能持有 loader lock, 调用它会死锁; 且本 DLL 已无 LoadLibrary 路径,
        // 不存在线程附加通知, 该调用无意义。
        if (InterlockedExchange(&g_attached, 1)) return TRUE; // 防重复加载二次初始化

        // 防重复注入: 若共享内存已存在且健康 (ready 或 tick>0), 说明已有实例在运行
        char mapName[64];
        snprintf(mapName, sizeof(mapName), XS(kEnc_MCCombatStatus_fmt, sizeof(kEnc_MCCombatStatus_fmt)),
                 (unsigned long)GetCurrentProcessId());
        HANDLE existing = OpenFileMappingA(FILE_MAP_READ, FALSE, mapName);
        if (existing) {
            CombatStatus* st = (CombatStatus*)MapViewOfFile(existing, FILE_MAP_READ, 0, 0, 0);
            bool healthy = st && (st->ready || st->tick > 0);
            if (st) UnmapViewOfFile(st);
            CloseHandle(existing);
            if (healthy) return TRUE;
            // 卡死/未初始化 -> 继续接管
        }

        // 共享内存 (packed CombatStatus, 布局与 V63/V64 完全一致)
        g_map = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                   0, sizeof(CombatStatus), mapName);
        if (g_map) {
            g_status = (CombatStatus*)MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS, 0, 0, 0);
        }
        if (!g_status) return TRUE;
        memset(g_status, 0, sizeof(*g_status));
        g_status->magic   = kMagic;
        g_status->version = kVersion;
        g_status->pid     = GetCurrentProcessId();
        CopyName(g_status->errMsg, sizeof(g_status->errMsg), "H0:dllmain-init");

        // V68: UDP 已移除, 无 socket 初始化

        // 钩住 gdi32!SwapBuffers (游戏渲染线程每帧调用)
        if (!InstallSwapBuffersHook()) {
            if (g_status) CopyName(g_status->errMsg, sizeof(g_status->errMsg),
                                   "H9:hook-install-fail");
        } else if (g_status) {
            CopyName(g_status->errMsg, sizeof(g_status->errMsg), "H0:hook-ok");
        }

        // 反检测 (默认关闭, 见 HIDE_MODULE_ANTI_DETECT 上方说明: 网易真机会黑屋)
#if HIDE_MODULE_ANTI_DETECT
        UnlinkFromPeb();
        ErasePeHeader();
#endif
    }
    else if (reason == DLL_PROCESS_DETACH) {
        if (g_status) { UnmapViewOfFile(g_status); g_status = NULL; }
        if (g_map)    { CloseHandle(g_map);    g_map    = NULL; }
        // 钩子有意不还原: DLL 与进程同生命周期, 进程退出阶段还原补丁
        // 会与其他仍在执行的线程竞态, 无意义且有崩溃风险。
    }
    return TRUE;
}
