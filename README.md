# MCCombatStatus-JNI — Minecraft 战斗状态检测工具 (C++ JNI DLL 注入)

> 原项目名 **MCCanAttack-JNI**（仓库/目录沿用历史名称）。
> V63 起 DLL、共享内存、导出函数全面更名（见"接口变更"一节），
> 外部程序需同步适配。
> V66 起注入器改为**手动映射**（无 LoadLibrary / 无模块链表条目 /
> 载荷加密内嵌），DLL **导出表已剥离**（见"接口变更"）。

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

通过向运行中的 Minecraft (java/javaw 进程) 注入 DLL，每 5ms 读取
`Minecraft` 单例的 `thePlayer` / `objectMouseOver`，判断玩家当前
**是否瞄准到了一个可以攻击的生物**（`canAttack`），并检测**手持物品
是否为放置物**（`canPlace`，1.8~1.16.5 的 ItemBlock / 1.14+ 的
BlockItem），通过 UDP 向本机 35785 端口持续上报 2 字节：
`[byte0=canAttack '1'/'0'][byte1=canPlace '1'/'0']`，同时把结果写入
共享内存供外部程序读取。

## 支持的版本 / 客户端

映射表由 `tools/gen_maps.py` 从 `mappings-extracted`（1.8.8~1.21.11，
6 命名空间）自动生成 **171 张表**，覆盖 **54 个版本 × 4 种命名空间**，
无需任何配置。DLL 先按 **classpath 里的版本号**定位到对应版本，再按顺序
尝试该版本的 vanilla → forge → mojang → intermediary 表；注入后
`map=` 字段显示命中的表。

| 运行时形态 | 版本 | 命名空间 | 验证 |
|---|---|---|---|
| **S1 字段式**（typeOfHit/entityHit 为字段） | 1.8.8 ~ 1.13.2 | vanilla=混淆名 / forge=MCP 类名+SRG `func_/field_` | ✅ 1.8.9、1.12.2（真机+diff） |
| **S2 getter 式**（getType/getEntity） | 1.14 ~ 1.16.5 | vanilla=混淆名 / forge=MCP+SRG / intermediary=`class_/method_/field_`(Fabric) | ✅ 原版 1.14、Forge 1.16.5、Fabric 1.16.5 真机 |
| **S3 getter 式** | 1.17 ~ 1.21.11 | vanilla=混淆名 / forge=Mojang+stable `m_/f_`(1.17~1.20.1) / mojang=全 Mojang(NeoForge 1.20.2+) / intermediary(Fabric) | ✅ Forge 1.20.1、NeoForge 1.20.4/1.21/1.21.8/1.21.11、Fabric 1.21.11 真机 |

> **环境感知起始表**（V65.1）：按探测到的 mod 加载器直接定位对应命名空间表
> （NeoForge→mojang / Forge→forge / Fabric→intermediary），首次解析从
> 全表轮询的 10~30 秒缩短到 **<1 秒**；失败仍回退全表。

> **真机实测命名规律（重要，详见《技术文档》第六章）**：
> - **原版启动器**：永远混淆名（1.14=`cvi`、1.20.1=`enn`）
> - **Forge 1.8~1.16.5**：类名转 **MCP 名** + 成员转 **SRG 名**（`func_71410_x`）
> - **Forge 1.17~1.20.1**：类名转 **Mojang 名** + 成员转 **MCP stable 名**（`m_91087_`）
> - **NeoForge 1.20.2+**：类名+成员**全 Mojang 官方名**（`getInstance`）
> - **Fabric（1.14+）**：全 **Intermediary 名**（`net/minecraft/class_310` + `method_1551`）

## 判定逻辑

```
canAttack = (objectMouseOver != null)
         && (typeOfHit == ENTITY)              // 准星命中实体
         && (目标 instanceof EntityLivingBase)  // 目标是活物
         && (目标.isEntityAlive())             // 目标存活
         && (目标 != 玩家自己)
         && (canAttackWithItem() || 版本无此检查)  // 1.8.9: 手持物品能造成伤害
         && (isAttackable()  || 版本无此检查)      // 1.20.1: 目标可被攻击

canPlace = (player.getHeldItem() / getMainHandItem() != null)   // 手持有物品
        && (stack.getItem() instanceof ItemBlock / BlockItem)   // 是放置物
```

