//============================================================================
//  MCCanAttackJni.dll
//  通过 JNI 注入到 Minecraft (java/javaw) 进程中的工具 DLL。
//
//  功能: 每 5ms 读取一次:
//      Minecraft.getMinecraft().thePlayer / player
//      Minecraft.getMinecraft().objectMouseOver / hitResult
//  判断玩家当前是否 "瞄准到了一个可以攻击的生物", 并把结果写入
//  共享内存 (Local\MCCanAttackStatus_<pid>), 同时通过 UDP 向本机
//  35785 端口发送 1 字节 (0x31='1'=可以攻击 / 0x30='0'=不可以)。
//
//  支持 5 套命名体系 (按顺序自动尝试):
//    1. mcp189      MCP 反混淆客户端 1.8.9 (MCP 名)
//    2. vanilla189  原版混淆客户端 1.8.9 (obfuscated 名)
//    3. forge189    Forge 1.8.9 运行时 (类=混淆名, 成员=SRG 名)
//    4. vanilla1201 原版/Fabric 1.20.1 运行时 (官方混淆名)
//    5. forge1201   Forge/NeoForge 1.20.1 运行时 (Mojang 官方名)
//
//  导出函数:
//    BOOL CanAttackNow(void)          -- 直接返回当前是否能攻击
//    BOOL IsJniReady(void)            -- JNI 是否已解析成功
//    BOOL GetCanAttackStatus(Status*) -- 拷贝完整状态结构
//============================================================================

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <jni.h>
#include <jvmti.h>
#include <string.h>
#include <stdio.h>

//--------------------------------------------------------------------------
// 与 injector.exe 共享的状态结构 (布局固定, 跨进程共享)
//--------------------------------------------------------------------------
#pragma pack(push, 1)
struct CanAttackStatus {
    DWORD        magic;            // 0x4D43414B = 'MCAK'
    DWORD        version;          // 3
    DWORD        pid;              // 被注入进程的 PID
    volatile LONG ready;           // JNI 解析是否成功
    volatile LONG inGame;          // 是否已进入游戏 (mc && player != null)
    volatile LONG canAttack;       // 核心结果: 当前是否能攻击
    volatile LONG hitType;         // 0=未命中 1=命中方块 2=命中实体
    volatile LONG targetLiving;    // 目标是否为 LivingEntity
    volatile LONG targetAlive;     // 目标是否存活
    volatile LONG targetIsPlayer;  // 目标是否是玩家自己
    char          targetName[128]; // 目标的类名 (如 EntityZombie / pr / bfj)
    char          mappingName[32]; // 命中的命名体系 (如 forge1201)
    char          envName[48];     // 环境探测结果 (forge/optifine/fabric/launchwrapper)
    char          loaderName[48];  // 使用的游戏类加载器类名
    char          errMsg[96];      // 最近一次错误详情 (类名/异常信息)
    char          failLog[160];    // 最近一轮 5 套映射的失败原因汇总
    volatile LONG mcNull;          // 1 = getMinecraft() 返回 null (双份类副本问题)
    volatile LONG tick;            // 更新计数
    volatile LONG lastError;       // 最近一次错误码, 0=无错误
};
#pragma pack(pop)

static const char* kMapNameFmt = "Local\\MCCanAttackStatus_%lu";
static const DWORD kMagic      = 0x4D43414B;
static const DWORD kVersion    = 4;

static HANDLE           g_map    = NULL;
static CanAttackStatus* g_status = NULL;
static volatile LONG    g_stop   = 0;

// UDP 上报: 向本机 35785 端口发送 0/1 (1=可以攻击, 0=不可以)
static SOCKET               g_sock = INVALID_SOCKET;
static struct sockaddr_in   g_dst;

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
};

