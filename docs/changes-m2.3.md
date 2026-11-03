# M2.3 变更范围与阅读路径

本轮只实现本地/全局资源背压。相对 M2.2，暂时容量不足由立即断连改成保留状态并事件恢复；永久不能容纳的单帧仍拒绝。没有加入限流、心跳、超时或 graceful drain，没有性能结论。

## 文件范围

| 文件 | 变更及原因 |
| --- | --- |
| include/gateway/resource_budget.hpp、src/resource_budget.cpp（新增） | 预算、资源凭证、数量/字节水位、原子准入、等待顺序、事件代次及有界唤醒 |
| include/gateway/frame.hpp、src/frame.cpp | 包头校验后、reserve 前的 Admission；AwaitingCapacity 保留状态，允许空输入重试 |
| include/gateway/buffered_input.hpp、src/buffered_input.cpp | 接入 Admission；完整帧存储销毁后才回收 Receive；停止和暂停分开 |
| include/gateway/bounded_task_queue.hpp、src/bounded_task_queue.cpp | queued 高低水位；TaskTicket 持有统一预算；队列容量通知在锁外执行 |
| include/gateway/dispatcher.hpp、src/dispatcher.cpp | 转交业务/发送凭证，限制 ACK 输出，Request 销毁后缩减预留 |
| include/gateway/send_queue.hpp | OutgoingFrame 绑定字节与凭证；队列和在途写共享同一对象 |
| include/gateway/config.hpp、src/config.cpp、config/gateway.json | 新增八个全局预算/水位配置；推导统一业务容量并校验静态读池 |
| include/gateway/server.hpp、src/server.cpp | 分配 Session 前取得读块信用；帧准入、Full 等待、strand 恢复、ACK 直接序列化与预算统计 |
| CMakeLists.txt | 登记预算实现及新增测试 |
| tests/resource_budget_test.cpp（新增） | 原子性、精确边界、水位回滞、公平推进、事件竞态、取消及最后所有者释放 |
| tests/unit.cpp | 准入前无分配、包头空输入重试、protobuf 规范化余量、配置关系及发送对象适配 |
| tests/buffered_input_test.cpp | 只有包头的暂停恢复、接收信用释放时机、准入期间关闭 |
| tests/dispatcher_test.cpp | 队列数量/字节水位及超大 ACK 归一 |
| tests/async_server_test.cpp | 真实 socket + worker gate，控制队列满、半帧、竞争与关闭顺序；所有预算最终归零 |
| tests/integration.py | 单 ACK 槽及小全局预算下多连接推进，退出预算统计检查 |
| README.md、ROADMAP.md、docs/architecture.md、docs/protocol.md、docs/m2-plan.md | 更新实际行为、配置公式、ACK 上限及后续阶段状态 |
| docs/changes-m2.3.md、docs/validation-m2.3.md（新增） | 本轮阅读指南和真实验证记录 |

共 30 个文件。保留 frame.cpp 和 buffered_input.cpp 的已有学习注释，受修改位置按 clang-format 统一格式。未修改 AGENTS.md、protobuf、客户端、Router 业务规则或 presets；历史阶段验证记录保持原样。

## 一条请求的预算如何走

1. 接入先占一个 4096 字节固定读块；原始数据及未消费尾部都在该块中。
2. 完整合法长度 L 的包头申请三个凭证：Receive L、Business L+16+256、Send 256。全有才分配消息体；没有就保留包头和输入偏移。
3. protobuf 解析后，Business 缩至 Request::ByteSizeLong()+256；输入 body 真正销毁后才释放 Receive。若队列 Full，保留已解析 Request 和原文直到成功接管。
4. Accepted 才把请求和业务/发送凭证移给 TaskTicket。worker 生成有界 ACK，销毁 Request，Business 缩到 256，凭证跟结果 post 回 strand。
5. strand 直接编码一份线帧，Send 缩至实际大小；ACK 对象销毁后归还 Business 和队列 outstanding，再恢复已有输入。
6. 写完成或取消后，最后一个 OutgoingFrame 所有者销毁，归还 Send。仅清空队列并不代表在途 buffer 已释放。

固定读池与动态池分开，避免空闲连接的固定内存导致动态高水位永久无法解除。资源等待登记每连接一项，待处理恢复事件每连接最多一个；预算和队列用独立代次避免漏唤醒与无事件重复尝试。

## 三个重点函数

- ResourceBudget::try_frame：看“检查三个资源→统一计费→返回凭证”的原子边界，以及 Waiting 和 Impossible 的区别。
- ResourceLease::release：看最后所有者释放如何更新计数、水位和释放代次，再在锁外投递通知。
- Session::process_buffered_input：看准入、业务消费者和 body 销毁回调怎样连接；暂停时保留输入，恢复时先处理旧数据。

下一步阅读 ResourceBudgetState::candidate 和 WakeupTicket::finish 理解竞争顺序，再看 Dispatcher::run_worker 与 Session::handle_business_result 理解跨线程资源转交。完整公式、线程和关闭语义见 architecture.md。
