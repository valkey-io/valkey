# Redis 6.2 与 Valkey 9.0 命令兼容性整理

> 本文基于当前仓库 `src/commands/*.json` 的实际命令定义整理。Valkey 由 Redis 7.2.4 分叉而来，因此在命令层面**完全向后兼容 Redis 6.2**，并在其基础上新增了大量命令与子命令。

## 一、总体结论

- **向后兼容**：Redis 6.2 中的全部命令在 Valkey 9.0 中均可用，行为与返回值保持一致，无需修改客户端代码即可平滑迁移。
- **协议兼容**：同时支持 RESP2 与 RESP3（`HELLO` 协议协商），与 Redis 6.2（默认 RESP2、可选 RESP3）一致。
- **超集关系**：Valkey 9.0 是 Redis 6.2 命令集的超集，新增命令主要来自 Redis 7.x 时代特性，以及 Valkey 独立演进（8.0／8.1／9.0）的特性。
- **迁移注意**：从 Redis 6.2 迁移到 Valkey 9.0 时，命令兼容性不是障碍，需要关注的是 RDB／AOF 文件版本、配置项变化以及部分集群运维命令的差异（详见第四节）。

## 二、Redis 6.2 命令在 Valkey 9.0 中的状态

下列 Redis 6.2 的代表性命令在 Valkey 9.0 中均原样保留：

| 类别 | 命令（节选） |
| --- | --- |
| 字符串 | `GET`、`SET`、`GETDEL`、`GETEX`、`SETRANGE`、`GETRANGE`、`APPEND`、`STRLEN`、`INCR`、`INCRBYFLOAT`、`MSET`、`MGET`、`LCS` |
| 哈希 | `HSET`、`HGET`、`HDEL`、`HRANDFIELD`、`HSCAN`、`HMGET`、`HGETALL` |
| 列表 | `LPUSH`、`RPUSH`、`LPOP`、`RPOP`、`LMOVE`、`BLMOVE`、`LPOS`、`LRANGE`、`LINSERT` |
| 集合 | `SADD`、`SREM`、`SMEMBERS`、`SMISMEMBER`、`SINTERSTORE`、`SRANDMEMBER`、`SSCAN` |
| 有序集合 | `ZADD`、`ZRANGE`、`ZRANGESTORE`、`ZDIFF`、`ZUNION`、`ZRANDMEMBER`、`ZPOPMIN`、`BZPOPMAX` |
| 通用键 | `COPY`、`OBJECT`、`EXPIRE`、`PERSIST`、`TTL`、`SCAN`、`TYPE`、`RENAME`、`TOUCH`、`UNLINK` |
| 位图 | `SETBIT`、`GETBIT`、`BITCOUNT`、`BITPOS`、`BITFIELD`、`BITOP` |
| HyperLogLog | `PFADD`、`PFCOUNT`、`PFMERGE` |
| 地理位置 | `GEOADD`、`GEOSEARCH`、`GEOSEARCHSTORE`、`GEODIST`、`GEORADIUS`（已废弃但保留） |
| Stream | `XADD`、`XREAD`、`XRANGE`、`XAUTOCLAIM`、`XACK`、`XGROUP`、`XINFO` |
| 发布订阅 | `PUBLISH`、`SUBSCRIBE`、`PSUBSCRIBE`、`PUBSUB` |
| 脚本 | `EVAL`、`EVALSHA`、`EVAL_RO`、`SCRIPT`、`SUBSCRIBE` |
| 事务 | `MULTI`、`EXEC`、`DISCARD`、`WATCH`、`UNWATCH` |
| 连接／管理 | `HELLO`、`AUTH`、`CLIENT`、`ACL`、`CONFIG`、`SLOWLOG`、`LATENCY`、`MEMORY`、`INFO` |
| 复制／集群 | `REPLICAOF`、`WAIT`、`FAILOVER`、`CLUSTER`、`MIGRATE`、`DUMP`、`RESTORE` |

> 说明：Redis 6.2 引入的 `SLAVEOF`→`REPLICAOF` 别名、`SMISMEMBER`、`GETDEL`、`GETEX`、`COPY`、`ZRANGESTORE`、`LMPOP` 前身能力等均已包含。

## 三、Valkey 9.0 相比 Redis 6.2 新增的命令

以下命令在 Redis 6.2 中**不存在**，是 Valkey 9.0 相对 6.2 的增量能力，按来源分组。

