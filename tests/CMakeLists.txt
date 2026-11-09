# DS5Dummy 单元测试
#
# 全部测试必须能在无 root、无手柄的环境下运行：需要特权或硬件的验证
# 由人工清单覆盖，归用户执行。

find_package(GTest REQUIRED)
find_package(Threads REQUIRED)

add_executable(ds5dummy_tests
    core_test.cpp
    crc32_test.cpp
    options_test.cpp
    input_state_test.cpp
    output_state_test.cpp
    state_report_test.cpp
    core_descriptors_test.cpp
    audio_encoder_test.cpp
    fake_mac_test.cpp
    audio_report_test.cpp
    mic_report_test.cpp
    power_report_test.cpp
    audio_identity_test.cpp
    test_dump_sink.cpp
    handoff_test.cpp
    config_test.cpp
    identity_test.cpp
    discovery_test.cpp
    bt_test.cpp
    bt_tx_test.cpp
    bt_cache_test.cpp
    usb_test.cpp
    usb_report_test.cpp
    usb_ep0_test.cpp
    bridge_hook_test.cpp
    plugin_test.cpp
    plugin_idle_test.cpp
    plugin_util_test.cpp
    buttons_ids_test.cpp
    buttons_settings_test.cpp
    buttons_sense_test.cpp
    buttons_pipeline_test.cpp
    ../src/daemon/options.cpp
    ../src/daemon/config.cpp
    ../src/daemon/discovery.cpp
    ../src/daemon/identity.cpp
    ../src/core/protocol/input_state.cpp
    ../src/core/protocol/output_state.cpp
    ../src/audio/audio_encoder.cpp
    ../src/audio/audio_identity.cpp
    ../src/bridge/bridge.cpp
    ../src/bridge/fake_mac.cpp
    ../src/bridge/state_report.cpp
    ../src/bridge/audio_report.cpp
    ../src/bridge/usb_report.cpp
    ../src/bt/bt_link.cpp
    ../src/bt/bt_plane.cpp
    ../src/bt/bt_rx.cpp
    ../src/bt/bt_tx.cpp
    ../src/bt/bt_detail.cpp
    ../src/plugin/plugin.cpp
    ../src/plugin/plugin_loader.cpp
    ../plugin/idle/idle.cpp
    # buttons 的四个模块层 TU 直接编进来单测（装配层 buttons.cpp 不在其中：它和
    # idle.cpp 都导出同名 extern "C" 的 ds5_plugin_desc，两份进同一个二进制会撞
    # 符号；它那一层的武装与不生效路径由下面的真 .so 走装载链覆盖）。
    ../plugin/buttons/ids.cpp
    ../plugin/buttons/settings.cpp
    ../plugin/buttons/sense.cpp
    ../plugin/buttons/pipeline.cpp
    ../tools/dump_sink.cpp
    ../src/daemon/handoff.cpp
    ../src/daemon/privdrop.cpp
    ../src/usb/usb.cpp
    ../src/usb/usb_plane.cpp
    ../src/usb/usb_inout.cpp
    ../src/usb/usb_ep0.cpp
    ../src/usb/ffs_device.cpp
    ../src/usb/gadget_setup.cpp
    ../third_party/loguru.cpp
)
target_include_directories(ds5dummy_tests PRIVATE ../src .. ../third_party)
# 统计断言依赖诊断代码，所以测试目标固定打开 DS5DUMMY_DEBUG；日志门保持
# 关闭，顺带让空宏形态在每个构建里都被编译一遍。
target_compile_definitions(ds5dummy_tests PRIVATE
    LOGURU_USE_FMTLIB=1
    DS5DUMMY_DEBUG=1
    DS5DUMMY_DEBUG_LOG=0
    # 只给测试存在的钩子（idle 的时间拨表）在生产构建里不编进来。
    DS5DUMMY_TESTING=1
    # 用户配置模板的路径：用例把这份发布文件解析出来，和内置默认层逐项比对，
    # 这样"模板宣告的默认值"与"家目录没有配置时的生效值"不会各走各的。
    DS5DUMMY_USER_TEMPLATE="${CMAKE_CURRENT_SOURCE_DIR}/../pkg/user-config.example.yaml")
target_link_libraries(ds5dummy_tests PRIVATE
    GTest::gtest
    GTest::gtest_main
    Threads::Threads
    fmt
    # audio_encoder 的出包边界测试：不连 PipeWire，直接灌样本，所以要 Opus + SRC。
    PkgConfig::OPUS
    PkgConfig::SAMPLERATE
    # 配置解析（config_test.cpp）直接依赖 yaml-cpp。
    PkgConfig::YAMLCPP
    # 身份选择（identity_test.cpp）的生产实现要链接 libsystemd 的 sd-login。
    PkgConfig::SYSTEMD
    # 硬件身份（audio_identity_test.cpp）只写内存里的 pw_properties 字典，不连
    # daemon，但那些符号来自 libpipewire。
    PkgConfig::PIPEWIRE
    # plugin_loader.cpp 用 dlopen/dlsym 装载 .so 插件，测试走的是同一条路。
    ${CMAKE_DL_LIBS}
)

