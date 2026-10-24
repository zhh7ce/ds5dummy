// DS5Dummy — minimal FunctionFS HID gadget, used to bisect the bring-up.
// Nothing here depends on Bluetooth, PipeWire or the production bridge.

#include "core/device/descriptors.h"
#include "usb/gadget_setup.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <linux/usb/ch9.h>
#include <linux/usb/functionfs.h>

// This tool drives the usb bring-up API directly, bypassing the production
// facade: GadgetSetup/GadgetBind/GadgetTeardown/FindDummyUdc live in
// usb::detail.
using namespace usb::detail;

namespace {

// linux/hid.h is not self-contained on every kernel headers install.
constexpr uint8_t HID_REQ_GET_REPORT = 0x01;
constexpr uint8_t HID_REQ_GET_IDLE = 0x02;
constexpr uint8_t HID_REQ_GET_PROTOCOL = 0x03;
constexpr uint8_t HID_REQ_SET_REPORT = 0x09;
constexpr uint8_t HID_REQ_SET_IDLE = 0x0A;
constexpr uint8_t HID_REQ_SET_PROTOCOL = 0x0B;

volatile sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

// A plain joystick: 8 buttons + X/Y. No report ID, so the shape never has to
// match anything the kernel knows — its only job is to be parseable.
constexpr uint8_t MINI_REPORT_DESC[] = {
    0x05, 0x01,             // Usage Page (Generic Desktop)
    0x09, 0x04,             // Usage (Joystick)
    0xA1, 0x01,             // Collection (Application)
    0x05, 0x09,             //   Usage Page (Button)
    0x19, 0x01,             //   Usage Minimum (1)
    0x29, 0x08,             //   Usage Maximum (8)
    0x15, 0x00,             //   Logical Minimum (0)
    0x25, 0x01,             //   Logical Maximum (1)
    0x75, 0x01,             //   Report Size (1)
    0x95, 0x08,             //   Report Count (8)
    0x81, 0x02,             //   Input (Data,Var,Abs)
    0x05, 0x01,             //   Usage Page (Generic Desktop)
    0x09, 0x30,             //   Usage (X)
    0x09, 0x31,             //   Usage (Y)
    0x15, 0x00,             //   Logical Minimum (0)
    0x26, 0xFF, 0x00,       //   Logical Maximum (255)
    0x75, 0x08,             //   Report Size (8)
    0x95, 0x02,             //   Report Count (2)
    0x81, 0x02,             //   Input (Data,Var,Abs)
    0xC0                    // End Collection
};

// Interface + HID class + one interrupt IN endpoint.
constexpr uint8_t MINI_FS_DESC[] = {
    9, 0x04, 0, 0,
    1,                       // bNumEndpoints
    0x03, 0x00, 0x00, 0,
    9, 0x21, 0x11, 0x01, 0x00, 1, 0x22,
    sizeof(MINI_REPORT_DESC) & 0xFF, sizeof(MINI_REPORT_DESC) >> 8,
    7, 0x05, 0x81, 0x03, 0x40, 0x00, 5
};
constexpr uint32_t MINI_FS_DESC_COUNT = 3;

// This tool stands up exactly one station, so one fixed serial is enough.
// Production derives a per-controller serial from its MAC (usb::ResolveGadgetId),
// because two stations sharing one iSerialNumber look like the same device
// coming and going to the host.
constexpr char MINI_SERIAL[] = "MINIGADGET001";

bool write_attr(const std::string& path, const std::string& value) {
    int fd = ::open(path.c_str(), O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "[mini] open %s: %s\n", path.c_str(), strerror(errno));
        return false;
    }
    ssize_t n = ::write(fd, value.data(), value.size());
    int err = errno;
    ::close(fd);
    if (n < 0) {
        fprintf(stderr, "[mini] write %s = %s: %s\n",
                path.c_str(), value.c_str(), strerror(err));
        return false;
    }
    return true;
}

