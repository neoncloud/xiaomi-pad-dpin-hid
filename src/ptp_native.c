#define _GNU_SOURCE

#include <android/log.h>
#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <link.h>
#include <linux/input.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/auxv.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/system_properties.h>
#include <time.h>
#include <unistd.h>

#include "shadowhook.h"
#include "ptp_native_generated.h"
#include "digitizer_reports.h"

#define PTP_TAG "CodexPtpNative"
#define PTP_CONTROL_KEY "debug.codex.ptp.control"
#define DIGITIZER_PALM_TEST_KEY "debug.codex.ptp.palm_test"
#define PTP_TAG_PREFIX "CodexPTPModule"
#define PTP_GADGET_GET_REPORT_ID 0x80016741u
#define PTP_GADGET_WRITE_GET_REPORT 0x40486742u
#define PTP_MAX_CONTACTS 5
#define PTP_INPUT_SLOTS 16

struct ptp_hidg_report {
    uint8_t report_id;
    uint8_t userspace_req;
    uint16_t length;
    uint8_t data[64];
    uint8_t padding[4];
};

typedef struct {
    int tracking;
    int wire;
    int x;
    int y;
} ptp_contact;

enum ptp_slot_admission {
    PTP_SLOT_NEW,
    PTP_SLOT_ADMITTED,
    PTP_SLOT_OVERFLOW,
};

typedef struct {
    int id;
    int x;
    int y;
    int valid;
    enum ptp_slot_admission admission;
} ptp_slot;

static uintptr_t g_base;
static int g_hidg0 = -1;
static int g_hidg1 = -1;
static int g_touchpad_fd = -1;
static int g_null_fd = -1;
static int g_ready;
static int g_descriptor_patched;
static int g_input_active;
static int g_disarm;
static int g_stop;
static int g_gate_active;
static int g_release_sent;
static int g_mode;
static int g_selective = 3;
static int g_segment;
static int g_button;
static int g_slot = 0;
static int g_active_count;
static int g_reports;
static int g_raw_contact_peak;
static int g_sent_contact_peak;
static int g_write_errors;
static int g_write_error_streak;
static int g_suppressed;
static int g_shortcut_suppressed;
static void *g_send_shortcut_original;
static int g_generation = 1;
static int64_t g_release_deadline_ms;
static int64_t g_last_us;
static int g_last_feature_id = -1;
static int64_t g_last_feature_ms;
static int64_t g_last_write_log_ms;
static int g_stage;
static int g_fail_site;
static int g_log_head;
static int g_log_tail;
static char g_log_ring[64][256];
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;
static ptp_slot g_slots[PTP_INPUT_SLOTS];
static ptp_contact g_active[PTP_MAX_CONTACTS];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static __thread int t_touchpad_depth;
static __thread int t_touchpad_read_fd = -1;
static __thread void *t_touchpad_read_buf;
static __thread uint64_t t_touchpad_read_count;
static __thread int t_hid_read_fd = -1;
static __thread void *t_hid_read_buf;
static __thread uint64_t t_hid_read_count;
static __thread int t_hid_read_generation;
static __thread int t_open0_valid;
static __thread int t_open1_valid;

typedef struct {
    int fd;
    int pressure;
    int pressure_max;
    int twist;
    int64_t twist_read_us;
    bool twist_axis;
    bool twist_events;
    bool in_range;
    bool tip;
    bool barrel;
    bool rubber;
} digitizer_pen_input;

typedef struct {
    uint16_t code;
    bool down;
} digitizer_action;

enum { DIGITIZER_ACTION_CAPACITY = 8 };

static bool g_digitizer_enhanced;
static int g_digitizer_touch_fd = -1;
static unsigned int g_digitizer_touch_slot;
static int g_digitizer_touch_major[DIGITIZER_SLOTS];
static unsigned int g_digitizer_onset_pending;
static unsigned int g_digitizer_onset_low_confidence;
static digitizer_contact g_digitizer_contacts[DIGITIZER_SLOTS];
static digitizer_contact g_digitizer_draft[DIGITIZER_SLOTS];
static digitizer_pen_input g_digitizer_pens[2];
static int g_digitizer_host_pen = -1;
static uint8_t g_digitizer_last_pen[DIGITIZER_PEN_BYTES];
static digitizer_action g_digitizer_actions[DIGITIZER_ACTION_CAPACITY];
static unsigned int g_digitizer_action_head;
static unsigned int g_digitizer_action_count;
static uint8_t g_digitizer_aux_buttons;
static uint8_t g_digitizer_swipe_key;
static bool g_digitizer_release_actions;
static int g_digitizer_aux_fd = -1;
static int g_digitizer_pen_route;
static __thread uint8_t t_digitizer_report[DIGITIZER_TOUCH_BYTES];
static __thread const struct input_event *t_digitizer_event;
static __thread int t_digitizer_read_fd;
static __thread int t_digitizer_pen = -1;
static __thread int t_digitizer_pen_rotation;
static __thread bool t_digitizer_pen_syn;
static __thread bool t_digitizer_pen_handled;
static __thread bool t_digitizer_touch_pending;
static __thread bool t_digitizer_pen_pending;

static void *g_intercept_stubs[PTP_INTERCEPT_COUNT];
static struct dl_phdr_info g_main_info;
static int g_main_info_valid;
static uintptr_t g_at_phdr;

#define PTP_LOAD(x) __atomic_load_n(&(x), __ATOMIC_ACQUIRE)
#define PTP_STORE(x, v) __atomic_store_n(&(x), (v), __ATOMIC_RELEASE)
#define PTP_ADD(x, v) __atomic_add_fetch(&(x), (v), __ATOMIC_RELAXED)

static void ptp_intercept_callback(shadowhook_cpu_context_t *, void *);
static void ptp_mark(const char *name);

static void ptp_log(const char *fmt, ...) {
    va_list ap;
    char buffer[512];
    if (pthread_mutex_trylock(&g_log_lock) != 0) return;
    va_start(ap, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, ap);
    va_end(ap);
    int next = (g_log_head + 1) % 64;
    if (next != g_log_tail) {
        strncpy(g_log_ring[g_log_head], buffer, sizeof(g_log_ring[0]) - 1);
        g_log_ring[g_log_head][sizeof(g_log_ring[0]) - 1] = '\0';
        g_log_head = next;
    }
    pthread_mutex_unlock(&g_log_lock);
}

static void *ptp_logger_thread(void *unused) {
    (void)unused;
    ptp_mark("ptp-logger");
    char buffer[256];
    for (;;) {
        int have = 0;
        pthread_mutex_lock(&g_log_lock);
        if (g_log_tail != g_log_head) {
            memcpy(buffer, g_log_ring[g_log_tail], sizeof(buffer));
            g_log_tail = (g_log_tail + 1) % 64;
            have = 1;
        }
        pthread_mutex_unlock(&g_log_lock);
        if (have) __android_log_print(ANDROID_LOG_INFO, PTP_TAG, "%s", buffer);
        else usleep(100000);
    }
    return NULL;
}

static void ptp_mark(const char *name) {
    prctl(PR_SET_NAME, name, 0, 0, 0);
}

