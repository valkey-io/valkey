# 内存数据库的"阿喀琉斯之踵"：从 Valkey PR #1032 看 Redis 集群在磁盘故障场景下的可用性问题

> 本文通过分析 Valkey 项目的一个历时 15 个月的 Pull Request，深入探讨 Redis/Valkey 集群在磁盘只读或故障场景下的可用性问题，揭示内存数据库中一个容易被忽视的设计缺陷。

## 一、引言：一个"反直觉"的故障场景

作为运维或开发人员，你可能会认为：**既然 Redis 是内存数据库，那么即使禁用了 RDB 和 AOF 持久化，它也应该完全不依赖磁盘，对吧？**

答案是：**错。**

2024 年 9 月，Valkey 社区开发者 enjoy-binbin 提交了一个 PR（[#1032](https://github.com/valkey-io/valkey/pull/1032)），揭示了一个令人惊讶的事实：

> **即使你禁用了所有持久化功能，当磁盘发生故障（如磁盘满、只读、IO 错误）时，Redis/Valkey 集群节点仍然会立即退出，可能导致整个集群不可用。**

这个 PR 从提交到合并，历时整整 **15 个月**，期间经历了激烈的技术讨论，涉及多位核心维护者。本文将通过分析这个 PR，深入探讨 Redis/Valkey 集群架构中这个容易被忽视的可用性问题。

---

## 二、问题背景：nodes.conf 的"致命"角色

### 2.1 什么是 nodes.conf？

在 Redis/Valkey 集群模式下，每个节点都会维护一个名为 `nodes.conf` 的配置文件，用于存储集群的元数据信息，包括：

- 所有节点的 ID、IP 地址和端口
- 每个节点负责的槽位（slots）范围
- 节点的角色（主节点/从节点）
- 节点的状态（正常/故障）
- 当前的配置纪元（config epoch）

### 2.2 问题的根源：clusterSaveConfigOrDie

在原有的代码实现中，存在一个名为 `clusterSaveConfigOrDie` 的函数。从名字就可以看出它的行为模式：**保存配置，否则死亡（die）**。

```c
// 原有的致命逻辑
void clusterSaveConfigOrDie(int do_fsync) {
    if (clusterSaveConfig(do_fsync) == C_ERR) {
        serverLog(LL_WARNING, "Unable to update cluster config file.");
        exit(1);  // 直接退出进程！
    }
}
```

这个函数会在以下场景被调用：

- 节点加入或离开集群
- 槽位迁移
- 故障转移（failover）
- 主从切换
- 任何导致集群配置变更的操作

### 2.3 灾难场景

假设你有一个部署在物理机上的 Redis 集群，多个实例共享同一块磁盘。某天，磁盘出现故障变为只读状态：

```
┌─────────────────────────────────────────────────────────┐
│                     物理服务器                           │
│  ┌─────────┐  ┌─────────┐  ┌─────────┐  ┌─────────┐    │
│  │ Redis-1 │  │ Redis-2 │  │ Redis-3 │  │ Redis-4 │    │
│  │  :7001  │  │  :7002  │  │  :7003  │  │  :7004  │    │
│  └────┬────┘  └────┬────┘  └────┬────┘  └────┬────┘    │
│       │            │            │            │          │
│       └────────────┴─────┬──────┴────────────┘          │
│                          │                              │
│                    ┌─────▼─────┐                        │
│                    │   磁盘    │ ← 只读/故障            │
│                    │ (共享)   │                        │
│                    └───────────┘                        │
└─────────────────────────────────────────────────────────┘
```

当任何一个节点尝试更新 `nodes.conf` 时：

1. **Redis-1** 检测到某个节点故障，尝试更新配置 → **写入失败** → **进程退出**
2. **Redis-2** 收到集群状态变更消息，尝试更新配置 → **写入失败** → **进程退出**
3. **Redis-3**、**Redis-4** 同样的命运...

**结果：几秒钟内，整台服务器上的所有 Redis 实例全部宕机，集群雪崩。**

---

## 三、社区讨论：一场持续 15 个月的技术辩论

### 3.1 核心争议点

这个看似简单的修复背后，隐藏着深层次的架构设计问题。社区讨论主要围绕以下几个核心问题展开：

#### 问题一：是否应该引入配置开关？

**enjoy-binbin（PR 作者）** 最初提议引入一个配置项 `cluster-ignore-disk-write-errors`：

> "这个配置与 `replica-ignore-disk-write-errors` 保持一致，让用户可以选择是否在磁盘写入失败时退出。"

**PingXie（核心成员）** 提出了担忧：

> "如果配置写入失败，重启后可能会读到非常陈旧的配置，造成'无界过期'（unbounded staleness）。虽然 epoch 机制能处理大部分问题，但引入新配置增加了很多需要推理的状态。"

#### 问题二：元数据过期的风险有多大？

这是讨论中最核心的技术问题。如果 `nodes.conf` 无法更新，节点重启后会读取到过时的配置。最坏情况下可能导致：

- **Epoch 碰撞**：节点可能在同一个 epoch 中投票两次
- **槽位映射错误**：客户端可能被路由到错误的节点
- **脑裂**：网络分区恢复后，集群可能出现不一致状态

**enjoy-binbin** 对此进行了风险分析：

> "元数据过期的风险极小。一方面，存在 `CLUSTER_WRITABLE_DELAY` 逻辑（2 秒延迟），可以防止主节点在不安全的情况下重新加入集群。最坏情况是节点在同一 epoch 中投票两次，但这类似于故障转移场景，只会丢失少量写入，且非常罕见。"

#### 问题三：是否需要"无盘集群模式"？

**zuiderkwast（贡献者）** 提出了一个更激进的想法：

> "是否应该有一个完全不读写 `nodes.conf` 的'无盘集群模式'？对于 Kubernetes 等有外部控制平面的环境，根本不需要持久化的 `nodes.conf`。"

这个提议获得了部分支持，但也有反对意见：

**PingXie** 指出：

> "无盘模式将配置管理委托给外部组件，不适合那些喜欢'自包含'部署和崩溃后自动重启的用户。"

### 3.2 用户认知断层

讨论中最发人深省的是 enjoy-binbin 指出的"用户认知断层"问题：

> **"普通用户认为 Redis/Valkey 是内存数据库，在禁用持久化（RDB 和 AOF）后，很难接受因为配置文件保存失败导致的集群故障。"**

这确实是一个认知上的盲区。大多数用户会认为：

- ✅ 禁用 RDB = 不写 dump.rdb
- ✅ 禁用 AOF = 不写 appendonly.aof
- ❌ 禁用持久化 = 完全不依赖磁盘 **（错误！）**

实际上，即使禁用了所有持久化功能，`nodes.conf` 的写入仍然是强制的，而且是同步阻塞的。

### 3.3 IO 延迟问题

除了故障场景，enjoy-binbin 还提到了性能问题：

> "fsync 卡顿可能导致 100ms+ 甚至几秒的延迟，这对性能是致命的。我们遇到过主线程被 fsync 阻塞超过 100ms 的情况。"

**PingXie** 对此建议：

> "未来可以将这些写入改为异步，以便可以对 `nodes.conf` 更新进行超时处理。"

---

## 四、最终解决方案

经过长达 15 个月的讨论，社区最终达成了共识：**不再让节点因为配置保存失败而退出**。

### 4.1 核心代码变更

引入了新函数 `clusterSaveConfigOrLog`，替代原有的 `clusterSaveConfigOrDie`：

```c
#define CONFIG_SAVE_LOG_ERROR_RATE 30 /* 日志限流：每 30 秒最多记录一次 */

void clusterSaveConfigOrLog(int do_fsync) {
    if (clusterSaveConfig(do_fsync) == C_ERR) {
        static time_t last_save_error_log = 0;
        /* 限制日志频率，防止日志刷屏 */
        if ((server.unixtime - last_save_error_log) > CONFIG_SAVE_LOG_ERROR_RATE) {
            serverLog(LL_WARNING, 
                "Cluster config updated even though writing "
                "the cluster config file to disk failed.");
            last_save_error_log = server.unixtime;
        }
    }
}
```

### 4.2 安全机制：safe_to_join

为了防止过时配置导致的问题，还引入了 `safe_to_join` 机制：

```c
// 在 clusterState 结构体中新增字段
struct clusterState {
    // ... 其他字段
    int safe_to_join; /* 重启的节点是否可以安全加入集群？ */
};
```

这个机制确保：
- 重启后的节点不会立即参与故障转移投票
- 需要等待一定时间确认集群状态后才能"安全加入"

### 4.3 修改后的行为对比

| 场景 | 修改前 | 修改后 |
|------|--------|--------|
| 磁盘写入失败 | 进程立即退出 | 记录警告日志，继续运行 |
| 日志输出 | 无（直接退出） | 限流输出（每 30 秒一次） |
| 集群可用性 | 可能雪崩 | 保持可用 |
| 管理员响应时间 | 无（已宕机） | 有时间进行故障排查和迁移 |

---

## 五、深入分析：设计哲学的权衡

### 5.1 一致性 vs 可用性

这个 PR 本质上是 CAP 定理在实践中的体现。原有设计更倾向于一致性（Consistency）：

- **优先保证配置一致性**：宁可退出，也不允许配置不一致
- **悲观策略**：假设配置不一致会导致严重问题

修改后的设计更倾向于可用性（Availability）：

- **优先保证服务可用**：即使配置可能过时，也要保持服务
- **乐观策略**：相信 epoch 机制能够处理大部分不一致情况

### 5.2 自愈 vs 人工干预

原有设计假设：

> "配置文件损坏 = 不可恢复的严重错误 = 必须人工介入"

但实际生产环境中，磁盘故障往往是临时的：

- 磁盘满了可以清理
- 文件系统只读可以重新挂载
- IO 错误可能是暂时的

新设计给了管理员更多的响应时间，而不是立即"自杀"。

### 5.3 显式配置 vs 隐式行为

讨论中一个有趣的演变是关于是否需要配置开关：

```
初期想法：引入 cluster-ignore-disk-write-errors 配置
    ↓
中期讨论：考虑 cluster-persist-config 配置（sync/async/off）
    ↓
最终决定：直接修改默认行为，不引入新配置
```

最终选择不引入配置的原因：

1. **减少认知负担**：用户不需要了解另一个配置项
2. **合理的默认值**：保持服务可用是更合理的默认行为
3. **简化测试矩阵**：减少需要测试的配置组合

---

## 六、生产环境建议

基于这个 PR 的分析，我们可以得出以下生产环境建议：

### 6.1 监控磁盘健康状态

即使升级到修复后的版本，磁盘故障仍然会导致配置无法持久化。建议：

```bash
# 监控磁盘空间
df -h /var/lib/redis/

# 监控磁盘 IO 错误
dmesg | grep -i "error\|fail" | grep -i "sd\|nvme"

# 监控文件系统状态
mount | grep "ro,"  # 检查是否有只读挂载
```

### 6.2 监控 Valkey/Redis 日志

修复后的版本会输出警告日志，应该配置告警：

```bash
# 关键告警日志
grep "Cluster config updated even though writing.*failed" /var/log/redis/redis.log
```

### 6.3 避免单点故障

```yaml
# 推荐的部署架构
- 不同物理机部署主从节点
- 使用独立的磁盘或 SSD
- 考虑使用 tmpfs 或 RAM disk 存储 nodes.conf（如果可以接受重启后重新加入集群）
```

### 6.4 Kubernetes 环境特别注意

在 Kubernetes 环境中，可以考虑：

```yaml
# 使用 emptyDir 存储 nodes.conf
volumes:
  - name: cluster-config
    emptyDir: {}
    
# 或者使用 PVC，但要注意存储后端的可靠性
volumes:
  - name: cluster-config
    persistentVolumeClaim:
      claimName: redis-config-pvc
```

---

## 七、总结

### 7.1 关键发现

1. **Redis/Valkey 集群并非完全的内存数据库**：即使禁用所有持久化，`nodes.conf` 的写入仍然是必需的。

2. **磁盘故障可能导致集群雪崩**：在修复之前，单个磁盘故障可能导致同一服务器上的所有实例同时退出。

3. **设计权衡是复杂的**：这个看似简单的 Bug 背后，涉及一致性 vs 可用性、自愈 vs 人工干预等深层次的架构决策。

4. **社区协作的价值**：15 个月的讨论虽然漫长，但确保了最终方案的合理性和完整性。

### 7.2 版本建议

- **Valkey 用户**：建议升级到 9.1 及以上版本
- **Redis 用户**：关注 Redis 社区是否有类似的修复（截至本文写作时，Redis 主仓库尚未合并类似修复）

### 7.3 延伸思考

这个 PR 也引发了一些更深层次的思考：

- **是否需要完全的无盘集群模式？**
- **配置文件是否应该与状态分离？**
- **异步写入是否是更好的方案？**

这些问题可能会在未来的版本中得到解答。正如 zuiderkwast 在审查中所说：

> "Let's get this merged. It is sad that a fix like this can be stale for a very long time."
> 
> （让我们合并这个修复吧。一个这样的修复被搁置这么长时间，实在是令人遗憾。）

---

## 参考资料

- [Valkey PR #1032: Fail to save the cluster config file will not exit the process](https://github.com/valkey-io/valkey/pull/1032)
- [Redis Cluster Specification](https://redis.io/docs/reference/cluster-spec/)
- [Valkey Official Documentation](https://valkey.io/docs/)

---

*本文基于 Valkey PR #1032 的公开讨论整理，感谢 enjoy-binbin、zuiderkwast、PingXie、madolson、hpatro 等社区贡献者的精彩讨论。*

---

**作者**：[Your Name]  
**日期**：2026 年 3 月  
**版权**：本文采用 CC BY-SA 4.0 协议发布
