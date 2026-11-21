# Chat 发布切换与回滚

只有批准的维护窗口才能执行切换。本手册保留当前库和对象，不做旧客户端／旧格式兼容层，也不假定可以丢弃现有数据。示例为单节点 Docker Silo 和同机 Chat；若现场不同，先完成对应步骤的命令表，不能用猜测的进程、桶或库路径代替。

## 0. 确定部署目标，窗口前完成预检

- 填写并由值班方确认：主机、Chat 启停命令／服务账号、全部 WebSocket 和直连 Silo 路由（含 IPv4、IPv6、内网、代理）、现用数据库绝对路径 `DB`、`$DB.archives`、Silo 容器和源卷 `SILO_DATA`、对象 endpoint／桶、运维触发器和备份位置。探查不到的入口不是不存在。若 Chat 不在运行，先确认这是初次部署还是另一台主机的服务；不能把开发库当现网库。
- `docker inspect` 记录镜像 ID/摘要、入口、参数、挂载、网络、端口、restart、环境与覆盖。结合桶和账号策略、代理及直连流量、其他客户端和负责人确认 Silo 只服务 Chat。单桶、单节点、容器名或无当前连接均不能单独证明独占。不能确认就不设 `off`。
- 明确非业务访问的管理路径、隔离恢复目标、足够的备份空间和窗口截止时间。源卷访问及备份工具应能读取、保留 UID/GID、权限、ACL、xattr 和完整隐藏目录；若生产数据仍在 `/tmp`，须先确认主机不会清理它并预先保护或另行批准迁往持久卷，本窗口不顺便迁移。
- 收集 Chat 旧可执行文件及哈希、原启动定义和环境文件、原运维入口及启用状态。导出 Docker inspect、旧镜像（`docker image save`）和启动配置到只允许管理员访问的位置；inspect 可能含密钥，不上传、不写日志。保留旧容器，不在切换时删除。
- 用候选 `SHA256SUMS` 校验。为生产预置管理员可写、服务账号不可写且父目录也不可由服务账号替换的固定前缀。按 [SQLite 构建说明](sqlite-runtime.md)使用候选 SQLite 3.51.3 重链 Release 服务端；旧产物 ELF 指向候选原绝对路径，移动 SQLite 后不得直接复用该 ELF。保留新产物哈希和 `readelf -d`、`ldd` 结果。验证实际运行路径及 [WAL／恢复验收入口](acceptance/README.md)后才进入窗口；预置及验证不涉及系统 SQLite。Silo 候选放独立只读挂载路径。
- 逐个准备 `archive`、`wal`、`reclaim`、`backup`、`verify-backup`、`restore` 和定期核验实际入口，确认新旧版本的启停／修改可逆。若有 systemd timer，记录原启用状态、先停 timer 并等所触发的 service 完成；若有 cron，备份对应 crontab/配置、只暂停 Chat 条目再等已有进程退出。也暂停外部计划任务和人工运维。不得停系统全部 cron。

## 1. 停写并形成配对备份

1. 先关闭 Chat 客户端入口，使已有 WebSocket 排空或断开，阻止新签发 URL。关闭所有对象写入路径的外部入口，包括已签发 URL 可直接访问的端口；保留受控内部管理路径供 Chat 优雅退出。等已接收的 PUT 等写请求完成或明确取消。只关反代或等 900 秒 URL 到期都不能证明已接收请求排空。
2. 停止已确认的定时／人工任务并等待退出；优雅停止 Chat（已确认的 unit 用 `systemctl [--user] stop <unit>`，手动服务对经确认的 PID 发 SIGTERM），等待进程退出和清理收尾。随后 `docker stop --time 60 "$SILO_NAME"`，核对容器已退出、无强杀迹象、所有相关卷写入者已停止；不并行挂载同一生产卷。停写期间保持外部栅栏。
3. 记录 UTC 恢复点、主机、同一 `BACKUP` 的构成和哈希。以有权限的管理员在权限为 0700 的新目录执行，`REPO` 和 `SQLITE_PREFIX` 指向已核实的版本。备份目标需另有持久副本，不能和源卷同一故障域。

```sh
umask 077
set -eu
admin() {
    "$REPO/ops/with-sqlite-runtime.sh" "$SQLITE_PREFIX" /usr/bin/python3 "$REPO/ops/chat_admin.py" "$@"
}
mkdir -m 700 -- "$BACKUP"
admin --db "$DB" backup "$BACKUP/chat"
admin verify-backup "$BACKUP/chat"
mkdir -m 700 -- "$BACKUP/silo"
rsync -aHAX --numeric-ids -- "$SILO_DATA/" "$BACKUP/silo/"
audit=$(rsync -aHAXnci --delete --numeric-ids -- "$SILO_DATA/" "$BACKUP/silo/")
test -z "$audit"
```

