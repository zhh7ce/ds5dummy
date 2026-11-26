#!/bin/sh
# DS5Dummy — 可重定位分发树的安装脚本
#
# 这棵树是自洽的：程序、随包库、随包插件、文档、系统配置全在一个目录里。
# 本脚本只做"让这棵树能被 systemd 拉起来"这一件事，外加把内核模块的配置装到
# 系统目录。程序自己按所在位置找配置与插件（编译时 DS5DUMMY_LAYOUT=tree），
# 所以树搬到哪、装到哪，都不需要改程序。
#
# 两种装法，默认第一种：
#
#   sudo ./install.sh                 就地：树不动，unit 的 ExecStart 指向当前目录
#   sudo ./install.sh --prefix DIR    拷贝：把树复制到 DIR，ExecStart 指向 DIR
#
# 写进系统的东西只有这四样（都在 /etc 或 /usr/lib）：
#   /etc/systemd/system/ds5dummy.service     ExecStart 指向树里的程序
#   /usr/lib/systemd/system-sleep/ds5dummy   挂起前停、恢复后起
#   /etc/modules-load.d/ds5dummy.conf        开机加载内核模块
#   /etc/modprobe.d/ds5dummy.conf            dummy_hcd 的参数（num=3、full-speed）
#
# **系统配置不写 /etc**：它是树里的 etc/config.yaml，root 所有，改身份策略就改那份。
#
# 为什么还要写 /etc：SteamOS 的 A/B 镜像更新会整份替换根文件系统，/usr 与 /etc
# 里手装的文件都会丢，家目录不参与镜像替换。所以树放家目录（或 /opt 之类），
# 每次系统更新后重跑本脚本把上面四样装回去即可。
#
# 程序以 root 跑（configfs、挂载、降权都要），所以树里的文件属主改成 root：
# 家目录可写的人就能改这个二进制的话，等于把 root 交出去。
#
# 用法：
#   sudo ./install.sh                     # 就地安装
#   sudo ./install.sh --prefix DIR        # 拷到 DIR 再装（绝对路径，不能是树自己）
#   sudo ./install.sh --no-start          # 只装、不启动服务
#   sudo ./install.sh --uninstall         # 撤系统侧；--prefix 装的连 DIR 一起删
#   ./install.sh --help

set -eu

self_dir=$(cd -P "$(dirname "$0")" && pwd)
orig_args="$*"
prefix=
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

Register the tree this script sits in with systemd: write the unit, the
suspend hook and the kernel-module configs, then start the service. The
tree itself stays where it is unless --prefix is given.

Options:
  --prefix DIR   copy the tree to DIR first and point ExecStart there
                 (absolute path; must differ from the tree itself)
  --no-start     install only; do not start ds5dummy.service
  --uninstall    stop and remove the unit, the suspend hook and the
                 module configs; with --prefix, also delete that copy.
                 The extracted tree is never deleted.
  -h, --help     this text

The system config is not copied to /etc: it lives in <tree>/etc/config.yaml
and the daemon reads it from there. Re-run the same command after a SteamOS
update: the image replaces /usr and /etc, so the unit and module configs must
be put back; the tree itself survives.
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

# 这棵树长什么样由程序决定：没有 bin/ds5dummyd 的目录不是我们的树。
[ -x "$self_dir/bin/ds5dummyd" ] ||
    die "$self_dir/bin/ds5dummyd is missing or not executable; run this script from the extracted tree"
[ -f "$self_dir/etc/config.yaml" ] ||
    die "$self_dir/etc/config.yaml is missing; it is part of the tree"