`canPlace` 只依赖玩家手持物品，与准星无关（未瞄准时仍正常上报）。
放置物成员在所有映射中为**可选解析**——某环境下解析失败仅 `canPlace`
恒为 0，**不影响 canAttack**。

## 文件结构

```
MCCombatStatus-JNI/
├── build.bat                 一键编译 (需要 MinGW-w64 g++)
├── MCCombatStatusJni.dll     注入到游戏进程的 JNI 工具 DLL (检测 + UDP 上报, V66 无导出)
├── injector.exe              V66 手动映射注入器 (DLL XOR 加密内嵌, 注入即退出, 不落盘)
├── include/                  JNI/JVMTI 头文件
├── src/
│   ├── MCCombatStatusJni.cpp 注入 DLL 源码 (映射表自动生成 + 环境探测 + UDP 上报)
│   ├── mc_maps_generated.h   自动生成的映射表 (54 版本 171 张, 勿手改)
│   ├── payload.h             自动生成的加密载荷 (build.bat 生成, 勿手改)
│   └── injector.cpp          V66 手动映射注入器源码
├── tools/
│   ├── gen_maps.py           映射表生成器 (从 mappings-extracted 生成 + 查询 CLI)
│   ├── smoke_test.py         端到端冒烟测试 (假客户端+注入+UDP 采样)
│   └── smoke_test_hook.py    V65 hook 版冒烟测试 (渲染线程调 SwapBuffers 假客户端)
├── test/                     假客户端测试 (7 套: 各命名体系一套)
│   └── swapclient/           V65 hook 版假客户端 (Java 线程每 10ms 调 gdi32!SwapBuffers)
│       └── native/swapstub.cpp  调用真实 SwapBuffers 的辅助库 (必须 extern "C")
├── verify/                   验证工具 (jmap/jcmd 记录等)
│   └── hooktest.cpp          SwapBuffers 钩子链路独立验证 (追跳存根/槽位替换)
├── JNI零基础教学.md           零基础入门教程
├── 技术文档.md                总思路 + 架构原理 + 调试历程 + Forge 规律
└── 避坑指南.md                JNI/注入开发避坑清单
```

## 使用方法

1. 启动游戏（上述任一环境），进入世界。
2. 运行 `injector.exe`（单文件即可，DLL 已加密内嵌，**不落盘**），注入即退出：
   ```
   injector.exe                       自动查找 Minecraft 窗口并注入 (嵌入载荷)
   injector.exe -pid <PID>            手动指定进程 (注意: javapath 垫片会派生真 JVM, 要注入子进程)
   injector.exe -title <子串>         按窗口标题子串查找 (默认 "Minecraft")
   injector.exe -dll <路径>           用指定明文 DLL 文件替代嵌入载荷
   ```
3. DLL 注入后在游戏渲染帧内检测（帧驱动、5ms 节流，不再有自己的采集线程），
   并向本机 **35785 端口 (UDP)** 持续发送
   2 字节：
   ```
   byte0 = 0x31 ('1') = 当前可以攻击准星所指的生物
          0x30 ('0') = 不可以
   byte1 = 0x31 ('1') = 手持物品是放置物 (ItemBlock/BlockItem)
          0x30 ('0') = 不是或空手
   ```
   接收方无需应答，直接收 UDP 包即可。**byte0 与旧版 1 字节协议完全
   一致**，旧接收端（只读 byte0）无需改动；新接收端 `recvfrom(2)`
   一次拿到两个状态。

> 旧版的实时状态显示与 probe.log 诊断日志已移除；需要调试信息时可通过
> 共享内存 `Local\MCCombatStatus_<pid>` 读取（字段含义见下文）。