// Mirrors FfsDevice::WriteFfsDescriptors() from the production driver.
bool write_descriptors(int ep0, const uint8_t* desc, size_t desc_len,
                       uint32_t desc_count) {
    struct {
        uint32_t magic, length, flags, fs_count;
    } header = {
        FUNCTIONFS_DESCRIPTORS_MAGIC_V2,
        static_cast<uint32_t>(sizeof(header) + desc_len),
        FUNCTIONFS_HAS_FS_DESC,
        desc_count,
    };

    uint8_t buf[sizeof(header) + 64];
    if (desc_len > sizeof(buf) - sizeof(header)) {
        fprintf(stderr, "[mini] descriptor block too large\n");
        return false;
    }
    memcpy(buf, &header, sizeof(header));
    memcpy(buf + sizeof(header), desc, desc_len);

    ssize_t n = ::write(ep0, buf, sizeof(header) + desc_len);
    if (n != static_cast<ssize_t>(sizeof(header) + desc_len)) {
        fprintf(stderr, "[mini] write descriptors: %s\n", strerror(errno));
        return false;
    }

    const char* strings[] = { core::device::kManufacturer, core::device::kProduct, MINI_SERIAL };
    struct {
        uint32_t magic, length, str_count, lang_count;
    } sheader = {
        FUNCTIONFS_STRINGS_MAGIC, 0, 3, 1,
    };
    uint16_t langid = 0x0409;

    size_t total = sizeof(sheader) + sizeof(langid);
    for (const char* s : strings) total += strlen(s) + 1;
    sheader.length = static_cast<uint32_t>(total);

    std::string out(total, '\0');
    memcpy(out.data(), &sheader, sizeof(sheader));
    size_t off = sizeof(sheader);
    memcpy(out.data() + off, &langid, sizeof(langid));
    off += sizeof(langid);
    for (const char* s : strings) {
        size_t len = strlen(s) + 1;
        memcpy(out.data() + off, s, len);
        off += len;
    }

    n = ::write(ep0, out.data(), out.size());
    if (n != static_cast<ssize_t>(out.size())) {
        fprintf(stderr, "[mini] write strings: %s\n", strerror(errno));
        return false;
    }
    fprintf(stderr, "[mini] descriptors (%zu) + strings (%zu) accepted\n",
            desc_len, out.size());
    return true;
}

// f_fs copies the setup packet into the event struct in host byte order.
uint16_t le16(__le16 v) { return static_cast<uint16_t>(v); }

void respond_in(int ep0, const uint8_t* data, size_t len, uint16_t w_length) {
    size_t n = len < w_length ? len : w_length;
    ssize_t written = ::write(ep0, data, n);
    if (written < 0)
        fprintf(stderr, "[mini] ep0 IN %zu: %s\n", n, strerror(errno));
    else
        fprintf(stderr, "[mini] ep0 answered IN: %zd byte(s)\n", written);
}

// FunctionFS has no "stall" call: handing the kernel a zero-length data phase
// makes it queue a STALL for the stage the host has not yet completed.
void stall(int ep0, bool data_in) {
    if (data_in) {
        uint8_t discard;
        ::read(ep0, &discard, 0);
    } else {
        ::write(ep0, nullptr, 0);
    }
    fprintf(stderr, "[mini] ep0 stalled %s phase\n", data_in ? "IN" : "OUT");
}

// Endpoint files are numbered by descriptor order ("ep1", "ep2") unless the
// FUNCTIONFS_VIRTUAL_ADDR flag is set. An endpoint whose file nobody has opened
// is never passed to usb_ep_enable(), so the host's interrupt IN URB would fail
// in usbhid_start() and the probe would be blamed on the wrong thing.
int open_ep(const std::string& path, const char* name, int flags) {
    std::string p = path + "/" + name;
    int fd = ::open(p.c_str(), flags);
    if (fd < 0)
        fprintf(stderr, "[mini] open %s: %s\n", p.c_str(), strerror(errno));
    else
        fprintf(stderr, "[mini] %s opened (fd %d)\n", p.c_str(), fd);
    return fd;
}

