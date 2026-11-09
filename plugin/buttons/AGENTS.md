# AGENTS.md — buttons 插件程序侧说明

面向要改这一层代码的人。内容全部从当前代码与构建脚本推出，不含历史叙述。
配置怎么写、有哪些取值语法、用户会踩哪些限制在 [README.md](README.md)，那份文档不讲
函数、文件与线程，这一份不重复它的用法。

---

## 1. 这个 `.so` 在契约里的位置

插件与宿主之间只隔 `src/plugin/plugin_api.h` 那份 C 边界，`kPluginAbiVersion` 当前是 1。
导出的是 `ds5_plugin_desc`（标记节 `.ds5_plugin`、名字 `buttons`、四个槽），四个槽各自
跑在哪条线程、能做什么，全部由宿主决定：

| 槽 | 调用线程 | 这一层做什么 |
|---|---|---|
| `init` | worker 主线程，`bt::StartThreads()` 之前 | 验 `host->abi_version`；`settings::Load` 填 `g_rules`；`sense::Reset` 按规则条数定容；打一行武装摘要；返回 0 表示这一轮不生效 |
| `stop` | worker 主线程，两条传输线程 join 之后 | 只清武装标志，并把攒着的关机请求丢掉 |
| `on_state_in` | bt 读线程，100–120 Hz | 解码快照 → 判定 → 执行 → 必要时置 `g_power_off_pending` |
| `on_tick` | worker 主循环，100 ms | 把 pending 搬给 `host->power_off()` |

三条纪律是从线程相位推出来的，不是风格：

- **`on_state_in` 里一条日志都不打**。100–120 Hz 的路径上打日志就是刷屏，而且 loguru 的锁
  会出现在输入链上。全部日志在 `init`（语法回落、武装摘要）与 `on_tick`（动作失败）。
- **`host->power_off()` 只在 `on_tick` 调**。它同步走 `bt::SetFeature`，预算最长 2 s；搬到
  读线程上就是把整条输入链停住（需求 K-10）。
- **pending 取走即清，失败不放回去**。想再要一次动作得重新按那组键（K-11），否则控制通道
  每 100 ms 被撞一次。

不加锁的依据是这张表：`g_rules` 只在 `init` 里写、之后每帧只读；`g_state` 只在 bt 读线程上
读写；`g_host`/`g_armed` 由 `init`/`stop` 写、两个钩子读；主循环一个字节都不碰前三者。
唯一的跨线程量是 `std::atomic<bool> g_power_off_pending`，它就是把识别（读线程）和执行
（主循环）隔开的那一次交接。

插件不链宿主、不引用宿主符号：编译命令里只有 `-I src`，能看到的两份头是
`plugin/plugin_api.h` 与 `plugin/plugin_util.h`。`nm -D --undefined-only buttons.so` 只应
出现 libstdc++/libc 的符号，这一条是人工检查，不进 CI。

---

## 2. 模块划分与 include 方向

| 文件 | namespace | 唯一职责 | 不含 |
|---|---|---|---|
| `buttons.cpp` | `buttons` | 装配：描述符、静态存储、四个函数体 | 语法、布局、判据 |
| `ids.h` `ids.cpp` | `buttons::ids` | id 空间、token 表、63 字节编解码、轴形状原语 | 配置文本、规则、状态 |
| `rules.h` | `buttons::rules` | 规则表的内存形状与全部缺省值/区间 | 解析函数（没有 `.cpp`） |
| `settings.h` `settings.cpp` | `buttons::settings` | 配置文本 → `rules::Set`，语法错误的三种下场 | 63 字节、判定 |
| `sense.h` `sense.cpp` | `buttons::sense` | 跨帧判定：时序状态机、连点相位、宏脉冲、方向迟滞 | 不写缓冲一个字节 |
| `pipeline.h` `pipeline.cpp` | `buttons::pipeline` | 一帧的执行：位一趟、轴一趟、末尾一次写回 | 不含时间，不持状态 |

依赖方向只有一条规则：**ids 谁都不依赖，rules 只依赖 ids，settings / sense / pipeline 依赖
这两个基础模块，装配层依赖全部**。被多个 TU include 的头是 `ids.h` 与 `rules.h`；
`settings.h` 只被装配层 include，`sense.h` 被装配层与 `pipeline.h` include ——
`pipeline::Apply` 的入参就是 `sense::FrameDecision`，这条 include 是签名要求的，不是越界。
模块划分的判据是三个变化频率不同的东西各自独立：布局（协议变了才变）、语法（加功能组
就变）、时序（判定与执行）。加一条触发形态改 `settings.cpp` + `rules.h` + `sense.cpp`；
加一种输入形态（触摸板那族就是这样进来的）改 `ids.*`（打包字解码、判据与 token）+
`sense.*`（那一份跨帧状态挂在 `State` 里）。

