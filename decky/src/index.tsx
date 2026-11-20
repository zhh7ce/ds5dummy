/**
 * DS5Dummy 的 Decky 面板：只读显示 + 五个动作，逻辑全在后端（§4.9、U-01～U-09）。
 *
 * 这一层刻意不做判断：状态、原因文案、按钮可用性都由后端快照里的字段决定，面板
 * 不做二次推导，也不直接读写任何文件。唯一的本地状态是"轮询到的最新快照"和
 * "正在等哪个 RPC 返回"——后者只用于把开关拨到用户点的那一侧并且立刻回弹。
 *
 * 线程模型：全部在 Steam 的 UI 线程（React）上跑；每个 RPC 都是一个 Promise，
 * 后端在自己的事件循环里执行，UI 线程只等结果。轮询计时器挂在组件卸载时清掉。
 */

import { useEffect, useState } from "react";
import type { CSSProperties } from "react";
import {
  ButtonItem,
  DialogButton,
  Field,
  Focusable,
  ModalRoot,
  PanelSection,
  PanelSectionRow,
  ToggleField,
  showModal,
  staticClasses,
} from "@decky/ui";
import { callable, definePlugin, toaster } from "@decky/api";

/** 轮询间隔（ms）。2 s 与 §4.9 一致：状态迁移比这更快的只有下载进度。 */
const POLL_MS = 2000;

// 八种状态（后端 `ds5dummy/state.py` 的 STATE_*）。字符串直接照抄，避免在两侧各
// 维护一份枚举；后端加状态时这里只会多出"未识别"的兜底显示。
const STATE_DISABLED = "disabled";
const STATE_RESOLVING = "resolving";
const STATE_UNAVAILABLE = "unavailable";
const STATE_WAITING = "waiting";
const STATE_TAKING_OVER = "taking_over";
const STATE_RUNNING = "running";
const STATE_STOPPING = "stopping";
const STATE_FAILED = "failed";

/** 有阶段名（`stage`）可显示的三类进行中状态（U-04）。 */
const STAGE_STATES: ReadonlySet<string> = new Set([
  STATE_RESOLVING,
  STATE_TAKING_OVER,
  STATE_STOPPING,
]);

/** 后端的 `get_state` 快照；字段名与 `Controller.snapshot()` 逐项对应。 */
interface Snapshot {
  state: string;
  /** 原因码（§5）；空串表示没有异常。 */
  reason: string;
  /** 原因码对应的完整中文句子；空串表示没有异常。 */
  reason_text: string;
  /** 面向用户的补充（内核错误、程序日志尾行、被拒绝的操作提示）。 */
  detail: string;
  /** 阶段名，只在进行中状态里非空（U-04）。 */
  stage: string;
  /** 下载进度百分比 0..100；-1 表示当前阶段没有可量化的进度。 */
  progress: number;
  /** 后端是否有流程在跑；面板据此禁用动作按钮。 */
  busy: boolean;
  /** 持久化的主开关（L-02），不一定等于最终状态。 */
  enabled: boolean;
  /** 当前内核 `uname -r` 全串（G-10）。 */
  uname: string;
  udc_num: number;
  /** 随包的转发程序版本（U-08）。 */
  version: string;
  module: {
    /** `custom` / `in_tree` / `none`，现读 `/proc/modules`。 */
    owner: string;
    /** `cache` / `remote` / 空串（本次还没取到模块）。 */
    source: string;
    release: string;
    path: string;
    mtime: number;
  };
  daemon: {
    pid: number | null;
    running: boolean;
    /** 最近一次退出码；从没退出过时为 null。 */
    exit_code: number | null;
    /** L-05 的重启次数。 */
    restarts: number;
    version: string;
  };
  /** WAITING 已持续的秒数（面板显示"已等 X"）。 */
  waited_s: number;
  /** 下一次自动重试还剩几秒；0 表示没排期。 */
  retry_in_s: number;
  /** `daemon.log` 最后一条非空行，WAITING 时用来说明运行环境缺少什么。 */
  log_last_line: string;
}