void handle_setup(int ep0, const usb_ctrlrequest& req, const uint8_t* fs_desc,
                  const uint8_t* report_desc, size_t report_len) {
    const uint8_t type = req.bRequestType & USB_TYPE_MASK;
    const uint8_t dir = req.bRequestType & USB_DIR_IN;
    const uint16_t w_value = le16(req.wValue);
    const uint16_t w_length = le16(req.wLength);

    fprintf(stderr, "[mini] SETUP type=%u req=0x%02x wValue=0x%04x wIndex=0x%04x wLength=%u\n",
            type >> 5, req.bRequest, w_value, le16(req.wIndex), w_length);

    if (type == USB_TYPE_STANDARD) {
        if (req.bRequest == USB_REQ_GET_DESCRIPTOR) {
            if (w_value >> 8 == core::device::kDescriptorTypeReport)
                respond_in(ep0, report_desc, report_len, w_length);
            else if (w_value >> 8 == core::device::kDescriptorTypeHid)
                respond_in(ep0, fs_desc + 9, 9, w_length);
            else
                stall(ep0, true);
            return;
        }
        // Alt-setting negotiation: accept the only setting we publish.
        if (req.bRequest == USB_REQ_SET_INTERFACE ||
            req.bRequest == USB_REQ_GET_INTERFACE) {
            if (dir) { uint8_t alt = 0; respond_in(ep0, &alt, 1, w_length); }
            else { uint8_t s; ::read(ep0, &s, 0); }
            return;
        }
        stall(ep0, dir != 0);
        return;
    }

    if (type == USB_TYPE_CLASS) {
        switch (req.bRequest) {
        case HID_REQ_GET_REPORT: {
            uint8_t zero[337] = {};
            respond_in(ep0, zero, w_length < sizeof(zero) ? w_length : sizeof(zero), w_length);
            return;
        }
        case HID_REQ_GET_IDLE:
        case HID_REQ_GET_PROTOCOL: {
            uint8_t zero = 0;
            respond_in(ep0, &zero, 1, w_length);
            return;
        }
        case HID_REQ_SET_REPORT:
            if (w_length > 0) {
                uint8_t data[128];
                ssize_t n = ::read(ep0, data, w_length < sizeof(data) ? w_length : sizeof(data));
                fprintf(stderr, "[mini] SET_REPORT data: %zd byte(s)\n", n);
            }
            [[fallthrough]];
        case HID_REQ_SET_IDLE:
        case HID_REQ_SET_PROTOCOL: {
            uint8_t status;
            ::read(ep0, &status, 0); // zero-length read completes the status stage
            fprintf(stderr, "[mini] ep0 class OUT acknowledged\n");
            return;
        }
        default:
            stall(ep0, dir != 0);
            return;
        }
    }

    stall(ep0, dir != 0);
}

void handle_event(int ep0, const usb_functionfs_event& ev, const uint8_t* fs_desc,
                  const uint8_t* report_desc, size_t report_len) {
    switch (ev.type) {
    case FUNCTIONFS_BIND:    fprintf(stderr, "[mini] event BIND\n"); break;
    case FUNCTIONFS_UNBIND:  fprintf(stderr, "[mini] event UNBIND\n"); break;
    case FUNCTIONFS_ENABLE:  fprintf(stderr, "[mini] event ENABLE (host set configuration)\n"); break;
    case FUNCTIONFS_DISABLE: fprintf(stderr, "[mini] event DISABLE\n"); break;
    case FUNCTIONFS_SUSPEND: fprintf(stderr, "[mini] event SUSPEND\n"); break;
    case FUNCTIONFS_RESUME:  fprintf(stderr, "[mini] event RESUME\n"); break;
    case FUNCTIONFS_SETUP:
        handle_setup(ep0, ev.u.setup, fs_desc, report_desc, report_len);
        break;
    default:                 fprintf(stderr, "[mini] event %d\n", ev.type); break;
    }
}

