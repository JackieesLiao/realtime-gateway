# 线协议与 ACK 语义（截至 M2.3）

连接为 TCP，设备请求是 `realtime.Request`，网关响应是 `realtime.Ack`，结构见 proto/gateway.proto。请求和 ACK 采用相同的 framing：

```text
+-------------------------------+------------------------------+
| uint32 body_length (4 bytes)  | protobuf body (body_length)  |
| big endian / network order    | 不含任何额外分隔符            |
+-------------------------------+------------------------------+
```

长度不包括包头。例如 body 长度为 258 时包头为 `00 00 01 02`。ACK 的方向由连接角色区分，不使用共用 envelope。TCP 是字节流，send 次数和 recv 次数没有对应关系；允许包头/消息体分片、连续帧和不完整尾包。服务端只消费完整消息体。

## 请求字段与校验

| 字段 | protobuf 类型 | 约束 |
| --- | --- | --- |
| request_id | uint64 | 非 0，由设备生成，ACK 原样返回 |
| device_id | string | UTF-8，长度 1..128 字节 |
| message_type | MessageType | TELEMETRY=1 已实现；0 和其他数字均不支持 |
| payload | bytes | 非空、内容不作解释，受总消息体长度约束 |

proto3 不提供这些业务必填约束，必须由 Router 检查。先检查 request_id/device_id/payload，再按 message_type 路由；同时违反时返回 INVALID_REQUEST。protobuf 本身允许的未知字段会被接受，不代表该消息类型已实现。

## 限制与错误

| 情况 | 行为 | ACK |
| --- | --- | --- |
| 正常 TELEMETRY | 校验，增加 processed 和 payload_bytes，再排队发送 | 同 request_id，OK=0，error 为空 |
| 缺必填/非法业务字段 | 增加 rejected，保持连接可继续使用 | 同 request_id（包括 0），INVALID_REQUEST=1，error 非空 |
| 未知/未指定消息类型 | 增加 rejected，保持连接 | 同 request_id，UNKNOWN_MESSAGE_TYPE=2，error 非空 |
| 业务处理器抛出异常 | worker 转为失败结果，连接可继续处理 | 同 request_id，INTERNAL_ERROR=3，固定错误信息 |
| 资源或业务队列暂满 | 暂停准入/新读，释放后恢复已有输入 | 不发送忙碌 ACK，接受并处理后正常回应 |
| 单帧永久超过资源硬限额 | 关闭，reason=receive_capacity/business_capacity/send_capacity/global_capacity | 无，业务未执行 |
| Dispatcher 停止 | 拒绝新任务，关闭，reason=dispatcher_stopping | 无 |
| 空帧（长度 0） | 立即关闭，reason=empty_frame | 无 |
| 声明长度 > max_body_bytes | 完整包头校验后立即关闭，reason=frame_too_large | 无，不等待消息体、不为该消息体分配 |
| 声明长度 > max_receive_body_bytes | 立即关闭，reason=receive_capacity | 无，不等待/分配消息体 |
| 非法 protobuf | 关闭，reason=invalid_protobuf | 无；无法信任 request_id |
| Handler 返回超大 ACK | worker 归一为固定失败结果 | 同 request_id，INTERNAL_ERROR，连接可继续 |
| 已预留发送仍发生入队失败 | 防御性关闭，reason=send_capacity | 不保证，正常准入路径不应触发 |
| 半包期间断开/EOF | 关闭并释放连接，不处理半包 | 无 |
| 网络错误/服务端停止 | 取消读写并关闭 | 不保证已有 ACK 送达 |
| 连接数超限 | 新连接立即关闭，reason=connection_capacity | 无 |
| 旧连接取消尚占读块槽位 | 新连接关闭，reason=global_read_capacity | 无 |

默认 max_body_bytes=1,048,576、max_receive_body_bytes=1,048,576；发送最多 64 帧、262,144 字节（包括包头、尚未生成的 ACK 预留和在途帧）。读缓冲固定 4096 字节，接收器每次最多保留一条不完整或等待接管的完整消息体。包头长度首先按协议上限检查，再按接收容量检查。body 上限可配置为 1..16 MiB；为 Python/C++ 生成代码选择同一 /usr/bin/protoc。

ACK 没有额外的服务端时间戳、持久化序号或提交凭据。一个请求对应一次处理尝试；当前 M2.3 每连接最多一个业务任务在途，因此仍按接收顺序执行、排队 ACK；不同连接可并行。客户端应始终按 request_id 关联，避免依赖将来未承诺的跨请求完成顺序。

ACK 表示业务处理函数已经完成，**不表示持久化**。业务成功后如果连接断开，设备无法判断是否完成；重复发送同一 request_id 将再次计数。没有自动重试、去重或 exactly-once。无效 Request、ACK 或字段缺失不得由客户端推断为成功；proto3 的 OK 是默认 enum 值，所以客户端仍必须检查 protobuf 解析与 request_id。

当前没有半包、空闲和心跳超时，发送也没有期限；一个半包连接可一直占用槽位。SIGINT/SIGTERM 关闭连接并丢弃排队任务；已执行任务完成后丢弃迟到结果，不保证 ACK。超时和 graceful drain 留到 M2.4/M2.5。

## 业务准入与背压

INTERNAL_ERROR=3 在 M2.2 引入，本次不改变 protobuf 字段及 enum。业务异常固定返回 `business handler failed`；ACK 线帧最大 256 字节，Handler 输出体超过 252 字节时固定返回 `business result exceeds ACK limit`，均携带原 request_id。客户端仍按正常 framing 读取，无需更换编码。

默认业务 queued 为 128 条/8 MiB，outstanding 为 256 条/16 MiB，以 Request::ByteSizeLong() 计费；统一预算另覆盖接收原文、规范化余量、ACK 对象及线帧。完整合法长度头之后、消息体分配之前原子预留 Receive L、Business L+16+256、Send 256 字节。即使预计 ACK 很小，发送硬字节容量也必须容纳 256 字节的保守预留，否则该请求永久无法准入，关闭而不等待。

默认全局 Receive 为 256 帧/64 MiB、Send 为 4096 帧/16 MiB；总预算为 8192 个阶段凭证/128 MiB，其中固定读池保留 max_connections*4096 字节。每帧初始占 3 个动态消息凭证，并不是 8192 个独立请求；预算单位和释放规则见 architecture.md。

任一相关数量或字节达到高水位（默认 80%）暂停新准入及新读；两项都降至低水位（默认 50%）且完整申请可满足时恢复。已获得全部信用的半帧可以继续收齐。暂停保留已有完整帧或尾部，释放事件回到连接 strand 后先处理它们，不覆盖原缓冲、不主动轮询。

任务只有 Accepted 后才执行。临时不足不再断连；永久超过单帧硬限额、停止或协议错误仍关闭。等待不承诺一定执行，也没有等待超时；连接断开或服务端停止仍可能收不到 ACK。背压不改变 ACK 的业务完成语义，不提供持久化或去重。

## 后续传输映射（本轮未实现）

WebSocket 每条 binary message 携带一条 Request/Ack protobuf，**不再加 TCP 长度头**。gRPC 使用生成的 Request/Ack 和服务接口，由 gRPC 自身负责 framing；接口与 Callback API 生命周期在 M3 定义。三个 Transport 共享业务 Router/Dispatcher，避免把 TCP 细节带入业务。
