#!/bin/sh
# DS5Dummy — 可重定位分发包的安装脚本
#
# 把 tarball 里的整棵树复制到用户家目录下的一个前缀（默认 ~/DS5Dummy），
# 再把只有 root 能写的那几处挂上去：
#
#   <prefix>/bin/ds5dummyd                   数据面程序（Manager + worker 同一个二进制）
#   <prefix>/lib/                            随包的 yaml-cpp（便携构建）
#   <prefix>/plugins/                        随包插件与它们的面板
#   /etc/systemd/system/ds5dummy.service     ExecStart 指向前缀里的那个程序
#   /usr/lib/systemd/system-sleep/ds5dummy   挂起前停、恢复后起
#   /etc/modules-load.d/ds5dummy.conf        开机加载内核模块
#   /etc/modprobe.d/ds5dummy.conf            dummy_hcd 的参数（num=3、full-speed）
#   /etc/ds5dummy/config.yaml                系统级配置模板（已存在则保留）
#
# 为什么二进制不装在系统目录：SteamOS 的 A/B 镜像更新会整份替换根文件系统，
# /usr 与 /etc 里手装的文件都会丢，家目录不参与镜像替换。代价是系统单元与
# /etc 下的那几份还得重装——每次系统更新后重跑一次本脚本即可（它只覆盖自己
# 那几个文件，/etc/ds5dummy/config.yaml 在的时候不动）。
#
# 程序以 root 跑（configfs、挂载、降权都要），所以复制到前缀里的文件属主改成
# root：家目录可写的人就能改这个二进制的话，等于把 root 交出去。
#
# 用法：
#   sudo ./install.sh                     # 装到 $SUDO_USER 家目录的 DS5Dummy
#   sudo ./install.sh --prefix DIR        # 换前缀（绝对路径）
#   sudo ./install.sh --user NAME         # 不用 sudo 时指定家目录属于谁
#   sudo ./install.sh --no-start          # 只装、不启动服务
#   sudo ./install.sh --uninstall         # 停服务、删单元与前缀（保留 /etc 配置）
#   ./install.sh --help

set -eu

self_dir=$(cd -P "$(dirname "$0")" && pwd)
orig_args="$*"
prefix=
user=
no_start=0
uninstall=0

die() {
    echo "ds5dummy: $*" >&2
    exit 1
}

log() {
    echo "[ds5dummy] $*"
}

usage() {
    cat <<'EOF'
Usage: sudo ./install.sh [OPTIONS]

Install the DS5Dummy tree into a home-directory prefix (default
<home>/DS5Dummy) and register the systemd unit, the suspend hook, the
kernel-module files and the system config template.

Options:
  --prefix DIR   install prefix (absolute path, default <home>/DS5Dummy)
  --user NAME    owner of that home directory (default $SUDO_USER)
  --no-start     install only; do not start ds5dummy.service
  --uninstall    stop and remove the unit and the prefix; keep
                 /etc/ds5dummy/config.yaml
  -h, --help     this text

Re-run the same command after a SteamOS update: the image replaces /usr and
/etc, so the unit and the module files must be put back. The prefix itself
lives in the home directory and survives.
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --prefix)
            [ $# -ge 2 ] || die "--prefix needs a value"
            prefix=$2
            shift 2
            ;;
        --prefix=*)
            prefix=${1#*=}
            shift
            ;;
        --user)
            [ $# -ge 2 ] || die "--user needs a value"
            user=$2
            shift 2
            ;;
        --user=*)
            user=${1#*=}
            shift
            ;;
        --no-start)
            no_start=1
            shift
            ;;
        --uninstall)
            uninstall=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            die "unknown option: $1 (try --help)"
            ;;
    esac
done

[ "$(id -u)" -eq 0 ] || die "run as root: sudo $0 $orig_args"

# 前缀的属主：sudo 装的时候是发起 sudo 的那个用户。root 自己跑的时候推不出来，
# 必须显式给 --user。
if [ -z "$user" ]; then
    user=${SUDO_USER:-}
