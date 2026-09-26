# AgentOS 底座 v0.2 — Agent 原语原生下沉方案 (2026-09-26)

## 一、目标 (从第一性原理)

让 Agent 的执行引擎从 proot 用户态 Linux 下沉到 Android 内核态原生用户态：

- **现在**: Operit APP → proot Linux → bash → shell 命令 (多层转发, 高开销)
- **目标**: Operit APP → AgentCore (Android 原生 ELF) → 直接 syscall (一层, 零开销)

## 二、现状验证结果 (2026-09-26 实测)

### ✅ 已跑通
- AgentCore v0.2 (Python) 在 proot 内正常监听 127.0.0.1:9990
- 10 个 agent 原语全部工作: ping/shell/file_read/file_write/http/process_list/process_kill/ui/screenshot/device_info
- LLM function-calling 的 JSON schema 自动生成 (/schemas 端点)
- 通过 HTTP POST JSON 调用

### ⚠️ 未达成 (关键)
- 仍在 proot 用户态 (uid=0 但其实是 proot 的 root)
- Android 原生环境没有 Python, 只有 toybox/nc/curl
- 真正的 Android 原生 shell 是 uid=2000, 通过 Shizuku 获得

## 三、社区最新骚操作 (2026-09 实时抓取)

### 模式 1: 临时 exploit + LKM late-load (最主流)
- **GhostLock CVE-2026-43499**: rtmutex UAF + DirtyPipe 凭证 patch → KernelSU LKM
- **CVE-2025-21479-FX5P**: KGSL 物理 R/W → disarm guard → ksud late-load
- 特点: 重启即消失, 完美用后即焚

### 模式 2: DSU + GSI (免刷 root)
- **Dsu-Manager**: DSU + GSI 双管齐下, 不刷 bootloader
- 特点: 需要 Google OTA 通道, 部分 ROM 不支持

### 模式 3: KernelSU LKM 生态 (长期 root)
- **MakoSU**: KMI-aware LKM + SuSFS + KPM
- **ApexSU**: Rust 用户态 + KernelSU + 隐蔽性
- **SUSFS-LKM**: SUSFS 也 LKM 化
- 特点: 需要 root 或解锁 bootloader

### 模式 4: eBPF 内核监控 (Agent 感知)
- **ecapture** (15.4k★): SSL/TLS 抓包, Android arm64
- **Chiral**: eBPF 事件绑定到 APK, 追踪任意 APP

## 四、完整升级路线图

### v0.1: Bash + FIFO (已否决)
- ❌ FIFO 阻塞, JSON 转义不稳, toybox nc 有限制

### v0.2: Python + HTTP + proot (当前) ✅
- ✅ 10 原语全跑通, 快速验证架构
- ⚠️ 仍在 proot 内, 非真正原生
- 🎯 用途: 架构验证 + LLM 决策层对接

### v0.3: C 静态 ELF + Unix socket (下一步)
```c
// 目标: 编译成 Android arm64 静态 ELF (~200KB)
// 部署: /data/local/tmp/agentcore
// 运行: 通过 Shizuku 或 shell 直接启动
// 权限: uid=2000 (真正的 Android shell 权限)
// 依赖: 仅 libc.bionic (Android 原生)
```
- 静态编译: `musl-gcc` 或 NDK
- Unix socket 替代 HTTP (性能 10x)
- 隐藏进程名 (prctl PR_SET_NAME)
- 零外部依赖

### v0.4: Rust + binder 服务 (Android 原生)
```rust
// 用 binder::Node / BinderService 直接注册 binder 服务
// Operit APP 通过 binder transaction 调用
// 完全融入 Android IPC 生态, 与 shell 命令平级
```
- Rust + Android bionic
- binder Node + AIDL
- 与 Android 系统原生进程无差别

### v0.5: DSU 模块 + root (需要 DSU 通道)
- Magisk DSU 部署 AgentCore
- 获得真正 root 权限
- 不刷 bootloader, Play Integrity 通过
- 卸载 DSU 即恢复