/** 后端的 `get_logs`（U-07）。 */
interface LogBundle {
  /** `daemon.log` 尾部若干行。 */
  daemon: string[];
  /** 本插件自己的环形缓冲，带时间与级别。 */
  events: string[];
}

const getState = callable<[], Snapshot>("get_state");
const setEnabled = callable<[enabled: boolean], Snapshot>("set_enabled");
const recheck = callable<[], Snapshot>("recheck");
const redownload = callable<[], Snapshot>("redownload");
const restartDaemon = callable<[], Snapshot>("restart_daemon");
const cleanCache = callable<[], Snapshot>("clean_cache");
const getLogs = callable<[], LogBundle>("get_logs");

/**
 * 八种状态的中文名（§4.3）。
 *
 * 状态本身是后端的事，面板只负责把字母串翻成人话；不认识的值原样显示，便于发现
 * 两侧不同步。
 *
 * @param state 快照里的 `state`。
 * @returns 面板上「状态」一行显示的文字。
 */
function stateText(state: string): string {
  switch (state) {
    case STATE_DISABLED:
      return "已关闭";
    case STATE_RESOLVING:
      return "正在取驱动模块";
    case STATE_UNAVAILABLE:
      return "当前内核没有可用模块";
    case STATE_WAITING:
      return "等待运行环境就绪";
    case STATE_TAKING_OVER:
      return "正在接管";
    case STATE_RUNNING:
      return "运行中";
    case STATE_STOPPING:
      return "正在停用";
    case STATE_FAILED:
      return "失败";
    default:
      return state;
  }
}

/**
 * 模块归属的中文说明（U-01）。
 *
 * @param owner 快照里的 `module.owner`。
 * @returns 面板上「归属」一行显示的文字；不认识的值原样透出，便于发现两侧不同步。
 */
function ownerText(owner: string): string {
  if (owner === "custom") {
    return "定制模块（本插件加载）";
  }
  if (owner === "in_tree") {
    return "in-tree（系统自带）";
  }
  if (owner === "none") {
    return "未加载";
  }
  return owner;
}

/**
 * 模块来源/状态一行的文字（U-01、M-05）。
 *
 * 三种"没有"必须分开显示：还没发布（`no_artifact`）、取不到（`network`）、取回了
 * 但校验不过（`bad_artifact`）。有模块时只显示来源，归属另起一行。
 *
 * @param snapshot 当前快照。
 * @returns 面板上「模块」一行显示的文字。
 */
function moduleText(snapshot: Snapshot): string {
  if (snapshot.module.source === "cache") {
    return "缓存命中";
  }
  if (snapshot.module.source === "remote") {
    return "已从 GitHub 取回";
  }
  if (snapshot.state === STATE_UNAVAILABLE) {
    if (snapshot.reason === "no_artifact") {
      return "没有对应模块（尚未发布）";
    }
    if (snapshot.reason === "network") {
      return "取不到模块（网络不通）";
    }
    if (snapshot.reason === "bad_artifact") {
      return "校验失败，已丢弃";
    }
    return "没有可用模块";
  }
  return "—";
}

/**
 * 程序状态一行（U-01）：运行中显示 pid，停止时把最近退出码一起写出来。
 *
 * @param snapshot 当前快照。
 * @returns 面板上「程序」一行显示的文字。
 */
function daemonText(snapshot: Snapshot): string {
  if (snapshot.daemon.running) {
    return snapshot.daemon.pid === null
      ? "运行中"
      : `运行中（pid ${snapshot.daemon.pid}）`;
  }
  if (snapshot.daemon.exit_code !== null) {
    return `已停止（最近退出码 ${snapshot.daemon.exit_code}）`;
  }
  return "已停止";
}

/**
 * 把秒数写成中文时长；只用于 WAITING 的"已等 X"。
 *
 * @param seconds 已经等待的秒数，可能带小数。
 * @returns 如 `12 秒` / `3 分 5 秒`。
 */
function formatWaited(seconds: number): string {
  const total = Math.max(0, Math.floor(seconds));
  const minutes = Math.floor(total / 60);
  if (minutes === 0) {
    return `${total} 秒`;
  }
  return `${minutes} 分 ${total % 60} 秒`;
}