// ---- 1. MCP 反混淆客户端 1.8.9 ----
static const JniMap kMcpMap = {
    "mcp189",
    "net/minecraft/client/Minecraft", "()Lnet/minecraft/client/Minecraft;", "getMinecraft",
    "thePlayer", "Lnet/minecraft/client/entity/EntityPlayerSP;",
    "objectMouseOver", "Lnet/minecraft/util/MovingObjectPosition;",
    "net/minecraft/util/MovingObjectPosition",
    "typeOfHit", NULL, "Lnet/minecraft/util/MovingObjectPosition$MovingObjectType;",
    NULL,
    "entityHit", NULL, "Lnet/minecraft/entity/Entity;",
    "net/minecraft/util/MovingObjectPosition$MovingObjectType",
    "ENTITY", NULL, "Lnet/minecraft/util/MovingObjectPosition$MovingObjectType;",
    "net/minecraft/entity/Entity",
    "canAttackWithItem", "isEntityAlive", NULL,
    "net/minecraft/entity/EntityLivingBase",
};

// ---- 2. 原版混淆客户端 1.8.9 ----
static const JniMap kVanilla189Map = {
    "vanilla189",
    "ave", "()Lave;", "A",
    "h", "Lbew;",
    "s", "Lauh;",
    "auh",
    "a", NULL, "Lauh$a;",
    NULL,
    "d", NULL, "Lpk;",
    "auh$a",
    "c", NULL, "Lauh$a;",
    "pk",
    "aD", "ai", NULL,
    "pr",
};

// ---- 3. Forge 1.8.9 (类=混淆名, 成员=SRG 名) ----
static const JniMap kForge189Map = {
    "forge189",
    "ave", "()Lave;", "func_71410_x",
    "field_71439_g", "Lbew;",
    "field_71476_x", "Lauh;",
    "auh",
    "field_72313_a", NULL, "Lauh$a;",
    NULL,
    "field_72308_g", NULL, "Lpk;",
    "auh$a",
    "ENTITY", "c", "Lauh$a;",
    "pk",
    "func_70075_an", "func_70089_S", NULL,
    "pr",
};

// ---- 3b. Forge 1.8.9 (FML 运行时反混淆: 类名=MCP 名, 成员=SRG 名) ----
// (实测验证: net.minecraft.client.Minecraft + func_71410_x)
static const JniMap kForge189McpMap = {
    "forge189mcp",
    "net/minecraft/client/Minecraft", "()Lnet/minecraft/client/Minecraft;", "func_71410_x",
    "field_71439_g", "Lnet/minecraft/client/entity/EntityPlayerSP;",
    "field_71476_x", "Lnet/minecraft/util/MovingObjectPosition;",
    "net/minecraft/util/MovingObjectPosition",
    "field_72313_a", NULL, "Lnet/minecraft/util/MovingObjectPosition$MovingObjectType;",
    NULL,
    "field_72308_g", NULL, "Lnet/minecraft/entity/Entity;",
    "net/minecraft/util/MovingObjectPosition$MovingObjectType",
    "ENTITY", "field_72376_c", "Lnet/minecraft/util/MovingObjectPosition$MovingObjectType;",
    "net/minecraft/entity/Entity",
    "func_70075_an", "func_70089_S", NULL,
    "net/minecraft/entity/EntityLivingBase",
};

// ---- 3c. Forge 1.12.2 (类名=MCP 名, 成员=SRG 名; 与 forge189mcp 同形态,
//       唯一区别: MovingObjectPosition 在 1.12 改名为 RayTraceResult!)
//  (真机实测: mccls:OK + 方法名 func_xxx; 映射来自 deobfuscation_data-1.12.2.lzma) ----
static const JniMap kForge1122Map = {
    "forge1122",
    "net/minecraft/client/Minecraft", "()Lnet/minecraft/client/Minecraft;", "func_71410_x",
    "field_71439_g", "Lnet/minecraft/client/entity/EntityPlayerSP;",
    "field_71476_x", "Lnet/minecraft/util/math/RayTraceResult;",
    "net/minecraft/util/math/RayTraceResult",
    "field_72313_a", NULL, "Lnet/minecraft/util/math/RayTraceResult$Type;",
    NULL,
    "field_72308_g", NULL, "Lnet/minecraft/entity/Entity;",
    "net/minecraft/util/math/RayTraceResult$Type",
    "ENTITY", "c", "Lnet/minecraft/util/math/RayTraceResult$Type;",
    "net/minecraft/entity/Entity",
    "func_70075_an", "func_70089_S", NULL,
    "net/minecraft/entity/EntityLivingBase",
};