static int64_t ptp_now_us(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return g_last_us + 100;
    return (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

static int64_t ptp_now_ms(void) {
    return ptp_now_us() / 1000;
}

static int ptp_verify_manifest(void) {
    if (!g_base) return -1;
    for (size_t i = 0; i < PTP_FUNCTION_COUNT; i++) {
        const PtpFuncSpec *spec = &k_functions[i];
        if (memcmp((const void *)(g_base + spec->va), spec->bytes, 32) != 0) {
            ptp_log("manifest function mismatch: %s va=%x", spec->name, spec->va);
            return -1;
        }
    }
    for (size_t i = 0; i < PTP_INTERCEPT_COUNT; i++) {
        if (memcmp((const void *)(g_base + k_intercept_sites[i].va),
                   k_intercept_sites[i].bytes, 4) != 0) {
            ptp_log("manifest intercept mismatch site=%x kind=%u",
                    k_intercept_sites[i].va, k_intercept_sites[i].kind);
            return -1;
        }
    }
    return 0;
}

static int ptp_main_info_callback(struct dl_phdr_info *info, size_t size, void *data) {
    (void)size;
    (void)data;
    if ((g_at_phdr && (uintptr_t)info->dlpi_phdr == g_at_phdr) ||
        (info->dlpi_name == NULL || info->dlpi_name[0] == '\0')) {
        g_main_info = *info;
        g_main_info_valid = 1;
        return 1;
    }
    return 0;
}

static int ptp_find_main(void) {
    g_main_info_valid = 0;
    g_at_phdr = (uintptr_t)getauxval(AT_PHDR);
    dl_iterate_phdr(ptp_main_info_callback, NULL);
    if (!g_main_info_valid && g_at_phdr) {
        /* Some vendor binaries expose their pathname instead of an empty name. */
        dl_iterate_phdr(ptp_main_info_callback, NULL);
    }
    if (!g_main_info_valid) return -1;
    g_base = (uintptr_t)g_main_info.dlpi_addr;
    return g_base ? 0 : -1;
}

static void ptp_uninstall_intercepts(void) {
    for (size_t i = 0; i < PTP_INTERCEPT_COUNT; i++) {
        if (!g_intercept_stubs[i]) continue;
        int rc = shadowhook_unintercept(g_intercept_stubs[i]);
        if (rc != 0)
            ptp_log("unintercept failed site=%x errno=%d",
                    k_intercept_sites[i].va, shadowhook_get_errno());
        g_intercept_stubs[i] = NULL;
    }
}

static int ptp_install_intercepts(void) {
    for (size_t i = 0; i < PTP_INTERCEPT_COUNT; i++) {
        const PtpInterceptSpec *site = &k_intercept_sites[i];
        PTP_STORE(g_stage, 6);
        PTP_STORE(g_fail_site, (int)site->va);
        g_intercept_stubs[i] = shadowhook_intercept_instr_addr(
            (void *)(g_base + site->va),
            ptp_intercept_callback,
            (void *)site,
            SHADOWHOOK_INTERCEPT_DEFAULT);
        if (!g_intercept_stubs[i]) {
            ptp_log("intercept failed site=%x kind=%u errno=%d",
                    site->va, site->kind, shadowhook_get_errno());
            ptp_uninstall_intercepts();
            return -1;
        }
    }
    PTP_STORE(g_fail_site, 0);
    return 0;
}

static void ptp_reset_state(const char *reason) {
    pthread_mutex_lock(&g_lock);
    PTP_STORE(g_input_active, 0);
    PTP_STORE(g_disarm, 0);
    PTP_STORE(g_gate_active, 0);
    PTP_STORE(g_release_sent, 0);
    PTP_STORE(g_write_error_streak, 0);
    PTP_STORE(g_touchpad_fd, -1);
    PTP_STORE(g_mode, 0);
    PTP_STORE(g_selective, 3);
    PTP_STORE(g_segment, 0);
    g_button = 0;
    g_slot = 0;
    g_active_count = 0;
    PTP_STORE(g_raw_contact_peak, 0);
    PTP_STORE(g_sent_contact_peak, 0);
    memset(g_slots, 0, sizeof(g_slots));
    memset(g_active, 0, sizeof(g_active));
    PTP_ADD(g_generation, 1);
    pthread_mutex_unlock(&g_lock);
    ptp_log("reset generation=%d reason=%s", PTP_LOAD(g_generation), reason);
}

static int ptp_all_up_locked(void) {
    for (size_t i = 0; i < PTP_INPUT_SLOTS; i++)
        if (g_slots[i].valid) return 0;
    return 1;
}

static int ptp_scale(int value, int maximum, int *out) {
    if (value < 0 || value > maximum + 1) return -1;
    if (value > maximum) value = maximum;
    *out = (value * 4095 + maximum / 2) / maximum;
    return 0;
}

static int ptp_write_report(const uint8_t report[30]) {
    int hidg0 = PTP_LOAD(g_hidg0);
    if (hidg0 < 0) {
        PTP_ADD(g_write_errors, 1);
        PTP_ADD(g_write_error_streak, 1);
        return -1;
    }
    ssize_t n = -1;
    int last_errno = 0;
    for (int attempt = 0; attempt < 20; attempt++) {
        n = syscall(SYS_write, hidg0, report, 30);
        if (n >= 0) {
            // Each write is a separate HID report. A suffix cannot complete it.
            last_errno = n == 30 ? 0 : EIO;
            break;
        }
        last_errno = errno;
        if (last_errno == EINTR) continue;
        if (last_errno != EAGAIN && last_errno != EWOULDBLOCK &&
            last_errno != ENOBUFS)
            break;
        usleep(500);
    }
    if (n != 30) {
        PTP_ADD(g_write_errors, 1);
        int streak = PTP_ADD(g_write_error_streak, 1);
        int64_t now = ptp_now_ms();
        if (streak == 1 || now - PTP_LOAD(g_last_write_log_ms) >= 250) {
            PTP_STORE(g_last_write_log_ms, now);
            ptp_log("PTP write failed n=%ld errno=%d streak=%d",
                    (long)n, last_errno, streak);
        }
        return -1;
    }
    PTP_STORE(g_write_error_streak, 0);
    PTP_ADD(g_reports, 1);
    return 0;
}

static int ptp_publish_frame(const ptp_contact *next, int next_count,
                             const ptp_contact *removed, int removed_count,
                             int button, int64_t t_us) {
    uint8_t report[30] = {0};
    report[0] = 16;
    int count = next_count + removed_count;
    for (int i = 0; i < count; i++) {
        int down = i < next_count;
        const ptp_contact *c = down ? &next[i] : &removed[i - next_count];
        int off = 1 + i * 5;
        report[off] = (uint8_t)(1 | (down << 1) | (c->wire << 2));
        report[off + 1] = c->x & 0xff;
        report[off + 2] = c->x >> 8;
        report[off + 3] = c->y & 0xff;
        report[off + 4] = c->y >> 8;
    }
    uint16_t scan = (uint16_t)((t_us / 100) & 0xffff);
    report[26] = scan & 0xff;
    report[27] = scan >> 8;
    report[28] = (uint8_t)count;
    report[29] = (uint8_t)(button & 1);
    if (ptp_write_report(report) != 0) return -1;
    if (next_count) memcpy(g_active, next, (size_t)next_count * sizeof(*next));
    g_active_count = next_count;
    if (next_count > PTP_LOAD(g_sent_contact_peak))
        PTP_STORE(g_sent_contact_peak, next_count);
    return 0;
}

static void ptp_admit_contacts(void) {
    int raw = 0;
    int admitted = 0;
    for (int i = 0; i < PTP_INPUT_SLOTS; i++) {
        if (!g_slots[i].valid) continue;
        raw++;
        admitted += g_slots[i].admission == PTP_SLOT_ADMITTED;
    }
    if (raw > PTP_LOAD(g_raw_contact_peak)) PTP_STORE(g_raw_contact_peak, raw);
    for (int i = 0; i < PTP_INPUT_SLOTS; i++) {
        ptp_slot *s = &g_slots[i];
        if (!s->valid || s->admission != PTP_SLOT_NEW) continue;
        s->admission = admitted < PTP_MAX_CONTACTS ? PTP_SLOT_ADMITTED : PTP_SLOT_OVERFLOW;
        admitted += s->admission == PTP_SLOT_ADMITTED;
    }
}

static int ptp_send_snapshot(int button, int64_t t_us) {
    ptp_contact next[PTP_MAX_CONTACTS];
    ptp_contact retained[PTP_MAX_CONTACTS];
    ptp_contact removed[PTP_MAX_CONTACTS];
    int next_count = 0, retained_count = 0, removed_count = 0;
    unsigned matched = 0, used = 0;

    for (int i = 0; i < PTP_INPUT_SLOTS; i++) {
        const ptp_slot *s = &g_slots[i];
        if (!s->valid || s->admission != PTP_SLOT_ADMITTED) continue;
        if (next_count == PTP_MAX_CONTACTS) return -1;
        ptp_contact c = {.tracking = s->id, .wire = -1};
        if (ptp_scale(s->x, 3683, &c.x) != 0 || ptp_scale(s->y, 2175, &c.y) != 0)
            return -1;
        for (int j = 0; j < g_active_count; j++) {
            if (g_active[j].tracking != c.tracking) continue;
            if (matched & (1u << j)) return -1;
            matched |= 1u << j;
            c.wire = g_active[j].wire;
            retained[retained_count++] = c;
            break;
        }
        next[next_count++] = c;
    }
    for (int i = 0; i < g_active_count; i++) {
        used |= 1u << g_active[i].wire;
        if (!(matched & (1u << i))) removed[removed_count++] = g_active[i];
    }
    if (next_count + removed_count > PTP_MAX_CONTACTS) {
        // Commit the lifts before reusing any released ID in a second report.
        if (ptp_publish_frame(retained, retained_count, removed, removed_count, button, t_us) != 0)
            return -1;
        for (int i = 0; i < removed_count; i++) used &= ~(1u << removed[i].wire);
        removed_count = 0;
        t_us += 100;
    }
    for (int i = 0; i < next_count; i++) {
        if (next[i].wire >= 0) continue;
        for (int wire = 0; wire < PTP_MAX_CONTACTS; wire++) {
            if (used & (1u << wire)) continue;
            next[i].wire = wire;
            used |= 1u << wire;
            break;
        }
        if (next[i].wire < 0) return -1;
    }
    return ptp_publish_frame(next, next_count, removed, removed_count, button, t_us);
}

static void ptp_finish_input(const char *reason) {
    pthread_mutex_lock(&g_lock);
    PTP_STORE(g_input_active, 0);
    PTP_STORE(g_disarm, 0);
    PTP_STORE(g_gate_active, 0);
    PTP_STORE(g_release_sent, 0);
    PTP_STORE(g_write_error_streak, 0);
    g_active_count = 0;
    g_release_deadline_ms = 0;
    pthread_mutex_unlock(&g_lock);
    ptp_log("PTP session ended reason=%s reports=%d errors=%d suppressed=%d",
            reason, PTP_LOAD(g_reports), PTP_LOAD(g_write_errors),
            PTP_LOAD(g_suppressed));
}

static void ptp_request_disarm(const char *reason) {
    if (!PTP_LOAD(g_input_active)) return;
    if (!PTP_LOAD(g_disarm)) {
        pthread_mutex_lock(&g_lock);
        PTP_STORE(g_disarm, 1);
        g_release_deadline_ms = ptp_now_ms() + 1500;
        pthread_mutex_unlock(&g_lock);
        ptp_log("PTP disarm reason=%s", reason);
    }
}

static int ptp_send_empty_release(int64_t t_us) {
    return ptp_publish_frame(NULL, 0, g_active, g_active_count, 0, t_us);
}

static void ptp_control_poll(void) {
    char value[128] = {0};
    __system_property_get(PTP_CONTROL_KEY, value);
    if (strstr(value, ":run") != NULL) {
        if (PTP_LOAD(g_stop)) ptp_log("control run");
        PTP_STORE(g_stop, 0);
    }
    if (strstr(value, ":stop") != NULL) {
        if (!PTP_LOAD(g_stop)) ptp_log("control stop");
        PTP_STORE(g_stop, 1);
        ptp_request_disarm("module_stop");
    }
}

static void ptp_best_effort_release(const char *reason) {
    if (PTP_LOAD(g_input_active) && !PTP_LOAD(g_release_sent)) {
        pthread_mutex_lock(&g_lock);
        if (PTP_LOAD(g_input_active) && !PTP_LOAD(g_release_sent)) {
            int rc = ptp_send_empty_release(ptp_now_us());
            PTP_STORE(g_release_sent, rc == 0);
            ptp_log("best-effort release reason=%s rc=%d", reason, rc);
        }
        pthread_mutex_unlock(&g_lock);
    }
    ptp_reset_state(reason);
}

static void ptp_parse_feature_set(const uint8_t *raw, size_t n) {
    int id = 0;
    const uint8_t *p = raw;
    size_t plen = n;
    if (n == 9 && raw[0] == 20) { id = 20; p = raw + 1; plen = 8; }
    else if (n == 8) { id = 20; }
    else if (n == 13 && raw[0] == 21) { id = 21; p = raw + 1; plen = 12; }
    else if (n == 12) { id = 21; }
    else if (n == 34 && raw[0] == 18) { id = 18; p = raw + 1; plen = 33; }
    else if (n == 33) { id = 18; }
    if (!id || plen < 1) return;
    if (id == 20 && (p[0] == 0 || p[0] == 3)) {
        for (size_t i = 1; i < plen; i++) if (p[i] != 0) return;
        PTP_STORE(g_mode, p[0]);
        if (PTP_LOAD(g_mode) != 3) ptp_request_disarm("mode_exit");
        ptp_log("mode=%d", PTP_LOAD(g_mode));
    } else if (id == 21 && p[0] <= 3) {
        for (size_t i = 1; i < plen; i++) if (p[i] != 0) return;
        PTP_STORE(g_selective, p[0]);
        if ((PTP_LOAD(g_selective) & 1) == 0)
            ptp_request_disarm("surface_disabled");
        ptp_log("selective=%d", PTP_LOAD(g_selective));
    } else if (id == 18 && p[0] <= 7) {
        PTP_STORE(g_segment, p[0]);
        ptp_log("segment=%d", PTP_LOAD(g_segment));
    }
}

static void ptp_feature_reply(int id) {
    struct ptp_hidg_report report;
    memset(&report, 0, sizeof(report));
    report.report_id = (uint8_t)id;
    report.userspace_req = id == 17 ? 0 : 1;
    if (id == 17) {
        report.length = 2;
        report.data[0] = 17;
        report.data[1] = PTP_MAX_CONTACTS;
    } else if (id == 18) {
        report.length = 34;
        report.data[0] = 18;
        report.data[1] = (uint8_t)PTP_LOAD(g_segment);
        memcpy(&report.data[2], k_ptphqa + PTP_LOAD(g_segment) * 32, 32);
    } else if (id == 20) {
        report.length = 9;
        report.data[0] = 20;
        report.data[1] = (uint8_t)PTP_LOAD(g_mode);
    } else if (id == 21) {
        report.length = 13;
        report.data[0] = 21;
        report.data[1] = (uint8_t)PTP_LOAD(g_selective);
    } else {
        return;
    }
    int hidg0 = PTP_LOAD(g_hidg0);
    if (hidg0 < 0) return;
    if (syscall(SYS_ioctl, hidg0, PTP_GADGET_WRITE_GET_REPORT, &report) != 0)
        ptp_log("feature reply failed id=%d errno=%d", id, errno);
}

static void ptp_feature_poll(void) {
    if (PTP_LOAD(g_stop)) return;
    int hidg0 = PTP_LOAD(g_hidg0);
    if (hidg0 < 0) return;
    uint8_t id = 0;
    if (syscall(SYS_ioctl, hidg0, PTP_GADGET_GET_REPORT_ID, &id) != 0) return;
    if (id != 17 && id != 18 && id != 20 && id != 21) return;
    int64_t now = ptp_now_ms();
    if (id == PTP_LOAD(g_last_feature_id) &&
        now - PTP_LOAD(g_last_feature_ms) < 250)
        return;
    PTP_STORE(g_last_feature_id, id);
    PTP_STORE(g_last_feature_ms, now);
    ptp_feature_reply(id);
}

static int ptp_check_all_up(int fd) {
    struct input_absinfo slot_info;
    if (syscall(SYS_ioctl, fd, EVIOCGABS(ABS_MT_SLOT), &slot_info) != 0)
        return 0;
    int max_slot = slot_info.maximum;
    if (max_slot < 0 || max_slot > 15) return 0;
    uint8_t slots[4 + 16 * 4];
    memset(slots, 0, sizeof(slots));
    int32_t code = ABS_MT_TRACKING_ID;
    memcpy(slots, &code, sizeof(code));
    unsigned long request = _IOC(_IOC_READ, 'E', 0x0a, 4 + (max_slot + 1) * 4);
    if (syscall(SYS_ioctl, fd, request, slots) != 0) return 0;
    for (int i = 0; i <= max_slot; i++) {
        int32_t value;
        memcpy(&value, slots + 4 + i * 4, sizeof(value));
        if (value != -1) return 0;
    }
    int slot = slot_info.value;
    if (slot < 0 || slot > max_slot) return 0;
    PTP_STORE(g_slot, slot);
    return 1;
}

static void ptp_owner_tick(int fd) {
    ptp_control_poll();

    if (PTP_LOAD(g_input_active) &&
        (PTP_LOAD(g_mode) != 3 || (PTP_LOAD(g_selective) & 1) == 0 ||
         PTP_LOAD(g_stop)))
        ptp_request_disarm("state_exit");

    if (!PTP_LOAD(g_input_active) && !PTP_LOAD(g_disarm) &&
        !PTP_LOAD(g_stop) && PTP_LOAD(g_mode) == 3 &&
        (PTP_LOAD(g_selective) & 1) != 0 && ptp_check_all_up(fd)) {
        int armed = 0;
        pthread_mutex_lock(&g_lock);
        if (!PTP_LOAD(g_input_active) && !PTP_LOAD(g_disarm) &&
            !PTP_LOAD(g_stop)) {
            memset(g_slots, 0, sizeof(g_slots));
            g_active_count = 0;
            PTP_STORE(g_input_active, 1);
            PTP_STORE(g_gate_active, 1);
            PTP_STORE(g_release_sent, 0);
            armed = 1;
        }
        pthread_mutex_unlock(&g_lock);
        if (armed) ptp_log("PTP auto armed generation=%d fd=%d",
                           PTP_LOAD(g_generation), fd);
    }

    if (PTP_LOAD(g_input_active) && PTP_LOAD(g_disarm)) {
        if (!PTP_LOAD(g_release_sent)) {
            pthread_mutex_lock(&g_lock);
            PTP_STORE(g_release_sent, ptp_send_empty_release(ptp_now_us()) == 0);
            pthread_mutex_unlock(&g_lock);
        }
        int all_up = 0;
        int64_t deadline = 0;
        pthread_mutex_lock(&g_lock);
        all_up = ptp_all_up_locked();
        deadline = g_release_deadline_ms;
        pthread_mutex_unlock(&g_lock);
        if (PTP_LOAD(g_release_sent) && all_up)
            ptp_finish_input("release_complete");
        else if (deadline && ptp_now_ms() >= deadline)
            ptp_finish_input("release_timeout");
    }

    ptp_feature_poll();
}

static void ptp_observe_event(const uint8_t *raw) {
    const struct input_event *ev = (const struct input_event *)raw;
    if (ev->type == EV_SYN && ev->code == SYN_DROPPED) {
        ptp_request_disarm("syn_dropped");
        return;
    }
    int send_failed = 0;
    int release_complete = 0;
    pthread_mutex_lock(&g_lock);
    if (ev->type == EV_ABS) {
        int slot = PTP_LOAD(g_slot);
        if (ev->code == ABS_MT_SLOT && ev->value >= 0 && ev->value < PTP_INPUT_SLOTS) {
            slot = ev->value;
            PTP_STORE(g_slot, slot);
        }
        ptp_slot *s = &g_slots[slot];
        if (ev->code == ABS_MT_TRACKING_ID) {
            if (!s->valid || s->id != ev->value) s->admission = PTP_SLOT_NEW;
            s->id = ev->value;
            s->valid = ev->value >= 0;
        } else if (ev->code == ABS_MT_POSITION_X) {
            s->x = ev->value;
        } else if (ev->code == ABS_MT_POSITION_Y) {
            s->y = ev->value;
        }
    } else if (ev->type == EV_KEY && ev->code == BTN_LEFT) {
        g_button = ev->value != 0;
    } else if (ev->type == EV_SYN && ev->code == SYN_REPORT) {
        int64_t t_us = (int64_t)ev->time.tv_sec * 1000000LL + ev->time.tv_usec;
        PTP_STORE(g_last_us, t_us);
        if (PTP_LOAD(g_input_active) && !PTP_LOAD(g_disarm)) {
            ptp_admit_contacts();
            int rc = ptp_send_snapshot(g_button, t_us);
            if (rc != 0) send_failed = 1;
        }
        if (PTP_LOAD(g_input_active) && PTP_LOAD(g_disarm) &&
            PTP_LOAD(g_release_sent) && ptp_all_up_locked())
            release_complete = 1;
    }
    pthread_mutex_unlock(&g_lock);
    if (send_failed && PTP_LOAD(g_write_error_streak) >= 3)
        ptp_request_disarm("write_error");
    if (release_complete) ptp_finish_input("release_complete");
}

static void ptp_hid_read_enter(shadowhook_cpu_context_t *ctx) {
    t_hid_read_fd = (int)ctx->regs[0];
    t_hid_read_buf = (void *)(uintptr_t)ctx->regs[1];
    t_hid_read_count = ctx->regs[2];
    t_hid_read_generation = PTP_LOAD(g_generation);
}

static void ptp_hid_read_leave(shadowhook_cpu_context_t *ctx) {
    ssize_t n = (ssize_t)ctx->regs[0];
    int fd = t_hid_read_fd;
    if (t_hid_read_generation == PTP_LOAD(g_generation) && fd >= 0 &&
        n > 0 && n <= 64 && t_hid_read_buf &&
        (fd == PTP_LOAD(g_hidg0) || fd == PTP_LOAD(g_hidg1))) {
        ptp_parse_feature_set((const uint8_t *)t_hid_read_buf, (size_t)n);
        ptp_feature_poll();
    }
    t_hid_read_fd = -1;
    t_hid_read_buf = NULL;
    t_hid_read_count = 0;
    t_hid_read_generation = 0;
}

static void ptp_touchpad_read_enter(shadowhook_cpu_context_t *ctx) {
    t_touchpad_read_fd = (int)ctx->regs[0];
    t_touchpad_read_buf = (void *)(uintptr_t)ctx->regs[1];
    t_touchpad_read_count = ctx->regs[2];
}

static void ptp_touchpad_read_leave(shadowhook_cpu_context_t *ctx) {
    ssize_t n = (ssize_t)ctx->regs[0];
    if (n == 24 && t_touchpad_depth > 0 && t_touchpad_read_buf &&
        t_touchpad_read_fd == PTP_LOAD(g_touchpad_fd))
        ptp_observe_event((const uint8_t *)t_touchpad_read_buf);
    t_touchpad_read_fd = -1;
    t_touchpad_read_buf = NULL;
    t_touchpad_read_count = 0;
}

static int ptp_gate_stock_output(int fd) {
    return PTP_LOAD(g_ready) && PTP_LOAD(g_gate_active) &&
           PTP_LOAD(g_input_active) && t_touchpad_depth > 0 && fd >= 0 &&
           (fd == PTP_LOAD(g_hidg0) || fd == PTP_LOAD(g_hidg1));
}

static int64_t ptp_send_shortcut(int fd, char modifier, char key) {
    if (ptp_gate_stock_output(fd)) {
        PTP_ADD(g_shortcut_suppressed, 1);
        return 0;
    }
    int64_t (*original)(int, char, char) =
        __atomic_load_n(&g_send_shortcut_original, __ATOMIC_ACQUIRE);
    return original(fd, modifier, key);
}

static void digitizer_reset(void) {
    g_digitizer_enhanced = false;
    g_digitizer_touch_fd = -1;
    g_digitizer_touch_slot = 0;
    g_digitizer_host_pen = -1;
    memset(g_digitizer_last_pen, 0, sizeof(g_digitizer_last_pen));
    g_digitizer_action_head = g_digitizer_action_count = 0;
    g_digitizer_aux_buttons = g_digitizer_swipe_key = 0;
    g_digitizer_release_actions = false;
    g_digitizer_aux_fd = -1;
    PTP_STORE(g_digitizer_pen_route, 0);
    memset(g_digitizer_contacts, 0, sizeof(g_digitizer_contacts));
    memset(g_digitizer_touch_major, 0, sizeof(g_digitizer_touch_major));
    g_digitizer_onset_pending = 0;
    g_digitizer_onset_low_confidence = 0;
    memset(g_digitizer_pens, 0, sizeof(g_digitizer_pens));
    g_digitizer_pens[0].fd = g_digitizer_pens[1].fd = -1;
    t_digitizer_pen = -1;
    t_digitizer_event = NULL;
    t_digitizer_touch_pending = t_digitizer_pen_pending = false;
}

static void digitizer_screen_read(const struct input_event *event, int fd) {
    if (fd != g_digitizer_touch_fd || (event->type == EV_SYN && event->code == SYN_DROPPED)) {
        struct input_absinfo info = {0};
        int32_t width[DIGITIZER_SLOTS + 1] = {ABS_MT_TOUCH_MAJOR};
        if (ioctl(fd, EVIOCGABS(ABS_MT_SLOT), &info) == 0 &&
            info.value >= 0 && info.value < DIGITIZER_SLOTS)
            g_digitizer_touch_slot = (unsigned int)info.value;
        if (ioctl(fd, EVIOCGMTSLOTS(sizeof(width)), width) == 0)
            memcpy(g_digitizer_touch_major, width + 1, sizeof(g_digitizer_touch_major));
        g_digitizer_onset_pending = 0;
        g_digitizer_onset_low_confidence = 0;
        g_digitizer_touch_fd = fd;
    }
    if (event->type == EV_ABS && event->code == ABS_MT_SLOT &&
        (unsigned int)event->value < DIGITIZER_SLOTS)
        g_digitizer_touch_slot = (unsigned int)event->value;
    if (event->type == EV_ABS && event->code == ABS_MT_TOUCH_MAJOR)
        g_digitizer_touch_major[g_digitizer_touch_slot] = event->value;
    if (event->type == EV_ABS && event->code == ABS_MT_TRACKING_ID) {
        unsigned int bit = 1u << g_digitizer_touch_slot;
        g_digitizer_onset_low_confidence &= ~bit;
        g_digitizer_onset_pending &= ~bit;
        if (event->value >= 0) g_digitizer_onset_pending |= bit;
    }
    if (event->type == EV_SYN && event->code == SYN_REPORT && g_digitizer_onset_pending) {
        char option[PROP_VALUE_MAX] = {0};
        __system_property_get(DIGITIZER_PALM_TEST_KEY, option);
        int threshold = 0;
        if (option[0] == '\0' || strcmp(option, "21") == 0) threshold = 21;
        if (strcmp(option, "22") == 0) threshold = 22;
        if (threshold) {
            for (unsigned int slot = 0; slot < DIGITIZER_SLOTS; slot++) {
                unsigned int bit = 1u << slot;
                int major = g_digitizer_touch_major[slot];
                if ((g_digitizer_onset_pending & bit) && major >= threshold) {
                    g_digitizer_onset_low_confidence |= bit;
                    ptp_log("digitizer palm test onset slot=%u major=%d", slot, major);
                }
            }
        }
        g_digitizer_onset_pending = 0;
    }
}

static bool digitizer_key(const uint8_t *bits, unsigned int code) {
    return (bits[code / 8] >> (code % 8)) & 1u;
}

static void digitizer_read_twist(digitizer_pen_input *pen) {
    struct input_absinfo axis = {0};
    if (ioctl(pen->fd, EVIOCGABS(ABS_BRAKE), &axis) == 0 &&
        axis.minimum == 0 && axis.maximum == 360) {
        pen->twist_axis = true;
        if (axis.value >= 0 && axis.value <= 360) pen->twist = axis.value;
    }
}

static void digitizer_pen_read(const struct input_event *event, int fd) {
    if (t_digitizer_pen < 0) return;
    digitizer_pen_input *pen = &g_digitizer_pens[t_digitizer_pen];
    if (pen->fd != fd || (event->type == EV_SYN && event->code == SYN_DROPPED)) {
        *pen = (digitizer_pen_input){.fd = fd};
        struct input_absinfo axis = {0};
        uint8_t keys[KEY_MAX / 8 + 1] = {0};
        if (ioctl(fd, EVIOCGKEY(sizeof(keys)), keys) == 0) {
            pen->in_range = digitizer_key(keys, BTN_TOOL_PEN);
            pen->tip = digitizer_key(keys, BTN_TOUCH);
            pen->barrel = digitizer_key(keys, BTN_STYLUS);
            pen->rubber = digitizer_key(keys, BTN_TOOL_RUBBER);
        }
        if (ioctl(fd, EVIOCGABS(ABS_PRESSURE), &axis) == 0) {
            pen->pressure = axis.value;
            pen->pressure_max = axis.maximum;
        }
        digitizer_read_twist(pen);
    }
    if (event->type == EV_KEY) {
        switch (event->code) {
            case BTN_TOOL_PEN: pen->in_range = event->value != 0; break;
            case BTN_TOUCH: pen->tip = event->value != 0; break;
            case BTN_STYLUS: pen->barrel = event->value != 0; break;
            case BTN_TOOL_RUBBER: pen->rubber = event->value != 0; break;
        }
    }
    if (event->type == EV_ABS && event->code == ABS_PRESSURE) pen->pressure = event->value;
    if (event->type == EV_ABS && event->code == ABS_BRAKE &&
        pen->twist_axis && event->value >= 0 && event->value <= 360) {
        pen->twist = event->value;
        pen->twist_events = true;
    }
    if (event->type == EV_SYN && event->code == SYN_REPORT) {
        t_digitizer_pen_syn = true;
        // Some driver revisions update the ABS value without a matching event.
        if (pen->twist_axis && !pen->twist_events && pen->in_range) {
            int64_t now = ptp_now_us();
            if (now - pen->twist_read_us >= 16000) {
                digitizer_read_twist(pen);
                pen->twist_read_us = now;
            }
        }
    }
}

static bool digitizer_target(const shadowhook_cpu_context_t *ctx) {
    int fd = PTP_LOAD(g_hidg1);
    return g_digitizer_enhanced && fd >= 0 && (int)ctx->regs[0] == fd && ctx->regs[1];
}

static void digitizer_replace_report(shadowhook_cpu_context_t *ctx, unsigned int size) {
    ctx->regs[1] = (uint64_t)(uintptr_t)t_digitizer_report;
    ctx->regs[2] = size;
}

static void digitizer_screen_write(shadowhook_cpu_context_t *ctx, bool synthetic) {
    if (!digitizer_target(ctx) || ctx->regs[2] != STOCK_TOUCH_BYTES) return;
    const uint8_t *stock = (const uint8_t *)(uintptr_t)ctx->regs[1];
    if (stock[0] != DIGITIZER_TOUCH_ID) return;
    unsigned int n = 0;
    memset(t_digitizer_report, 0, DIGITIZER_TOUCH_BYTES);
    t_digitizer_report[0] = DIGITIZER_TOUCH_ID;
    memcpy(g_digitizer_draft, g_digitizer_contacts, sizeof(g_digitizer_draft));
    bool down[DIGITIZER_SLOTS] = {false};
    for (unsigned int i = 0; i < DIGITIZER_SLOTS && i < stock[61]; i++) {
        const uint8_t *record = stock + 1 + 6 * i;
        unsigned int id = record[1];
        if (id >= DIGITIZER_SLOTS || down[id]) continue;
        if (!(record[0] & 1)) continue;
        unsigned int x = digitizer_u16(record + 2), y = digitizer_u16(record + 4);
        if (x > 32767 || y > 32767) continue;
        down[id] = true;
        int major = synthetic ? 1 : g_digitizer_touch_major[id];
        digitizer_contact *point = &g_digitizer_draft[id];
        bool confidence = major > 0 && major <= 255 && (!point->down || point->confidence);
        if (!synthetic && (g_digitizer_onset_low_confidence & (1u << id))) confidence = false;
        *point = (digitizer_contact){(uint16_t)x, (uint16_t)y, true, confidence};
        digitizer_touch_record(t_digitizer_report + 1 + 6 * n++, 3, id, point);
    }
    // Every host-visible contact gets a lift at its last successfully sent position.
    for (unsigned int id = 0; id < DIGITIZER_SLOTS; id++) {
        digitizer_contact *point = &g_digitizer_draft[id];
        if (!down[id] && point->down) {
            digitizer_touch_record(t_digitizer_report + 1 + 6 * n++, 0, id, point);
            point->down = false;
        }
    }
    t_digitizer_report[61] = (uint8_t)n;
    t_digitizer_touch_pending = true;
    digitizer_replace_report(ctx, DIGITIZER_TOUCH_BYTES);
}

static void digitizer_pen_release(uint8_t *out) {
    memcpy(out, g_digitizer_last_pen, DIGITIZER_PEN_BYTES);
    out[0] = DIGITIZER_PEN_ID;
    out[1] = 0;
    digitizer_put_u16(out + 6, 0);
    digitizer_put_u16(out + 10, 0);
}

static bool digitizer_action_write(int fd, const uint8_t *report, size_t length) {
    return syscall(SYS_write, fd, report, length) == (ssize_t)length;
}

static bool digitizer_write_buttons(int fd, uint8_t buttons) {
    uint8_t report[DIGITIZER_PEN_BYTES];
    if (g_digitizer_host_pen >= 0) {
        memcpy(report, g_digitizer_last_pen, sizeof(report));
        report[1] &= ~(DIGITIZER_BARREL | DIGITIZER_SECONDARY_BARREL);
        if (g_digitizer_pens[g_digitizer_host_pen].barrel) report[1] |= DIGITIZER_BARREL;
    } else {
        digitizer_pen_release(report);
    }
    report[1] |= buttons;
    if (!digitizer_action_write(fd, report, sizeof(report))) return false;
    g_digitizer_aux_buttons = buttons;
    memcpy(g_digitizer_last_pen, report, sizeof(report));
    return true;
}

static bool digitizer_write_swipe(int fd, uint8_t key) {
    const uint8_t report[DIGITIZER_SWIPE_BYTES] = {DIGITIZER_SWIPE_ID, 0, 0, key};
    if (!digitizer_action_write(fd, report, sizeof(report))) return false;
    g_digitizer_swipe_key = key;
    return true;
}

static void digitizer_action_tick(bool closing) {
    int fd = PTP_LOAD(g_hidg1);
    if (!g_digitizer_enhanced || fd < 0) return;

    if (closing || PTP_LOAD(g_stop) || g_digitizer_release_actions) {
        g_digitizer_action_count = 0;
        if (g_digitizer_swipe_key && !digitizer_write_swipe(fd, 0)) return;
        if (g_digitizer_aux_buttons && !digitizer_write_buttons(fd, 0)) return;
        g_digitizer_release_actions = false;
        return;
    }
    if (!g_digitizer_action_count) return;

    const digitizer_action *action = &g_digitizer_actions[g_digitizer_action_head];
    if (action->code == KEY_F19 || action->code == KEY_KPENTER) {
        uint8_t mask = action->code == KEY_F19 ? DIGITIZER_BARREL : DIGITIZER_SECONDARY_BARREL;
        uint8_t buttons = action->down ? g_digitizer_aux_buttons | mask :
                                         g_digitizer_aux_buttons & ~mask;
        if (buttons != g_digitizer_aux_buttons && !digitizer_write_buttons(fd, buttons)) return;
    } else {
        uint8_t key = action->down ? (action->code == KEY_KP9 ? 0x6a : 0x6b) : 0;
        if (key != g_digitizer_swipe_key && !digitizer_write_swipe(fd, key)) return;
    }
    ptp_log("pen gesture code=%u down=%d", (unsigned int)action->code, action->down);
    g_digitizer_action_head = (g_digitizer_action_head + 1) % DIGITIZER_ACTION_CAPACITY;
    g_digitizer_action_count--;
}

static void digitizer_aux_read(const struct input_event *event, int fd, int result, int error) {
    if (fd == g_digitizer_aux_fd && (result == 0 ||
        (result < 0 && (error == ENODEV || error == EBADF || error == EIO)))) {
        g_digitizer_aux_fd = -1;
        g_digitizer_release_actions = true;
        return;
    }
    if (result != (int)sizeof(*event) || !event || PTP_LOAD(g_stop)) return;
    if (fd == g_digitizer_aux_fd && event->type == EV_SYN && event->code == SYN_DROPPED) {
        g_digitizer_release_actions = true;
        return;
    }
    if (event->type != EV_KEY || (event->value != 0 && event->value != 1)) return;
    switch (event->code) {
        case KEY_F19:
        case KEY_KPENTER:
        case KEY_KP9:
        case KEY_KP3:
            break;
        default:
            return;
    }
    if (fd != g_digitizer_aux_fd) {
        struct input_id id = {0};
        char name[64] = {0};
        if (ioctl(fd, EVIOCGID, &id) != 0 || id.bustype != BUS_BLUETOOTH ||
            id.vendor != 0x0022 || id.product != 0x5081 ||
            ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) < 0 ||
            strcmp(name, "Xiaomi Focus Pen Pro Keyboard") != 0)
            return;
        g_digitizer_aux_fd = fd;
    }
    if (g_digitizer_action_count == DIGITIZER_ACTION_CAPACITY) {
        g_digitizer_release_actions = true;
        return;
    }
    unsigned int tail = (g_digitizer_action_head + g_digitizer_action_count) % DIGITIZER_ACTION_CAPACITY;
    g_digitizer_actions[tail] = (digitizer_action){event->code, event->value == 1};
    g_digitizer_action_count++;
}

static bool digitizer_pen_tip(const digitizer_pen_input *pen) {
    // This digitizer also asserts BTN_TOUCH during zero-pressure hover.
    return pen->tip && pen->pressure > 0;
}

static void digitizer_pen_write(shadowhook_cpu_context_t *ctx) {
    if (!digitizer_target(ctx) || ctx->regs[2] != STOCK_PEN_BYTES || t_digitizer_pen < 0) return;
    const uint8_t *stock = (const uint8_t *)(uintptr_t)ctx->regs[1];
    if (stock[0] != DIGITIZER_PEN_ID) return;
    const digitizer_pen_input *pen = &g_digitizer_pens[t_digitizer_pen];
    bool tip = digitizer_pen_tip(pen);
    bool near = pen->in_range || tip || pen->rubber;
    int null_fd = PTP_LOAD(g_null_fd);
    if ((!near && g_digitizer_host_pen != t_digitizer_pen) ||
        (near && g_digitizer_host_pen >= 0 && g_digitizer_host_pen != t_digitizer_pen)) {
        ctx->regs[0] = (uint64_t)(unsigned int)null_fd;
        t_digitizer_pen_handled = true;
        return;
    }
    memcpy(t_digitizer_report, stock, STOCK_PEN_BYTES);
    if (near) {
        t_digitizer_report[1] = (uint8_t)(8u | g_digitizer_aux_buttons | (pen->barrel << 1) |
            ((pen->rubber && tip) << 2) | ((!pen->rubber && tip) ? 1 : 0));
        digitizer_put_u16(t_digitizer_report + 10, (uint16_t)((pen->twist % 360) * 100));
        int pressure = tip ? (int)((int64_t)DIGITIZER_PEN_PRESSURE_MAX *
                                  digitizer_u16(stock + 6) / DIGITIZER_STOCK_PEN_PRESSURE_MAX) : 0;
        if (tip && pen->pressure_max > 0) {
            int raw = pen->pressure;
            if (raw > pen->pressure_max) raw = pen->pressure_max;
            pressure = (int)((int64_t)DIGITIZER_PEN_PRESSURE_MAX * raw / pen->pressure_max);
        }
        if (pressure > DIGITIZER_PEN_PRESSURE_MAX) pressure = DIGITIZER_PEN_PRESSURE_MAX;
        if (tip && pressure == 0) pressure = 1;
        digitizer_put_u16(t_digitizer_report + 6, (uint16_t)pressure);
        int x = (int8_t)stock[8], y = (int8_t)stock[9];
        switch (t_digitizer_pen_rotation) {
            case 1: t_digitizer_report[8] = (uint8_t)y; t_digitizer_report[9] = (uint8_t)-x; break;
            case 2: t_digitizer_report[8] = (uint8_t)-x; t_digitizer_report[9] = (uint8_t)-y; break;
            case 3: t_digitizer_report[8] = (uint8_t)-y; t_digitizer_report[9] = (uint8_t)x; break;
        }
        if (!tip && (g_digitizer_last_pen[1] & 5) && g_digitizer_host_pen == t_digitizer_pen)
            memcpy(t_digitizer_report + 2, g_digitizer_last_pen + 2, 4);
    } else {
        digitizer_pen_release(t_digitizer_report);
    }
    t_digitizer_pen_pending = true;
    t_digitizer_pen_handled = true;
    digitizer_replace_report(ctx, DIGITIZER_PEN_BYTES);
}

static void digitizer_flush_pen(void) {
    if (!t_digitizer_pen_syn || t_digitizer_pen < 0 ||
        g_digitizer_host_pen != t_digitizer_pen) return;
    const digitizer_pen_input *pen = &g_digitizer_pens[t_digitizer_pen];
    if (t_digitizer_pen_handled && (pen->in_range || digitizer_pen_tip(pen) || pen->rubber)) return;
    int fd = PTP_LOAD(g_hidg1);
    if (fd < 0) return;
    // A pen frame outside the stock clip returns before the write callsite.
    int (*write_report)(int, const void *, size_t) = (void *)(g_base + 0xb870);
    digitizer_pen_release(t_digitizer_report);
    if (write_report(fd, t_digitizer_report, DIGITIZER_PEN_BYTES) == 0)
        g_digitizer_host_pen = -1;
}

static void ptp_intercept_callback(shadowhook_cpu_context_t *ctx, void *data) {
    if (!ctx || !data) return;
    const PtpInterceptSpec *site = (const PtpInterceptSpec *)data;
    switch (site->kind) {
        case PTP_IX_CREATE_HID:
            if (ctx->regs[0] && ctx->regs[1] && ctx->regs[2] &&
                strcmp((const char *)(uintptr_t)ctx->regs[0], "hid.usb0") == 0 &&
                strcmp((const char *)(uintptr_t)ctx->regs[1], "9") == 0 &&
                ctx->regs[3] == sizeof(k_stock_descriptor) &&
                memcmp((const void *)(uintptr_t)ctx->regs[2],
                       k_stock_descriptor, sizeof(k_stock_descriptor)) == 0) {
                static const char length64[] = "64";
                ctx->regs[1] = (uint64_t)(uintptr_t)length64;
                ctx->regs[2] = (uint64_t)(uintptr_t)k_combined_descriptor;
                ctx->regs[3] = sizeof(k_combined_descriptor);
                PTP_STORE(g_descriptor_patched, 1);
                ptp_log("combined descriptor args installed");
            }
            break;
        case PTP_IX_DIGITIZER_CREATE:
            if (PTP_LOAD(g_ready) && ctx->regs[0] && ctx->regs[1] && ctx->regs[2] &&
                strcmp((const char *)(uintptr_t)ctx->regs[0], "hid.usb1") == 0 &&
                strcmp((const char *)(uintptr_t)ctx->regs[1], "62") == 0 &&
                ctx->regs[3] == sizeof(k_stock_digitizer_descriptor) &&
                memcmp((const void *)(uintptr_t)ctx->regs[2], k_stock_digitizer_descriptor,
                       sizeof(k_stock_digitizer_descriptor)) == 0) {
                ctx->regs[2] = (uintptr_t)k_digitizer_descriptor;
                ctx->regs[3] = sizeof(k_digitizer_descriptor);
                g_digitizer_enhanced = true;
                ptp_log("enhanced touch and pen descriptor installed");
            }
            break;
        case PTP_IX_SETUP_ENTER:
            digitizer_reset();
            PTP_STORE(g_hidg0, -1);
            PTP_STORE(g_hidg1, -1);
            PTP_STORE(g_touchpad_fd, -1);
            ptp_reset_state("do_setup");
            break;
        case PTP_IX_SETUP_LEAVE:
            ptp_reset_state("do_setup_done");
            break;
        case PTP_IX_TOUCHPAD_ENTER:
            if (PTP_LOAD(g_hidg0) >= 0) {
                PTP_STORE(g_touchpad_fd, (int)ctx->regs[1]);
                t_touchpad_depth++;
                ptp_control_poll();
                ptp_owner_tick((int)ctx->regs[1]);
            }
            break;
        case PTP_IX_TOUCHPAD_LEAVE:
            if (t_touchpad_depth > 0) t_touchpad_depth--;
            if (t_touchpad_depth == 0) PTP_STORE(g_touchpad_fd, -1);
            break;
        case PTP_IX_TEARDOWN_ENTER:
            digitizer_action_tick(true);
            PTP_STORE(g_digitizer_pen_route, 0);
            ptp_best_effort_release("do_teardown");
            PTP_STORE(g_hidg0, -1);
            PTP_STORE(g_hidg1, -1);
            break;
        case PTP_IX_TEARDOWN_LEAVE:
            digitizer_reset();
            PTP_STORE(g_hidg0, -1);
            PTP_STORE(g_hidg1, -1);
            ptp_reset_state("do_teardown_done");
            break;
        case PTP_IX_HIDG0_OPEN_ENTER:
            t_open0_valid = ctx->regs[0] &&
                strcmp((const char *)(uintptr_t)ctx->regs[0], "/dev/hidg0") == 0 &&
                (int)ctx->regs[1] == 0x802;
            break;
        case PTP_IX_HIDG0_OPEN_LEAVE:
            if (t_open0_valid && (int)ctx->regs[0] >= 0) {
                PTP_STORE(g_hidg0, (int)ctx->regs[0]);
                ptp_log("borrowed hidg0 fd=%d", (int)ctx->regs[0]);
            }
            t_open0_valid = 0;
            break;
        case PTP_IX_HIDG1_OPEN_ENTER:
            t_open1_valid = ctx->regs[0] &&
                strcmp((const char *)(uintptr_t)ctx->regs[0], "/dev/hidg1") == 0 &&
                (int)ctx->regs[1] == 0x802;
            break;
        case PTP_IX_HIDG1_OPEN_LEAVE:
            if (t_open1_valid && (int)ctx->regs[0] >= 0) {
                PTP_STORE(g_hidg1, (int)ctx->regs[0]);
                PTP_STORE(g_digitizer_pen_route, g_digitizer_enhanced);
                ptp_log("borrowed hidg1 fd=%d", (int)ctx->regs[0]);
            }
            t_open1_valid = 0;
            break;
        case PTP_IX_HIDG0_READ_ENTER:
        case PTP_IX_HIDG1_READ_ENTER:
            ptp_hid_read_enter(ctx);
            break;
        case PTP_IX_HIDG0_READ_LEAVE:
        case PTP_IX_HIDG1_READ_LEAVE:
            ptp_hid_read_leave(ctx);
            break;
        case PTP_IX_TOUCHPAD_READ_ENTER:
            ptp_touchpad_read_enter(ctx);
            break;
        case PTP_IX_TOUCHPAD_READ_LEAVE:
            ptp_touchpad_read_leave(ctx);
            break;
        case PTP_IX_DIGITIZER_READ_ENTER:
            if (g_digitizer_enhanced) {
                t_digitizer_read_fd = (int)ctx->regs[0];
                t_digitizer_event = (const struct input_event *)(uintptr_t)ctx->regs[1];
            }
            break;
        case PTP_IX_SCREEN_READ_LEAVE:
        case PTP_IX_PEN_READ_LEAVE:
            if (g_digitizer_enhanced && ctx->regs[0] == sizeof(*t_digitizer_event) && t_digitizer_event) {
                if (site->kind == PTP_IX_SCREEN_READ_LEAVE)
                    digitizer_screen_read(t_digitizer_event, t_digitizer_read_fd);
                else
                    digitizer_pen_read(t_digitizer_event, t_digitizer_read_fd);
            }
            t_digitizer_event = NULL;
            break;
        case PTP_IX_PEN_AUX_READ_LEAVE: {
            int error = errno;
            if (g_digitizer_enhanced) {
                digitizer_aux_read(t_digitizer_event, t_digitizer_read_fd, (int)ctx->regs[0], error);
                digitizer_action_tick(false);
            }
            t_digitizer_event = NULL;
            errno = error;
            break;
        }
        case PTP_IX_PEN_ACTION_TICK: {
            int error = errno;
            digitizer_action_tick(false);
            errno = error;
            break;
        }
        case PTP_IX_PEN_ENTER:
            if (g_digitizer_enhanced && ctx->regs[6] < 2) {
                int rotation;
                memcpy(&rotation, (const uint8_t *)(uintptr_t)ctx->regs[0] + 12, sizeof(rotation));
                t_digitizer_pen = (int)ctx->regs[6];
                t_digitizer_pen_rotation = rotation;
                t_digitizer_pen_syn = t_digitizer_pen_handled = false;
            }
            break;
        case PTP_IX_PEN_LEAVE:
            if (g_digitizer_enhanced) digitizer_flush_pen();
            t_digitizer_pen = -1;
            break;
        case PTP_IX_SCREEN_WRITE:
        case PTP_IX_SCREEN_EMPTY:
            digitizer_screen_write(ctx, site->kind == PTP_IX_SCREEN_EMPTY);
            break;
        case PTP_IX_SCREEN_WRITE_LEAVE:
            if (t_digitizer_touch_pending && ((int)ctx->regs[0] == 0 || ctx->regs[0] == DIGITIZER_TOUCH_BYTES))
                memcpy(g_digitizer_contacts, g_digitizer_draft, sizeof(g_digitizer_contacts));
            t_digitizer_touch_pending = false;
            break;
        case PTP_IX_PEN_WRITE:
            digitizer_pen_write(ctx);
            break;
        case PTP_IX_PEN_EMPTY:
            if (digitizer_target(ctx) && ctx->regs[2] == STOCK_PEN_BYTES) {
                digitizer_pen_release(t_digitizer_report);
                digitizer_replace_report(ctx, DIGITIZER_PEN_BYTES);
            }
            break;
        case PTP_IX_PEN_WRITE_LEAVE:
            if (t_digitizer_pen_pending && (int)ctx->regs[0] == 0) {
                memcpy(g_digitizer_last_pen, t_digitizer_report, DIGITIZER_PEN_BYTES);
                g_digitizer_host_pen = t_digitizer_report[1] & 8 ? t_digitizer_pen : -1;
            }
            t_digitizer_pen_pending = false;
            break;
        case PTP_IX_DIRECT_WRITE:
        case PTP_IX_HELPER_WRITE: {
            int fd = (int)ctx->regs[0];
            int null_fd = PTP_LOAD(g_null_fd);
            if (null_fd >= 0 && ptp_gate_stock_output(fd)) {
                ctx->regs[0] = (uint64_t)(uint32_t)null_fd;
                PTP_ADD(g_suppressed, 1);
            } else if (site->va == 0xd8bc) {
                digitizer_screen_write(ctx, true);
            }
            break;
        }
        case PTP_IX_CLOSE: {
            int fd = (int)ctx->regs[0];
            int h0 = PTP_LOAD(g_hidg0);
            int h1 = PTP_LOAD(g_hidg1);
            if (fd >= 0 && (fd == h0 || fd == h1)) {
                if (fd == h1) {
                    digitizer_action_tick(true);
                    PTP_STORE(g_digitizer_pen_route, 0);
                }
                ptp_log("owner close fd=%d", fd);
                ptp_best_effort_release("owner_close");
                if (fd == PTP_LOAD(g_hidg0)) PTP_STORE(g_hidg0, -1);
                if (fd == PTP_LOAD(g_hidg1)) PTP_STORE(g_hidg1, -1);
            }
            break;
        }
        default:
            break;
    }
}

static void *ptp_monitor(void *unused) {
    (void)unused;
    ptp_mark("ptp-monitor");
    while (!PTP_LOAD(g_ready)) usleep(100000);
    int pen_route = 0;
    for (;;) {
        ptp_control_poll();
        int route = PTP_LOAD(g_digitizer_pen_route) && !PTP_LOAD(g_stop);
        if (route != pen_route) {
            ptp_mark(route ? "ptp-pen-route" : "ptp-monitor");
            pen_route = route;
        }
        if (PTP_LOAD(g_input_active) && PTP_LOAD(g_disarm)) {
            int64_t deadline = 0;
            pthread_mutex_lock(&g_lock);
            deadline = g_release_deadline_ms;
            pthread_mutex_unlock(&g_lock);
            if (deadline && ptp_now_ms() >= deadline)
                ptp_finish_input("release_timeout");
        }
        ptp_feature_poll();
        usleep(100000);
    }
    return NULL;
}

static void *ptp_state_log_thread(void *unused) {
    (void)unused;
    ptp_mark("ptp-state-log");
    for (;;) {
        char thread_name[16];
        int fail_site = PTP_LOAD(g_fail_site);
        if (fail_site)
            snprintf(thread_name, sizeof(thread_name), "ptp-f-%x", fail_site);
        else
            snprintf(thread_name, sizeof(thread_name), "ptp-s-%d", PTP_LOAD(g_stage));
        ptp_mark(thread_name);
        if (PTP_LOAD(g_ready)) {
            __android_log_print(
                ANDROID_LOG_INFO, PTP_TAG,
                "state pid=%d gen=%d mode=%d selective=%d active=%d reports=%d errors=%d suppressed=%d shortcut_suppressed=%d raw_peak=%d sent_peak=%d h0=%d h1=%d",
                getpid(), PTP_LOAD(g_generation), PTP_LOAD(g_mode),
                PTP_LOAD(g_selective), PTP_LOAD(g_input_active),
                PTP_LOAD(g_reports), PTP_LOAD(g_write_errors),
                PTP_LOAD(g_suppressed), PTP_LOAD(g_shortcut_suppressed),
                PTP_LOAD(g_raw_contact_peak), PTP_LOAD(g_sent_contact_peak),
                PTP_LOAD(g_hidg0), PTP_LOAD(g_hidg1));
            usleep(5000000);
        } else {
            usleep(1000000);
        }
    }
    return NULL;
}

static void ptp_init_hooks(void) {
    PTP_STORE(g_stage, 1);
    ptp_mark("ptp-start");
    ptp_log("constructor begin");
    if (ptp_find_main() != 0) {
        ptp_log("main executable not found");
        return;
    }
    PTP_STORE(g_stage, 2);
    ptp_mark("ptp-main");
    ptp_log("main base=%p", (void *)g_base);
    PTP_STORE(g_stage, 3);
    if (ptp_verify_manifest() != 0) return;
    ptp_mark("ptp-manifest");
    ptp_log("manifest verified");
    if (shadowhook_init(SHADOWHOOK_MODE_UNIQUE, false) != 0) {
        ptp_log("shadowhook init failed errno=%d", shadowhook_get_errno());
        return;
    }
    PTP_STORE(g_stage, 4);
    ptp_mark("ptp-shadowhook");
    ptp_log("shadowhook initialized");
    g_null_fd = (int)syscall(SYS_openat, AT_FDCWD, "/dev/null", O_WRONLY | O_CLOEXEC, 0);
    if (g_null_fd < 0) {
        ptp_log("open /dev/null failed errno=%d", errno);
        return;
    }
    PTP_STORE(g_stage, 5);
    PTP_STORE(g_fail_site, (int)PTP_SEND_SHORTCUT_VA);
    void *shortcut_stub = shadowhook_hook_func_addr(
        (void *)(g_base + PTP_SEND_SHORTCUT_VA), (void *)ptp_send_shortcut,
        &g_send_shortcut_original);
    if (!shortcut_stub) {
        ptp_log("shortcut hook failed site=%x errno=%d",
                PTP_SEND_SHORTCUT_VA, shadowhook_get_errno());
        return;
    }
    if (ptp_install_intercepts() != 0) {
        ptp_log("intercept installation failed errno=%d", shadowhook_get_errno());
        if (shadowhook_unhook(shortcut_stub) != 0)
            ptp_log("shortcut unhook failed errno=%d", shadowhook_get_errno());
        return;
    }
    PTP_STORE(g_stage, 7);
    ptp_mark("ptp-ix");
    ptp_log("callsite intercepts installed count=%d", PTP_INTERCEPT_COUNT);
    ptp_log("shortcut bypass installed site=%x", PTP_SEND_SHORTCUT_VA);
    PTP_STORE(g_ready, 1);
    ptp_mark("ptp-ready");
    ptp_log("native ready source=%s mode=callsite", PTP_SOURCE_SHA256);
}

static void *ptp_init_thread(void *unused) {
    (void)unused;
    /* ShadowHook installs linker constructor hooks; do not initialize it while
     * the dynamic linker is still executing this library's constructor. */
    usleep(50000);
    ptp_init_hooks();
    return NULL;
}

__attribute__((constructor)) static void ptp_constructor(void) {
    pthread_t logger;
    if (pthread_create(&logger, NULL, ptp_logger_thread, NULL) == 0)
        pthread_detach(logger);
    pthread_t monitor;
    if (pthread_create(&monitor, NULL, ptp_monitor, NULL) == 0)
        pthread_detach(monitor);
    pthread_t state_log;
    if (pthread_create(&state_log, NULL, ptp_state_log_thread, NULL) == 0)
        pthread_detach(state_log);
    pthread_t thread;
    if (pthread_create(&thread, NULL, ptp_init_thread, NULL) == 0)
        pthread_detach(thread);
}