## 导出函数

V66 起 DLL **不再导出任何函数**（导出表已剥离，缩小检测面）。外部程序
只需通过共享内存 `Local\MCCombatStatus_<pid>` (结构见
`MCCombatStatusJni.cpp` 顶部) 读取状态，无需调用 DLL 函数。

## 接口变更 (V63 / V66)

V63 起项目外部接口全面更名（原 `MCCanAttack` 前缀 → `MCCombatStatus`）：

| 项 | 旧名 (V62-) | 新名 (V63+) |
|---|---|---|
| DLL 文件 | `MCCanAttackJni.dll` | `MCCombatStatusJni.dll` |
| 源码文件 | `src/MCCanAttackJni.cpp` | `src/MCCombatStatusJni.cpp` |
| 共享内存 | `Local\MCCanAttackStatus_<pid>` | `Local\MCCombatStatus_<pid>` |
| 导出函数 | `CanAttackNow()` / `GetCanAttackStatus()` | `GetCanAttackNow()` / `GetCombatStatus()` |
| 状态结构 | `CanAttackStatus` (magic `'MCAK'`, v6) | `CombatStatus` (magic `'MCST'`, v7) |

UDP 35785 协议（2 字节 `[canAttack][canPlace]`）不变。调用 DLL 函数
或读共享内存的外部程序需按新名适配。

### V66（2026-08）

| 项 | 旧 (V65-) | 新 (V66+) |
|---|---|---|
| 注入方式 | CreateRemoteThread + LoadLibrary | **手动映射**（无 LoadLibrary / 无 LoadImage 回调 / PEB 模块链表无条目 / 载荷 XOR 加密内嵌不落盘） |
| 导出函数 | `GetCanAttackNow()` / `IsJniReady()` / `GetCombatStatus()` | **全部剥离**（共享内存是唯一接口） |
| 进程句柄 | 全权限 | 最小权限 (CREATE_THREAD\|QUERY_INFORMATION\|VM_OPERATION\|VM_WRITE\|VM_READ) |
| 反重复注入 | — | 共享内存 `Local\MCCombatStatus_<pid>` 健康检查 |
| DLL 内部 | 明文字符串 + 有导出 + PEB 可见 | 字符串 XOR 混淆 + 导出剥离 + PEB 摘链/名称抹除 + PE 头内存擦除 |

## 测试方法（不需要开真实游戏）

`test/` 下有 7 套假客户端，分别模拟 7 种运行时的类名/成员名结构
（mcp189 / vanilla189 / forge189 / forge1122 / vanilla1122 / vanilla1201 / forge1201）。
以 forge1201 为例：

```bat
cd test\forge1201
javac -encoding UTF-8 -d out TestMC1201F.java net\minecraft\client\Minecraft.java ^
      net\minecraft\client\player\LocalPlayer.java net\minecraft\world\entity\player\Player.java ^
      net\minecraft\world\entity\LivingEntity.java net\minecraft\world\entity\Entity.java ^
      net\minecraft\world\phys\HitResult.java net\minecraft\world\phys\EntityHitResult.java ^
      net\minecraft\world\item\ItemStack.java net\minecraft\world\item\Item.java ^
      net\minecraft\world\item\BlockItem.java
java -cp out TestMC1201F        :: 另开终端运行下一条
injector.exe                    :: 注入后, 在本机 35785 端口收 UDP 2 字节即通过
                                :: (可用 python 监听: python -c "import socket;s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);s.bind(('127.0.0.1',35785));d,a=s.recvfrom(64);print('canAttack=%s canPlace=%s'%(d[0:1],d[1:2]))" 循环执行)
```

**注意**：假客户端用 JDK 普通启动（单一类加载器），无法暴露
Forge/launchwrapper 的多加载器问题——**真机验证不可替代**（本项目的
核心 bug 全部是真机才暴露的）。