`plugin/` 不是宿主那套模块门面体系的一部分：这个插件不对外暴露任何东西，因此没有
`detail` 命名空间。

---

## 3. 63 字节状态体的布局

`ids.cpp` 里那几张表是这一层唯一的写法来源：单比特键位表 `kSlots`、hat 双向表
`kHatDirs`/`kHatOf`、轴偏移、触摸点打包字的偏移。**这是 `src/core/protocol/input_state.cpp` 那张解码表的第二处
写法**——插件拿不到宿主的协议头，所以只能自己编一遍；两份实现由
`tests/buttons_ids_test.cpp` 逐字段对账（触摸点那两个坐标也在对账之内），漏改任何一边都会在那里失败，而不是变成"按 ○ 出 ✕"
这种静默错误。

| 字节 | 内容 |
|---|---|
| `[0..5]` | `lx ly rx ry l2 r2`，与 `Axis` 枚举同序（`IndexOf` 就是下标） |
| `[6]` | 序号/时间戳一类，不碰 |
| `[7]` 低 nibble | 十字键 hat 值：0..7 是八个方向（顺时针从正北起），8 是未按下，9..15 手柄不报、解码时与未按同列 |
| `[7]` 高 nibble | bit4 □、bit5 ✕、bit6 ○、bit7 △ |
| `[8]` | bit0 起依次 `L1 R1 L2 R2 Create Options L3 R3` |
| `[9]` | bit0 起依次 `Home Pad Mute`、bit3 未解释、bit4 起 `左Fn 右Fn 左背 右背` |
| `[32..35]` `[36..39]` | 两个触摸点，各一个 32 位小端字：低 7 位是手柄自己的触点序号（**与"第几个点"不是一件事，本插件不看它**）、bit7 **未接触**、bit8..19 x、bit20..31 y，两个坐标各 12 位。`[40]` 的触摸时间戳不读 |
| `[10..62]` 其余 | 传感器、`[55..62]` 的 CMAC，全部不碰 |

`EncodeFrame` 的写集合是 `[0..5]`、`[7]`、`[8]`、`[9]` 里本插件拥有的七个位。
触摸板那两字是**只读**的：插件判半区与划动，但一个字节都不写回去。
`[9]` bit3（core 里叫 `unknown_button`）从原缓冲带过来再写回去：插件不生产的位连 0 都不生产。
因此 `DecodeFrame` 与 `EncodeFrame` **不是彼此的逆**——解码多读的位不参与往返，这一点由
ids 单测断言"同一份 63 字节读进来再写回去，逐字节相同"。

十字键在这套模型里是**四个方向位**，不是线上那个 hat 值，两者无损对应：hat 的 8 个有效值
恰好是这四个位的全部组合。所以编码时"同一轴上两个相反方向都置位"那一轴取居中
（`HatOf` 里 `up == down` 与 `right == left` 各归 0），于是需求 M-10（按轴合并）与 M-11
（只抹一个方向）就是掩码上的位运算，不需要特判对角。

---

## 4. id 空间与 token

`Source` 枚举顺序 = token 表顺序 = 比特表顺序，三条由 `static_assert` 钉住。可绑定源
一共 40 个：23 个线上真存在的位（十字键按四个方向计）+ 8 个摇杆方向 + 9 个触摸板形态
（`touch_left` `touch_right` `touch_two` `pad_left` `pad_right` 与四个 `swipe_*`）。
`IsWireBit(src)` 就是"下标 < `kLstickUp`"，它的判据（17 个判出来的源排在 23 个线上位之后）
也由 `static_assert` 钉住。判出来的两类源各有自己的边界谓词：`IsStickDirection`（八个方向
那一块）与 `IsSwipe`（最后四个），两者都由相邻枚举值的 `static_assert` 钉住排列。

`IsWireBit` 有两处决定性用途，都是"G-03 的输出面"的落地：

- **目标必须是线上位**。八个方向是从轴判出来的形态、触摸板那族是从坐标判出来的形态，
  都没有任何 wire bit 可以去置，所以 `settings` 在解析**输出**那一侧（`remap` 的目标、
  `macro` 的键组）时拒绝它们。
- **连点只能配在线上位**，并且另外排除 `l2 r2 pad`：扳机的形态是行程、触摸板只有一个物理
  开关（需求 T-03）。

