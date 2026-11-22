# Chat 服务端运行手册

用服务账号按项目 README 建好私有 Silo 桶、TLS 证书和环境变量。数据库及同名 `.archives` 目录放在同一个受控持久卷。仅在服务所在机器运行管理命令。管理员需要 Python 3.10+ 和对该库的文件权限。对象核验使用已配置只读凭据的 MinIO Client（`mc`）。

假设数据库路径在 `DB` 中，下列命令均从仓库根目录执行。

服务端静态链接 SQLite；以下所有运维 Python 命令均应经[同源私有库的运行时包装器](sqlite-runtime.md)启动。先把 `SQLITE_PREFIX` 设为该私有库的安装前缀：

```sh
chat_admin() { ops/with-sqlite-runtime.sh "$SQLITE_PREFIX" /usr/bin/python3 ops/chat_admin.py "$@"; }
```

## 备份与恢复

```sh
chat_admin --db "$DB" backup /backup/chat-2026-11-20
chat_admin verify-backup /backup/chat-2026-11-20
chat_admin restore /backup/chat-2026-11-20 /srv/chat-restored
```

在线备份读取 SQLite 的一致性快照，同时复制该快照登记的不可变月档。写入仍可继续，归档发布与备份互斥。输出含 `chat.sqlite3`、热库 SHA-256 和所需月档。验收覆盖数据库完整性、外键、月档校验和、范围、条数及热库引用。请把完成的备份复制到独立存储，并在隔离目录定期演练恢复。备份与恢复都要求使用全新的目的目录。

