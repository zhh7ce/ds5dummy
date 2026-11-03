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
)

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