源那一侧宽松得多：八个方向与触摸板那九个都能当触发串里的项（`sense` 只看"这一帧这一位成
不成立"）。唯一的例外是**划动不能当 `remap` 的源**——`remap` 的形状是"源按着、目标就按着"，
而划动是一次性事件，套上去等于把一句手势拖成一串按住。要一次性的输出就写 `macro`
（`settings` 在语法层拒掉并留一行 WARNING，那一行顺便指一句"想划动出键请配 macro.N"）。

`GateBitsOf(src)` 是这一族位的第三个用途，也是唯一一个"判出来的源却有线上位要抹"的地方：
`pad_left`/`pad_right` 展开成 `pad` 那一位，其余判出来的源返回 0（半区触点自己不该抹掉触摸板
按下位——那是"任意半区"那条绑定的东西）。摇杆方向不需要这种展开：游戏读的是轴值，轴那边由
`AxisRewrite` 与 M-09 那套走。

`Mask` 用 `uint64_t`：23 + 8 + 9 = 40 位已经在用，`uint32_t` 做完触摸板那族之后只剩 24 位，
而六轴手势（I-05）要占多少还没算过——这一位宽不是按它设计的，只是别在它还只是猜测的时候
就把宽度定死。

轴侧的两个配对函数把布局前提写成了断言：`StickOther`（同一根摇杆的另一根轴，X/Y 相邻 →
最低位取反）、`SwapPartner`（另一根摇杆上同方向的轴，左右两对相差 2）。`center_lstick` 与
`swap_lstick_rstick` 的解析都走这两个函数，不写下标奇偶。

`Invert(v) = 256 - v`，`v == 0` 时夹回 255。写成 `255 - v` 会让静置点 128 变成 127，那是需求
M-14 要避免的事故。代价是 0 与 1 都落到 255，这一端不可逆——一个以 128 为唯一不动点的对合
必须丢掉一端的刻度，按 M-14 选"丢刻度"而不是"静置点漂移"。

---

## 5. 一帧的数据流

```
OnStateIn(state):
  frame    = ids::DecodeFrame(state)        // 快照，栈上，POD
  decision = sense::Evaluate(g_rules, frame, plugin::NowMs(), g_state)
  pipeline::Apply(g_rules, decision, frame, state)   // 只读快照，末尾一次 EncodeFrame
  if decision.power_off: g_power_off_pending.store(true)
```

三个读取点各拿各的东西，这是需求 K-07 与 M-15 的全部实现方式：

- `sense::Evaluate` 收到的是 `const ids::Frame&`，**拿不到可写的字节**——判定不受本插件改写
  结果影响，由类型保证而不是由纪律保证。
- 摇杆方向判据读的是快照里那四根轴的原始值（`StickDirections`），所以轴互换与轴反向改不到
  它（M-17）："左摇杆上推触发 X"不会跟着一次互换变成右摇杆的事。触摸板那族同理读快照里
  那两个触点（`TouchpadStates`），它只读不改 `frame.digits`。
- `pipeline::Apply` 的所有取值都来自 `frame` 与 `FrameDecision`，从不回读刚写进结果的字节的
  值。`左X ↔ 右X` 因此不可能拿到两份左摇杆（M-15），链式映射因此不可能叠加（M-06）。

时刻由调用方注入（`NowMs()` 在 `OnStateIn` 里取一次），所以 `sense` 可以在单测里被一帧一帧
地喂虚拟时钟。**这里没有 `idle` 那种 `PretendIdleFor()`**：不需要只在测试构建里存在的拨表入口。

---

## 6. 表算法

### 6.1 数字位：一趟 gate/inject

`pipeline::Apply` 的第一趟，在一个 `Mask` 上算完：

```
out.digits = (decision.digits & ~gate) | inject | decision.inject
```

- `decision.digits` 是**映射层这一帧的输入**：线上 23 位 ∪ 判出的 8 个方向位 ∪ 判出的触摸板
  那 9 位 ∪ 连点的开半周期 −连点的关半周期。连点塑形排在映射之前，就是需求 T-04
  那句"连点作用在物理按键上、映射作用在结果上"的数据结构表达。
- `gate` 由每条 `remap` 在 `!keep_source` **且这一帧源的那个位真的按着**时把 `GateBitsOf(源)`
  并进去（M-02 的默认"换掉"）；`+keep`（M-03）因此只是"这条规则不往 gate 里放东西"，不需要
  第二个分支。"且真的按着"只对半区点击有意义：`pad_left` 抹的是 `pad` 那一位，而那位能被
  右半点击或任意半区按着——无条件并进来的话，一条只绑了左半的规则会顺手抹掉别人的那一下。
  绑在线上位上的规则两种写法等价，因为没按下的位本来就不在 `digits` 里。