// ---- 4. 原版/Fabric 1.20.1 (官方混淆名, 经 Mojang 官方映射验证) ----
static const JniMap kVanilla1201Map = {
    "vanilla1201",
    "enn", "()Lenn;", "N",
    "t", "Lfiy;",
    "w", "Leeg;",
    "eeg",
    NULL, "c", "()Leeg$a;",
    "eef",
    NULL, "a", "()Lbfj;",
    "eeg$a",
    "c", NULL, "Leeg$a;",
    "bfj",
    NULL, "bs", "cn",
    "bfz",
};

// ---- 4b. Forge/NeoForge 1.20.1 (类名=Mojang 官方名, 成员=混淆名) ----
// (真机实测: ModLauncher 只重映射类名, 成员名保持混淆!
//  混淆名来自 verify/client-1.20.1-mappings.txt 官方映射文件)
static const JniMap kForge1201ObfMap = {
    "forge1201obf",
    "net/minecraft/client/Minecraft", "()Lnet/minecraft/client/Minecraft;", "N",
    "t", "Lnet/minecraft/client/player/LocalPlayer;",
    "w", "Lnet/minecraft/world/phys/HitResult;",
    "net/minecraft/world/phys/HitResult",
    NULL, "c", "()Lnet/minecraft/world/phys/HitResult$Type;",
    "net/minecraft/world/phys/EntityHitResult",
    NULL, "a", "()Lnet/minecraft/world/entity/Entity;",
    "net/minecraft/world/phys/HitResult$Type",
    "c", NULL, "Lnet/minecraft/world/phys/HitResult$Type;",
    "net/minecraft/world/entity/Entity",
    NULL, "bs", "cn",
    "net/minecraft/world/entity/LivingEntity",
};

// ---- 4c. Forge/NeoForge 1.20.1 (类名=Mojang 官方名, 成员=MCP stable 名) ----
// (真机实测: 方法名是 m_91087_ 格式! 来自 mcp_config 的 stable 映射:
//  getInstance=m_91087_ player=f_91074_ hitResult=f_91077_ getType=m_6662_
//  getEntity=m_82443_ isAlive=m_6084_ isAttackable=m_6097_ ENTITY=ENTITY)
static const JniMap kForge1201StableMap = {
    "forge1201stb",
    "net/minecraft/client/Minecraft", "()Lnet/minecraft/client/Minecraft;", "m_91087_",
    "f_91074_", "Lnet/minecraft/client/player/LocalPlayer;",
    "f_91077_", "Lnet/minecraft/world/phys/HitResult;",
    "net/minecraft/world/phys/HitResult",
    NULL, "m_6662_", "()Lnet/minecraft/world/phys/HitResult$Type;",
    "net/minecraft/world/phys/EntityHitResult",
    NULL, "m_82443_", "()Lnet/minecraft/world/entity/Entity;",
    "net/minecraft/world/phys/HitResult$Type",
    "ENTITY", NULL, "Lnet/minecraft/world/phys/HitResult$Type;",
    "net/minecraft/world/entity/Entity",
    NULL, "m_6084_", "m_6097_",
    "net/minecraft/world/entity/LivingEntity",
};

// ---- 5. Forge/NeoForge 1.20.1 (Mojang 官方名) ----
static const JniMap kOfficial1201Map = {
    "forge1201",
    "net/minecraft/client/Minecraft", "()Lnet/minecraft/client/Minecraft;", "getInstance",
    "player", "Lnet/minecraft/client/player/LocalPlayer;",
    "hitResult", "Lnet/minecraft/world/phys/HitResult;",
    "net/minecraft/world/phys/HitResult",
    NULL, "getType", "()Lnet/minecraft/world/phys/HitResult$Type;",
    "net/minecraft/world/phys/EntityHitResult",
    NULL, "getEntity", "()Lnet/minecraft/world/entity/Entity;",
    "net/minecraft/world/phys/HitResult$Type",
    "ENTITY", NULL, "Lnet/minecraft/world/phys/HitResult$Type;",
    "net/minecraft/world/entity/Entity",
    NULL, "isAlive", "isAttackable",
    "net/minecraft/world/entity/LivingEntity",
};

