# M1 线协议与 ACK 语义

连接为 TCP，设备请求是 `realtime.Request`，网关响应是 `realtime.Ack`，结构见 proto/gateway.proto。请求和 ACK 采用相同的 framing：

```text
+-------------------------------+------------------------------+
| uint32 body_length (4 bytes)  | protobuf body (body_length)  |
| big endian / network order    | 不含任何额外分隔符            |
+-------------------------------+------------------------------+
```

长度不包括包头。例如 body 长度为 258 时包头为 `00 00 01 02`。ACK 的方向由连接角色区分，不使用共用 envelope。TCP 是字节流，send 次数和 recv 次数没有对应关系；允许包头/消息体分片、连续帧和不完整尾包。服务端只消费完整消息体。

## 请求字段与校验

| 字段 | protobuf 类型 | M1 约束 |
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
| 空帧（长度 0） | 立即关闭，reason=empty_frame | 无 |
| 声明长度 > max_body_bytes | 完整包头校验后立即关闭，reason=frame_too_large | 无，不等待消息体、不为该消息体分配 |
| 声明长度 > max_receive_body_bytes | 立即关闭，reason=receive_capacity | 无，不等待/分配消息体 |
| 非法 protobuf | 关闭，reason=invalid_protobuf | 无；无法信任 request_id |
| 发送队列容量超限 | 关闭，reason=send_capacity，丢弃排队数据 | 不保证，业务可能已经完成 |
| 半包期间断开/EOF | 关闭并释放连接，不处理半包 | 无 |
| 网络错误/服务端停止 | 取消读写并关闭 | 不保证已有 ACK 送达 |
| 连接数超限 | 新连接立即关闭，reason=connection_capacity | 无 |

默认 max_body_bytes=1,048,576、max_receive_body_bytes=1,048,576；发送最多 64 帧、262,144 字节（包括包头和在途帧）。读缓冲固定 4096 字节，接收器每次只积累一条不完整消息体。包头长度首先按协议上限检查，再按接收容量检查。body 上限可配置为 1..16 MiB；为 Python/C++ 生成代码选择同一 /usr/bin/protoc。

ACK 没有额外的服务端时间戳、持久化序号或提交凭据。一个请求对应一次处理尝试；同一连接在 M1 按接收顺序执行、排队 ACK。未来异步业务不预先承诺 ACK 顺序，客户端应始终按 request_id 关联。

ACK 表示业务处理函数已经完成，**不表示持久化**。业务成功后如果连接断开，设备无法判断是否完成；重复发送同一 request_id 将再次计数。没有自动重试、去重或 exactly-once。无效 Request、ACK 或字段缺失不得由客户端推断为成功；proto3 的 OK 是默认 enum 值，所以客户端仍必须检查 protobuf 解析与 request_id。

M1 没有半包、空闲和心跳超时，发送也没有期限；一个半包连接可一直占用槽位。SIGINT/SIGTERM 会取消这些等待，M2 再实现超时和 graceful drain。

## 后续传输映射（本轮未实现）

WebSocket 每条 binary message 携带一条 Request/Ack protobuf，**不再加 TCP 长度头**。gRPC 使用生成的 Request/Ack 和服务接口，由 gRPC 自身负责 framing；接口与 Callback API 生命周期在 M3 定义。三个 Transport 共享业务 Router/Dispatcher，避免把 TCP 细节带入业务。