**自动化冒烟测试**（推荐替代手工测试）：`tools/smoke_test.py` 一键完成
"启动假客户端 → 自动找窗口注入 → 读共享内存 + UDP 采样"，按阶段校验
`canAttack/canPlace` 组合是否符合预期。注意它依赖假客户端的
`Minecraft` 窗口标题，且 JDK 版本需与假客户端匹配。

**V65 hook 版冒烟测试**（验证帧驱动链路）：`tools/smoke_test_hook.py` 一键完成
"编译启动 swapclient（Java 线程每 10ms 调真实 gdi32!SwapBuffers）→ 注入 →
读共享内存 + UDP 采样"；`verify/hooktest.exe`（build 后运行）独立验证
SwapBuffers 钩子链路本身（追跳存根 → 槽位原子替换 → 调用转发），不涉及 JNI。

## 重新编译

```
build.bat
```

（需要 MinGW-w64 g++，路径可在 build.bat 中修改；JNI 头文件已内置在 include/）

## 映射表自动生成 (54 版本)

`src/mc_maps_generated.h` 由 `tools/gen_maps.py` 从
`D:\VibeCoding\mappings-extracted`（1.8.8~1.21.11，6 命名空间）自动生成，
**不要手改**。生成 171 张表，覆盖四种运行时形态：

| 形态 | 版本 | vanilla (原版) | forge (FML/NeoForge) | mojang | intermediary (Fabric) |
|---|---|---|---|---|---|
| S1 字段式 | 1.8.8~1.13.2 | 混淆名 | MCP 类名 + SRG `func_/field_` | — | — |
| S2 getter 式 | 1.14~1.16.5 | 混淆名 | MCP 类名 + SRG `func_/field_` | — | class_/method_/field_ |
| S3 getter 式 | 1.17+ | 混淆名 | Mojang 类名 + stable `m_/f_` | 官方名 | class_/method_/field_ |

关键结论（已用数据验证）：SRG 名 `func_71410_x`(getMinecraft) 在 1.8.8~1.16.5
全程不变；stable 名 `m_91087_`(getInstance) 在 1.17~1.21.11 全程不变——
所以新增版本基本"查表即可"，无需再手写。
**注意**：Intermediary 名（Fabric）**并非**完全跨版本稳定（如 `ItemStack.getItem`
在 1.14 是 `method_7949`、1.21.11 是 `method_7909`），因此 DLL 靠 JVM classpath
里的版本号（如 `...\versions\1.21.11\1.21.11.jar`）来定位到正确版本的表。

```bash
# 重新生成 (改动了生成器或数据后)
python tools\gen_maps.py --emit

# 查一个名字跨 6 命名空间叫什么 (取代 grep lzma/mcp_config)
python tools\gen_maps.py --query 1.20.1 getInstance
python tools\gen_maps.py --query 1.12.2 func_184614_ca

# 打印某版本解析结果 (人工核对)
python tools\gen_maps.py --report --versions 1.16.5 1.21.11
```

> 数据目录路径 `BASE` 在 `gen_maps.py` 顶部，换机器时改这一处即可。

## 原理说明 (V65: 无线程/无 Attach 的帧驱动架构)

1. `injector.exe` 找到 java/javaw 进程后，用
   `CreateRemoteThread + LoadLibraryA` 注入 `MCCombatStatusJni.dll`。
2. DLL 入口（DllMain）**不创建任何线程**：只初始化共享内存与 UDP socket，
   并钩住 `gdi32!SwapBuffers` —— 游戏自己的 Client thread（Java 线程）
   每帧渲染都调用它（LWJGL2 的 WindowsDisplay 与 LWJGL3/GLFW 的 WGL
   后端最终都调 `gdi32!SwapBuffers`）。钩子安装时追跳存根链（FF 25/E9），
   对热补丁槽位做 8 字节原子指针替换（已处理"槽位惰性解析"问题）。
3. 钩子内用 `GetEnv()` **复用**该渲染线程已有的 JNIEnv（Java 线程天然
   attached）——**绝不调用 `AttachCurrentThread`**：那会在 JVM 里注册一个
   "外来原生线程"并触发 ThreadStart 事件，网易版等游戏侧保护发现后直接
   退出游戏（旧版 V64- 恰恰是 "CreateThread 采集线程 + AttachCurrentThread"
   组合，即被强杀的导火索）。