static const JniMap* kAllMaps[] = {
    &kMcpMap, &kVanilla189Map, &kForge189Map, &kForge189McpMap, &kForge1122Map,
    &kVanilla1201Map, &kForge1201ObfMap, &kForge1201StableMap, &kOfficial1201Map,
};
static const int kMapCount = (int)(sizeof(kAllMaps) / sizeof(kAllMaps[0]));

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
        for (int i = 0; i < kMapCount && !found2; ++i) {
            if (strcmp(kAllMaps[i]->mcClass, "net/minecraft/client/Minecraft") == 0)
                continue; // 已测过
            jclass probe = LoadClass(env, kAllMaps[i]->mcClass, loader, clsCls, forName);
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
        for (int i = 0; i < kMapCount && !canLoad; ++i) {
            jclass probe = LoadClass(env, kAllMaps[i]->mcClass, loader, clsCls, forName);
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
// UDP 上报: 向本机 35785 端口发送 1 字节 (0x31='1'=可以攻击 / 0x30='0'=不可以)
//--------------------------------------------------------------------------
static void SendCanAttack(int canAttack)
{
    if (g_sock == INVALID_SOCKET) return;
    char b = canAttack ? '1' : '0';
    sendto(g_sock, &b, 1, 0, (const struct sockaddr*)&g_dst, sizeof(g_dst));
}

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

    if (!player || !mop) {
        g_status->canAttack      = 0;
        g_status->hitType        = 0;
        g_status->targetLiving   = 0;
        g_status->targetAlive    = 0;
        g_status->targetIsPlayer = 0;
        CopyName(g_status->targetName, sizeof(g_status->targetName), NULL);
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
// 工作线程: 等待 JVM -> 解析映射 -> 循环更新状态
//--------------------------------------------------------------------------
static DWORD WINAPI JniWorker(LPVOID)
{
    char mapName[64];
    snprintf(mapName, sizeof(mapName), kMapNameFmt, (unsigned long)GetCurrentProcessId());

    g_map = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                               0, sizeof(CanAttackStatus), mapName);
    if (g_map) {
        g_status = (CanAttackStatus*)MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    }
    if (!g_status) return 0;

    memset(g_status, 0, sizeof(*g_status));
    g_status->magic   = kMagic;
    g_status->version = kVersion;
    g_status->pid     = GetCurrentProcessId();

    // UDP: 初始化 socket, 目标为本机 35785 端口
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) == 0) {
        g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (g_sock != INVALID_SOCKET) {
            memset(&g_dst, 0, sizeof(g_dst));
            g_dst.sin_family      = AF_INET;
            g_dst.sin_port        = htons(35785);
            g_dst.sin_addr.s_addr = inet_addr("127.0.0.1");
        }
    }

    // 等待 jvm.dll 被加载 (游戏进程启动后很快就有), 无限等待直到 g_stop
    HMODULE jvm = NULL;
    while (!g_stop) {
        jvm = GetModuleHandleA("jvm.dll");
        if (jvm) break;
        Sleep(200);
    }
    if (!jvm || g_stop) return 0; // 仅进程退出时才会走到这里

    typedef jint (JNICALL* GetCreatedVMs_t)(JavaVM**, jsize, jsize*);
    GetCreatedVMs_t getVMs = (GetCreatedVMs_t)GetProcAddress(jvm, "JNI_GetCreatedJavaVMs");
    if (!getVMs) return 0;

    JavaVM* vm = NULL;
    jsize   n  = 0;
    if (getVMs(&vm, 1, &n) != JNI_OK || n < 1 || !vm) return 0;

    JNIEnv* env = NULL;
    if (vm->AttachCurrentThread((void**)&env, NULL) != JNI_OK || !env) return 0;

    // 一次性获取 java/lang/Class.forName 的 ID, 以及游戏类加载器
    jclass  clsCls  = env->FindClass("java/lang/Class");
    jmethodID forName = clsCls ? env->GetStaticMethodID(clsCls, "forName",
        "(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;") : NULL;
    if (env->ExceptionCheck()) env->ExceptionClear();

    // 环境探测 (forge/optifine/fabric/launchwrapper)
    // 注意: 探测/找加载器阶段加了 30 秒看门狗, 防止 JVM 启动繁忙时卡死
    DWORD t0 = GetTickCount();
    CopyName(g_status->errMsg, sizeof(g_status->errMsg), "E0:detect-env");
#ifndef NO_ENV_DETECT
    DetectEnv(env, NULL, clsCls, forName,
              g_status->envName, sizeof(g_status->envName));
#endif
    CopyName(g_status->errMsg, sizeof(g_status->errMsg), "E1:detect-done");

    // 游戏类加载器: 解决 Forge/launchwrapper 双份类副本问题
    jobject launchLoader = NULL;
    jobject gameLoader = NULL;
    jobject sysLoader  = NULL;
    bool     useGameLoader = false;
    if (GetTickCount() - t0 < 30000) {
        CopyName(g_status->errMsg, sizeof(g_status->errMsg), "E2:find-loader");
        launchLoader = FindLaunchClassLoader(env, clsCls, forName);
        gameLoader = FindGameClassLoader(env, clsCls, forName);
        useGameLoader = (gameLoader != NULL);
        CopyName(g_status->errMsg, sizeof(g_status->errMsg),
                 useGameLoader ? "E3:loader-found" : "E3:loader-null");
    } else {
        CopyName(g_status->errMsg, sizeof(g_status->errMsg),
                 "E3:loader-timeout-skip");
    }
    // 系统类加载器 (app loader, -cp): 一定能拿到, 作为第二候选。
    // 注意: 它加载的是原版 jar 的类 (成员=混淆名), 可能是"双份副本"
    // (getMinecraft 返回 null), 由下方的 mcNull 切换逻辑自动换到 gameLoader。
    {
        jclass clCls2 = env->FindClass("java/lang/ClassLoader");
        jmethodID getSys = clCls2 ? env->GetStaticMethodID(clCls2, "getSystemClassLoader",
                                                           "()Ljava/lang/ClassLoader;") : NULL;
        if (getSys) {
            sysLoader = env->CallStaticObjectMethod(clCls2, getSys);
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
    }
    if (useGameLoader) {
        jclass clCls = env->FindClass("java/lang/ClassLoader");
        if (!clCls) env->ExceptionClear();
        jboolean isCL = clCls ? env->IsInstanceOf(gameLoader, clCls) : JNI_FALSE;
        if (env->ExceptionCheck()) env->ExceptionClear();
        jclass lc = (isCL && gameLoader) ? env->GetObjectClass(gameLoader) : NULL;
        // 注意: getName 属于 java/lang/Class, 必须用 clsCls 取方法 ID!
        // (GetObjectClass 的返回值只是"对象实例的类", 不能用于 GetMethodID)
        jmethodID nmM = (lc && clsCls) ? env->GetMethodID(clsCls, "getName", "()Ljava/lang/String;") : NULL;
        if (env->ExceptionCheck()) env->ExceptionClear();
        jstring nm = nmM ? (jstring)env->CallObjectMethod(lc, nmM) : NULL;
        const char* utf = nm ? env->GetStringUTFChars(nm, NULL) : NULL;
        CopyName(g_status->loaderName, sizeof(g_status->loaderName),
                 utf ? utf : (gameLoader ? "(unknown)" : "(null - 使用 FindClass)"));
        if (utf) env->ReleaseStringUTFChars(nm, utf);
        if (env->ExceptionCheck()) env->ExceptionClear();
    } else {
        CopyName(g_status->loaderName, sizeof(g_status->loaderName),
                 "(null - 使用 FindClass)");
    }

    Resolved res = {};
    int      mapIdx    = 0;
    int      nullMcStreak = 0;

    while (!g_stop) {
        if (!res.ok) {
            // 尚未解析成功: 依次尝试 5 套映射, 每 500ms 重试一轮
            // (游戏类可能还在加载)
            jobject loader = useGameLoader ? gameLoader : sysLoader;
            g_status->failLog[0] = 0; // 每轮清空失败汇总
            bool ok = false;
            while (mapIdx < kMapCount && !ok) {
                CopyName(g_status->errMsg, sizeof(g_status->errMsg),
                         kAllMaps[mapIdx]->name);
                // 尝试当前加载器
                Resolved tryRes;
                bool tryOk = ResolveWith(env, *kAllMaps[mapIdx], tryRes, loader, clsCls, forName);
                if (tryOk) {
                    // 终极验证: getMinecraft() 必须返回真实例, 排除双份类副本
                    jobject inst = env->CallStaticObjectMethod(tryRes.mcClass, tryRes.getMinecraft);
                    bool hasInst = (inst != NULL);
                    if (env->ExceptionCheck()) env->ExceptionClear();
                    if (inst) env->DeleteLocalRef(inst);
                    if (hasInst) { res = tryRes; ok = true; }
                    else {
                        CopyName(g_status->errMsg, sizeof(g_status->errMsg),
                                 kAllMaps[mapIdx]->name);
                        NoteErr(kAllMaps[mapIdx]->name, "getMinecraft()=null(副本)");
                    }
                }
                // 当前加载器失败/副本时, 同轮换另一个加载器再试 (TCL 与 app loader 都覆盖)
                if (!ok && gameLoader && sysLoader && gameLoader != sysLoader) {
                    jobject loader2 = (loader == gameLoader) ? sysLoader : gameLoader;
                    Resolved tryRes2;
                    bool tryOk2 = ResolveWith(env, *kAllMaps[mapIdx], tryRes2, loader2, clsCls, forName);
                    if (tryOk2) {
                        jobject inst2 = env->CallStaticObjectMethod(tryRes2.mcClass, tryRes2.getMinecraft);
                        bool hasInst2 = (inst2 != NULL);
                        if (env->ExceptionCheck()) env->ExceptionClear();
                        if (inst2) env->DeleteLocalRef(inst2);
                        if (hasInst2) {
                            res = tryRes2;
                            ok = true;
                            useGameLoader = (loader2 == gameLoader);
                        }
                    }
                }
                if (!ok) mapIdx++;
            }
            if (ok) {
                g_status->ready = 1;
                CopyName(g_status->mappingName, sizeof(g_status->mappingName), res.name);
                // 终极修正: JVMTI 找"真 Minecraft 类" (getter 返回非 null),
                // 用它的类加载器重新解析, 彻底解决双份类副本问题
                const JniMap& mOk = *kAllMaps[mapIdx]; // mapIdx 此时指向成功的那套
#ifndef NO_REAL_FIX
                // 终极修正 A: findLoadedClass 拿游戏已加载的真类
                jclass realMc = FindLoadedGameClass(env, launchLoader, mOk.mcClass,
                                                    clsCls, forName,
                                                    mOk.getMinecraft, mOk.mcSig);
                // 终极修正 B: JVMTI 扫描 (备用; 无 agent 的 JVM 上不可用, 已默认禁用)
#endif
#ifndef NO_REAL_FIX
                if (realMc) {
                    // 真类的类加载器 = 游戏类加载器 (findLoadedClass 的 loader 也行)
                    jmethodID getLdr = env->GetMethodID(clsCls, "getClassLoader",
                                                        "()Ljava/lang/ClassLoader;");
                    if (getLdr && !launchLoader) launchLoader = env->CallObjectMethod(realMc, getLdr);
                    jobject realLoader = getLdr ? env->CallObjectMethod(realMc, getLdr) : NULL;
                    if (env->ExceptionCheck()) env->ExceptionClear();
                    Resolved resReal;
                    bool okReal = ResolveWith(env, mOk, resReal, realLoader, clsCls, forName);
                    if (okReal) {
                        res = resReal;
                        CopyName(g_status->loaderName, sizeof(g_status->loaderName),
                                 "JVMTI-real-loader");
                    }
                }
#endif // NO_REAL_FIX
                CopyName(g_status->errMsg, sizeof(g_status->errMsg), ""); // 清调试残留
            } else {
                // 永不放弃: 持续重试直到解析成功 (防止半路永久失效)
                Sleep(500);
                continue;
            }
        }
        UpdateStatus(env, res);
        // UDP 上报: 1 = 可以攻击, 0 = 不可以 (与检查同频, 每 5ms)
        SendCanAttack(g_status ? g_status->canAttack : 0);
        // 若长期拿不到主类 (双份类副本), 自动切换加载器重新解析
        if (g_status->mcNull) {
            if (++nullMcStreak > 100) { // 连续 0.5 秒拿不到 mc (每 5ms 一次)
                nullMcStreak = 0;
                if (gameLoader && sysLoader) {
                    useGameLoader = !useGameLoader; // 游戏加载器 <-> 系统加载器 切换
                } else if (gameLoader) {
                    useGameLoader = true;
                }
                res.ok = false;
                mapIdx = 0;
                CopyName(g_status->mappingName, sizeof(g_status->mappingName), NULL);
                g_status->ready = 0;
            }
        } else {
            nullMcStreak = 0;
        }
        Sleep(5); // 5ms 周期检查
    }

    // 清理 UDP
    if (g_sock != INVALID_SOCKET) { closesocket(g_sock); g_sock = INVALID_SOCKET; }
    WSACleanup();
    vm->DetachCurrentThread();
    return 0;
}

//--------------------------------------------------------------------------
// 导出函数
//--------------------------------------------------------------------------
extern "C" {

__declspec(dllexport) BOOL WINAPI CanAttackNow(void)
{
    return (g_status && g_status->ready) ? (BOOL)g_status->canAttack : FALSE;
}

__declspec(dllexport) BOOL WINAPI IsJniReady(void)
{
    return g_status ? (BOOL)g_status->ready : FALSE;
}

__declspec(dllexport) BOOL WINAPI GetCanAttackStatus(CanAttackStatus* out)
{
    if (!out || !g_status) return FALSE;
    *out = *g_status;
    return TRUE;
}

} // extern "C"

//--------------------------------------------------------------------------
// DLL 入口
//--------------------------------------------------------------------------
BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hInst);

        // 防重复注入: 若共享内存已存在且 worker 健康 (ready 或 tick>0),
        // 说明已有实例在运行, 不再启动新 worker。
        // 若旧 worker 疑似卡死 (ready=0 且 tick=0), 允许新 worker 接管。
        char mapName[64];
        snprintf(mapName, sizeof(mapName), kMapNameFmt, (unsigned long)GetCurrentProcessId());
        HANDLE existing = OpenFileMappingA(FILE_MAP_READ, FALSE, mapName);
        if (existing) {
            CanAttackStatus* st = (CanAttackStatus*)MapViewOfFile(existing, FILE_MAP_READ, 0, 0, 0);
            bool healthy = st && (st->ready || st->tick > 0);
            if (st) UnmapViewOfFile(st);
            CloseHandle(existing);
            if (healthy) return TRUE;
            // 卡死/未初始化 -> 继续创建新 worker 接管
        }

        HANDLE t = CreateThread(NULL, 0, JniWorker, NULL, 0, NULL);
        if (t) CloseHandle(t);
    }
    else if (reason == DLL_PROCESS_DETACH) {
        InterlockedExchange(&g_stop, 1);
        Sleep(100);
        if (g_sock != INVALID_SOCKET) { closesocket(g_sock); g_sock = INVALID_SOCKET; }
        WSACleanup();
        if (g_status) { UnmapViewOfFile(g_status); g_status = NULL; }
        if (g_map)    { CloseHandle(g_map);    g_map    = NULL; }
    }
    return TRUE;
}
