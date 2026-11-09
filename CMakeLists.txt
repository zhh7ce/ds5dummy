cmake_minimum_required(VERSION 3.16)
project(ds5dummy LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# FindBoost is gone in newer CMake; Boost ships its own config package here
# (/usr/lib/cmake/Boost-1.92.0), so use it instead of the removed module.
if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()

# ---------------------------------------------------------------------------
# 编译警告与安全加固
# ---------------------------------------------------------------------------
add_compile_options(-Wall -Wextra)
add_compile_options(-Wformat=2 -Wformat-truncation)
add_compile_options(-Wvla -Wimplicit-fallthrough -Wshadow)
add_compile_options(-fstack-protector-strong)
add_link_options(-Wl,-z,relro,-z,now)

# _FORTIFY_SOURCE 不在这里定义：它的档位是发行版工具链的策略，Arch 的 makepkg
# 已经通过 CXXFLAGS 给了 -Wp,-D_FORTIFY_SOURCE=3。这里再加一条 -D，后定义的
# 赢，等于把 =3 动态对象大小检查降级。

# 全量告警默认打开：漏一条就是让新代码带着 unchecked narrowing 入库，代价是
# 后面靠手柄静默丢包来发现。想临时关掉（比如试第三方代码）再传 -DDS5DUMMY_STRICT=OFF。
# 开 -Wconversion 的话，拼一个字节必须写 static_cast，而 static_cast 会把常量
# 溢出检查也一起压掉（constexpr 超过 255 时 -Woverflow 不再报），于是警告逼出来的
# 噪声反而盖住了真正的问题。这里只留 signedness 这一类：符号性变化才是会静默改值的。
option(DS5DUMMY_STRICT "Enable pedantic + sign-conversion + old-style-cast warnings" ON)
if(DS5DUMMY_STRICT)
    add_compile_options(-Wpedantic -Wsign-conversion -Wold-style-cast)
endif()

# 两个独立的编译期开关：DS5DUMMY_DEBUG 控制诊断代码（计数器、耗时、统计
# 窗口）是否编译，DS5DUMMY_DEBUG_LOG 控制调试日志是否编译。都按构建类型
# 取默认值：Debug 打开，其余关闭。定义按目标注入而不是 add_compile_definitions
# ——测试目标需要固定 DS5DUMMY_DEBUG=1，同一命令行上出现两个不同值的 -D 就是
# 重定义。
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(DS5DUMMY_DEBUG_DEFAULT ON)
else()
    set(DS5DUMMY_DEBUG_DEFAULT OFF)
endif()
option(DS5DUMMY_DEBUG
       "Compile diagnostic counters, write timings and the stats window"
       ${DS5DUMMY_DEBUG_DEFAULT})
option(DS5DUMMY_DEBUG_LOG "Compile debug-only log statements"
       ${DS5DUMMY_DEBUG_DEFAULT})

# ---------------------------------------------------------------------------
# 蓝牙后端选择
#   HIDRAW — BlueZ kernel hidraw (当前默认，依赖 BlueZ 建 ACL+HID session)
#   L2CAP  — 原生 AF_BLUETOOTH SOCK_SEQPACKET 裸 L2CAP（未来切回，绕过 hid_playstation）
# 切换方式：cmake -DBT_BACKEND=L2CAP ..
# ---------------------------------------------------------------------------
set(BT_BACKEND "HIDRAW" CACHE STRING "Bluetooth backend: HIDRAW or L2CAP")
set_property(CACHE BT_BACKEND PROPERTY STRINGS HIDRAW L2CAP)

# Manager 与 worker 同成一个二进制，两边共用的源文件只列一次。
# 目录 = 模块（按 include 依据归位）：
#   core   协议与设备静态模型（protocol/device）
#   daemon options（命令行）、shutdown（signal 与退出标志）
#   daemon 进程形态：main 分发的两个入口(manager/worker) + 交接三件套
#   usb    gadget 实例化(configfs) + FunctionFS 端点/数据面线程（门面 usb.h）
#   bt     蓝牙后端（链接期二选一）+ 门面 bt_link + 读/写两条传输线程
#   bridge 转发 + 设备状态(state_report/fake_mac)，0x36/0x32 报告在这里拼
#   audio  双向 PipeWire 音频链（门面 audio.h），只搬数据，不认报告格式
set(DS5DUMMY_CORE_SRC
    src/main.cpp
    src/core/protocol/input_state.cpp
    src/core/protocol/output_state.cpp
    src/daemon/options.cpp
    src/daemon/config.cpp
    src/daemon/identity.cpp
    src/daemon/shutdown.cpp
    src/daemon/worker.cpp
    src/daemon/handoff.cpp
    src/daemon/privdrop.cpp
    src/daemon/discovery.cpp
    src/daemon/manager.cpp
    src/usb/gadget_setup.cpp
    src/usb/ffs_device.cpp
    src/usb/usb.cpp
    src/usb/usb_plane.cpp
    src/usb/usb_inout.cpp
    src/usb/usb_ep0.cpp
    src/bt/bt_link.cpp
    src/bt/bt_plane.cpp
    src/bt/bt_rx.cpp
    src/bt/bt_tx.cpp
    src/bt/bt_detail.cpp
    src/bridge/bridge.cpp
    src/bridge/fake_mac.cpp
    src/bridge/state_report.cpp
    src/bridge/audio_report.cpp
    src/bridge/usb_report.cpp
    src/audio/audio.cpp
    src/audio/audio_identity.cpp
    src/audio/audio_port.cpp
    src/audio/audio_sink.cpp
    src/audio/audio_encoder.cpp
    src/audio/audio_decoder.cpp
    src/audio/audio_source.cpp
    src/plugin/plugin.cpp
    src/plugin/plugin_loader.cpp
    third_party/loguru.cpp
)

# 单一搜索路径 src/：跨模块 include 必须写成 <dir>/<dir>.h 门面（内部头同理
# 带目录前缀），一眼能看出它属于哪个模块、是否越过了门面边界。
set(DS5DUMMY_MODULE_INCLUDE_DIRS
    src
)

# ---------------------------------------------------------------------------
# 配置校验器（buttons_check）：只读一份用户配置，用 buttons 插件自己的语法层
# 解析它那一节。不装载 .so、不需要 root/手柄/PipeWire。
# ---------------------------------------------------------------------------
function(ds5dummy_add_buttons_check)
    add_executable(buttons_check tools/buttons_check.cpp
        tools/buttons_config.cpp
        plugin/buttons/settings.cpp plugin/buttons/ids.cpp
        src/daemon/config.cpp src/daemon/options.cpp)
    target_include_directories(buttons_check PRIVATE
        ${DS5DUMMY_MODULE_INCLUDE_DIRS} ${CMAKE_SOURCE_DIR})
    target_link_libraries(buttons_check PRIVATE PkgConfig::YAMLCPP)
endfunction()

# ---------------------------------------------------------------------------
# 只编配置校验器（打包用）：ds5dummy-config 是独立于转发程序的包，页面与校验器
# 不需要 PipeWire/Opus/BlueZ 这些只为数据面存在的依赖。打开后只产出
# buttons_check，其余目标都不注册。
# ---------------------------------------------------------------------------
option(DS5DUMMY_CONFIG_ONLY "Build only buttons_check (for the split config package)" OFF)
if(DS5DUMMY_CONFIG_ONLY)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(YAMLCPP REQUIRED IMPORTED_TARGET yaml-cpp)
    ds5dummy_add_buttons_check()
    return()
endif()

if(BT_BACKEND STREQUAL "L2CAP")
    message(STATUS "BT backend: raw AF_BLUETOOTH L2CAP (BlueZ pkg required)")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(BLUEZ REQUIRED bluez)

    add_executable(ds5dummyd ${DS5DUMMY_CORE_SRC} src/bt/bt_l2cap.cpp)
    target_include_directories(ds5dummyd PRIVATE ${DS5DUMMY_MODULE_INCLUDE_DIRS} third_party ${BLUEZ_INCLUDE_DIRS})
    target_link_libraries(ds5dummyd PRIVATE Threads::Threads ${BLUEZ_LIBRARIES} fmt)
    target_compile_definitions(ds5dummyd PRIVATE BT_BACKEND_L2CAP)

else()
    # 默认 HIDRAW
    message(STATUS "BT backend: hidraw (BlueZ handles ACL+HIDP framing)")

    add_executable(ds5dummyd ${DS5DUMMY_CORE_SRC} src/bt/bt_hidraw.cpp)
    target_include_directories(ds5dummyd PRIVATE ${DS5DUMMY_MODULE_INCLUDE_DIRS} third_party)
    target_link_libraries(ds5dummyd PRIVATE Threads::Threads fmt)
    target_compile_definitions(ds5dummyd PRIVATE BT_BACKEND_HIDRAW)
endif()

target_compile_definitions(ds5dummyd PRIVATE
    DS5DUMMY_DEBUG=$<BOOL:${DS5DUMMY_DEBUG}>
    DS5DUMMY_DEBUG_LOG=$<BOOL:${DS5DUMMY_DEBUG_LOG}>)

# ---------------------------------------------------------------------------
# 随包插件：源码在仓库根的 plugin/（不在 src/ 里），每个插件单独编成一个 .so。
#   插件只 include 公共 ABI 头（plugin/plugin_api.h、plugin_util.h），不链接宿主
#   任何符号——buttons 这一份单独一条
#       c++ -std=c++20 -fPIC -shared -I src plugin/buttons/*.cpp -o buttons.so
#   就能编出来。产物文件名与 plugin/plugin.h 的 kSystemPluginDir、两个 PKGBUILD
#   的安装位置必须一致。
# ---------------------------------------------------------------------------
add_library(ds5dummy_idle MODULE plugin/idle/idle.cpp)
set_target_properties(ds5dummy_idle PROPERTIES
    PREFIX "" OUTPUT_NAME "idle" SUFFIX ".so" POSITION_INDEPENDENT_CODE ON)
target_include_directories(ds5dummy_idle PRIVATE ${DS5DUMMY_MODULE_INCLUDE_DIRS})

add_library(ds5dummy_buttons MODULE
    plugin/buttons/buttons.cpp
    plugin/buttons/ids.cpp
    plugin/buttons/settings.cpp
    plugin/buttons/sense.cpp
    plugin/buttons/pipeline.cpp)
set_target_properties(ds5dummy_buttons PROPERTIES
    PREFIX "" OUTPUT_NAME "buttons" SUFFIX ".so" POSITION_INDEPENDENT_CODE ON)
target_include_directories(ds5dummy_buttons PRIVATE ${DS5DUMMY_MODULE_INCLUDE_DIRS})

# ---------------------------------------------------------------------------
# 音频下链（PipeWire sink → Opus → 0x36）—— 两个后端都要，缺库直接 configure
# 失败：静默退化成 HID-only 会让除用户配置 audio: false 之外的人不知道音频
# 没起来。
# ---------------------------------------------------------------------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
pkg_check_modules(SPA REQUIRED IMPORTED_TARGET libspa-0.2)
pkg_check_modules(OPUS REQUIRED IMPORTED_TARGET opus)
pkg_check_modules(SAMPLERATE REQUIRED IMPORTED_TARGET samplerate)

# ---------------------------------------------------------------------------
# 启动配置（/etc/ds5dummy/config.yaml 与用户家目录下的同名文件）是 YAML；
# 缺 yaml-cpp 同样直接 configure 失败。
# ---------------------------------------------------------------------------
pkg_check_modules(YAMLCPP REQUIRED IMPORTED_TARGET yaml-cpp)
# 降权身份从 systemd-logind 的图形会话推导（sd_get_sessions 等）；缺库同样
# 直接 configure 失败，避免服务起来后才发现身份策略不可用。
pkg_check_modules(SYSTEMD REQUIRED IMPORTED_TARGET libsystemd)
# boost/circular_buffer.hpp — header-only, only the headers are checked.
find_package(Boost REQUIRED)

target_link_libraries(ds5dummyd PRIVATE
    PkgConfig::PIPEWIRE PkgConfig::SPA PkgConfig::OPUS PkgConfig::SAMPLERATE
    PkgConfig::YAMLCPP PkgConfig::SYSTEMD
    Boost::headers
    # plugin/loader.cpp 的 dlopen/dlsym；glibc 上它是独立的 libdl。
    ${CMAKE_DL_LIBS})

# loguru: 单头文件日志库。LOG_F 的等级过滤是运行时的（低于阈值时参数不被
# 求值，但代码与格式串仍在二进制里）；把调试日志整条去掉由 DS5DUMMY_DEBUG_LOG
# 负责。
target_compile_definitions(ds5dummyd PRIVATE LOGURU_USE_FMTLIB=1)

# pthreads — 两个后端都需要
find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# 诊断/验证工具（可选）
#   cmake -DDS5DUMMY_BUILD_TOOLS=ON ...
# ---------------------------------------------------------------------------
option(DS5DUMMY_BUILD_TOOLS "Build diagnostic tools in tools/" OFF)
if(DS5DUMMY_BUILD_TOOLS)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(BLUEZ REQUIRED bluez)

    add_executable(spike_l2cap tools/spike_l2cap.cpp)
    target_link_libraries(spike_l2cap PRIVATE ${BLUEZ_LIBRARIES} Threads::Threads)
    target_include_directories(spike_l2cap PRIVATE ${BLUEZ_INCLUDE_DIRS})

    add_executable(spike_rumble tools/spike_rumble.cpp)
    target_link_libraries(spike_rumble PRIVATE ${BLUEZ_LIBRARIES} Threads::Threads)
    target_include_directories(spike_rumble PRIVATE ${BLUEZ_INCLUDE_DIRS})

    add_executable(mini_gadget tools/mini_gadget.cpp src/usb/gadget_setup.cpp
        src/usb/ffs_device.cpp src/daemon/options.cpp
        third_party/loguru.cpp)
    target_link_libraries(mini_gadget PRIVATE Threads::Threads fmt)
    target_include_directories(mini_gadget PRIVATE ${DS5DUMMY_MODULE_INCLUDE_DIRS} third_party)
    target_compile_definitions(mini_gadget PRIVATE LOGURU_USE_FMTLIB=1)

    # 离线回听：audio 下行链 + dump sink，不链 usb/bt/bridge，手柄连不连都能跑。
    # tools/ 的头按 <repo>/tools/<file>.h 引用，所以搜索路径加仓库根。
    add_executable(audio_dump tools/audio_dump.cpp tools/dump_sink.cpp
        src/audio/audio.cpp src/audio/audio_identity.cpp
        src/audio/audio_port.cpp src/audio/audio_sink.cpp
        src/audio/audio_encoder.cpp
        src/audio/audio_decoder.cpp src/audio/audio_source.cpp
        third_party/loguru.cpp)
    target_include_directories(audio_dump PRIVATE
        ${DS5DUMMY_MODULE_INCLUDE_DIRS} ${CMAKE_SOURCE_DIR} third_party)
    target_link_libraries(audio_dump PRIVATE
        Threads::Threads fmt Boost::headers
        PkgConfig::PIPEWIRE PkgConfig::SPA PkgConfig::OPUS PkgConfig::SAMPLERATE)
    target_compile_definitions(audio_dump PRIVATE LOGURU_USE_FMTLIB=1)

    ds5dummy_add_buttons_check()

    # 本地配置页面是 tools/buttons_web.py + tools/buttons_web.html：页面不参与编译，
    # 判定仍然由上面的 buttons_check 给出，Python 只负责读取、写回与起服务。
    message(STATUS "Diagnostic tools enabled: spike_l2cap, spike_rumble, mini_gadget, audio_dump, buttons_check")
endif()

# ---------------------------------------------------------------------------
# 源码文档（Doxygen，可选）
#   cmake --build <build> --target doxygen
# Doxygen 缺失时本目标不注册，正常构建不受影响。输出 docs/doxygen/（已 gitignore）。
# ---------------------------------------------------------------------------
find_package(Doxygen OPTIONAL_COMPONENTS dot)
if(DOXYGEN_FOUND)
    add_custom_target(doxygen
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_SOURCE_DIR}/docs/doxygen
        COMMAND ${DOXYGEN_EXECUTABLE} ${CMAKE_SOURCE_DIR}/Doxyfile
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Generating API docs with Doxygen -> docs/doxygen/html"
        VERBATIM)
    if(NOT DOXYGEN_DOT_FOUND)
        message(WARNING "Doxygen found but no dot: dependency/class graphs disabled in docs")
    endif()
endif()

# ---------------------------------------------------------------------------
# 单元测试（googletest + CTest，无需 root/手柄）
#   cmake -DDS5DUMMY_BUILD_TESTS=ON ...
# ---------------------------------------------------------------------------
option(DS5DUMMY_BUILD_TESTS "Build googletest unit tests under tests/" OFF)
if(DS5DUMMY_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