// The production pump: select() over ep0 and the OUT endpoint with a 10 ms
// tick, ep0 opened O_NONBLOCK. Everything else is identical to the blocking
// loop, so a difference in behaviour points straight at this pump.
void pump_select(int ep0, const uint8_t* fs_desc,
                 const uint8_t* report_desc, size_t report_len) {
    while (!g_stop) {
        fd_set readfds;
        FD_ZERO(&readfds);
        // Only ep0: it has ffs_ep0_poll. The endpoint files have no .poll, so
        // they report ready every pass and select() never sleeps.
        FD_SET(ep0, &readfds);

        timeval tv{0, 10000};
        int ret = ::select(ep0 + 1, &readfds, nullptr, nullptr, &tv);
        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("[mini] select");
            return;
        }
        if (ret == 0) continue;

        // The endpoint files have no .poll, so ep_out reads as ready on every
        // single pass; only log passes where ep0 actually has an event.
        if (!FD_ISSET(ep0, &readfds)) continue;
        fprintf(stderr, "[mini] select ready: ep0=1\n");

        if (FD_ISSET(ep0, &readfds)) {
            usb_functionfs_event ev;
            ssize_t n = ::read(ep0, &ev, sizeof(ev));
            if (n == static_cast<ssize_t>(sizeof(ev))) {
                handle_event(ep0, ev, fs_desc, report_desc, report_len);
            } else if (n < 0 && errno != EAGAIN) {
                fprintf(stderr, "[mini] ep0 read: %s\n", strerror(errno));
                return;
            } else if (n > 0) {
                fprintf(stderr, "[mini] short ep0 read: %zd\n", n);
            }
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    bool ds5_mode = false;
    bool nonblock = false;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--ds5")) ds5_mode = true;
        else if (!strcmp(argv[i], "--nonblock")) nonblock = true;
        else {
            fprintf(stderr, "usage: %s [--ds5] [--nonblock]\n", argv[0]);
            return 2;
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    // Same mount point the production driver defaults to; the probe builds a
    // single gadget, so it uses the fixed diagnostic instance name.
    const std::string path = "/dev/ffs-ds5";
    const std::string instance = kGadgetInstance;

    if (!GadgetSetup(path, instance, MINI_SERIAL)) {
        fprintf(stderr, "[mini] GadgetSetup failed\n");
        return 1;
    }

    if (!ds5_mode) {
        // Any Sony product id hid-playstation claims would steal the probe
        // away from hid-generic and add a driver we are not testing here.
        if (!write_attr(std::string(kConfigfsBase) + "/" + kGadgetInstance +
                            "/idProduct",
                        "0x9999")) {
            GadgetTeardown(path, instance);
            return 1;
        }
    }

    std::string ep0_path = path + "/ep0";
    int ep0 = ::open(ep0_path.c_str(), O_RDWR | (nonblock ? O_NONBLOCK : 0));
    if (ep0 < 0) {
        fprintf(stderr, "[mini] open %s: %s\n", ep0_path.c_str(), strerror(errno));
        GadgetTeardown(path, instance);
        return 1;
    }

    const bool ok = ds5_mode
        ? write_descriptors(ep0, core::device::kFfsFsDescriptors, sizeof(core::device::kFfsFsDescriptors), core::device::kFfsFsDescCount)
        : write_descriptors(ep0, MINI_FS_DESC, sizeof(MINI_FS_DESC), MINI_FS_DESC_COUNT);
    if (!ok) {
        close(ep0);
        GadgetTeardown(path, instance);
        return 1;
    }

    int ep_in = open_ep(path, "ep1", O_WRONLY);
    if (ep_in < 0) {
        close(ep0);
        GadgetTeardown(path, instance);
        return 1;
    }
    int ep_out = ds5_mode ? open_ep(path, "ep2", O_RDONLY) : -1;
    if (ds5_mode && ep_out < 0) {
        close(ep_in);
        close(ep0);
        GadgetTeardown(path, instance);
        return 1;
    }

    if (!GadgetBind(instance)) {
        close(ep_in);
        if (ep_out >= 0) close(ep_out);
        close(ep0);
        GadgetTeardown(path, instance);
        return 1;
    }

    const uint8_t* fs_desc = ds5_mode ? core::device::kFfsFsDescriptors : MINI_FS_DESC;
    const uint8_t* report_desc = ds5_mode ? core::device::kHidReportDescriptor : MINI_REPORT_DESC;
    const size_t report_len = ds5_mode ? sizeof(core::device::kHidReportDescriptor)
                                       : sizeof(MINI_REPORT_DESC);
    fprintf(stderr, "[mini] bound, %s mode (%zu-byte report descriptor), ep0 %sblocking\n",
            ds5_mode ? "DS5 descriptors" : "minimal descriptors", report_len,
            nonblock ? "non-" : "");

    while (!g_stop) {
        if (nonblock) { pump_select(ep0, fs_desc, report_desc, report_len); break; }
        usb_functionfs_event ev;
        ssize_t n = ::read(ep0, &ev, sizeof(ev));
        if (n == static_cast<ssize_t>(sizeof(ev))) {
            handle_event(ep0, ev, fs_desc, report_desc, report_len);
        } else if (n == 0) {
            fprintf(stderr, "[mini] ep0 reached EOF\n");
            break;
        } else {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) { usleep(2000); continue; }
            fprintf(stderr, "[mini] ep0 read: %s\n", strerror(errno));
            break;
        }
    }

    close(ep_in);
    if (ep_out >= 0) close(ep_out);
    close(ep0);
    GadgetTeardown(path, instance);
    fprintf(stderr, "[mini] done\n");
    return 0;
}
