# 游戏音频设备错误与缓冲异常故障注入测试

确定性、零硬件依赖的音频栈韧性测试。用一个 mock 输出设备模拟真实声卡行为，
向健壮音频引擎注入 5 类故障，连续运行 200 次，验证：

- **零崩溃**：每次迭代在独立 `fork()` 进程中运行，任何信号（SIGSEGV 等）
  都由父进程捕获并计为失败，测试进程本身永不崩溃。
- **零泄漏**：引擎分配全部走带校验的跟踪堆 `aheap_*`（守卫值检测越界/重复
  释放），同时编译启用 ASan + UBSan + LSan；每轮断言 live 分配数和未关闭
  设备数均归零。
- **状态可恢复**：每轮结束断言引擎回到 `ENGINE_STREAMING`、设备存活且正在
  运行；每一类已投递故障都有对应的恢复计数。
- **失败可复现**：固定种子，两次运行结果逐字节一致；失败时打印最小操作序列
  （全局日志 + 设备环形日志的尾部）。

## 文件

| 文件 | 作用 |
| --- | --- |
| `mock_audio.h/.c` | mock 音频设备：定容环形缓冲、回调周期 `mock_tick`、5 类故障注入、操作环形日志、跟踪堆 |
| `audio_engine.h/.c` | 被测健壮引擎 + 三个故意写错的反例引擎（`bad_engine_*`） |
| `test_fault_injection.c` | 200 次迭代主测试、fork 隔离、不变量检查、汇总与反例驱动 |
| `Makefile` | 构建（默认开启 ASan/UBSan） |

## 故障模型与恢复策略

| 故障 | 注入方式 | 引擎恢复动作 |
| --- | --- | --- |
| 打开失败 `OPEN_FAIL` | 预置 N 次瞬态 `mock_open` 失败 | 有界重试（最多 3 次），每次失败计数，成功即复位 |
| 热插拔 `HOTPLUG` | tick 时设备掉线，`running=0` | stop/close 旧设备 → 重新 open/start，状态机经 `REOPENING` 回到 `STREAMING` |
| 回调超时 `TIMEOUT` | tick 上报 deadline missed | 跳过本周期，不改动缓冲，下一 tick 自然恢复 |
| 欠载 `UNDERRUN` | 设备读到空缓冲 | 缓冲保留，立即补水到低水位之上，继续流式播放 |
| 过载 `OVERRUN` | 设备冲刷环形缓冲 | 接受设备自恢复，重新补水；写侧短写按背压处理（不算故障） |

每轮 60 个 tick，第 31 tick 注入一次主故障（5 类按迭代号轮转），其余 tick
以 6% 概率随机注入 4 类流式故障，因此 200 轮共覆盖约 2800 次故障事件。

## 运行

```sh
cd qa/fault_injection
make
./test_fault_injection                # 200 次主测试
./test_fault_injection counterexample # 三个反例引擎
```

## 实测汇总（参考，gcc 15.2 / WSL2 x86_64）

```
fault          injected    recovered      ratio
OPEN_FAIL            80           80     100.0%
HOTPLUG             736          736     100.0%
TIMEOUT             743          743     100.0%
UNDERRUN            633          633     100.0%
OVERRUN             604          604     100.0%
tracked heap peak live allocations: 1
tracked heap live after all iterations: 0
unclosed mock devices: 0
RESULT: 200/200 iterations passed - zero crashes, zero leaks, state recovered
```

反例（同一套不变量全部抓获）：

```
bad[leak ] reproduced: lost heap allocations (leak)
bad[stuck] reproduced: wedged REOPENING state (no recovery)
bad[crash] reproduced: SIGSEGV inside fault handler (crash)
```

## 反例说明

`audio_engine.c` 末尾的 `bad_engine_create`/`bad_engine_pump_half`/
`bad_engine_destroy` 与健壮引擎同 API，通过 `flaw` 参数选择缺陷：

- `leak`：每次故障处理都 `aheap_alloc` 一块暂存区并丢弃引用，destroy 不释放
  → 跟踪堆 live 计数无法归零，被泄漏不变量抓获（ASan/LSan 同样报告）。
- `stuck`：热插拔后不关闭/重开设备，状态机永久卡在 `REOPENING`
  → 结束时未回到 `STREAMING`，被可恢复性不变量抓获。
- `crash`：故障处理中解引用空指针 → 子进程 SIGSEGV，被 fork 隔离层抓获，
  主测试进程不受影响继续运行。

## 失败时的最小操作序列

任一轮不变量失败时，子进程通过管道把操作环形日志尾部回传给父进程，例如：

```
[FAIL INVARIANT iter=2] minimal operation sequence (oldest -> newest):
  g: engine handling HOTPLUG: reopening device
  g: close
  g: open -> OK cap=256 period=16
  g: engine open success on attempt 1
  g: unrecovered TIMEOUT: injected=6 healed=5
  d: arm-fault TIMEOUT -> next tick
  d: tick#31 -> TIMEOUT (deadline missed)
```

`g:` 为全局事件（打开/关闭/引擎决策），`d:` 为设备级事件（tick、水位、
故障投递），足以最小化重放故障路径。