# 插件装载测试需要一个真的 .so：夹具按 plugin_api.h 单独编译成 MODULE，不链接
# 测试二进制，也不链宿主——它证明的正是"第三方只拿得到那份 C 边界"。
add_library(ds5dummy_plugin_probe MODULE fixtures/plugin_probe.cpp)
set_target_properties(ds5dummy_plugin_probe PROPERTIES
    PREFIX "" SUFFIX ".so" POSITION_INDEPENDENT_CODE ON)
target_include_directories(ds5dummy_plugin_probe PRIVATE ../src)

# 同一份源码再编一份导出宿主不认的 ABI 版本号：装载点必须拒绝它。
add_library(ds5dummy_plugin_probe_bad_abi MODULE fixtures/plugin_probe.cpp)
set_target_properties(ds5dummy_plugin_probe_bad_abi PROPERTIES
    PREFIX "" SUFFIX ".so" POSITION_INDEPENDENT_CODE ON)
target_include_directories(ds5dummy_plugin_probe_bad_abi PRIVATE ../src)
target_compile_definitions(ds5dummy_plugin_probe_bad_abi PRIVATE
    DS5_PLUGIN_TEST_ABI_VERSION=2)

# 同一份源码再编一份不带标记节的：导出名与版本号都对，但装载器必须在 dlopen
# 之前就把它当成"不是我们的插件"跳过。
add_library(ds5dummy_plugin_probe_no_marker MODULE fixtures/plugin_probe.cpp)
set_target_properties(ds5dummy_plugin_probe_no_marker PROPERTIES
    PREFIX "" SUFFIX ".so" POSITION_INDEPENDENT_CODE ON)
target_include_directories(ds5dummy_plugin_probe_no_marker PRIVATE ../src)
target_compile_definitions(ds5dummy_plugin_probe_no_marker PRIVATE
    DS5DUMMY_TEST_NO_MARKER=1)

add_dependencies(ds5dummy_tests ds5dummy_plugin_probe
                                 ds5dummy_plugin_probe_bad_abi
                                 ds5dummy_plugin_probe_no_marker
                                 ds5dummy_idle
                                 ds5dummy_buttons)
target_compile_definitions(ds5dummy_tests PRIVATE
    DS5DUMMY_TEST_PROBE_SO="$<TARGET_FILE:ds5dummy_plugin_probe>"
    DS5DUMMY_TEST_PROBE_BAD_ABI_SO="$<TARGET_FILE:ds5dummy_plugin_probe_bad_abi>"
    DS5DUMMY_TEST_PROBE_NO_MARKER_SO="$<TARGET_FILE:ds5dummy_plugin_probe_no_marker>"
    # 随包插件的真实产物：用例把它复制进临时插件目录，走与第三方插件完全相同的
    # 扫描 + 标记 + dlopen 路径。
    DS5DUMMY_TEST_IDLE_SO="$<TARGET_FILE:ds5dummy_idle>"
    DS5DUMMY_TEST_BUTTONS_SO="$<TARGET_FILE:ds5dummy_buttons>")

# bt 契约测试链接共享门面（bt_link.cpp）加选中的那个后端 TU：
# bt::detail::IsOpen 直接是后端符号，不碰硬件。其余 bt_* 要真链路，这里
# 不测。BLUEZ 变量由父项目在 L2CAP 分支已 find。
if(BT_BACKEND STREQUAL "L2CAP")
    target_sources(ds5dummy_tests PRIVATE ../src/bt/bt_l2cap.cpp)
    target_compile_definitions(ds5dummy_tests PRIVATE BT_BACKEND_L2CAP)
    target_include_directories(ds5dummy_tests PRIVATE ${BLUEZ_INCLUDE_DIRS})
    target_link_libraries(ds5dummy_tests PRIVATE ${BLUEZ_LIBRARIES})
else()
    target_sources(ds5dummy_tests PRIVATE ../src/bt/bt_hidraw.cpp)
    target_compile_definitions(ds5dummy_tests PRIVATE BT_BACKEND_HIDRAW)
endif()

include(GoogleTest)
# 构建期枚举 TEST 名，ctest 才能看到逐个用例而不是整个二进制。
gtest_discover_tests(ds5dummy_tests)

# 本地配置页面（tools/buttons_web.py + buttons_web.html）的 Python 单测：页面
# 不参与编译，判定仍由 buttons_check 给出。缺 Python3 或 PyYAML 时整组不注册。
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    execute_process(
        COMMAND ${Python3_EXECUTABLE} -c "import yaml"
        RESULT_VARIABLE _buttons_web_pyyaml
        OUTPUT_QUIET ERROR_QUIET)
    if(_buttons_web_pyyaml EQUAL 0)
        # 保存路径要 buttons_check 做权威判定；有它就一并构建并把路径交给用例。
        # 用 `cmake -E env` 传，而不是测试的 ENVIRONMENT 属性：后者在旧 CMake 上
        # 不展开生成器表达式。
        set(_buttons_web_checker "")
        if(TARGET buttons_check)
            add_dependencies(ds5dummy_tests buttons_check)
            set(_buttons_web_checker "$<TARGET_FILE:buttons_check>")
        endif()
        add_test(NAME buttons_web_python
            COMMAND ${CMAKE_COMMAND} -E env
                "DS5DUMMY_BUTTONS_CHECK=${_buttons_web_checker}"
                ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/../tools/test_buttons_web.py)
        set_tests_properties(buttons_web_python PROPERTIES
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    else()
        message(STATUS "buttons_web Python tests skipped: PyYAML is missing")
    endif()
endif()