- `inject` 是"源在 `decision.digits` 里置位"时并上的目标位集合。多条规则指向同一目标自然
  是 OR（M-05）。目标为空集就是抹掉（M-04），所以 `rules::Remap` 没有 `erase` 布尔量。
- `decision.inject`（宏脉冲的位）排在最后，**不参与映射**——宏要的就是那组键本身。
- 每条规则只看 `decision.digits`，不看上一轮的结果，两跳因此表达不出来（M-06）。

### 6.2 六根轴：取值 → 整形

第二趟对每根轴独立算一次，取值那一步的优先级是五档：

```
强制（宏全按 / 方向命中期间居中） > 映射行程 > 抹掉归零 > 互换（读对方） > 快照里的自己
```

然后才轮到整形：`invert`、`always_center`。整形排在最后有个附带好处——128 是 `Invert` 的
不动点，所以"居中"与"反向"谁先谁后结果一样，这一族组合不必再判。

- **映射行程**（M-08/M-16）：源是扳机时搬的是源轴的实际深度，非扳机的键驱动扳机时是全按
  255。`Active()` 在源扳机只要**轴值非 0** 就算有事可做，不看那个要压过阈值才置位的数字位
  ——否则"缓慢压 L2、R2 跟着动"在阈值之前整个不发生。
- 一个目标扳机被多条规则投值时取**最深**的那个（`ShapeAxes` 里的 max）：主机看到的是一根
  扳机，不是三个来源。
- `has_travel` 那格里 `hidden` 与非 `hidden` 分两种取法：没有别的规则换掉这根扳机时，投来的
  行程与自己手指的深度取 max（多一个驱动源，不是抹掉这只手）；被换掉了才用 `hidden` 那条
  支路。
- **抹掉归零**（M-09）：源扳机被换掉时 `hidden[]` 置位，那一格取 `kTriggerReleased`。游戏读
  扳机读的是轴值，只抹数字位等于什么都没抹掉。
- **互换**（M-12）在 `AxisRewrite::read_from` 里就是"读对方那一格"，`kCount` 表示读自己；
  因为读的是快照，所以整根换、按轴换、两条规则互为来源都是同一段代码。
- `rules::axis[]` 是**定长六格表**而不是 vector：pipeline 每帧不必找"有没有针对这根轴的
  条目"，"这根轴没被改写"就是默认构造的那个形状（`IsIdentity()`）。

### 6.3 扳机的两半

扳机的数字位在 `digits` 里、模拟量在 `axis[]` 里，同一个源的两半由
`TriggerAxisOf(Source)` 从位上找到轴（只有 `kL2`/`kR2` 有，其余返回 `kCount`）。所以
`Remap` 不需要 `travel_axis` 字段，`Macro` 不需要 `axes` 掩码——两半各自在所属那一趟里被同
一条 gate/inject/取值规则管到，`L2 → R2` 天然就是把两半一起搬。

---

## 7. 触发判定的状态机

一条触发（`shortcut` 或 `macro` 共用 `rules::Trigger`）占 `State::slots` 里一格，`Advance()`
每帧推进一格。两种沿各管一件事：

- **按下沿**（这组键刚刚全部按下）：记下 `press_start_ms`；双击在这里数第二下——
  `awaiting_second` 且与上一次按下的间隔 ≤ 窗口才算命中，然后更新 `last_press_start_ms`。
- **松手沿**（刚刚不再全部按下）：单击在这里结算（按住多久只有松手这一刻才知道，K-02/K-05）；
  双击在这里把 `awaiting_second` 打开。
- **组合与长按不需要沿**：它们要的就是"当前全部按下"这一件事，配上闸门就只出一轮。

四个判据的位置：

- `AllDown(terms, physical)` 是"全部按下"，`terms == 0` 永远 false（一条没有源的触发不可能命中）。
- **K-08 的闸门**只在 `AllReleased()` 时打开（这组键**一个都没按着**）。以"不再全部按下"为
  复位点的话，按住组合里的一个键、反复敲另一个键就会一下一下出动作。
- `slot.fired` 一旦置位，本轮不再判：单击命中之后不再等第二下（否则一次命中之后的第一次
  松手就重新武装，之后每一下都算一次双击，而一次双击要消耗两下）。
