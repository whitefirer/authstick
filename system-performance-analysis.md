# 系统性能诊断报告

**时间**: 2026-06-01 21:06 CST  
**主机**: tenbox-vm, Debian 13 (trixie), Linux 6.12.73+deb13-amd64  
**CPU**: 18 核 (x86_64) | **RAM**: 7.8 GB | **Swap**: 0  
**症状**: 系统严重卡顿，宿主机磁盘持续高 I/O，现已恢复

---

## 1. 负载分析

| 指标 | 值 | 正常范围 | 状态 |
|------|-----|----------|------|
| Load 1min | 1.95 | <18 | 正常 |
| Load 5min | 41.32 | <18 | **超 2.3×** |
| Load 15min | 66.80 | <18 | **超 3.7×** |
| 可用内存 | 1.5 GB | >2 GB | 紧张 |
| 空闲内存 | 487 MB | >1 GB | 紧张 |

15 分钟负载 66.80 代表平均有 **49 个进程同时排队等待资源**（66.80 - 18 核 = ~49）。

## 2. 根因：内存耗尽 + 无 Swap → 页面抖动

### 2.1 机制链

```
4× claude + VS Code + Chromium + chroma-mcp + claude-mem + copaw + milvus ...
                              ↓
                     7.8 GB RAM 全部耗尽
                              ↓
                  无 swap，内核别无选择
                              ↓
             只回收文件页（干净页丢弃，脏页强制回写）
                              ↓
           进程立即访问被回收页 → 缺页异常 → 从磁盘读回
                              ↓
         另一进程需要更多内存 → 再次回收 → 死循环（thrashing）
                              ↓
         磁盘 I/O 持续拉满，CPU 大量时间空转等 I/O → 负载飙升
```

### 2.2 为什么 swap = 0 加剧问题

有 swap 时，内核可以将**匿名页**（堆、栈、malloc）换出到 swap，保留文件页缓存。匿名页通常不会被频繁访问，换出后影响小。

无 swap 时，内核**只能回收文件页**（可执行代码、共享库、mmap 文件）。这些恰好是进程运行**需要频繁访问**的页面，刚回收就被访问，陷入"回收 → 读回 → 再回收"的死循环。

## 3. 内存占用分解

采集时间点占用最大的进程：

| 进程 | RSS | 数量 | 合计 |
|------|-----|------|------|
| Xorg | 577 MB | 1 | 577 MB |
| claude | 314-460 MB | 4 | ~1.5 GB |
| chroma-mcp (python) | 456 MB | 1 | 456 MB |
| copaw (python) | 425 MB | 1 | 425 MB |
| VS Code (主进程) | 380 MB | 1 | 380 MB |
| milvus | 302 MB | 1 | 302 MB |
| Chromium | 172-225 MB | 3+ | ~500 MB |
| claude-context-mcp (node) | 120-131 MB | 3+ | ~370 MB |
| bun (claude-mem) | 73-76 MB | 3+ | ~220 MB |
| VS Code (子进程) | 143-191 MB | 2+ | ~330 MB |
| chromium gpu/renderer | 188-225 MB | 2 | ~410 MB |
| 其他 (docker, minio, etcd) | ~ | ~ | ~400 MB |

**总计**: ~5.9 GB 常驻内存 + ~1.6 GB 文件缓存 ≈ **7.5 GB / 7.8 GB**

剩余 ~300 MB 留给内核和突发分配，任何额外内存需求即触发回收。

## 4. 磁盘 I/O 分解

vda 自启动累计：**读取 278 GB**，**写入 9 GB**，读/写比 = 30:1。

### 4.1 读取大户

| 进程 | 累计读取 | 占比(累计) |
|------|----------|-----------|
| claude (PID 2155, 运行最久) | 46 GB | 36.5% |
| VS Code (PID 285179, 主进程) | 13 GB | 10.3% |
| bun worker-service (PID 2374, claude-mem 常驻) | 8.5 GB | 6.7% |
| VS Code zygote (PID 285185) | 7.7 GB | 6.1% |
| Chromium (PID 350174) | 5.7 GB | 4.5% |
| claude (PID 40979) | 4.7 GB | 3.7% |
| copaw (PID 855) | 4.5 GB | 3.6% |
| claude (PID 161046) | 4.3 GB | 3.4% |
| Chromium renderer (PID 350292) | 3.9 GB | 3.1% |
| python3 server.py (PID 282393) | 3.8 GB | 3.0% |
| claude-context-mcp (PID 41195) | 3.3 GB | 2.6% |
| 其余 ~30 进程 | ~20 GB | ~16% |

