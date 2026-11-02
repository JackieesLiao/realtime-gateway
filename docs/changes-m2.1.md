# M2.1 变更范围与阅读路径

本轮实现接收解析的暂停/恢复基础，新增中文注释说明状态、输入偏移、所有权和 strand 约束。保留并整理了原有学习注释。M2 整体尚未完成，下一步是 M2.2 有界业务执行。

## 文件级范围

| 文件 | 变更内容 |
| --- | --- |
| include/gateway/frame.hpp | 新增 DecodeStatus/DecodeResult；feed 改为单帧接口，新增 has_frame/body/take_body，移除原 Consumer |
| src/frame.cpp | 返回本次消费量，完整帧保留到取走；增加帧查询与存储所有权转移，长度检查仍在分配之前 |
| include/gateway/buffered_input.hpp（新增） | Session 的输入状态：固定读缓冲、偏移、消费者决定及处理状态 |
| src/buffered_input.cpp（新增） | 先交付已有完整帧，再消费原始尾部；暂停保留、终止优先、禁止覆盖旧输入 |
| src/server.cpp | Session 持有 BufferedInput；新增读在途/暂停标志、pause/resume 和统一输入推进函数 |
| CMakeLists.txt | 登记 BufferedInput 源文件和新测试文件，不增加依赖或变更编译选项 |
| tests/unit.cpp | 迁移原 Frame 单测，保留原验收覆盖，补充状态、消费量、取走和所有权边界 |
| tests/buffered_input_test.cpp（新增） | 确定性暂停/恢复、原始尾部、半包复用、停止和覆盖保护测试 |
| tests/integration.py | 新增大于固定读缓冲的真实请求，验证跨多次读取和后续连接可用性 |
| README.md | 当前交付状态、新阅读路径、格式化命令覆盖新测试文件、交付文档链接 |
| ROADMAP.md | 标记 M2.1 完成，下一步指向 M2.2；不把 M2 整体标成完成 |
| docs/architecture.md | 更新解码器/输入状态职责、恢复顺序和在途读生命周期 |
| docs/m2-plan.md | 同步 M2.1 实际交付状态与新增输入状态模块 |
| docs/changes-m2.1.md（新增） | 本文件，明确 API 与文件范围 |
| docs/validation-m2.1.md（新增） | 本轮真实构建、测试结果与验证边界 |

业务 Router/Dispatcher、protobuf 文件、JSON 配置、客户端源代码、发送队列及工具链不在本轮实现范围内。encode_frame 的行为和签名保持一致，仅整理了同文件中的学习注释和格式。

## 接口变化

旧接口 `bool feed(span, Consumer)` 被替换为 `DecodeResult feed(span)`。这是仓库内部 C++ API 变化；TCP 线上长度头和 Request/Ack 都不变。

- NeedMore：本次输入已经消费完，等待后续数据；消费量可以为 0，例如空输入。
- FrameReady：消费到当前帧边界，body 留在解析器中；未取走前再次 feed 返回 FrameReady、消费量 0。
- Error：终止状态；首次失败报告实际消费到的位置，后续调用不再消费输入。
- body() 借用完整消息体，take_body() 转移存储并重置当前帧；无完整帧时两个接口均抛出 logic_error，避免把半包交给业务。

BufferedInput 消费者返回 Accepted、Paused 或 Stopped。Accepted 前必须完成处理或取得后续所需的数据副本；Paused 表示尚未接管且不应产生该帧业务效果，恢复时会重试同一帧。借用的消息体引用不能存入异步任务。

## 为什么新增 BufferedInput

Session 仍拥有全部输入状态，但将这部分状态封装成一个可直接测试的成员。这样可以固定输入字节和暂停点，测试生产使用的同一段消费循环，而不是依赖操作系统是否把多条 send 合并为一次 recv。

它只保存一个 4096 字节读块、一个解码器和两个输入偏移，不引入工作队列、通用调度器或新的动态接收队列。

```text
本次读缓冲：[完整帧 A][完整帧 B][完整帧 C][半个尾包]
                            ↑
                   B 的消费者返回 Paused

A 已处理；B 留在解码器中；C 和尾包保留在原始缓冲中。
再次 process：重试 B → 处理 C → 保存尾包进度 → 返回 NeedRead。
```

## 线程与生命周期

Session::read 检查 closed、reading_paused、read_in_progress 和 input.can_read。只有旧输入及完整待处理帧均已消费，才能复用读缓冲；只有一条异步读链。

pause_reading/resume_reading 进入 socket executor，也就是连接 strand。暂停不会取消在途读；回调返回后先保存 bytes_received，再根据暂停状态决定是否解析。恢复期间若读仍在途，则等待该读回调，避免解析还在被网络操作写入的存储。

close 标记输入终止，但不提前销毁读缓冲。异步回调继续持有 Session，确保取消完成之前 buffer 有效。消费者触发同步关闭时，Stopped 优先于回调返回的 Accepted/Paused，不继续处理尾部帧。

## 重点阅读三个函数

1. FrameDecoder::feed：单帧边界、消费量、半包状态、长度拒绝的位置。
2. BufferedInput::process：完整帧接管与暂停、输入偏移、恢复顺序。
3. Session::process_buffered_input：串行访问、在途读保护、何时允许读新数据。

当前业务仍同步执行，不主动触发暂停。可控消费者只在测试中设置；没有生产延迟或测试开关。真实业务投递、全局预算和高低水位分别在 M2.2/M2.3 接入。暂停基础就绪不等于完整背压已经实现。