- `physical` 取的是塑形之前的那份位集合（`frame.digits | direction_latch | TouchpadStates()`），
  所以映射与连点的结果不会喂给触发判定（K-07）。

**宏脉冲**是一次性的（A-05）：命中时 `pulsing = true`、`pulse_until_ms = now + 80`，之后每帧
把 `macro.digits` 并进 `inject`，到点自己收，**不看那组键还按不按着**。目标位里含扳机时
`PressTriggers()` 把那一根轴强制成全按（A-03）。

**连点相位**按经过的毫秒数推，不按帧数推（帧率是 100–120 Hz 的浮动值，按帧数计频会让达成
频率随蓝牙抖动）。半周期 `HalfPeriodMs(hz) = round(500/hz)`，至少 1 ms：

- 键没按下 → `on = true`、`edge_ms = now`。松手立即停（T-01），并且**下一次按下从"开着"这一半
  开始**；从关起算的话第一帧读到的是一次空拍，玩家那一下按下去游戏看不见。
- 落后不足一个完整周期但跨过半周期 → 翻一沿，`edge_ms += half`（不跳帧，保持相位连续）。
- 落后超过一个完整周期 → `edge_ms = now`，**一个沿都不补**。逐帧追欠的话，欠得越久之后每
  一帧都要翻一次，那是一串真机上不该出现的快闪。

因此达成的频率只会低于配置值，绝不高于（T-02）。

**摇杆方向**每帧由 `StickDirections(frame, kStickStep=48, kStickHysteresis=10, latch)` 判：
每根轴一个带符号投影（线上 y 向下增长，所以"上"是负值），已锁存的方向用松开线 38 判、未锁存
的用触火线 48 判，跨过中心投影变负自然解除。判出的位存在 `State::direction_latch`，
**从不写进状态体**——它们不是线上的位。

**触摸板那族**每帧由 `ids::TouchpadStates(frame, io.touch)` 判，跨帧那一份存在
`State::touch`（与 `direction_latch` 同一条约定：判据在 ids，状态由 sense 代管）。三件事：

- **半区在触点出现那一刻定下**（`HalfOf`：离中线 8% 那条带里算"任意"，不属于任何一半），
  之后原样保留到这个触点离开。手指滑过中线时归属不跟着当前坐标跑，否则一次划动会中途从
  `touch_left` 变成 `touch_right`。
- **`pad_left`/`pad_right` 是"按下位 ∧ 那一侧有起始触点"**：面板上没有任何触点时两个都不成立，
  线上那个 `pad` 位因此仍然只代表"任意半区"（I-03 的三种绑定互斥就是这么表达的）。
- **划动位是事件位，不是电流位**：位移折算过 `kSwipeMinSpan`（12% 宽度）就置位，并且**保持到
  所有触点离开**，一次手势只置一次。这样 `AllDown`/`AllReleased` 那一套时序机器看得见一次完整
  的按下—松开——事件只出现一帧的话，触发判定每帧读到的都是"没按下"。方向与长度都先折算到
  同一刻度上（面板实物 16:9，而两个坐标各自跑满 0..4095，raw 刻度是各向异性的），所以"斜着划"
  归到位移真正更长的那一侧。

**I-02 的居中**分两半，分别在两个模块：`always` 是静态配置，落在 `AxisRewrite::always_center`；
"只在方向命中期间居中"是运行期的，`Evaluate()` 第 4 步只对**既被判出、又被某条规则当源用过**
的方向（`BoundSources()` 扫四条 vector 的源位）把整根摇杆的两根轴都写进 `forced_axis`。只居中
一根轴会把摇杆推到对角线上，所以配对走 `StickOther`。没有任何规则用这个方向时，静置摇杆
不该被插件动过一根轴。这一圈的边界是 `IsStickDirection`，**不是"到 `Source::kCount` 为止"**：
触摸板那族同样是判出来的位，但它没有"自己的摇杆"，`StickAxisOf` 对它返回 `Axis::kCount`，
走进 `CenterStick` 就是往一根不存在的轴上写强制值。

---

## 8. 配置解析的三种下场

`settings::Load` 遍历 `ds5Setting` 数组、按组名前缀分派（`remap.` / `axis.` / `turbo.` /
`shortcut.` / `macro.`）。**不靠精确查键**：摊平后交给插件的键本身就带点，组内条目数不定，
而且数组不保证同组相邻，所以也不假设顺序。

一行配置只有三种结局：

