# M2.2 变更范围与阅读路径

本轮把业务执行从 I/O strand 移到固定业务线程，新增排队和总在途双层容量限制，并接通 M2.1 的暂停/恢复入口。新增代码附有中文注释，说明锁、所有权、凭证和回送生命周期。M2.3–M2.5 仍是计划。

## 文件级范围

| 文件 | 变更 |
| --- | --- |
| include/gateway/bounded_task_queue.hpp（新增） | 准入状态、容量、快照、TaskTicket、拥有 Request 的任务及结果 |
| src/bounded_task_queue.cpp（新增） | 数量/字节准入、条件变量等待、任务弹出、停止丢弃及预算归还 |
| include/gateway/dispatcher.hpp（新增） | 异步 Dispatcher、可注入 Handler、固定 worker、stop/join 和失败计数 |
| src/dispatcher.cpp（新增） | 锁外执行、异常转换、Request 销毁、结果及凭证回送 |
| include/gateway/business.hpp | 移除旧同步 Dispatcher，保留 Router/Statistics |
| include/gateway/config.hpp、src/config.cpp | 业务线程及四项容量配置、范围校验 |
| config/gateway.json | 展示新配置及默认值 |
| include/gateway/server.hpp | Router/Dispatcher 生命周期、可注入业务处理器、启动端口和预算快照 |
| src/server.cpp | 异步准入、单连接业务在途、弱引用回送、work guard、strand 消费和停止 |
| src/main.cpp | I/O 结束后 join worker；异常退出也先回收 worker |
| proto/gateway.proto | 新增 INTERNAL_ERROR=3，原字段编号和枚举值不变 |
| tests/dispatcher_test.cpp（新增） | 队列边界、请求所有权、凭证移动、异常、回送积压和停止测试 |
| tests/async_server_test.cpp（新增） | 注入 gate 的真实 socket 测试，验证 ACK 时机、跨连接并行和迟到结果 |
| tests/unit.cpp | 同步业务单测直接调用 Router，新增配置错误覆盖 |
| tests/integration.py | 新增业务字节超限、多 I/O/worker 流水线；退出检查业务预算归零 |
| CMakeLists.txt | 新源文件和独立 C++ socket 测试目标；TSan 选项与 sanitizer 互斥检查 |
| CMakePresets.json | 独立 tsan configure/build/test preset |
| README.md | 构建、配置、现状和可复现 TSan 命令 |
| ROADMAP.md、docs/m2-plan.md | 标记 M2.2 完成，下一步 M2.3 |
| docs/architecture.md、docs/protocol.md | 更新线程、预算、失败结果和停止语义 |
| docs/changes-m2.2.md、docs/validation-m2.2.md（新增） | 本轮范围、真实验证和限制 |

共 25 个文件。FrameDecoder、BufferedInput、发送队列、Router 的业务规则及客户端源代码未改动；没有加入高低水位、统一收发预算、限流、心跳、超时或 graceful drain。

## 关键接口和所有权

Dispatcher::dispatch(Request) 的同步接口移除，新的 dispatcher.hpp 提供 try_submit(unique_ptr<Request>&, BusinessCompletion)。返回 Accepted 时请求指针被移动并变为空；Full/Stopping 时原请求仍由调用方持有，Session 当前选择关闭连接。

四项准入条件在同一把短临界区内同时检查：queued_messages、queued_bytes、outstanding_messages、outstanding_bytes。调用方不等待空位或业务完成；worker 才在 condition_variable 上等待。业务和完成回调不在队列锁内调用。

TaskTicket 不可复制，可移动；它的共享计数状态可以比队列存活更久。排队任务先销毁 Request/回调，再归还凭证；worker 则先销毁请求，把凭证移交给 BusinessResult。连接关闭不会提前归还仍在执行或等待回送的任务预算。

同连接只允许一项业务在途。完成回到 strand 后排队 ACK、释放旧凭证，再恢复输入。顺序不可颠倒，否则 outstanding 上限为 1 时会错误拒绝同连接下一请求。

## 回送与停止

Session::make_completion 只捕获 weak_ptr、executor 和 work guard。worker 把结果 post 回连接 strand；连接不存在或已关闭时丢弃结果，TaskTicket 析构归还预算。guard 保证最后一个 socket 关闭后，I/O 仍能处理这个迟到回调。

当前 stop 关闭连接、丢弃未执行任务、等待执行中任务完成后的结果清理。join 发生在 I/O run 返回后的控制线程；异常路径也先回收 worker。尚无执行中业务的强制取消或退出截止时间，Handler 必须有限时返回。

## 新的错误与配置

新增 INTERNAL_ERROR=3，业务异常返回同 request_id 和固定错误信息 business handler failed；不会把异常文本直接发给设备。未接受任务的业务容量失败直接关闭连接，reason=business_capacity，不发送繁忙 ACK。

默认业务线程 2；排队容量 128 条/8 MiB；总在途容量 256 条/16 MiB。字节按 Request::ByteSizeLong() 计费，不含 TCP 包头，不等于真实 RSS；排队、执行和待消费结果共同占用总预算，发送队列单独计费。

## 三个重点函数

1. BoundedTaskQueue::try_push：容量检查和 Accepted 才发生的 Request 所有权转移。
2. Dispatcher::run_worker：锁外执行、异常转 ACK、销毁请求并移动结果凭证。
3. Session::handle_business_result：连接状态检查、ACK 入队、归还凭证和恢复输入。

结合 TaskTicket::release 理解失败/取消预算归还，结合 Session::make_completion 理解连接和 io_context 的不同生命周期。真实验证见 [validation-m2.2.md](validation-m2.2.md)。