**前 4 名占比**: claude (46G) + VS Code (13G) + bun (8.5G) + VS Code (7.7G) = 75.2 GB / 可追溯 126 GB ≈ 60%

剩余 ~152 GB 来自已退出/已回收的进程碎片和内核自身 I/O。

### 4.2 写入大户

| 进程 | 累计写入 | 说明 |
|------|----------|------|
| claude (PID 2155) | 5.0 GB | session 日志、状态文件 |
| chroma-mcp (PID 2544) | 588 MB | 向量索引、sqlite |
| bun worker-service (PID 2374) | 523 MB | claude-mem 守护进程 |
| 其他 | ~3 GB | 分散写入 |

chroma-mcp 写入占比最高（588M / 1.3G 读取 = 45% 写入比），向量数据库特性决定。

## 5. 磁盘缓存与数据目录

| 目录 | 大小 | 说明 |
|------|------|------|
| ~/.cache/uv | 6.2 GB | Python 包缓存 |
| ~/.cache/pip | 2.7 GB | pip 包缓存 |
| ~/.cache/go-build | 2.7 GB | Go 构建缓存 |
| ~/.cache/camoufox | 1.3 GB | 浏览器指纹工具 |
| ~/.claude/projects/.../sessions/ | 515 MB | MCP 会话日志 |
| ~/.claude-mem/chroma/ | 150 MB | 向量数据库 |
| ~/.cache/chromium | 335 MB | 浏览器缓存 |
| ~/.cache/Espressif | 599 MB | ESP-IDF 工具链 |
| ~/.cache/ms-playwright | 641 MB | 自动化测试 |
| **合计** | **~15.8 GB** | |

16 GB 缓存占据磁盘空间，内存压力时这些目录的元数据本身也在竞争页缓存。

## 6. 线程与上下文切换压力

| 进程 | 线程数 | 说明 |
|------|--------|------|
| copaw | 144 | 大量线程，异步框架 |
| chroma-mcp | 77 | Python 线程池 |
| milvus | 62 | 向量数据库 |
| Chromium GPU | 47 | GPU 加速 |
| VS Code | 41 | 编辑器扩展 |
| claude | 24-29 | CLI 工具 |
| chromium | 37 | 浏览器 |

总计 500+ 线程在 18 核上竞争调度，内存压力下上下文切换成本更高（TLB 刷新、缓存失效）。

## 7. 时间线重建

```
时刻 T-?      系统正常运行
              ↓
时刻 T        内存使用接近上限，内核开始频繁回收文件页
              ↓
T + 几分钟   某个进程请求大块内存 → 触发激进回收
              → 页面抖动开始
              → 磁盘灯常亮，系统响应变慢
              → 负载从正常值开始攀升
              ↓
T + 10-30min 抖动持续，负载攀升至 40-66
              所有进程 I/O 等待增加
              UI 卡顿，终端响应延迟
              ↓
T + ~30min   某个/某些重进程退出或内存释放
              → 压力缓解
              → 负载开始下降
              ↓
当前 (T+?)   负载 1.95，已基本恢复
              但 4 个 claude + milvus 仍在运行
```

## 8. 建议措施

### 立即执行

```bash
# 1. 创建 8GB swap（最直接有效）
sudo fallocate -l 8G /swapfile
sudo chmod 600 /swapfile
sudo mkswap /swapfile
sudo swapon /swapfile
echo '/swapfile none swap sw 0 0' | sudo tee -a /etc/fstab
```

### 短期优化

- **限制 claude 并发数**: 同时最多 2 个实例
- **停止 milvus**: 不使用时 `sudo systemctl stop milvus` 或 kill PID 388384
- **停止 chroma-mcp**: 不使用时停掉，节省 456 MB
- **清理 claude-mem session**: `~/.claude/projects/-home-tenbox--claude-mem-observer-sessions/` 下旧文件可归档
- **清理 VS Code 缓存**: `~/.cache/uv` (6.2G) 和 `~/.cache/pip` (2.7G) 考虑清理

### 长期

- **升级内存**: 当前负载下 7.8 GB 明显不足，建议 ≥16 GB
- **配置 swap 永久生效**: 已包含在上方 fstab 命令中
- **监控脚本**: 设置内存/负载告警阈值

---

*此报告基于 2026-06-01 21:06 系统快照生成。所有进程级 I/O 数据来自 /proc/[pid]/io，为进程自启动以来的累计值。*
