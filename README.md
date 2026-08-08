# MCCanAttack-JNI — Minecraft 能否攻击检测工具 (C++ JNI DLL 注入)

通过向运行中的 Minecraft (java/javaw 进程) 注入 DLL，每 50ms 读取
`Minecraft` 单例的 `thePlayer` / `objectMouseOver`，判断玩家当前
**是否瞄准到了一个可以攻击的生物**，并把结果写入共享内存供外部程序读取。

## 支持的版本 / 客户端

| 标识 | 适用环境 | 命名体系 | 验证方式 |
|---|---|---|---|
| `mcp189` | MCP 反混淆客户端 1.8.9 | MCP 名 | 假客户端测试 ✅ |
| `vanilla189` | 原版启动器 1.8.9 | 混淆名 (ave/A/h/s) | 假客户端测试 ✅ + 官方 jar 反编译核对 |
| `forge189` | Forge 1.8.9（类名混淆+成员SRG，少见形态） | 混淆类名 + SRG 成员 | 假客户端测试 ✅ |
| `forge189mcp` | **Forge 1.8.9 标准运行时** | **MCP 类名 + SRG 成员** | **真机实测 ✅ (PCL+OptiFine)** |
| `forge1122` | **Forge 1.12.2** | **MCP 类名 + SRG 成员**（RayTraceResult 改名） | **真机实测 ✅** |
| `vanilla1201` | 原版 / Fabric 1.20.1 | 官方混淆名 | 假客户端测试 ✅ + Mojang 官方映射核对 |
| `forge1201obf` | Forge/NeoForge 1.20.1（理论形态） | Mojang 类名 + 混淆成员 | 假客户端测试 ✅ |
| `forge1201stb` | **Forge/NeoForge 1.20.1 标准运行时** | **Mojang 类名 + MCP stable 成员** | **真机实测 ✅** |
| `forge1201` | Forge/NeoForge 1.20.1（其他形态） | Mojang 官方名 | 假客户端测试 ✅ + Mojang 官方映射核对 |

DLL 按上表顺序自动尝试 8 套命名，**无需任何配置**。注入后 `map=` 字段显示命中的体系。

> **重要发现（真机实测）**：
> - **Forge 1.8.9**：FML 运行时反混淆 = **类名转 MCP 名**（`net.minecraft.client.Minecraft`）+ **成员转 SRG 名**（`func_71410_x`）
> - **Forge 1.20.1**：ModLauncher = **类名用 Mojang 官方名**（`net.minecraft.client.Minecraft`）+ **成员用 MCP stable 名**（`m_91087_` 格式）
> 两个版本都既不是纯混淆也不是纯官方名——这就是本项目踩坑最多的部分（详见《技术文档》复盘）。

## 判定逻辑

```
canAttack = (objectMouseOver != null)
         && (typeOfHit == ENTITY)              // 准星命中实体
         && (目标 instanceof EntityLivingBase)  // 目标是活物
         && (目标.isEntityAlive())             // 目标存活
         && (目标 != 玩家自己)
         && (canAttackWithItem() || 版本无此检查)  // 1.8.9: 手持物品能造成伤害
         && (isAttackable()  || 版本无此检查)      // 1.20.1: 目标可被攻击
```

## 文件结构

```
MCCanAttack-JNI/
├── build.bat                 一键编译 (需要 MinGW-w64 g++)
├── MCCanAttackJni.dll        注入到游戏进程的 JNI 工具 DLL
├── injector.exe              注入器 + 实时状态监视器
├── include/                  JNI/JVMTI 头文件
├── src/
│   ├── MCCanAttackJni.cpp    注入 DLL 源码 (6 套映射表 + 环境探测 + 诊断)
│   └── injector.cpp          注入器源码
├── test/                     假客户端测试 (5 种命名体系各一套)
├── verify/                   验证工具 (jmap/jcmd 记录、probe.log 诊断日志)
├── JNI零基础教学.md           零基础入门教程
├── 技术文档.md                总思路 + 架构原理 + 调试历程 + Forge 规律
└── 避坑指南.md                JNI/注入开发避坑清单
```