| 结局 | 触发条件 | 日志 |
|---|---|---|
| 变成一条规则 | 组名、键名、取值全部读得通 | 无 |
| 整行忽略 | 组名不认识，或键名不认识（C-05） | DEBUG 一行 |
| 这一条不生效 | 键名认得，取值读不通 | WARNING 一行，其余照常（G-06） |

写成 DEBUG 而不是 WARNING 的理由：**写错的键名和还没实现的键名在语法层分不开**，报成
WARNING 就是把后者也说成前者的错。取值读不通才是用户真的能修的东西，所以那一行要带上键路径
原文与取值原文。

- 让服务退出在任何一种下场里都不存在（G-06）。
- `Load` 返回 false 只有一种含义：`out.Empty()`——五张表全空且六格轴表全是恒等（C-04）。
  轴表按**内容**判，不按"有没有写过这一组"判：只写一句 `invert_rstick_x` 同样是要生效的配置。
- 数值区间都在 `rules.h`：阈值 50..10000 ms、频率 1..60 Hz。数值走 `plugin::ParseInt` 的严格
  十进制（只认非负数字，越界即拒）。
- **后缀必须在所有 token 之后且只能有一个**（`~`、`*2`、`>毫秒`）：`l3~+r3` 在语法层就被拒，
  因为组合要求"全部按下"、单击要求"松手才判"，同一条规则里两者互斥。
- 触发串的 `+` 在冒号两边含义不同且不需要消歧：左边是"与"（`sense` 只看），右边是"同时按下"
  （`pipeline` 只看），两边的处理者不同。
- `Trim` 只在值侧生效：`l3 + r3` 与 `l3+r3` 是同一句话。键名用不到这一步——宿主的 `IsToken`
  已经保证键段里没有空格。
- 日志前缀 `"buttons: plugins: buttons: …"` 由插件自己写全（宿主给插件日志加不了前缀），
  与随包 `idle` 的文案同形。
- **`>毫秒` 必须显式写**：长按没有缺省值。宏脉冲时长也不可配，恒为 `kDefaultPulseMs = 80`，
  所以 `rules::Macro` 里没有 `pulse_ms` 字段——留一个没人能拧的旋钮等于留一处误导。

---

## 9. 测试怎么组织

插件的 TU 像 `idle.cpp` 一样直接编进 `ds5dummy_tests`（`tests/CMakeLists.txt` 里列 4 个被测源
+ 4 个测试源），所以模块边界能逐个覆盖；真实 `.so` 的装载链单独走
`tests/plugin_test.cpp`。**全部测试无 root、无手柄**。触摸板那一族的用例喂的是自己摆的
两个 32 位触摸点字，与真机一样不需要硬件。

| 文件 | 覆盖 | 承重断言 |
|---|---|---|
| `tests/buttons_ids_test.cpp` | token ↔ 位一一对应；hat 0..7 ↔ 四方向无损、9..15 归一成未按；`Invert` 的不动点；触摸点解码与半区/触点数/划动的判据 | **与 `core::protocol::DecodeInputState` 逐字段相等**（防"按 ○ 出 ✕"）；`DecodeFrame` → `EncodeFrame` 往返后整份 63 字节逐字节相同（这一条正好盖住 `[6]`、`[9]` bit3、`[10..62]`，触摸板那两字因此确实只是读）；`TouchContactsDecodeLikeTheCoreDecoder` 把那两个打包字也对到 core；`TheHalfIsSetWhenTheContactAppearsAndHeldUntilItLeaves`（半区锁存）、`SwipeDirectionWeighsActualDistanceNotRawTicks`（16:9 折算）、`OnlyTheHalfClicksExpandToAWireBitTheyDoNotOwn`（`GateBitsOf` 的那个例外） |
| `tests/buttons_settings_test.cpp` | 五组语法的正反例逐条；区间；未知组名/token 被忽略；空节；触摸板那族的三种角色（能做源、不能做目标、划动不能做 remap 的源） | 每条反例只让那一条缺席 + **恰好一条** WARNING；`Set::Empty()` |
| `tests/buttons_sense_test.cpp` | K-01..K-05、K-07/K-08、T-01..T-05、A-01/A-02/A-05、I-01/I-02、I-03/I-04 | 时钟逐帧注入。命名即判据的那几条：`PartialReleaseDoesNotReArmTheGate`（K-08 复位点）、`FirstFrameOfAPressIsStillPressed`（重武装从"开着"起）、`ALateFrameDoesNotCauseAFlickerBurst`（不追欠）、`FourQuickTapsAreTwoDoubles`（一次双击消耗两下）、`InjectedBitsNeverFeedTheTrigger` / `TurboFramesDoNotFeedTheTrigger`（K-07）、`DirectionsAreJudgedFromTheRawAxes`（M-17）、`ASwipeFiresOncePerGestureAndReArmsOnLift`（划动作为事件位喂给既有时序机）、`ATouchpadBindingCentersNoAxis`（I-02 那个遍历边界不越过八个方向） |
| `tests/buttons_pipeline_test.cpp` | M-01..M-17 + 半区点击的 gate | 逐位断言；M-06 两跳不出现；M-05 三路 OR；M-08/M-16 行程搬运；M-09/M-10/M-11 按轴；M-14 静置点仍是 128；M-15 对调不重复取值；`EverythingThePluginDoesNotProduceIsLeftAlone`；`AClickOnTheOtherHalfIsNotTheBoundOne`（绑左半的规则抹不掉右半那一次按下，因为 pad 那一位是两条半区共用的） |