/**
 * 把整份日志拼成可复制的文本（U-07：可查看、可复制）。
 *
 * @param logs 后端返回的两段日志。
 * @returns 带两段标题的纯文本，两者都为空时也有标题，便于粘贴后看出"确实没有"。
 */
function joinLogs(logs: LogBundle): string {
  const daemon = logs.daemon.length > 0 ? logs.daemon.join("\n") : "（没有内容）";
  const events = logs.events.length > 0 ? logs.events.join("\n") : "（没有内容）";
  return `== 程序日志（daemon.log 尾部） ==\n${daemon}\n\n== 插件事件 ==\n${events}\n`;
}

/** 日志弹窗里两段 `<pre>` 的统一样式：限高、可滚动、不撑破弹窗宽度。 */
const preStyle: CSSProperties = {
  maxHeight: "28vh",
  overflow: "auto",
  margin: 0,
  padding: 8,
  borderRadius: 4,
  background: "rgba(0, 0, 0, 0.35)",
  fontSize: 11,
  lineHeight: 1.4,
  whiteSpace: "pre-wrap",
  wordBreak: "break-all",
};

/**
 * 日志弹窗：两段只读文本 + 复制 / 关闭。
 *
 * @param props.logs 后端取回的日志。
 * @param props.closeModal 由 `showModal` 注入的关闭函数。
 */
function LogModal(props: { logs: LogBundle; closeModal?: () => void }) {
  const { logs, closeModal } = props;
  const copy = async () => {
    try {
      await navigator.clipboard.writeText(joinLogs(logs));
      toaster.toast({ title: "日志已复制", body: "" });
    } catch (error) {
      toaster.toast({ title: "复制失败", body: String(error) });
    }
  };
  return (
    <ModalRoot closeModal={closeModal}>
      <div style={{ marginBottom: 8 }}>
        <div className={staticClasses.PanelSectionTitle}>
          程序日志（daemon.log 尾部）
        </div>
        <pre style={preStyle}>
          {logs.daemon.length > 0 ? logs.daemon.join("\n") : "（没有内容）"}
        </pre>
      </div>
      <div style={{ marginBottom: 12 }}>
        <div className={staticClasses.PanelSectionTitle}>插件事件</div>
        <pre style={preStyle}>
          {logs.events.length > 0 ? logs.events.join("\n") : "（没有内容）"}
        </pre>
      </div>
      <Focusable style={{ display: "flex", justifyContent: "flex-end", gap: 8 }}>
        <DialogButton
          style={{ width: "auto", minWidth: 0, padding: "0 16px" }}
          onClick={() => void copy()}
        >
          复制
        </DialogButton>
        <DialogButton
          style={{ width: "auto", minWidth: 0, padding: "0 16px" }}
          onClick={() => closeModal?.()}
        >
          关闭
        </DialogButton>
      </Focusable>
    </ModalRoot>
  );
}