### v0.6: KernelSU LKM + eBPF (终极)
- KernelSU LKM 提供持久 root
- eBPF 内核监控 (对标 ecapture/Chiral)
- Agent 具备内核态感知能力
- 三层渗透 (内核 + 系统服务 + 应用)

### v0.7: 临时 exploit + LKM late-load (最骚)
- 免 root 触发 exploit 拿 root
- 立即加载 LKM 持久化
- 任务完成卸载 LKM, 用后即焚
- 对标 GhostLock CVE-2026-43499

## 五、代码资产

```
/sdcard/Download/Operit/AgentOS/
├── src/
│   ├── agentcore.py       # v0.2 Python 版 (已跑通)
│   └── (待补) agentcore.c  # v0.3 C 静态版
├── bin/                    # 编译产物
├── build/                  # 构建脚本
└── docs/                   # 本文档
```

## 六、Agent 原语清单 (v0.2 已实现)

| 原语 | 用途 | LLM 可直接调用 |
|------|------|----------------|
| ping | 心跳 | ✅ |
| shell | Shell 命令执行 | ✅ |
| file_read | 读取文件 (≤1MB) | ✅ |
| file_write | 写文件 (含 base64) | ✅ |
| http | HTTP 请求 (任意 headers) | ✅ |
| process_list | 列进程 (pattern 过滤) | ✅ |
| process_kill | 杀进程 (SIGTERM/KILL) | ✅ |
| ui | UI 自动化 (tap/swipe/text/key) | ✅ |
| screenshot | 截屏 | ✅ |
| device_info | 设备信息 | ✅ |

### 未来原语 (v0.3+)
- `net_monitor` — 抓包 (需要 eBPF)
- `ssl_capture` — SSL 明文 (需要 eBPF + ecapture)
- `selinux_check` — SELinux 状态查询
- `app_install` — 安装 APK (需要 root)
- `app_uninstall` — 卸载 APK
- `app_freeze` — 冻结 APP
- `system_backup` — 系统快照
- `system_restore` — 系统恢复

## 七、下一步决策

### 优先级 P0 (今天)
- [x] 验证架构可行性 ✅
- [ ] 写 C 版 AgentCore 静态 ELF (v0.3)
- [ ] 交叉编译配置 (NDK 或 musl)

### 优先级 P1 (本周)
- [ ] v0.3 C 版部署到 /data/local/tmp
- [ ] 通过 Shizuku 启动原生 shell 权限
- [ ] 与 Operit APP 对接 (替换 proot bash)

### 优先级 P2 (本月)
- [ ] 搭建 DSU 环境 (需要验证 Redmi Note 10 Pro 兼容性)
- [ ] 编译 KernelSU LKM for 4.14 内核
- [ ] 部署 AgentCore 到 DSU 模块

### 优先级 P3 (下个季度)
- [ ] eBPF backport for 4.14
- [ ] 临时 exploit 适配 (CVE-2025-21479 / CVE-2026-43499)
- [ ] 三层渗透完整实现

## 八、关键坑记录

1. **proot 内的 /data/local/tmp 是伪路径**
   - 从 proot 里看 /data/local/tmp 是虚构的
   - 真正落到 Android 需要 /sdcard 共享路径
   - 或直接绕过 proot

2. **Android 原生没有 Python**
   - toybox 有 nc/curl, 但无 python
   - 静态 ELF 是唯一原生方案
   - 或用 Rust 静态编译

3. **Shizuku vs Root**
   - Shizuku: shell 权限 (uid=2000), 不需要 root
   - Root: uid=0, 需要 Magisk/KernelSU
   - MVP 阶段用 Shizuku 就够

4. **Android 4.14 内核 eBPF 能力有限**
   - tracepoint/kprobe 可用
   - LSM hooks/sockmap/ring buffer 不可用
   - 需要 backport (参考 acroreiser/linux-4.19.325)

---

**总结**: AgentCore v0.2 证明了架构可行, 但仍在 proot 内。真正的下沉需要写 C 版 v0.3。
DSU + LKM + eBPF 是终极方案, 但需要分阶段推进。
