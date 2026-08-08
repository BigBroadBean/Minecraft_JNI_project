# verify/ 目录说明

本目录是开发调试过程中的临时工作区，**不随仓库推送**。

## 内容

- `probe.log` — 注入 DLL 的运行时探测日志（逐类/逐成员探测结果）
- `stage/` — 历史版本 DLL（dll_Vxx），用于二分回归定位
- `client-1.20.1-mappings.txt` — **Mojang 官方映射文件，版权禁止再分发**
- 其他调试临时文件

## 排除原因

1. 包含 **Mojang 官方映射**（`client-1.20.1-mappings.txt`），
   其许可协议明确禁止完整再分发
2. 大量调试临时产物（几十个历史 DLL、日志），不适合进版本库
3. 体积大（50MB+）

## 需要的映射文件去哪找

- **1.17+ 的 stable 名**：Forge 安装目录
  `libraries/de/oceanlabs/mcp/mcp_config/<版本>/mcp_config-<版本>-mappings-merged.txt`
- **1.8-1.12 的 SRG 名**：Forge 库 jar 内的
  `deobfuscation_data-<版本>.lzma`（python lzma 可解）
- **1.20.1 官方混淆名**：Mojang version manifest 的 `client_mappings` 下载项