/** 面板主体：轮询快照、渲染只读行与动作按钮。 */
function Content() {
  const [snapshot, setSnapshot] = useState<Snapshot | null>(null);
  // 正在等一个 RPC 返回：只用来挡住重复点击，与后端的 busy 是两回事。
  const [pending, setPending] = useState(false);
  // 用户刚拨到的位置。RPC 返回前开关就停在这一侧，返回后用后端快照复核。
  const [switchTarget, setSwitchTarget] = useState<boolean | null>(null);

  // 每 2 s 取一次快照；组件卸载（面板关闭）时清掉计时器。取失败不改界面——后端
  // 可能正在重载，下一次轮询会重新连上。
  useEffect(() => {
    let alive = true;
    const poll = async () => {
      try {
        const next = await getState();
        if (alive) {
          setSnapshot(next);
        }
      } catch {
        // 静默：轮询失败本身就是"后端暂时不在"，界面上没有比旧快照更有用的东西。
      }
    };
    void poll();
    const timer = window.setInterval(() => void poll(), POLL_MS);
    return () => {
      alive = false;
      window.clearInterval(timer);
    };
  }, []);

  /**
   * 执行一个动作 RPC；成功时用返回的快照替换界面，失败时弹一条 toast。
   *
   * 后端在动作内部串行化所有系统改动，这里的 `pending` 只防同一个按钮被连点。
   *
   * @param action 无参 RPC，返回最终快照。
   * @param label 失败时 toast 标题里的动作名。
   */
  const run = async (action: () => Promise<Snapshot>, label: string) => {
    setPending(true);
    try {
      setSnapshot(await action());
    } catch (error) {
      toaster.toast({ title: `${label}失败`, body: String(error) });
    } finally {
      setPending(false);
    }
  };

  /**
   * 拨主开关。先本地落位再等后端，避免长流程里开关看起来没动。
   *
   * @param value 用户拨到的目标位置。
   */
  const toggle = async (value: boolean) => {
    setSwitchTarget(value);
    setPending(true);
    try {
      setSnapshot(await setEnabled(value));
    } catch (error) {
      toaster.toast({ title: "切换开关失败", body: String(error) });
    } finally {
      setSwitchTarget(null);
      setPending(false);
    }
  };

  /** 取日志并弹出日志窗；取不到只报警，不改变面板状态。 */
  const openLogs = async () => {
    try {
      showModal(<LogModal logs={await getLogs()} />);
    } catch (error) {
      toaster.toast({ title: "读取日志失败", body: String(error) });
    }
  };

  if (snapshot === null) {
    return (
      <PanelSection>
        <PanelSectionRow>
          <Field label="正在连接插件后端…" />
        </PanelSectionRow>
      </PanelSection>
    );
  }

  const running = snapshot.state === STATE_RUNNING;
  const unavailable = snapshot.state === STATE_UNAVAILABLE;
  const stageState = STAGE_STATES.has(snapshot.state);
  const busy = pending || snapshot.busy;
  const checked = switchTarget ?? snapshot.enabled;
  // 拨开关本身要保持可用：启用途中用户必须能关（E-09），停用途中必须能反悔。
  // 只有"不可用且现在是关的"才置灰——那种情况下拨开只会再失败一次（U-02）。
  const toggleDisabled = pending || (unavailable && !snapshot.enabled);
  const errorLine = snapshot.reason_text || snapshot.detail;

  return (
    <>
      {/* 主开关放第一段：拨开关不必先滚过一整段只读信息，进行中的状态与解除入口
          也跟着它走（E-09）。 */}
      <PanelSection title="控制">
        <PanelSectionRow>
          <ToggleField
            label="启用 DS5Dummy"
            description="让蓝牙 DualSense 以虚拟 USB 手柄的身份出现在主机上"
            checked={checked}
            disabled={toggleDisabled}
            onChange={(value) => void toggle(value)}
          />
        </PanelSectionRow>

        {/* 没有可用模块时的固定说明（U-02）：原因码已经在上面一行，这里只补出路。 */}
        {unavailable && (
          <PanelSectionRow>
            <div style={{ color: "#ffb4a9", fontSize: "0.9em", lineHeight: 1.4 }}>
              当前内核 {snapshot.uname} 没有对应的驱动模块，暂时不可用。
              <div style={{ color: "#aaa", marginTop: 2 }}>
                发布之后点「重新检查」；网络问题就检查网络再试。
              </div>
            </div>
          </PanelSectionRow>
        )}

        {/* WAITING：运行环境没就绪（E-05），显示已等多久、下次重试与程序自己说的原因。 */}
        {snapshot.state === STATE_WAITING && (
          <PanelSectionRow>
            <Field
              label={`等待运行环境就绪（已等 ${formatWaited(snapshot.waited_s)}）`}
              description={snapshot.log_last_line || undefined}
            >
              {snapshot.retry_in_s > 0
                ? `${Math.ceil(snapshot.retry_in_s)} 秒后重试`
                : "即将重试"}
            </Field>
          </PanelSectionRow>
        )}

        {/* 进行中：阶段名、可量化的进度、取消入口（U-04、E-09）。 */}
        {stageState && (
          <>
            <PanelSectionRow>
              <Field label="当前阶段">
                {snapshot.progress >= 0
                  ? `${snapshot.stage || "处理中"}（${snapshot.progress}%）`
                  : snapshot.stage || "处理中"}
              </Field>
            </PanelSectionRow>
            <PanelSectionRow>
              <ButtonItem
                layout="below"
                disabled={pending}
                onClick={() => void toggle(!checked)}
              >
                {checked ? "取消启用" : "取消停用"}
              </ButtonItem>
            </PanelSectionRow>
          </>
        )}
      </PanelSection>

      {/* 只读状态：开关下方（U-01）。每行都要 `focusable`：QAM 的滚动跟手柄焦点走，
          不可聚焦的行永远滚不进视野。 */}
      <PanelSection title="状态">
        <PanelSectionRow>
          <Field label="状态" focusable>
            {stateText(snapshot.state)}
          </Field>
        </PanelSectionRow>
        <PanelSectionRow>
          <Field label="内核" focusable>
            {snapshot.uname}
          </Field>
        </PanelSectionRow>
        <PanelSectionRow>
          <Field label="模块" focusable>
            {moduleText(snapshot)}
          </Field>
        </PanelSectionRow>
        <PanelSectionRow>
          <Field label="归属" focusable>
            {ownerText(snapshot.module.owner)}
          </Field>
        </PanelSectionRow>
        <PanelSectionRow>
          <Field label="程序" focusable>
            {daemonText(snapshot)}
          </Field>
        </PanelSectionRow>
        <PanelSectionRow>
          <Field label="重启次数" focusable>
            {`${snapshot.daemon.restarts} 次`}
          </Field>
        </PanelSectionRow>
        <PanelSectionRow>
          <Field label="转发程序版本" focusable>
            {snapshot.version || "未知版本"}
          </Field>
        </PanelSectionRow>
        <PanelSectionRow>
          <Field
            label="最近一次错误"
            focusable
            description={snapshot.reason_text ? snapshot.detail : undefined}
          >
            {errorLine || "—"}
          </Field>
        </PanelSectionRow>
      </PanelSection>

      <PanelSection title="操作">
        <PanelSectionRow>
          <ButtonItem
            layout="below"
            disabled={busy}
            onClick={() => void run(recheck, "重新检查")}
          >
            重新检查
          </ButtonItem>
        </PanelSectionRow>
        <PanelSectionRow>
          <ButtonItem
            layout="below"
            disabled={busy || running}
            onClick={() => void run(redownload, "重新下载")}
          >
            重新下载当前内核模块
          </ButtonItem>
        </PanelSectionRow>
        <PanelSectionRow>
          <ButtonItem
            layout="below"
            disabled={busy || !running}
            onClick={() => void run(restartDaemon, "重启程序")}
          >
            重启程序
          </ButtonItem>
        </PanelSectionRow>
        <PanelSectionRow>
          <ButtonItem
            layout="below"
            disabled={busy}
            onClick={() => void run(cleanCache, "清理内核缓存")}
          >
            清理内核缓存
          </ButtonItem>
        </PanelSectionRow>
        <PanelSectionRow>
          <ButtonItem
            layout="below"
            disabled={pending}
            onClick={() => void openLogs()}
          >
            查看日志
          </ButtonItem>
        </PanelSectionRow>
      </PanelSection>
    </>
  );
}

/** 插件图标：手绘的手柄轮廓；用 `currentColor` 跟随 Steam UI 的主题色。 */
function GamepadIcon() {
  return (
    <svg
      width="1em"
      height="1em"
      viewBox="0 0 24 24"
      fill="none"
      stroke="currentColor"
      strokeWidth="2"
      strokeLinecap="round"
      strokeLinejoin="round"
      aria-hidden="true"
    >
      <rect x="2" y="7" width="20" height="11" rx="5.5" />
      <path d="M8 10v4M6 12h4" />
      <circle cx="16" cy="11" r="1" fill="currentColor" stroke="none" />
      <circle cx="18.5" cy="13.5" r="1" fill="currentColor" stroke="none" />
    </svg>
  );
}

export default definePlugin(() => ({
  name: "DS5Dummy",
  titleView: <div className={staticClasses.Title}>DS5Dummy</div>,
  content: <Content />,
  icon: <GamepadIcon />,
}));
