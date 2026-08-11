# Minecraft 1.8.9 · JNI 注入工具 —— 零基础教学文档

> 配套项目：`MCCombatStatus-JNI/`（桌面文件夹原名为 MCCanAttack-JNI，DLL 仍叫 MCCombatStatusJni.dll）
> 本文从"完全不懂 JNI"开始，一步一步带你搞懂：Java 程序怎么跑、JNI 是什么、
> DLL 注入是什么、以及我们的工具每一行代码在干什么。
> 学完你可以自己改代码，做出"读取玩家坐标""读取目标血量"等新功能。

---

## 目录

- [第 0 章　这份文档怎么用](#第-0-章这份文档怎么用)
- [第 1 章　五个必须搞懂的大概念](#第-1-章五个必须搞懂的大概念)
- [第 2 章　目标拆解：怎么知道"能不能攻击"](#第-2-章目标拆解怎么知道能不能攻击)
- [第 3 章　JNI 速成（最重要的一章）](#第-3-章jni-速成最重要的一章)
- [第 4 章　逐行读代码：MCCombatStatusJni.dll](#第-4-章逐行读代码mccanattackjnidll)
- [第 5 章　逐行读代码：injector.exe](#第-5-章逐行读代码injectorexe)
- [第 6 章　动手实验与改造练习](#第-6-章动手实验与改造练习)
- [第 7 章　常见坑与 FAQ](#第-7-章常见坑与-faq)
- [附录 A　JNI 函数速查表](#附录-ajni-函数速查表)
- [附录 B　类型签名（Descriptor）速查](#附录-b类型签名descriptor速查)
- [附录 C　1.8.9 本项目用到的全部名字对照表](#附录-c189-本项目用到的全部名字对照表)
- [附录 D　延伸学习](#附录-d延伸学习)

---

## 第 0 章　这份文档怎么用

### 0.1 先修知识

你只需要：

| 需要 | 程度 |
|---|---|
| Java 语法 | 会看 `class`、`字段`、`方法` 即可，不需要会写 |
| C/C++ 语法 | 会看 `struct`、`函数`、`指针` 概念即可 |
| Windows 操作 | 会运行 `.exe`、`.bat` |

完全不懂 JNI？没关系，这正是本文要教的。

### 0.2 阅读路线

- **只想用工具** → 读第 1、2 章和 README 就够了。
- **想看懂原理** → 1 → 2 → 3 章（JNI 速成是核心）。
- **想改代码做新功能** → 1 → 2 → 3 → 4 → 5 → 6 章全读。

### 0.3 一个比喻贯穿全文

> 把 Minecraft 的 Java 进程想象成一座**大厦**（JVM），里面住着很多"类"。
> 我们的 DLL 是一个**偷偷住进大厦的房客**（注入），
> JNI 是**大厦的管理员电话**（JNIEnv），
> 我们通过这个电话向管理员查询："住在 3 楼的玩家 `thePlayer` 现在手里指着什么？"
> 每 50 毫秒问一次，把答案写在**门口的黑板**（共享内存）上，外面的人就能看到。

记住这个比喻，后面每章都会呼应它。

---

## 第 1 章　五个必须搞懂的大概念

### 1.1 Java 程序是怎么跑起来的

Java 代码不是直接被 CPU 执行的，中间隔着一层：

```
你的 Java 源码 (TestMC.java)
        │ javac 编译
        ▼
字节码 (TestMC.class)  ← 一种"中间语言"，机器看不懂
        │
        ▼
Java 虚拟机 (JVM)  ← 一个真正的程序，它负责"解释/编译执行"字节码
        │
        ▼
CPU
```

- **JVM**（Java Virtual Machine）：Minecraft 的 Java 版就是一个 JVM 进程，
  进程名叫 `java.exe` 或 `javaw.exe`。
- **类（class）**：Java 的基本单位。Minecraft 有几千个类，
  比如 `Minecraft`（游戏主类）、`EntityZombie`（僵尸）。
- 类里装着**字段**（数据，比如玩家的坐标）和**方法**（行为，比如"是否存活"）。
- **jvm.dll**：Windows 上 JVM 本体是以 DLL 形式存在的（HotSpot 实现）。
  每个 Java 进程都会加载它。**这是后面注入的关键**。

### 1.2 Minecraft 与 MCP：同一个东西的三种名字

Mojang 发布的 Minecraft 是**混淆过**的：类名、字段名、方法名都被改成了
毫无意义的短名字（防止别人读代码）。但是 MCP（Mod Coder Pack，你这个
`mcp918` 工作区就是它）做了**反混淆**：把短名字还原成正常人能懂的名字。

同一个东西，存在三个"世界"的名字：

| 世界 | 例子 | 说明 |
|---|---|---|
| **混淆名**（游戏实际运行的名字） | `ave`、`auh`、`pk`、`h`、`s` | 官方 1.8.9.jar 里的真名 |
| **SRG 中间名** | `field_71439_g`、`func_71410_x` | MCP 生成的稳定过渡名，也是 Forge 1.8.9 运行时的成员名 |
| **MCP 名**（人话） | `Minecraft`、`thePlayer`、`objectMouseOver` | MCP 反混淆后的可读名 |
| **Mojang 官方名** | `getInstance`、`player`、`hitResult` | 1.17+ Forge/NeoForge 运行时的可读名 |

> ⚠️ 关键结论：**注入后我们面对的是运行中的真实 jar**，不同环境下名字不同：
> - 1.8.9 原版 → 混淆名；MCP 反混淆客户端 → MCP 名；Forge 1.8.9 → 混淆类名 + SRG 成员名
> - 1.20.1 原版/Fabric → 官方混淆名；Forge/NeoForge 1.20.1 → Mojang 官方名
> 所以我们的工具准备 **10 套映射表**，按顺序自动尝试（见第 6.7 节）。

### 1.3 什么是 JNI（Java Native Interface）

**JNI = Java 与 C/C++ 之间的官方"电话线"。**

- 它允许 C/C++ 代码**进入 JVM 内部**：找到类、读字段、调用方法。
- 它是 Java 官方标准（不是黑客技术），JDK 自带头文件 `jni.h`。
- 我们的 DLL 就是通过 JNI 这个"电话"，向游戏询问各种数据的。

JNI 世界里的三个核心"角色"：

```
JavaVM   —— 整个 JVM 的"总机"（一个进程只有一个）
JNIEnv   —— 当前线程的"电话机"（每个线程各有一部）
jobject  —— 指向某个 Java 对象的"遥控器"（类、实例、字符串…）
```

第 3 章会详细拆解。

### 1.4 什么是 DLL

DLL（Dynamic Link Library）是 Windows 的动态链接库：

- 里面是一堆编译好的函数，供程序调用。
- 它**不是独立程序**，必须被某个进程"加载"进自己的内存里才能执行。
- 我们的 `MCCombatStatusJni.dll` 就是一个 DLL：加载进游戏进程后，
  里面的代码就在游戏进程里运行了。

### 1.5 什么是 DLL 注入

正常情况下，游戏进程只会加载它自己需要的 DLL。
**注入**就是：我们强行让游戏进程加载我们的 DLL，从而在游戏进程内部执行我们的代码。

经典注入手法（injector.exe 用的就是它），一共四步：

```
1. VirtualAllocEx      在游戏进程的内存里"租一块地"（存放 DLL 路径字符串）
2. WriteProcessMemory  把 DLL 的完整路径写进那块地
3. GetProcAddress      找到 kernel32.dll 的 LoadLibraryA 函数地址
4. CreateRemoteThread  在游戏进程里开一个线程，让它去执行 LoadLibraryA(路径)
                       → 游戏进程就会像加载正常库一样加载我们的 DLL！
```

加载完成后，Windows 会调用 DLL 的入口函数 `DllMain`，我们的工作线程就从那里启动。

> 为什么注入后能访问游戏数据？
> 因为我们的代码运行在**游戏进程内部**，和游戏共享同一份内存，
> JNI 自然能找到游戏的所有对象。

---

## 第 2 章　目标拆解：怎么知道"能不能攻击"

### 2.1 数据在哪里

在 1.8.9 里，判定"玩家瞄准了什么"的数据链是：

```
Minecraft 单例 (getMinecraft())
   │ 字段 thePlayer            ← 玩家自己（EntityPlayerSP）
   │ 字段 objectMouseOver      ← 准星瞄准的结果（MovingObjectPosition）
   │
   └── objectMouseOver 里:
         ├─ typeOfHit   ← 命中类型: MISS(没打中) / BLOCK(方块) / ENTITY(实体)
         └─ entityHit   ← 命中的实体（当 typeOfHit==ENTITY 时有值）
```

`objectMouseOver` 是游戏**每 tick（1/20 秒）自己更新**的：
它从玩家眼睛位置沿视线方向做射线检测（ray trace），记录准星指到的东西。
所以我们不用自己算瞄准，直接**读**这个现成结果就行。

### 2.2 判定公式

```java
// 这是游戏内 Java 视角的伪代码：
canAttack = (objectMouseOver != null)
         && (typeOfHit == ENTITY)              // 准星确实命中了实体
         && (entityHit instanceof EntityLivingBase)  // 目标是活物（僵尸/猪/玩家…）
         && (entityHit.isEntityAlive())        // 目标还活着
         && (entityHit != thePlayer)           // 不是自己
         && (thePlayer.canAttackWithItem())    // 手持物品能造成伤害（空手/武器=true，食物=false）
```

> `EntityLivingBase` 是 1.8.9 里"有血量的生物"的基类：
> 僵尸、猪、村民、其他玩家都继承它；而掉落物、箭、方块实体不是。

### 2.3 目标数据在三套名字下长什么样

| 含义 | MCP 名（反混淆客户端） | 混淆名（原版客户端） |
|---|---|---|
| Minecraft 类 | `net/minecraft/client/Minecraft` | `ave` |
| 静态方法 获取单例 | `getMinecraft` | `A` |
| 字段 玩家 | `thePlayer` | `h` |
| 字段 准星结果 | `objectMouseOver` | `s` |
| MovingObjectPosition 类 | `net/minecraft/util/MovingObjectPosition` | `auh` |
| 字段 命中类型 | `typeOfHit` | `a` |
| 字段 命中实体 | `entityHit` | `d` |
| 枚举类 命中类型 | `...$MovingObjectType` | `auh$a` |
| 枚举常量 实体 | `ENTITY` | `c` |
| Entity 类 | `net/minecraft/entity/Entity` | `pk` |
| 方法 能攻击吗 | `canAttackWithItem` | `aD` |
| 方法 存活吗 | `isEntityAlive` | `ai` |
| EntityLivingBase 类 | `net/minecraft/entity/EntityLivingBase` | `pr` |

> 这些名字不是猜的：混淆名是我从官方 `1.8.9.jar` 用 `javap` 反编译
> 逐个验证过的；MCP 名来自 `conf/joined.srg`、`methods.csv`、`fields.csv`。
> 第 6.6 节教你自己查。

---

## 第 3 章　JNI 速成（最重要的一章）

> 本章把本项目用到的 JNI 知识一次讲完。看懂这章，代码就只剩"查字典"了。

### 3.1 JavaVM vs JNIEnv：总机与电话机

| | `JavaVM` | `JNIEnv` |
|---|---|---|
| 数量 | 一个进程一个 | 每个线程一个 |
| 比喻 | 大厦总机 | 你手里的电话机 |
| 主要方法 | `AttachCurrentThread`、`DetachCurrentThread`、`GetEnv` | 其余**所有** JNI 函数 |
| 怎么获得 | `JNI_GetCreatedJavaVMs()` | 从 JavaVM 拿到 |

规则：**JNIEnv 不能跨线程使用**。我们 DLL 的工作线程是 `CreateThread`
创建的"外来线程"，JVM 不认识它，所以必须先 `AttachCurrentThread`
（相当于用总机给自己开通一部电话），用完再 `DetachCurrentThread`。

### 3.2 JNIEnv 的底层秘密：函数指针表

`JNIEnv` 本质上是一个**函数指针数组**的包装：

```
JNIEnv*  ──▶ 指向一个结构 ──▶ 里面是一大排函数指针：
                                [0]  reserved
                                [1]  reserved
                                [2]  reserved
                                [3]  GetVersion
                                [4]  DefineClass
                                [5]  FindClass        ← 找类
                                [6]  FromReflectedMethod
                                ...  （一共 230+ 个）
                                [x]  GetFieldID       ← 找字段
                                [x]  GetMethodID      ← 找方法
                                [x]  CallBooleanMethod ← 调用方法
                                ...
```

- 在 **C** 里写：`(*env)->FindClass(env, "xxx");`（因为要多解引用一层）
- 在 **C++** 里写：`env->FindClass("xxx");`（C++ 封装层帮你隐藏了细节）

我们项目用 C++，所以都是 `env->XXX(...)` 的写法。

### 3.3 第一步：找到 JVM（JNI_GetCreatedJavaVMs）

我们 DLL 在游戏进程内部，可以直接问"这个进程里的 JVM 在哪"：

```cpp
// 1. jvm.dll 一定在进程里，先拿到它的句柄
HMODULE jvm = GetModuleHandleA("jvm.dll");

// 2. 从 jvm.dll 里取出导出函数 JNI_GetCreatedJavaVMs 的地址
//    （这个函数会告诉你本进程创建了哪些 JVM）
auto getVMs = (jint (JNICALL*)(JavaVM**, jsize, jsize*))
              GetProcAddress(jvm, "JNI_GetCreatedJavaVMs");

// 3. 调用它，拿到 JavaVM*
JavaVM* vm = NULL;
jsize n = 0;
getVMs(&vm, 1, &n);   // 最多要 1 个，实际数量写入 n
```

> 为什么要"动态取地址"而不是直接链接 jvm.lib？
> 因为我们不知道用户 Java 装在哪个目录，动态查找最稳。

### 3.4 第二步：把当前线程 attach 到 JVM

```cpp
JNIEnv* env = NULL;
vm->AttachCurrentThread((void**)&env, NULL);   // 成功返回 JNI_OK(0)
```

- attach 之后，`env` 就是本线程专属的 JNIEnv，可以随意调用。
- 线程结束前要 `vm->DetachCurrentThread()`，否则 JVM 里会残留僵尸线程记录。

### 3.5 第三步：找类（FindClass）

```cpp
jclass mc = env->FindClass("net/minecraft/client/Minecraft");
// 或混淆版: env->FindClass("ave");
```

- 参数是**类名**，用 `/` 分隔包名，不是 `.`。
- 找不到时返回 `NULL` 并挂起一个异常（见 3.11）。
- `jclass` 也是一种 `jobject`，本质上是一个"指向 java.lang.Class 对象的引用"。

### 3.6 第四步：找字段和方法（ID 是"书签"）

JNI 的设计哲学：**找一次，用一辈子**。

```cpp
// 找实例字段：GetFieldID(类, 字段名, 字段类型签名)
jfieldID thePlayerField = env->GetFieldID(mc, "thePlayer", "Lnet/minecraft/client/entity/EntityPlayerSP;");

// 找静态字段：GetStaticFieldID（枚举常量就是静态字段）
jfieldID entityConst = env->GetStaticFieldID(typeClass, "ENTITY", "Lnet/minecraft/util/MovingObjectPosition$MovingObjectType;");

// 找实例方法：GetMethodID(类, 方法名, 方法签名)
jmethodID alive = env->GetMethodID(entityClass, "isEntityAlive", "()Z");

// 找静态方法：GetStaticMethodID
jmethodID getMc = env->GetStaticMethodID(mc, "getMinecraft", "()Lnet/minecraft/client/Minecraft;");
```

- `jfieldID` / `jmethodID` 是**不透明指针**，本质是类里的"书签"。
- 只要类不被卸载（游戏进程里不会），**ID 永远有效**，所以只解析一次。
- 为什么必须带"签名"？因为 Java 允许**方法重载**（同名不同参），
  签名用于精确指定是哪一个。字段也同理（理论上字段类型也要精确匹配）。

### 3.7 类型签名（Descriptor）——JNI 的"摩斯密码"

签名就是 JNI 表达"类型"用的缩写。基础表：

| Java 类型 | 签名 |
|---|---|
| `boolean` | `Z` |
| `byte` | `B` |
| `char` | `C` |
| `short` | `S` |
| `int` | `I` |
| `long` | `J` |
| `float` | `F` |
| `double` | `D` |
| `void` | `V` |
| 任意对象 `Xxx` | `L包名/Xxx;`（注意分号结尾） |
| 数组 `Xxx[]` | `[Xxx` |

**方法签名格式**：`(参数签名...)返回值签名`

逐字拆解本项目用到的两个签名：

```
"()Lnet/minecraft/client/Minecraft;"
  └┬┘ └──────────┬──────────────┘
  无参数       返回值是 Minecraft 对象
```

```
"()Z"
  └┬┘└┬┘
  无参数 返回值是 boolean
```

再举例：`getDistanceToEntity(Entity)` 返回 float → `(Lnet/minecraft/entity/Entity;)F`
（第 6.3 节练习会用到）。

### 3.8 第五步：读写字段、调用方法

拿到 ID 后，读写就是"对号入座"：

```cpp
// 读实例字段 → jobject
jobject mc = env->CallStaticObjectMethod(mcClass, getMinecraft);  // 调用静态方法
jobject player = env->GetObjectField(mc, thePlayerField);         // 读实例字段
jobject mop    = env->GetObjectField(mc, mopField);

// 读基本类型字段
jdouble x = env->GetDoubleField(entity, posXField);   // double 字段

// 调用返回 boolean 的方法
jboolean alive = env->CallBooleanMethod(entity, isAliveMethod);
```

对应关系：

| 你要干什么 | JNI 函数 |
|---|---|
| 调静态方法返回对象 | `CallStaticObjectMethod` |
| 调实例方法返回 boolean | `CallBooleanMethod` |
| 读对象字段 | `GetObjectField` |
| 读 double 字段 | `GetDoubleField` |
| 读静态对象字段（枚举常量） | `GetStaticObjectField` |

### 3.9 判断类型：IsInstanceOf（相当于 Java 的 instanceof）

```cpp
jboolean isLiving = env->IsInstanceOf(entity, livingClass);
// 等价于 Java:  entity instanceof EntityLivingBase
```

### 3.10 比较对象 / 枚举

- 两个引用是否指向同一个对象 → `env->IsSameObject(a, b)`
  （相当于 Java 的 `a == b`；注意不能直接比较指针数值！）
- 枚举比较：拿到枚举类的 `ENTITY` 静态字段，再 `IsSameObject`：

```cpp
jobject typeObj  = env->GetObjectField(mop, typeOfHitField);   // 准星命中类型
jobject entConst = env->GetStaticObjectField(typeClass, entityConstField); // ENTITY 常量
bool hitEntity = env->IsSameObject(typeObj, entConst);
```

### 3.11 异常处理：JNI 不"抛"异常，只"挂"异常

JNI 的重要特性：**C++ 侧调用 JNI 失败时，不会像 Java 那样抛出异常**，
而是：返回 `NULL`/`0`，并在当前线程挂起一个 pending exception。

所以必须自己检查：

```cpp
jclass mc = env->FindClass("net/minecraft/client/Minecraft");
if (!mc) {
    env->ExceptionClear();   // 清掉挂起的异常，否则后续 JNI 调用全部失败
    return false;            // 走回退逻辑
}
```

项目里每次批量调用后都检查并清异常，防止污染后续调用。

### 3.12 引用管理：局部引用会"泄漏"！

JVM 为每个线程维护一张**局部引用表**，容量至少 16 个。规则：

- 每次 JNI 返回的 `jobject`（包括 `FindClass`、`GetObjectField`、
  `CallXXXMethod` 的返回值）都占用**一个槽位**。
- 槽位**不会自动释放**！不处理的话，循环跑 100 次就溢出了
  （JNI 会报错甚至崩溃）。
- 释放方式：
  1. 单个释放：`env->DeleteLocalRef(ref);`
  2. **成批释放（我们的做法）**：每轮循环开头 `PushLocalFrame`，
     结尾 `PopLocalFrame`，一次性清掉本帧内所有局部引用：

```cpp
while (true) {
    env->PushLocalFrame(32);          // 开辟一个"引用篮子"，容量 32
    // ... 随便创建多少局部引用 ...
    env->PopLocalFrame(NULL);         // 篮子整个扔掉，所有引用一次释放
}
```

另外注意：`GetStringUTFChars` 拿到的 C 字符串必须配 `ReleaseStringUTFChars` 归还。

> 这是本项目最容易踩的坑之一：**不释放局部引用的注入 DLL 会在几分钟内崩掉游戏**。

### 3.13 完整生命周期回顾

```
CreateThread 启动工作线程
   │
   ├─ 等待 jvm.dll 出现（游戏可能还没加载完）
   ├─ JNI_GetCreatedJavaVMs → JavaVM*
   ├─ vm->AttachCurrentThread → JNIEnv*（本线程专用电话）
   ├─ FindClass / GetFieldID / GetMethodID（解析一次，永久使用）
   ├─ 循环 { PushLocalFrame → 读取判定 → 写共享内存 → PopLocalFrame → Sleep(50ms) }
   └─ vm->DetachCurrentThread → 线程退出
```

---

## 第 4 章　逐行读代码：MCCombatStatusJni.dll

> 建议打开 `src/MCCombatStatusJni.cpp` 对照着读。下面按文件顺序讲。

### 4.1 总体架构

```
DllMain (进程加载 DLL 时被 Windows 调用)
   │
   └─ CreateThread → JniWorker 工作线程
                        │
                        ├─ 等 jvm.dll → 拿 JavaVM → AttachCurrentThread
                        ├─ ResolveWith(kMcpMap) 失败就 ResolveWith(kObfMap)
                        └─ while: UpdateStatus() 每 50ms 一次
                                    │
                                    └─ 结果写入共享内存 CombatStatus
```

### 4.2 共享内存结构体：门口的黑板

```cpp
struct CombatStatus {        // 这个结构跨进程共享，所以全部用固定大小类型
    DWORD        magic;         // 魔法数 'MCAK'，用于校验"这是不是我们的黑板"
    DWORD        version;
    DWORD        pid;           // 被注入的进程 ID
    volatile LONG ready;        // JNI 解析成功了吗
    volatile LONG inGame;       // 进入游戏了吗（mc 和 thePlayer 存在吗）
    volatile LONG canAttack;    // ★ 核心结果：当前能不能攻击
    volatile LONG hitType;      // 0=MISS 1=BLOCK 2=ENTITY
    volatile LONG targetLiving; // 目标是否是活物
    volatile LONG targetAlive;  // 目标是否存活
    volatile LONG targetIsPlayer;
    char          targetName[128]; // 目标类名（方便调试）
    volatile LONG tick;         // 更新计数器
    volatile LONG lastError;    // 错误码
};
```

- `volatile`：防止编译器优化掉"看似没用的重复读写"（别的进程在改这块内存）。
- `#pragma pack(push,1)`：禁止结构体对齐填充，保证两个进程看到完全相同的布局。

### 4.3 DllMain：入口与防重复注入

```cpp
BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {        // DLL 刚被加载进进程
        DisableThreadLibraryCalls(hInst);      // 省事：不再关心线程创建/销毁通知

        // 防重复注入：如果共享内存已存在，说明已经有实例在跑了
        HANDLE existing = OpenFileMappingA(FILE_MAP_READ, FALSE, mapName);
        if (existing) { CloseHandle(existing); return TRUE; }   // 直接退出，不开新线程

        HANDLE t = CreateThread(NULL, 0, JniWorker, NULL, 0, NULL);  // 启动工作线程
        if (t) CloseHandle(t);
    }
    else if (reason == DLL_PROCESS_DETACH) {   // 进程退出 / DLL 被卸载
        InterlockedExchange(&g_stop, 1);       // 通知工作线程停止
        Sleep(100);
        // 清理共享内存句柄...
    }
    return TRUE;
}
```

> ⚠️ DllMain 的规矩：Windows 在调用 DllMain 时持有"加载锁"，
> 所以**不能在 DllMain 里直接做复杂操作**（比如 JNI 调用）。
> 标准做法就是像这样：只开一个线程，把活都丢给线程干。

### 4.4 工作线程：等待 JVM 并 attach

```cpp
static DWORD WINAPI JniWorker(LPVOID)
{
    // ① 创建共享内存（名字带 PID，避免多个游戏互相干扰）
    snprintf(mapName, sizeof(mapName), kMapNameFmt, GetCurrentProcessId());
    g_map = CreateFileMappingA(...);  g_status = MapViewOfFile(...);

    // ② 等 jvm.dll：游戏刚启动时 JVM 可能还没加载，最多等 60 秒
    HMODULE jvm = NULL;
    for (int i = 0; i < 600 && !g_stop; ++i) {
        jvm = GetModuleHandleA("jvm.dll");
        if (!jvm) Sleep(100);
    }

    // ③ 拿 JavaVM（见 3.3）
    GetCreatedVMs_t getVMs = (GetCreatedVMs_t)GetProcAddress(jvm, "JNI_GetCreatedJavaVMs");
    ...
    getVMs(&vm, 1, &n);

    // ④ attach 本线程（见 3.4）
    vm->AttachCurrentThread((void**)&env, NULL);

    // ⑤ 解析 + 循环更新
    Resolved res = {};
    while (!g_stop) {
        if (!res.ok) {
            // 解析失败就每 500ms 重试（游戏类可能还在加载）
            if (ResolveWith(env, kMcpMap, res) || ResolveWith(env, kObfMap, res))
                g_status->ready = 1;
            else { Sleep(500); continue; }
        }
        UpdateStatus(env, res);
        Sleep(50);                       // 20 次/秒，足够实时
    }
    vm->DetachCurrentThread();
    return 0;
}
```

### 4.5 映射表 JniMap：两套名字，一套代码

```cpp
struct JniMap {            // 描述"一套命名体系下，所有需要的东西叫什么"
    const char* mcClass;   // Minecraft 类名
    const char* getMinecraft;
    const char* thePlayerField;
    ...（共 18 个成员）
};

static const JniMap kMcpMap = {   // 方案 A：MCP 反混淆客户端
    "net/minecraft/client/Minecraft",
    "()Lnet/minecraft/client/Minecraft;", "getMinecraft",
    "thePlayer", "Lnet/minecraft/client/entity/EntityPlayerSP;",
    "objectMouseOver", "Lnet/minecraft/util/MovingObjectPosition;",
    "net/minecraft/util/MovingObjectPosition",
    "typeOfHit", "Lnet/minecraft/util/MovingObjectPosition$MovingObjectType;",
    "entityHit", "Lnet/minecraft/entity/Entity;",
    "net/minecraft/util/MovingObjectPosition$MovingObjectType", "ENTITY",
    "net/minecraft/entity/Entity", "canAttackWithItem", "isEntityAlive",
    "net/minecraft/entity/EntityLivingBase",
};

static const JniMap kObfMap = {   // 方案 B：原版混淆客户端
    "ave", "()Lave;", "A",
    "h", "Lbew;", "s", "Lauh;",
    "auh", "a", "Lauh$a;", "d", "Lpk;",
    "auh$a", "c", "pk", "aD", "ai", "pr",
};
```

> 这份"字典"就是第 2.3 节那张表的代码形态。
> 想要支持别的版本（比如 1.12），只需要换掉这张表。

### 4.6 ResolveWith：一次性解析全部 ID

```cpp
static bool ResolveWith(JNIEnv* env, const JniMap& m, Resolved& r)
{
    // 找 5 个类
    jclass mc   = env->FindClass(m.mcClass);
    jclass mop  = env->FindClass(m.mopClass);
    jclass type = env->FindClass(m.typeClass);
    jclass ent  = env->FindClass(m.entityClass);
    jclass liv  = env->FindClass(m.livingClass);
    jclass jlc  = env->FindClass("java/lang/Class");   // JDK 自带的类
    if (!mc || !mop || ...) { env->ExceptionClear(); return false; }

    // 找 4 个方法 + 5 个字段（"书签"）
    jmethodID getMc = env->GetStaticMethodID(mc, m.getMinecraft, m.mcSig);
    jmethodID atk   = env->GetMethodID(ent, m.canAttackWithItem, "()Z");
    jmethodID alive = env->GetMethodID(ent, m.isEntityAlive, "()Z");
    jmethodID name  = env->GetMethodID(jlc, "getName", "()Ljava/lang/String;");
    jfieldID  pl    = env->GetFieldID(mc, m.thePlayerField, m.playerFieldSig);
    jfieldID  mopF  = env->GetFieldID(mc, m.mopField, m.mopFieldSig);
    jfieldID  hitF  = env->GetFieldID(mop, m.typeOfHitField, m.typeFieldSig);
    jfieldID  entF  = env->GetFieldID(mop, m.entityHitField, m.entityFieldSig);
    jfieldID  entC  = env->GetStaticFieldID(type, m.entityConstField, m.typeFieldSig);

    if (任何一个为空) { env->ExceptionClear(); return false; }  // 整套方案失败

    r.ok = true;  // 全部塞进 Resolved 结构，供 UpdateStatus 使用
    r.mcClass = mc; r.getMinecraft = getMc; ...
    return true;
}
```

> 注意 `java/lang/Class` 的 `getName()`：
> 我们用它获取目标实体的类名（测试输出里的
> `net.minecraft.entity.monster.EntityZombie` 就是这么来的），纯调试用途。

### 4.7 UpdateStatus：每帧判定逻辑（本项目的心脏）

```cpp
static void UpdateStatus(JNIEnv* env, const Resolved& r)
{
    if (env->PushLocalFrame(32) < 0) { ... }   // 开"引用篮子"

    // ① 拿游戏单例
    jobject mc = env->CallStaticObjectMethod(r.mcClass, r.getMinecraft);
    if (!mc) { /* 游戏还没初始化 */ ...; env->PopLocalFrame(NULL); return; }

    // ② 拿玩家和准星结果
    jobject player = env->GetObjectField(mc, r.thePlayerField);
    jobject mop    = env->GetObjectField(mc, r.mopField);

    // ③ 命中类型判定（0=MISS 1=BLOCK 2=ENTITY）
    jobject typeObj  = env->GetObjectField(mop, r.typeOfHitField);
    jobject entConst = env->GetStaticObjectField(r.typeClass, r.entityConstField);
    int hit = 0;
    if (typeObj && entConst && env->IsSameObject(typeObj, entConst)) hit = 2;
    else if (typeObj) hit = 1;

    // ④ 目标检查（活物？存活？自己？）
    jobject entity = env->GetObjectField(mop, r.entityHitField);
    LONG living = env->IsInstanceOf(entity, r.livingClass);          // instanceof
    LONG alive  = env->CallBooleanMethod(entity, r.isEntityAlive);   // 存活
    LONG isSelf = env->IsSameObject(entity, player);                 // 是不是自己

    // ⑤ 手持物品能否造成伤害
    LONG canUseItem = env->CallBooleanMethod(player, r.canAttackWithItem);

    // ⑥ 汇总 ★
    g_status->canAttack = (hit == 2 && living && alive && !isSelf && canUseItem);

    env->PopLocalFrame(NULL);   // 关"引用篮子"，一次性释放所有局部引用
}
```

这就是 2.2 节公式的 C++ 实现，一行不多，一行不少。

### 4.8 导出函数：给外部程序用的"按钮"

```cpp
extern "C" {   // 用 C 链接方式导出，名字不会被 C++ 修饰

__declspec(dllexport) BOOL WINAPI GetCanAttackNow(void) {
    return (g_status && g_status->ready) ? (BOOL)g_status->canAttack : FALSE;
}
__declspec(dllexport) BOOL WINAPI IsJniReady(void) { ... }
__declspec(dllexport) BOOL WINAPI GetCombatStatus(CombatStatus* out) {
    if (!out || !g_status) return FALSE;
    *out = *g_status;    // 把黑板内容整个拷贝给调用者
    return TRUE;
}

}
```

其他程序（比如你的 Python/易语言/另一个 C++ 程序）加载这个 DLL 后，
直接调用 `GetCanAttackNow()` 就能拿到结果，**完全不需要懂 JNI**。
这就是把"脏活"封装进 DLL 的价值。

---

## 第 5 章　逐行读代码：injector.exe

> 对照 `src/injector.cpp` 阅读。它干三件事：找进程 → 注入 → 监视。

### 5.1 找游戏进程

```cpp
// 遍历所有窗口，找标题含 "Minecraft" 且属于 java.exe/javaw.exe 的窗口
static BOOL CALLBACK EnumWinProc(HWND hwnd, LPARAM lp) {
    if (!IsWindowVisible(hwnd)) return TRUE;       // 跳过隐藏窗口
    char title[256];
    GetWindowTextA(hwnd, title, sizeof(title));
    if (!strstr(title, ctx->sub)) return TRUE;     // 标题不含关键词，跳过
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);          // 窗口属于哪个进程
    if (pid && IsJavaProcess(pid)) { ctx->pid = pid; return FALSE; }  // 找到了！
    return TRUE;
}
```

也可以手动指定：`injector.exe -pid 12345`。

### 5.2 注入（第 1.5 节四步走的代码版）

```cpp
static bool InjectDll(DWORD pid, const char* dllPath)
{
    HANDLE proc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);  // 打开游戏进程

    // 位数检查：64 位注入器不能注入 32 位进程（反之亦然）
    BOOL isWow64 = FALSE;
    if (sizeof(void*) == 8 && IsWow64Process(proc, &isWow64) && isWow64) { 报错 }

    // ① 在游戏进程里分配内存，写入 DLL 路径
    void* mem = VirtualAllocEx(proc, NULL, len, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);
    WriteProcessMemory(proc, mem, dllPath, len, NULL);

    // ② 让游戏进程执行 LoadLibraryA(路径) —— 加载我们的 DLL！
    FARPROC loadLib = GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA");
    HANDLE thread = CreateRemoteThread(proc, NULL, 0,
                                       (LPTHREAD_START_ROUTINE)loadLib, mem, 0, NULL);
    WaitForSingleObject(thread, 10000);   // 等加载完成
    ...
}
```

> LoadLibraryA 执行完，Windows 就会调用我们 DLL 的 DllMain → 工作线程启动。
> 整个注入就完成了。

### 5.3 监视：打开同一块共享内存

```cpp
// 共享内存名 = "Local\MCCombatStatus_" + 游戏 PID（和 DLL 里的规则一致）
snprintf(mapName, sizeof(mapName), kMapFmt, pid);
HANDLE map = OpenFileMappingA(FILE_MAP_READ, FALSE, mapName);

const CombatStatus* s = (CombatStatus*)MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0);

// 循环打印，Ctrl+C 退出
while (true) {
    PrintStatus(s);    // 打印 ready / canAttack / target ...
    Sleep(100);
}
```

> **共享内存原理**：`CreateFileMappingA` 创建一个内核对象，两个进程各自
> `MapViewOfFile` 映射到自己的地址空间，看到的是**同一块物理内存**。
> DLL 往里写，injector 往外读，零拷贝、实时同步。
> 名字带 `Local\` 前缀表示"本登录会话内可见"。

### 5.4 完整数据流全景图

```
┌──────────────┐   注入    ┌──────────────────────────────┐
│  injector.exe │ ────────▶ │  java.exe (Minecraft 1.8.9)   │
│              │           │   ┌────────────────────────┐  │
│ 1. 找窗口     │           │   │ MCCombatStatusJni.dll     │  │
│ 2. 注入 DLL   │           │   │  DllMain               │  │
│ 3. 读共享内存  │           │   │   └─ JniWorker 线程     │  │
│ 4. 打印状态   │           │   │      ├─ JNI 拿 JavaVM   │  │
└──────┬───────┘           │   │      ├─ attach 线程      │  │
       │                   │   │      ├─ 解析类/字段/方法  │  │
       │                   │   │      └─ 每50ms 判定并写入 │  │
       │                   │   └────────────┬───────────┘  │
       │                   └───────────────┼──────────────┘
       │                                   │ 共享内存
       └────────────── 读取 ◀──────────────┘
              Local\MCCombatStatus_<pid>
```

---

## 第 6 章　动手实验与改造练习

### 6.1 编译

双击运行 `build.bat`（需要 MinGW-w64 g++，路径在脚本开头可改）：

```
=== build MCCombatStatusJni.dll ===
=== build injector.exe ===
[OK] done: MCCombatStatusJni.dll + injector.exe
```

### 6.2 端到端测试（不用开游戏！）

`test/` 目录里有一个**假 Minecraft**：类名结构完全模仿 MCP 客户端，
只是内部逻辑是写死的。用来验证"注入 → JNI → 判定 → 共享内存"整条链路。

```bat
:: 1. 编译假客户端
cd test
javac -encoding UTF-8 -d out src\net\minecraft\entity\Entity.java ... src\TestMC.java
   （或直接: javac -encoding UTF-8 -d out src\TestMC.java 让 javac 自动编译依赖）

:: 2. 运行假客户端（它会开一个标题为 "Minecraft 1.8.9" 的窗口，
::    3 秒后"瞄准僵尸"，10 秒后取消瞄准）
java -cp out TestMC

:: 3. 另开一个终端，注入并监视
injector.exe -once
```

预期输出（-once 会等到 JNI 就绪再打印一次）：

```
[READY] tick=3 inGame=1 hit=ENTITY canAttack=1 target=net.minecraft.entity.monster.EntityZombie living=1 alive=1 err=0
```

- 3 秒内注入 → `hit=MISS canAttack=0`
- 3~13 秒注入 → `hit=ENTITY canAttack=1 target=...EntityZombie` ✅
- 13 秒后注入 → `hit=MISS canAttack=0`

**看到 `canAttack=1` 就说明你完全跑通了整条技术链路。**

### 6.3 改造练习 1：读取玩家坐标

目标：在 `CombatStatus` 里加 3 个 double 字段，每帧写入玩家坐标。

**第一步**：结构体加字段（DLL 和 injector 两边要同步改！）

```cpp
// MCCombatStatusJni.cpp 和 injector.cpp 都要加：
double playerX, playerY, playerZ;   // 放在 targetName 后面
```

**第二步**：`Resolved` 加 3 个书签，`ResolveWith` 里解析：

```cpp
// Resolved 结构里加：
jfieldID posXField, posYField, posZField;

// ResolveWith 里加（MCP 名；混淆名为 pk/s, pk/t, pk/u）：
r.posXField = env->GetFieldID(ent, "posX", "D");
r.posYField = env->GetFieldID(ent, "posY", "D");
r.posZField = env->GetFieldID(ent, "posZ", "D");
```

> `D` 就是 double 的签名（附录 B）。`posX` 定义在 `Entity` 上，
> 所以从 `ent`（Entity 类）取。注意 JniMap 里也要加对应名字
> （MCP: `posX/posY/posZ`，混淆: `s/t/u`）——这就是为什么 JniMap
> 设计成一张表，加新功能只需要加表项。

**第三步**：UpdateStatus 里读取：

```cpp
g_status->playerX = env->GetDoubleField(player, r.posXField);
g_status->playerY = env->GetDoubleField(player, r.posYField);
g_status->playerZ = env->GetDoubleField(player, r.posZField);
```

**第四步**：injector 的 PrintStatus 里打印。重新编译，跑 6.2 的测试。

### 6.4 改造练习 2：读取目标血量

`getHealth()` 定义在 `EntityLivingBase` 上，返回 float（签名 `()F`）：

```cpp
// ResolveWith 里：
r.getHealth = env->GetMethodID(liv, "getHealth", "()F");   // MCP 名
// 混淆名是 func_110143_aJ，对应 obf 客户端的方法名需要查表（见 6.6）

// UpdateStatus 里（entity 存在时）：
jfloat hp = env->CallFloatMethod(entity, r.getHealth);
g_status->targetHealth = hp;
```

> 函数命名规律：**方法**在 srg 里是 `func_数字_字母`，
> **字段**是 `field_数字_字母`。看到 `func_110143_aJ` 就知道是方法。

### 6.5 改造练习 3：新增一个导出函数

```cpp
extern "C" __declspec(dllexport) double WINAPI GetPlayerX(void) {
    return g_status ? g_status->playerX : 0.0;
}
```

编译后可用 Python 验证（python 里加载 DLL 调用）：

```python
import ctypes
dll = ctypes.WinDLL(r"C:\Users\11407\Desktop\MCCanAttack-JNI\MCCombatStatusJni.dll")
print("canAttack =", dll.GetCanAttackNow())
```

### 6.6 自己查新名字的方法（重要技能！）

以后想读任何新数据，三步查到名字：

**① 先查 MCP 名**（人话名）—— 打开 MCP 工作区：

```
conf\methods.csv    每行: func_xxxxxx, 方法名, 参数个数, 注释
conf\fields.csv     每行: field_xxxxxx, 字段名, 参数个数, 注释
```

例如 `grep -i "health" conf\fields.csv` 就能找到血量相关字段。

**② 再查混淆名** —— 两个途径：

- 从 srg 反查：`conf\joined.srg` 里
  `FD: pk/s net/minecraft/entity/Entity/field_70165_t`
  表示"混淆名 `pk/s` = Entity 的 posX"。
- 直接反编译官方 jar（最权威）：

```bat
cd jars\versions\1.8.9
unzip -o 1.8.9.jar ave.class   :: 先把 ave.class 解出来
javap -p ave.class             :: 用 JDK 自带的反编译工具看成员
```

`javap` 输出里 `public auh s;` 就是 `objectMouseOver` 的字段定义。
（我们做这个项目时就是这么验证的：`ave/A()Lave;` 确认了 getMinecraft
的混淆名是 `A`。）

**③ 写进 JniMap 表**，然后照 6.3 的模式加解析和读取代码。

### 6.7 多版本适配：本项目 10 套映射是怎么来的

项目当前按顺序自动尝试 10 套映射（`src/MCCombatStatusJni.cpp` 里的 `kAllMaps[]`）：

| # | 标识 | 适用环境 | 关键名字示例 |
|---|---|---|---|
| 1 | `mcp189` | MCP 反混淆 1.8.9 | `thePlayer` / `getMinecraft` / `canAttackWithItem` |
| 2 | `vanilla189` | 原版 1.8.9 | `ave` `h` `s` / `A()` / `aD` `ai` |
| 3 | `forge189` | Forge 1.8.9（少见形态） | `ave` + `field_71439_g` / `func_71410_x` |
| 4 | `forge189mcp` | **Forge 1.8.9 标准**（真机实测） | MCP 类名 + `func_71410_x` / `field_71439_g` |
| 5 | `forge1122` | **Forge 1.12.2**（真机实测） | MCP 类名 + SRG 成员（`func_184614_ca` 取手持） |
| 6 | `vanilla1122` | **原版 1.12.2**（真机实测） | `bib` `h` `s` / `z()` / `vp` `vp` |
| 7 | `vanilla1201` | **原版/Fabric 1.20.1**（真机实测） | `enn` `t` `w` / `N()` / `eO()` |
| 8 | `forge1201obf` | Forge/NeoForge 1.20.1（理论形态） | Mojang 类名 + 混淆成员 |
| 9 | `forge1201stb` | **Forge/NeoForge 1.20.1 标准**（真机实测） | Mojang 类名 + `m_91087_` / `m_21205_` |
| 10 | `forge1201` | Forge/NeoForge 1.20.1（其他形态） | `getInstance` / `player` / `hitResult` / `isAlive` |

每套映射其实就是一张"名字字典"（`JniMap` 结构体），解析时从头到尾
依次尝试，第一套全部解析成功的就生效（状态里的 `map=` 字段可看到）。

1.20.1 的两套名字是**上网查证**的：从 Mojang 官方下载 1.20.1 客户端 jar 和
官方映射文件（`client_mappings`），用 `javap` 反编译 + 映射表逐项核对。
查证结果存在 `verify/client-1.20.1-mappings.txt`。

**1.20.1 与 1.8.9 的两个结构差异**（代码里为它们做了适配）：

1. `entityHit` 不再是公开字段：1.20.1 里 `EntityHitResult` 的实体是
   **私有字段 + getter**（`getEntity()`），所以 `JniMap` 同时支持
   "字段方式"和"getter 方式"两种取数。
2. `canAttackWithItem()` 在 1.20.1 **已不存在**，改用目标的 `isAttackable()`
   作为"可否攻击"检查；`JniMap` 里这两项都可空，为空就跳过。

> 想支持其他版本（如 1.19.2、1.20.4）：下载对应版本的官方映射文件，
> 用同样的方法查表，在 `kAllMaps[]` 里加一套即可。
> 1.17+ 的 Forge/NeoForge 都是 Mojang 官方名，大概率直接复用第 9/10 套。

---

## 第 7 章　常见坑与 FAQ

### Q1：注入后一直显示 `WAIT`（ready=0）

- 游戏还没进到主界面/世界？（`Minecraft` 类没加载，解析会一直重试）
- 注入太早？DLL 会等 jvm.dll 最多 60 秒，再等等。
- **位数不匹配**？64 位游戏必须配 64 位 DLL（我们的默认产物是 64 位）。
  32 位 Java 要用 `g++ -m32` 重编。
- 被安全软件拦截了？注入是敏感操作，杀软可能静默拦截，加白名单试试。

### Q2：`canAttack` 一直是 0

- 没瞄准生物（看 `hit` 是不是 `ENTITY`）
- 目标不是 `EntityLivingBase`（比如瞄准的是掉落物）
- 目标死了 / 手持食物等不能攻击的物品（`canAttackWithItem` 为 false）
- 目标是你自己（不可能，但逻辑上排除了）

### Q3：游戏崩溃了

- 最常见原因：**局部引用泄漏**（3.12）。确认每轮循环都 Push/PopLocalFrame。
- 在 DllMain 里做了 JNI 调用（违反 4.3 的规矩）。
- JVM 已销毁后还调用 JNI（进程退出瞬间）。
- 注意：任何注入行为都可能被反作弊系统检测，**仅限单机/自建服务器学习使用**。

### Q4：为什么 DLL 里找不到 `jvm.lib` 链接？

不需要链接。`JNI_GetCreatedJavaVMs` 是**动态**从已加载的 `jvm.dll`
里 `GetProcAddress` 取的（3.3 节），所以编译时零依赖。

### Q5：为什么字段要写完整类型签名？不能只写名字吗？

`GetFieldID` 的签名必须和 Java 源码里**声明的类型**完全一致。
Java 允许重载，名字不能唯一确定目标；签名就是 JNI 的"精确寻址"。

### Q6：为什么枚举比较用 `IsSameObject` 而不是比较指针？

JNI 返回的 `jobject` 是"引用"，不是对象地址本身；
两个引用可能指向同一个对象但数值不同。比较对象身份一律用 `IsSameObject`。

### Q7：为什么要 `AttachCurrentThread`？DLL 不是已经在游戏进程里了吗？

在进程里 ≠ 在线程里。JNIEnv 是**线程级**的。我们的工作线程是
`CreateThread` 创建的原始线程，JVM 对它一无所知；
必须 attach 后 JVM 才会给它分配 JNIEnv、局部引用表等资源。

### Q8：50ms 轮询会不会卡游戏？

不会。JNI 调用本身开销极小（微秒级），而且我们只读几个字段。
游戏每 tick（50ms）本来就在更新 objectMouseOver，节奏刚好。

### Q9：为什么用共享内存而不是直接调 DLL 导出函数？

导出函数需要"跨进程调用"（要在目标进程里执行代码），复杂且容易被杀软盯上；
共享内存只是读写一块内存，**任何语言**（Python/C#/易语言…）都能
`OpenFileMapping` 读取，简单可靠。所以我们两条路都提供了。

### Q10：想支持别的 Minecraft 版本？

换一张映射表：用 6.6 的方法查出新版本的混淆名/MCP 名，改 `kMcpMap`/`kObfMap`
即可。类结构（`objectMouseOver`/`typeOfHit`）在 1.8~1.12 基本一致；
1.13+ 改动较大，需要重新分析类结构。

---

## 附录 A　JNI 函数速查表

| 用途 | 函数 | 备注 |
|---|---|---|
| 拿 JVM | `JNI_GetCreatedJavaVMs` | 从 jvm.dll 动态获取 |
| 线程接入 | `vm->AttachCurrentThread` / `DetachCurrentThread` | 外来线程必须 |
| 找类 | `FindClass("a/b/C")` | 失败返回 NULL |
| 找字段 | `GetFieldID` / `GetStaticFieldID` | 需要类型签名 |
| 找方法 | `GetMethodID` / `GetStaticMethodID` | 需要方法签名 |
| 读字段 | `GetObjectField` / `GetDoubleField` / `GetIntField` / `GetBooleanField` | 类型对应函数 |
| 写字段 | `SetObjectField` / `SetDoubleField` … | 本项目没用到 |
| 调方法 | `CallObjectMethod` / `CallBooleanMethod` / `CallFloatMethod` / `CallStaticObjectMethod` … | 返回类型对应函数 |
| 类型判断 | `IsInstanceOf` | instanceof |
| 身份比较 | `IsSameObject` | `==` |
| 对象类 | `GetObjectClass` | 得到 jclass |
| 异常 | `ExceptionCheck` / `ExceptionClear` | 必须成对检查 |
| 引用 | `DeleteLocalRef` / `PushLocalFrame` / `PopLocalFrame` | 防泄漏 |
| 字符串 | `GetStringUTFChars` / `ReleaseStringUTFChars` | 记得归还 |

> 完整列表：JDK 文档搜索 "JNI Functions"（见附录 D）。

## 附录 B　类型签名（Descriptor）速查

| Java | 签名 | Java | 签名 |
|---|---|---|---|
| boolean | `Z` | float | `F` |
| byte | `B` | double | `D` |
| char | `C` | void | `V` |
| short | `S` | 对象 | `L包/名;` |
| int | `I` | 数组 | `[元素签名` |
| long | `J` | 方法 | `(参数...)返回` |

本项目实例：

```
()Z                          → 无参返回 boolean（isEntityAlive / canAttackWithItem）
()Lnet/minecraft/client/Minecraft;  → 无参返回 Minecraft（getMinecraft）
()Ljava/lang/String;         → 无参返回 String（Class.getName）
(Lnet/minecraft/entity/Entity;)F   → 一个 Entity 参数返回 float（getDistanceToEntity）
```

## 附录 C　1.8.9 本项目用到的全部名字对照表

（表内 SRG 名也是 Forge 1.8.9 运行时的成员名）

| 含义 | 混淆名 | SRG 名 | MCP 名 |
|---|---|---|---|
| Minecraft 类 | `ave` | `net/minecraft/client/Minecraft` | 同左 |
| 获取单例(静态) | `A` | `func_71410_x` | `getMinecraft` |
| 玩家字段 | `h` | `field_71439_g` | `thePlayer` |
| 准星结果字段 | `s` | `field_71476_x` | `objectMouseOver` |
| MovingObjectPosition 类 | `auh` | 同左包名 | 同左 |
| 命中类型字段 | `a` | `field_72313_a` | `typeOfHit` |
| 命中实体字段 | `d` | `field_72308_g` | `entityHit` |
| 枚举类 | `auh$a` | `...$MovingObjectType` | 同左 |
| 枚举常量 ENTITY | `c` | 同左 | `ENTITY` |
| Entity 类 | `pk` | `net/minecraft/entity/Entity` | 同左 |
| 能否攻击方法 | `aD` | `func_70075_an` | `canAttackWithItem` |
| 是否存活方法 | `ai` | `func_70089_S` | `isEntityAlive` |
| EntityLivingBase 类 | `pr` | 同左包名 | 同左 |
| 玩家 X 坐标字段 | `s`(pk内) | `field_70165_t` | `posX` |
| 玩家 Y 坐标字段 | `t` | `field_70163_u` | `posY` |
| 玩家 Z 坐标字段 | `u` | `field_70161_v` | `posZ` |
| 血量方法 | （在 pr 上） | `func_110143_aJ` | `getHealth` |
| 最大血量方法 | （在 pr 上） | `func_110138_aP` | `getMaxHealth` |
| 实体 ID 方法 | （在 pk 上） | `func_145782_y` | `getEntityId` |
| 距离方法 | （在 pk 上） | `func_70032_d` | `getDistanceToEntity` |

### 1.20.1 名字对照（本项目第 7/10 套映射）

> 已通过 Mojang 官方映射文件 + jar 反编译双重验证（见 `verify/`）。
> 原版/Fabric 运行时用混淆名（`vanilla1201` 表），Forge/NeoForge 运行时用官方名（`forge1201` 表）。

| 含义 | 混淆名 (原版/Fabric) | 官方名 (Forge/NeoForge) |
|---|---|---|
| Minecraft 类 | `enn` | `net/minecraft/client/Minecraft` |
| 获取单例(静态) | `N` | `getInstance` |
| 玩家字段 | `t` (类型 `fiy`) | `player` (类型 `LocalPlayer`) |
| 准星结果字段 | `w` (类型 `eeg`) | `hitResult` (类型 `HitResult`) |
| HitResult 类 | `eeg` | `net/minecraft/world/phys/HitResult` |
| 命中类型(getter) | `c()` → `eeg$a` | `getType()` → `HitResult$Type` |
| 枚举常量 ENTITY | `eeg$a.c` | `HitResult$Type.ENTITY` |
| EntityHitResult 类 | `eef` | `net/minecraft/world/phys/EntityHitResult` |
| 命中实体(getter) | `a()` → `bfj` | `getEntity()` → `Entity` |
| Entity 类 | `bfj` | `net/minecraft/world/entity/Entity` |
| 存活方法 | `bs` | `isAlive` |
| 可攻击方法 | `cn` | `isAttackable` |
| LivingEntity 类 | `bfz` | `net/minecraft/world/entity/LivingEntity` |
| LocalPlayer 类 | `fiy` | `net/minecraft/client/player/LocalPlayer` |

> 注意 1.20.1 与 1.8.9 的两处差异：`entityHit` 是私有字段（必须用
> `getEntity()` getter）；`canAttackWithItem()` 已不存在（改用目标的
> `isAttackable()`）。

## 附录 D　延伸学习

**官方文档**
- Oracle JNI 规范（必读，讲得最清楚）：搜索 "JNI Specification"
- JDK 自带示例：`jdk-24\demo`（新版 JDK 可能不带，可看老版本）

**本书对应的知识点地图**

| 你想学 | 看哪里 |
|---|---|
| JNI 函数细节 | 本文 3 章 + Oracle JNI 规范 |
| 注入原理 | 本文 1.5、5 章 + 搜索 "DLL injection CreateRemoteThread" |
| Windows 共享内存 | 本文 4.2、5.3 + 搜索 "CreateFileMapping MapViewOfFile 教程" |
| MCP 反混淆 | 本文 1.2、6.6 + mcp918 的 conf 目录 |
| Minecraft 类结构 | 用 javap 反编译 1.8.9.jar 自己探索（6.6） |

**练习路径建议**
1. ✅ 跑通 6.2 的测试（看到 canAttack=1）
2. ✅ 完成 6.3 坐标读取
3. 尝试 6.4 血量
4. 挑战：读取玩家当前手持物品名（提示：`thePlayer.getHeldItem()` → `ItemStack` → `getDisplayName()`，MCP 名在 methods.csv 里查）
5. 挑战：把 20Hz 轮询改成"事件驱动"（提示：Hook `Minecraft.runTick`）

> 最后提醒：注入与游戏修改可能违反服务器规则，**请仅用于单机学习**。
> 祝学习愉快！🎮