装配层（描述符形态、`init` 的两条不返回路径、动作落在心跳而不是钩子、随包 `.so` 真的被扫出）
不单开文件，在 `tests/plugin_test.cpp` 的随包用例里——那一组走的是与真实安装完全相同的
`ListPlugins`/`HasMarker`/`dlopen`/`dlsym` 路径（`DS5DUMMY_TEST_BUTTONS_SO`），比一个自制的
`kDesc` 断言更接近交付物。触摸板那族在这里也各有一条：`ShippedButtonsRewritesAHalfClick`
从裸字节摆出左半那一次按下，断言主机读到的是 ✕ 而触摸板位被抹掉，再摆右半那一次断言它
不受这条规则影响；`ShippedButtonsPowersOffOnASwipe` 摆一次上划，断言关机动作仍然落在
心跳而不是钩子上。

测试二进制在严格开关下构建（`-Wsign-conversion -Wold-style-cast -Wmissing-field-initializers`），
所以**构造 `rules::Set` 一律走测试里的 builder 函数，不用聚合初始化**：`AxisRewrite` 那一格
数组没有默认初值可写，缺一个成员就会在新增字段时静默填 0。

---

## 10. 构建

```bash
# 随主构建：CMakeLists.txt 里的 ds5dummy_buttons 目标（MODULE，不链接任何东西）
cmake --build build --target ds5dummy_buttons

# 脱离本项目单独编（这是"插件只 include 那两份头"的可执行证明）
c++ -std=c++20 -fPIC -shared -I src plugin/buttons/*.cpp -o buttons.so

# 配置校验器（tools/buttons_check.cpp，只在 -DDS5DUMMY_BUILD_TOOLS=ON 时存在）：
# 编进 ids + settings 与 daemon 的 YAML 解析，不 dlopen、不链接插件本体，逐条
# 打印三种下场与整节摘要。它和按钮本体的武装摘要共用 settings::Summary。
cmake --build build --target buttons_check

# 本地配置页面（tools/buttons_web.py + 壳 + 面板）：同一套判定另加"按层读一段、
# 只写回这一段"。它不参与编译，也不重复插件语法：
#   buttons_config.*  一行 → 三种下场（两个工具共用）
#   buttons_web.py    PyYAML 只用来取行号，文本按行搬运（注释/引号/键序不动）；
#                     回环监听、Host/Origin 检查、原子写回，判定仍调
#                     buttons_check --json --layer。它不知道任何插件的语法：
#                     CODECS 里只登记 buttons 的模型↔文本，其余插件走 flat 键值。
#                     层清单 = 文件里的 controllers + 已连接但未写进文件的手柄；
#                     页面手写的 MAC 由 plugin_state 接住，保存时才落成条目。
#                     api/plugin 回的那一节自带词表（catalog），面板不再取。
#   buttons_web.html  壳，也是唯一的读文件者：画顶部的服务/层选择/只读当前状态，
#                     按当前层把 api/state 与每个插件的 api/plugin 取回来装进
#                     DS5.sections，再以 ctx 推给各面板的 refresh。推过去的节已去掉
#                     layer/layers/path；面板不认 global/controllers.<MAC>、不读配置
#                     路径、不发请求。插件名与层由壳在收草稿时补进 {kind:'plugin'}，
#                     节内标量走 {kind:'section_set'|'section_remove'}，路径前缀由壳拼。
#                     基础配置是根级 enabled/bt_addr/log_level 与当前层的六项，由
#                     tools/panels/core.js 画（ctx.base 里带 inherited）。
#   buttons.panel.js  本插件的 UI，与 buttons.so 同目录。63 字节解码表在这里是
#                     第二处写法，与 ids.cpp 的对账在 test_buttons_web.py。
#                     词表从 ctx.catalog 来，判定走 ctx.validate(items)。
# 两个包都装 /usr/bin/ds5dummy-config（入口脚本）、/usr/lib/ds5dummy 下的
# buttons_web.py + 页面壳 + panels/core.js，以及 /usr/bin/buttons_check；
# 插件面板随各自的 .so 装在插件目录里。
ds5dummy-config --config /path/to/config.yaml --no-open
python3 tools/buttons_web.py --config /path/to/config.yaml --no-open  # 源码树
```