rsync 的 `--delete` 仅用于与 `-n` 同时出现的差异检查。这里备份整个 Silo 卷，包括隐藏的 MPU 元数据与分片；仅同步已完成对象不够。若命令或比对失败，不进入切换。把配对备份用 `admin restore "$BACKUP/chat" "$RESTORE_DB_DIR"` 恢复到新目录，以 `admin --db "$RESTORE_DB_DIR/chat.sqlite3" check` 校验热库和月档；将 Silo 副本复制到另一全新卷，在隔离地址用原逻辑桶名启动，至少核实代表性已完成对象的 HEAD 和已知未完成上传的精确 ListParts。恢复实例、Chat 配置、RPC 和定时任务均不得连现网桶或生产库。核对点见[恢复验收](acceptance/README.md#恢复与部署核对)。

## 2. 分组件替换并验收

1. Silo：用原镜像 ID、入口、命令、环境、卷、端口和网络起候选，仅增加候选 Silo 在 `/usr/bin/silo` 的只读挂载，以及 `MINIO_API_STALE_UPLOADS_EXPIRY=off`。旧容器保持停止并改名留作回退。检查单一卷写入者、精确镜像与运行二进制哈希、日志、就绪、有效环境，以及切换前后同批已知 MPU 的 ListParts 和完成对象 HEAD。`off` 来自环境，不能只靠 `mc admin config get` 的持久配置值。
2. Chat：保留原运行账号、数据库、SQLite/月档目录、对象 endpoint、桶与秘密来源，只换经验证的可执行文件。用原方式启动、保持用户入口关闭；确认 `/health`、启动与回收日志、运行进程 `/proc/PID/maps` 确实映射已核验的私有 SQLite。隔离环境完成读写验证；现场只做已批准的只读健康检查，不造业务消息或假上传。启动本身会执行正常恢复和过期回收，只有配对备份通过后才允许。
3. 运维：把已确认的各个调用改为 `with-sqlite-runtime.sh "$SQLITE_PREFIX" /usr/bin/python3 …/chat_admin.py …`。用实际运维账号及环境验证包装器下的 `sqlite_source_id()` 和 `/proc/self/maps`，与服务端路径及校验值一致。复核备份可读；不以运行 `wal` 等修改命令充当只读检查。
4. 任一哈希或加载路径不符、可能接错库/卷、确认不全、备份不可恢复、仍有效的原上传分片缺失或健康失败：保持入口和计划任务关闭，按下文回退。都通过后才恢复对象路由与 Chat 入口、按原启用状态恢复运维，观察失败、保留任务和容量。不要在开写后使用旧快照覆盖新写入。

## 回滚 A：只退 Chat／SQLite

先重新封住入口、排空所有写请求并暂停定时任务；保存本次回退点。隔离克隆确认旧 Chat 能打开**当前**库和月档、支持现有上传状态及对象接口，旧 Python 只读检查也通过后，恢复旧可执行文件、原启动定义和运维入口。继续使用 Silo 候选及 `off`，不要动原卷或重开旧清扫。核对实际库版本、健康和上传列表后才恢复入口。若退回未修复 SQLite，在短时回退中避免并发运维 checkpoint；尽快恢复已修复运行时。旧版不读当前格式时继续停写，不回放旧快照来伪造兼容。

## 回滚 B：必须撤掉 Silo 候选

默认**不在原卷启动 24h 旧清扫器**。先做同样停写和配对备份，用停写后的已验证 Chat 快照枚举全部仍可续传的任务。下例按 UTC 毫秒标记 Chat 的创建时间作预筛，不把它当 Silo 的创建时间。输出仅保存在受控目录。

```sh
umask 077
sqlite3 -readonly -csv -header "$BACKUP/chat/chat.sqlite3" "
SELECT object_key, multipart, state, created_at, touched,
       created_at <= (CAST(strftime('%s','now') AS INTEGER)-86400)*1000 AS chat_created_over_24h
FROM attachment_uploads
WHERE state IN ('active','paused')
  AND touched > (CAST(strftime('%s','now') AS INTEGER)-1296000)*1000
ORDER BY touched, id;" > "$BACKUP/resumable.csv"
```

对每条任务用准确 key 的 S3 ListMultipartUploads，核对返回 key、ID 和 Silo `Initiated`，再按已知 ID ListParts。使用已封住写入的 `off` 实例；若已停止，在配对副本用 `off` 作核验，不为核验而让旧清扫器接触原卷。multipart ID 为空也必须精确找回；不能用空前缀桶列表。没有 AWS CLI 可用配置好的 S3 SDK；以下是运维工作站命令：

```sh
aws --endpoint-url "$S3_ENDPOINT" s3api list-multipart-uploads --bucket "$S3_BUCKET" --prefix "$OBJECT_KEY"
aws --endpoint-url "$S3_ENDPOINT" s3api list-parts --bucket "$S3_BUCKET" --key "$OBJECT_KEY" --upload-id "$UPLOAD_ID"
```

Silo 年龄已达 24h 即危险（含边界）；`Initiated` 或关联无法核实、跨过窗口会变老、权限/存储错误、未知外来 MPU，一律当作未通过。即使当前没到 24h，在线运行旧清扫器仍不能保证剩余 15 天，因此不能凭空名单或备份宣布可以复业。

实际处置：保留经验证的 Silo 候选或修复后仍具有相同保留语义的替代版本；若候选不能运行，保持业务停写且不让旧清扫器接触原卷。经单独批准，若必须用原版临时诊断，只有在隔离卷副本、无业务入口时启动原容器镜像；原卷和配对备份不动。用户可另行决定维护延期，或先在安全的保留版本恢复服务、让原设备完成任务；不可默认为活跃上传取消、改 ID 或改时间。只读诊断通过与恢复生产是两件事。没有满足 15 天语义且经过验收的回退版本，就没有无损的 Silo 生产回退。
