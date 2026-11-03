# TCP 日志实验的正确性检查

性能结论来自 Release 的九轮串行实验；本文件记录实现与测量工具的检查，不将 sanitizer 运行计入吞吐比较。

## 已执行命令

```bash
cmake --build --preset debug
cmake --build --preset release
cmake --build --preset asan
cmake --build --preset tsan
ctest --preset debug
ctest --preset release
ctest --preset asan
setarch x86_64 -R ctest --preset tsan
python3 -m py_compile benchmarks/*.py tests/benchmark_analysis_test.py
python3 tests/benchmark_analysis_test.py
python3 benchmarks/analyze.py experiments/tcp-20261103
```

四种配置全部构建成功，CTest 均 4/4 组通过：60 个普通单元测试、12 个受控 C++ socket 测试、22 个 Python socket 测试、3 个压测分析边界测试。

新增集成用例验证默认成功日志不输出、失败日志保留、debug 恢复成功日志；原 ACK、协议错误、容量、断连、多连接和退出预算回归继续执行。配置单测新增非法日志等级和类型校验。

分析器的合成边界夹具明确不作为性能样本：预热发出但测量期收到的 ACK 计入吞吐；测量期发出但排空期收到的 ACK 计入延迟；修改吞吐计数或原始压缩文件必须失败。

## 新客户端的 sanitizer 实际运行

```bash
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
python3 benchmarks/run_experiment.py \
  --server build/asan/gateway_server --client build/asan/gateway_bench \
  --out build/benchmark-smoke-asan \
  --connections 2 --window 2 --warmup 0 --seconds 1

TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1 \
setarch x86_64 -R python3 benchmarks/run_experiment.py \
  --server build/tsan/gateway_server --client build/tsan/gateway_bench \
  --out build/benchmark-smoke-tsan \
  --connections 2 --window 2 --warmup 0 --seconds 1
```

两次均 valid=true、退出码 0；没有 sanitizer 错误，ACK/processed 一致、预算全零。生成文件在 build 中，不作为 Release 性能样本。TSan 沿用本环境的进程级 ASLR 设置；GCC 对 Boost.Asio atomic_thread_fence 的覆盖警告仍存在，apt 动态库未用 sanitizer 重编译。

## 数据与差异审阅

正式九轮的原始样本、日志哈希及客户端汇总均由独立分析器核对，plan.json 九个退出码均为 0；基线、候选及客户端二进制 SHA256 保留在各轮 metadata.json 和 source-manifest.json。测量后二进制仍与对应哈希一致。

按 AGENTS.md 审阅修改，运行项目 clang-format --dry-run --Werror 检查所有改动的手写 C++，检查缩进、控制流及行宽；Python 文件执行语法和相关用例检查。使用轮次开始时文件快照进行 git diff --no-index 审阅，检查意外修改。本地 .git 不可用，不改动它；通过 GitHub 工具以当前远端为父提交提交，校验预期分支版本。

生产差异仅是日志等级及构造前门控；性能目录新增 C++ 客户端、Python 编排/分析器、原始数据和文档。未改变业务、协议、预算、网络参数或线程数。没有执行不可用的 perf/strace 或伪造采样栈；瓶颈判断来自日志调用计数、/proc 辅助统计和重新开启日志的控制实验。

本次不验证开放到达负载、跨机器网络、长时间稳定性或生产容量。完整实测方法、代价、范围和复现命令见 [实验报告](tcp-logging-20261103.md)。