`tools/buttons_web.py` 的写回路径与 worker 读配置走的是同一个判定：拼出的整份文档
先交给 `buttons_check`（它调用 daemon 的解析器）过一遍才落盘，所以"页面写出一个宿主
读不进去的文件"不在可能里。`tools/test_buttons_web.py` 用临时文件覆盖读、写、删与
新建四种路径，其中"原文不变则整份文件逐字节相同"是这一层的承重断言；三种层各有一条
用例，确认缩进能把同一段文本摆回 `plugins` / `global` / `controllers.<MAC>` 下。
基础配置那边另有标量改写（行尾注释不动）、删键时的祖先级联、`controllers.<MAC>` 新建
条目与大小写不敏感的幂等、`discover_panels` 的目录口径（只看普通 `.so`，用户目录覆盖
系统目录），以及面板路由只发登记过的那几个文件。层这一侧另有：已连接但文件里没写的
手柄进层清单、`plugin_state` 接受页面手写的 MAC 并回出 `global` 的继承值、保存时为
新层补出 `controllers.<MAC>:`；`plugin_state` 对带 CODECS 的插件回 `catalog`、对没有的
回 `null`，`api/catalog` 那条路由已经删掉；配置文件不存在时 `state.exists` 为 false、
插件节 `present` 为 false 仍可编辑，首次保存建出目录（`0o700`）与文件（`0o600`）。
页面固定走 `/`，旧的令牌路径不存在。
全部无 root、无手柄、不读真实用户配置目录。

两个 PKGBUILD 各把产物装进 `/usr/lib/ds5dummy/plugins/buttons.so`，面板脚本装成同目录的
`buttons.panel.js`，README 装进 `/usr/share/doc/ds5dummy/buttons.md`。装载由目录决定、
不由配置决定，插件名决定顺序（`buttons` 排在 `idle` 之前，所以 `idle` 看到的是本插件
改写后的字节）。

---

## 11. 这一层已知不做 / 未验证

- **规则之间没有仲裁**（需求 K-06 移出本期）。每条规则各自判定、各自松手闸门，所以
  `R3` 与 `R3 + 十字上` 并存时短的那条会先命中、长的跟着命中。
- **同一个键上 `~` 与 `*2` 各绑一条会双触发**：单击在松手沿命中，双击在第二下的按下沿命中，
  两条的 `fired` 互不遮挡。
- **六轴手势（I-05）不做**，`[10..29]`（陀螺仪/加速度计）与 `[40..54]` 因此一律不读。
  触摸板那族已经接进来了：`[32..39]` 那两个打包字读，从不写。
- **触摸板的几何判据不可逐条覆盖**：半区中线两侧那条 8% 的带、划动的最小位移 12%，
  连同摇杆方向的阈值与迟滞，都是 `ids.h` 里的全局值（经验值，按 16:9 的面板折算纵向
  位移）。需求 I-01 那句"阈值可按条覆盖"到本期仍然没有对应语法，I-03/I-04 同样没有留
  逐条旋钮的入口。
- **划动只能当触发的项与宏的项，不能当 `remap` 的源**：事件位没有"按住"的形态，映射它
  等于把一次手势拉长成持续几帧的按键。想要"划一下出几下"是 `macro.N`。
- **CMAC 前提未验证**：`[55..62]` 是手柄自己算的 AES-CMAC，密钥在手柄里。本插件改写
  `[0..5]`/`[7..9]` 之后，那份报告的签名与内容不再匹配。本项目从不校验它，但"主机与游戏也不
  校验它"目前**没有证据**——现有链路里输入状态一直原样转发，这是第一次改这些字节。
  "不碰插件不生产的字节"只是把风险面收窄，不构成豁免。
- **跨插件只看得到结果**：排在 `buttons` 之后的插件（按名字就是 `idle`）读到的是改写后的
  状态体。按住一个被抹掉的键，`idle` 会认为静置。这一条是 ABI 单缓冲契约的既有代价，不在本
  插件里补。