Silo 对象和未完成的分片由对象存储另外备份。需要可恢复到同一时点时，停止 Chat 写入并暂停对象回收，保留此时的 SQLite 备份和配套 Silo 存储快照。恢复演练使用独立恢复桶，检查对象后再启动服务，以免旧快照中的清理任务作用于现网桶。用恢复目录中的 `chat.sqlite3` 作为服务端的数据库参数。数据库与桶中均有用户数据，应限制备份访问权限。恢复未完成上传还需保留 multipart 元数据、分片和原设备日志；完整卷恢复到独立实例时使用原逻辑桶名。配套演练和本机版本核验见[隔离恢复记录](acceptance/README.md#恢复与部署核对)。

## 归档与数据库检查

```sh
chat_admin --db "$DB" archive 2025-01
chat_admin --db "$DB" archive-status
chat_admin --db "$DB" check
chat_admin --db "$DB" wal
chat_admin --db "$DB" reclaim
```

按 UTC 自然月执行归档。状态中含最近执行时间、完成或失败状态及错误。检查空间、权限、校验值后，重跑相同月份继续工作。归档文件丢失或损坏时从已验证的备份恢复对应文件；已移出的正文需要原月档。热库保留当前状态和原始 ID。读取月档失败会返回读取错误。

`wal` 执行被动 checkpoint，报告 WAL 总页数、已写回页数和空闲页数。持续的未写回页数应结合长读事务、磁盘空间排查。归档结束时已分批归还空闲页，`reclaim` 也可手动执行。不要手动删除活动数据库的 WAL 或 SHM 文件。

## Silo 核验与回收

```sh
chat_admin --db "$DB" objects private/chat-bucket
chat_admin --db "$DB" objects private/chat-bucket --checksum
mc ls --recursive --incomplete private/chat-bucket
```

第一条比对对象 key、大小、完成文件、上传任务和回收队列。第二条还逐个流式下载核对 SHA-256，会产生完整下载流量。返回非零时按输出检查缺失对象、校验不符或孤儿候选。清单与数据库跨资源读取，在线报告的差异须在停止写入和回收后复核。孤儿候选由管理员核对备份和在途上传；检查命令仅列举差异。

先核对部署的 Silo 版本、每个节点的有效 `api` 配置及环境覆盖。版本 `44e848fedf8c` 使用 `MINIO_API_STALE_UPLOADS_EXPIRY`（源码默认 24h）和 `MINIO_API_STALE_UPLOADS_CLEANUP_INTERVAL`（默认 6h）。该版本按上传创建时间清扫，旧元数据有时间回退。Chat 的 15 天从 `attachment_uploads.touched` 起算。桶级 `PutBucketLifecycle` 不支持 `AbortIncompleteMultipartUpload`。[配置源码](https://github.com/pgsty/silo/blob/44e848fedf8c/internal/config/api/api.go) · [清扫实现](https://github.com/pgsty/silo/blob/44e848fedf8c/cmd/erasure-multipart.go) · [S3 兼容性契约](https://silo.pigsty.io/reference/s3-api-compatibility/#differences-from-s3-apis-for-multipart-uploads)

`mc ls --incomplete` 只作线索。上述版本的空前缀列表来自节点本地缓存，可能漏掉重启前或其他节点的上传；非空前缀按准确对象键处理。桶列表为空仍需核对数据库已知任务。[列表实现](https://github.com/pgsty/silo/blob/44e848fedf8c/cmd/erasure-server-pool.go)

使用支持 `-readonly` 的 sqlite3 和已配置只读权限的 AWS CLI（或等效 S3 工具）。从服务端配置核对完整 endpoint URL 与 bucket，对待查记录使用数据库中的完整 `object_key` 和非空 `multipart` 值：

```sh
sqlite3 -readonly "$DB" 'SELECT object_key,multipart,state,touched FROM attachment_uploads;'
aws --endpoint-url "$S3_ENDPOINT" s3api list-parts \
  --bucket "$S3_BUCKET" --key "$OBJECT_KEY" --upload-id "$UPLOAD_ID"
aws --endpoint-url "$S3_ENDPOINT" s3api head-object \
  --bucket "$S3_BUCKET" --key "$OBJECT_KEY"
```

`ListParts` 查看未完成上传的分片；`HEAD` 检查已完成对象。`NoSuchUpload` 也可能来自正常完成或终止。结合 `files`、`message_attachments`、`conversation_files` 及 `attachment_cleanup` 中同一键的预留或回收状态判断。权限、证书或连接错误须单独排查。不要据空列表修改任务或终止上传。

服务启动、运行期间和退出时会处理回收队列。撤回消息或移除群文件后仍有其他引用的对象继续保留。确认最后引用消失后由服务端删除对象；15 天未继续的上传先终止 multipart，再清理预留。检查未完成分片时，应结合数据库的上传状态、最后活动时间以及 Silo 的响应。Silo 不可达时，先检查 endpoint、证书、私有桶权限和凭据，恢复后观察队列是否下降。

## 候选：由 Chat 管理未完成上传的保留

上游 `44e848fedf8cec3d8c0da4b1aa8422390a2b7a8b` 不提供兼容开关；`0` 回落到 24h，负数不是关闭，延长固定创建寿命无法覆盖任意最后活动时间。隔离候选采用该版本的独立最小补丁；仅补丁版识别 `MINIO_API_STALE_UPLOADS_EXPIRY=off`，仅跳过持久 multipart 目录的按创建时间清扫。其它临时文件清理、对象生命周期、缓存淘汰和显式 Abort/Complete 均不关闭；临时文件和缓存继续使用 24h 默认值。原镜像不设置 `off`。候选未写入业务容器。

只在 Chat 独占的 Silo 实例考虑启用，因为设置作用于节点、不是桶。Chat 继续以数据库 `touched` 为准，启动、周期和退出清理把到期任务转为 `discarding`、终止 multipart 并回收未发布对象；失败留待重试。已发布对象仍由独立的最后引用回收队列管理。已登记上传任务但 Initiate 响应或 multipart ID 保存中断时，重试或到期终止会按准确 object key 找回上传。没有相应 Chat 任务/key 的外来 MPU，或数据库与对象存储失配，才需要人工复核。切换后需要监测容量和任务积压；孤儿只在停止写入、用完整快照与数据库复核并确定没有业务引用后处理，不能用空桶列表代替复核。

候选二进制和精确基线的补丁独立保存，不将 Silo 源码纳入本仓库。构建以 Go 1.27.4 在该 SHA 上应用补丁再运行：

```sh
git apply --check /path/to/silo-chat-multipart-off.patch
git apply /path/to/silo-chat-multipart-off.patch
CGO_ENABLED=0 go build -trimpath -tags kqueue --ldflags "$(go run buildscripts/gen-ldflags.go)" -o /path/to/candidate/silo .
```

回归命令（临时卷、单节点、回环端口，不能指向线上实例）：

```sh
SILO_BIN=/path/to/candidate/silo CHAT_TEST_SERVER=/path/to/candidate/chat_server     ops/with-sqlite-runtime.sh "$SQLITE_PREFIX" python3 ops/acceptance/silo_retention.py
```

测试停写后将临时实例的 Silo 上传创建标识回拨 17 天，副本不设置过期覆盖，使用默认 24h 创建阈值和 1s 扫描周期，确证两个分片不可再列；另一份使用 `off`，等待多轮扫描仍保有相同分片。再把 Chat 活动时间分别设为 14/16 天前，确认只有后者被 Chat 终止，原设备用保留的同一 ID 从第二片续传，其他设备不能接管，仍被群文件引用的完整对象不变。另设旧、新普通临时目录，确认启用 `off` 后只清扫旧目录原路径；这不验证垃圾目录空间回收，也不是实际等待 15 天。

切换使用[发布切换与回滚手册](release-cutover.md)。不得在未核实实例独占性、写入入口与在途任务时恢复原 24h 清扫。

## 日志

服务端写标准输出与标准错误，记录数据库初始化、连接监听、关闭信号、关闭完成及回收失败。对象连通性可通过上述 `objects` 命令核验。归档状态在数据库中记录，执行命令的输出可与进程日志共同保存。

在已有 Bash 和 Apache `rotatelogs` 的 Linux 主机上，可为 Chat 进程单独设置大小轮转。先由管理员为服务账号准备日志目录，再按 README 配好运行环境。用下面的启动形式可保留服务端 PID，进程管理器可直接发送 SIGTERM。

```bash
exec ./build/chat_server 8080 4 "$DB" \
  > >(rotatelogs -n 5 /var/log/chat/server.log 10M) 2>&1
```

轮换保留 5 个约 10 MiB 文件。归档、备份、复核命令使用非零退出状态报告失败；定时任务应记录退出状态及标准输出。请勿把 S3 密钥、登录凭据、预签名 URL 或消息正文写入诊断日志。