# 目标树：默认就地；--prefix 时是那份拷贝。
tree=$self_dir
if [ -n "$prefix" ]; then
    case "$prefix" in
        /*) ;;
        *) die "--prefix must be an absolute path: $prefix" ;;
    esac
    # 末尾的斜杠会让单元里的路径变成 //bin，也干扰 -ef 比较。
    prefix=$(printf '%s' "$prefix" | sed 's:/*$::')
    [ -n "$prefix" ] || die "--prefix must not be /"
    tree=$prefix
fi

if [ "$uninstall" -eq 1 ]; then
    log "stopping and disabling ds5dummy.service"
    systemctl disable --now ds5dummy.service >/dev/null 2>&1 || true
    rm -f /etc/systemd/system/ds5dummy.service
    systemctl daemon-reload >/dev/null 2>&1 || true
    rm -f /usr/lib/systemd/system-sleep/ds5dummy
    rm -f /etc/modules-load.d/ds5dummy.conf /etc/modprobe.d/ds5dummy.conf
    log "removed the unit, the suspend hook and the kernel-module configs"
    if [ "$tree" != "$self_dir" ] && [ -f "$tree/bin/ds5dummyd" ]; then
        rm -rf "$tree"
        log "removed the copy at $tree"
    fi
    log "kept the extracted tree at $self_dir (delete it by hand if you want it gone)"
    exit 0
fi

# 冒烟测试：程序带 -z now 链接，--help 会把所有动态符号解析一遍，随包库没找
# 到时（RPATH 没生效）在这里当场暴露，而不是等 systemd 每 2 秒拉一次失败。
# 就地安装时先跑树里的那份；--prefix 时先跑源树，拷贝完再跑目标树。
"$self_dir/bin/ds5dummyd" --help >/dev/null 2>&1 ||
    die "$self_dir/bin/ds5dummyd --help failed (noexec mount, or a bundled library is missing)"

# SteamOS 的根文件系统默认只读；下面的 /etc 与 /usr/lib 写入都要先解锁。
if [ -x /usr/bin/steamos-readonly ] &&
   /usr/bin/steamos-readonly status 2>/dev/null | grep -qi enabled; then
    cat >&2 <<'EOF'
ds5dummy: the SteamOS root filesystem is read-only, but this script must write
          /etc/systemd/system/ds5dummy.service and the kernel-module configs.

          Run `sudo steamos-readonly disable`, then re-run this script.
          A SteamOS image update replaces the root filesystem, so re-run it
          after every update; the tree itself is not affected.
EOF
    exit 1
fi

if [ "$tree" != "$self_dir" ]; then
    # 拷贝式安装：只拷这棵树自己的东西，源树不动。
    log "copying the tree to $tree"
    install -d -m 755 "$tree"
    for sub in bin lib plugins etc share; do
        [ -e "$self_dir/$sub" ] || continue
        rm -rf "$tree/$sub"
        cp -a "$self_dir/$sub" "$tree/$sub"
    done
    install -m 755 "$self_dir/install.sh" "$tree/install.sh"
    if [ -f "$self_dir/VERSION" ]; then
        install -m 644 "$self_dir/VERSION" "$tree/VERSION"
    fi
    "$tree/bin/ds5dummyd" --help >/dev/null 2>&1 ||
        die "$tree/bin/ds5dummyd --help failed after copying"
fi
# 树里的一切（含系统配置）都是 root 的：能改这个二进制就等于拿到 root。
chown -R root:root "$tree" 2>/dev/null || true
chmod 755 "$tree"

# 内核模块配置：内容随包，位置在系统目录。
install -Dm644 "$tree/share/ds5dummy/ds5dummy.modules-load.conf" \
    /etc/modules-load.d/ds5dummy.conf
install -Dm644 "$tree/share/ds5dummy/ds5dummy.modprobe.conf" \
    /etc/modprobe.d/ds5dummy.conf
install -Dm755 "$tree/share/ds5dummy/ds5dummy.sleep-hook" \
    /usr/lib/systemd/system-sleep/ds5dummy

# 单元是随包的模板，唯一要替换的是 ExecStart。路径里有空白时 systemd 要求
# 整段加引号。
exec_path="$tree/bin/ds5dummyd"
case "$exec_path" in
    *[[:space:]]*) exec_path="\"$exec_path\"" ;;
esac
sed_pattern=$(printf '%s\n' "$exec_path" | sed 's/[\\&|]/\\&/g')
unit_tmp=$(mktemp)
sed -e "s|^ExecStart=.*\$|ExecStart=${sed_pattern}|" \
    "$tree/share/ds5dummy/ds5dummy.service" > "$unit_tmp"
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
done < "$tree/share/ds5dummy/ds5dummy.modules-load.conf"

# HIDRAW 后端依赖 DKMS 版的 hid-playstation：in-tree 驱动会把蓝牙 DualSense
# 当普通手柄认领，不暴露桥接要读的 hidraw 节点。这份分发包不带内核模块。
if grep -qx 'hid_playstation_ds5dummy' \
       "$tree/share/ds5dummy/ds5dummy.modules-load.conf" 2>/dev/null &&
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

log "installed the tree at $tree"
log "unit:          /etc/systemd/system/ds5dummy.service (ExecStart=$tree/bin/ds5dummyd)"
log "system config: $tree/etc/config.yaml (edit this one; nothing is copied to /etc)"
log "user config:   ~/.config/ds5dummy/config.yaml"
log "example:       $tree/share/doc/ds5dummy/user-config.example.yaml"
log "config page:   $tree/bin/ds5dummy-config"
log "status:        systemctl status ds5dummy"