fi
if [ -z "$user" ] || [ "$user" = root ]; then
    die "cannot tell whose home directory to use; pass --user NAME"
fi
home=$(getent passwd "$user" | cut -d: -f6 || true)
[ -n "$home" ] || die "user '$user' has no passwd entry"
[ -d "$home" ] || die "home directory of '$user' does not exist: $home"

if [ -z "$prefix" ]; then
    prefix="$home/DS5Dummy"
fi
case "$prefix" in
    /*) ;;
    *) die "--prefix must be an absolute path: $prefix" ;;
esac
# 末尾的斜杠会让单元里的路径变成 //bin，也干扰 -ef 比较。
prefix=$(printf '%s' "$prefix" | sed 's:/*$::')
[ -n "$prefix" ] || die "--prefix must not be /"

if [ "$uninstall" -eq 1 ]; then
    # 先确认这个前缀里真的是我们装的东西，再动手删。
    [ -f "$prefix/bin/ds5dummyd" ] || die "no DS5Dummy tree at $prefix (check --prefix)"
    log "stopping and disabling ds5dummy.service"
    systemctl disable --now ds5dummy.service >/dev/null 2>&1 || true
    rm -f /etc/systemd/system/ds5dummy.service
    systemctl daemon-reload >/dev/null 2>&1 || true
    rm -f /usr/lib/systemd/system-sleep/ds5dummy
    rm -f /etc/modules-load.d/ds5dummy.conf /etc/modprobe.d/ds5dummy.conf
    rm -rf "$prefix"
    log "removed $prefix and the systemd/module hooks"
    log "kept /etc/ds5dummy/config.yaml; delete it by hand if you want it gone"
    exit 0
fi

[ -x "$self_dir/bin/ds5dummyd" ] ||
    die "bin/ds5dummyd is not next to this script; run install.sh from the extracted tarball"

# 冒烟测试：程序带 -z now 链接，--help 会把所有动态符号解析一遍，随包库没找
# 到时（RPATH 没生效）在这里当场暴露，而不是等 systemd 每 2 秒拉一次失败。
"$self_dir/bin/ds5dummyd" --help >/dev/null 2>&1 ||
    die "$self_dir/bin/ds5dummyd --help failed (noexec mount, or a bundled library is missing)"

# SteamOS 的根文件系统默认只读；下面的 /etc 与 /usr/lib 写入都要先解锁。
if [ -x /usr/bin/steamos-readonly ] &&
   /usr/bin/steamos-readonly status 2>/dev/null | grep -qi enabled; then
    cat >&2 <<'EOF'
ds5dummy: the SteamOS root filesystem is read-only, but this script must write
          /etc/systemd/system/ds5dummy.service and /etc/ds5dummy/config.yaml.

          Run `sudo steamos-readonly disable`, then re-run this script.
          A SteamOS image update replaces the root filesystem, so re-run it
          after every update; ~/DS5Dummy itself is not affected.
EOF
    exit 1
fi

log "installing into $prefix"
install -d -m 755 "$prefix"
# 从 tarball 装；如果脚本本身就在前缀里（装完再跑一次），源和目的是同一份。
if [ ! "$self_dir" -ef "$prefix" ]; then
    for sub in bin lib plugins share; do
        [ -e "$self_dir/$sub" ] || continue
        rm -rf "$prefix/$sub"
        cp -a "$self_dir/$sub" "$prefix/$sub"
    done
    install -m 755 "$self_dir/install.sh" "$prefix/install.sh"
fi
chown -R root:root "$prefix"
chmod 755 "$prefix"

# 系统级配置是 root 的身份策略（谁能用、日志级别），只在没有的时候落模板：
# 覆盖它等于把用户改过的名单丢掉。
if [ -e /etc/ds5dummy/config.yaml ]; then
    log "keeping existing /etc/ds5dummy/config.yaml"
else
    install -Dm644 "$prefix/share/ds5dummy/config.yaml" /etc/ds5dummy/config.yaml
    log "installed the default system config at /etc/ds5dummy/config.yaml"
fi

install -Dm644 "$prefix/share/ds5dummy/ds5dummy.modules-load.conf" \
    /etc/modules-load.d/ds5dummy.conf
install -Dm644 "$prefix/share/ds5dummy/ds5dummy.modprobe.conf" \
    /etc/modprobe.d/ds5dummy.conf
install -Dm755 "$prefix/share/ds5dummy/ds5dummy.sleep-hook" \
    /usr/lib/systemd/system-sleep/ds5dummy

# 单元是随包的模板，唯一要替换的是 ExecStart：模板里写的是 pacman 包的位置
# /usr/bin/ds5dummyd。路径里有空白时 systemd 要求整段加引号。
exec_path="$prefix/bin/ds5dummyd"
case "$exec_path" in
    *[[:space:]]*) exec_path="\"$exec_path\"" ;;
esac
sed_pattern=$(printf '%s\n' "$exec_path" | sed 's/[\\&|]/\\&/g')
unit_tmp=$(mktemp)
sed -e "s|^ExecStart=.*\$|ExecStart=${sed_pattern}|" \
    "$prefix/share/ds5dummy/ds5dummy.service" > "$unit_tmp"
install -Dm644 "$unit_tmp" /etc/systemd/system/ds5dummy.service
rm -f "$unit_tmp"

# 模块：modules-load.d 只在开机时读，首次安装要自己加载一遍；已加载的模块
# modprobe 是空操作。
if command -v systemctl >/dev/null 2>&1; then
    systemctl daemon-reload
    systemctl restart systemd-modules-load.service 2>/dev/null || true
fi
while IFS= read -r module; do
    case "$module" in
        ''|'#'*) continue ;;
    esac
    if command -v modprobe >/dev/null 2>&1; then
        modprobe "$module" 2>/dev/null ||
            log "warning: could not load kernel module '$module'"
    fi
done < "$prefix/share/ds5dummy/ds5dummy.modules-load.conf"

# HIDRAW 后端依赖 DKMS 版的 hid-playstation：in-tree 驱动会把蓝牙 DualSense
# 当普通手柄认领，不暴露桥接要读的 hidraw 节点。这份分发包不带内核模块。
if grep -qx 'hid_playstation_ds5dummy' \
       "$prefix/share/ds5dummy/ds5dummy.modules-load.conf" 2>/dev/null &&
   command -v modinfo >/dev/null 2>&1 &&
   ! modinfo hid_playstation_ds5dummy >/dev/null 2>&1; then
    cat >&2 <<'EOF'
[ds5dummy] warning: the DKMS module hid_playstation_ds5dummy is not installed.
           The HIDRAW backend needs pkg/hid-playstation-ds5dummy; with the
           in-tree hid-playstation driver the Bluetooth controller is claimed
           as a normal gamepad and no hidraw node appears for the bridge.
EOF
fi

if mountpoint -q /sys/kernel/config 2>/dev/null; then
    mount -o remount,rw /sys/kernel/config 2>/dev/null || true
else
    mount -t configfs none /sys/kernel/config 2>/dev/null || true
fi

systemctl enable ds5dummy.service >/dev/null
if [ "$no_start" -eq 0 ]; then
    # restart 而不是 start：升级时把仍在跑旧二进制的 worker 换掉。
    systemctl restart ds5dummy.service ||
        log "ds5dummy.service did not start; see: journalctl -u ds5dummy"
fi

log "installed to $prefix"
log "unit:          /etc/systemd/system/ds5dummy.service (ExecStart=$prefix/bin/ds5dummyd)"
log "system config: /etc/ds5dummy/config.yaml"
log "user config:   $home/.config/ds5dummy/config.yaml"
log "example:       $prefix/share/doc/ds5dummy/user-config.example.yaml"
log "status:        systemctl status ds5dummy"