## 使用方法

1. 启动游戏（上述任一环境），进入世界。
2. 运行 `injector.exe`（与 DLL 同一目录）：
   ```
   injector.exe                       自动查找 Minecraft 窗口并注入
   injector.exe -once                 注入后打印一次状态即退出
   injector.exe -pid <PID>            手动指定进程
   injector.exe -dll <路径>           指定 DLL 文件 (绕过同名缓存)
   injector.exe -title <子串>         按窗口标题查找 (默认 "Minecraft")
   ```
3. 控制台实时显示：
   ```
   [READY] env=forge+optifine+launchwrapper  map=forge189mcp  tick=123 inGame=1
           hit=ENTITY canAttack=1 target=net.minecraft.entity.monster.EntityCreeper living=1 alive=1 err=0
   ```
   - `env`       : 环境探测（forge/optifine/fabric/launchwrapper）
   - `map`       : 命中的命名体系
   - `canAttack` : **核心结果，1 = 当前可以攻击准星所指的生物**
   - `err/errMsg`: 失败详情（解析失败时显示具体类名+异常）

## 导出函数 (供其他程序调用)

| 函数 | 说明 |
|---|---|
| `BOOL CanAttackNow()` | 直接返回当前是否能攻击 |
| `BOOL IsJniReady()` | JNI 是否已就绪 |
| `BOOL GetCanAttackStatus(CanAttackStatus*)` | 拷贝完整状态结构（含 map/env 字段） |

其他程序可通过共享内存 `Local\MCCanAttackStatus_<pid>` (结构见
`MCCanAttackJni.cpp` 顶部) 读取状态，无需调用 DLL 函数。

## 测试方法（不需要开真实游戏）

`test/` 下有 5 套假客户端，分别模拟 5 种运行时的类名/成员名结构。
以 forge1201 为例：

```bat
cd test\forge1201
javac -encoding UTF-8 -d out TestMC1201F.java net\minecraft\client\Minecraft.java ^
      net\minecraft\client\player\LocalPlayer.java net\minecraft\world\entity\player\Player.java ^
      net\minecraft\world\entity\LivingEntity.java net\minecraft\world\entity\Entity.java ^
      net\minecraft\world\phys\HitResult.java net\minecraft\world\phys\EntityHitResult.java
java -cp out TestMC1201F        :: 另开终端运行下一条
injector.exe -once              :: 看到 map=forge1201 canAttack=1 即通过
```

**注意**：假客户端用 JDK 24 普通启动（单一类加载器），无法暴露
Forge/launchwrapper 的多加载器问题——**真机验证不可替代**（本项目的
核心 bug 全部是真机才暴露的）。

## 重新编译

```
build.bat
```

（需要 MinGW-w64 g++，路径可在 build.bat 中修改；JNI 头文件已内置在 include/）

## 原理说明

1. `injector.exe` 找到 java/javaw 进程后，用
   `CreateRemoteThread + LoadLibraryA` 注入 `MCCanAttackJni.dll`。
2. DLL 内工作线程通过 `JNI_GetCreatedJavaVMs` 拿到 JavaVM 并 `AttachCurrentThread`，
   找到游戏类加载器（`Launch.classLoader`，注意字段类型是 `LaunchClassLoader`！），
   用 JNI 反射解析 Minecraft 类/字段/方法 ID（6 套映射依次尝试）。
3. 每 50ms 计算一次 canAttack，写入共享内存 `Local\MCCanAttackStatus_<pid>`，
   同时把探测结果写入 `verify/probe.log`（诊断用）。
4. `injector.exe`（或任何程序）读取共享内存即可获得实时状态。

> 完整原理、真机排查过程与踩坑记录见《技术文档.md》和《避坑指南.md》。