4. 解析与采样由**帧驱动状态机**在渲染线程内完成：环境探测 → 加载器定位
   （Launch.classLoader / 线程遍历，可分帧续跑）→ 按 classpath 版本号定位
   后依次尝试映射表 → findLoadedClass 终极修正 → 采样（5ms 节流）。
   每帧工作预算 8ms，解析失败下帧续跑，绝不阻塞渲染线程。
5. 跨帧保留的 jclass/jobject 一律 `NewGlobalRef` 提升（本地引用在 native
   帧返回时即失效）；离开钩子前清空 pending exception；UDP 2 字节 + 共享
   内存 `Local\MCCombatStatus_<pid>` 的**协议与 V63/V64 完全一致**。
6. 任何程序监听 35785 端口即可获得实时状态（无需调用 DLL 函数）；
   也可读取共享内存获取完整状态（含 map/env 等诊断字段）。

## 原理说明 (V66: 手动映射注入 + 反检测加固)

1. `injector.exe` 把 DLL 以 **XOR 加密形式内嵌**在自身（`src/payload.h`，
   由 `build.bat` 调 `tools/gen_payload.ps1` 生成），运行时解密到内存，
   **全程不落盘**；`-dll <路径>` 可改用指定明文 DLL 文件。
2. 注入流程：解析 PE → 目标进程 `VirtualAllocEx(RW)` 分配映像大小缓冲 →
   按节区 **RVA 布局**拷贝（节区文件偏移≠RVA，这是首个大坑）→ 应用基址
   重定位（DIR64）+ **MinGW 伪重定位**（`___RUNTIME_PSEUDO_RELOC_LIST__`，
   COFF 符号表定位，含 `-O2` 下空表情形）→ 启发式补修无标准重定位条目的
   `.rdata` 绝对指针（值落在首选基址映像范围内）→ 本地解析导入
   (KERNEL32/msvcrt/WS2_32) 写入 IAT → 入口存根（`PAGE_EXECUTE_READWRITE`）
   远程线程执行 `DllMain(base, DLL_PROCESS_ATTACH)` → 节区权限改 RX。
   全程**不调用 LoadLibrary**：无 LoadImage 回调、PEB 模块链表无条目。
3. `OpenProcess` 仅申请最小权限；注入前检查共享内存
   `Local\MCCombatStatus_<pid>` 是否健康（反重复注入）。
4. DLL 本体反检测（V66）：敏感字符串 XOR 混淆（volatile 数组防 `-O2`
   常量折叠）、导出表剥离、PEB 摘链 + 全/基名抹除、PE 头内存擦除。
5. 游戏内检测/上报架构与 V65 完全一致（帧驱动 SwapBuffers 钩子、无线程、
   无 AttachCurrentThread）；共享内存 + UDP 35785 协议不变。
   验证：swapclient 假客户端上 `injector.exe -pid <真JVM>` 手动映射注入
   后 `ready=1 map=mojang1201`，阶段状态全部正确。

> 手动映射的两个经典坑（详见《避坑指南》）：映射映像必须按 RVA 布局、
> 节区拷贝必须读文件缓冲而非 RVA 缓冲；MinGW 的 `.refptr` 绝对指针没有
> 标准重定位条目，需要"值落在首选基址范围内"的启发式补修。

> 完整原理、真机排查过程与踩坑记录见《技术文档.md》和《避坑指南.md》。
>
> **网易中国版**：已在 1.20.1 Forge 端真机注入验证（2026-08-14）——游戏
> 存活、状态读取正确（详见《技术文档》8.7）。注意其窗口标题是服务器名
> （不是 "Minecraft"），`injector.exe` 需 `-pid` 或 `-title` 注入；长期
> 使用仍建议自行观察（反作弊可能还有更长周期的扫描）。
