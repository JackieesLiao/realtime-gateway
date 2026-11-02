# 线协议与 ACK 语义（截至 M2.2）

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
| 业务容量不足 | 未接受任务，关闭，reason=business_capacity | 无，业务未执行 |
| Dispatcher 停止 | 拒绝新任务，关闭，reason=dispatcher_stopping | 无 |
| 空帧（长度 0） | 立即关闭，reason=empty_frame | 无 |
| 声明长度 > max_body_bytes | 完整包头校验后立即关闭，reason=frame_too_large | 无，不等待消息体、不为该消息体分配 |
| 声明长度 > max_receive_body_bytes | 立即关闭，reason=receive_capacity | 无，不等待/分配消息体 |
| 非法 protobuf | 关闭，reason=invalid_protobuf | 无；无法信任 request_id |
| 发送队列容量超限 | 关闭，reason=send_capacity，丢弃排队数据 | 不保证，业务可能已经完成 |
| 半包期间断开/EOF | 关闭并释放连接，不处理半包 | 无 |
| 网络错误/服务端停止 | 取消读写并关闭 | 不保证已有 ACK 送达 |
| 连接数超限 | 新连接立即关闭，reason=connection_capacity | 无 |

默认 max_body_bytes=1,048,576、max_receive_body_bytes=1,048,576；发送最多 64 帧、262,144 字节（包括包头和在途帧）。读缓冲固定 4096 字节，接收器每次最多保留一条不完整或等待接管的完整消息体。包头长度首先按协议上限检查，再按接收容量检查。body 上限可配置为 1..16 MiB；为 Python/C++ 生成代码选择同一 /usr/bin/protoc。

ACK 没有额外的服务端时间戳、持久化序号或提交凭据。一个请求对应一次处理尝试；当前 M2.2 每连接最多一个业务任务在途，因此仍按接收顺序执行、排队 ACK；不同连接可并行。客户端应始终按 request_id 关联，避免依赖将来未承诺的跨请求完成顺序。

ACK 表示业务处理函数已经完成，**不表示持久化**。业务成功后如果连接断开，设备无法判断是否完成；重复发送同一 request_id 将再次计数。没有自动重试、去重或 exactly-once。无效 Request、ACK 或字段缺失不得由客户端推断为成功；proto3 的 OK 是默认 enum 值，所以客户端仍必须检查 protobuf 解析与 request_id。

当前没有半包、空闲和心跳超时，发送也没有期限；一个半包连接可一直占用槽位。SIGINT/SIGTERM 关闭连接并丢弃排队任务；已执行任务完成后丢弃迟到结果，不保证 ACK。超时和 graceful drain 留到 M2.4/M2.5。

## M2.2 业务准入和错误兼容

新增 INTERNAL_ERROR=3，原字段编号和 enum 值不变。业务异常不把异常文本作为 ACK 发送，固定返回 business handler failed；错误仍携带原 request_id。

默认业务排队上限为 128 条/8 MiB，总在途上限为 256 条/16 MiB；字节是 Request::ByteSizeLong()，不是 TCP 长度头声明。总在途包括结果等待连接 strand 消费的阶段，业务预算在 ACK 排队或结果丢弃时归还；等待网络写完的 ACK 由发送队列单独限制。

任务只有 Accepted 后才转移所有权。队列满时当前立即断连，不额外构造繁忙 ACK；其他连接已接受的任务不会因此取消。业务已完成但发送容量不足、断连或服务端停止时，仍可能收不到 ACK。

## 后续传输映射（本轮未实现）

WebSocket 每条 binary message 携带一条 Request/Ack protobuf，**不再加 TCP 长度头**。gRPC 使用生成的 Request/Ack 和服务接口，由 gRPC 自身负责 framing；接口与 Callback API 生命周期在 M3 定义。三个 Transport 共享业务 Router/Dispatcher，避免把 TCP 细节带入业务。
