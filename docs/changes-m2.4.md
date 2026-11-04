# M2.4 变更范围与阅读路径

本轮实现每连接消息/字节限流、应用心跳、空闲/半帧/等待/写入期限。保持 M2.3 预算、线程和 ACK 完成语义；停止仍立即取消，优雅 drain 留到 M2.5。本步没有重新压测，performance 目录中的旧数据不变。

## 文件范围

| 文件 | 变更及原因 |
| --- | --- |
| include/gateway/connection_policy.hpp、src/connection_policy.cpp（新增） | 显式时间点的整数令牌桶、等待状态、暂停扣时、统一期限判断 |
| include/gateway/config.hpp、src/config.cpp、config/gateway.json | 四个速率/突发字段、四个毫秒期限，启动时校验 |
| include/gateway/frame.hpp、src/frame.cpp | 暴露当前帧是否已开始，避免重试重复触发首字节事件 |
| include/gateway/buffered_input.hpp、src/buffered_input.cpp | 同步 Started/Completed 观察事件，跨分片及暂停只报告一次 |
| include/gateway/resource_budget.hpp、src/resource_budget.cpp | cancel_wait 允许本地限流计时接手恢复，避免阻塞全局等待者 |
| src/server.cpp | 包头处限流，记录读块时间，单 timer/代次、超时关闭和取消，保留 RAII 预算 |
| proto/gateway.proto | 追加 HEARTBEAT=2，既有字段及枚举值不变 |
| include/gateway/business.hpp、src/business.cpp | 共用 validate_request 校验；心跳空 payload 规则和独立统计 |
| src/client.cpp | 保留原用法，新增可选 telemetry/heartbeat 模式 |
| CMakeLists.txt | 登记策略实现及单测 |
| tests/connection_policy_test.cpp（新增） | 手动时间点精确验证令牌补充、双维准入、四类期限、暂停及旧唤醒 |
| tests/unit.cpp、tests/buffered_input_test.cpp、tests/resource_budget_test.cpp | 心跳/配置、输入事件不重复、撤销局部限流等待后的公平推进 |
| tests/async_server_test.cpp | 资源等待超时、执行业务期间超时后的迟到结果、真实 timer 旧唤醒检查 |
| tests/integration.py | 心跳及客户端、空闲/半包关闭、数量/字节限流和恢复、永久 burst 超限 |
| README.md、ROADMAP.md、docs/architecture.md、docs/protocol.md、docs/m2-plan.md | 更新配置、时间语义、升级兼容与阶段状态 |
| docs/changes-m2.4.md、docs/validation-m2.4.md（新增） | 本轮范围、三个重点函数和实际验证 |

共 30 个文件。AGENTS.md、CMakePresets、业务队列/Dispatcher、压测工具与原始实验数据均未修改。新增代码有中文注释；不对无关文件重新排版。

## 关键选择

完整合法包头先检查两个令牌桶，再原子申请全局帧资源，最后扣费一次。暂时不足保留包头/尾部并交给 timer；单帧超过 burst 直接拒绝，避免永久等待。限流等待退出全局等待列表，其余连接继续竞争预算。

ConnectionPolicy 不依赖 socket，也不自己调用 Clock::now；用显式时间点测试真正的边界。idle、wait、write 是墙钟期限；frame 使用扣除服务端暂停的 peer 时间。原始读块记录两条时间轴，帧被延后解析不能伪造新的活动或首字节。

每连接一个 timer 覆盖最早事件。延后期限保留旧唤醒并重新判断，提前或关闭时取消并递增 generation，过滤已经排队的旧回调。timer 使用 weak_ptr；异步读写仍持强引用保持缓冲，close 后不提前归还资源。成员输入 Observer 仅同步借用 this，Session 明确禁止移动。

## 三个重点函数

1. TokenBucket::refill（src/connection_policy.cpp）：定点信用、补充量及乘法前的溢出保护；结合 delay 看向上取整。
2. ConnectionPolicy::expired（同文件）：按当前状态检查墙钟和 peer 时间，理解暂停与截止边界。
3. Session::handle_timer（src/server.cpp）：检查取消/代次、重新判断策略、令牌恢复及重新排期。

再读 Session::admit_frame 理解限流与预算如何对接，读 BufferedInput::process 理解 Started/Completed 为何跨重试只触发一次。配置及 wire 语义见 README 和 protocol.md。