### 1. 来自 Redis 7.0 时代（Valkey 继承）

- 列表／有序集合多键弹出：`LMPOP`、`BLMPOP`、`ZMPOP`、`BZMPOP`
- 集合／有序集合交集基数：`SINTERCARD`、`ZINTERCARD`
- 过期时间戳查询：`EXPIRETIME`、`PEXPIRETIME`
- 函数引擎：`FUNCTION`（`LOAD`／`DELETE`／`FLUSH`／`LIST`／`DUMP`／`RESTORE`／`STATS`／`KILL`）、`FCALL`、`FCALL_RO`
- 分片发布订阅：`SPUBLISH`、`SSUBSCRIBE`、`SUNSUBSCRIBE`、`PUBSUB SHARDCHANNELS`、`PUBSUB SHARDNUMSUB`
- 集群可观测：`CLUSTER SHARDS`、`CLUSTER LINKS`
- 连接管理：`CLIENT NO-EVICT`
- 命令自省增强：`COMMAND DOCS`、`COMMAND LIST`、`COMMAND GETKEYSANDFLAGS`

### 2. 来自 Redis 7.2 时代（Valkey 继承）

- AOF 持久化等待：`WAITAOF`
- 连接管理：`CLIENT NO-TOUCH`、`CLIENT SETINFO`
- 集群分片标识：`CLUSTER MYSHARDID`

### 3. 哈希字段 TTL（与 Redis 7.4 能力重叠，Valkey 独立实现）

- `HEXPIRE`、`HPEXPIRE`、`HEXPIREAT`、`HPEXPIREAT`
- `HTTL`、`HPTTL`、`HEXPIRETIME`、`HPEXPIRETIME`
- `HPERSIST`

### 4. Valkey 专属新增（8.0／8.1／9.0）

- 哈希增强：`HGETEX`、`HGETDEL`、`HSETEX`
- 字符串／批量写：`MSETEX`、`DELIFEQ`（条件删除，9.0 新增）
- 脚本自省：`SCRIPT SHOW`
- 命令日志（替代／扩展 SLOWLOG 语义，二者并存）：`COMMANDLOG`（`GET`／`LEN`／`RESET`／`HELP`）
- 连接能力协商：`CLIENT CAPA`、`CLIENT IMPORT-SOURCE`
- 集群槽位统计：`CLUSTER SLOT-STATS`
- 集群原子化槽迁移（9.0 新增）：`CLUSTER MIGRATESLOTS`、`CLUSTER SYNCSLOTS`、`CLUSTER GETSLOTMIGRATIONS`、`CLUSTER CANCELSLOTMIGRATIONS`、`CLUSTER FLUSHSLOT`、`CLUSTER FLUSHSLOTS`
- 集群键扫描：`CLUSTERSCAN`

## 四、迁移与兼容性注意事项

1. **命令层面无破坏性变更**：Redis 6.2 客户端可直接连接 Valkey 9.0，命令调用不会因为兼容性问题失败。
2. **数据文件版本**：Valkey 9.0 的 RDB／AOF 版本高于 Redis 6.2，**向上兼容（旧 → 新可加载）**，但反向不可（不要用 Redis 6.2 加载 Valkey 9.0 产生的文件）。
3. **配置项差异**：部分 Redis 7.x／Valkey 新增配置项在 6.2 中不存在；从 6.2 沿用的旧 `redis.conf` 一般可直接被 Valkey 读取（保留 `redis-*` 兼容别名）。
4. **`SLOWLOG` 与 `COMMANDLOG` 并存**：`SLOWLOG` 仍可用，新功能建议关注 `COMMANDLOG`。
5. **已废弃命令仍保留**：如 `GEORADIUS`、`GEORADIUSBYMEMBER`、`SLAVEOF`、`SUBSTR` 等在两边都保留，行为一致。
6. **许可证差异（非命令兼容性，但需留意）**：Redis 6.2 为 BSD，Valkey 9.0 同样维持开源 BSD-3 协议；这与 Redis 7.4+ 改用的 RSALv2/SSPL 不同。

## 五、一句话总结

> 从 Redis 6.2 升级到 Valkey 9.0，**命令完全兼容、协议完全兼容**，可视为「Redis 6.2 命令集 + Redis 7.x 全部增量 + Valkey 8.0/8.1/9.0 专属增强」的超集；迁移工作重点不在命令本身，而在持久化文件方向性和少量运维命令的新用法。
