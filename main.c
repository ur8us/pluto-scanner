/*
 * Pluto SDR Scanner with a Million Times Zoom - Backend
 *
 * Operational model:
 *   Mode 1: scan/hop. For wide visible spans, retune ADALM-Pluto across
 *           several receiver centers, capture a short buffer at each hop,
 *           FFT every hop, stitch the hop spectra, and publish one waterfall
 *           line after all hop slots have contributed.
 *
 *   Mode 2: single frequency. For narrow visible spans, keep Pluto tuned to
 *           one receiver center and use FFT/CIC planning to balance true bin
 *           resolution against the requested waterfall line-rate budget.
 *
 * The HTTP API, SSE stream, and frontend all use air/signal frequencies.
 * Converter math is applied only at the hardware boundary.
 */

#define _USE_MATH_DEFINES
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <direct.h>
#include <io.h>
#ifndef ssize_t
typedef SSIZE_T ssize_t;
#endif
#define chdir _chdir
#define close(fd) closesocket((SOCKET)(uintptr_t)(fd))
#define read(fd, buf, len) recv((SOCKET)(uintptr_t)(fd), (char *)(buf), (int)(len), 0)
#define write(fd, buf, len) send((SOCKET)(uintptr_t)(fd), (const char *)(buf), (int)(len), 0)
#define PATH_MAX_CHARS 1024
static int win32_nanosleep(const struct timespec *req, struct timespec *rem)
{
    DWORD msec = 0;
    (void)rem;
    if (req) {
        uint64_t total = (uint64_t)req->tv_sec * 1000u +
                         (uint64_t)(req->tv_nsec + 999999L) / 1000000u;
        msec = total > UINT32_MAX ? UINT32_MAX : (DWORD)total;
    }
    Sleep(msec);
    return 0;
}
#define nanosleep(req, rem) win32_nanosleep((req), (rem))
#else
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <sys/select.h>
#include <fcntl.h>
#include <dirent.h>
#define PATH_MAX_CHARS 1024
#endif
#include <errno.h>
#include <limits.h>
#include <iio.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ------------------------------------------------------------------ */
/* Config                                                             */
/* ------------------------------------------------------------------ */
#define DEFAULT_HTTP_PORT   8080
#define DEFAULT_BIND_ADDRESS "127.0.0.1"
#define MAX_HEADERS         4096
#define MAX_REQUEST         65536
#define MAX_CLIENTS         64
#define MAX_SSE_CLIENTS     16
#define API_JSON_BODY_MAX   8192
#define MAX_FREQS           512
#define MAX_SAMPLE_RATES    32
#define HTML_PATH           "index.html"
#define BANDS_PATH          "bands.ini"
#define MARKERS_PATH        "markers.ini"
#define PROGRAM_TITLE       "Pluto SDR Scanner with a Million Times Zoom"
#define GOTO_TARGET_ZOOM_MIN 1.0
#define GOTO_TARGET_ZOOM_MAX 1000000.0
#define MIN_FREQ_START_HZ   0.0
#define RF_RECEIVER_MIN_HZ  70.0e6
#define RF_RECEIVER_MAX_HZ  6000.0e6
#define SCANNER_SAMPLE_RATE_HZ 61.44e6
#define DEFAULT_RF_BANDWIDTH_HZ 20.0e6
#define DEFAULT_BW_RATIO    0.85
#define PLUTO_AUTO_SAMPLE_RATE_HZ SCANNER_SAMPLE_RATE_HZ
#define PLUTO_AUTO_RF_BANDWIDTH_HZ DEFAULT_RF_BANDWIDTH_HZ
#define PLUTO_AUTO_BW_RATIO DEFAULT_BW_RATIO
#define PLUTO_RF_BW_LT_SR_RATIO 0.90
#define DISPLAY_BINS_MIN    64
#define DISPLAY_BINS_MAX    32768

/* FFT: powers of 2 from 1024 to 65536 */
#define FFT_SIZE_MIN        1024
#define FFT_SIZE_MAX        65536
#define SINGLE_RAW_FFT_MIN  2048
#define SINGLE_FFT_SIZE_MAX FFT_SIZE_MAX
#define SINGLE_DECIM_MAX    4096
#define SCAN_FFT_SIZE_MAX   8192
#define SINGLE_DECIM_ASYNC_MIN_LEN 8192U
#define SINGLE_DECIM_ASYNC_MAX_LEN 262144U
#define CACHED_PREVIEW_MAX_SAMPLES 16777984U
#define CACHED_PREVIEW_MAX_LINES 128U
#define CACHED_PREVIEW_MAX_AGE_MS 5000LL
#define SINGLE_ZERO_IF_GUARD_HZ 50000.0
#define PLUTO_REFERENCE_INPUT_HZ 40000000ULL
/* The stock AD936x driver doubles a 40 MHz input reference for the RFPLL. */
#define PLUTO_RFPLL_REFERENCE_HZ (2ULL * PLUTO_REFERENCE_INPUT_HZ)
#define PLUTO_RFPLL_FRACTIONAL_MODULUS 8388593ULL
#define PLUTO_RFPLL_VCO_MIN_HZ 6000000000ULL
#define PLUTO_RFPLL_VCO_MAX_HZ 12000000000ULL
#define JSON_COORD_FMT "%.9f"
#define CIC_STAGES          3
/* The minimum scan FFT has a Hann ENBW close to 90 kHz at 61.44 MSPS.
 * Waterfall presentation uses this as the common white-noise reference. */
#define WATERFALL_NOISE_REFERENCE_ENBW_HZ 90000.0
#define FFTS_PER_STEP       32
#define SCAN_FFTS_PER_STEP  1
/* Phase-1 measured best scan profile for Pluto: 1024-sample buffers,
 * one kernel buffer, two stale-buffer discards, and no settle delay. */
#define SCAN_BUF_LEN        1024U
#define PLUTO_SCAN_KERNEL_BUFFERS 1U
#ifndef PLUTO_CONTINUOUS_KERNEL_BUFFERS
#define PLUTO_CONTINUOUS_KERNEL_BUFFERS 4U
#endif
#define PLUTO_DISCARD_COUNT 2U
#define PLUTO_SETTLE_DELAY_US 0U
#define PLUTO_TUNE_RETRY_COUNT 10
#define PLUTO_TUNE_RETRY_DELAY_MS 20
#define PLUTO_RESTART_DELAY_MS 40
#define PLUTO_READ_RETRY_COUNT 2
#define PLUTO_READ_RETRY_DELAY_MS 150
#define PLUTO_REFILL_FLAG_RECOVERED 0x01U
#define PLUTO_REFILL_FLAG_SHORT_READ 0x02U
#define PLUTO_AUTO_RESTART_SUPPRESS_MS 3000LL
#define PLUTO_EST_HOP_MS    3.4
#define PLUTO_EST_SINGLE_STREAM_SPS 2100000.0
#define PLUTO_EST_CIC_STREAM_SPS 1850000.0
#define PLUTO_RATE_LIMIT_GUARD 0.85
#define PLUTO_DEFAULT_GAIN_DB 20.0
#define PLUTO_GAIN_MIN_DB   -3.0
#define PLUTO_GAIN_MAX_DB   71.0
#define PLUTO_DEFAULT_URI   "ip:192.168.2.1"
#define PLUTO_ERR_OK        0
#define PLUTO_MIN_FREQS_CNT 1
#define PROCESS_QUEUE_LEN   8
#ifndef PSEUDO_RANDOM_SAMPLE_SOURCE
#define PSEUDO_RANDOM_SAMPLE_SOURCE 0
#endif
#define SYNTHETIC_TONE_SAMPLE_SOURCE 2
#define DB_FLOOR            -100.0f
#define DB_CEIL             -20.0f
#define FRONTEND_IDLE_STOP_MS 20000LL
#define MAX_MARKERS         2048
#define MARKER_NAME_MAX     128

/* Max bins per step at largest FFT */
#define MAX_BINS_PER_STEP   FFT_SIZE_MAX

/* Max total bins per line (software scan steps * 65536 bins) */
#define MAX_BINS_PER_LINE   (MAX_FREQS * MAX_BINS_PER_STEP)

typedef struct pluto_sdr_dev_t {
    struct iio_context *ctx;
    struct iio_device *phy;
    struct iio_device *rxdev;
    struct iio_channel *phy_rx0;
    struct iio_channel *rx_lo;
    struct iio_channel *rx_i;
    struct iio_channel *rx_q;
    struct iio_buffer *rxbuf;
    int16_t *raw_i;
    int16_t *raw_q;
    float *float_buf;
    size_t buffer_samples;
    unsigned int kernel_buffers;
    int buffer_cancelled;
    double scan_freqs[MAX_FREQS];
    unsigned int scan_count;
    int scan_index;
    volatile int cancel_requested;
    char uri[128];
    char context_name[64];
    char context_description[256];
    char gain_mode[32];
    char rf_port[32];
    char rx_path_rates[256];
    double sample_rate;
    double rf_bandwidth;
    double hardwaregain_db;
    double rssi_db;
    double input_peak;
    uint64_t clipped_samples;
} pluto_sdr_dev_t;

static int g_fft_size = 1024;   /* runtime FFT size */
static int g_bins_per_step = 512;  /* g_fft_size / 2 */

/* ------------------------------------------------------------------ */
/* Global state                                                       */
/* ------------------------------------------------------------------ */
static struct pluto_sdr_dev_t *g_dev = NULL;
static pthread_t g_scan_thread;
static volatile int g_scanning = 0;
static volatile int g_scan_thread_joinable = 0;
static pthread_mutex_t g_cancel_mutex = PTHREAD_MUTEX_INITIALIZER;
static volatile int g_cancel_requested = 0;
typedef enum {
    RUN_MODE_SCAN = 0,
    RUN_MODE_SINGLE = 1
} run_mode_t;
typedef struct {
    /* FFT size applied to the current single-frequency stream. */
    int fft_size;
    /* CIC decimation factor before FFT; 1 means raw Pluto samples. */
    int decim_factor;
    /* Decimated samples advanced between FFT frames. */
    int decim_hop;
    /* Sample rate seen by the FFT after optional CIC decimation. */
    double fft_samplerate;
    /* Frequency span represented by the FFT source buffer. */
    double source_span;
    /* Fraction of FFT bins extracted when using the raw Pluto sample rate. */
    double extraction_ratio;
    /* Pluto hardware sample rate selected for this single-frequency view. */
    double hardware_samplerate;
    /* Pluto RF bandwidth selected for this single-frequency view. */
    double hardware_rf_bandwidth;
    /* Hardware/profile passband usage displayed for this view. */
    double hardware_bw_ratio;
    /* Minimum separation between visible edge and zero IF in hertz. */
    double zero_if_guard_hz;
    /* Nonzero when minimum cadence uses overlapping CIC FFT windows. */
    int minimum_rate_limited;
    /* Nonzero when estimated raw cadence satisfies the requested minimum. */
    int minimum_rate_achieved;
    /* Estimated unthrottled line cadence in lines/s. */
    double estimated_line_rate;
    /* FFT bins covering the visible interval before display reduction. */
    double visible_raw_bins;
    /* Visible raw-bin density relative to one frontend display pixel. */
    double visible_bins_per_pixel;
} single_fft_plan_t;
typedef struct {
    double start;
    double end;
} freq_interval_t;
static volatile run_mode_t g_active_mode = RUN_MODE_SCAN;
static pthread_mutex_t g_fft_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_fft_generation = 1;

/* Scan parameters */
static double g_freq_start   = 70.0e6;
static double g_freq_end     = 1000.0e6;
static double g_visible_start = 70.0e6;
static double g_visible_end   = 1000.0e6;
static double g_converter_freq = 0.0;
static double g_samplerate   = SCANNER_SAMPLE_RATE_HZ;
static double g_rf_bandwidth = DEFAULT_RF_BANDWIDTH_HZ;
static double g_bw_ratio     = DEFAULT_BW_RATIO;
static int g_display_bins    = 1024;
static uint32_t g_min_rate_lps = 10;
static uint32_t g_rate_limit_lps = 20;
/* Enable the conservative 40 MHz-reference RFPLL rounding model below. */
static uint32_t g_fq_err_correction = 1;
static uint32_t g_view_id    = 1;
static int g_http_port = DEFAULT_HTTP_PORT;
static char g_bind_address[128] = DEFAULT_BIND_ADDRESS;
static double g_hardwaregain_db = PLUTO_DEFAULT_GAIN_DB;
static char g_gain_mode[32] = "manual";
static char g_rf_port[32] = "A_BALANCED";
static double g_last_rssi_db = -999.0;
static double g_last_input_peak = 0.0;
static uint64_t g_last_clipped_samples = 0;
static char g_pluto_uri[128] = PLUTO_DEFAULT_URI;
static uint32_t g_direct_sampling = 0;
static double g_goto_freq = 435.0e6;
static double g_goto_target_zoom = GOTO_TARGET_ZOOM_MAX;
static uint32_t g_goto_animate = 0;
static double g_goto_delay_s = 2.0;

typedef struct {
    float *samples;
    size_t capacity;
    size_t count;
    size_t write_pos;
    double source_center_hz;
    double source_start_hz;
    double source_end_hz;
    double samplerate_hz;
    double rf_bandwidth_hz;
    long long updated_msec;
    uint64_t continuity_generation;
    int valid;
} recent_sample_cache_t;

static recent_sample_cache_t g_recent_samples;
static pthread_mutex_t g_recent_samples_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Device info */
static char g_hw_rev[64]    = "unknown";
static char g_fw_ver[64]    = "unknown";
static char g_serial[64]    = "unknown";
static char g_manufacturer[64] = "unknown";
static char g_product[64]   = "unknown";
static double g_sample_rates[MAX_SAMPLE_RATES];
static unsigned int g_sample_rate_count = 0;
static long long g_last_frontend_activity_msec = 0;
static volatile int g_auto_restart_on_reconnect = 0;
static long long g_last_auto_restart_msec = 0;
static long long g_auto_restart_suppress_until_msec = 0;

/* SSE client list */
static int g_sse_fds[MAX_SSE_CLIENTS];
static pthread_mutex_t g_sse_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Frontend traffic accounting: compressed SSE bytes for one frontend stream. */
#define TRAFFIC_SAMPLE_COUNT 1024
#define TRAFFIC_WINDOW_MS    2000LL
typedef struct {
    long long msec;
    size_t bytes;
} traffic_sample_t;

static traffic_sample_t g_traffic_samples[TRAFFIC_SAMPLE_COUNT];
static int g_traffic_sample_pos = 0;
static pthread_mutex_t g_traffic_mutex = PTHREAD_MUTEX_INITIALIZER;

static long long now_msec(void);

/* ------------------------------------------------------------------ */
/* SSE helpers                                                        */
/* ------------------------------------------------------------------ */
static void mark_frontend_activity(void)
{
    g_last_frontend_activity_msec = now_msec();
}

/**
 * @brief Write an entire response buffer to a blocking HTTP client.
 *
 * @param fd Socket descriptor.
 * @param data Bytes to write.
 * @param len Byte count.
 * @return 0 on complete write, -1 on disconnect or write error.
 */
static int write_all(int fd, const char *data, size_t len)
{
    size_t done = 0;
    while (done < len) {
        ssize_t n = write(fd, data + done, len - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        done += (size_t)n;
    }
    return 0;
}

/**
 * @brief Write an SSE event without allowing one slow client to stall scanning.
 *
 * The SSE sockets are set nonblocking when registered. If a client cannot
 * accept a complete event immediately, the client is closed and will reconnect
 * through the browser's EventSource retry path.
 *
 * @param fd SSE socket descriptor.
 * @param data Event bytes.
 * @param len Byte count.
 * @return 0 on complete write, -1 when the client should be dropped.
 */
static int write_all_nonblocking(int fd, const char *data, size_t len)
{
    size_t done = 0;
    while (done < len) {
        ssize_t n = write(fd, data + done, len - done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return -1;
        if (n <= 0)
            return -1;
        done += (size_t)n;
    }
    return 0;
}

static int sse_client_count(void)
{
    int count = 0;

    pthread_mutex_lock(&g_sse_mutex);
    for (int i = 0; i < MAX_SSE_CLIENTS; i++) {
        if (g_sse_fds[i] > 0)
            count++;
    }
    pthread_mutex_unlock(&g_sse_mutex);

    return count;
}

static void sse_add_client(int fd)
{
    int added = 0;
#ifdef _WIN32
    u_long nonblocking = 1;
    (void)ioctlsocket((SOCKET)(uintptr_t)fd, FIONBIO, &nonblocking);
#else
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags >= 0)
        (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif

    pthread_mutex_lock(&g_sse_mutex);
    for (int i = 0; i < MAX_SSE_CLIENTS; i++) {
        if (g_sse_fds[i] == 0) {
            g_sse_fds[i] = fd;
            added = 1;
            break;
        }
    }
    pthread_mutex_unlock(&g_sse_mutex);
    if (!added) close(fd);
}

static size_t sse_broadcast(const char *data, int len)
{
    int fds[MAX_SSE_CLIENTS];
    int count = 0;
    size_t delivered = 0;

    pthread_mutex_lock(&g_sse_mutex);
    for (int i = 0; i < MAX_SSE_CLIENTS; i++) {
        int fd = g_sse_fds[i];
        if (fd > 0 && count < MAX_SSE_CLIENTS)
            fds[count++] = fd;
    }
    pthread_mutex_unlock(&g_sse_mutex);

    for (int i = 0; i < count; i++) {
        int fd = fds[i];
        if (write_all_nonblocking(fd, data, (size_t)len) == 0) {
            delivered += (size_t)len;
            continue;
        }

        pthread_mutex_lock(&g_sse_mutex);
        for (int j = 0; j < MAX_SSE_CLIENTS; j++) {
            if (g_sse_fds[j] == fd) {
                close(fd);
                g_sse_fds[j] = 0;
                break;
            }
        }
        pthread_mutex_unlock(&g_sse_mutex);
    }

    return delivered;
}

/* ------------------------------------------------------------------ */
/* FFT: radix-2, in-place, interleaved float [re,im,...]              */
/* ------------------------------------------------------------------ */
static void fft_c2c(float *data, int n)
{
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float tr = data[2*i], ti = data[2*i+1];
            data[2*i] = data[2*j]; data[2*i+1] = data[2*j+1];
            data[2*j] = tr; data[2*j+1] = ti;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        float ang = 2.0f * (float)M_PI / (float)len;
        float w_re = cosf(ang), w_im = -sinf(ang);
        for (int i = 0; i < n; i += len) {
            float cur_re = 1.0f, cur_im = 0.0f;
            for (int j = 0; j < len/2; j++) {
                int i0 = i + j, i1 = i0 + len/2;
                float re1 = data[2*i0], im1 = data[2*i0+1];
                float re2 = data[2*i1]*cur_re - data[2*i1+1]*cur_im;
                float im2 = data[2*i1]*cur_im + data[2*i1+1]*cur_re;
                data[2*i0] = re1 + re2; data[2*i0+1] = im1 + im2;
                data[2*i1] = re1 - re2; data[2*i1+1] = im1 - im2;
                float nr = cur_re*w_re - cur_im*w_im;
                float ni = cur_re*w_im + cur_im*w_re;
                cur_re = nr; cur_im = ni;
            }
        }
    }
}

/* Hann window — sized for max FFT */
static float g_window[FFT_SIZE_MAX];

static void init_window_for_size(float *window, int fft_size)
{
    for (int i = 0; i < fft_size; i++)
        window[i] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * i / (float)(fft_size - 1)));
}

static void init_window(void)
{
    init_window_for_size(g_window, g_fft_size);
}

static int normalize_fft_size(int new_size)
{
    if (new_size < FFT_SIZE_MIN) new_size = FFT_SIZE_MIN;
    if (new_size > FFT_SIZE_MAX) new_size = FFT_SIZE_MAX;
    /* Round down to nearest power of 2 */
    int size = 1;
    while (size * 2 <= new_size) size *= 2;
    return size;
}

/* Call this when g_fft_size changes */
static void update_fft_size(int new_size)
{
    int size = normalize_fft_size(new_size);
    pthread_mutex_lock(&g_fft_mutex);
    if (g_fft_size == size) {
        pthread_mutex_unlock(&g_fft_mutex);
        return;
    }
    g_fft_size = size;
    g_bins_per_step = g_fft_size / 2;
    init_window();
    g_fft_generation++;
    pthread_mutex_unlock(&g_fft_mutex);
}

static int current_fft_size(void)
{
    int size;
    pthread_mutex_lock(&g_fft_mutex);
    size = g_fft_size;
    pthread_mutex_unlock(&g_fft_mutex);
    return size;
}

static int current_display_bins(void);

static int next_power_of_two_int(int value)
{
    int size = 1;
    if (value <= 1)
        return 1;
    while (size < value && size <= SINGLE_FFT_SIZE_MAX / 2)
        size *= 2;
    return size;
}

static int clamp_power_of_two_int(int value, int min_value, int max_value)
{
    int result = next_power_of_two_int(value);
    if (result < min_value)
        result = min_value;
    if (result > max_value)
        result = max_value;
    return result;
}

/**
 * @brief Return the scan/hop RF bandwidth that is safe for Pluto.
 *
 * The AD936x IIO control exposes analog RF bandwidth independently from the
 * baseband sample rate. For this scanner we keep the configured analog
 * bandwidth strictly below the active sample rate, matching libad9361's normal
 * filter-design direction and avoiding ambiguous equal-rate profiles.
 *
 * @return Usable RF bandwidth in hertz for scan step calculation.
 */
static double pluto_usable_bandwidth_hz(void)
{
    double bw = g_rf_bandwidth;
    if (!isfinite(bw) || bw <= 0.0)
        bw = DEFAULT_RF_BANDWIDTH_HZ;
    if (bw >= g_samplerate)
        bw = g_samplerate * PLUTO_RF_BW_LT_SR_RATIO;
    return bw;
}

/**
 * @brief Return the host-side retune step used to stitch scan/hop rows.
 *
 * @return Air-frequency hop spacing in hertz.
 */
static double scan_step_hz(void)
{
    double ratio = g_bw_ratio;
    if (!isfinite(ratio) || ratio <= 0.0)
        ratio = DEFAULT_BW_RATIO;
    if (ratio > 1.0)
        ratio = 1.0;
    return pluto_usable_bandwidth_hz() * ratio;
}

typedef struct {
    double samplerate;
    double rf_bandwidth;
} pluto_rf_profile_t;

static const pluto_rf_profile_t g_single_profiles[] = {
    {  4.0e6,   2.0e6 },
    {  8.0e6,   4.0e6 },
    { 16.0e6,   8.0e6 },
    { 20.0e6,  10.0e6 },
    { 30.72e6, 15.0e6 },
    { 61.44e6, 20.0e6 }
};

/**
 * @brief Return the desired visible-edge clearance from zero IF.
 *
 * This guard is intentionally small compared with the displayed passband but
 * large enough that the DC/zero-IF artifact is outside the visible waterfall
 * span instead of pinned to the center pixel.
 *
 * @param span Visible air-frequency span in hertz.
 * @return Guard in hertz.
 */
static double single_zero_if_guard_for_span(double span)
{
    (void)span;
    return SINGLE_ZERO_IF_GUARD_HZ;
}

/**
 * @brief Source span required to show a view while keeping zero IF outside it.
 *
 * @param span Visible air-frequency span in hertz.
 * @return Required single-frequency source span in hertz.
 */
static double single_required_source_span(double span)
{
    double guard = single_zero_if_guard_for_span(span);
    if (!isfinite(span) || span <= 0.0)
        return guard * 2.0;
    return span + guard * 2.0;
}

/**
 * @brief Choose the smallest strict-bandwidth Pluto RF profile for a span.
 *
 * @param span Visible air-frequency span in hertz.
 * @return Sample-rate/RF-bandwidth pair where RF bandwidth is less than sample
 *         rate and the usable bandwidth covers the span with margin.
 */
static pluto_rf_profile_t single_profile_for_span(double span)
{
    double required = single_required_source_span(span);
    pluto_rf_profile_t profile =
        g_single_profiles[sizeof(g_single_profiles) / sizeof(g_single_profiles[0]) - 1U];

    for (size_t i = 0; i < sizeof(g_single_profiles) / sizeof(g_single_profiles[0]); i++) {
        double usable = g_single_profiles[i].samplerate;
        if (g_single_profiles[i].rf_bandwidth < usable)
            usable = g_single_profiles[i].rf_bandwidth;
        if (required <= usable) {
            profile = g_single_profiles[i];
            break;
        }
    }
    return profile;
}

/**
 * apply_pluto_auto_rf_profile:
 * Keep Pluto RF transport settings deterministic. Scan/hop mode benefits from
 * the highest sample rate because it only captures short buffers per hop, and
 * single-frequency mode uses FFT/CIC planning below to keep resolution
 * coefficients independent from the requested waterfall line rate. The UI
 * displays these values but does not let users change them.
 */
static void apply_pluto_auto_rf_profile(void)
{
    g_samplerate = PLUTO_AUTO_SAMPLE_RATE_HZ;
    g_rf_bandwidth = PLUTO_AUTO_RF_BANDWIDTH_HZ;
    g_bw_ratio = PLUTO_AUTO_BW_RATIO;
}

static uint32_t normalize_rate_limit_lps(uint32_t value)
{
    static const uint32_t allowed[] = { 1, 2, 5, 10, 20, 50, 100 };
    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
        if (value == allowed[i])
            return value;
    }
    return 20;
}

static uint32_t normalize_min_rate_lps(uint32_t value)
{
    static const uint32_t allowed[] = { 0, 1, 2, 5, 10, 20 };
    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
        if (value == allowed[i])
            return value;
    }
    return 0;
}

static uint32_t normalize_direct_sampling(uint32_t value)
{
    (void)value;
    return 0;
}

static int direct_sampling_enabled(void)
{
    return 0;
}

static double direct_sampling_max_hz(void)
{
    return 0.0;
}

static void force_direct_sampling_defaults(int reset_visible)
{
    (void)reset_visible;
    g_direct_sampling = 0;
}

/* ------------------------------------------------------------------ */
/* Persistent config                                                   */
/* ------------------------------------------------------------------ */
#define CONFIG_FILE "pluto-scanner.conf"

static void clamp_visible_to_config(void);
static int clamp_configured_band_to_receiver_limits(void);
static void clamp_scan_end_to_hardware_limit(void);
static int receiver_frequency_valid(double rx_freq);
static char *trim_ws(char *s);

static void copy_cstr(char *dst, size_t dst_len, const char *src)
{
    size_t n;

    if (!dst || dst_len == 0)
        return;
    if (!src)
        src = "";
    n = strlen(src);
    if (n >= dst_len)
        n = dst_len - 1;
    if (n > 0)
        memcpy(dst, src, n);
    dst[n] = '\0';
}

static double normalize_goto_delay_s(double value)
{
    static const double allowed[] = {0.2, 0.5, 1.0, 2.0, 3.0, 5.0};
    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
        if (fabs(value - allowed[i]) < 1e-9)
            return allowed[i];
    }
    return 2.0;
}

static double normalize_goto_target_zoom(double value)
{
    if (!isfinite(value))
        return GOTO_TARGET_ZOOM_MAX;
    if (value < GOTO_TARGET_ZOOM_MIN)
        return GOTO_TARGET_ZOOM_MIN;
    if (value > GOTO_TARGET_ZOOM_MAX)
        return GOTO_TARGET_ZOOM_MAX;
    return value;
}

static void save_config(void)
{
    FILE *f = fopen(CONFIG_FILE, "w");
    if (!f) return;
    fprintf(f, "freq_start = " JSON_COORD_FMT "\n", g_freq_start);
    fprintf(f, "freq_end = " JSON_COORD_FMT "\n", g_freq_end);
    fprintf(f, "converter_freq = " JSON_COORD_FMT "\n", g_converter_freq);
    fprintf(f, "samplerate = %.0f\n", g_samplerate);
    fprintf(f, "rf_bandwidth = %.0f\n", g_rf_bandwidth);
    fprintf(f, "bw_ratio = %g\n", g_bw_ratio);
    fprintf(f, "visible_start = " JSON_COORD_FMT "\n", g_visible_start);
    fprintf(f, "visible_end = " JSON_COORD_FMT "\n", g_visible_end);
    fprintf(f, "gain_mode = %s\n", g_gain_mode);
    fprintf(f, "hardwaregain_db = %.2f\n", g_hardwaregain_db);
    fprintf(f, "rf_port = %s\n", g_rf_port);
    fprintf(f, "pluto_uri = %s\n", g_pluto_uri);
    fprintf(f, "fft_size = %d\n", g_fft_size);
    fprintf(f, "min_rate_lps = %u\n", g_min_rate_lps);
    fprintf(f, "rate_limit_lps = %u\n", g_rate_limit_lps);
    fprintf(f, "fq_err_correction = %u\n", g_fq_err_correction);
    fprintf(f, "goto_freq = " JSON_COORD_FMT "\n", g_goto_freq);
    fprintf(f, "goto_target_zoom = %.6g\n", g_goto_target_zoom);
    fprintf(f, "goto_animate = %u\n", g_goto_animate);
    fprintf(f, "goto_delay_s = %g\n", g_goto_delay_s);
    fclose(f);
}

static void load_config(void)
{
    FILE *f = fopen(CONFIG_FILE, "r");
    int have_visible_start = 0;
    int have_visible_end = 0;
    if (!f) return;
    char line[512];
    double val;
    unsigned int uval;
    while (fgets(line, sizeof(line), f)) {
        char *trimmed = trim_ws(line);
        char *eq;
        char *key;
        char *value;
        char *comment;

        if (!*trimmed || *trimmed == '#')
            continue;
        eq = strchr(trimmed, '=');
        if (!eq)
            continue;
        *eq = 0;
        key = trim_ws(trimmed);
        value = trim_ws(eq + 1);
        comment = strchr(value, '#');
        if (comment) {
            *comment = 0;
            value = trim_ws(value);
        }
        if (!*key || !*value)
            continue;
        val = strtod(value, NULL);
        if      (strcmp(key, "freq_start") == 0)       g_freq_start = val;
        else if (strcmp(key, "freq_end") == 0)         g_freq_end = val;
        else if (strcmp(key, "converter_freq") == 0)   g_converter_freq = val;
        else if (strcmp(key, "samplerate") == 0)       g_samplerate = val;
        else if (strcmp(key, "rf_bandwidth") == 0)     g_rf_bandwidth = val;
        else if (strcmp(key, "bw_ratio") == 0)         g_bw_ratio = val;
        else if (strcmp(key, "visible_start") == 0)   { g_visible_start = val; have_visible_start = 1; }
        else if (strcmp(key, "visible_end") == 0)     { g_visible_end = val; have_visible_end = 1; }
        else if (strcmp(key, "gain_mode") == 0)        copy_cstr(g_gain_mode, sizeof(g_gain_mode), value);
        else if (strcmp(key, "hardwaregain_db") == 0)  g_hardwaregain_db = val;
        else if (strcmp(key, "rf_port") == 0)          copy_cstr(g_rf_port, sizeof(g_rf_port), value);
        else if (strcmp(key, "pluto_uri") == 0)        copy_cstr(g_pluto_uri, sizeof(g_pluto_uri), value);
        else if (strcmp(key, "direct_sampling") == 0) { g_direct_sampling = 0; }
        else if (strcmp(key, "fft_size") == 0) { update_fft_size((int)val); }
        else if (strcmp(key, "min_rate_lps") == 0) { uval = (unsigned int)val; g_min_rate_lps = normalize_min_rate_lps(uval); }
        else if (strcmp(key, "rate_limit_lps") == 0) { uval = (unsigned int)val; g_rate_limit_lps = normalize_rate_limit_lps(uval); }
        else if (strcmp(key, "fq_err_correction") == 0) {
            if (isfinite(val))
                g_fq_err_correction = val != 0.0 ? 1U : 0U;
        }
        else if (strcmp(key, "goto_freq") == 0)      { if (val > 0.0) g_goto_freq = val; }
        else if (strcmp(key, "goto_target_zoom") == 0) { g_goto_target_zoom = normalize_goto_target_zoom(val); }
        else if (strcmp(key, "goto_animate") == 0)   { uval = (unsigned int)val; g_goto_animate = uval ? 1 : 0; }
        else if (strcmp(key, "goto_delay_s") == 0)   { g_goto_delay_s = normalize_goto_delay_s(val); }
    }
    fclose(f);
    if (!have_visible_start || !have_visible_end) {
        g_visible_start = g_freq_start;
        g_visible_end = g_freq_end;
    }
    apply_pluto_auto_rf_profile();
    if (g_hardwaregain_db < PLUTO_GAIN_MIN_DB)
        g_hardwaregain_db = PLUTO_GAIN_MIN_DB;
    if (g_hardwaregain_db > PLUTO_GAIN_MAX_DB)
        g_hardwaregain_db = PLUTO_GAIN_MAX_DB;
    if (strcmp(g_gain_mode, "manual") != 0 &&
        strcmp(g_gain_mode, "slow_attack") != 0 &&
        strcmp(g_gain_mode, "fast_attack") != 0 &&
        strcmp(g_gain_mode, "hybrid") != 0)
        copy_cstr(g_gain_mode, sizeof(g_gain_mode), "manual");
    g_direct_sampling = 0;
    if (direct_sampling_enabled())
        force_direct_sampling_defaults(!have_visible_start || !have_visible_end);
    else {
        clamp_configured_band_to_receiver_limits();
        clamp_scan_end_to_hardware_limit();
        clamp_visible_to_config();
    }
    printf("[SDR] Loaded config from %s\n", CONFIG_FILE);
}

/* ------------------------------------------------------------------ */
/* File reader                                                        */
/* ------------------------------------------------------------------ */
typedef enum {
    FILE_READ_OK = 0,
    FILE_READ_MISSING,
    FILE_READ_EMPTY,
    FILE_READ_IO,
    FILE_READ_MEMORY
} file_read_status_t;

static const char *file_read_status_text(file_read_status_t status)
{
    switch (status) {
    case FILE_READ_OK: return "ok";
    case FILE_READ_MISSING: return "missing";
    case FILE_READ_EMPTY: return "empty";
    case FILE_READ_IO: return "io_error";
    case FILE_READ_MEMORY: return "out_of_memory";
    }
    return "unknown";
}

static file_read_status_t read_file_ex(const char *path, char **out_buf, size_t *out_len)
{
    FILE *f;
    long len;
    size_t got;
    char *buf;

    if (out_buf)
        *out_buf = NULL;
    if (out_len)
        *out_len = 0;

    f = fopen(path, "rb");
    if (!f)
        return errno == ENOENT ? FILE_READ_MISSING : FILE_READ_IO;

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return FILE_READ_IO;
    }
    len = ftell(f);
    if (len < 0) {
        fclose(f);
        return FILE_READ_IO;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return FILE_READ_IO;
    }
    if (len == 0) {
        fclose(f);
        return FILE_READ_EMPTY;
    }

    buf = (char *)malloc((size_t)len + 1);
    if (!buf) {
        fclose(f);
        return FILE_READ_MEMORY;
    }
    got = fread(buf, 1, (size_t)len, f);
    if (got != (size_t)len && ferror(f)) {
        free(buf);
        fclose(f);
        return FILE_READ_IO;
    }
    buf[got] = 0;
    fclose(f);

    if (out_buf)
        *out_buf = buf;
    else
        free(buf);
    if (out_len)
        *out_len = got;
    return FILE_READ_OK;
}

static char *read_file(const char *path, size_t *out_len)
{
    char *buf = NULL;
    file_read_status_t status = read_file_ex(path, &buf, out_len);
    return status == FILE_READ_OK ? buf : NULL;
}

static void send_json_response(int client_fd, int code, const char *reason,
                               const char *cors, const char *body);

typedef struct {
    char name[MARKER_NAME_MAX];
    char group[MARKER_NAME_MAX];
    double frequency_hz;
} marker_t;

static char *trim_ws(char *s)
{
    char *end;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
        s++;
    end = s + strlen(s);
    while (end > s &&
           (end[-1] == ' ' || end[-1] == '\t' ||
            end[-1] == '\r' || end[-1] == '\n')) {
        *--end = 0;
    }
    return s;
}

static void copy_marker_text(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0)
        return;
    if (!src)
        src = "";
    snprintf(dst, dst_size, "%s", src);
}

static int marker_group_exists(char groups[][MARKER_NAME_MAX], int count, const char *group)
{
    for (int i = 0; i < count; i++) {
        if (strcmp(groups[i], group) == 0)
            return 1;
    }
    return 0;
}

static void add_marker_group(char groups[][MARKER_NAME_MAX], int *count, const char *group)
{
    const char *name = (group && *group) ? group : "Ungrouped";
    if (*count >= MAX_MARKERS || marker_group_exists(groups, *count, name))
        return;
    copy_marker_text(groups[*count], MARKER_NAME_MAX, name);
    (*count)++;
}

static int load_markers(marker_t *markers, int max_markers,
                        char groups[][MARKER_NAME_MAX], int *group_count)
{
    size_t len = 0;
    char *text = read_file(MARKERS_PATH, &len);
    char *saveptr = NULL;
    char *line;
    marker_t current;
    int have_marker = 0;
    int count = 0;

    *group_count = 0;
    add_marker_group(groups, group_count, "Ungrouped");

    if (!text)
        return 0;

    memset(&current, 0, sizeof(current));
    copy_marker_text(current.group, sizeof(current.group), "Ungrouped");

    for (line = strtok_r(text, "\n", &saveptr);
         line;
         line = strtok_r(NULL, "\n", &saveptr)) {
        char *trimmed = trim_ws(line);
        char *eq;

        if (!*trimmed || *trimmed == '#' || *trimmed == ';')
            continue;

        if (trimmed[0] == '[') {
            if (have_marker && current.frequency_hz > 0.0 && current.name[0] && count < max_markers) {
                markers[count++] = current;
                add_marker_group(groups, group_count, current.group);
            }
            memset(&current, 0, sizeof(current));
            copy_marker_text(current.group, sizeof(current.group), "Ungrouped");
            have_marker = 1;
            continue;
        }

        eq = strchr(trimmed, '=');
        if (!eq)
            continue;
        *eq = 0;
        {
            char *key = trim_ws(trimmed);
            char *value = trim_ws(eq + 1);
            if (strcmp(key, "name") == 0) {
                copy_marker_text(current.name, sizeof(current.name), value);
            } else if (strcmp(key, "group") == 0) {
                copy_marker_text(current.group, sizeof(current.group), value);
                if (!current.group[0])
                    copy_marker_text(current.group, sizeof(current.group), "Ungrouped");
            } else if (strcmp(key, "frequency_hz") == 0) {
                current.frequency_hz = atof(value);
            } else if (strcmp(key, "frequency_mhz") == 0) {
                current.frequency_hz = atof(value) * 1.0e6;
            }
        }
    }

    if (have_marker && current.frequency_hz > 0.0 && current.name[0] && count < max_markers) {
        markers[count++] = current;
        add_marker_group(groups, group_count, current.group);
    }

    free(text);
    return count;
}

static int marker_text_is_safe(const char *value)
{
    const unsigned char *p = (const unsigned char *)value;
    while (*p) {
        if (*p < 0x20 && *p != '\t')
            return 0;
        p++;
    }
    return 1;
}

static int validate_markers_text(const char *body, size_t body_len,
                                 char *err, size_t err_len)
{
    char *copy;
    char *line;
    char *saveptr = NULL;
    int in_section = 0;
    int have_name = 0;
    int have_freq = 0;
    int marker_count = 0;

    if (body_len > MAX_REQUEST) {
        snprintf(err, err_len, "markers.ini payload is too large");
        return -1;
    }
    copy = (char *)malloc(body_len + 1);
    if (!copy) {
        snprintf(err, err_len, "out of memory validating markers");
        return -1;
    }
    memcpy(copy, body, body_len);
    copy[body_len] = 0;

    for (line = strtok_r(copy, "\n", &saveptr);
         line;
         line = strtok_r(NULL, "\n", &saveptr)) {
        char *trimmed = trim_ws(line);
        char *eq;

        if (!*trimmed || *trimmed == '#' || *trimmed == ';')
            continue;

        if (trimmed[0] == '[') {
            size_t len = strlen(trimmed);
            if (len < 3 || trimmed[len - 1] != ']') {
                snprintf(err, err_len, "invalid marker section header");
                free(copy);
                return -1;
            }
            if (in_section && (!have_name || !have_freq)) {
                snprintf(err, err_len, "marker section missing name or frequency");
                free(copy);
                return -1;
            }
            marker_count++;
            if (marker_count > MAX_MARKERS) {
                snprintf(err, err_len, "too many markers");
                free(copy);
                return -1;
            }
            in_section = 1;
            have_name = 0;
            have_freq = 0;
            continue;
        }

        if (!in_section) {
            snprintf(err, err_len, "marker key outside section");
            free(copy);
            return -1;
        }

        eq = strchr(trimmed, '=');
        if (!eq) {
            snprintf(err, err_len, "marker line is missing '='");
            free(copy);
            return -1;
        }
        *eq = 0;
        {
            char *key = trim_ws(trimmed);
            char *value = trim_ws(eq + 1);
            char *after = NULL;
            double freq;

            if (!marker_text_is_safe(value)) {
                snprintf(err, err_len, "marker value contains control characters");
                free(copy);
                return -1;
            }

            if (strcmp(key, "name") == 0) {
                if (!*value || strlen(value) >= MARKER_NAME_MAX) {
                    snprintf(err, err_len, "marker name is invalid");
                    free(copy);
                    return -1;
                }
                have_name = 1;
            } else if (strcmp(key, "group") == 0) {
                if (strlen(value) >= MARKER_NAME_MAX) {
                    snprintf(err, err_len, "marker group is too long");
                    free(copy);
                    return -1;
                }
            } else if (strcmp(key, "frequency_hz") == 0 ||
                       strcmp(key, "frequency_mhz") == 0) {
                errno = 0;
                freq = strtod(value, &after);
                if (errno != 0 || after == value || *trim_ws(after) != 0 ||
                    !isfinite(freq) || freq <= 0.0) {
                    snprintf(err, err_len, "marker frequency is invalid");
                    free(copy);
                    return -1;
                }
                have_freq = 1;
            } else {
                snprintf(err, err_len, "unsupported marker key '%s'", key);
                free(copy);
                return -1;
            }
        }
    }

    if (in_section && (!have_name || !have_freq)) {
        snprintf(err, err_len, "marker section missing name or frequency");
        free(copy);
        return -1;
    }

    free(copy);
    return 0;
}

/**
 * @brief Atomically replace a text file using a unique temporary file.
 *
 * The temporary file is created in the target directory with `mkstemp`, then
 * renamed over `path` after the full body has been written and closed.
 *
 * @param path Destination path.
 * @param body Bytes to write; may be NULL when `body_len` is zero.
 * @param body_len Number of bytes to write.
 * @return 0 on success, -1 on write, close, or rename failure.
 */
static int write_file_atomic(const char *path, const char *body, size_t body_len)
{
    char tmp_path[PATH_MAX_CHARS];
    int fd;
    FILE *f;
    int n;

    if (!path || !*path || (!body && body_len > 0))
        return -1;
    n = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.XXXXXX", path);
    if (n < 0 || (size_t)n >= sizeof(tmp_path))
        return -1;
    fd = mkstemp(tmp_path);
    if (fd < 0)
        return -1;
    f = fdopen(fd, "wb");
    if (!f) {
        close(fd);
        unlink(tmp_path);
        return -1;
    }
    if (body_len > 0 && fwrite(body, 1, body_len, f) != body_len) {
        fclose(f);
        unlink(tmp_path);
        return -1;
    }
    if (fclose(f) != 0) {
        unlink(tmp_path);
        return -1;
    }
    if (rename(tmp_path, path) != 0) {
        unlink(tmp_path);
        return -1;
    }
    return 0;
}

static int append_json_escaped(char *dst, size_t dst_size, int pos, const char *src)
{
    const unsigned char *s = (const unsigned char *)(src ? src : "");
    if (!dst || dst_size == 0)
        return 0;
    if (pos < 0)
        pos = 0;
    if ((size_t)pos >= dst_size)
        pos = (int)dst_size - 1;
    while (*s && (size_t)pos < dst_size - 1) {
        unsigned char c = *s++;
        if (c == '"' || c == '\\') {
            if ((size_t)pos + 2 >= dst_size)
                break;
            dst[pos++] = '\\';
            dst[pos++] = (char)c;
        } else if (c < 0x20) {
            int n;
            if ((size_t)pos + 6 >= dst_size)
                break;
            n = snprintf(dst + pos, dst_size - (size_t)pos, "\\u%04x", c);
            if (n != 6)
                break;
            pos += n;
        } else {
            dst[pos++] = (char)c;
        }
    }
    dst[pos] = 0;
    return pos;
}

static void send_markers_json(int client_fd, const char *cors)
{
    marker_t *markers = NULL;
    char (*groups)[MARKER_NAME_MAX] = NULL;
    int group_count = 0;
    size_t body_size = 1048576;
    char *body = malloc(body_size);
    int marker_count;
    int pos;

    markers = malloc((size_t)MAX_MARKERS * sizeof(*markers));
    groups = malloc((size_t)MAX_MARKERS * sizeof(*groups));
    if (!markers || !groups || !body) {
        free(markers);
        free(groups);
        free(body);
        send_json_response(client_fd, 500, "Internal Server Error", cors,
                           "{\"status\":\"error\"}");
        return;
    }

    marker_count = load_markers(markers, MAX_MARKERS, groups, &group_count);
    pos = snprintf(body, body_size, "{\"status\":\"ok\",\"groups\":[");
    for (int i = 0; i < group_count; i++) {
        pos += snprintf(body + pos, body_size - (size_t)pos, "%s\"", i ? "," : "");
        pos = append_json_escaped(body, body_size, pos, groups[i]);
        pos += snprintf(body + pos, body_size - (size_t)pos, "\"");
    }
    pos += snprintf(body + pos, body_size - (size_t)pos, "],\"markers\":[");
    for (int i = 0; i < marker_count; i++) {
        pos += snprintf(body + pos, body_size - (size_t)pos,
                        "%s{\"id\":%d,\"frequency_hz\":%.0f,\"name\":\"",
                        i ? "," : "", i, markers[i].frequency_hz);
        pos = append_json_escaped(body, body_size, pos, markers[i].name);
        pos += snprintf(body + pos, body_size - (size_t)pos, "\",\"group\":\"");
        pos = append_json_escaped(body, body_size, pos, markers[i].group);
        pos += snprintf(body + pos, body_size - (size_t)pos, "\"}");
    }
    pos += snprintf(body + pos, body_size - (size_t)pos, "]}");
    send_json_response(client_fd, 200, "OK", cors, body);
    free(markers);
    free(groups);
    free(body);
}

static void url_decode(char *s)
{
    char *r = s;
    while (*s) {
        if (*s == '%' && s[1] && s[2]) {
            char hex[3] = {s[1], s[2], 0};
            *r++ = (char)strtol(hex, NULL, 16); s += 3;
        } else if (*s == '+') { *r++ = ' '; s++; }
        else { *r++ = *s++; }
    }
    *r = 0;
}

static void chdir_to_executable_dir(const char *argv0)
{
    char path[PATH_MAX_CHARS];
    char *slash;

#ifdef _WIN32
    if (argv0 && (strchr(argv0, '/') || strchr(argv0, '\\'))) {
        snprintf(path, sizeof(path), "%s", argv0);
    } else {
        DWORD n = GetModuleFileNameA(NULL, path, (DWORD)sizeof(path));
        if (n == 0 || n >= sizeof(path))
            return;
        path[n] = 0;
    }
    for (char *p = path; *p; p++) {
        if (*p == '\\')
            *p = '/';
    }
#else
    if (argv0 && strchr(argv0, '/')) {
        snprintf(path, sizeof(path), "%s", argv0);
    } else {
        ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
        if (n <= 0)
            return;
        path[n] = 0;
    }
#endif

    slash = strrchr(path, '/');
    if (!slash)
        return;
    if (slash == path)
        slash[1] = 0;
    else
        *slash = 0;
    if (chdir(path) != 0)
        fprintf(stderr, "[SDR] chdir(%s) failed: %s\n", path, strerror(errno));
}

static int ascii_case_equal_n(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (tolower(ca) != tolower(cb))
            return 0;
    }
    return 1;
}

static int http_content_length_n(const char *req, size_t len, int *out_len)
{
    const char *p = req;
    const char *end = req + len;

    if (out_len)
        *out_len = 0;

    while (p < end) {
        const char *line_end = NULL;
        const char *colon;
        size_t line_len;

        for (const char *q = p; q + 1 < end; q++) {
            if (q[0] == '\r' && q[1] == '\n') {
                line_end = q;
                break;
            }
        }
        if (!line_end)
            return -1;
        if (line_end == p)
            return 0;

        line_len = (size_t)(line_end - p);
        colon = memchr(p, ':', line_len);
        if (colon) {
            size_t name_len = (size_t)(colon - p);
            if (name_len == strlen("Content-Length") &&
                ascii_case_equal_n(p, "Content-Length", name_len)) {
                const char *v = colon + 1;
                char *after = NULL;
                long parsed;
                while (v < line_end && isspace((unsigned char)*v))
                    v++;
                errno = 0;
                parsed = strtol(v, &after, 10);
                if (errno != 0 || after == v || parsed < 0 || parsed > MAX_REQUEST)
                    return -1;
                while (after < line_end && isspace((unsigned char)*after))
                    after++;
                if (after != line_end)
                    return -1;
                if (out_len)
                    *out_len = (int)parsed;
                return 0;
            }
        }
        p = line_end + 2;
    }

    return -1;
}

static int http_content_length(const char *req)
{
    const char *header_end = strstr(req, "\r\n\r\n");
    int len = 0;
    if (!header_end)
        return 0;
    if (http_content_length_n(req, (size_t)(header_end - req) + 4, &len) != 0)
        return -1;
    return len;
}

/**
 * @brief Normalize a user supplied libiio URI or bare Pluto host.
 *
 * Values that already contain a libiio URI scheme, such as `ip:host`, `usb:`,
 * or `local:`, are copied unchanged. Bare hostnames and IPv4 addresses are
 * converted to `ip:<value>` so command-line values like `192.168.2.1` work.
 *
 * @param src Input URI, hostname, or IPv4 address.
 * @param dst Destination buffer for the normalized URI.
 * @param dst_len Destination buffer length in bytes.
 */
static void normalize_pluto_uri(const char *src, char *dst, size_t dst_len)
{
    const char *value = (src && *src) ? src : PLUTO_DEFAULT_URI;

    if (!dst || dst_len == 0)
        return;

    if (strchr(value, ':')) {
        copy_cstr(dst, dst_len, value);
    } else {
        snprintf(dst, dst_len, "ip:%s", value);
    }
}

/**
 * @brief Return the active normalized libiio URI.
 *
 * `PLUTO_URI` has highest priority, followed by `--uri`, saved config, and the
 * compiled default. Bare host/IP values are normalized to the `ip:` backend.
 *
 * @return Pointer to a process-lifetime URI string.
 */
static const char *pluto_context_uri(void)
{
    static char env_uri[128];
    const char *env = getenv("PLUTO_URI");
    if (env && *env) {
        normalize_pluto_uri(env, env_uri, sizeof(env_uri));
        return env_uri;
    }
    return g_pluto_uri[0] ? g_pluto_uri : PLUTO_DEFAULT_URI;
}

static void trim_iio_string(char *s)
{
    size_t n;
    if (!s)
        return;
    n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' '))
        s[--n] = 0;
}

static void pluto_read_channel_string(struct iio_channel *ch, const char *attr,
                                      char *dst, size_t dst_len)
{
    ssize_t n;
    if (!dst || dst_len == 0)
        return;
    dst[0] = 0;
    if (!ch)
        return;
    n = iio_channel_attr_read(ch, attr, dst, dst_len - 1);
    if (n < 0)
        return;
    if ((size_t)n >= dst_len)
        n = (ssize_t)dst_len - 1;
    dst[n] = 0;
    trim_iio_string(dst);
}

static void pluto_read_device_string(struct iio_device *dev, const char *attr,
                                     char *dst, size_t dst_len)
{
    ssize_t n;
    if (!dst || dst_len == 0)
        return;
    dst[0] = 0;
    if (!dev)
        return;
    n = iio_device_attr_read(dev, attr, dst, dst_len - 1);
    if (n < 0)
        return;
    if ((size_t)n >= dst_len)
        n = (ssize_t)dst_len - 1;
    dst[n] = 0;
    trim_iio_string(dst);
}

static void pluto_update_readbacks(pluto_sdr_dev_t *dev)
{
    long long ll = 0;
    double dd = 0.0;

    if (!dev)
        return;
    if (iio_channel_attr_read_longlong(dev->phy_rx0, "sampling_frequency", &ll) == 0)
        dev->sample_rate = (double)ll;
    if (iio_channel_attr_read_longlong(dev->phy_rx0, "rf_bandwidth", &ll) == 0)
        dev->rf_bandwidth = (double)ll;
    if (iio_channel_attr_read_double(dev->phy_rx0, "hardwaregain", &dd) == 0)
        dev->hardwaregain_db = dd;
    if (iio_channel_attr_read_double(dev->phy_rx0, "rssi", &dd) == 0)
        dev->rssi_db = dd;
    pluto_read_channel_string(dev->phy_rx0, "gain_control_mode",
                              dev->gain_mode, sizeof(dev->gain_mode));
    pluto_read_channel_string(dev->phy_rx0, "rf_port_select",
                              dev->rf_port, sizeof(dev->rf_port));
    pluto_read_device_string(dev->phy, "rx_path_rates",
                             dev->rx_path_rates, sizeof(dev->rx_path_rates));
    g_last_rssi_db = dev->rssi_db;
    g_last_input_peak = dev->input_peak;
    g_last_clipped_samples = dev->clipped_samples;
}

static int pluto_sdr_get_device_count(void)
{
    struct iio_context *ctx;
    int ok;

    ctx = iio_create_context_from_uri(pluto_context_uri());
    if (!ctx)
        return 0;
    ok = iio_context_find_device(ctx, "ad9361-phy") != NULL &&
        iio_context_find_device(ctx, "cf-ad9361-lpc") != NULL;
    iio_context_destroy(ctx);
    return ok ? 1 : 0;
}

static int pluto_sdr_open(pluto_sdr_dev_t **out, int index)
{
    pluto_sdr_dev_t *dev;
    const char *uri;

    if (!out || index != 0)
        return -1;
    uri = pluto_context_uri();
    dev = calloc(1, sizeof(*dev));
    if (!dev)
        return -1;
    copy_cstr(dev->uri, sizeof(dev->uri), uri);
    copy_cstr(dev->gain_mode, sizeof(dev->gain_mode), g_gain_mode);
    copy_cstr(dev->rf_port, sizeof(dev->rf_port), g_rf_port);
    dev->sample_rate = g_samplerate;
    dev->rf_bandwidth = g_rf_bandwidth;
    dev->hardwaregain_db = g_hardwaregain_db;
    dev->rssi_db = -999.0;

    dev->ctx = iio_create_context_from_uri(uri);
    if (!dev->ctx)
        goto fail;
    (void)iio_context_set_timeout(dev->ctx, 2000);
    copy_cstr(dev->context_name, sizeof(dev->context_name),
              iio_context_get_name(dev->ctx) ? iio_context_get_name(dev->ctx) : "unknown");
    copy_cstr(dev->context_description, sizeof(dev->context_description),
              iio_context_get_description(dev->ctx) ? iio_context_get_description(dev->ctx) : "unknown");

    dev->phy = iio_context_find_device(dev->ctx, "ad9361-phy");
    dev->rxdev = iio_context_find_device(dev->ctx, "cf-ad9361-lpc");
    if (!dev->phy || !dev->rxdev)
        goto fail;
    dev->phy_rx0 = iio_device_find_channel(dev->phy, "voltage0", false);
    dev->rx_lo = iio_device_find_channel(dev->phy, "altvoltage0", true);
    dev->rx_i = iio_device_find_channel(dev->rxdev, "voltage0", false);
    dev->rx_q = iio_device_find_channel(dev->rxdev, "voltage1", false);
    if (!dev->phy_rx0 || !dev->rx_lo || !dev->rx_i || !dev->rx_q)
        goto fail;

    pluto_update_readbacks(dev);
    *out = dev;
    return PLUTO_ERR_OK;

fail:
    if (dev->rxbuf)
        iio_buffer_destroy(dev->rxbuf);
    if (dev->ctx)
        iio_context_destroy(dev->ctx);
    free(dev);
    return -1;
}

static void pluto_sdr_close(pluto_sdr_dev_t *dev)
{
    if (!dev)
        return;
    if (dev->rxbuf)
        iio_buffer_destroy(dev->rxbuf);
    if (dev->ctx)
        iio_context_destroy(dev->ctx);
    free(dev->raw_i);
    free(dev->raw_q);
    free(dev->float_buf);
    free(dev);
}

static void pluto_sdr_get_api_info(char *lib_ver, char *drv_ver)
{
    unsigned int major = 0;
    unsigned int minor = 0;
    char git_tag[16] = {0};

    iio_library_get_version(&major, &minor, git_tag);
    if (lib_ver)
        snprintf(lib_ver, 64, "libiio %u.%u%s%s", major, minor,
                 git_tag[0] ? " " : "", git_tag);
    if (drv_ver)
        snprintf(drv_ver, 64, "ad9361 IIO");
}

/**
 * read_trimmed_sysfs_file:
 * Read a small sysfs attribute into `dst`, stripping newline and space.
 * Returns 0 only when the file exists and contains a non-empty value.
 */
#ifndef _WIN32
static int read_trimmed_sysfs_file(const char *path, char *dst, size_t dst_len)
{
    FILE *f;

    if (!path || !dst || dst_len == 0)
        return -1;
    dst[0] = 0;
    f = fopen(path, "r");
    if (!f)
        return -1;
    if (!fgets(dst, (int)dst_len, f)) {
        fclose(f);
        dst[0] = 0;
        return -1;
    }
    fclose(f);
    trim_iio_string(dst);
    return dst[0] ? 0 : -1;
}
#endif

/**
 * pluto_read_usb_iserial:
 * Try to read the host USB iSerial descriptor from sysfs. This only works for
 * a locally attached Pluto USB gadget; network-only libiio contexts do not
 * expose the host USB descriptor. IIO device `serial`/`boardid` remains the
 * portable fallback below.
 */
static int pluto_read_usb_iserial(char *serial, size_t serial_len)
{
#ifdef _WIN32
    if (!serial || serial_len == 0)
        return -1;
    serial[0] = 0;
    return -1;
#else
    DIR *dir;
    struct dirent *entry;
    int found = -1;

    if (!serial || serial_len == 0)
        return -1;
    serial[0] = 0;

    dir = opendir("/sys/bus/usb/devices");
    if (!dir)
        return -1;

    while ((entry = readdir(dir)) != NULL) {
        char path[PATH_MAX_CHARS];
        char vendor[32] = {0};
        char product[128] = {0};
        char serial_tmp[128] = {0};
        int n;

        if (entry->d_name[0] == '.')
            continue;

        n = snprintf(path, sizeof(path), "/sys/bus/usb/devices/%s/idVendor",
                     entry->d_name);
        if (n < 0 || (size_t)n >= sizeof(path))
            continue;
        if (read_trimmed_sysfs_file(path, vendor, sizeof(vendor)) != 0)
            continue;
        if (strcmp(vendor, "0456") != 0)
            continue;

        n = snprintf(path, sizeof(path), "/sys/bus/usb/devices/%s/product",
                     entry->d_name);
        if (n < 0 || (size_t)n >= sizeof(path))
            continue;
        if (read_trimmed_sysfs_file(path, product, sizeof(product)) != 0)
            product[0] = 0;
        if (strstr(product, "Pluto") == NULL && strstr(product, "PLUTO") == NULL)
            continue;

        n = snprintf(path, sizeof(path), "/sys/bus/usb/devices/%s/serial",
                     entry->d_name);
        if (n < 0 || (size_t)n >= sizeof(path))
            continue;
        if (read_trimmed_sysfs_file(path, serial_tmp, sizeof(serial_tmp)) == 0) {
            copy_cstr(serial, serial_len, serial_tmp);
            found = 0;
            break;
        }
    }

    closedir(dir);
    return found;
#endif
}

static int pluto_sdr_get_board_info(pluto_sdr_dev_t *dev, char *hw_rev, char *fw_ver,
                                    char *manufacturer, char *product, char *serial)
{
    if (!dev)
        return -1;
    if (hw_rev) {
        if (dev->context_description[0])
            copy_cstr(hw_rev, 64, dev->context_description);
        else if (dev->context_name[0])
            copy_cstr(hw_rev, 64, dev->context_name);
        else
            copy_cstr(hw_rev, 64, "unknown");
    }
    if (fw_ver)
        copy_cstr(fw_ver, 64, dev->context_name[0] ? dev->context_name : "unknown");
    if (manufacturer)
        copy_cstr(manufacturer, 64, "Analog Devices");
    if (product)
        copy_cstr(product, 64, "ADALM-Pluto");
    if (serial) {
        if (pluto_read_usb_iserial(serial, 64) == 0)
            return PLUTO_ERR_OK;
        pluto_read_device_string(dev->phy, "serial", serial, 64);
        if (serial[0] == '\0')
            pluto_read_device_string(dev->phy, "boardid", serial, 64);
        if (serial[0] == '\0')
            copy_cstr(serial, 64, dev->uri);
    }
    return PLUTO_ERR_OK;
}

static int pluto_sdr_get_samplerates(pluto_sdr_dev_t *dev, double *rates, unsigned int *count)
{
    static const double defaults[] = {
        4000000.0, 8000000.0, 16000000.0, 20000000.0,
        30720000.0, 40000000.0, 61440000.0
    };
    unsigned int n = (unsigned int)(sizeof(defaults) / sizeof(defaults[0]));
    (void)dev;
    if (!rates || !count)
        return -1;
    if (*count > 0 && *count < n)
        n = *count;
    for (unsigned int i = 0; i < n; i++)
        rates[i] = defaults[i];
    *count = n;
    return PLUTO_ERR_OK;
}

static int pluto_sdr_cancel_async(pluto_sdr_dev_t *dev)
{
    if (!dev)
        return PLUTO_ERR_OK;
    dev->cancel_requested = 1;
    if (dev->rxbuf) {
        iio_buffer_cancel(dev->rxbuf);
        dev->buffer_cancelled = 1;
    }
    return PLUTO_ERR_OK;
}

static int pluto_sdr_set_samplerate(pluto_sdr_dev_t *dev, double rate)
{
    int rc;
    if (!dev)
        return -1;
    rc = iio_channel_attr_write_longlong(dev->phy_rx0, "sampling_frequency", (long long)llround(rate));
    if (rc < 0)
        return rc;
    dev->sample_rate = rate;
    return PLUTO_ERR_OK;
}

static int pluto_sdr_set_bandwidth(pluto_sdr_dev_t *dev, double bw)
{
    int rc;
    if (!dev)
        return -1;
    rc = iio_channel_attr_write_longlong(dev->phy_rx0, "rf_bandwidth", (long long)llround(bw));
    if (rc < 0)
        return rc;
    dev->rf_bandwidth = bw;
    return PLUTO_ERR_OK;
}

static int pluto_sdr_set_gain_mode(pluto_sdr_dev_t *dev, const char *mode)
{
    int rc;
    if (!dev || !mode || !*mode)
        return -1;
    rc = (int)iio_channel_attr_write(dev->phy_rx0, "gain_control_mode", mode);
    if (rc < 0)
        return rc;
    copy_cstr(dev->gain_mode, sizeof(dev->gain_mode), mode);
    return PLUTO_ERR_OK;
}

/**
 * @brief Select the AD936x receive input port.
 *
 * The value is written through the stock `rf_port_select` IIO channel
 * attribute. The selected port takes effect on the next stream start.
 *
 * @param dev Open Pluto device.
 * @param port AD936x port name, such as `A_BALANCED`.
 * @return `PLUTO_ERR_OK` on success, otherwise the negative IIO error.
 */
static int pluto_sdr_set_rf_port(pluto_sdr_dev_t *dev, const char *port)
{
    int rc;

    if (!dev || !port || !*port)
        return -1;
    rc = (int)iio_channel_attr_write(dev->phy_rx0, "rf_port_select", port);
    if (rc < 0)
        return rc;
    copy_cstr(dev->rf_port, sizeof(dev->rf_port), port);
    return PLUTO_ERR_OK;
}

static int pluto_sdr_set_hardwaregain(pluto_sdr_dev_t *dev, double gain_db)
{
    int rc;
    if (!dev)
        return -1;
    if (gain_db < PLUTO_GAIN_MIN_DB)
        gain_db = PLUTO_GAIN_MIN_DB;
    if (gain_db > PLUTO_GAIN_MAX_DB)
        gain_db = PLUTO_GAIN_MAX_DB;
    rc = iio_channel_attr_write_double(dev->phy_rx0, "hardwaregain", gain_db);
    if (rc < 0)
        return rc;
    dev->hardwaregain_db = gain_db;
    return PLUTO_ERR_OK;
}

static int pluto_sdr_set_frequency(pluto_sdr_dev_t *dev, double frequency)
{
    int rc;
    if (!dev)
        return -1;
    for (int attempt = 0; attempt < PLUTO_TUNE_RETRY_COUNT; attempt++) {
        rc = iio_channel_attr_write_longlong(dev->rx_lo, "frequency",
                                             (long long)llround(frequency));
        if (rc >= 0)
            return PLUTO_ERR_OK;
        if (!g_scanning || dev->cancel_requested)
            return PLUTO_ERR_OK;
        {
            struct timespec ts;
            ts.tv_sec = 0;
            ts.tv_nsec = PLUTO_TUNE_RETRY_DELAY_MS * 1000000L;
            nanosleep(&ts, NULL);
        }
    }
    if (!g_scanning || dev->cancel_requested)
        return PLUTO_ERR_OK;
    return rc;
}

static int pluto_sdr_stop_scan(pluto_sdr_dev_t *dev)
{
    if (!dev)
        return PLUTO_ERR_OK;
    dev->scan_count = 0;
    dev->scan_index = 0;
    return PLUTO_ERR_OK;
}

static int pluto_sdr_start_scan(pluto_sdr_dev_t *dev, const double *freqs, unsigned int count)
{
    if (!dev || !freqs || count == 0 || count > MAX_FREQS)
        return -1;
    memcpy(dev->scan_freqs, freqs, (size_t)count * sizeof(double));
    dev->scan_count = count;
    dev->scan_index = 0;
    return PLUTO_ERR_OK;
}

static int pluto_sdr_get_scan_index(pluto_sdr_dev_t *dev)
{
    return dev ? dev->scan_index : -1;
}

/**
 * @brief Create an RX buffer with the requested sample and kernel queue depth.
 *
 * Scan/hop mode uses one kernel block for low retune latency. Continuous mode
 * uses several blocks so acquisition can continue while userspace extracts,
 * converts, and queues the preceding block.
 *
 * @param dev Open Pluto device.
 * @param samples Complex samples per IIO refill.
 * @param kernel_buffers Number of kernel capture blocks kept queued.
 * @return `PLUTO_ERR_OK` on success, otherwise a negative error.
 */
static int pluto_sdr_prepare_buffer(pluto_sdr_dev_t *dev, uint32_t samples,
                                    unsigned int kernel_buffers)
{
    int rc;
    size_t bytes;

    if (!dev || samples == 0 || kernel_buffers == 0)
        return -1;
    if (dev->rxbuf && dev->buffer_samples == samples &&
        dev->kernel_buffers == kernel_buffers && !dev->buffer_cancelled)
        return PLUTO_ERR_OK;
    if (dev->rxbuf) {
        iio_buffer_destroy(dev->rxbuf);
        dev->rxbuf = NULL;
    }

    bytes = (size_t)samples * sizeof(int16_t);
    free(dev->raw_i);
    free(dev->raw_q);
    free(dev->float_buf);
    dev->raw_i = malloc(bytes);
    dev->raw_q = malloc(bytes);
    dev->float_buf = malloc((size_t)samples * 2U * sizeof(float));
    if (!dev->raw_i || !dev->raw_q || !dev->float_buf)
        return -1;

    rc = iio_device_set_kernel_buffers_count(dev->rxdev, kernel_buffers);
    if (rc < 0)
        return rc;
    iio_channel_enable(dev->rx_i);
    iio_channel_enable(dev->rx_q);
    dev->rxbuf = iio_device_create_buffer(dev->rxdev, samples, false);
    if (!dev->rxbuf)
        return -1;
    dev->buffer_samples = samples;
    dev->kernel_buffers = kernel_buffers;
    dev->buffer_cancelled = 0;
    return PLUTO_ERR_OK;
}

static int pluto_sdr_refill_float(pluto_sdr_dev_t *dev, uint32_t requested,
                                  float **out_buf, uint32_t *out_len)
{
    ssize_t nbytes;
    ssize_t got_i;
    ssize_t got_q;
    size_t samples;
    double peak = 0.0;
    uint64_t clipped = 0;

    if (!dev || !dev->rxbuf || requested == 0)
        return -1;
    nbytes = iio_buffer_refill(dev->rxbuf);
    if (nbytes < 0)
        return (int)nbytes;
    got_i = (ssize_t)iio_channel_read_raw(dev->rx_i, dev->rxbuf,
                                          dev->raw_i, (size_t)requested * sizeof(int16_t));
    got_q = (ssize_t)iio_channel_read_raw(dev->rx_q, dev->rxbuf,
                                          dev->raw_q, (size_t)requested * sizeof(int16_t));
    if (got_i <= 0 || got_q <= 0)
        return -1;
    samples = (size_t)(got_i / (ssize_t)sizeof(int16_t));
    if ((size_t)(got_q / (ssize_t)sizeof(int16_t)) < samples)
        samples = (size_t)(got_q / (ssize_t)sizeof(int16_t));
    if (samples > requested)
        samples = requested;
    for (size_t i = 0; i < samples; i++) {
        double ai = fabs((double)dev->raw_i[i]);
        double aq = fabs((double)dev->raw_q[i]);
        if (ai > peak)
            peak = ai;
        if (aq > peak)
            peak = aq;
        if (dev->raw_i[i] >= 2047 || dev->raw_i[i] <= -2048 ||
            dev->raw_q[i] >= 2047 || dev->raw_q[i] <= -2048)
            clipped++;
        dev->float_buf[2 * i] = (float)dev->raw_i[i] / 2048.0f;
        dev->float_buf[2 * i + 1] = (float)dev->raw_q[i] / 2048.0f;
    }
    dev->input_peak = peak;
    dev->clipped_samples = clipped;
    g_last_input_peak = dev->input_peak;
    g_last_clipped_samples = dev->clipped_samples;
    if (out_buf)
        *out_buf = dev->float_buf;
    if (out_len)
        *out_len = (uint32_t)samples;
    return PLUTO_ERR_OK;
}

static int pluto_sdr_recreate_buffer(pluto_sdr_dev_t *dev, uint32_t samples,
                                     unsigned int kernel_buffers)
{
    if (!dev)
        return -1;
    if (dev->rxbuf) {
        iio_buffer_destroy(dev->rxbuf);
        dev->rxbuf = NULL;
    }
    dev->buffer_samples = 0;
    dev->kernel_buffers = 0;
    dev->buffer_cancelled = 0;
    return pluto_sdr_prepare_buffer(dev, samples, kernel_buffers);
}

static int pluto_sdr_refill_float_retry(pluto_sdr_dev_t *dev, uint32_t requested,
                                        float **out_buf, uint32_t *out_len,
                                        int *out_recovered)
{
    int ret = -1;

    if (out_recovered)
        *out_recovered = 0;
    for (int attempt = 0; attempt < 3; attempt++) {
        ret = pluto_sdr_refill_float(dev, requested, out_buf, out_len);
        if (ret == PLUTO_ERR_OK) {
            if (out_recovered && attempt > 0)
                *out_recovered = 1;
            return PLUTO_ERR_OK;
        }
        if (!g_scanning || !dev || dev->cancel_requested)
            return PLUTO_ERR_OK;
        if (pluto_sdr_recreate_buffer(dev, requested,
                                      dev->kernel_buffers ? dev->kernel_buffers :
                                      PLUTO_SCAN_KERNEL_BUFFERS) != PLUTO_ERR_OK)
            return ret;
    }

    return ret;
}

typedef void (*pluto_sdr_async_cb_t)(float *buf, uint32_t buf_len,
                                     uint32_t refill_flags,
                                     pluto_sdr_dev_t *dev, void *user);

static void pluto_sleep_after_tune(void)
{
    if (PLUTO_SETTLE_DELAY_US > 0) {
        struct timespec ts;
        ts.tv_sec = PLUTO_SETTLE_DELAY_US / 1000000U;
        ts.tv_nsec = (long)(PLUTO_SETTLE_DELAY_US % 1000000U) * 1000L;
        nanosleep(&ts, NULL);
    }
}

static void pluto_sleep_msec(long delay_ms)
{
    struct timespec ts;

    if (delay_ms <= 0)
        return;
    ts.tv_sec = delay_ms / 1000L;
    ts.tv_nsec = (delay_ms % 1000L) * 1000000L;
    nanosleep(&ts, NULL);
}

static int pluto_sdr_read_async(pluto_sdr_dev_t *dev, pluto_sdr_async_cb_t cb,
                                void *user, unsigned int kernel_buffers,
                                uint32_t async_len)
{
    if (!dev || !cb || async_len == 0 || kernel_buffers == 0)
        return -1;
    if (pluto_sdr_prepare_buffer(dev, async_len, kernel_buffers) != PLUTO_ERR_OK)
        return -1;
    dev->cancel_requested = 0;

    while (g_scanning && !dev->cancel_requested) {
        unsigned int count = dev->scan_count ? dev->scan_count : 1U;
        for (unsigned int i = 0; i < count && g_scanning && !dev->cancel_requested; i++) {
            float *buf = NULL;
            uint32_t got = 0;
            int ret = PLUTO_ERR_OK;

            if (dev->scan_count) {
                ret = pluto_sdr_set_frequency(dev, dev->scan_freqs[i]);
                if (ret != PLUTO_ERR_OK)
                    return ret;
                if (!g_scanning || dev->cancel_requested)
                    break;
                dev->scan_index = (int)i;
                pluto_sleep_after_tune();
                for (unsigned int d = 0;
                     d < PLUTO_DISCARD_COUNT && g_scanning && !dev->cancel_requested;
                     d++) {
                    ret = pluto_sdr_refill_float_retry(dev, async_len, NULL, NULL, NULL);
                    if (ret != PLUTO_ERR_OK) {
                        if (!g_scanning || dev->cancel_requested)
                            return PLUTO_ERR_OK;
                        return ret;
                    }
                }
            }

            if (!g_scanning || dev->cancel_requested)
                break;
            {
                int recovered = 0;
                uint32_t flags = 0;
                ret = pluto_sdr_refill_float_retry(dev, async_len, &buf, &got, &recovered);
                if (recovered)
                    flags |= PLUTO_REFILL_FLAG_RECOVERED;
                if (got > 0 && got != async_len)
                    flags |= PLUTO_REFILL_FLAG_SHORT_READ;
                if (ret != PLUTO_ERR_OK) {
                    if (!g_scanning || dev->cancel_requested)
                        return PLUTO_ERR_OK;
                    return ret;
                }
                if (got > 0)
                    cb(buf, got, flags, dev, user);
            }
        }
    }
    return PLUTO_ERR_OK;
}

#if !PSEUDO_RANDOM_SAMPLE_SOURCE
static void load_sample_rates(struct pluto_sdr_dev_t *dev)
{
    unsigned int count = 0;

    g_sample_rate_count = 0;
    if (pluto_sdr_get_samplerates(dev, g_sample_rates, &count) != PLUTO_ERR_OK)
        return;

    if (count > MAX_SAMPLE_RATES)
        count = MAX_SAMPLE_RATES;
    g_sample_rate_count = count;
}
#endif

static void format_sample_rates_json(char *buf, size_t len)
{
    size_t pos = 0;

    if (len == 0)
        return;

    pos += (size_t)snprintf(buf + pos, len - pos, "[");
    for (unsigned int i = 0; i < g_sample_rate_count && pos < len; i++) {
        int n = snprintf(buf + pos, len - pos, "%s%.0f",
                         (i ? "," : ""), g_sample_rates[i]);
        if (n < 0)
            break;
        pos += (size_t)n;
    }
    if (pos < len)
        snprintf(buf + pos, len - pos, "]");
    else
        buf[len - 1] = 0;
}

static int normalize_display_bins(int bins);
static int current_display_bins(void);
static double estimated_single_stream_sps_for_line_samples(uint32_t line_sample_count);
static single_fft_plan_t single_fft_plan_for_span(double span);
static int scan_effective_fft_size_for_span(double span, int selected_fft_size);
static int planned_required_points(void);
static run_mode_t planned_run_mode(void);
static double current_active_samplerate(void);
static void clamp_visible_to_config(void);
static void raw_visible_band(double *out_start, double *out_end);
static void active_scan_band(double *out_start, double *out_end);
static void send_json_response(int client_fd, int code, const char *reason,
                               const char *cors, const char *body);
#if !PSEUDO_RANDOM_SAMPLE_SOURCE
static void load_sample_rates(struct pluto_sdr_dev_t *dev);
#endif
static long long now_msec(void);

static void reset_async_cancel_request(void)
{
    pthread_mutex_lock(&g_cancel_mutex);
    g_cancel_requested = 0;
    pthread_mutex_unlock(&g_cancel_mutex);
}

static void request_async_cancel(void)
{
    pthread_mutex_lock(&g_cancel_mutex);
    if (!g_cancel_requested) {
        g_cancel_requested = 1;
#if !PSEUDO_RANDOM_SAMPLE_SOURCE
        if (g_dev)
            pluto_sdr_cancel_async(g_dev);
#endif
    }
    pthread_mutex_unlock(&g_cancel_mutex);
}

static int async_cancel_requested(void)
{
    int requested;

    pthread_mutex_lock(&g_cancel_mutex);
    requested = g_cancel_requested;
    pthread_mutex_unlock(&g_cancel_mutex);
    return requested;
}

static void record_frontend_traffic(size_t bytes)
{
    if (bytes == 0)
        return;

    pthread_mutex_lock(&g_traffic_mutex);
    g_traffic_samples[g_traffic_sample_pos].msec = now_msec();
    g_traffic_samples[g_traffic_sample_pos].bytes = bytes;
    g_traffic_sample_pos = (g_traffic_sample_pos + 1) % TRAFFIC_SAMPLE_COUNT;
    pthread_mutex_unlock(&g_traffic_mutex);
}

static double measured_frontend_kbytes_s(void)
{
    long long now = now_msec();
    long long cutoff = now - TRAFFIC_WINDOW_MS;
    long long oldest = now;
    size_t bytes = 0;

    pthread_mutex_lock(&g_traffic_mutex);
    for (int i = 0; i < TRAFFIC_SAMPLE_COUNT; i++) {
        if (g_traffic_samples[i].msec >= cutoff) {
            bytes += g_traffic_samples[i].bytes;
            if (g_traffic_samples[i].msec < oldest)
                oldest = g_traffic_samples[i].msec;
        }
    }
    pthread_mutex_unlock(&g_traffic_mutex);

    if (bytes == 0)
        return 0.0;

    {
        long long elapsed = now - oldest;
        if (elapsed < 1000)
            elapsed = 1000;
        if (elapsed > TRAFFIC_WINDOW_MS)
            elapsed = TRAFFIC_WINDOW_MS;
        return ((double)bytes / 1024.0) * (1000.0 / (double)elapsed);
    }
}

static void reset_device_info(void)
{
    copy_cstr(g_hw_rev, sizeof(g_hw_rev), "unknown");
    copy_cstr(g_fw_ver, sizeof(g_fw_ver), "unknown");
    copy_cstr(g_serial, sizeof(g_serial), "unknown");
    copy_cstr(g_manufacturer, sizeof(g_manufacturer), "unknown");
    copy_cstr(g_product, sizeof(g_product), "unknown");
    g_sample_rate_count = 0;
}

static void close_device(void)
{
    if (!g_dev)
        return;
    pluto_sdr_close(g_dev);
    g_dev = NULL;
    reset_device_info();
}

static int open_first_device(int verbose)
{
#if PSEUDO_RANDOM_SAMPLE_SOURCE
    (void)verbose;
    copy_cstr(g_hw_rev, sizeof(g_hw_rev), "pseudo");
    copy_cstr(g_fw_ver, sizeof(g_fw_ver),
              PSEUDO_RANDOM_SAMPLE_SOURCE == SYNTHETIC_TONE_SAMPLE_SOURCE ?
              "synthetic-tone" : "pseudo-random");
    copy_cstr(g_serial, sizeof(g_serial), "pseudo");
    copy_cstr(g_manufacturer, sizeof(g_manufacturer), "local");
    copy_cstr(g_product, sizeof(g_product),
              PSEUDO_RANDOM_SAMPLE_SOURCE == SYNTHETIC_TONE_SAMPLE_SOURCE ?
              "synthetic tone source" : "pseudo-random source");
    g_sample_rates[0] = SCANNER_SAMPLE_RATE_HZ;
    g_sample_rate_count = 1;
    if (verbose)
        printf("[SDR] Using %s instead of Pluto SDR hardware\n",
               PSEUDO_RANDOM_SAMPLE_SOURCE == SYNTHETIC_TONE_SAMPLE_SOURCE ?
               "synthetic tone source" : "pseudo-random sample source");
    return PLUTO_ERR_OK;
#else
    int count;
    int ret;

    if (g_dev)
        return PLUTO_ERR_OK;

    count = pluto_sdr_get_device_count();
    if (verbose)
        printf("[SDR] Devices found: %d\n", count);
    if (count <= 0) {
        if (verbose)
            printf("[SDR] No device connected.\n");
        return -1;
    }

    ret = pluto_sdr_open(&g_dev, 0);
    if (ret != PLUTO_ERR_OK) {
        if (verbose)
            printf("[SDR] Could not open device: %d\n", ret);
        g_dev = NULL;
        reset_device_info();
        return ret;
    }

    ret = pluto_sdr_get_board_info(g_dev, g_hw_rev, g_fw_ver,
                                   g_manufacturer, g_product, g_serial);
    if (ret != PLUTO_ERR_OK && verbose)
        printf("[SDR] Could not read board info: %d\n", ret);
    load_sample_rates(g_dev);
    if (verbose)
        printf("[SDR] Pluto hardware: %s %s\n", g_manufacturer, g_product);
    printf("[SDR] Pluto connected: software %s, serial %s\n",
           g_fw_ver, g_serial);
    return PLUTO_ERR_OK;
#endif
}

/* ------------------------------------------------------------------ */
/* Hardware scan context                                              */
/* ------------------------------------------------------------------ */
typedef struct {
    float *samples;
    uint32_t buf_len;
    uint32_t flags;
    uint64_t block_sequence;
    uint64_t sample_index_start;
    uint64_t sample_index_end;
    int channel;
    int ready;
} sample_queue_item_t;

typedef struct {
    int total_steps;
    int bins_per_step;
    int last_bins;
    int line_bins;
    int steps_seen;
    int line_num;
    int fft_size;
    int selected_fft_size;
    int decim_factor;
    int decim_fill;
    int decim_phase;
    int decim_hop;
    double overlap_factor;
    int async_buf_len;
    int single_mode;
    int rate_drop_factor;
    int rate_drop_cycle;
    int rate_have_cycle;
    int max_ffts_per_buffer;
    uint32_t rate_limit_lps;
    double rate_keep_ratio;
    double rate_keep_credit;
    double estimated_line_rate;
    int minimum_rate_limited;
    int minimum_rate_achieved;
    double visible_raw_bins;
    double visible_bins_per_pixel;
    uint64_t rate_callback_seq;
    uint64_t rate_scan_cycle_seq;
    uint64_t rate_output_seq;
    uint64_t rate_dropped;
    uint64_t rate_output_dropped;
    uint64_t cic_raw_samples_in;
    uint64_t cic_decim_samples_out;
    uint64_t cic_fft_frames_completed;
    uint64_t cic_account_raw_samples;
    uint64_t cic_account_decim_samples;
    uint64_t cic_account_fft_frames;
    uint64_t cic_account_warmup_outputs;
    uint64_t cic_continuity_errors;
    uint64_t cic_discontinuities;
    uint64_t cic_short_reads;
    uint64_t cic_refill_recoveries;
    uint64_t cic_reset_frames_skipped;
    uint64_t cic_sample_order_errors;
    int cic_skip_frames_after_reset;
    int cic_warmup_outputs_remaining;
    uint32_t pending_refill_flags;
    uint64_t capture_block_sequence;
    uint64_t capture_sample_next;
    uint64_t worker_expected_block_sequence;
    uint64_t worker_expected_sample_index;
    int worker_sequence_initialized;
    long long last_publish_msec;
    uint64_t tuning_skipped;
    volatile long long last_callback_msec;
    volatile int watchdog_stop;
    volatile int watchdog_triggered;
    uint32_t fft_generation;
    double configured_start;
    double configured_end;
    double visible_start;
    double visible_end;
    double scan_start;
    double scan_end;
    double samplerate;
    double raw_samplerate;
    double bw_ratio;
    double step_width;
    double last_width;
    /* Modeled actual-minus-requested source-center offset, in hertz. */
    double frequency_correction_hz;
    double center_freq;
    double second_if_hz;
    double zero_if_guard_hz;
    uint32_t view_id;
    int preview_mode;
    long long preview_age_ms;
    int preview_sequence;
    int preview_count;
    /* Planned display pacing for cached preview rows, in milliseconds. */
    double preview_interval_ms;
    /* Monotonic live-capture timing, used only for transition diagnostics. */
    long long live_capture_started_msec;
    long long live_first_input_msec;
    long long live_first_fft_msec;
    long long live_first_publish_msec;
    uint32_t direct_sampling;
    double direct_mix_phase;
    double direct_mix_inc;
    /* Display-only white-noise density factor; coherent FFT data is unchanged. */
    float waterfall_noise_scale;
    float mag_scale;
    float *window;
    float *fft_scratch;
    float *decim_accum;
    float *line_buf;
    double *cic_delay;
    double cic_sum_re[CIC_STAGES];
    double cic_sum_im[CIC_STAGES];
    double cic_dc_gain;
    int cic_delay_pos;
#if PSEUDO_RANDOM_SAMPLE_SOURCE == SYNTHETIC_TONE_SAMPLE_SOURCE
    double synthetic_tone_hz;
    uint64_t synthetic_spectral_checks;
    uint64_t synthetic_spectral_failures;
#endif
    uint8_t step_seen[MAX_FREQS];
    sample_queue_item_t queue[PROCESS_QUEUE_LEN];
    int queue_head;
    int queue_tail;
    int queue_len;
    int worker_stop;
    uint64_t queue_dropped;
    uint64_t cic_queue_waits;
    pthread_mutex_t queue_mutex;
    pthread_cond_t queue_cond;
    pthread_t worker_thread;
} scan_ctx_t;

/** Invalidate cached raw history at a known hardware-stream discontinuity. */
static void recent_sample_cache_invalidate(void)
{
    pthread_mutex_lock(&g_recent_samples_mutex);
    g_recent_samples.count = 0;
    g_recent_samples.write_pos = 0;
    g_recent_samples.valid = 0;
    g_recent_samples.continuity_generation++;
    pthread_mutex_unlock(&g_recent_samples_mutex);
}

/**
 * @brief Start collecting one new contiguous single-frequency sample stream.
 *
 * @param center_hz Air-frequency source center in hertz.
 * @param source_start_hz Air-frequency lower cache coverage edge in hertz.
 * @param source_end_hz Air-frequency upper cache coverage edge in hertz.
 * @param samplerate_hz Raw complex sample rate in samples/s.
 * @param rf_bandwidth_hz Pluto RF bandwidth in hertz.
 */
static void recent_sample_cache_begin(double center_hz,
                                      double source_start_hz,
                                      double source_end_hz,
                                      double samplerate_hz,
                                      double rf_bandwidth_hz)
{
    pthread_mutex_lock(&g_recent_samples_mutex);
    if (!g_recent_samples.samples) {
        g_recent_samples.capacity = CACHED_PREVIEW_MAX_SAMPLES;
        g_recent_samples.samples = malloc(g_recent_samples.capacity * 2U *
                                          sizeof(float));
    }
    g_recent_samples.count = 0;
    g_recent_samples.write_pos = 0;
    g_recent_samples.source_center_hz = center_hz;
    g_recent_samples.source_start_hz = source_start_hz;
    g_recent_samples.source_end_hz = source_end_hz;
    g_recent_samples.samplerate_hz = samplerate_hz;
    g_recent_samples.rf_bandwidth_hz = rf_bandwidth_hz;
    g_recent_samples.updated_msec = 0;
    g_recent_samples.continuity_generation++;
    g_recent_samples.valid = g_recent_samples.samples != NULL;
    pthread_mutex_unlock(&g_recent_samples_mutex);
}

/**
 * @brief Append raw float I/Q to the recent contiguous sample ring.
 *
 * A flagged refill starts a new cache segment before its samples are appended.
 * The cache never claims continuity across a retry or short read.
 *
 * @param ctx Active single-frequency capture context.
 * @param samples Interleaved complex float samples.
 * @param sample_count Complex sample count.
 * @param refill_flags Discontinuity flags associated with this refill.
 */
static void recent_sample_cache_append(const scan_ctx_t *ctx,
                                       const float *samples,
                                       uint32_t sample_count,
                                       uint32_t refill_flags)
{
    size_t source_offset = 0;
    size_t append_count;
    size_t first_count;

    if (!ctx || !ctx->single_mode || ctx->direct_sampling || !samples ||
        sample_count == 0)
        return;

    pthread_mutex_lock(&g_recent_samples_mutex);
    if (!g_recent_samples.valid || !g_recent_samples.samples ||
        fabs(g_recent_samples.source_center_hz - ctx->center_freq) > 1.0 ||
        fabs(g_recent_samples.samplerate_hz - ctx->raw_samplerate) > 1.0) {
        pthread_mutex_unlock(&g_recent_samples_mutex);
        return;
    }
    if (refill_flags != 0) {
        g_recent_samples.count = 0;
        g_recent_samples.write_pos = 0;
        g_recent_samples.continuity_generation++;
    }
    if ((size_t)sample_count > g_recent_samples.capacity)
        source_offset = (size_t)sample_count - g_recent_samples.capacity;
    append_count = (size_t)sample_count - source_offset;
    first_count = g_recent_samples.capacity - g_recent_samples.write_pos;
    if (first_count > append_count)
        first_count = append_count;
    memcpy(g_recent_samples.samples + g_recent_samples.write_pos * 2U,
           samples + source_offset * 2U,
           first_count * 2U * sizeof(float));
    if (append_count > first_count) {
        memcpy(g_recent_samples.samples,
               samples + (source_offset + first_count) * 2U,
               (append_count - first_count) * 2U * sizeof(float));
    }
    g_recent_samples.write_pos =
        (g_recent_samples.write_pos + append_count) % g_recent_samples.capacity;
    if (g_recent_samples.count + append_count >= g_recent_samples.capacity)
        g_recent_samples.count = g_recent_samples.capacity;
    else
        g_recent_samples.count += append_count;
    g_recent_samples.updated_msec = now_msec();
    pthread_mutex_unlock(&g_recent_samples_mutex);
}

/**
 * @brief Snapshot one compatible historical line for immediate preview.
 *
 * The returned samples are chronological and digitally shifted from the old
 * source center to the new source center. The caller owns the returned buffer.
 *
 * @param ctx New single-frequency context and requested RF interval.
 * @param rf_bandwidth_hz New Pluto RF bandwidth in hertz.
 * @param needed_samples Raw complex samples required including CIC warm-up.
 * @param out_age_ms Cache age in milliseconds on success.
 * @return Allocated interleaved float I/Q, or NULL when not compatible.
 */
static float *recent_sample_cache_snapshot(const scan_ctx_t *ctx,
                                           double rf_bandwidth_hz,
                                           uint32_t needed_samples,
                                           long long *out_age_ms)
{
    float *snapshot = NULL;
    long long age_ms;
    size_t start;
    double shift_hz;
    double coverage_guard_hz;
    double phase = 0.0;
    double phase_inc;
    const char *reject_reason = NULL;

    if (out_age_ms)
        *out_age_ms = 0;
    if (!ctx || !ctx->single_mode || ctx->direct_sampling ||
        needed_samples == 0 || needed_samples > CACHED_PREVIEW_MAX_SAMPLES)
        return NULL;

    pthread_mutex_lock(&g_recent_samples_mutex);
    age_ms = now_msec() - g_recent_samples.updated_msec;
    coverage_guard_hz = fmax(
        (ctx->visible_end - ctx->visible_start) * 0.05,
        ctx->raw_samplerate / (double)(ctx->fft_size > 0 ? ctx->fft_size : 1) * 2.0);
    if (!g_recent_samples.valid || !g_recent_samples.samples)
        reject_reason = "no contiguous raw cache";
    else if (g_recent_samples.count < (size_t)needed_samples)
        reject_reason = "insufficient raw samples";
    else if (g_recent_samples.updated_msec <= 0 || age_ms < 0 ||
             age_ms > CACHED_PREVIEW_MAX_AGE_MS)
        reject_reason = "raw cache is stale";
    else if (fabs(g_recent_samples.samplerate_hz - ctx->raw_samplerate) > 1.0)
        reject_reason = "raw sample rate changed";
    else if (fabs(g_recent_samples.rf_bandwidth_hz - rf_bandwidth_hz) > 1.0)
        reject_reason = "RF bandwidth changed";
    else if (ctx->visible_start - coverage_guard_hz <
             g_recent_samples.source_start_hz - 1.0 ||
             ctx->visible_end + coverage_guard_hz >
             g_recent_samples.source_end_hz + 1.0)
        reject_reason = "target view exceeds raw RF coverage";
    if (reject_reason) {
        pthread_mutex_unlock(&g_recent_samples_mutex);
        return NULL;
    }

    snapshot = malloc((size_t)needed_samples * 2U * sizeof(float));
    if (!snapshot) {
        pthread_mutex_unlock(&g_recent_samples_mutex);
        return NULL;
    }
    start = (g_recent_samples.write_pos + g_recent_samples.capacity -
             (size_t)needed_samples) % g_recent_samples.capacity;
    for (size_t n = 0; n < (size_t)needed_samples; n++) {
        size_t source = ((start + n) % g_recent_samples.capacity) * 2U;
        snapshot[n * 2U] = g_recent_samples.samples[source];
        snapshot[n * 2U + 1U] = g_recent_samples.samples[source + 1U];
    }
    shift_hz = g_recent_samples.source_center_hz - ctx->center_freq;
    pthread_mutex_unlock(&g_recent_samples_mutex);

    phase_inc = ctx->raw_samplerate > 0.0 ?
        2.0 * M_PI * shift_hz / ctx->raw_samplerate : 0.0;
    for (size_t n = 0; n < (size_t)needed_samples; n++) {
        float re = snapshot[n * 2U];
        float im = snapshot[n * 2U + 1U];
        double c = cos(phase);
        double s = sin(phase);
        snapshot[n * 2U] = (float)((double)re * c - (double)im * s);
        snapshot[n * 2U + 1U] = (float)((double)re * s + (double)im * c);
        phase += phase_inc;
        if (phase > M_PI || phase < -M_PI)
            phase = fmod(phase, 2.0 * M_PI);
    }
    if (out_age_ms)
        *out_age_ms = age_ms;
    fprintf(stderr,
            "[SDR] Cached preview accepted: %u raw samples, age %lld ms, shift %.6f Hz\n",
            needed_samples, age_ms, shift_hz);
    return snapshot;
}

static int build_scan_frequencies_for_band(double start, double end, double *freqs, double *out_step)
{
    double step = scan_step_hz();
    double span = end - start;
    int count;

    if (out_step) *out_step = 0.0;
    if (step <= 0.0 || span <= 0.0) return 0;

    count = (int)ceil(span / step);
    if (count > MAX_FREQS)
        count = MAX_FREQS;

    if (out_step) *out_step = step;

    if (freqs) {
        for (int i = 0; i < count; i++) {
            double slice_start = start + (double)i * step;
            double width = end - slice_start;
            if (width > step)
                width = step;
            freqs[i] = slice_start + width / 2.0;
        }
    }

    return count;
}

static int build_scan_frequencies(double *freqs, double *out_step)
{
    double start;
    double end;
    active_scan_band(&start, &end);
    return build_scan_frequencies_for_band(start, end, freqs, out_step);
}

static double build_scan_effective_end_for_band(double start, double end_limit, int count, double step)
{
    if (count <= 0 || step <= 0.0)
        return start;
    double end = start + (double)count * step;
    return (end < end_limit) ? end : end_limit;
}

static double build_scan_effective_end(int count, double step)
{
    double start;
    double end;
    active_scan_band(&start, &end);
    return build_scan_effective_end_for_band(start, end, count, step);
}

static double scan_last_width_for_band(double start, double end, int count, double step)
{
    double last_start;
    double width;

    if (count <= 0 || step <= 0.0)
        return 0.0;

    last_start = start + (double)(count - 1) * step;
    width = end - last_start;
    if (width > step)
        width = step;
    if (width <= 0.0)
        width = step;
    return width;
}

static void clamp_scan_end_to_hardware_limit(void)
{
    double step = scan_step_hz();
    double max_end;

    if (direct_sampling_enabled()) {
        force_direct_sampling_defaults(1);
        return;
    }

    if (clamp_configured_band_to_receiver_limits() != 0)
        return;

    if (step <= 0.0 || g_freq_end <= g_freq_start)
        return;

    max_end = g_freq_start + (double)MAX_FREQS * step;
    if (g_freq_end > max_end)
        g_freq_end = max_end;
    clamp_visible_to_config();
}

static int current_scan_plan(double *out_step, double *out_freq_end)
{
    double step = 0.0;
    int total_steps;

    if (planned_run_mode() == RUN_MODE_SINGLE) {
        double start;
        double end;
        single_fft_plan_t plan;
        raw_visible_band(&start, &end);
        plan = single_fft_plan_for_span(end - start);
        step = plan.source_span;
        total_steps = 1;
        if (out_step)
            *out_step = step;
        if (out_freq_end)
            *out_freq_end = end;
        return total_steps;
    }

    total_steps = build_scan_frequencies(NULL, &step);

    if (out_step)
        *out_step = step;
    if (out_freq_end)
        *out_freq_end = build_scan_effective_end(total_steps, step);
    return total_steps;
}

static int build_device_scan_frequencies(const double *air_freqs, int count, double *device_freqs)
{
    for (int i = 0; i < count; i++) {
        if (g_converter_freq >= 0.0)
            device_freqs[i] = air_freqs[i] - g_converter_freq;
        else
            device_freqs[i] = fabs(-g_converter_freq - air_freqs[i]);
        if (!receiver_frequency_valid(device_freqs[i]))
            return -1;
    }
    return 0;
}

static double receiver_frequency_from_radio(double air_freq)
{
    if (g_converter_freq >= 0.0)
        return air_freq - g_converter_freq;
    return fabs(-g_converter_freq - air_freq);
}

static int receiver_frequency_valid(double rx_freq)
{
    return rx_freq >= RF_RECEIVER_MIN_HZ && rx_freq <= RF_RECEIVER_MAX_HZ;
}

/**
 * @brief Model the quantization error of Pluto's fractional-N RX RFPLL.
 *
 * The supported IIO frequency attribute accepts an integer-Hz RX LO request.
 * Stock Pluto hardware normally derives the RX LO from a 40 MHz reference,
 * first rounds the supported IIO request to an integer hertz. The AD936x Linux
 * clock bridge then represents that rate as `freq >> 1` and reads it back as
 * `freq << 1`, so an odd request is programmed as the preceding even hertz.
 * With the normal 40 MHz Pluto reference, the stock driver doubles the RFPLL
 * parent to 80 MHz. It then selects a power-of-two VCO divider for the 6..12
 * GHz RFPLL range and rounds the fractional numerator to the AD936x
 * `8,388,593` modulus. The driver owns the actual divider and calibration;
 * this is intentionally a display/bin coordinate model, never a replacement
 * for IIO LO control.
 *
 * @param requested_rx_hz Requested Pluto receive LO in hertz.
 * @return Modeled actual-minus-requested receive-LO error in hertz, or zero
 *         when the requested LO cannot be represented by this model.
 */
static double pluto_rfpll_modeled_error_hz(double requested_rx_hz)
{
    long long iio_requested_rx_hz;
    uint64_t driver_requested_rx_hz;
    uint64_t vco_hz;
    uint64_t integer_part;
    uint64_t remainder_hz;
    uint64_t fractional_part;
    uint64_t output_divider;
    long double actual_vco_hz;
    int vco_div = -1;

    if (!isfinite(requested_rx_hz) || requested_rx_hz <= 0.0)
        return 0.0;
    iio_requested_rx_hz = llround(requested_rx_hz);
    if (iio_requested_rx_hz <= 0)
        return 0.0;
    driver_requested_rx_hz =
        (((uint64_t)iio_requested_rx_hz) >> 1U) << 1U;

    vco_hz = driver_requested_rx_hz;
    while (vco_hz <= PLUTO_RFPLL_VCO_MIN_HZ) {
        if (vco_hz > UINT64_MAX / 2U)
            return 0.0;
        vco_hz <<= 1U;
        vco_div++;
    }
    if (vco_div < 0 || vco_hz > PLUTO_RFPLL_VCO_MAX_HZ)
        return 0.0;

    integer_part = vco_hz / PLUTO_RFPLL_REFERENCE_HZ;
    remainder_hz = vco_hz % PLUTO_RFPLL_REFERENCE_HZ;
    fractional_part = (remainder_hz * PLUTO_RFPLL_FRACTIONAL_MODULUS +
                       PLUTO_RFPLL_REFERENCE_HZ / 2U) /
        PLUTO_RFPLL_REFERENCE_HZ;
    output_divider = 1ULL << (unsigned int)(vco_div + 1);
    actual_vco_hz = (long double)PLUTO_RFPLL_REFERENCE_HZ *
        ((long double)integer_part +
         (long double)fractional_part /
         (long double)PLUTO_RFPLL_FRACTIONAL_MODULUS);
    /* Include the same integer-Hz IIO request rounding used by
     * pluto_sdr_set_frequency(), then the fractional-N tuning-word error. */
    return (double)(actual_vco_hz / (long double)output_divider -
                    (long double)requested_rx_hz);
}

/**
 * @brief Convert a receive-LO error into the corresponding air-frequency sign.
 *
 * A positive converter preserves the air-to-receiver slope. A negative
 * converter may invert it on the lower side of its absolute-value mapping.
 *
 * @param air_hz Air/source frequency at the planned receiver center in hertz.
 * @param receiver_error_hz Actual-minus-requested RX LO error in hertz.
 * @return Actual-minus-requested air-frequency source-center error in hertz.
 */
static double air_error_from_receiver_error(double air_hz,
                                            double receiver_error_hz)
{
    if (!isfinite(air_hz) || !isfinite(receiver_error_hz))
        return 0.0;
    if (g_converter_freq >= 0.0)
        return receiver_error_hz;
    return air_hz >= -g_converter_freq ? receiver_error_hz : -receiver_error_hz;
}

/**
 * @brief Return the RFPLL-model source-coordinate correction for one air LO.
 *
 * @param air_hz Requested air/source center frequency in hertz.
 * @return Signed modeled source-center correction in hertz; zero when disabled.
 */
static double modeled_frequency_correction_hz(double air_hz)
{
    double receiver_hz;

    if (!g_fq_err_correction)
        return 0.0;
    receiver_hz = receiver_frequency_from_radio(air_hz);
    return air_error_from_receiver_error(
        air_hz, pluto_rfpll_modeled_error_hz(receiver_hz));
}

/**
 * @brief Return the total display/bin correction for one air-frequency LO.
 *
 * The correction moves the assumed source interval onto the deterministic
 * RFPLL model. It never changes the IIO tuning request, configured air range,
 * converter math, or RFPLL/DDS hardware state.
 *
 * @param air_hz Requested air/source center frequency in hertz.
 * @return Signed source-center correction in hertz.
 */
static double configured_frequency_correction_hz(double air_hz)
{
    return modeled_frequency_correction_hz(air_hz);
}

static int append_air_interval(freq_interval_t *intervals, int count,
                               double start, double end)
{
    if (end <= MIN_FREQ_START_HZ || end <= start || count >= 2)
        return count;
    if (start < MIN_FREQ_START_HZ)
        start = MIN_FREQ_START_HZ;
    if (end > start) {
        intervals[count].start = start;
        intervals[count].end = end;
        count++;
    }
    return count;
}

/**
 * @brief Build valid air-frequency intervals for a converter setting.
 *
 * Frequencies are hertz. For positive converters, receiver frequency is
 * `air - converter`. For negative converters, receiver frequency is
 * `abs(abs(converter) - air)`, which can create two valid air intervals.
 *
 * @param intervals Output array with room for two intervals.
 * @param converter_freq Converter frequency in hertz.
 * @return Number of valid intervals written.
 */
static int air_intervals_for_converter(freq_interval_t *intervals,
                                       double converter_freq)
{
    if (converter_freq >= 0.0) {
        return append_air_interval(intervals, 0,
                                   converter_freq + RF_RECEIVER_MIN_HZ,
                                   converter_freq + RF_RECEIVER_MAX_HZ);
    }

    {
        double conv = -converter_freq;
        int count = 0;
        count = append_air_interval(intervals, count,
                                    conv - RF_RECEIVER_MAX_HZ,
                                    conv - RF_RECEIVER_MIN_HZ);
        count = append_air_interval(intervals, count,
                                    conv + RF_RECEIVER_MIN_HZ,
                                    conv + RF_RECEIVER_MAX_HZ);
        return count;
    }
}

/**
 * @brief Build valid air-frequency intervals for the active converter setting.
 *
 * @param intervals Output array with room for two hertz intervals.
 * @return Number of valid intervals written.
 */
static int air_intervals_for_receiver_limits(freq_interval_t *intervals)
{
    return air_intervals_for_converter(intervals, g_converter_freq);
}

/**
 * @brief Check whether an air-frequency band maps fully inside receiver range.
 *
 * The UI and API accept air/signal frequencies. This validator enforces the
 * Pluto receiver limits only after converter conversion, without rewriting the
 * requested air band.
 *
 * @param start_hz Air-frequency band start in hertz.
 * @param end_hz Air-frequency band end in hertz.
 * @param converter_freq Converter frequency in hertz.
 * @return 1 when the full band is valid, otherwise 0.
 */
static int air_band_within_receiver_limits(double start_hz, double end_hz,
                                           double converter_freq)
{
    freq_interval_t intervals[2];
    int count;

    if (!isfinite(start_hz) || !isfinite(end_hz) ||
        !isfinite(converter_freq) || end_hz <= start_hz)
        return 0;

    count = air_intervals_for_converter(intervals, converter_freq);
    for (int i = 0; i < count; i++) {
        if (start_hz >= intervals[i].start && end_hz <= intervals[i].end)
            return 1;
    }
    return 0;
}

static int clamp_configured_band_to_receiver_limits(void)
{
    freq_interval_t intervals[2];
    double req_start = g_freq_start;
    double req_end = g_freq_end;
    double req_span = req_end - req_start;
    double req_center = (req_start + req_end) * 0.5;
    int count;
    int best = -1;
    double best_overlap = 0.0;

    if (direct_sampling_enabled())
        return 0;

    count = air_intervals_for_receiver_limits(intervals);
    if (count <= 0)
        return -1;

    for (int i = 0; i < count; i++) {
        double overlap_start = req_start > intervals[i].start ?
            req_start : intervals[i].start;
        double overlap_end = req_end < intervals[i].end ?
            req_end : intervals[i].end;
        double overlap = overlap_end - overlap_start;
        if (overlap > best_overlap) {
            best_overlap = overlap;
            best = i;
        }
    }

    if (best >= 0 && best_overlap > 0.0) {
        if (g_freq_start < intervals[best].start)
            g_freq_start = intervals[best].start;
        if (g_freq_end > intervals[best].end)
            g_freq_end = intervals[best].end;
    } else {
        double best_distance = 0.0;
        for (int i = 0; i < count; i++) {
            double distance = 0.0;
            if (req_center < intervals[i].start)
                distance = intervals[i].start - req_center;
            else if (req_center > intervals[i].end)
                distance = req_center - intervals[i].end;
            if (best < 0 || distance < best_distance) {
                best = i;
                best_distance = distance;
            }
        }
        if (best < 0)
            return -1;
        if (req_span <= 0.0 || req_span > intervals[best].end - intervals[best].start)
            req_span = intervals[best].end - intervals[best].start;
        req_center = (req_center < intervals[best].start) ? intervals[best].start :
            (req_center > intervals[best].end ? intervals[best].end : req_center);
        g_freq_start = req_center - req_span * 0.5;
        g_freq_end = g_freq_start + req_span;
        if (g_freq_start < intervals[best].start) {
            g_freq_start = intervals[best].start;
            g_freq_end = g_freq_start + req_span;
        }
        if (g_freq_end > intervals[best].end) {
            g_freq_end = intervals[best].end;
            g_freq_start = g_freq_end - req_span;
        }
    }

    if (g_freq_start < MIN_FREQ_START_HZ)
        g_freq_start = MIN_FREQ_START_HZ;
    if (g_freq_end <= g_freq_start)
        return -1;
    return 0;
}

static int source_band_covers_visible(double center, double source_span,
                                      double visible_start, double visible_end)
{
    double source_start = center - source_span * 0.5;
    double source_end = center + source_span * 0.5;
    return source_start <= visible_start && source_end >= visible_end;
}

static double zero_if_shift_for_visible(double visible_start, double visible_end,
                                        double source_span)
{
    double span = visible_end - visible_start;
    double guard = single_zero_if_guard_for_span(span);
    double max_guard = (source_span - span) * 0.5;
    double outside_shift;

    if (!isfinite(guard) || guard < 0.0)
        guard = 0.0;
    if (!isfinite(max_guard) || max_guard < 0.0)
        max_guard = 0.0;
    outside_shift = span * 0.5 + guard;
    if (max_guard >= outside_shift)
        return outside_shift;
    return max_guard;
}

static double single_source_center_for_visible(double visible_start,
                                               double visible_end,
                                               double source_span)
{
    double visible_center = (visible_start + visible_end) * 0.5;
    double shift = zero_if_shift_for_visible(visible_start, visible_end, source_span);
    double candidates[3];

    candidates[0] = visible_center - shift;
    candidates[1] = visible_center + shift;
    candidates[2] = visible_center;

    for (int i = 0; i < 3; i++) {
        double center = candidates[i];
        if (!source_band_covers_visible(center, source_span, visible_start, visible_end))
            continue;
        if (receiver_frequency_valid(receiver_frequency_from_radio(center)))
            return center;
    }

    return candidates[2];
}

static double direct_source_center_for_visible(double visible_start,
                                               double visible_end,
                                               double source_span)
{
    double direct_max = direct_sampling_max_hz();
    double visible_center = (visible_start + visible_end) * 0.5;
    double shift = zero_if_shift_for_visible(visible_start, visible_end, source_span);
    double candidates[3];

    if (source_span >= direct_max)
        return direct_max * 0.5;

    candidates[0] = visible_center - shift;
    candidates[1] = visible_center + shift;
    candidates[2] = visible_center;

    for (int i = 0; i < 3; i++) {
        double center = candidates[i];
        double source_start = center - source_span * 0.5;
        double source_end = center + source_span * 0.5;
        if (source_start < 0.0 || source_end > direct_max)
            continue;
        if (source_start <= visible_start && source_end >= visible_end)
            return center;
    }

    {
        double center = (visible_start + visible_end) * 0.5;
        double half = source_span * 0.5;
        if (center < half)
            center = half;
        if (center > direct_max - half)
            center = direct_max - half;
        return center;
    }
}

static double second_if_for_center(double visible_start, double visible_end,
                                   double source_center)
{
    double visible_center = (visible_start + visible_end) * 0.5;
    if (!isfinite(source_center))
        return 0.0;
    return fabs(visible_center - source_center);
}

static void raw_visible_band(double *out_start, double *out_end)
{
    clamp_visible_to_config();
    *out_start = g_visible_start;
    *out_end = g_visible_end;
}

static double current_zero_if_guard_hz(void)
{
    double start;
    double end;
    if (planned_run_mode() != RUN_MODE_SINGLE)
        return 0.0;
    raw_visible_band(&start, &end);
    return single_zero_if_guard_for_span(end - start);
}

static double current_second_if_hz(void)
{
    double start;
    double end;
    single_fft_plan_t plan;
    double center;

    if (planned_run_mode() != RUN_MODE_SINGLE)
        return 0.0;
    raw_visible_band(&start, &end);
    if (end <= start)
        return 0.0;
    plan = single_fft_plan_for_span(end - start);
    center = direct_sampling_enabled() ?
        direct_source_center_for_visible(start, end, plan.source_span) :
        single_source_center_for_visible(start, end, plan.source_span);
    center += configured_frequency_correction_hz(center);
    return second_if_for_center(start, end, center);
}

/**
 * @brief Return the current modeled source-center correction for diagnostics.
 *
 * @return Effective signed correction in hertz, or zero outside single mode.
 */
static double current_frequency_correction_hz(void)
{
    double start;
    double end;
    single_fft_plan_t plan;
    double center;

    if (planned_run_mode() != RUN_MODE_SINGLE || direct_sampling_enabled())
        return 0.0;
    raw_visible_band(&start, &end);
    if (end <= start)
        return 0.0;
    plan = single_fft_plan_for_span(end - start);
    center = single_source_center_for_visible(start, end, plan.source_span);
    return configured_frequency_correction_hz(center);
}

/**
 * @brief Return the RFPLL air-coordinate model even when correction is disabled.
 *
 * @return Signed modeled actual-minus-requested source-center error in hertz.
 */
static double current_frequency_model_error_hz(void)
{
    double start;
    double end;
    single_fft_plan_t plan;
    double center;
    double receiver_hz;

    if (planned_run_mode() != RUN_MODE_SINGLE || direct_sampling_enabled())
        return 0.0;
    raw_visible_band(&start, &end);
    if (end <= start)
        return 0.0;
    plan = single_fft_plan_for_span(end - start);
    center = single_source_center_for_visible(start, end, plan.source_span);
    receiver_hz = receiver_frequency_from_radio(center);
    return air_error_from_receiver_error(
        center, pluto_rfpll_modeled_error_hz(receiver_hz));
}

static int required_points_for_band(double start, double end)
{
    double step = scan_step_hz();
    double span = end - start;
    int points;

    if (step <= 0.0 || span <= 0.0)
        return 0;

    points = (int)ceil(span / step);
    if (points < 1)
        points = 1;
    if (points > MAX_FREQS)
        points = MAX_FREQS;
    return points;
}

static int planned_required_points(void)
{
    double start;
    double end;
    if (direct_sampling_enabled())
        return 1;
    raw_visible_band(&start, &end);
    return required_points_for_band(start, end);
}

/**
 * planned_run_mode:
 * Select scan/hop when the visible span needs more than one Pluto passband.
 * Select single mode when one receiver center can cover the visible span.
 */
static run_mode_t planned_run_mode(void)
{
    if (direct_sampling_enabled())
        return RUN_MODE_SINGLE;
    return (planned_required_points() <= 1) ? RUN_MODE_SINGLE : RUN_MODE_SCAN;
}

/**
 * estimated_single_stream_sps_for_line_samples:
 * Estimate complex samples/s delivered by the Pluto single-frequency path.
 *
 * Machine-readable contract:
 * - Input is raw Pluto samples consumed by one published FFT line.
 * - Output is an empirical host-side throughput estimate, not RF sample rate.
 * - Larger libiio buffers are faster per sample on the tested Pluto/network
 *   path, so the estimate is intentionally piecewise instead of constant.
 *
 * Human note:
 * The waterfall limiter compares desired lines/s to host-visible line cadence.
 * Using 61.44 MSPS here would be fictional because the full stream cannot be
 * transferred continuously; using one flat low estimate under-throttles large
 * FFTs. These values are derived from the phase-4 matrix runs and are kept
 * conservative enough to avoid exceeding the configured max line rate.
 */
static double estimated_single_stream_sps_for_line_samples(uint32_t line_sample_count)
{
    (void)line_sample_count;
    return PLUTO_EST_SINGLE_STREAM_SPS;
}

static double estimated_scan_hop_ms_for_samples(uint32_t sample_count)
{
    if (sample_count >= 8192U)
        return 8.9;
    if (sample_count >= 4096U)
        return 6.0;
    if (sample_count >= 2048U)
        return 4.8;
    return PLUTO_EST_HOP_MS;
}

/**
 * fft_window_magnitude_scale:
 * Return the scalar that converts raw FFT magnitude back to input amplitude.
 *
 * Machine-readable contract:
 * - Input window contains the exact coefficients used before the FFT.
 * - Output scale is `1 / sum(window)`, i.e. coherent-gain correction.
 * - FFT size, CIC decimation, and bin width do not alter this scale.
 *
 * Human note:
 * A complex tone centered on an FFT bin has magnitude `amplitude * sum(window)`
 * after windowing. Dividing by that sum keeps the displayed level stable when
 * switching between 2048, 8192, 65536, raw, and CIC-decimated plans. Noise
 * floor can still move with bin bandwidth; signal amplitude should not.
 */
static float fft_window_magnitude_scale(const float *window, int fft_size)
{
    double window_sum = 0.0;

    if (!window || fft_size <= 0)
        return 1.0f;
    for (int i = 0; i < fft_size; i++)
        window_sum += window[i];
    if (window_sum <= 0.0)
        return 1.0f / (float)fft_size;
    return (float)(1.0 / window_sum);
}

/**
 * @brief Calculate the equivalent noise bandwidth of the active FFT window.
 *
 * The returned ENBW is in hertz. Unlike coherent gain, ENBW describes the
 * noise power admitted by one FFT bin: `Fs * sum(w^2) / sum(w)^2`.
 *
 * @param window Exact FFT window coefficients.
 * @param fft_size Number of window/FFT samples.
 * @param samplerate_hz Sample rate at the FFT input in samples/s.
 * @return Positive equivalent noise bandwidth in hertz, or zero on invalid input.
 */
static double fft_window_equivalent_noise_bandwidth(const float *window,
                                                    int fft_size,
                                                    double samplerate_hz)
{
    double sum = 0.0;
    double sum_sq = 0.0;

    if (!window || fft_size <= 0 || !isfinite(samplerate_hz) ||
        samplerate_hz <= 0.0)
        return 0.0;
    for (int i = 0; i < fft_size; i++) {
        double w = window[i];
        sum += w;
        sum_sq += w * w;
    }
    if (sum <= 0.0 || sum_sq <= 0.0)
        return 0.0;
    return samplerate_hz * sum_sq / (sum * sum);
}

/**
 * @brief Return the display-only white-noise normalization for one FFT plan.
 *
 * Coherent amplitude remains normalized by `1 / sum(window)`. A white-noise
 * bin instead scales with `sqrt(ENBW)`, so its apparent waterfall brightness
 * falls at fine resolution unless this presentation factor is applied.
 *
 * @param window Exact FFT window coefficients.
 * @param fft_size FFT length in samples.
 * @param samplerate_hz FFT input sample rate in samples/s.
 * @return Dimensionless display factor relative to the common 90 kHz ENBW.
 */
static float waterfall_noise_presentation_scale(const float *window,
                                                 int fft_size,
                                                 double samplerate_hz)
{
    double enbw = fft_window_equivalent_noise_bandwidth(window, fft_size,
                                                         samplerate_hz);
    double scale;

    if (enbw <= 0.0)
        return 1.0f;
    scale = sqrt(WATERFALL_NOISE_REFERENCE_ENBW_HZ / enbw);
    if (!isfinite(scale) || scale <= 0.0)
        return 1.0f;
    /* Invalid plans must not turn into an unbounded display gain. */
    if (scale > 1024.0)
        scale = 1024.0;
    return (float)scale;
}

/**
 * cic_frequency_weight:
 * Return inverse normalized CIC magnitude at a baseband frequency offset.
 *
 * Machine-readable contract:
 * - `decim` is the CIC decimation factor.
 * - `freq_hz` is signed baseband frequency after decimation.
 * - `raw_samplerate` is the Pluto input sample rate before decimation.
 * - Output is clamped inverse droop compensation for CIC_STAGES integrator/
 *   comb stages after the existing `decim^CIC_STAGES` DC-gain correction.
 *
 * Human note:
 * Dividing CIC output by `decim^stages` fixes DC gain only. Away from DC, CIC
 * droop depends on decimation and frequency. This weight keeps a steady tone
 * at the same displayed level when the planner changes CIC factors.
 */
static float cic_frequency_weight(int decim, double freq_hz, double raw_samplerate)
{
    double x;
    double num;
    double den;
    double gain;
    double weight;

    if (decim <= 1 || raw_samplerate <= 0.0 || !isfinite(freq_hz))
        return 1.0f;
    x = M_PI * freq_hz / raw_samplerate;
    if (fabs(x) < 1.0e-12)
        return 1.0f;
    num = sin((double)decim * x);
    den = (double)decim * sin(x);
    if (fabs(den) < 1.0e-18)
        return 1.0f;
    gain = fabs(num / den);
    gain = pow(gain, (double)CIC_STAGES);
    if (gain < 0.05)
        gain = 0.05;
    weight = 1.0 / gain;
    if (weight > 8.0)
        weight = 8.0;
    return (float)weight;
}

/**
 * @brief Estimate raw single-stream line cadence for one FFT/CIC candidate.
 *
 * @param hardware_samplerate Pluto complex sample rate in samples/s.
 * @param fft_size FFT length in decimated samples.
 * @param decim_factor Raw samples consumed per FFT input sample.
 * @return Estimated complete non-overlapped FFT frames per second.
 */
static double single_candidate_line_rate(double hardware_samplerate,
                                         int fft_size, int decim_factor)
{
    double stream_sps;
    double samples_per_line;

    if (hardware_samplerate <= 0.0 || fft_size <= 0 || decim_factor <= 0)
        return 0.0;
    stream_sps = decim_factor > 1 ? PLUTO_EST_CIC_STREAM_SPS :
        PLUTO_EST_SINGLE_STREAM_SPS;
    if (stream_sps > hardware_samplerate)
        stream_sps = hardware_samplerate;
    samples_per_line = (double)fft_size * (double)decim_factor;
    return samples_per_line > 0.0 ? stream_sps / samples_per_line : 0.0;
}

/**
 * @brief Populate cadence and visible-bin metrics for a single-mode plan.
 *
 * @param plan Plan to update.
 * @param span Visible air-frequency width in hertz.
 * @param display_bins Published frontend row width in bins.
 */
static void update_single_plan_metrics(single_fft_plan_t *plan, double span,
                                       int display_bins)
{
    if (!plan)
        return;
    plan->estimated_line_rate = single_candidate_line_rate(
        plan->hardware_samplerate, plan->fft_size, plan->decim_factor);
    plan->visible_raw_bins = 0.0;
    plan->visible_bins_per_pixel = 0.0;
    if (plan->fft_samplerate > 0.0 && span > 0.0) {
        plan->visible_raw_bins = (double)plan->fft_size * span /
            plan->fft_samplerate;
        if (display_bins > 0)
            plan->visible_bins_per_pixel = plan->visible_raw_bins /
                (double)display_bins;
    }
    plan->minimum_rate_limited = 0;
    plan->minimum_rate_achieved = 1;
}

/**
 * single_fft_plan_for_span:
 * Choose the FFT/CIC plan for "single frequency" views.
 *
 * Machine-readable contract:
 * - Input `span` is the visible air-frequency width in Hz.
 * - Output `fft_size` and `decim_factor` are powers of two.
 * - `source_span` covers the visible span and usually includes a zero-IF
 *   guard chosen by the hardware profile. At very high zoom, CIC decimation
 *   keeps the raw-bin product exact and zero-IF avoidance becomes best-effort.
 * - `fft_size * decim_factor` follows the Fobos-proven resolution product
 *   `display_bins * hardware_sample_rate / visible_span`.
 * - Hardware sample rate/RF bandwidth are lowered before CIC and every Pluto
 *   RF bandwidth is strictly less than its sample rate.
 * - CIC is introduced only after the product no longer fits in a 65536-point
 *   FFT at the chosen hardware profile.
 *
 * Human note:
 * The old Pluto planner sized single-mode FFTs from display width only, which
 * made narrow views fast but under-resolved. This planner restores the tested
 * Fobos rule while adding Pluto-specific hardware profiles to keep first-line
 * latency bounded.
 */
static single_fft_plan_t single_fft_plan_for_span(double span)
{
    single_fft_plan_t plan;
    int display_bins = current_display_bins();
    pluto_rf_profile_t profile = single_profile_for_span(span);
    double hardware_sr = profile.samplerate;
    double hardware_bw = profile.rf_bandwidth;
    double source_span = hardware_bw;
    double zero_if_guard = single_zero_if_guard_for_span(span);
    double required_product;
    int decim;
    int fft_size;

    if (hardware_sr <= 0.0)
        hardware_sr = PLUTO_AUTO_SAMPLE_RATE_HZ;
    if (hardware_bw <= 0.0)
        hardware_bw = hardware_sr;
    if (hardware_bw >= hardware_sr)
        hardware_bw = hardware_sr * PLUTO_RF_BW_LT_SR_RATIO;

    if (direct_sampling_enabled()) {
        hardware_sr = g_samplerate;
        hardware_bw = direct_sampling_max_hz();
        if (hardware_bw >= hardware_sr)
            hardware_bw = hardware_sr * PLUTO_RF_BW_LT_SR_RATIO;
    }

    source_span = hardware_bw;
    plan.fft_size = SINGLE_RAW_FFT_MIN;
    plan.decim_factor = 1;
    plan.decim_hop = SINGLE_RAW_FFT_MIN;
    plan.fft_samplerate = hardware_sr;
    plan.source_span = source_span;
    plan.extraction_ratio = hardware_sr > 0.0 ? source_span / hardware_sr : 1.0;
    plan.hardware_samplerate = hardware_sr;
    plan.hardware_rf_bandwidth = hardware_bw;
    plan.hardware_bw_ratio = hardware_sr > 0.0 ? hardware_bw / hardware_sr : 1.0;
    plan.zero_if_guard_hz = zero_if_guard;
    plan.minimum_rate_limited = 0;
    plan.minimum_rate_achieved = 1;
    plan.estimated_line_rate = 0.0;
    plan.visible_raw_bins = 0.0;
    plan.visible_bins_per_pixel = 0.0;

    if (span <= 0.0 || hardware_sr <= 0.0 || display_bins <= 0) {
        update_single_plan_metrics(&plan, span, display_bins);
        return plan;
    }

    /*
     * Preserve the Fobos-proven resolution rule: the processed product
     * FFT_size * decimation must provide enough raw bins for the exact
     * display-bin reducer. The published SSE row is then reduced to one
     * processed bin per screen pixel.
     */
    required_product = ceil(((double)display_bins * hardware_sr) / span);
    if (required_product < (double)SINGLE_RAW_FFT_MIN)
        required_product = (double)SINGLE_RAW_FFT_MIN;

    decim = (int)ceil(required_product / (double)SINGLE_FFT_SIZE_MAX);
    if (decim < 1)
        decim = 1;
    decim = clamp_power_of_two_int(decim, 1, SINGLE_DECIM_MAX);
    while (decim > 1 && (hardware_sr / (double)decim) < span * 1.05)
        decim >>= 1;

    fft_size = (int)ceil(required_product / (double)decim);
    fft_size = clamp_power_of_two_int(fft_size, SINGLE_RAW_FFT_MIN, SINGLE_FFT_SIZE_MAX);

    plan.fft_size = fft_size;
    plan.decim_factor = decim;
    plan.decim_hop = fft_size;
    plan.fft_samplerate = hardware_sr / (double)decim;
    plan.source_span = plan.fft_samplerate;
    if (plan.source_span > hardware_bw)
        plan.source_span = hardware_bw;
    plan.extraction_ratio = plan.fft_samplerate > 0.0 ?
        plan.source_span / plan.fft_samplerate : 1.0;
    if (plan.extraction_ratio <= 0.0)
        plan.extraction_ratio = 1.0;
    if (plan.extraction_ratio > 1.0)
        plan.extraction_ratio = 1.0;

    update_single_plan_metrics(&plan, span, display_bins);
    return plan;
}

static int single_effective_fft_size_for_span(double span)
{
    return single_fft_plan_for_span(span).fft_size;
}

/**
 * @brief Choose the scan FFT size that feeds the exact display-bin reducer.
 *
 * The raw FFT product may contain more bins than the display, but never fewer
 * in normal operating ranges. `publish_scan_line()` reduces that raw product
 * to exactly `display_bins` values by peak-per-pixel aggregation.
 *
 * @param span Visible air-frequency span in hertz.
 * @param selected_fft_size User/config FFT value retained for compatibility.
 * @return Power-of-two scan FFT size.
 */
static int scan_effective_fft_size_for_span(double span, int selected_fft_size)
{
    int display_bins = current_display_bins();
    double needed_fft;
    int effective;

    (void)selected_fft_size;
    if (span <= 0.0 || g_samplerate <= 0.0 || display_bins <= 0)
        return FFT_SIZE_MIN;

    needed_fft = ceil(((double)display_bins * g_samplerate) / span);
    if (needed_fft < FFT_SIZE_MIN)
        needed_fft = FFT_SIZE_MIN;
    if (needed_fft > (double)SCAN_FFT_SIZE_MAX)
        needed_fft = SCAN_FFT_SIZE_MAX;

    effective = next_power_of_two_int((int)needed_fft);
    if (effective < FFT_SIZE_MIN)
        effective = FFT_SIZE_MIN;
    if (effective > SCAN_FFT_SIZE_MAX)
        effective = SCAN_FFT_SIZE_MAX;
    return effective;
}

static int scan_effective_fft_size_for_current_view(void)
{
    double start;
    double end;
    raw_visible_band(&start, &end);
    return scan_effective_fft_size_for_span(end - start, current_fft_size());
}

static int single_decim_hop_for_plan(const single_fft_plan_t *plan);

/**
 * @brief Return raw input samples advanced by one decimated FFT hop.
 *
 * `hop` is measured in decimated complex samples. `decim_factor` is the raw
 * input samples consumed for one decimated output sample. The return value is
 * clamped to `UINT32_MAX` raw complex samples for buffer-size APIs.
 *
 * @param hop Decimated complex samples advanced per FFT line.
 * @param decim_factor Raw samples per decimated output sample.
 * @return Raw complex samples advanced per FFT line.
 */
static uint32_t decim_raw_sample_count_for_hop(int hop, int decim_factor)
{
    uint64_t samples;

    if (hop <= 0 || decim_factor <= 0)
        return 0;
    samples = (uint64_t)hop * (uint64_t)decim_factor;
    if (samples > UINT32_MAX)
        return UINT32_MAX;
    return (uint32_t)samples;
}

/**
 * @brief Choose Pluto async refill size for decimated single-frequency mode.
 *
 * For normal CIC plans the refill size matches the raw sample advance for one
 * displayed FFT line, so the steady-state worker receives one line worth of
 * raw samples per callback. Very large line advances are capped to bound queue
 * memory; capped cases are still visible in the scan log as
 * `async < line samples`.
 *
 * @param line_sample_count Raw complex samples advanced per FFT line.
 * @return Raw complex samples requested from one Pluto/IIO refill.
 */
static uint32_t single_decim_async_len_for_line(uint32_t line_sample_count)
{
    if (line_sample_count == 0)
        return SINGLE_DECIM_ASYNC_MIN_LEN;
    if (line_sample_count > SINGLE_DECIM_ASYNC_MAX_LEN)
        return SINGLE_DECIM_ASYNC_MAX_LEN;
    return line_sample_count;
}

static uint32_t single_line_sample_count_for_span(double span)
{
    single_fft_plan_t plan = single_fft_plan_for_span(span);
    int hop = single_decim_hop_for_plan(&plan);
    return decim_raw_sample_count_for_hop(hop, plan.decim_factor);
}

/**
 * @brief Choose decimated samples advanced between single-mode FFT frames.
 *
 * CIC resolution remains fixed when minimum cadence changes. A power-of-two
 * integer overlap factor shortens the hop in decimated-sample units, so raw
 * input still enters the CIC once while adjacent FFT windows intentionally
 * share decimated samples.
 *
 * @param plan Single-frequency FFT/CIC plan with hertz and bin fields.
 * @return Decimated-sample hop, clamped to `[1, plan->fft_size]`.
 */
static int single_decim_hop_for_plan(const single_fft_plan_t *plan)
{
    uint32_t min_rate;
    uint32_t max_rate;
    double target_lps;
    double nonoverlap_lps;
    int required_overlap;
    int overlap_factor;

    if (!plan)
        return FFT_SIZE_MIN;
    min_rate = normalize_min_rate_lps(g_min_rate_lps);
    if (plan->decim_factor <= 1 || min_rate == 0)
        return plan->fft_size;

    max_rate = normalize_rate_limit_lps(g_rate_limit_lps);
    target_lps = (double)min_rate;
    if (max_rate > 0 && target_lps > (double)max_rate)
        target_lps = (double)max_rate;
    nonoverlap_lps = single_candidate_line_rate(
        plan->hardware_samplerate, plan->fft_size, plan->decim_factor);
    if (target_lps <= 0.0 || nonoverlap_lps <= 0.0 ||
        nonoverlap_lps >= target_lps)
        return plan->fft_size;

    required_overlap = (int)ceil(target_lps / nonoverlap_lps);
    overlap_factor = next_power_of_two_int(required_overlap);
    if (overlap_factor < 1)
        overlap_factor = 1;
    if (overlap_factor > plan->fft_size)
        overlap_factor = plan->fft_size;
    return plan->fft_size / overlap_factor;
}

/** Return the FFT size that the current view will actually use. */
static int current_effective_fft_size(void)
{
    double start;
    double end;
    if (planned_run_mode() != RUN_MODE_SINGLE)
        return scan_effective_fft_size_for_current_view();
    raw_visible_band(&start, &end);
    return single_effective_fft_size_for_span(end - start);
}

/** Return current CIC decimation; scan/hop mode always uses raw buffers. */
static int current_decim_factor(void)
{
    double start;
    double end;
    if (planned_run_mode() != RUN_MODE_SINGLE)
        return 1;
    raw_visible_band(&start, &end);
    return single_fft_plan_for_span(end - start).decim_factor;
}

/** Return raw Pluto samples consumed per published waterfall line. */
static uint32_t current_line_sample_count(void)
{
    double start;
    double end;
    if (planned_run_mode() != RUN_MODE_SINGLE) {
        int fft = current_effective_fft_size();
        return (uint32_t)((fft > (int)SCAN_BUF_LEN) ? fft : (int)SCAN_BUF_LEN);
    }
    raw_visible_band(&start, &end);
    return single_line_sample_count_for_span(end - start);
}

/** Return decimated samples advanced per FFT frame. */
static int current_decim_hop(void)
{
    double start;
    double end;
    single_fft_plan_t plan;
    if (planned_run_mode() != RUN_MODE_SINGLE)
        return current_effective_fft_size();
    raw_visible_band(&start, &end);
    plan = single_fft_plan_for_span(end - start);
    return single_decim_hop_for_plan(&plan);
}

/** Return overlap factor: 1.0 means independent FFT frames. */
static double current_overlap_factor(void)
{
    int fft_size = current_effective_fft_size();
    int hop = current_decim_hop();
    if (hop <= 0)
        return 1.0;
    return (double)fft_size / (double)hop;
}

/** Return whether minimum cadence enabled overlapping CIC FFT windows. */
static int current_minimum_rate_limited(void)
{
    if (planned_run_mode() != RUN_MODE_SINGLE)
        return 0;
    return current_decim_factor() > 1 && current_decim_hop() <
        current_effective_fft_size();
}

/** Return whether the current single-mode raw cadence meets its minimum. */
static int current_minimum_rate_achieved(void)
{
    double start;
    double end;
    single_fft_plan_t plan;
    int hop;
    double stream_sps;
    double line_rate;
    if (planned_run_mode() != RUN_MODE_SINGLE)
        return g_min_rate_lps == 0;
    raw_visible_band(&start, &end);
    plan = single_fft_plan_for_span(end - start);
    hop = single_decim_hop_for_plan(&plan);
    stream_sps = plan.decim_factor > 1 ? PLUTO_EST_CIC_STREAM_SPS :
        PLUTO_EST_SINGLE_STREAM_SPS;
    if (stream_sps > plan.hardware_samplerate)
        stream_sps = plan.hardware_samplerate;
    line_rate = stream_sps /
        ((double)hop * (double)plan.decim_factor);
    return g_min_rate_lps == 0 ||
        line_rate + 1.0e-9 >= (double)g_min_rate_lps;
}

/**
 * @brief Return true FFT line cadence before min-rate overlap boosting.
 *
 * The returned rate is the base cadence of fresh FFT windows for the current
 * plan. It intentionally excludes single-mode overlap used to satisfy the
 * minimum waterfall-rate slider.
 *
 * @return Fresh FFT lines per second for the current visible span.
 */
static double current_true_line_rate(void)
{
    run_mode_t mode = planned_run_mode();
    if (mode == RUN_MODE_SINGLE) {
        double start;
        double end;
        single_fft_plan_t plan;
        raw_visible_band(&start, &end);
        plan = single_fft_plan_for_span(end - start);
        return single_candidate_line_rate(plan.hardware_samplerate,
                                          plan.fft_size,
                                          plan.decim_factor);
    }

    {
        double step = 0.0;
        double scan_end = 0.0;
        int total_steps = current_scan_plan(&step, &scan_end);
        double hop_ms = estimated_scan_hop_ms_for_samples(
            current_line_sample_count());
        if (total_steps <= 0 || hop_ms <= 0.0)
            return 0.0;
        return 1000.0 / (hop_ms * (double)total_steps);
    }
}

/**
 * @brief Return one scan context's true FFT cadence before overlap boosting.
 *
 * @param ctx Active scan context containing FFT, decimation, and samplerate.
 * @return Fresh FFT lines per second for that context, or zero if unavailable.
 */
static double ctx_true_line_rate(const scan_ctx_t *ctx)
{
    double stream_sps;

    if (!ctx)
        return 0.0;
    if (!ctx->single_mode)
        return ctx->estimated_line_rate;
    stream_sps = ctx->decim_factor > 1 ?
        PLUTO_EST_CIC_STREAM_SPS : PLUTO_EST_SINGLE_STREAM_SPS;
    if (ctx->raw_samplerate > 0.0 && stream_sps > ctx->raw_samplerate)
        stream_sps = ctx->raw_samplerate;
    if (ctx->fft_size <= 0 || ctx->decim_factor <= 0)
        return 0.0;
    return stream_sps /
        ((double)ctx->fft_size * (double)ctx->decim_factor);
}

/**
 * @brief Estimate the cadence at which this context publishes display rows.
 *
 * This includes CIC overlap used by the minimum-rate control and the final
 * maximum-rate keep ratio, unlike `ctx_true_line_rate()`.
 *
 * @param ctx Active scan context.
 * @return Estimated emitted waterfall rows per second.
 */
static double ctx_published_line_rate(const scan_ctx_t *ctx)
{
    double rate;

    if (!ctx)
        return 0.0;
    rate = ctx->estimated_line_rate;
    if (ctx->rate_keep_ratio > 0.0 && ctx->rate_keep_ratio < 1.0)
        rate *= ctx->rate_keep_ratio;
    return rate;
}

/** Return raw FFT-bin coverage of the current visible single-mode interval. */
static double current_visible_raw_bins(void)
{
    double start;
    double end;
    if (planned_run_mode() != RUN_MODE_SINGLE)
        return 0.0;
    raw_visible_band(&start, &end);
    return single_fft_plan_for_span(end - start).visible_raw_bins;
}

/** Return raw FFT bins per published display pixel for the current view. */
static double current_visible_bins_per_pixel(void)
{
    double start;
    double end;
    if (planned_run_mode() != RUN_MODE_SINGLE)
        return 0.0;
    raw_visible_band(&start, &end);
    return single_fft_plan_for_span(end - start).visible_bins_per_pixel;
}

/**
 * @brief Estimate initial waterfall latency for a planned acquisition mode.
 *
 * The return value is milliseconds until the first complete display row can be
 * computed. For CIC single-frequency mode this is `fft * decim / stream_sps`;
 * for raw single mode it is `fft / stream_sps`; for scan mode it is the
 * measured hop time multiplied by hop count.
 *
 * @param samplerate_hz Hardware sample rate in samples/s.
 * @param fft_size FFT length in samples.
 * @param line_sample_count Raw input samples advanced per steady-state line.
 * @param total_steps Scan hop count; 1 means single-frequency mode.
 * @param decim_factor CIC decimation factor, or 1 for raw FFT.
 * @return First-line latency in milliseconds.
 */
static double estimated_first_line_ms_for_plan(double samplerate_hz,
                                               int fft_size,
                                               uint32_t line_sample_count,
                                               int total_steps,
                                               int decim_factor)
{
    double stream_sps;

    if (fft_size <= 0 || total_steps <= 0)
        return 0.0;
    if (total_steps > 1) {
        uint32_t samples = line_sample_count > 0 ? line_sample_count :
            (uint32_t)fft_size;
        return estimated_scan_hop_ms_for_samples(samples) * (double)total_steps;
    }

    stream_sps = decim_factor > 1 ? PLUTO_EST_CIC_STREAM_SPS :
        estimated_single_stream_sps_for_line_samples((uint32_t)fft_size);
    if (samplerate_hz > 0.0 && stream_sps > samplerate_hz)
        stream_sps = samplerate_hz;
    if (stream_sps <= 0.0)
        return 0.0;
    if (decim_factor > 1)
        return (((double)fft_size + CIC_STAGES) * (double)decim_factor /
                stream_sps) * 1000.0;
    return ((double)fft_size / stream_sps) * 1000.0;
}

/** Return the estimated initial waterfall latency for the current view. */
static double current_first_line_ms(int total_steps, uint32_t line_sample_count)
{
    return estimated_first_line_ms_for_plan(current_active_samplerate(),
                                            current_effective_fft_size(),
                                            line_sample_count,
                                            total_steps,
                                            current_decim_factor());
}

/** Return sample rate seen by the FFT for the planned current view. */
static double current_active_samplerate(void)
{
    double start;
    double end;
    if (planned_run_mode() != RUN_MODE_SINGLE)
        return g_samplerate;
    raw_visible_band(&start, &end);
    return single_fft_plan_for_span(end - start).hardware_samplerate;
}

/** Return bandwidth represented by one backend row for the planned view. */
static double current_active_rf_bandwidth(void)
{
    double start;
    double end;
    if (planned_run_mode() != RUN_MODE_SINGLE)
        return g_rf_bandwidth;
    raw_visible_band(&start, &end);
    return single_fft_plan_for_span(end - start).hardware_rf_bandwidth;
}

/** Return active passband fraction for the planned current view. */
static double current_active_bw_ratio(void)
{
    double start;
    double end;
    if (planned_run_mode() != RUN_MODE_SINGLE)
        return g_bw_ratio;
    raw_visible_band(&start, &end);
    return single_fft_plan_for_span(end - start).hardware_bw_ratio;
}

static const char *run_mode_name(run_mode_t mode)
{
    return mode == RUN_MODE_SINGLE ? "single" : "scan";
}

static int rate_drop_factor_for_plan(double samplerate, uint32_t line_sample_count,
                                     int total_steps, int decim_factor,
                                     uint32_t limit_lps, double *out_line_rate)
{
    double buffers_per_second;
    double lines_per_second;
    int factor;

    if (out_line_rate)
        *out_line_rate = 0.0;
    if (samplerate <= 0.0 || line_sample_count == 0 || total_steps <= 0)
        return 1;

    limit_lps = normalize_rate_limit_lps(limit_lps);
    /*
     * pluto_sdr_read_async() reports buf_len as complex I/Q sample count.
     * Single-frequency streaming is host/libiio-throughput limited, so use
     * empirical Pluto stream rates instead of the mathematical RF sample rate.
     * CIC paths are lower because the host must run the stateful decimator
     * over every raw sample before one full FFT frame is initially available.
     * Hardware scan line rate is dominated by retune time per hop.
     */
    if (total_steps == 1) {
        double stream_sps = decim_factor > 1 ?
            PLUTO_EST_CIC_STREAM_SPS :
            estimated_single_stream_sps_for_line_samples(line_sample_count);
        if (samplerate > 0.0 && stream_sps > samplerate)
            stream_sps = samplerate;
        lines_per_second = stream_sps / (double)line_sample_count;
    } else if (total_steps > 1) {
        lines_per_second = 1000.0 /
            (estimated_scan_hop_ms_for_samples(line_sample_count) * (double)total_steps);
    } else {
        buffers_per_second = samplerate / (double)line_sample_count;
        lines_per_second = buffers_per_second / (double)total_steps;
    }
    if (out_line_rate)
        *out_line_rate = lines_per_second;

    if (lines_per_second <= (double)limit_lps)
        return 1;
    factor = (int)ceil(lines_per_second / (double)limit_lps);
    if (factor < 1)
        factor = 1;
    return factor;
}

/**
 * rate_keep_ratio_for_line_rate:
 * Convert a raw line rate into deterministic fractional keep/drop scheduling.
 * This throttles fast sources without changing FFT planning.
 */
static double rate_keep_ratio_for_line_rate(double line_rate, uint32_t limit_lps,
                                            int single_mode)
{
    double target_limit;
    limit_lps = normalize_rate_limit_lps(limit_lps);
    if (line_rate <= 0.0 || line_rate <= (double)limit_lps)
        return 1.0;
    target_limit = (double)limit_lps;
    (void)single_mode;
    target_limit *= PLUTO_RATE_LIMIT_GUARD;
    if (target_limit > line_rate)
        target_limit = line_rate;
    return target_limit / line_rate;
}

static int scan_bins_per_step_for_width_and_fft(double width, int fft_size)
{
    double max_width = scan_step_hz();
    int bins;
    if (width > max_width) width = max_width;
    bins = (int)lrint((width / g_samplerate) * (double)fft_size);
    if (bins < 1) bins = 1;
    if (bins > fft_size) bins = fft_size;
    return bins;
}

static int bins_for_width_and_rate(double width, double samplerate,
                                   double ratio, int fft_size)
{
    double max_width;
    int bins;
    if (ratio <= 0.0)
        ratio = 1.0;
    if (ratio > 1.0)
        ratio = 1.0;
    max_width = samplerate * ratio;
    if (width > max_width) width = max_width;
    bins = (int)lrint((width / samplerate) * (double)fft_size);
    if (bins < 1) bins = 1;
    if (bins > fft_size) bins = fft_size;
    return bins;
}

static int scan_bins_per_step_for_width(double width)
{
    return scan_bins_per_step_for_width_and_fft(width, scan_effective_fft_size_for_current_view());
}

static int scan_bins_per_step(void)
{
    double step = 0.0;
    double start;
    double end;
    if (planned_run_mode() == RUN_MODE_SINGLE) {
        single_fft_plan_t plan;
        raw_visible_band(&start, &end);
        plan = single_fft_plan_for_span(end - start);
        return bins_for_width_and_rate(plan.source_span, plan.fft_samplerate,
                                       plan.extraction_ratio, plan.fft_size);
    }
    if (build_scan_frequencies(NULL, &step) <= 0)
        return 1;
    return scan_bins_per_step_for_width(scan_step_hz());
}

static int current_line_bins(void)
{
    double scan_start;
    double scan_end;
    double step = 0.0;
    int total_steps;
    int bins_per_step;
    int last_bins;
    int fft_size;

    if (planned_run_mode() == RUN_MODE_SINGLE) {
        single_fft_plan_t plan;
        raw_visible_band(&scan_start, &scan_end);
        plan = single_fft_plan_for_span(scan_end - scan_start);
        return bins_for_width_and_rate(plan.source_span, plan.fft_samplerate,
                                       plan.extraction_ratio, plan.fft_size);
    }

    active_scan_band(&scan_start, &scan_end);
    total_steps = build_scan_frequencies_for_band(scan_start, scan_end, NULL, &step);
    if (total_steps <= 0)
        return 0;
    fft_size = scan_effective_fft_size_for_current_view();
    bins_per_step = scan_bins_per_step_for_width_and_fft(step, fft_size);
    last_bins = scan_bins_per_step_for_width_and_fft(scan_last_width_for_band(scan_start, scan_end, total_steps, step), fft_size);
    return (total_steps - 1) * bins_per_step + last_bins;
}

static int scan_ctx_apply_fft_config(scan_ctx_t *ctx)
{
    int fft_size;
    uint32_t fft_generation;
    int bins_per_step;
    int last_bins;
    int line_bins;
    int decim_factor;
    int decim_hop;
    double fft_samplerate;
    double fft_ratio;
    double step_width;
    double last_width;
    size_t bin_count;
    float *window = NULL;
    float *fft_scratch = NULL;
    float *decim_accum = NULL;
    double *cic_delay = NULL;
    float *line_buf = NULL;
    float mag_scale;

    for (;;) {
        pthread_mutex_lock(&g_fft_mutex);
        fft_size = g_fft_size;
        fft_generation = g_fft_generation;
        pthread_mutex_unlock(&g_fft_mutex);
        ctx->selected_fft_size = fft_size;
        decim_factor = 1;
        decim_hop = fft_size;
        ctx->max_ffts_per_buffer = ctx->single_mode ? 1 : SCAN_FFTS_PER_STEP;
        fft_samplerate = ctx->raw_samplerate > 0.0 ? ctx->raw_samplerate : ctx->samplerate;
        fft_ratio = ctx->bw_ratio;
        step_width = ctx->step_width;
        last_width = ctx->last_width;
        if (ctx->single_mode) {
            single_fft_plan_t plan = single_fft_plan_for_span(ctx->visible_end - ctx->visible_start);
            fft_size = plan.fft_size;
            decim_factor = plan.decim_factor;
            decim_hop = single_decim_hop_for_plan(&plan);
            fft_samplerate = plan.fft_samplerate;
            fft_ratio = plan.extraction_ratio;
            step_width = plan.source_span;
            last_width = plan.source_span;
            ctx->minimum_rate_limited = decim_factor > 1 &&
                decim_hop < fft_size;
            {
                double overlap_rate = single_candidate_line_rate(
                    plan.hardware_samplerate, decim_hop, decim_factor);
                ctx->minimum_rate_achieved = g_min_rate_lps == 0 ||
                    overlap_rate + 1.0e-9 >= (double)g_min_rate_lps;
            }
            ctx->visible_raw_bins = plan.visible_raw_bins;
            ctx->visible_bins_per_pixel = plan.visible_bins_per_pixel;
        } else {
            fft_size = scan_effective_fft_size_for_span(ctx->visible_end - ctx->visible_start,
                                                        ctx->selected_fft_size);
            decim_hop = fft_size;
        }

        if (ctx->total_steps <= 0)
            return -1;
        bins_per_step = bins_for_width_and_rate(step_width, fft_samplerate, fft_ratio, fft_size);
        last_bins = bins_for_width_and_rate(last_width, fft_samplerate, fft_ratio, fft_size);
        line_bins = (ctx->total_steps - 1) * bins_per_step + last_bins;
        bin_count = (size_t)line_bins;

        window = malloc((size_t)fft_size * sizeof(float));
        fft_scratch = malloc((size_t)fft_size * 2 * sizeof(float));
        if (decim_factor > 1) {
            decim_accum = malloc((size_t)fft_size * 2 * sizeof(float));
            cic_delay = calloc((size_t)CIC_STAGES * (size_t)decim_factor * 2U,
                               sizeof(double));
        }
        line_buf = malloc(bin_count * sizeof(float));
        if (!window || !fft_scratch ||
            (decim_factor > 1 && (!decim_accum || !cic_delay)) ||
            !line_buf) {
            free(window);
            free(fft_scratch);
            free(decim_accum);
            free(cic_delay);
            free(line_buf);
            return -1;
        }

        pthread_mutex_lock(&g_fft_mutex);
        if (ctx->selected_fft_size == g_fft_size) {
            if (fft_size == g_fft_size)
                memcpy(window, g_window, (size_t)fft_size * sizeof(float));
            else
                init_window_for_size(window, fft_size);
            fft_generation = g_fft_generation;
            pthread_mutex_unlock(&g_fft_mutex);
            break;
        }
        pthread_mutex_unlock(&g_fft_mutex);

        free(window);
        free(fft_scratch);
        free(decim_accum);
        free(cic_delay);
        free(line_buf);
        window = NULL;
        fft_scratch = NULL;
        decim_accum = NULL;
        cic_delay = NULL;
        line_buf = NULL;
    }

    mag_scale = fft_window_magnitude_scale(window, fft_size);

    free(ctx->window);
    free(ctx->fft_scratch);
    free(ctx->decim_accum);
    free(ctx->cic_delay);
    free(ctx->line_buf);

    ctx->fft_size = fft_size;
    ctx->fft_generation = fft_generation;
    ctx->decim_factor = decim_factor;
    ctx->decim_hop = decim_hop;
    ctx->overlap_factor = decim_hop > 0 ? (double)fft_size / (double)decim_hop : 1.0;
    ctx->decim_fill = 0;
    ctx->decim_phase = 0;
    ctx->cic_delay_pos = 0;
    ctx->cic_dc_gain = decim_factor > 1 ?
        pow((double)decim_factor, (double)CIC_STAGES) : 1.0;
    ctx->bins_per_step = bins_per_step;
    ctx->last_bins = last_bins;
    ctx->line_bins = line_bins;
    ctx->samplerate = fft_samplerate;
    ctx->bw_ratio = fft_ratio;
    ctx->step_width = step_width;
    ctx->last_width = last_width;
    if (ctx->single_mode) {
        ctx->scan_start = ctx->center_freq - step_width * 0.5;
        ctx->scan_end = ctx->center_freq + step_width * 0.5;
    }
    ctx->direct_mix_phase = 0.0;
    ctx->direct_mix_inc = 0.0;
    if (ctx->direct_sampling && decim_factor > 1 &&
        ctx->raw_samplerate > 0.0) {
        ctx->direct_mix_inc = 2.0 * M_PI * ctx->center_freq / ctx->raw_samplerate;
        while (ctx->direct_mix_inc >= 2.0 * M_PI)
            ctx->direct_mix_inc -= 2.0 * M_PI;
        while (ctx->direct_mix_inc < 0.0)
            ctx->direct_mix_inc += 2.0 * M_PI;
    }
    ctx->mag_scale = mag_scale;
    ctx->waterfall_noise_scale = waterfall_noise_presentation_scale(
        window, fft_size, fft_samplerate);
    ctx->window = window;
    ctx->fft_scratch = fft_scratch;
    ctx->decim_accum = decim_accum;
    ctx->cic_delay = cic_delay;
    ctx->line_buf = line_buf;
    ctx->steps_seen = 0;
    memset(ctx->cic_sum_re, 0, sizeof(ctx->cic_sum_re));
    memset(ctx->cic_sum_im, 0, sizeof(ctx->cic_sum_im));
    ctx->cic_account_raw_samples = 0;
    ctx->cic_account_decim_samples = 0;
    ctx->cic_account_fft_frames = 0;
    ctx->cic_account_warmup_outputs = 0;
    ctx->cic_skip_frames_after_reset = 0;
    ctx->cic_warmup_outputs_remaining = decim_factor > 1 ? CIC_STAGES : 0;
    memset(ctx->step_seen, 0, sizeof(ctx->step_seen));

    return 0;
}

static uint32_t current_fft_generation(void)
{
    uint32_t generation;
    pthread_mutex_lock(&g_fft_mutex);
    generation = g_fft_generation;
    pthread_mutex_unlock(&g_fft_mutex);
    return generation;
}

static int average_fft_magnitude(float *buf, uint32_t buf_len, int bins_per_step,
                                 int fft_size, const float *window,
                                 float *local_fft, float mag_scale,
                                 int positive_half, int max_ffts,
                                 float *out)
{
    int fft_count = 0;
    int shifted_start = (fft_size - bins_per_step) / 2;

    memset(out, 0, (size_t)bins_per_step * sizeof(float));

    if (max_ffts < 1)
        max_ffts = 1;

    for (uint32_t pos = 0; pos + (uint32_t)fft_size <= buf_len && fft_count < max_ffts; pos += (uint32_t)fft_size) {
        for (int i = 0; i < fft_size; i++) {
            float w = window[i];
            local_fft[2*i]   = buf[2*(pos+i)]   * w;
            local_fft[2*i+1] = buf[2*(pos+i)+1] * w;
        }

        fft_c2c(local_fft, fft_size);

        for (int i = 0; i < bins_per_step; i++) {
            int fft_bin;
            if (positive_half)
                fft_bin = i;
            else {
                int shifted_bin = shifted_start + i;
                fft_bin = (shifted_bin + fft_size / 2) % fft_size;
            }
            float re = local_fft[2*fft_bin];
            float im = local_fft[2*fft_bin+1];
            {
                float mag = sqrtf(re*re + im*im);
                out[i] += mag;
            }
        }
        fft_count++;
    }

    if (fft_count <= 0) return 0;

    float inv = mag_scale / (float)fft_count;
    for (int i = 0; i < bins_per_step; i++)
        out[i] *= inv;

    return fft_count;
}

/**
 * @brief Reset CIC filter, frame accumulator, and accounting segment state.
 *
 * This is used after a known stream discontinuity such as a recovered refill
 * error or short read. The next complete FFT frame can be skipped to let the
 * zero-history CIC moving sums warm up before another row is published.
 *
 * @param ctx Active scan context.
 * @param skip_frames Number of complete CIC FFT frames to drop after reset.
 */
static void cic_reset_state(scan_ctx_t *ctx, int skip_frames)
{
    size_t delay_count;

    if (!ctx)
        return;

    ctx->decim_fill = 0;
    ctx->decim_phase = 0;
    ctx->cic_delay_pos = 0;
    memset(ctx->cic_sum_re, 0, sizeof(ctx->cic_sum_re));
    memset(ctx->cic_sum_im, 0, sizeof(ctx->cic_sum_im));
    if (ctx->cic_delay && ctx->decim_factor > 1) {
        delay_count = (size_t)CIC_STAGES * (size_t)ctx->decim_factor * 2U;
        memset(ctx->cic_delay, 0, delay_count * sizeof(double));
    }
    if (ctx->decim_accum && ctx->fft_size > 0)
        memset(ctx->decim_accum, 0, (size_t)ctx->fft_size * 2U * sizeof(float));

    ctx->cic_account_raw_samples = 0;
    ctx->cic_account_decim_samples = 0;
    ctx->cic_account_fft_frames = 0;
    ctx->cic_account_warmup_outputs = 0;
    ctx->cic_skip_frames_after_reset = skip_frames > 0 ? skip_frames : 0;
    ctx->cic_warmup_outputs_remaining =
        ctx->decim_factor > 1 ? CIC_STAGES : 0;
    ctx->rate_keep_credit = 1.0;
    ctx->last_publish_msec = 0;
}

/**
 * @brief Verify exact CIC sample accounting for the current stream segment.
 *
 * The invariant proves that the host path consumed exactly the unique raw
 * samples represented by completed frame advances plus the partial accumulator
 * and current decimation phase. In overlap mode completed frames advance by
 * `decim_hop` unique decimated samples while older decimated samples are
 * intentionally retained for the next FFT window.
 *
 * @param ctx Active scan context.
 * @return Nonzero when the accounting identity holds.
 */
static int cic_check_continuity_accounting(scan_ctx_t *ctx)
{
    uint64_t expected_decim;
    uint64_t expected_raw;

    if (!ctx || ctx->decim_factor <= 1 || ctx->decim_hop <= 0)
        return 1;

    expected_decim = ctx->cic_account_warmup_outputs +
        ctx->cic_account_fft_frames * (uint64_t)ctx->decim_hop +
        (uint64_t)ctx->decim_fill;
    expected_raw = expected_decim * (uint64_t)ctx->decim_factor +
        (uint64_t)ctx->decim_phase;

    if (ctx->cic_account_decim_samples == expected_decim &&
        ctx->cic_account_raw_samples == expected_raw)
        return 1;

    ctx->cic_continuity_errors++;
    if (ctx->cic_continuity_errors <= 5) {
        fprintf(stderr,
                "[SDR] CIC continuity mismatch: raw=%llu expected=%llu, decim=%llu expected=%llu, frames=%llu fill=%d phase=%d\n",
                (unsigned long long)ctx->cic_account_raw_samples,
                (unsigned long long)expected_raw,
                (unsigned long long)ctx->cic_account_decim_samples,
                (unsigned long long)expected_decim,
                (unsigned long long)ctx->cic_account_fft_frames,
                ctx->decim_fill, ctx->decim_phase);
    }
    cic_reset_state(ctx, 1);
    return 0;
}

/** Slide the decimated FFT frame by `decim_hop` samples after one line. */
static void advance_decim_frame(scan_ctx_t *ctx)
{
    int hop = ctx->decim_hop;
    int keep;

    if (hop <= 0 || hop >= ctx->fft_size) {
        ctx->decim_fill = 0;
        return;
    }

    keep = ctx->fft_size - hop;
    memmove(ctx->decim_accum, ctx->decim_accum + (size_t)hop * 2,
            (size_t)keep * 2 * sizeof(float));
    ctx->decim_fill = keep;
}

#if PSEUDO_RANDOM_SAMPLE_SOURCE == SYNTHETIC_TONE_SAMPLE_SOURCE
/**
 * @brief Measure whether a synthetic bin-centred tone remains Hann-narrow.
 *
 * The check runs on unquantized CIC/FFT magnitudes before display reduction.
 * A clean bin-centred complex tone should occupy the Hann main lobe (peak plus
 * its two adjacent bins) with distant leakage below -70 dBc.
 *
 * @param ctx Active synthetic scan context.
 * @param magnitude Shifted FFT magnitudes.
 * @param bins Number of valid magnitude bins.
 */
static void synthetic_check_fft_spectrum(scan_ctx_t *ctx,
                                         const float *magnitude, int bins)
{
    int peak_bin = 0;
    int width_start;
    int width_end;
    int width;
    int shifted_start;
    int expected_peak_bin;
    float peak = 0.0f;
    float distant = 0.0f;
    double outside_power = 0.0;
    double distant_dbc;
    double outside_dbc;
    int passed;

    if (!ctx || !magnitude || bins <= 0)
        return;
    for (int i = 0; i < bins; i++) {
        if (magnitude[i] > peak) {
            peak = magnitude[i];
            peak_bin = i;
        }
    }
    if (!(peak > 0.0f) || !isfinite(peak)) {
        ctx->synthetic_spectral_checks++;
        ctx->synthetic_spectral_failures++;
        fprintf(stderr, "[TEST] CIC tone spectrum ERROR: invalid peak\n");
        return;
    }

    width_start = peak_bin;
    while (width_start > 0 && magnitude[width_start - 1] >= peak * 0.49f)
        width_start--;
    width_end = peak_bin;
    while (width_end + 1 < bins && magnitude[width_end + 1] >= peak * 0.49f)
        width_end++;
    width = width_end - width_start + 1;

    for (int i = 0; i < bins; i++) {
        if (abs(i - peak_bin) <= 3)
            continue;
        if (magnitude[i] > distant)
            distant = magnitude[i];
        outside_power += (double)magnitude[i] * (double)magnitude[i];
    }
    distant_dbc = 20.0 * log10((double)distant / (double)peak + 1.0e-30);
    outside_dbc = 10.0 * log10(outside_power / ((double)peak * (double)peak) +
                                1.0e-30);
    shifted_start = (ctx->fft_size - bins) / 2;
    expected_peak_bin = (int)llround(ctx->synthetic_tone_hz *
        (double)ctx->fft_size / ctx->samplerate +
        (double)ctx->fft_size * 0.5) - shifted_start;
    passed = abs(peak_bin - expected_peak_bin) <= 1 && width <= 3 &&
        distant_dbc <= -70.0;
    ctx->synthetic_spectral_checks++;
    if (!passed)
        ctx->synthetic_spectral_failures++;

    if (ctx->synthetic_spectral_checks <= 8 || !passed) {
        fprintf(stderr,
                "[TEST] CIC tone spectrum %s: frame %llu, decim x%d, tone %.6f Hz, peak %.9g at bin %d (expected %d), -6 dB width %d bins, distant %.2f dBc, outside-main-lobe energy %.2f dBc\n",
                passed ? "PASS" : "ERROR",
                (unsigned long long)ctx->synthetic_spectral_checks,
                ctx->decim_factor, ctx->synthetic_tone_hz, peak, peak_bin,
                expected_peak_bin, width, distant_dbc, outside_dbc);
    }
}
#endif

/** Run one FFT over the accumulated CIC-decimated frame. */
static int fft_magnitude_from_accum(scan_ctx_t *ctx, int bins_per_step, float *out)
{
    int shifted_start = (ctx->fft_size - bins_per_step) / 2;

    memset(out, 0, (size_t)bins_per_step * sizeof(float));
    for (int i = 0; i < ctx->fft_size; i++) {
        float w = ctx->window[i];
        ctx->fft_scratch[2*i] = ctx->decim_accum[2*i] * w;
        ctx->fft_scratch[2*i+1] = ctx->decim_accum[2*i+1] * w;
    }

    fft_c2c(ctx->fft_scratch, ctx->fft_size);

    for (int i = 0; i < bins_per_step; i++) {
        int shifted_bin = shifted_start + i;
        int fft_bin = (shifted_bin + ctx->fft_size / 2) % ctx->fft_size;
        double freq_hz = ((double)shifted_bin - (double)ctx->fft_size * 0.5) *
            ctx->samplerate / (double)ctx->fft_size;
        float cic_weight = cic_frequency_weight(ctx->decim_factor, freq_hz,
                                                ctx->raw_samplerate);
        float re = ctx->fft_scratch[2*fft_bin];
        float im = ctx->fft_scratch[2*fft_bin+1];
        out[i] = sqrtf(re*re + im*im) * ctx->mag_scale * cic_weight;
    }

#if PSEUDO_RANDOM_SAMPLE_SOURCE == SYNTHETIC_TONE_SAMPLE_SOURCE
    if (!ctx->preview_mode)
        synthetic_check_fft_spectrum(ctx, out, bins_per_step);
#endif

    return 1;
}

/**
 * @brief Process one raw complex sample through the bounded CIC decimator.
 *
 * The CIC transfer function is implemented as `CIC_STAGES` cascaded moving
 * sums of length `decim_factor`, followed by decimation. This is equivalent to
 * a 3-stage integrator/comb decimator with differential delay 1, but all state
 * is bounded by `decim_factor` instead of growing with runtime. Input and
 * output values are complex baseband samples; the output is normalized by the
 * CIC DC gain `decim_factor^CIC_STAGES`.
 *
 * @param ctx Active scan context with CIC rings allocated.
 * @param in_re Raw input real component.
 * @param in_im Raw input imaginary component.
 * @param out_re Normalized decimated output real component.
 * @param out_im Normalized decimated output imaginary component.
 * @return 1 when this raw sample completed one decimated output, else 0.
 */
static int bounded_cic_decimate_sample(scan_ctx_t *ctx, double in_re,
                                       double in_im, double *out_re,
                                       double *out_im)
{
    int decim = ctx->decim_factor;
    int pos = ctx->cic_delay_pos;
    double stage_re = in_re;
    double stage_im = in_im;
    double gain = ctx->cic_dc_gain;

    if (decim <= 1 || !ctx->cic_delay || gain <= 0.0)
        return 0;

    for (int s = 0; s < CIC_STAGES; s++) {
        size_t offset = ((size_t)s * (size_t)decim + (size_t)pos) * 2U;
        double old_re = ctx->cic_delay[offset];
        double old_im = ctx->cic_delay[offset + 1U];

        ctx->cic_sum_re[s] += stage_re - old_re;
        ctx->cic_sum_im[s] += stage_im - old_im;
        ctx->cic_delay[offset] = stage_re;
        ctx->cic_delay[offset + 1U] = stage_im;
        stage_re = ctx->cic_sum_re[s];
        stage_im = ctx->cic_sum_im[s];
    }

    pos++;
    if (pos >= decim)
        pos = 0;
    ctx->cic_delay_pos = pos;

    ctx->decim_phase++;
    if (ctx->decim_phase < decim)
        return 0;
    ctx->decim_phase = 0;

    *out_re = stage_re / gain;
    *out_im = stage_im / gain;
    return 1;
}

/**
 * @brief Process raw samples through CIC decimation and FFT magnitude.
 *
 * CIC samples are produced by `bounded_cic_decimate_sample()`, a bounded
 * moving-sum realization of the documented 3-stage CIC response. That avoids
 * unbounded integrator growth while preserving the same `decim^3` DC gain and
 * per-bin droop compensation model.
 *
 * The async refill size normally equals one raw-sample line advance, so the
 * steady-state worker receives one FFT hop per buffer. When overlap or capped
 * refill sizes produce multiple FFT completions from one callback, each
 * completion is published or maximum-rate-throttled independently; no FFT
 * result is overwritten or averaged into another line.
 *
 * @param ctx Active scan context.
 * @param buf Interleaved complex input samples.
 * @param buf_len Complex sample count.
 * @param bins_per_step Output raw-bin count before display reduction.
 * @param out Magnitude output buffer with `bins_per_step` floats.
 * @return 1 when one FFT frame was written to `out`, 0 when no frame completed.
 */
static void complete_processed_channel(scan_ctx_t *ctx, int channel);

static int cic_decimated_fft_magnitude(scan_ctx_t *ctx, float *buf,
                                       uint32_t buf_len, int bins_per_step,
                                       float *out)
{
    int decim = ctx->decim_factor;
    int produced = 0;

    if (decim <= 1 || !ctx->decim_accum || !ctx->cic_delay)
        return average_fft_magnitude(buf, buf_len, bins_per_step, ctx->fft_size,
                                     ctx->window, ctx->fft_scratch, ctx->mag_scale, 0,
                                     ctx->max_ffts_per_buffer,
                                     out);

    for (uint32_t n = 0; n < buf_len; n++) {
        double re = buf[2*n];
        double im = buf[2*n + 1];

        ctx->cic_raw_samples_in++;
        ctx->cic_account_raw_samples++;
        if (!bounded_cic_decimate_sample(ctx, re, im, &re, &im))
            continue;

        ctx->cic_decim_samples_out++;
        ctx->cic_account_decim_samples++;
        if (ctx->cic_warmup_outputs_remaining > 0) {
            ctx->cic_warmup_outputs_remaining--;
            ctx->cic_account_warmup_outputs++;
            continue;
        }
        ctx->decim_accum[2*ctx->decim_fill] = (float)re;
        ctx->decim_accum[2*ctx->decim_fill + 1] = (float)im;
        ctx->decim_fill++;

        if (ctx->decim_fill >= ctx->fft_size) {
            if (ctx->cic_skip_frames_after_reset > 0) {
                ctx->cic_reset_frames_skipped++;
                ctx->cic_skip_frames_after_reset--;
            } else {
                int ok = fft_magnitude_from_accum(ctx, bins_per_step, out);
                if (ok > 0) {
                    produced++;
                    complete_processed_channel(ctx, 0);
                }
            }
            ctx->cic_fft_frames_completed++;
            ctx->cic_account_fft_frames++;
            advance_decim_frame(ctx);
        }
    }

    if (!cic_check_continuity_accounting(ctx))
        return 0;
    return produced;
}

static void prepare_direct_sampling_samples(scan_ctx_t *ctx, float *buf, uint32_t buf_len)
{
    uint32_t direct_sampling = ctx->direct_sampling;

    if (direct_sampling == 0)
        return;

    if (ctx->decim_factor > 1) {
        double phase = ctx->direct_mix_phase;
        double inc = ctx->direct_mix_inc;

        for (uint32_t i = 0; i < buf_len; i++) {
            float sample = (direct_sampling == 2) ? buf[2*i + 1] : buf[2*i];
            buf[2*i] = sample * (float)cos(phase);
            buf[2*i + 1] = -sample * (float)sin(phase);
            phase += inc;
            if (phase >= 2.0 * M_PI)
                phase -= 2.0 * M_PI;
            else if (phase < 0.0)
                phase += 2.0 * M_PI;
        }

        ctx->direct_mix_phase = phase;
        return;
    }

    if (direct_sampling == 1) {
        for (uint32_t i = 0; i < buf_len; i++)
            buf[2*i + 1] = 0.0f;
    } else if (direct_sampling == 2) {
        for (uint32_t i = 0; i < buf_len; i++) {
            buf[2*i] = buf[2*i + 1];
            buf[2*i + 1] = 0.0f;
        }
    }
}

/**
 * @brief Return the median Rayleigh magnitude after selecting the peak of N bins.
 *
 * White complex Gaussian FFT bins have Rayleigh magnitudes. The exact median
 * of the peak of `count` independent bins makes peak-per-pixel reduction less
 * bright at low zoom without reducing a narrow signal's peak sensitivity.
 *
 * @param count Number of raw FFT bins reduced into one display pixel.
 * @return Median magnitude in units of the underlying Rayleigh sigma.
 */
static double rayleigh_peak_median(int count)
{
    double root;
    double tail;

    if (count <= 1)
        return sqrt(2.0 * log(2.0));
    root = exp(log(0.5) / (double)count);
    tail = 1.0 - root;
    if (tail <= 0.0)
        return sqrt(2.0 * log(2.0));
    return sqrt(-2.0 * log(tail));
}

/**
 * @brief Return a display-only normalization for peak-per-pixel aggregation.
 *
 * @param raw_bins Number of FFT/CIC bins contributing to one screen pixel.
 * @return Dimensionless factor that keeps median white-noise brightness near
 *         the one-bin reference while retaining peak aggregation for signals.
 */
static float peak_reducer_noise_scale(int raw_bins)
{
    double one = rayleigh_peak_median(1);
    double many = rayleigh_peak_median(raw_bins);

    if (many <= 0.0 || !isfinite(many))
        return 1.0f;
    return (float)(one / many);
}

/**
 * @brief Estimate the strongest visible raw FFT/CIC bin coordinate.
 *
 * The result is diagnostic metadata for live frequency validation. Hidden
 * source-span bins outside the visible interval are ignored so zero-IF/DC
 * artifacts cannot replace the carrier the user is actually inspecting. The
 * peak coordinate uses a three-point log-magnitude parabola when adjacent bins
 * are available; the interpolation is bounded to one half-bin so noise or an
 * edge maximum cannot invent a coordinate outside the selected source bin.
 *
 * @param values Raw magnitude bins in increasing source-frequency order.
 * @param count Number of valid raw bins.
 * @param start_hz Source-coordinate frequency at the lower bin edge, in hertz.
 * @param span_hz Source-coordinate span represented by `values`, in hertz.
 * @param visible_start_hz Visible interval lower edge in hertz.
 * @param visible_end_hz Visible interval upper edge in hertz.
 * @return Interpolated peak source coordinate in hertz, or zero when invalid.
 */
static double raw_peak_frequency_hz(const float *values, int count,
                                    double start_hz, double span_hz,
                                    double visible_start_hz,
                                    double visible_end_hz)
{
    int peak_index = 0;
    int first;
    int last;
    float peak = 0.0f;
    double bin_fraction = 0.5;

    if (!values || count <= 0 || !isfinite(start_hz) ||
        !isfinite(span_hz) || span_hz <= 0.0 ||
        !isfinite(visible_start_hz) || !isfinite(visible_end_hz) ||
        visible_end_hz <= visible_start_hz)
        return 0.0;

    first = (int)floor(((visible_start_hz - start_hz) / span_hz) *
                       (double)count);
    last = (int)ceil(((visible_end_hz - start_hz) / span_hz) *
                     (double)count);
    if (first < 0)
        first = 0;
    if (last > count)
        last = count;
    if (last <= first)
        return 0.0;

    for (int i = first; i < last; i++) {
        if (isfinite(values[i]) && values[i] > peak) {
            peak = values[i];
            peak_index = i;
        }
    }
    if (!(peak > 0.0f))
        return 0.0;

    if (peak_index > 0 && peak_index + 1 < count &&
        values[peak_index - 1] > 0.0f && values[peak_index + 1] > 0.0f) {
        double left = log((double)values[peak_index - 1]);
        double center = log((double)values[peak_index]);
        double right = log((double)values[peak_index + 1]);
        double denominator = left - 2.0 * center + right;
        double offset;

        if (isfinite(denominator) && fabs(denominator) > 1.0e-12) {
            offset = 0.5 * (left - right) / denominator;
            if (isfinite(offset))
                bin_fraction += fmax(-0.5, fmin(0.5, offset));
        }
    }
    return start_hz + ((double)peak_index + bin_fraction) *
        span_hz / (double)count;
}

/**
 * @brief Convert normalized FFT magnitude into the 8-bit waterfall transport.
 *
 * Input magnitudes are already corrected for Hann coherent gain and optional
 * CIC DC/droop effects. `publish_scan_line()` additionally applies the
 * documented display-only noise-density factor before this common dB window.
 *
 * @param mag Linear normalized magnitude.
 * @return Packed unsigned waterfall level.
 */
static uint8_t magnitude_to_u8(float mag)
{
    float db = 20.0f * log10f(mag + 1e-20f);
    float v = (db - DB_FLOOR) * (255.0f / (DB_CEIL - DB_FLOOR));
    if (v < 0.0f) v = 0.0f;
    if (v > 255.0f) v = 255.0f;
    return (uint8_t)lrintf(v);
}

/**
 * @brief Compute encoded base64 length for packed waterfall bytes.
 *
 * @param len Number of raw bytes.
 * @return Number of base64 characters, excluding the terminating NUL.
 */
static size_t base64_encoded_len(size_t len)
{
    return ((len + 2U) / 3U) * 4U;
}

/**
 * @brief Encode packed uint8 waterfall bytes as base64.
 *
 * @param src Source byte array.
 * @param len Number of source bytes.
 * @param dst Destination buffer with at least `base64_encoded_len(len)` bytes.
 */
static void base64_encode_u8(const uint8_t *src, size_t len, char *dst)
{
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t in = 0;
    size_t out = 0;

    while (in < len) {
        size_t chunk = len - in;
        uint32_t a = src[in++];
        uint32_t b = chunk > 1 ? src[in++] : 0U;
        uint32_t c = chunk > 2 ? src[in++] : 0U;
        uint32_t triple = (a << 16) | (b << 8) | c;

        dst[out++] = table[(triple >> 18) & 0x3fU];
        dst[out++] = table[(triple >> 12) & 0x3fU];
        dst[out++] = chunk > 1 ? table[(triple >> 6) & 0x3fU] : '=';
        dst[out++] = chunk > 2 ? table[triple & 0x3fU] : '=';
    }
}

/**
 * @brief Publish one exact-width waterfall row to every SSE client.
 *
 * `ctx->line_buf` contains the raw stitched FFT/CIC product. This function
 * reduces that raw product into exactly `display_bins` processed output values
 * using peak-per-pixel aggregation over the visible frequency span. The SSE
 * field `source_bins` remains the historical published output width for
 * compatibility, `published_bins` names that width explicitly, and
 * `raw_source_bins` exposes the pre-reduction FFT/CIC bin count for tests and
 * diagnostics.
 *
 * @param ctx Active scan context with a complete raw line buffer.
 */
static void publish_scan_line(scan_ctx_t *ctx)
{
    int source_bins = ctx->line_bins;
    int display_bins = current_display_bins();
    double source_span = ctx->scan_end - ctx->scan_start;
    double visible_span = ctx->visible_end - ctx->visible_start;
    uint8_t *line_packed;
    char *json;
    char *packed_b64;
    int pos;
    int prefix_len;
    size_t packed_len;
    size_t json_size;
    double raw_peak_hz;
    long long now;
    long long live_capture_age_ms = 0;
    long long live_first_input_ms = 0;
    long long live_first_fft_ms = 0;
    long long live_first_publish_ms = 0;
    const char *prefix_fmt =
        "event: line\ndata: {\"view\":%u,\"n\":%d,\"b\":%d,"
        "\"mode\":\"%s\","
        "\"f0\":" JSON_COORD_FMT ",\"f1\":" JSON_COORD_FMT ","
        "\"full_f0\":" JSON_COORD_FMT ",\"full_f1\":" JSON_COORD_FMT ","
        "\"visible_start_hz\":" JSON_COORD_FMT ",\"visible_end_hz\":" JSON_COORD_FMT ","
        "\"second_if_hz\":" JSON_COORD_FMT ",\"zero_if_guard_hz\":" JSON_COORD_FMT ","
        "\"fq_err_correction_hz\":%.9f,"
        "\"raw_peak_hz\":%.9f,"
        "\"live_capture_age_ms\":%lld,"
        "\"live_first_input_ms\":%lld,\"live_first_fft_ms\":%lld,"
        "\"live_first_publish_ms\":%lld,"
        "\"display_bins\":%d,\"source_bins\":%d,\"published_bins\":%d,"
        "\"raw_source_bins\":%d,"
        "\"effective_fft_size\":%d,"
        "\"decim_factor\":%d,\"decim_hop\":%d,\"overlap_factor\":%.3f,"
        "\"minimum_rate_overlap\":%d,\"minimum_rate_limited\":%d,"
        "\"minimum_rate_achieved\":%d,"
        "\"true_line_rate\":%.3f,"
        "\"preview_interval_ms\":%.3f,"
        "\"waterfall_noise_scale\":%.6f,"
        "\"visible_raw_bins\":%.3f,\"visible_bins_per_pixel\":%.6f,"
        "\"preview\":%d,\"preview_age_ms\":%lld,"
        "\"preview_sequence\":%d,\"preview_count\":%d,"
        "\"encoding\":\"u8b64\",\"db\":\"";

    if (source_bins <= 0 || display_bins <= 0 || source_span <= 0.0 || visible_span <= 0.0)
        return;

    now = now_msec();
    if (!ctx->preview_mode && ctx->live_first_publish_msec == 0)
        ctx->live_first_publish_msec = now;
    if (ctx->live_capture_started_msec > 0) {
        live_capture_age_ms = now - ctx->live_capture_started_msec;
        if (ctx->live_first_input_msec > 0)
            live_first_input_ms = ctx->live_first_input_msec -
                ctx->live_capture_started_msec;
        if (ctx->live_first_fft_msec > 0)
            live_first_fft_ms = ctx->live_first_fft_msec -
                ctx->live_capture_started_msec;
        if (ctx->live_first_publish_msec > 0)
            live_first_publish_ms = ctx->live_first_publish_msec -
                ctx->live_capture_started_msec;
    }
    raw_peak_hz = raw_peak_frequency_hz(ctx->line_buf, source_bins,
                                        ctx->scan_start, source_span,
                                        ctx->visible_start,
                                        ctx->visible_end);

    line_packed = malloc((size_t)display_bins);
    if (!line_packed)
        return;

    for (int x = 0; x < display_bins; x++) {
        double pixel_start = ctx->visible_start + ((double)x / (double)display_bins) * visible_span;
        double pixel_end = ctx->visible_start + ((double)(x + 1) / (double)display_bins) * visible_span;
        int first = (int)floor(((pixel_start - ctx->scan_start) / source_span) * (double)source_bins);
        int last = (int)ceil(((pixel_end - ctx->scan_start) / source_span) * (double)source_bins);
        float peak = 0.0f;

        if (first < 0) first = 0;
        if (first >= source_bins) first = source_bins - 1;
        if (last < first + 1) last = first + 1;
        if (last > source_bins) last = source_bins;

        for (int i = first; i < last; i++) {
            if (ctx->line_buf[i] > peak)
                peak = ctx->line_buf[i];
        }
        line_packed[x] = magnitude_to_u8(
            peak * ctx->waterfall_noise_scale *
            peak_reducer_noise_scale(last - first));
    }

    prefix_len = snprintf(NULL, 0, prefix_fmt,
        ctx->view_id, ctx->line_num, ctx->total_steps,
        ctx->single_mode ? "single" : "scan",
        ctx->visible_start, ctx->visible_end,
        ctx->configured_start, ctx->configured_end,
        ctx->visible_start, ctx->visible_end,
        ctx->second_if_hz, ctx->zero_if_guard_hz,
        ctx->frequency_correction_hz,
        raw_peak_hz, live_capture_age_ms, live_first_input_ms,
        live_first_fft_ms, live_first_publish_ms,
        display_bins, display_bins, display_bins, source_bins, ctx->fft_size,
        ctx->decim_factor, ctx->decim_hop, ctx->overlap_factor,
        ctx->minimum_rate_limited, ctx->minimum_rate_limited,
        ctx->minimum_rate_achieved,
        ctx_true_line_rate(ctx),
        ctx->preview_interval_ms, ctx->waterfall_noise_scale,
        ctx->visible_raw_bins, ctx->visible_bins_per_pixel,
        ctx->preview_mode, ctx->preview_age_ms,
        ctx->preview_sequence, ctx->preview_count);
    if (prefix_len < 0) {
        free(line_packed);
        return;
    }
    packed_len = base64_encoded_len((size_t)display_bins);
    json_size = (size_t)prefix_len + packed_len + 6;
    json = malloc(json_size);
    if (!json) {
        free(line_packed);
        return;
    }
    packed_b64 = json + prefix_len;

    pos = snprintf(json, json_size, prefix_fmt,
        ctx->view_id, ctx->line_num, ctx->total_steps,
        ctx->single_mode ? "single" : "scan",
        ctx->visible_start, ctx->visible_end,
        ctx->configured_start, ctx->configured_end,
        ctx->visible_start, ctx->visible_end,
        ctx->second_if_hz, ctx->zero_if_guard_hz,
        ctx->frequency_correction_hz,
        raw_peak_hz, live_capture_age_ms, live_first_input_ms,
        live_first_fft_ms, live_first_publish_ms,
        display_bins, display_bins, display_bins, source_bins, ctx->fft_size,
        ctx->decim_factor, ctx->decim_hop, ctx->overlap_factor,
        ctx->minimum_rate_limited, ctx->minimum_rate_limited,
        ctx->minimum_rate_achieved,
        ctx_true_line_rate(ctx),
        ctx->preview_interval_ms, ctx->waterfall_noise_scale,
        ctx->visible_raw_bins, ctx->visible_bins_per_pixel,
        ctx->preview_mode, ctx->preview_age_ms,
        ctx->preview_sequence, ctx->preview_count);

    if (pos < 0 || (size_t)pos != (size_t)prefix_len) {
        free(line_packed);
        free(json);
        return;
    }
    base64_encode_u8(line_packed, (size_t)display_bins, packed_b64);
    pos += (int)packed_len;
    pos += snprintf(json + pos, json_size - (size_t)pos, "\"}\n\n");
    if (sse_broadcast(json, pos) > 0)
        record_frontend_traffic((size_t)pos);
    free(line_packed);
    free(json);
}

static int rate_limiter_should_keep(scan_ctx_t *ctx);

/**
 * @brief Enforce the configured maximum waterfall publish rate by wall clock.
 *
 * The existing ratio limiter drops callbacks before or after FFT work based on
 * empirical raw-rate estimates. This final guard uses actual publish time so
 * short-buffer single-frequency profiles cannot exceed the visible waterfall
 * rate cap when the Pluto transport is faster than the estimate.
 *
 * @param ctx Active scan context.
 * @return Nonzero when the completed row may be published.
 */
static int publish_clock_should_keep(scan_ctx_t *ctx)
{
    uint32_t limit_lps;
    double target_lps;
    double min_interval_ms;
    long long now;

    if (!ctx)
        return 1;
    if (ctx->preview_mode)
        return 1;
    limit_lps = normalize_rate_limit_lps(ctx->rate_limit_lps);
    if (!ctx->single_mode || ctx->estimated_line_rate <= (double)limit_lps)
        return 1;

    target_lps = (double)limit_lps * PLUTO_RATE_LIMIT_GUARD;
    if (target_lps <= 0.0)
        return 1;
    min_interval_ms = 1000.0 / target_lps;
    now = now_msec();
    if (ctx->last_publish_msec > 0 &&
        (double)(now - ctx->last_publish_msec) < min_interval_ms) {
        ctx->rate_output_dropped++;
        return 0;
    }
    ctx->last_publish_msec = now;
    return 1;
}

static int should_publish_processed_line(scan_ctx_t *ctx)
{
    if (ctx->preview_mode)
        return 1;
    if (ctx->single_mode && ctx->decim_factor > 1 &&
        ctx->rate_keep_ratio < 0.999999) {
        if (rate_limiter_should_keep(ctx))
            return 1;
        ctx->rate_output_dropped++;
        return 0;
    }
    return 1;
}

/**
 * @brief Complete one independently computed channel/FFT product.
 *
 * In single CIC mode this is called once per overlapping FFT completion, which
 * guarantees that no result is overwritten by another completion in the same
 * raw input buffer. Maximum-rate throttling may drop the complete line after
 * FFT, but raw samples have already advanced the continuous CIC exactly once.
 *
 * @param ctx Active scan context.
 * @param channel Scan slot index, or zero for single-frequency mode.
 */
static void complete_processed_channel(scan_ctx_t *ctx, int channel)
{
    int preview_completion;

    if (!ctx || channel < 0 || channel >= ctx->total_steps)
        return;
    preview_completion = ctx->preview_mode;
    if (!preview_completion && ctx->live_first_fft_msec == 0)
        ctx->live_first_fft_msec = now_msec();
    if (!should_publish_processed_line(ctx)) {
        if (preview_completion)
            ctx->preview_sequence++;
        return;
    }

    if (!ctx->step_seen[channel]) {
        ctx->step_seen[channel] = 1;
        ctx->steps_seen++;
    }

    if (ctx->steps_seen >= ctx->total_steps) {
        if (publish_clock_should_keep(ctx))
            publish_scan_line(ctx);
        memset(ctx->step_seen, 0, sizeof(ctx->step_seen));
        ctx->steps_seen = 0;
        ctx->line_num++;
    }
    if (preview_completion)
        ctx->preview_sequence++;
}

static int rate_limiter_should_keep(scan_ctx_t *ctx)
{
    double ratio = ctx->rate_keep_ratio;

    if (ratio <= 0.0 || ratio >= 0.999999)
        return 1;

    ctx->rate_keep_credit += ratio;
    if (ctx->rate_keep_credit >= 1.0) {
        ctx->rate_keep_credit -= 1.0;
        if (ctx->rate_keep_credit > 1.0)
            ctx->rate_keep_credit = fmod(ctx->rate_keep_credit, 1.0);
        return 1;
    }
    return 0;
}

/**
 * process_scan_buffer:
 * Worker-thread entry for one captured Pluto buffer. In scan/hop mode each
 * channel fills its slot in the stitched row. In single mode channel 0 produces
 * the whole row, optionally after CIC decimation.
 */
static void process_scan_buffer(scan_ctx_t *ctx, float *buf, uint32_t buf_len,
                                int channel, uint32_t flags)
{
    int channel_bins;
    float *slot;

    if (channel < 0 || channel >= ctx->total_steps)
        return;

    if (ctx->fft_generation != current_fft_generation()) {
        if (scan_ctx_apply_fft_config(ctx) != 0) {
            fprintf(stderr, "[SDR] Failed to apply FFT size %d\n", current_fft_size());
            return;
        }
    }

    if (ctx->single_mode && ctx->decim_factor > 1 && flags != 0) {
        if (flags & PLUTO_REFILL_FLAG_RECOVERED)
            ctx->cic_refill_recoveries++;
        if (flags & PLUTO_REFILL_FLAG_SHORT_READ)
            ctx->cic_short_reads++;
        ctx->cic_discontinuities++;
        cic_reset_state(ctx, 1);
    }

    if (ctx->direct_sampling)
        prepare_direct_sampling_samples(ctx, buf, buf_len);

    channel_bins = (channel == ctx->total_steps - 1) ? ctx->last_bins : ctx->bins_per_step;
    slot = ctx->line_buf + (size_t)channel * ctx->bins_per_step;
    if (ctx->single_mode && ctx->decim_factor > 1) {
        if (cic_decimated_fft_magnitude(ctx, buf, buf_len, channel_bins, slot) <= 0)
            return;
        return;
    } else {
        if (average_fft_magnitude(buf, buf_len, channel_bins, ctx->fft_size,
                                  ctx->window, ctx->fft_scratch, ctx->mag_scale,
                                  ctx->direct_sampling != 0 && ctx->decim_factor <= 1,
                                  ctx->max_ffts_per_buffer,
                                  slot) <= 0)
            return;
    }

    complete_processed_channel(ctx, channel);
}

static int normalize_display_bins(int bins)
{
    if (bins < DISPLAY_BINS_MIN) bins = DISPLAY_BINS_MIN;
    if (bins > DISPLAY_BINS_MAX) bins = DISPLAY_BINS_MAX;
    return bins;
}

static int current_display_bins(void)
{
    return normalize_display_bins(g_display_bins);
}

static void clamp_visible_to_config(void)
{
    if (direct_sampling_enabled()) {
        force_direct_sampling_defaults(0);
        return;
    }

    if (g_freq_start < MIN_FREQ_START_HZ)
        g_freq_start = MIN_FREQ_START_HZ;
    if (g_freq_end <= g_freq_start)
        g_freq_end = g_freq_start + g_samplerate;

    if (g_visible_start < g_freq_start)
        g_visible_start = g_freq_start;
    if (g_visible_end > g_freq_end)
        g_visible_end = g_freq_end;
    if (g_visible_end <= g_visible_start) {
        g_visible_start = g_freq_start;
        g_visible_end = g_freq_end;
    }
}

static void active_scan_band(double *out_start, double *out_end)
{
    double start = g_visible_start;
    double end = g_visible_end;
    double step = scan_step_hz();
    double config_span;
    double min_span;
    double center;

    clamp_visible_to_config();
    start = g_visible_start;
    end = g_visible_end;

    if (step <= 0.0 || end <= start) {
        *out_start = start;
        *out_end = end;
        return;
    }

    config_span = g_freq_end - g_freq_start;
    min_span = step * (double)PLUTO_MIN_FREQS_CNT;
    if (config_span < min_span)
        min_span = config_span;

    if (end - start < min_span && min_span > 0.0) {
        center = (start + end) * 0.5;
        start = center - min_span * 0.5;
        end = start + min_span;
        if (start < g_freq_start) {
            end += g_freq_start - start;
            start = g_freq_start;
        }
        if (end > g_freq_end) {
            start -= end - g_freq_end;
            end = g_freq_end;
        }
        if (start < g_freq_start)
            start = g_freq_start;
    }

    *out_start = start;
    *out_end = end;
}

static int scan_queue_init(scan_ctx_t *ctx, uint32_t sample_capacity)
{
    if (pthread_mutex_init(&ctx->queue_mutex, NULL) != 0)
        return -1;
    if (pthread_cond_init(&ctx->queue_cond, NULL) != 0) {
        pthread_mutex_destroy(&ctx->queue_mutex);
        return -1;
    }

    ctx->async_buf_len = (int)sample_capacity;
    for (int i = 0; i < PROCESS_QUEUE_LEN; i++) {
        ctx->queue[i].samples = malloc((size_t)sample_capacity * 2 * sizeof(float));
        if (!ctx->queue[i].samples) {
            for (int j = 0; j < i; j++) {
                free(ctx->queue[j].samples);
                ctx->queue[j].samples = NULL;
            }
            pthread_cond_destroy(&ctx->queue_cond);
            pthread_mutex_destroy(&ctx->queue_mutex);
            return -1;
        }
    }
    return 0;
}

static void scan_queue_destroy(scan_ctx_t *ctx)
{
    for (int i = 0; i < PROCESS_QUEUE_LEN; i++) {
        free(ctx->queue[i].samples);
        ctx->queue[i].samples = NULL;
    }
    pthread_cond_destroy(&ctx->queue_cond);
    pthread_mutex_destroy(&ctx->queue_mutex);
}

static void scan_queue_stop(scan_ctx_t *ctx)
{
    pthread_mutex_lock(&ctx->queue_mutex);
    ctx->worker_stop = 1;
    pthread_cond_broadcast(&ctx->queue_cond);
    pthread_mutex_unlock(&ctx->queue_mutex);
}

/**
 * @brief Return whether queued buffers must remain contiguous for processing.
 *
 * CIC decimation is stateful across buffers. Dropping a raw buffer before the
 * CIC path creates a time discontinuity and can smear narrow signals across
 * FFT bins, so single-frequency CIC mode waits for queue space instead.
 *
 * @param ctx Active scan context.
 * @return Nonzero when software queue drops are forbidden.
 */
static int scan_queue_requires_contiguous_samples(const scan_ctx_t *ctx)
{
    return ctx && ctx->single_mode && ctx->decim_factor > 1;
}

/**
 * @brief Queue a captured sample buffer for FFT/CIC processing.
 *
 * Non-CIC paths keep the bounded queue's historical drop-on-full behavior.
 * Single-frequency CIC mode waits for queue space because dropped raw buffers
 * introduce discontinuities into the stateful decimator and widen signals in
 * the waterfall.
 *
 * @param ctx Active scan context.
 * @param channel Scan step index, or 0 in single-frequency mode.
 * @param buf Interleaved complex samples.
 * @param buf_len Complex sample count in `buf`.
 * @param flags Refill/discontinuity flags consumed in FIFO order.
 * @param sample_index_start Logical source index of the first sample.
 * @param sample_index_end Logical source index one past the final sample.
 */
static void scan_queue_push(scan_ctx_t *ctx, int channel, float *buf,
                            uint32_t buf_len, uint32_t flags,
                            uint64_t sample_index_start,
                            uint64_t sample_index_end)
{
    sample_queue_item_t *item;
    int require_contiguous = scan_queue_requires_contiguous_samples(ctx);
    uint64_t block_sequence;

    if (buf_len > (uint32_t)ctx->async_buf_len)
        buf_len = (uint32_t)ctx->async_buf_len;

    pthread_mutex_lock(&ctx->queue_mutex);
    while (ctx->queue_len >= PROCESS_QUEUE_LEN && require_contiguous &&
           !ctx->worker_stop && g_scanning) {
        ctx->cic_queue_waits++;
        pthread_cond_wait(&ctx->queue_cond, &ctx->queue_mutex);
    }
    if (ctx->queue_len >= PROCESS_QUEUE_LEN) {
        if (!require_contiguous)
            ctx->queue_dropped++;
        pthread_mutex_unlock(&ctx->queue_mutex);
        return;
    }
    if (ctx->worker_stop || !g_scanning) {
        pthread_mutex_unlock(&ctx->queue_mutex);
        return;
    }

    item = &ctx->queue[ctx->queue_tail];
    item->ready = 0;
    block_sequence = ctx->capture_block_sequence++;
    ctx->queue_tail = (ctx->queue_tail + 1) % PROCESS_QUEUE_LEN;
    ctx->queue_len++;
    pthread_mutex_unlock(&ctx->queue_mutex);

    memcpy(item->samples, buf, (size_t)buf_len * 2 * sizeof(float));

    pthread_mutex_lock(&ctx->queue_mutex);
    item->buf_len = buf_len;
    item->channel = channel;
    item->flags = flags;
    item->block_sequence = block_sequence;
    item->sample_index_start = sample_index_start;
    item->sample_index_end = sample_index_end;
    item->ready = 1;
    pthread_cond_signal(&ctx->queue_cond);
    pthread_mutex_unlock(&ctx->queue_mutex);
}

/**
 * @brief Verify FIFO block order and adjacent logical sample ranges in CIC mode.
 *
 * Hardware has no per-sample timestamp, so this proves continuity only from
 * callback acceptance through the software queue. Synthetic tone mode supplies
 * independently advanced source indices and can therefore expose injected
 * sample skips or repetitions as range-length mismatches.
 *
 * @param ctx Active scan context.
 * @param item Queue item about to be processed.
 */
static void scan_queue_check_cic_sequence(scan_ctx_t *ctx,
                                          const sample_queue_item_t *item)
{
    int valid = 1;

    if (!scan_queue_requires_contiguous_samples(ctx) || !item)
        return;

    if (item->sample_index_end < item->sample_index_start ||
        item->sample_index_end - item->sample_index_start != item->buf_len)
        valid = 0;
    if (ctx->worker_sequence_initialized &&
        (item->block_sequence != ctx->worker_expected_block_sequence ||
         item->sample_index_start != ctx->worker_expected_sample_index))
        valid = 0;

    if (!valid) {
        ctx->cic_sample_order_errors++;
        if (ctx->cic_sample_order_errors <= 5) {
            fprintf(stderr,
                    "[SDR] CIC sample-order mismatch: block %llu expected %llu, range %llu..%llu expected start %llu, len %u\n",
                    (unsigned long long)item->block_sequence,
                    (unsigned long long)ctx->worker_expected_block_sequence,
                    (unsigned long long)item->sample_index_start,
                    (unsigned long long)item->sample_index_end,
                    (unsigned long long)ctx->worker_expected_sample_index,
                    item->buf_len);
        }
    }

    ctx->worker_expected_block_sequence = item->block_sequence + 1U;
    ctx->worker_expected_sample_index = item->sample_index_end;
    ctx->worker_sequence_initialized = 1;
}

static void *scan_worker_thread(void *arg)
{
    scan_ctx_t *ctx = (scan_ctx_t *)arg;

    for (;;) {
        sample_queue_item_t *item;

        pthread_mutex_lock(&ctx->queue_mutex);
        while (ctx->queue_len == 0 || !ctx->queue[ctx->queue_head].ready) {
            if (ctx->worker_stop) {
                pthread_mutex_unlock(&ctx->queue_mutex);
                return NULL;
            }
            pthread_cond_wait(&ctx->queue_cond, &ctx->queue_mutex);
        }
        item = &ctx->queue[ctx->queue_head];
        pthread_mutex_unlock(&ctx->queue_mutex);

        scan_queue_check_cic_sequence(ctx, item);
        process_scan_buffer(ctx, item->samples, item->buf_len, item->channel,
                            item->flags);

        pthread_mutex_lock(&ctx->queue_mutex);
        item->ready = 0;
        ctx->queue_head = (ctx->queue_head + 1) % PROCESS_QUEUE_LEN;
        ctx->queue_len--;
        pthread_cond_signal(&ctx->queue_cond);
        pthread_mutex_unlock(&ctx->queue_mutex);
    }
}

static int scan_callback_should_process(scan_ctx_t *ctx, int channel)
{
    if (ctx->rate_keep_ratio >= 0.999999)
        return 1;

    if (ctx->single_mode) {
        if (ctx->decim_factor > 1)
            return 1;
        ctx->rate_callback_seq++;
        if (rate_limiter_should_keep(ctx))
            return 1;
        ctx->rate_dropped++;
        return 0;
    }

    if (channel == 0) {
        uint64_t seq = ctx->rate_scan_cycle_seq++;
        ctx->rate_have_cycle = 1;
        (void)seq;
        ctx->rate_drop_cycle = !rate_limiter_should_keep(ctx);
    } else if (!ctx->rate_have_cycle) {
        ctx->rate_drop_cycle = 0;
    }

    if (ctx->rate_drop_cycle) {
        ctx->rate_dropped++;
        return 0;
    }
    return 1;
}

#if !PSEUDO_RANDOM_SAMPLE_SOURCE
static void scan_callback(float *buf, uint32_t buf_len, uint32_t refill_flags,
                          struct pluto_sdr_dev_t *dev, void *user)
{
    scan_ctx_t *ctx = (scan_ctx_t *)user;
    int channel;
    uint32_t flags;
    uint64_t sample_index_start;
    uint64_t sample_index_end;

    (void)dev;

    if (!g_scanning) {
        return;
    }

    ctx->last_callback_msec = now_msec();
    if (ctx->live_capture_started_msec > 0 &&
        ctx->live_first_input_msec == 0)
        ctx->live_first_input_msec = ctx->last_callback_msec;
    channel = ctx->single_mode ? 0 : pluto_sdr_get_scan_index(dev);
    if (channel < 0) {
        ctx->tuning_skipped++;
        return;
    }
    if (channel >= ctx->total_steps)
        return;
    if (ctx->single_mode && ctx->decim_factor > 1 &&
        buf_len != (uint32_t)ctx->async_buf_len)
        refill_flags |= PLUTO_REFILL_FLAG_SHORT_READ;
    recent_sample_cache_append(ctx, buf, buf_len, refill_flags);
    if (!scan_callback_should_process(ctx, channel))
        return;

    flags = refill_flags | ctx->pending_refill_flags;
    ctx->pending_refill_flags = 0;
    sample_index_start = ctx->capture_sample_next;
    sample_index_end = sample_index_start + (uint64_t)buf_len;
    ctx->capture_sample_next = sample_index_end;
    scan_queue_push(ctx, channel, buf, buf_len, flags,
                    sample_index_start, sample_index_end);
}
#endif

#if PSEUDO_RANDOM_SAMPLE_SOURCE
#if PSEUDO_RANDOM_SAMPLE_SOURCE != SYNTHETIC_TONE_SAMPLE_SOURCE
static uint32_t pseudo_random_next(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x ? x : 0x9e3779b9U;
    return *state;
}

static void fill_pseudo_random_samples(float *buf, uint32_t buf_len,
                                       uint32_t *state)
{
    for (uint32_t i = 0; i < buf_len * 2U; i++) {
        uint8_t byte = (uint8_t)(pseudo_random_next(state) >> 24);
        buf[i] = ((float)byte - 127.5f) / 128.0f;
    }
}
#else
typedef struct {
    uint64_t source_index;
    uint64_t generated_count;
    uint64_t fault_period;
    int fault_kind;
    double phase_increment;
} synthetic_tone_state_t;

/** Return a positive integer environment value, or zero when disabled. */
static uint64_t synthetic_env_u64(const char *name)
{
    const char *value = getenv(name);
    char *end = NULL;
    unsigned long long parsed;

    if (!value || !*value)
        return 0;
    errno = 0;
    parsed = strtoull(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0')
        return 0;
    return (uint64_t)parsed;
}

/**
 * @brief Generate one continuous complex tone with optional sample faults.
 *
 * `source_index` advances independently of output slots. A skip advances it
 * twice for one slot; a duplicate emits the preceding source sample and does
 * not advance it for that slot. Queue range metadata therefore detects both
 * fault types in addition to their FFT leakage.
 *
 * @param buf Output interleaved complex samples.
 * @param buf_len Complex output sample count.
 * @param state Persistent phase/source-index state across refills.
 */
static void fill_synthetic_tone_samples(float *buf, uint32_t buf_len,
                                        synthetic_tone_state_t *state)
{
    const float amplitude = 0.25f;

    for (uint32_t i = 0; i < buf_len; i++) {
        uint64_t logical_index = state->source_index;
        int inject = state->fault_period > 0 && state->generated_count > 0 &&
            state->generated_count % state->fault_period == 0;

        if (inject && state->fault_kind == 1) {
            state->source_index++;
            logical_index = state->source_index;
        } else if (inject && state->fault_kind == 2 && state->source_index > 0) {
            logical_index = state->source_index - 1U;
        }

        {
            double phase = fmod(state->phase_increment * (double)logical_index,
                                2.0 * M_PI);
            buf[2U * i] = amplitude * (float)cos(phase);
            buf[2U * i + 1U] = amplitude * (float)sin(phase);
        }

        if (!(inject && state->fault_kind == 2 && state->source_index > 0))
            state->source_index++;
        state->generated_count++;
    }
}
#endif

static void sleep_for_sample_count(uint32_t sample_count, double samplerate)
{
    struct timespec ts;
    double seconds;

    if (sample_count == 0 || samplerate <= 0.0)
        return;

    seconds = (double)sample_count / samplerate;
    ts.tv_sec = (time_t)seconds;
    ts.tv_nsec = (long)((seconds - (double)ts.tv_sec) * 1000000000.0);
    if (ts.tv_nsec < 0)
        ts.tv_nsec = 0;
    if (ts.tv_nsec > 999999999L)
        ts.tv_nsec = 999999999L;
    nanosleep(&ts, NULL);
}

static int run_pseudo_random_sample_source(scan_ctx_t *ctx, uint32_t async_len)
{
    float *buf;
#if PSEUDO_RANDOM_SAMPLE_SOURCE == SYNTHETIC_TONE_SAMPLE_SOURCE
    synthetic_tone_state_t tone_state = {0};
    const char *fault = getenv("PLUTO_SYNTHETIC_FAULT");
    int realtime = synthetic_env_u64("PLUTO_SYNTHETIC_REALTIME") != 0;
#else
    uint32_t state = 0x12345678U;
#endif
    int channel = 0;

    buf = malloc((size_t)async_len * 2U * sizeof(float));
    if (!buf)
        return -1;

#if PSEUDO_RANDOM_SAMPLE_SOURCE == SYNTHETIC_TONE_SAMPLE_SOURCE
    {
        double bin_hz = ctx->samplerate / (double)ctx->fft_size;
        double visible_center = (ctx->visible_start + ctx->visible_end) * 0.5;
        double target_hz = visible_center - ctx->center_freq;

        ctx->synthetic_tone_hz = bin_hz > 0.0 ? round(target_hz / bin_hz) * bin_hz : 0.0;
        tone_state.phase_increment = ctx->raw_samplerate > 0.0 ?
            2.0 * M_PI * ctx->synthetic_tone_hz / ctx->raw_samplerate : 0.0;
        tone_state.fault_period = synthetic_env_u64("PLUTO_SYNTHETIC_FAULT_PERIOD");
        if (fault && strcmp(fault, "skip") == 0)
            tone_state.fault_kind = 1;
        else if (fault && strcmp(fault, "duplicate") == 0)
            tone_state.fault_kind = 2;
        if (tone_state.fault_kind == 0)
            tone_state.fault_period = 0;
        printf("[TEST] Synthetic complex tone running: %.6f Hz, %u samples/buffer, fault %s every %llu samples\n",
               ctx->synthetic_tone_hz, async_len,
               tone_state.fault_kind == 1 ? "skip" :
               (tone_state.fault_kind == 2 ? "duplicate" : "none"),
               (unsigned long long)tone_state.fault_period);
    }
#else
    printf("[SDR] Pseudo-random sample source running (%u complex samples/buffer)\n",
           async_len);
#endif

    while (g_scanning) {
        int current_channel = ctx->single_mode ? 0 : channel;
        uint64_t sample_index_start;
        uint64_t sample_index_end;

#if PSEUDO_RANDOM_SAMPLE_SOURCE == SYNTHETIC_TONE_SAMPLE_SOURCE
        sample_index_start = tone_state.source_index;
        fill_synthetic_tone_samples(buf, async_len, &tone_state);
        sample_index_end = tone_state.source_index;
#else
        sample_index_start = ctx->capture_sample_next;
        fill_pseudo_random_samples(buf, async_len, &state);
        sample_index_end = sample_index_start + (uint64_t)async_len;
        ctx->capture_sample_next = sample_index_end;
#endif
        ctx->last_callback_msec = now_msec();
        if (ctx->live_capture_started_msec > 0 &&
            ctx->live_first_input_msec == 0)
            ctx->live_first_input_msec = ctx->last_callback_msec;

        recent_sample_cache_append(ctx, buf, async_len, 0);

        if (scan_callback_should_process(ctx, current_channel))
            scan_queue_push(ctx, current_channel, buf, async_len, 0,
                            sample_index_start, sample_index_end);

        if (!ctx->single_mode) {
            channel++;
            if (channel >= ctx->total_steps)
                channel = 0;
        }

#if PSEUDO_RANDOM_SAMPLE_SOURCE == SYNTHETIC_TONE_SAMPLE_SOURCE
        if (realtime)
            sleep_for_sample_count(async_len, ctx->raw_samplerate);
#else
        sleep_for_sample_count(async_len, g_samplerate);
#endif
    }

    free(buf);
    return PLUTO_ERR_OK;
}
#endif

static void *scan_watchdog_thread(void *arg)
{
    scan_ctx_t *ctx = (scan_ctx_t *)arg;
    const long long startup_timeout_ms = 8000;
    const long long stall_timeout_ms = 5000;
    const struct timespec sleep_time = { 0, 100000000L };

    while (!ctx->watchdog_stop && g_scanning) {
        long long now = now_msec();
        long long last = ctx->last_callback_msec;
        long long timeout = ctx->line_num == 0 ? startup_timeout_ms : stall_timeout_ms;

        if (last > 0 && now - last > timeout) {
            ctx->watchdog_triggered = 1;
            fprintf(stderr,
                    "[SDR] No sample buffers received for %.1f s; canceling async read\n",
                    (double)(now - last) / 1000.0);
            g_scanning = 0;
            request_async_cancel();
            break;
        }
        nanosleep(&sleep_time, NULL);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Scanning thread                                                     */
/* ------------------------------------------------------------------ */
static void *scan_thread_func(void *arg)
{
    (void)arg;
    double air_freqs[MAX_FREQS];
    double device_freqs[MAX_FREQS];
    double scan_start = 0.0;
    double scan_end = 0.0;
    double step = 0.0;
    double device_center = 0.0;
    run_mode_t mode;
    int total_steps;
    scan_ctx_t ctx;
    int ret;
    int worker_started = 0;
    int device_error = 0;
    int watchdog_started = 0;
    pthread_t watchdog_thread;
    uint32_t async_len;
    uint32_t line_sample_count;
    uint32_t preview_sample_count = 0;
    int preview_line_count = 0;
    unsigned int kernel_buffers;
    single_fft_plan_t single_plan = {0};
    float *preview_samples = NULL;
    long long preview_age_ms = 0;
    double preview_first_line_ms = 0.0;
    double frequency_correction_hz = 0.0;

    mode = planned_run_mode();
    g_active_mode = mode;
    if (mode != RUN_MODE_SINGLE)
        recent_sample_cache_invalidate();

    if (mode == RUN_MODE_SINGLE) {
        double visible_start;
        double visible_end;
        double center;

        raw_visible_band(&visible_start, &visible_end);
        if (visible_end <= visible_start) {
            g_scanning = 0;
            return NULL;
        }
        single_plan = single_fft_plan_for_span(visible_end - visible_start);
        center = direct_sampling_enabled() ?
            direct_source_center_for_visible(visible_start, visible_end,
                                             single_plan.source_span) :
            single_source_center_for_visible(visible_start, visible_end,
                                             single_plan.source_span);
        frequency_correction_hz = direct_sampling_enabled() ? 0.0 :
            configured_frequency_correction_hz(center);
        total_steps = 1;
        step = single_plan.source_span;
        scan_start = center + frequency_correction_hz -
            single_plan.source_span * 0.5;
        scan_end = center + frequency_correction_hz +
            single_plan.source_span * 0.5;
        device_center = direct_sampling_enabled() ? 0.0 : receiver_frequency_from_radio(center);
        if (!direct_sampling_enabled() && !receiver_frequency_valid(device_center)) {
            fprintf(stderr, "[SDR] Converter frequency puts receiver center outside %.0f - %.0f Hz\n",
                    RF_RECEIVER_MIN_HZ, RF_RECEIVER_MAX_HZ);
            g_scanning = 0;
            return NULL;
        }
    } else {
        active_scan_band(&scan_start, &scan_end);
        total_steps = build_scan_frequencies_for_band(scan_start, scan_end, air_freqs, &step);

        if (total_steps < PLUTO_MIN_FREQS_CNT) {
            fprintf(stderr, "[SDR] Hardware scan needs at least %d frequencies\n", PLUTO_MIN_FREQS_CNT);
            g_scanning = 0;
            return NULL;
        }

        if (build_device_scan_frequencies(air_freqs, total_steps, device_freqs) != 0) {
            fprintf(stderr, "[SDR] Converter frequency puts hardware scan outside %.0f - %.0f Hz\n",
                    RF_RECEIVER_MIN_HZ, RF_RECEIVER_MAX_HZ);
            g_scanning = 0;
            return NULL;
        }
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.total_steps = total_steps;
    ctx.single_mode = (mode == RUN_MODE_SINGLE);
    ctx.raw_samplerate = ctx.single_mode ? single_plan.hardware_samplerate : g_samplerate;
    ctx.samplerate = ctx.single_mode ? single_plan.fft_samplerate : g_samplerate;
    ctx.bw_ratio = ctx.single_mode ? single_plan.extraction_ratio : g_bw_ratio;
    ctx.step_width = step;
    ctx.last_width = ctx.single_mode ? (scan_end - scan_start) :
        scan_last_width_for_band(scan_start, scan_end, total_steps, step);
    ctx.configured_start = g_freq_start;
    ctx.configured_end = g_freq_end;
    ctx.visible_start = g_visible_start;
    ctx.visible_end = g_visible_end;
    ctx.scan_start = scan_start;
    ctx.scan_end = ctx.single_mode ? scan_end :
        build_scan_effective_end_for_band(scan_start, scan_end, total_steps, step);
    ctx.center_freq = (scan_start + scan_end) * 0.5;
    ctx.frequency_correction_hz = frequency_correction_hz;
    ctx.zero_if_guard_hz = ctx.single_mode ? single_plan.zero_if_guard_hz : 0.0;
    ctx.second_if_hz = ctx.single_mode ?
        second_if_for_center(ctx.visible_start, ctx.visible_end, ctx.center_freq) : 0.0;
    ctx.view_id = g_view_id;
    ctx.direct_sampling = normalize_direct_sampling(g_direct_sampling);

    if (scan_ctx_apply_fft_config(&ctx) != 0) {
        fprintf(stderr, "[SDR] Out of memory\n");
        g_scanning = 0;
        return NULL;
    }

    if (ctx.single_mode && ctx.decim_factor > 1) {
        line_sample_count = decim_raw_sample_count_for_hop(ctx.decim_hop,
                                                           ctx.decim_factor);
        async_len = single_decim_async_len_for_line(line_sample_count);
    } else {
        async_len = ctx.single_mode ? (uint32_t)ctx.fft_size :
            (uint32_t)((ctx.fft_size > (int)SCAN_BUF_LEN) ? ctx.fft_size : (int)SCAN_BUF_LEN);
        line_sample_count = async_len;
    }
    kernel_buffers = ctx.single_mode ? PLUTO_CONTINUOUS_KERNEL_BUFFERS :
        PLUTO_SCAN_KERNEL_BUFFERS;
    ctx.rate_limit_lps = normalize_rate_limit_lps(g_rate_limit_lps);
    ctx.rate_drop_factor = rate_drop_factor_for_plan(ctx.raw_samplerate, line_sample_count,
                                                     ctx.total_steps,
                                                     ctx.decim_factor,
                                                     ctx.rate_limit_lps,
                                                     &ctx.estimated_line_rate);
    ctx.rate_keep_ratio = rate_keep_ratio_for_line_rate(ctx.estimated_line_rate,
                                                        ctx.rate_limit_lps,
                                                        ctx.single_mode);
    ctx.rate_keep_credit = 1.0;
    {
        double published_rate = ctx_published_line_rate(&ctx);
        ctx.preview_interval_ms = published_rate > 0.0 ?
            1000.0 / published_rate : 0.0;
        if (ctx.preview_interval_ms > 0.0 && ctx.preview_interval_ms < 10.0)
            ctx.preview_interval_ms = 10.0;
    }

    if (ctx.single_mode && !ctx.direct_sampling) {
        int max_preview_lines = 1;
        if (ctx.decim_factor > 1) {
            preview_first_line_ms = estimated_first_line_ms_for_plan(
                ctx.raw_samplerate, ctx.fft_size, line_sample_count,
                ctx.total_steps, ctx.decim_factor);
            if (ctx.preview_interval_ms > 0.0)
                max_preview_lines = (int)ceil(preview_first_line_ms /
                                              ctx.preview_interval_ms) + 1;
            if (max_preview_lines < 1)
                max_preview_lines = 1;
            if (max_preview_lines > (int)CACHED_PREVIEW_MAX_LINES)
                max_preview_lines = (int)CACHED_PREVIEW_MAX_LINES;
        }

        for (int lines = max_preview_lines; lines >= 1; lines--) {
            uint64_t needed_decimated = (uint64_t)ctx.fft_size;
            uint64_t needed_raw;

            if (ctx.decim_factor > 1) {
                needed_decimated += CIC_STAGES;
                needed_decimated += (uint64_t)(lines - 1) *
                    (uint64_t)ctx.decim_hop;
            }
            needed_raw = needed_decimated * (uint64_t)ctx.decim_factor;
            if (needed_raw > UINT32_MAX ||
                needed_raw > CACHED_PREVIEW_MAX_SAMPLES)
                continue;
            preview_sample_count = (uint32_t)needed_raw;
            preview_samples = recent_sample_cache_snapshot(
                &ctx, single_plan.hardware_rf_bandwidth,
                preview_sample_count, &preview_age_ms);
            if (preview_samples) {
                preview_line_count = lines;
                break;
            }
        }
        if (!preview_samples && ctx.decim_factor > 1) {
            fprintf(stderr,
                    "[SDR] Cached preview unavailable for view %u; live CIC fill will be used\n",
                    ctx.view_id);
        }
        recent_sample_cache_begin(
            ctx.center_freq,
            ctx.center_freq - single_plan.hardware_rf_bandwidth * 0.5,
            ctx.center_freq + single_plan.hardware_rf_bandwidth * 0.5,
            ctx.raw_samplerate, single_plan.hardware_rf_bandwidth);
        if (preview_samples) {
            /* Distribute every cached row across the predicted fresh live
             * fill. Keeping the old raw-line cadence here can show one preview
             * and then leave a visible pause, especially at x64 CIC where the
             * cache contains several overlapping target frames. This affects
             * display timing only; each preview remains one independent FFT. */
            if (preview_line_count > 0 && preview_first_line_ms > 0.0) {
                double coverage_interval = preview_first_line_ms /
                    (double)preview_line_count;
                ctx.preview_interval_ms = coverage_interval;
                if (ctx.preview_interval_ms < 10.0)
                    ctx.preview_interval_ms = 10.0;
            }
            ctx.preview_mode = 1;
            ctx.preview_age_ms = preview_age_ms;
            ctx.preview_sequence = 1;
            ctx.preview_count = preview_line_count;
            process_scan_buffer(&ctx, preview_samples, preview_sample_count, 0, 0);
            ctx.preview_mode = 0;
            ctx.preview_age_ms = 0;
            ctx.preview_sequence = 0;
            ctx.preview_count = 0;
            if (ctx.decim_factor > 1)
                cic_reset_state(&ctx, 0);
            free(preview_samples);
            preview_samples = NULL;
        }
    }

    if (scan_queue_init(&ctx, async_len) != 0) {
        fprintf(stderr, "[SDR] Out of memory\n");
        free(ctx.window);
        free(ctx.fft_scratch);
        free(ctx.decim_accum);
        free(ctx.cic_delay);
        free(ctx.line_buf);
        g_scanning = 0;
        return NULL;
    }

    if (pthread_create(&ctx.worker_thread, NULL, scan_worker_thread, &ctx) != 0) {
        fprintf(stderr, "[SDR] Failed to start scan processing worker\n");
        scan_queue_destroy(&ctx);
        free(ctx.window);
        free(ctx.fft_scratch);
        free(ctx.decim_accum);
        free(ctx.cic_delay);
        free(ctx.line_buf);
        g_scanning = 0;
        return NULL;
    }
    worker_started = 1;

#if !PSEUDO_RANDOM_SAMPLE_SOURCE
    ret = pluto_sdr_set_samplerate(g_dev, ctx.raw_samplerate);
    if (ret != PLUTO_ERR_OK) { fprintf(stderr, "[SDR] set_samplerate failed: %d\n", ret); device_error = 1; }
    ret = pluto_sdr_set_bandwidth(g_dev, ctx.single_mode ? single_plan.hardware_rf_bandwidth : g_rf_bandwidth);
    if (ret != PLUTO_ERR_OK) { fprintf(stderr, "[SDR] set_rf_bandwidth failed: %d\n", ret); device_error = 1; }
    ret = pluto_sdr_set_rf_port(g_dev, g_rf_port);
    if (ret != PLUTO_ERR_OK) { fprintf(stderr, "[SDR] set_rf_port_select failed: %d\n", ret); device_error = 1; }
    ret = pluto_sdr_set_gain_mode(g_dev, g_gain_mode);
    if (ret != PLUTO_ERR_OK) { fprintf(stderr, "[SDR] set_gain_mode failed: %d\n", ret); device_error = 1; }
    if (strcmp(g_gain_mode, "manual") == 0) {
        ret = pluto_sdr_set_hardwaregain(g_dev, g_hardwaregain_db);
        if (ret != PLUTO_ERR_OK) fprintf(stderr, "[SDR] set_hardwaregain failed: %d\n", ret);
    }
#else
    ret = PLUTO_ERR_OK;
#endif

    if (ctx.single_mode) {
#if !PSEUDO_RANDOM_SAMPLE_SOURCE
        pluto_sdr_stop_scan(g_dev);
#endif
        if (!direct_sampling_enabled()) {
#if PSEUDO_RANDOM_SAMPLE_SOURCE
            ret = PLUTO_ERR_OK;
#else
            ret = pluto_sdr_set_frequency(g_dev, device_center);
#endif
        } else {
            ret = PLUTO_ERR_OK;
        }
        if (ret != PLUTO_ERR_OK) {
            fprintf(stderr, "[SDR] set_frequency failed: %d\n", ret);
            device_error = 1;
            g_scanning = 0;
        }
        if (direct_sampling_enabled()) {
            printf("[SDR] Direct stream HF%u: visible %.0f - %.0f Hz, source %.0f - %.0f Hz, second IF %.0f Hz, SR %.0f Hz, RF BW %.0f Hz, fft %d effective %d, async %u, line samples %u, kernel buffers %u, %.1f lines/s raw, limit %u, drop %d\n",
                   ctx.direct_sampling, ctx.visible_start, ctx.visible_end,
                   ctx.scan_start, ctx.scan_end, ctx.second_if_hz, ctx.raw_samplerate,
                   single_plan.hardware_rf_bandwidth,
                   ctx.selected_fft_size, ctx.fft_size,
                   async_len, line_sample_count, kernel_buffers,
                   ctx.estimated_line_rate, ctx.rate_limit_lps, ctx.rate_drop_factor);
        } else {
            printf("[SDR] Single stream: visible %.0f - %.0f Hz, source %.0f - %.0f Hz, second IF %.0f Hz, converter %.0f Hz, SDR center %.0f Hz, receiver %.0f - %.0f Hz extended, SR %.0f Hz, RF BW %.0f Hz, fft %d effective %d, decim %d, hop %d, overlap %.2f, async %u, line samples %u, kernel buffers %u, %.1f lines/s raw, min %u, limit %u, drop %d\n",
                   ctx.visible_start, ctx.visible_end,
                   ctx.scan_start, ctx.scan_end, ctx.second_if_hz, g_converter_freq,
                   device_center, RF_RECEIVER_MIN_HZ, RF_RECEIVER_MAX_HZ,
                   ctx.raw_samplerate, single_plan.hardware_rf_bandwidth,
                   ctx.selected_fft_size, ctx.fft_size, ctx.decim_factor,
                   ctx.decim_hop, ctx.overlap_factor,
                   async_len, line_sample_count, kernel_buffers,
                   ctx.estimated_line_rate, g_min_rate_lps, ctx.rate_limit_lps, ctx.rate_drop_factor);
        }
    } else {
        printf("[SDR] Pluto hop scan: air band %.0f - %.0f Hz, converter %.0f Hz, SDR centers %.0f - %.0f Hz, receiver %.0f - %.0f Hz extended, step %.0f Hz (%d freqs, max %d), SR %.0f Hz, RF BW %.0f Hz, gain %s %.1f dB, %.1f lines/s raw, limit %u, drop %d\n",
               ctx.scan_start, ctx.scan_end, g_converter_freq,
               device_freqs[0], device_freqs[total_steps - 1],
               RF_RECEIVER_MIN_HZ, RF_RECEIVER_MAX_HZ,
               step, total_steps, MAX_FREQS,
               g_samplerate, g_rf_bandwidth, g_gain_mode, g_hardwaregain_db,
               ctx.estimated_line_rate, ctx.rate_limit_lps, ctx.rate_drop_factor);

#if PSEUDO_RANDOM_SAMPLE_SOURCE
        ret = PLUTO_ERR_OK;
#else
        ret = pluto_sdr_start_scan(g_dev, device_freqs, (unsigned int)total_steps);
#endif
        if (ret != PLUTO_ERR_OK) {
            fprintf(stderr, "[SDR] pluto_sdr_start_scan failed: %d\n", ret);
            device_error = 1;
            g_scanning = 0;
        }
    }

    if (g_scanning) {
        ctx.live_capture_started_msec = now_msec();
        ctx.last_callback_msec = now_msec();
        ctx.watchdog_stop = 0;
        ctx.watchdog_triggered = 0;
        reset_async_cancel_request();
        if (pthread_create(&watchdog_thread, NULL, scan_watchdog_thread, &ctx) == 0) {
            watchdog_started = 1;
        } else {
            fprintf(stderr, "[SDR] Failed to start scan watchdog\n");
        }
#if PSEUDO_RANDOM_SAMPLE_SOURCE
        ret = run_pseudo_random_sample_source(&ctx, async_len);
#else
        for (int attempt = 0; attempt <= PLUTO_READ_RETRY_COUNT; attempt++) {
            ret = pluto_sdr_read_async(g_dev, scan_callback, &ctx,
                                       kernel_buffers, async_len);
            if (ret == PLUTO_ERR_OK || !g_scanning || async_cancel_requested())
                break;
            if (attempt >= PLUTO_READ_RETRY_COUNT)
                break;
            fprintf(stderr,
                    "[SDR] pluto_sdr_read_async failed: %d; retrying RX buffer (%d/%d)\n",
                    ret, attempt + 1, PLUTO_READ_RETRY_COUNT);
            ctx.pending_refill_flags |= PLUTO_REFILL_FLAG_RECOVERED;
            ctx.last_callback_msec = now_msec();
            pluto_sleep_msec(PLUTO_READ_RETRY_DELAY_MS);
            if (pluto_sdr_recreate_buffer(g_dev, async_len,
                                          kernel_buffers) != PLUTO_ERR_OK)
                break;
            ctx.last_callback_msec = now_msec();
        }
#endif
        if (ret != PLUTO_ERR_OK && g_scanning && !async_cancel_requested()) {
#if PSEUDO_RANDOM_SAMPLE_SOURCE
            fprintf(stderr, "[SDR] pseudo-random sample source failed: %d\n", ret);
#else
            fprintf(stderr, "[SDR] pluto_sdr_read_async failed: %d\n", ret);
#endif
            device_error = 1;
        }
    }

    if (watchdog_started) {
        ctx.watchdog_stop = 1;
        pthread_join(watchdog_thread, NULL);
        if (ctx.watchdog_triggered)
            device_error = 1;
    }

    if (!ctx.single_mode) {
#if !PSEUDO_RANDOM_SAMPLE_SOURCE
        pluto_sdr_stop_scan(g_dev);
#endif
    }
    if (worker_started) {
        scan_queue_stop(&ctx);
        pthread_join(ctx.worker_thread, NULL);
    }
    if (ctx.queue_dropped > 0) {
        fprintf(stderr, "[SDR] Dropped %llu scan buffers in processing queue\n",
                (unsigned long long)ctx.queue_dropped);
    }
    if (ctx.cic_queue_waits > 0) {
        fprintf(stderr, "[SDR] CIC queue waited %llu times to preserve raw continuity\n",
                (unsigned long long)ctx.cic_queue_waits);
    }
    if (ctx.rate_dropped > 0) {
        fprintf(stderr, "[SDR] Rate-limited %llu sample buffers before FFT\n",
                (unsigned long long)ctx.rate_dropped);
    }
    if (ctx.rate_output_dropped > 0) {
        fprintf(stderr, "[SDR] Rate-limited %llu processed FFT lines\n",
                (unsigned long long)ctx.rate_output_dropped);
    }
    if (ctx.cic_sample_order_errors > 0) {
        fprintf(stderr, "[SDR] CIC sample-order errors %llu\n",
                (unsigned long long)ctx.cic_sample_order_errors);
    }
    if (ctx.cic_raw_samples_in > 0 || ctx.cic_continuity_errors > 0 ||
        ctx.cic_discontinuities > 0) {
        fprintf(stderr,
                "[SDR] CIC samples: raw %llu, decim %llu, frames %llu, discontinuities %llu, short reads %llu, refill recoveries %llu, skipped warmup frames %llu, accounting errors %llu\n",
                (unsigned long long)ctx.cic_raw_samples_in,
                (unsigned long long)ctx.cic_decim_samples_out,
                (unsigned long long)ctx.cic_fft_frames_completed,
                (unsigned long long)ctx.cic_discontinuities,
                (unsigned long long)ctx.cic_short_reads,
                (unsigned long long)ctx.cic_refill_recoveries,
                (unsigned long long)ctx.cic_reset_frames_skipped,
                (unsigned long long)ctx.cic_continuity_errors);
    }
#if PSEUDO_RANDOM_SAMPLE_SOURCE == SYNTHETIC_TONE_SAMPLE_SOURCE
    if (ctx.synthetic_spectral_checks > 0) {
        fprintf(stderr,
                "[TEST] CIC tone result: checks %llu, spectral errors %llu, sample-order errors %llu\n",
                (unsigned long long)ctx.synthetic_spectral_checks,
                (unsigned long long)ctx.synthetic_spectral_failures,
                (unsigned long long)ctx.cic_sample_order_errors);
    }
#endif
    if (ctx.tuning_skipped > 0) {
        fprintf(stderr, "[SDR] Skipped %llu tuning-incomplete scan buffers\n",
                (unsigned long long)ctx.tuning_skipped);
    }
    g_scanning = 0;
    scan_queue_destroy(&ctx);
    free(ctx.window);
    free(ctx.fft_scratch);
    free(ctx.decim_accum);
    free(ctx.cic_delay);
    free(ctx.line_buf);
    if (device_error) {
        long long now = now_msec();
        recent_sample_cache_invalidate();
        if (g_last_auto_restart_msec > 0 &&
            now - g_last_auto_restart_msec < PLUTO_AUTO_RESTART_SUPPRESS_MS) {
            g_auto_restart_on_reconnect = 0;
            g_auto_restart_suppress_until_msec =
                now + PLUTO_AUTO_RESTART_SUPPRESS_MS;
            fprintf(stderr,
                    "[SDR] Auto-restarted scan failed quickly; manual Start required\n");
        } else {
            g_auto_restart_on_reconnect = 1;
        }
        fprintf(stderr, "[SDR] Closing SDR device after I/O error; will poll for reconnect\n");
        close_device();
    }
    printf("[SDR] Scan thread stopped\n");
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Start / Stop scan                                                  */
/* ------------------------------------------------------------------ */
/**
 * @brief Clear pending reconnect auto-start state after explicit user action.
 *
 * Manual Stop and manual Start must take precedence over stale reconnect
 * requests. The suppress window avoids a reconnect poll racing immediately
 * after the frontend has explicitly stopped the stream.
 *
 * @param suppress_ms Monotonic suppression interval in milliseconds.
 */
static void clear_auto_restart_request(long long suppress_ms)
{
    g_auto_restart_on_reconnect = 0;
    if (suppress_ms > 0)
        g_auto_restart_suppress_until_msec = now_msec() + suppress_ms;
}

static int start_scan(void)
{
    double air_freqs[MAX_FREQS];
    double device_freqs[MAX_FREQS];
    double start;
    double end;
    double step = 0.0;
    int total_steps;
    run_mode_t mode;

    clear_auto_restart_request(0);

    /* If already scanning, restart with new params */
    if (g_scan_thread_joinable) {
        g_scanning = 0;
        request_async_cancel();
        pthread_join(g_scan_thread, NULL);
        g_scan_thread_joinable = 0;
        pluto_sleep_msec(PLUTO_RESTART_DELAY_MS);
    }

    mode = planned_run_mode();
    if (mode == RUN_MODE_SINGLE) {
        single_fft_plan_t plan;
        double center;
        raw_visible_band(&start, &end);
        if (end <= start)
            return -1;
        plan = single_fft_plan_for_span(end - start);
        center = direct_sampling_enabled() ?
            direct_source_center_for_visible(start, end, plan.source_span) :
            single_source_center_for_visible(start, end, plan.source_span);
        if (!direct_sampling_enabled() &&
            !receiver_frequency_valid(receiver_frequency_from_radio(center)))
            return -1;
    } else {
        total_steps = build_scan_frequencies(air_freqs, &step);
        if (total_steps < PLUTO_MIN_FREQS_CNT) return -1;
        if (build_device_scan_frequencies(air_freqs, total_steps, device_freqs) != 0) return -1;
    }

    if (!g_dev && open_first_device(0) != PLUTO_ERR_OK)
        return -1;

    g_scanning = 1;
    if (pthread_create(&g_scan_thread, NULL, scan_thread_func, NULL) != 0) {
        g_scanning = 0;
        return -1;
    }
    g_scan_thread_joinable = 1;
    return 0;
}

static void stop_scan(void)
{
    if (!g_scan_thread_joinable) return;
    g_scanning = 0;
    request_async_cancel();
    pthread_join(g_scan_thread, NULL);
    g_scan_thread_joinable = 0;
}

static void stop_scan_if_frontend_idle(void)
{
    long long now;
    long long idle_ms;

    if (!g_scanning)
        return;
    if (sse_client_count() > 0)
        return;
    if (g_last_frontend_activity_msec <= 0)
        return;

    now = now_msec();
    idle_ms = now - g_last_frontend_activity_msec;
    if (idle_ms < FRONTEND_IDLE_STOP_MS)
        return;

    printf("[SDR] No frontend activity for %.1f s; stopping scan\n",
           (double)idle_ms / 1000.0);
    stop_scan();
}

static int apply_gain_settings(void)
{
    if (!g_dev || !g_scanning)
        return PLUTO_ERR_OK;
    if (pluto_sdr_set_gain_mode(g_dev, g_gain_mode) != PLUTO_ERR_OK)
        return -1;
    if (strcmp(g_gain_mode, "manual") == 0 &&
        pluto_sdr_set_hardwaregain(g_dev, g_hardwaregain_db) != PLUTO_ERR_OK)
        return -1;
    return PLUTO_ERR_OK;
}

/* ------------------------------------------------------------------ */
/* HTTP request handler                                               */
/* ------------------------------------------------------------------ */
/**
 * @brief Send a complete HTTP response fragment.
 *
 * @param client_fd Socket descriptor.
 * @param buf Response bytes.
 * @param len Byte count.
 * @return 0 on success, -1 on write failure.
 */
static int write_response_part(int client_fd, const char *buf, size_t len)
{
    if (!buf && len > 0)
        return -1;
    return write_all(client_fd, buf ? buf : "", len);
}

static void send_json_response(int client_fd, int code, const char *reason,
                               const char *cors, const char *body)
{
    char header[512];
    size_t len = body ? strlen(body) : 0;
    int n = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "%s\r\n",
        code, reason, len, cors ? cors : "");
    if (n < 0 || (size_t)n >= sizeof(header))
        return;
    if (write_response_part(client_fd, header, (size_t)n) != 0)
        return;
    if (len > 0)
        (void)write_response_part(client_fd, body, len);
}

static void send_empty_response(int client_fd, int code, const char *reason,
                                const char *cors)
{
    char header[512];
    int n = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n"
        "%s\r\n",
        code, reason, cors ? cors : "");
    if (n < 0 || (size_t)n >= sizeof(header))
        return;
    (void)write_response_part(client_fd, header, (size_t)n);
}

static void send_text_response(int client_fd, int code, const char *reason,
                               const char *cors, const char *content_type,
                               const char *body, size_t len)
{
    char header[512];
    int n = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "%s\r\n",
        code, reason, content_type, len, cors ? cors : "");
    if (n < 0 || (size_t)n >= sizeof(header))
        return;
    if (write_response_part(client_fd, header, (size_t)n) != 0)
        return;
    if (body && len > 0)
        (void)write_response_part(client_fd, body, len);
}

static void send_json_error(int client_fd, int code, const char *reason,
                            const char *cors, const char *message)
{
    char escaped[512];
    char body[768];
    append_json_escaped(escaped, sizeof(escaped), 0, message ? message : "error");
    snprintf(body, sizeof(body),
             "{\"status\":\"error\",\"message\":\"%s\"}", escaped);
    send_json_response(client_fd, code, reason, cors, body);
}

static void send_file_response(int client_fd, const char *cors, const char *path,
                               const char *content_type, int missing_code,
                               const char *missing_reason)
{
    char *body = NULL;
    size_t len = 0;
    file_read_status_t status = read_file_ex(path, &body, &len);

    if (status == FILE_READ_OK) {
        send_text_response(client_fd, 200, "OK", cors, content_type, body, len);
        free(body);
        return;
    }
    if (status == FILE_READ_EMPTY) {
        send_text_response(client_fd, 200, "OK", cors, content_type, "", 0);
        return;
    }
    if (status == FILE_READ_MISSING) {
        char msg[256];
        snprintf(msg, sizeof(msg), "%s is missing", path);
        send_json_error(client_fd, missing_code, missing_reason, cors, msg);
        return;
    }
    {
        char msg[256];
        snprintf(msg, sizeof(msg), "Could not read %s: %s",
                 path, file_read_status_text(status));
        send_json_error(client_fd, 500, "Internal Server Error", cors, msg);
    }
}

typedef struct {
    char method[16];
    char path[PATH_MAX_CHARS];
    char version[16];
    const char *body;
    size_t body_len;
} http_request_t;

typedef struct {
    const char *body;
    size_t len;
} json_doc_t;

static const char *skip_ws_range(const char *p, const char *end)
{
    while (p < end && isspace((unsigned char)*p))
        p++;
    return p;
}

static int parse_http_request(const char *raw, size_t raw_len, int truncated,
                              http_request_t *out, char *err, size_t err_len)
{
    const char *header_end;
    const char *line_end;
    const char *sp1;
    const char *sp2;
    int content_len = 0;
    size_t header_len;
    size_t method_len;
    size_t path_len;
    size_t version_len;

    if (err && err_len)
        err[0] = 0;
    memset(out, 0, sizeof(*out));

    header_end = strstr(raw, "\r\n\r\n");
    if (!header_end) {
        snprintf(err, err_len, "HTTP headers are incomplete");
        return -1;
    }
    header_len = (size_t)(header_end - raw) + 4;
    if (truncated) {
        snprintf(err, err_len, "HTTP request exceeds %d bytes", MAX_REQUEST);
        return -1;
    }
    if (http_content_length_n(raw, header_len, &content_len) != 0) {
        snprintf(err, err_len, "Invalid Content-Length header");
        return -1;
    }
    if (content_len < 0 || (size_t)content_len > MAX_REQUEST - header_len) {
        snprintf(err, err_len, "Request body is too large");
        return -1;
    }
    if (raw_len < header_len + (size_t)content_len) {
        snprintf(err, err_len, "HTTP request body is incomplete");
        return -1;
    }

    line_end = strstr(raw, "\r\n");
    if (!line_end || line_end == raw) {
        snprintf(err, err_len, "Invalid HTTP request line");
        return -1;
    }
    sp1 = memchr(raw, ' ', (size_t)(line_end - raw));
    if (!sp1) {
        snprintf(err, err_len, "Invalid HTTP request line");
        return -1;
    }
    sp2 = memchr(sp1 + 1, ' ', (size_t)(line_end - sp1 - 1));
    if (!sp2) {
        snprintf(err, err_len, "Invalid HTTP request line");
        return -1;
    }

    method_len = (size_t)(sp1 - raw);
    path_len = (size_t)(sp2 - sp1 - 1);
    version_len = (size_t)(line_end - sp2 - 1);
    if (method_len == 0 || method_len >= sizeof(out->method) ||
        path_len == 0 || path_len >= sizeof(out->path) ||
        version_len == 0 || version_len >= sizeof(out->version)) {
        snprintf(err, err_len, "HTTP request line is too long");
        return -1;
    }

    memcpy(out->method, raw, method_len);
    out->method[method_len] = 0;
    memcpy(out->path, sp1 + 1, path_len);
    out->path[path_len] = 0;
    memcpy(out->version, sp2 + 1, version_len);
    out->version[version_len] = 0;
    url_decode(out->path);
    {
        char *qs = strchr(out->path, '?');
        if (qs)
            *qs = 0;
    }
    out->body = raw + header_len;
    out->body_len = (size_t)content_len;
    return 0;
}

static int json_skip_string(const char **pp, const char *end)
{
    const char *p = *pp;
    if (p >= end || *p != '"')
        return -1;
    p++;
    while (p < end) {
        unsigned char c = (unsigned char)*p++;
        if (c == '"') {
            *pp = p;
            return 0;
        }
        if (c == '\\') {
            if (p >= end)
                return -1;
            c = (unsigned char)*p++;
            if (c == 'u') {
                for (int i = 0; i < 4; i++) {
                    if (p >= end || !isxdigit((unsigned char)*p))
                        return -1;
                    p++;
                }
            } else if (!(c == '"' || c == '\\' || c == '/' || c == 'b' ||
                         c == 'f' || c == 'n' || c == 'r' || c == 't')) {
                return -1;
            }
        } else if (c < 0x20) {
            return -1;
        }
    }
    return -1;
}

static int json_read_key(const char **pp, const char *end, char *key, size_t key_len)
{
    const char *p = *pp;
    size_t pos = 0;
    if (p >= end || *p != '"')
        return -1;
    p++;
    while (p < end) {
        unsigned char c = (unsigned char)*p++;
        if (c == '"') {
            key[pos] = 0;
            *pp = p;
            return 0;
        }
        if (c == '\\') {
            if (p >= end)
                return -1;
            c = (unsigned char)*p++;
            if (c == 'u') {
                for (int i = 0; i < 4; i++) {
                    if (p >= end || !isxdigit((unsigned char)*p))
                        return -1;
                    p++;
                }
                c = '?';
            } else if (!(c == '"' || c == '\\' || c == '/' || c == 'b' ||
                         c == 'f' || c == 'n' || c == 'r' || c == 't')) {
                return -1;
            }
        } else if (c < 0x20) {
            return -1;
        }
        if (pos + 1 < key_len)
            key[pos++] = (char)c;
    }
    return -1;
}

static int json_skip_number(const char **pp, const char *end)
{
    const char *p = *pp;
    if (p < end && *p == '-')
        p++;
    if (p >= end)
        return -1;
    if (*p == '0') {
        p++;
    } else if (isdigit((unsigned char)*p)) {
        while (p < end && isdigit((unsigned char)*p))
            p++;
    } else {
        return -1;
    }
    if (p < end && *p == '.') {
        p++;
        if (p >= end || !isdigit((unsigned char)*p))
            return -1;
        while (p < end && isdigit((unsigned char)*p))
            p++;
    }
    if (p < end && (*p == 'e' || *p == 'E')) {
        p++;
        if (p < end && (*p == '+' || *p == '-'))
            p++;
        if (p >= end || !isdigit((unsigned char)*p))
            return -1;
        while (p < end && isdigit((unsigned char)*p))
            p++;
    }
    *pp = p;
    return 0;
}

static int json_skip_literal(const char **pp, const char *end,
                             const char *literal)
{
    size_t len = strlen(literal);
    if ((size_t)(end - *pp) < len || memcmp(*pp, literal, len) != 0)
        return -1;
    *pp += len;
    return 0;
}

static int json_skip_value(const char **pp, const char *end)
{
    const char *p = skip_ws_range(*pp, end);
    if (p >= end)
        return -1;
    if (*p == '"') {
        if (json_skip_string(&p, end) != 0)
            return -1;
    } else if (*p == '-' || isdigit((unsigned char)*p)) {
        if (json_skip_number(&p, end) != 0)
            return -1;
    } else if (*p == 't') {
        if (json_skip_literal(&p, end, "true") != 0)
            return -1;
    } else if (*p == 'f') {
        if (json_skip_literal(&p, end, "false") != 0)
            return -1;
    } else if (*p == 'n') {
        if (json_skip_literal(&p, end, "null") != 0)
            return -1;
    } else {
        return -1;
    }
    *pp = p;
    return 0;
}

static int json_find_value(const json_doc_t *doc, const char *wanted,
                           const char **out_start, size_t *out_len)
{
    const char *p = doc->body;
    const char *end = doc->body + doc->len;
    int found = 0;

    if (out_start)
        *out_start = NULL;
    if (out_len)
        *out_len = 0;

    p = skip_ws_range(p, end);
    if (p >= end || *p != '{')
        return -1;
    p++;
    p = skip_ws_range(p, end);
    if (p < end && *p == '}') {
        p++;
        p = skip_ws_range(p, end);
        return p == end ? 0 : -1;
    }

    for (;;) {
        char key[96];
        const char *value_start;
        const char *value_end;

        p = skip_ws_range(p, end);
        if (json_read_key(&p, end, key, sizeof(key)) != 0)
            return -1;
        p = skip_ws_range(p, end);
        if (p >= end || *p != ':')
            return -1;
        p++;
        value_start = skip_ws_range(p, end);
        p = value_start;
        if (json_skip_value(&p, end) != 0)
            return -1;
        value_end = p;
        if (strcmp(key, wanted) == 0) {
            found = 1;
            if (out_start)
                *out_start = value_start;
            if (out_len)
                *out_len = (size_t)(value_end - value_start);
        }
        p = skip_ws_range(p, end);
        if (p < end && *p == ',') {
            p++;
            continue;
        }
        if (p < end && *p == '}') {
            p++;
            p = skip_ws_range(p, end);
            return p == end ? found : -1;
        }
        return -1;
    }
}

static int json_get_double(const json_doc_t *doc, const char *key,
                           double *out_value, int *out_present)
{
    const char *start;
    size_t len;
    char tmp[128];
    char *after = NULL;
    double value;
    int status = json_find_value(doc, key, &start, &len);
    if (out_present)
        *out_present = 0;
    if (status < 0)
        return -1;
    if (status == 0)
        return 0;
    if (len == 0 || len >= sizeof(tmp))
        return -1;
    memcpy(tmp, start, len);
    tmp[len] = 0;
    errno = 0;
    value = strtod(tmp, &after);
    if (errno != 0 || after == tmp || *after != 0 || !isfinite(value))
        return -1;
    if (out_value)
        *out_value = value;
    if (out_present)
        *out_present = 1;
    return 0;
}

static int json_get_uint(const json_doc_t *doc, const char *key,
                         uint32_t *out_value, int *out_present)
{
    const char *start;
    size_t len;
    char tmp[64];
    char *after = NULL;
    unsigned long value;
    int status = json_find_value(doc, key, &start, &len);
    if (out_present)
        *out_present = 0;
    if (status < 0)
        return -1;
    if (status == 0)
        return 0;
    if (len == 0 || len >= sizeof(tmp))
        return -1;
    memcpy(tmp, start, len);
    tmp[len] = 0;
    if (tmp[0] == '-')
        return -1;
    errno = 0;
    value = strtoul(tmp, &after, 10);
    if (errno != 0 || after == tmp || *after != 0 || value > UINT32_MAX)
        return -1;
    if (out_value)
        *out_value = (uint32_t)value;
    if (out_present)
        *out_present = 1;
    return 0;
}

static int json_get_string(const json_doc_t *doc, const char *key,
                           char *out, size_t out_len, int *out_present)
{
    const char *start;
    size_t len;
    size_t pos = 0;
    int status = json_find_value(doc, key, &start, &len);

    if (out_present)
        *out_present = 0;
    if (out && out_len)
        out[0] = 0;
    if (status < 0)
        return -1;
    if (status == 0)
        return 0;
    if (len < 2 || start[0] != '"' || start[len - 1] != '"')
        return -1;
    if (!out || out_len == 0)
        return -1;
    for (size_t i = 1; i + 1 < len; i++) {
        unsigned char c = (unsigned char)start[i];
        if (c == '\\') {
            if (i + 2 >= len)
                return -1;
            c = (unsigned char)start[++i];
            if (!(c == '"' || c == '\\' || c == '/' || c == 'b' ||
                  c == 'f' || c == 'n' || c == 'r' || c == 't'))
                return -1;
            if (c == 'b' || c == 'f' || c == 'n' || c == 'r' || c == 't')
                c = ' ';
        }
        if (pos + 1 < out_len)
            out[pos++] = (char)c;
    }
    out[pos] = 0;
    if (out_present)
        *out_present = 1;
    return 0;
}

static int json_validate_object(const json_doc_t *doc)
{
    return json_find_value(doc, "", NULL, NULL) >= 0 ? 0 : -1;
}

static int validate_scan_settings(double freq_start, double freq_end,
                                  double converter_freq, double samplerate,
                                  double bw_ratio, char *err, size_t err_len)
{
    if (!isfinite(samplerate) || samplerate < 4.0e6 || samplerate > SCANNER_SAMPLE_RATE_HZ) {
        snprintf(err, err_len, "sample rate must be 4 MHz..61.44 MHz");
        return -1;
    }
    if (!isfinite(bw_ratio) || bw_ratio <= 0.0 || bw_ratio > 1.0) {
        snprintf(err, err_len, "bw_ratio must be greater than 0 and at most 1");
        return -1;
    }
    if (!isfinite(converter_freq) || fabs(converter_freq) > 1.0e12) {
        snprintf(err, err_len, "converter frequency is out of range");
        return -1;
    }
    if (!isfinite(freq_start) || !isfinite(freq_end) ||
        freq_start < MIN_FREQ_START_HZ || freq_end <= freq_start) {
        snprintf(err, err_len, "frequency range is invalid");
        return -1;
    }
    return 0;
}

static int validate_pluto_settings(double samplerate, double rf_bandwidth, const char *gain_mode,
                                   double hardwaregain_db, char *err, size_t err_len)
{
    if (!isfinite(rf_bandwidth) || rf_bandwidth < 200000.0 ||
        rf_bandwidth > DEFAULT_RF_BANDWIDTH_HZ) {
        snprintf(err, err_len, "RF bandwidth must be 0.2 MHz..20 MHz");
        return -1;
    }
    if (rf_bandwidth >= samplerate) {
        snprintf(err, err_len, "RF bandwidth must be less than sample rate");
        return -1;
    }
    if (!gain_mode || (strcmp(gain_mode, "manual") != 0 &&
                       strcmp(gain_mode, "slow_attack") != 0 &&
                       strcmp(gain_mode, "fast_attack") != 0 &&
                       strcmp(gain_mode, "hybrid") != 0)) {
        snprintf(err, err_len, "gain_mode must be manual, slow_attack, fast_attack, or hybrid");
        return -1;
    }
    if (strcmp(gain_mode, "manual") == 0 &&
        (!isfinite(hardwaregain_db) ||
         hardwaregain_db < PLUTO_GAIN_MIN_DB ||
         hardwaregain_db > PLUTO_GAIN_MAX_DB)) {
        snprintf(err, err_len, "hardwaregain_db must be %.0f..%.0f dB",
                 PLUTO_GAIN_MIN_DB, PLUTO_GAIN_MAX_DB);
        return -1;
    }
    return 0;
}

/**
 * @brief Check whether a frontend-selectable AD936x RX input name is supported.
 *
 * @param port Requested `rf_port_select` value.
 * @return Nonzero for a supported balanced input, otherwise zero.
 */
static int rf_port_is_supported(const char *port)
{
    return port && (strcmp(port, "A_BALANCED") == 0 ||
                    strcmp(port, "B_BALANCED") == 0 ||
                    strcmp(port, "C_BALANCED") == 0);
}

static int validate_visible_range(double start, double end,
                                  char *err, size_t err_len)
{
    if (!isfinite(start) || !isfinite(end) || end <= start) {
        snprintf(err, err_len, "visible frequency range is invalid");
        return -1;
    }
    return 0;
}

static int json_body_for_request(const http_request_t *http, json_doc_t *doc,
                                 char *err, size_t err_len)
{
    doc->body = http->body;
    doc->len = http->body_len;
    if (doc->len == 0) {
        snprintf(err, err_len, "Missing JSON body");
        return -1;
    }
    if (json_validate_object(doc) != 0) {
        snprintf(err, err_len, "Malformed JSON body");
        return -1;
    }
    return 0;
}

static const char *build_sw_version(void)
{
    static char version[32];
    static int initialized = 0;
    char mon[4] = {0};
    int day = 0;
    int year = 0;
    int month = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    static const char *months[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };

    if (initialized)
        return version;

    if (sscanf(__DATE__, "%3s %d %d", mon, &day, &year) == 3) {
        for (int i = 0; i < 12; i++) {
            if (strcmp(mon, months[i]) == 0) {
                month = i + 1;
                break;
            }
        }
    }
    if (sscanf(__TIME__, "%d:%d:%d", &hour, &minute, &second) != 3) {
        hour = 0;
        minute = 0;
    }

    if (year > 0 && month > 0 && day > 0)
        snprintf(version, sizeof(version), "%04d%02d%02d%02d%02d",
                 year, month, day, hour, minute);
    else
        snprintf(version, sizeof(version), "000000000000");
    initialized = 1;
    return version;
}

static void handle_request(int client_fd, const char *req, size_t req_len,
                           int truncated)
{
    http_request_t http;
    char parse_error[160];

    const char *cors = "Access-Control-Allow-Origin: *\r\n"
                       "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                       "Access-Control-Allow-Headers: Content-Type\r\n";

    if (parse_http_request(req, req_len, truncated, &http,
                           parse_error, sizeof(parse_error)) != 0) {
        send_json_error(client_fd, 400, "Bad Request", cors, parse_error);
        close(client_fd);
        return;
    }
    mark_frontend_activity();

    if (strcmp(http.method, "OPTIONS") == 0) {
        char resp[512];
        int n = snprintf(resp, sizeof(resp),
            "HTTP/1.1 204 No Content\r\n%s\r\n", cors);
        if (n >= 0 && (size_t)n < sizeof(resp))
            (void)write_response_part(client_fd, resp, (size_t)n);
        close(client_fd);
        return;
    }

    /* GET / or /index.html */
    if (strcmp(http.method, "GET") == 0 && (strcmp(http.path, "/") == 0 || strcmp(http.path, "/index.html") == 0)) {
        send_file_response(client_fd, cors, HTML_PATH,
                           "text/html; charset=utf-8",
                           500, "Internal Server Error");
        close(client_fd);
        return;
    }

    if (strcmp(http.method, "GET") == 0 && strcmp(http.path, "/bands.ini") == 0) {
        send_file_response(client_fd, cors, BANDS_PATH,
                           "text/plain; charset=utf-8",
                           404, "Not Found");
        close(client_fd);
        return;
    }

    if (strcmp(http.method, "GET") == 0 && strcmp(http.path, "/markers.ini") == 0) {
        size_t len;
        char *text = NULL;
        file_read_status_t status = read_file_ex(MARKERS_PATH, &text, &len);
        if (status == FILE_READ_MISSING) {
            const char *empty =
                "# Human-editable frequency markers\n"
                "# Each marker section supports name, group, frequency_hz or frequency_mhz.\n";
            send_text_response(client_fd, 200, "OK", cors,
                               "text/plain; charset=utf-8",
                               empty, strlen(empty));
            close(client_fd);
            return;
        }
        if (status == FILE_READ_EMPTY) {
            send_text_response(client_fd, 200, "OK", cors,
                               "text/plain; charset=utf-8", "", 0);
            close(client_fd);
            return;
        }
        if (status != FILE_READ_OK) {
            char msg[256];
            snprintf(msg, sizeof(msg), "Could not read %s: %s",
                     MARKERS_PATH, file_read_status_text(status));
            send_json_error(client_fd, 500, "Internal Server Error", cors, msg);
            close(client_fd);
            return;
        }
        send_text_response(client_fd, 200, "OK", cors,
                           "text/plain; charset=utf-8", text, len);
        free(text);
        close(client_fd);
        return;
    }

    if (strcmp(http.method, "GET") == 0 && strcmp(http.path, "/api/markers") == 0) {
        send_markers_json(client_fd, cors);
        close(client_fd);
        return;
    }

    if (strcmp(http.method, "POST") == 0 && strcmp(http.path, "/api/markers/save") == 0) {
        char err[160];
        if (validate_markers_text(http.body, http.body_len,
                                  err, sizeof(err)) != 0) {
            send_json_error(client_fd, 400, "Bad Request", cors, err);
            close(client_fd);
            return;
        }
        if (write_file_atomic(MARKERS_PATH, http.body, http.body_len) != 0) {
            send_json_response(client_fd, 500, "Internal Server Error", cors,
                               "{\"status\":\"error\",\"message\":\"could not save markers\"}");
            close(client_fd);
            return;
        }
        send_json_response(client_fd, 200, "OK", cors, "{\"status\":\"ok\"}");
        close(client_fd);
        return;
    }

    /* GET /api/status */
    if (strcmp(http.method, "GET") == 0 && strcmp(http.path, "/api/status") == 0) {
        char body[API_JSON_BODY_MAX];
        char sample_rates[1024];
        double step = 0.0;
        double scan_start = 0.0;
        double scan_end = 0.0;
        int total_steps;
        int required_points = planned_required_points();
        run_mode_t mode = planned_run_mode();
        int bins_per_step = scan_bins_per_step();
        int display_bins = current_display_bins();
        int raw_line_bins = current_line_bins();
        int line_bins = display_bins;
        int effective_fft = current_effective_fft_size();
        int decim_factor = current_decim_factor();
        int decim_hop = current_decim_hop();
        double overlap_factor = current_overlap_factor();
        uint32_t line_sample_count;
        double raw_line_rate = 0.0;
        double true_line_rate;
        double traffic_kbytes_s;
        double first_line_ms;
        double source_span_hz;
        double visible_span_hz;
        double visible_raw_bins_per_pixel = 0.0;
        double visible_raw_bins = 0.0;
        int minimum_rate_limited = current_minimum_rate_limited();
        int minimum_rate_achieved;
        int rate_drop_factor;

        if (mode == RUN_MODE_SINGLE) {
            single_fft_plan_t plan;
            double center;
            raw_visible_band(&scan_start, &scan_end);
            plan = single_fft_plan_for_span(scan_end - scan_start);
            center = direct_sampling_enabled() ?
                direct_source_center_for_visible(scan_start, scan_end,
                                                 plan.source_span) :
                single_source_center_for_visible(scan_start, scan_end,
                                                 plan.source_span);
            center += current_frequency_correction_hz();
            step = plan.source_span;
            scan_start = center - plan.source_span * 0.5;
            scan_end = center + plan.source_span * 0.5;
            total_steps = 1;
        } else {
            active_scan_band(&scan_start, &scan_end);
            total_steps = build_scan_frequencies_for_band(scan_start, scan_end, NULL, &step);
            scan_end = build_scan_effective_end_for_band(scan_start, scan_end, total_steps, step);
        }
        line_sample_count = current_line_sample_count();
        rate_drop_factor = rate_drop_factor_for_plan(current_active_samplerate(), line_sample_count,
                                                     total_steps,
                                                     decim_factor,
                                                     g_rate_limit_lps,
                                                     &raw_line_rate);
        true_line_rate = current_true_line_rate();
        first_line_ms = current_first_line_ms(total_steps, line_sample_count);
        minimum_rate_achieved = mode == RUN_MODE_SINGLE ?
            current_minimum_rate_achieved() :
            (g_min_rate_lps == 0 || raw_line_rate >= (double)g_min_rate_lps);
        source_span_hz = scan_end - scan_start;
        visible_span_hz = g_visible_end - g_visible_start;
        visible_raw_bins = mode == RUN_MODE_SINGLE ?
            current_visible_raw_bins() :
            ((source_span_hz > 0.0) ?
                (double)raw_line_bins * (visible_span_hz / source_span_hz) : 0.0);
        if (mode == RUN_MODE_SINGLE)
            visible_raw_bins_per_pixel = current_visible_bins_per_pixel();
        else if (source_span_hz > 0.0 && visible_span_hz > 0.0 && display_bins > 0)
            visible_raw_bins_per_pixel =
                ((double)raw_line_bins * (visible_span_hz / source_span_hz)) /
                (double)display_bins;
        traffic_kbytes_s = measured_frontend_kbytes_s();
        double active_samplerate = current_active_samplerate();
        double active_rf_bandwidth = current_active_rf_bandwidth();
        double active_bw_ratio = current_active_bw_ratio();
        double second_if_hz = current_second_if_hz();
        double zero_if_guard_hz = current_zero_if_guard_hz();
        double fq_err_model_hz = current_frequency_model_error_hz();
        double fq_err_effective_hz = current_frequency_correction_hz();
        format_sample_rates_json(sample_rates, sizeof(sample_rates));

        int n = snprintf(body, sizeof(body),
            "{\"device\":\"Pluto SDR\","
            "\"sw_version\":\"%s\","
            "\"hardware\":\"%s\",\"firmware\":\"%s\",\"serial\":\"%s\","
            "\"manufacturer\":\"%s\",\"product\":\"%s\","
            "\"receiver_min_hz\":%.0f,\"receiver_max_hz\":%.0f,"
            "\"receiver_range_note\":\"unofficial extended Pluto range\","
            "\"scanning\":%d,\"device_present\":%d,"
            "\"auto_restart_on_reconnect\":%d,"
            "\"freq_start\":" JSON_COORD_FMT ",\"freq_end\":" JSON_COORD_FMT ","
            "\"converter_freq\":" JSON_COORD_FMT ","
            "\"configured_start_hz\":" JSON_COORD_FMT ",\"configured_end_hz\":" JSON_COORD_FMT ","
            "\"visible_start_hz\":" JSON_COORD_FMT ",\"visible_end_hz\":" JSON_COORD_FMT ","
            "\"scan_start_hz\":" JSON_COORD_FMT ",\"scan_end_hz\":" JSON_COORD_FMT ","
            "\"second_if_hz\":" JSON_COORD_FMT ",\"zero_if_guard_hz\":" JSON_COORD_FMT ","
            "\"fq_err_correction\":%u,\"fq_err_model_hz\":%.9f,"
            "\"fq_err_effective_hz\":%.9f,"
            "\"samplerate\":%.0f,\"rf_bandwidth\":%.0f,\"bw_ratio\":%.2f,"
            "\"hardware_samplerate\":%.0f,\"hardware_rf_bandwidth\":%.0f,"
            "\"hardware_bw_ratio\":%.2f,"
            "\"active_samplerate\":%.0f,\"active_rf_bandwidth\":%.0f,"
            "\"active_bw_ratio\":%.3f,"
            "\"step_hz\":" JSON_COORD_FMT ",\"steps\":%d,"
            "\"required_points\":%d,\"mode\":\"%s\",\"active_mode\":\"%s\","
            "\"min_rate_lps\":%u,\"rate_limit_lps\":%u,"
            "\"rate_drop_factor\":%d,\"raw_line_rate\":%.3f,"
            "\"true_line_rate\":%.3f,"
            "\"first_line_ms\":%.3f,"
            "\"minimum_rate_overlap\":%d,\"minimum_rate_limited\":%d,"
            "\"minimum_rate_achieved\":%d,"
            "\"traffic_kbytes_s\":%.1f,"
            "\"source_span_hz\":" JSON_COORD_FMT ",\"visible_raw_bins\":%.3f,"
            "\"visible_bins_per_pixel\":%.6f,\"visible_raw_bins_per_pixel\":%.6f,"
            "\"bins_per_step\":%d,\"line_bins\":%d,\"raw_line_bins\":%d,"
            "\"display_bins\":%d,"
            "\"view_id\":%u,\"fft_size\":%d,\"effective_fft_size\":%d,"
            "\"decim_factor\":%d,\"decim_hop\":%d,\"overlap_factor\":%.3f,"
            "\"effective_input_samples\":%u,"
            "\"gain_mode\":\"%s\",\"hardwaregain_db\":%.1f,\"rf_port\":\"%s\","
            "\"rssi_db\":%.2f,\"input_peak\":%.0f,\"clipped_samples\":%llu,"
            "\"goto_freq_hz\":" JSON_COORD_FMT ",\"goto_target_zoom\":%.6g,"
            "\"goto_animate\":%u,"
            "\"goto_delay_s\":%.1f,"
            "\"sample_rates\":%s}",
            build_sw_version(),
            g_hw_rev, g_fw_ver, g_serial,
            g_manufacturer, g_product,
            RF_RECEIVER_MIN_HZ, RF_RECEIVER_MAX_HZ,
            g_scanning, g_dev != NULL || PSEUDO_RANDOM_SAMPLE_SOURCE,
            g_auto_restart_on_reconnect,
            g_freq_start, g_freq_end, g_converter_freq,
            g_freq_start, g_freq_end,
            g_visible_start, g_visible_end,
            scan_start,
            scan_end,
            second_if_hz, zero_if_guard_hz,
            g_fq_err_correction, fq_err_model_hz, fq_err_effective_hz,
            active_samplerate, active_rf_bandwidth, active_bw_ratio,
            active_samplerate, active_rf_bandwidth, active_bw_ratio,
            active_samplerate, active_rf_bandwidth, active_bw_ratio,
            step, total_steps,
            required_points, run_mode_name(mode), run_mode_name(g_active_mode),
            g_min_rate_lps, g_rate_limit_lps, rate_drop_factor, raw_line_rate,
            true_line_rate,
            first_line_ms,
            minimum_rate_limited, minimum_rate_limited,
            minimum_rate_achieved,
            traffic_kbytes_s, source_span_hz, visible_raw_bins,
            visible_raw_bins_per_pixel, visible_raw_bins_per_pixel,
            bins_per_step, line_bins, raw_line_bins, display_bins,
            g_view_id, current_fft_size(), effective_fft,
            decim_factor, decim_hop, overlap_factor, line_sample_count,
            g_gain_mode, g_hardwaregain_db, g_rf_port,
            g_last_rssi_db, g_last_input_peak,
            (unsigned long long)g_last_clipped_samples,
            g_goto_freq, g_goto_target_zoom, g_goto_animate, g_goto_delay_s,
            sample_rates);

        if (n < 0 || (size_t)n >= sizeof(body)) {
            send_json_error(client_fd, 500, "Internal Server Error", cors,
                            "Status response too large");
            close(client_fd);
            return;
        }

        send_json_response(client_fd, 200, "OK", cors, body);
        close(client_fd);
        return;
    }

    /* POST /api/goto */
    if (strcmp(http.method, "POST") == 0 && strcmp(http.path, "/api/goto") == 0) {
        json_doc_t json;
        char err[160];
        double freq_tmp = g_goto_freq;
        double target_zoom_tmp = g_goto_target_zoom;
        double delay_tmp = g_goto_delay_s;
        uint32_t animate_tmp = g_goto_animate;
        double number_tmp;
        uint32_t uint_tmp;
        int present = 0;

        if (json_body_for_request(&http, &json, err, sizeof(err)) != 0) {
            send_json_error(client_fd, 400, "Bad Request", cors, err);
            close(client_fd);
            return;
        }
        if (json_get_double(&json, "goto_freq_hz", &number_tmp, &present) != 0 ||
            !present) {
            send_json_error(client_fd, 400, "Bad Request", cors,
                            "goto_freq_hz is required");
            close(client_fd);
            return;
        }
        freq_tmp = number_tmp;
        if (json_get_double(&json, "goto_target_zoom", &number_tmp, &present) != 0) {
            send_json_error(client_fd, 400, "Bad Request", cors,
                            "Malformed goto_target_zoom");
            close(client_fd);
            return;
        }
        if (present)
            target_zoom_tmp = number_tmp;
        if (json_get_uint(&json, "goto_animate", &uint_tmp, &present) != 0) {
            send_json_error(client_fd, 400, "Bad Request", cors,
                            "Malformed goto_animate");
            close(client_fd);
            return;
        }
        if (present)
            animate_tmp = uint_tmp ? 1 : 0;
        if (json_get_double(&json, "goto_delay_s", &number_tmp, &present) != 0) {
            send_json_error(client_fd, 400, "Bad Request", cors,
                            "Malformed goto_delay_s");
            close(client_fd);
            return;
        }
        if (present)
            delay_tmp = number_tmp;

        if (!isfinite(freq_tmp) || freq_tmp <= 0.0) {
            send_json_error(client_fd, 400, "Bad Request", cors,
                            "goto_freq_hz is out of range");
            close(client_fd);
            return;
        }
        if (!isfinite(target_zoom_tmp) ||
            target_zoom_tmp < GOTO_TARGET_ZOOM_MIN ||
            target_zoom_tmp > GOTO_TARGET_ZOOM_MAX) {
            send_json_error(client_fd, 400, "Bad Request", cors,
                            "goto_target_zoom is out of range");
            close(client_fd);
            return;
        }
        delay_tmp = normalize_goto_delay_s(delay_tmp);
        if (present && fabs(delay_tmp - number_tmp) >= 1e-9) {
            send_json_error(client_fd, 400, "Bad Request", cors,
                            "goto_delay_s is unsupported");
            close(client_fd);
            return;
        }

        g_goto_freq = freq_tmp;
        g_goto_target_zoom = target_zoom_tmp;
        g_goto_animate = animate_tmp ? 1 : 0;
        g_goto_delay_s = delay_tmp;
        save_config();

        {
            char json_body[256];
            snprintf(json_body, sizeof(json_body),
                "{\"status\":\"ok\",\"goto_freq_hz\":" JSON_COORD_FMT ","
                "\"goto_target_zoom\":%.6g,"
                "\"goto_animate\":%u,\"goto_delay_s\":%.1f}",
                g_goto_freq, g_goto_target_zoom,
                g_goto_animate, g_goto_delay_s);
            send_json_response(client_fd, 200, "OK", cors, json_body);
        }
        close(client_fd);
        return;
    }

    /* GET /api/waterfall (SSE) */
    if (strcmp(http.method, "GET") == 0 && strcmp(http.path, "/api/waterfall") == 0) {
        char resp[512];
        int n = snprintf(resp, sizeof(resp),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/event-stream\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: keep-alive\r\n%s\r\n", cors);
        if (n < 0 || (size_t)n >= sizeof(resp) ||
            write_response_part(client_fd, resp, (size_t)n) != 0) {
            close(client_fd);
            return;
        }
        sse_add_client(client_fd);
        return; /* keep open */
    }

    /* POST /api/start */
    if (strcmp(http.method, "POST") == 0 && strcmp(http.path, "/api/start") == 0) {
        json_doc_t json;
        char err[160];
        double freq_start_tmp = g_freq_start;
        double freq_end_tmp = g_freq_end;
        double converter_tmp = g_converter_freq;
        double samplerate_tmp = g_samplerate;
        double rf_bandwidth_tmp = g_rf_bandwidth;
        double bw_ratio_tmp = g_bw_ratio;
        double hardwaregain_tmp = g_hardwaregain_db;
        char gain_mode_tmp[32];
        char rf_port_tmp[32];
        char string_tmp[32];
        double visible_start_tmp = 0.0;
        double visible_end_tmp = 0.0;
        uint32_t fft_tmp = (uint32_t)current_fft_size();
        uint32_t display_tmp = (uint32_t)current_display_bins();
        uint32_t min_rate_tmp = g_min_rate_lps;
        uint32_t rate_tmp = g_rate_limit_lps;
        double number_tmp;
        uint32_t uint_tmp;
        int present = 0;
        int have_visible_start = 0;
        int have_visible_end = 0;

        copy_cstr(gain_mode_tmp, sizeof(gain_mode_tmp), g_gain_mode);
        copy_cstr(rf_port_tmp, sizeof(rf_port_tmp), g_rf_port);

        if (json_body_for_request(&http, &json, err, sizeof(err)) != 0) {
            send_json_error(client_fd, 400, "Bad Request", cors, err);
            close(client_fd);
            return;
        }

        if (json_get_double(&json, "freq_start", &number_tmp, &present) != 0) goto start_bad_json;
        if (present) freq_start_tmp = number_tmp * 1.0e6;
        if (json_get_double(&json, "freq_end", &number_tmp, &present) != 0) goto start_bad_json;
        if (present) freq_end_tmp = number_tmp * 1.0e6;
        if (json_get_double(&json, "converter_freq", &number_tmp, &present) != 0) goto start_bad_json;
        if (present) converter_tmp = number_tmp * 1.0e6;
        if (json_get_double(&json, "samplerate", &number_tmp, &present) != 0) goto start_bad_json;
        if (json_get_double(&json, "sample_rate", &number_tmp, &present) != 0) goto start_bad_json;
        if (json_get_double(&json, "rf_bandwidth", &number_tmp, &present) != 0) goto start_bad_json;
        if (json_get_double(&json, "rf_bandwidth_hz", &number_tmp, &present) != 0) goto start_bad_json;
        if (json_get_double(&json, "bw_ratio", &number_tmp, &present) != 0) goto start_bad_json;
        samplerate_tmp = PLUTO_AUTO_SAMPLE_RATE_HZ;
        rf_bandwidth_tmp = PLUTO_AUTO_RF_BANDWIDTH_HZ;
        bw_ratio_tmp = PLUTO_AUTO_BW_RATIO;
        if (json_get_string(&json, "gain_mode", string_tmp, sizeof(string_tmp), &present) != 0) goto start_bad_json;
        if (present)
            copy_cstr(gain_mode_tmp, sizeof(gain_mode_tmp), string_tmp);
        if (json_get_double(&json, "hardwaregain_db", &number_tmp, &present) != 0) goto start_bad_json;
        if (present) hardwaregain_tmp = number_tmp;
        if (json_get_string(&json, "rf_port", string_tmp, sizeof(string_tmp), &present) != 0) goto start_bad_json;
        if (present)
            copy_cstr(rf_port_tmp, sizeof(rf_port_tmp), string_tmp);
        if (json_get_uint(&json, "fft_size", &uint_tmp, &present) != 0) goto start_bad_json;
        if (present) fft_tmp = uint_tmp;
        if (json_get_uint(&json, "display_bins", &uint_tmp, &present) != 0) goto start_bad_json;
        if (present) display_tmp = uint_tmp;
        if (json_get_uint(&json, "min_rate_lps", &uint_tmp, &present) != 0) goto start_bad_json;
        if (present) min_rate_tmp = uint_tmp;
        if (json_get_uint(&json, "rate_limit_lps", &uint_tmp, &present) != 0) goto start_bad_json;
        if (present) rate_tmp = uint_tmp;
        if (json_get_double(&json, "visible_start_hz", &visible_start_tmp, &have_visible_start) != 0) goto start_bad_json;
        if (json_get_double(&json, "visible_end_hz", &visible_end_tmp, &have_visible_end) != 0) goto start_bad_json;

        if (have_visible_start != have_visible_end) {
            send_json_error(client_fd, 400, "Bad Request", cors,
                            "visible_start_hz and visible_end_hz must be sent together");
            close(client_fd);
            return;
        }
        if (have_visible_start &&
            validate_visible_range(visible_start_tmp, visible_end_tmp,
                                   err, sizeof(err)) != 0) {
            send_json_error(client_fd, 400, "Bad Request", cors, err);
            close(client_fd);
            return;
        }
        if (fft_tmp < FFT_SIZE_MIN || fft_tmp > FFT_SIZE_MAX) {
            send_json_error(client_fd, 400, "Bad Request", cors,
                            "fft_size is out of range");
            close(client_fd);
            return;
        }
        if (validate_scan_settings(freq_start_tmp, freq_end_tmp, converter_tmp,
                                   samplerate_tmp, bw_ratio_tmp,
                                   err, sizeof(err)) != 0) {
            send_json_error(client_fd, 400, "Bad Request", cors, err);
            close(client_fd);
            return;
        }
        if (validate_pluto_settings(samplerate_tmp, rf_bandwidth_tmp,
                                    gain_mode_tmp, hardwaregain_tmp,
                                    err, sizeof(err)) != 0) {
            send_json_error(client_fd, 400, "Bad Request", cors, err);
            close(client_fd);
            return;
        }
        if (!rf_port_is_supported(rf_port_tmp)) {
            send_json_error(client_fd, 400, "Bad Request", cors,
                            "rf_port must be A_BALANCED, B_BALANCED, or C_BALANCED");
            close(client_fd);
            return;
        }
        if (!air_band_within_receiver_limits(freq_start_tmp, freq_end_tmp,
                                             converter_tmp)) {
            send_json_error(client_fd, 400, "Bad Request", cors,
                            "frequency range maps outside receiver range after converter conversion");
            close(client_fd);
            return;
        }

        g_freq_start = freq_start_tmp;
        g_freq_end = freq_end_tmp;
        g_converter_freq = converter_tmp;
        g_samplerate = samplerate_tmp;
        g_rf_bandwidth = rf_bandwidth_tmp;
        g_bw_ratio = bw_ratio_tmp;
        g_hardwaregain_db = hardwaregain_tmp;
        copy_cstr(g_gain_mode, sizeof(g_gain_mode), gain_mode_tmp);
        copy_cstr(g_rf_port, sizeof(g_rf_port), rf_port_tmp);
        g_direct_sampling = 0;
        update_fft_size((int)fft_tmp);
        if (display_tmp > 0)
            g_display_bins = normalize_display_bins((int)display_tmp);
        g_min_rate_lps = normalize_min_rate_lps(min_rate_tmp);
        if (rate_tmp > 0)
            g_rate_limit_lps = normalize_rate_limit_lps(rate_tmp);
        if (direct_sampling_enabled()) {
            force_direct_sampling_defaults(!have_visible_start || !have_visible_end);
            if (have_visible_start && have_visible_end) {
                g_visible_start = visible_start_tmp;
                g_visible_end = visible_end_tmp;
                clamp_visible_to_config();
            }
        } else {
            if (g_bw_ratio > 1.0) g_bw_ratio = 1.0;
            if (g_freq_start < MIN_FREQ_START_HZ)
                g_freq_start = MIN_FREQ_START_HZ;
            if (clamp_configured_band_to_receiver_limits() != 0) {
                send_json_error(client_fd, 400, "Bad Request", cors,
                                "converter frequency leaves no valid receiver range");
                close(client_fd);
                return;
            }
            clamp_scan_end_to_hardware_limit();
            if (have_visible_start && have_visible_end) {
                g_visible_start = visible_start_tmp;
                g_visible_end = visible_end_tmp;
                clamp_visible_to_config();
            } else {
                g_visible_start = g_freq_start;
                g_visible_end = g_freq_end;
            }
        }
        g_view_id++;

        save_config();

        int ret = start_scan();
        const char *status = (ret == 0) ? "ok" : "error";
        double step = 0.0;
        double scan_end = 0.0;
        int total_steps = current_scan_plan(&step, &scan_end);
        run_mode_t mode = planned_run_mode();
        int effective_fft = current_effective_fft_size();
        int decim_factor = current_decim_factor();
        int decim_hop = current_decim_hop();
        double overlap_factor = current_overlap_factor();
        uint32_t line_sample_count = current_line_sample_count();
        int minimum_rate_limited = current_minimum_rate_limited();
        int minimum_rate_achieved;
        double raw_line_rate = 0.0;
        double true_line_rate;
        double first_line_ms;
        int rate_drop_factor = rate_drop_factor_for_plan(current_active_samplerate(), line_sample_count,
                                                         total_steps,
                                                         decim_factor,
                                                         g_rate_limit_lps,
                                                         &raw_line_rate);
        true_line_rate = current_true_line_rate();
        first_line_ms = current_first_line_ms(total_steps, line_sample_count);
        minimum_rate_achieved = mode == RUN_MODE_SINGLE ?
            current_minimum_rate_achieved() :
            (g_min_rate_lps == 0 || raw_line_rate >= (double)g_min_rate_lps);
        double traffic_kbytes_s = measured_frontend_kbytes_s();
        double active_samplerate = current_active_samplerate();
        double active_rf_bandwidth = current_active_rf_bandwidth();
        double active_bw_ratio = current_active_bw_ratio();
        double second_if_hz = current_second_if_hz();
        double zero_if_guard_hz = current_zero_if_guard_hz();
        char sample_rates[1024];
        char json_body[API_JSON_BODY_MAX];
        format_sample_rates_json(sample_rates, sizeof(sample_rates));
        int n = snprintf(json_body, sizeof(json_body),
            "{\"status\":\"%s\",\"device\":\"Pluto SDR\","
            "\"sw_version\":\"%s\",\"hardware\":\"%s\","
            "\"firmware\":\"%s\",\"serial\":\"%s\","
            "\"manufacturer\":\"%s\",\"product\":\"%s\","
            "\"receiver_min_hz\":%.0f,\"receiver_max_hz\":%.0f,"
            "\"receiver_range_note\":\"unofficial extended Pluto range\","
            "\"freq_start\":" JSON_COORD_FMT ",\"freq_end\":" JSON_COORD_FMT ","
            "\"configured_start_hz\":" JSON_COORD_FMT ",\"configured_end_hz\":" JSON_COORD_FMT ","
            "\"visible_start_hz\":" JSON_COORD_FMT ",\"visible_end_hz\":" JSON_COORD_FMT ","
            "\"second_if_hz\":" JSON_COORD_FMT ",\"zero_if_guard_hz\":" JSON_COORD_FMT ","
            "\"converter_freq\":" JSON_COORD_FMT ",\"steps\":%d,\"step_hz\":" JSON_COORD_FMT ","
            "\"required_points\":%d,\"mode\":\"%s\","
            "\"min_rate_lps\":%u,\"rate_limit_lps\":%u,"
            "\"rate_drop_factor\":%d,\"raw_line_rate\":%.3f,"
            "\"true_line_rate\":%.3f,"
            "\"first_line_ms\":%.3f,"
            "\"minimum_rate_overlap\":%d,\"minimum_rate_limited\":%d,"
            "\"minimum_rate_achieved\":%d,"
            "\"traffic_kbytes_s\":%.1f,"
            "\"display_bins\":%d,\"view_id\":%u,\"effective_fft_size\":%d,"
            "\"decim_factor\":%d,\"decim_hop\":%d,\"overlap_factor\":%.3f,"
            "\"effective_input_samples\":%u,\"samplerate\":%.0f,\"rf_bandwidth\":%.0f,"
            "\"bw_ratio\":%.2f,"
            "\"hardware_samplerate\":%.0f,\"hardware_rf_bandwidth\":%.0f,"
            "\"hardware_bw_ratio\":%.2f,"
            "\"active_samplerate\":%.0f,\"active_rf_bandwidth\":%.0f,"
            "\"active_bw_ratio\":%.3f,"
            "\"gain_mode\":\"%s\",\"hardwaregain_db\":%.1f,\"rf_port\":\"%s\","
            "\"rssi_db\":%.2f,\"input_peak\":%.0f,\"clipped_samples\":%llu,"
            "\"sample_rates\":%s}",
            status,
            build_sw_version(), g_hw_rev, g_fw_ver, g_serial,
            g_manufacturer, g_product,
            RF_RECEIVER_MIN_HZ, RF_RECEIVER_MAX_HZ,
            g_freq_start, g_freq_end,
            g_freq_start, g_freq_end,
            g_visible_start, g_visible_end,
            second_if_hz, zero_if_guard_hz,
            g_converter_freq, total_steps, step,
            planned_required_points(), run_mode_name(mode),
            g_min_rate_lps, g_rate_limit_lps, rate_drop_factor, raw_line_rate,
            true_line_rate,
            first_line_ms, minimum_rate_limited, minimum_rate_limited,
            minimum_rate_achieved, traffic_kbytes_s,
            current_display_bins(), g_view_id, effective_fft,
            decim_factor, decim_hop, overlap_factor, line_sample_count,
            active_samplerate, active_rf_bandwidth,
            active_bw_ratio,
            active_samplerate, active_rf_bandwidth, active_bw_ratio,
            active_samplerate, active_rf_bandwidth, active_bw_ratio,
            g_gain_mode, g_hardwaregain_db, g_rf_port,
            g_last_rssi_db, g_last_input_peak,
            (unsigned long long)g_last_clipped_samples,
            sample_rates);
        if (n < 0 || (size_t)n >= sizeof(json_body)) {
            send_json_error(client_fd, 500, "Internal Server Error", cors,
                            "Start response too large");
            close(client_fd);
            return;
        }
        send_json_response(client_fd, 200, "OK", cors, json_body);
        close(client_fd);
        return;

start_bad_json:
        send_json_error(client_fd, 400, "Bad Request", cors,
                        "Malformed JSON field in start request");
        close(client_fd);
        return;
    }

    /* POST /api/view */
    if (strcmp(http.method, "POST") == 0 && strcmp(http.path, "/api/view") == 0) {
        json_doc_t json;
        char err[160];
        double visible_start = g_visible_start;
        double visible_end = g_visible_end;
        uint32_t display_tmp = 0;
        double number_tmp;
        uint32_t uint_tmp;
        int present = 0;
        int visible_changed;
        int display_changed = 0;
        int ret = 0;

        if (json_body_for_request(&http, &json, err, sizeof(err)) != 0) {
            send_json_error(client_fd, 400, "Bad Request", cors, err);
            close(client_fd);
            return;
        }
        if (json_get_double(&json, "visible_start_hz", &number_tmp, &present) != 0) {
            send_json_error(client_fd, 400, "Bad Request", cors,
                            "Malformed visible_start_hz");
            close(client_fd);
            return;
        }
        if (present) visible_start = number_tmp;
        if (json_get_double(&json, "visible_end_hz", &number_tmp, &present) != 0) {
            send_json_error(client_fd, 400, "Bad Request", cors,
                            "Malformed visible_end_hz");
            close(client_fd);
            return;
        }
        if (present) visible_end = number_tmp;
        if (json_get_uint(&json, "display_bins", &uint_tmp, &present) != 0) {
            send_json_error(client_fd, 400, "Bad Request", cors,
                            "Malformed display_bins");
            close(client_fd);
            return;
        }
        if (present) display_tmp = uint_tmp;

        if (validate_visible_range(visible_start, visible_end,
                                   err, sizeof(err)) != 0) {
            send_json_error(client_fd, 400, "Bad Request", cors, err);
            close(client_fd);
            return;
        }

        if (display_tmp > 0) {
            int normalized_display = normalize_display_bins((int)display_tmp);
            display_changed = normalized_display != g_display_bins;
            g_display_bins = normalized_display;
        }

        if (visible_start < g_freq_start) visible_start = g_freq_start;
        if (visible_end > g_freq_end) visible_end = g_freq_end;
        if (visible_end <= visible_start) {
            visible_start = g_freq_start;
            visible_end = g_freq_end;
        }

        visible_changed = fabs(visible_start - g_visible_start) >= 1.0 ||
                          fabs(visible_end - g_visible_end) >= 1.0;
        if (visible_changed || display_changed) {
            g_visible_start = visible_start;
            g_visible_end = visible_end;
            g_view_id++;
            if (g_scanning)
                ret = start_scan();
        }
        if (visible_changed || display_changed)
            save_config();

        {
            double step = 0.0;
            double scan_end = 0.0;
            int total_steps = current_scan_plan(&step, &scan_end);
            run_mode_t mode = planned_run_mode();
            int effective_fft = current_effective_fft_size();
            int decim_factor = current_decim_factor();
            int decim_hop = current_decim_hop();
            double overlap_factor = current_overlap_factor();
            uint32_t line_sample_count = current_line_sample_count();
            double raw_line_rate = 0.0;
            double true_line_rate;
            double first_line_ms;
            int minimum_rate_limited = current_minimum_rate_limited();
            int minimum_rate_achieved;
            double visible_raw_bins = current_visible_raw_bins();
            double visible_bins_per_pixel = current_visible_bins_per_pixel();
            int rate_drop_factor = rate_drop_factor_for_plan(current_active_samplerate(), line_sample_count,
                                                             total_steps,
                                                             decim_factor,
                                                             g_rate_limit_lps,
                                                             &raw_line_rate);
            true_line_rate = current_true_line_rate();
            first_line_ms = current_first_line_ms(total_steps, line_sample_count);
            minimum_rate_achieved = mode == RUN_MODE_SINGLE ?
                current_minimum_rate_achieved() :
                (g_min_rate_lps == 0 || raw_line_rate >= (double)g_min_rate_lps);
            double traffic_kbytes_s = measured_frontend_kbytes_s();
            double active_samplerate = current_active_samplerate();
            double active_rf_bandwidth = current_active_rf_bandwidth();
            double active_bw_ratio = current_active_bw_ratio();
            double second_if_hz = current_second_if_hz();
            double zero_if_guard_hz = current_zero_if_guard_hz();
            char json_body[2048];
            snprintf(json_body, sizeof(json_body),
                "{\"status\":\"%s\",\"view_id\":%u,"
                "\"freq_start\":" JSON_COORD_FMT ",\"freq_end\":" JSON_COORD_FMT ","
                "\"configured_start_hz\":" JSON_COORD_FMT ",\"configured_end_hz\":" JSON_COORD_FMT ","
                "\"visible_start_hz\":" JSON_COORD_FMT ",\"visible_end_hz\":" JSON_COORD_FMT ","
                "\"second_if_hz\":" JSON_COORD_FMT ",\"zero_if_guard_hz\":" JSON_COORD_FMT ","
                "\"steps\":%d,\"step_hz\":" JSON_COORD_FMT ",\"display_bins\":%d,"
                "\"required_points\":%d,\"mode\":\"%s\","
                "\"min_rate_lps\":%u,\"rate_limit_lps\":%u,"
                "\"rate_drop_factor\":%d,\"raw_line_rate\":%.3f,"
                "\"true_line_rate\":%.3f,"
                "\"first_line_ms\":%.3f,"
                "\"minimum_rate_overlap\":%d,\"minimum_rate_limited\":%d,"
                "\"minimum_rate_achieved\":%d,"
                "\"visible_raw_bins\":%.3f,\"visible_bins_per_pixel\":%.6f,"
                "\"traffic_kbytes_s\":%.1f,"
                "\"effective_fft_size\":%d,\"decim_factor\":%d,"
                "\"decim_hop\":%d,\"overlap_factor\":%.3f,"
                "\"effective_input_samples\":%u,"
                "\"hardware_samplerate\":%.0f,\"hardware_rf_bandwidth\":%.0f,"
                "\"hardware_bw_ratio\":%.2f,"
                "\"active_samplerate\":%.0f,\"active_rf_bandwidth\":%.0f,"
                "\"active_bw_ratio\":%.3f,"
                "\"samplerate\":%.0f,\"rf_bandwidth\":%.0f,"
                "\"bw_ratio\":%.2f,\"scanning\":%d}",
                (ret == 0) ? "ok" : "error", g_view_id,
                g_freq_start, g_freq_end,
                g_freq_start, g_freq_end,
                g_visible_start, g_visible_end,
                second_if_hz, zero_if_guard_hz,
                total_steps, step, current_display_bins(),
                planned_required_points(), run_mode_name(mode),
                g_min_rate_lps, g_rate_limit_lps, rate_drop_factor, raw_line_rate,
                true_line_rate,
                first_line_ms, minimum_rate_limited, minimum_rate_limited,
                minimum_rate_achieved,
                visible_raw_bins, visible_bins_per_pixel, traffic_kbytes_s,
                effective_fft, decim_factor, decim_hop, overlap_factor, line_sample_count,
                active_samplerate, active_rf_bandwidth, active_bw_ratio,
                active_samplerate, active_rf_bandwidth, active_bw_ratio,
                active_samplerate, active_rf_bandwidth, active_bw_ratio,
                g_scanning);
            send_json_response(client_fd, 200, "OK", cors, json_body);
        }
        close(client_fd);
        return;
    }

    /* POST /api/fft */
    if (strcmp(http.method, "POST") == 0 && strcmp(http.path, "/api/fft") == 0) {
        json_doc_t json;
        char err[160];
        uint32_t fft_tmp = 0;
        int present = 0;

        if (json_body_for_request(&http, &json, err, sizeof(err)) != 0) {
            send_json_error(client_fd, 400, "Bad Request", cors, err);
            close(client_fd);
            return;
        }
        if (json_get_uint(&json, "fft_size", &fft_tmp, &present) != 0 || !present) {
            send_json_error(client_fd, 400, "Bad Request", cors,
                            "fft_size is required");
            close(client_fd);
            return;
        }

        if (fft_tmp >= FFT_SIZE_MIN && fft_tmp <= FFT_SIZE_MAX) {
            update_fft_size((int)fft_tmp);
            save_config();
            char json_body[384];
            snprintf(json_body, sizeof(json_body),
                "{\"status\":\"ok\",\"fft_size\":%d,\"effective_fft_size\":%d,"
                "\"decim_factor\":%d,\"decim_hop\":%d,\"overlap_factor\":%.3f,"
                "\"scanning\":%d}",
                current_fft_size(), current_effective_fft_size(),
                current_decim_factor(), current_decim_hop(), current_overlap_factor(),
                g_scanning);
            send_json_response(client_fd, 200, "OK", cors, json_body);
        } else {
            char json_body[256];
            snprintf(json_body, sizeof(json_body),
                "{\"status\":\"error\",\"fft_size\":%d}",
                current_fft_size());
            send_json_response(client_fd, 400, "Bad Request", cors, json_body);
        }
        close(client_fd);
        return;
    }

    /* POST /api/gain */
    if (strcmp(http.method, "POST") == 0 && strcmp(http.path, "/api/gain") == 0) {
        json_doc_t json;
        char err[160];
        char gain_mode_tmp[32];
        double hardwaregain_tmp = g_hardwaregain_db;
        double number_tmp;
        int present = 0;

        copy_cstr(gain_mode_tmp, sizeof(gain_mode_tmp), g_gain_mode);

        if (json_body_for_request(&http, &json, err, sizeof(err)) != 0) {
            send_json_error(client_fd, 400, "Bad Request", cors, err);
            close(client_fd);
            return;
        }
        if (json_get_string(&json, "gain_mode", gain_mode_tmp, sizeof(gain_mode_tmp), &present) != 0) {
            send_json_error(client_fd, 400, "Bad Request", cors,
                            "Malformed gain_mode");
            close(client_fd);
            return;
        }
        if (json_get_double(&json, "hardwaregain_db", &number_tmp, &present) != 0) {
            send_json_error(client_fd, 400, "Bad Request", cors,
                            "Malformed hardwaregain_db");
            close(client_fd);
            return;
        }
        if (present)
            hardwaregain_tmp = number_tmp;
        if (validate_pluto_settings(g_samplerate, g_rf_bandwidth,
                                    gain_mode_tmp, hardwaregain_tmp,
                                    err, sizeof(err)) != 0) {
            send_json_error(client_fd, 400, "Bad Request", cors, err);
            close(client_fd);
            return;
        }

        copy_cstr(g_gain_mode, sizeof(g_gain_mode), gain_mode_tmp);
        g_hardwaregain_db = hardwaregain_tmp;
        int ret = apply_gain_settings();
        const char *status = (ret == PLUTO_ERR_OK) ? "ok" : "error";
        char json_body[384];
        snprintf(json_body, sizeof(json_body),
            "{\"status\":\"%s\",\"gain_mode\":\"%s\",\"hardwaregain_db\":%.1f,"
            "\"rssi_db\":%.2f,\"input_peak\":%.0f,\"clipped_samples\":%llu}",
            status, g_gain_mode, g_hardwaregain_db,
            g_last_rssi_db, g_last_input_peak,
            (unsigned long long)g_last_clipped_samples);
        send_json_response(client_fd, 200, "OK", cors, json_body);
        close(client_fd);
        return;
    }

    /* POST /api/rate */
    if (strcmp(http.method, "POST") == 0 && strcmp(http.path, "/api/rate") == 0) {
        json_doc_t json;
        char err[160];
        uint32_t rate_tmp = 0;
        uint32_t min_rate_tmp = g_min_rate_lps;
        int present = 0;
        int min_present = 0;
        int rate_changed = 0;
        int min_changed = 0;
        int restart_needed = 0;
        int ret = 0;

        if (json_body_for_request(&http, &json, err, sizeof(err)) != 0) {
            send_json_error(client_fd, 400, "Bad Request", cors, err);
            close(client_fd);
            return;
        }
        if (json_get_uint(&json, "rate_limit_lps", &rate_tmp, &present) != 0 || !present) {
            send_json_error(client_fd, 400, "Bad Request", cors,
                            "rate_limit_lps is required");
            close(client_fd);
            return;
        }
        if (json_get_uint(&json, "min_rate_lps", &min_rate_tmp, &min_present) != 0) {
            send_json_error(client_fd, 400, "Bad Request", cors,
                            "Malformed min_rate_lps");
            close(client_fd);
            return;
        }

        if (rate_tmp > 0) {
            uint32_t normalized_rate = normalize_rate_limit_lps(rate_tmp);
            rate_changed = normalized_rate != g_rate_limit_lps;
            g_rate_limit_lps = normalized_rate;
        }
        if (min_present) {
            uint32_t normalized_min = normalize_min_rate_lps(min_rate_tmp);
            min_changed = normalized_min != g_min_rate_lps;
            g_min_rate_lps = normalized_min;
        }
        if (rate_changed || min_changed) {
            save_config();
            restart_needed = rate_changed ||
                (min_changed && planned_run_mode() == RUN_MODE_SINGLE);
            if (g_scanning && restart_needed) {
                g_view_id++;
                ret = start_scan();
            }
        }

        {
            double step = 0.0;
            double scan_end = 0.0;
            int total_steps = current_scan_plan(&step, &scan_end);
            run_mode_t mode = planned_run_mode();
            int effective_fft = current_effective_fft_size();
            int decim_factor = current_decim_factor();
            int decim_hop = current_decim_hop();
            double overlap_factor = current_overlap_factor();
            uint32_t line_sample_count = current_line_sample_count();
            double raw_line_rate = 0.0;
            double true_line_rate;
            double first_line_ms;
            int minimum_rate_limited = current_minimum_rate_limited();
            int minimum_rate_achieved;
            int rate_drop_factor = rate_drop_factor_for_plan(current_active_samplerate(), line_sample_count,
                                                             total_steps,
                                                             decim_factor,
                                                             g_rate_limit_lps,
                                                             &raw_line_rate);
            true_line_rate = current_true_line_rate();
            first_line_ms = current_first_line_ms(total_steps, line_sample_count);
            minimum_rate_achieved = mode == RUN_MODE_SINGLE ?
                current_minimum_rate_achieved() :
                (g_min_rate_lps == 0 || raw_line_rate >= (double)g_min_rate_lps);
            double traffic_kbytes_s = measured_frontend_kbytes_s();
            double active_samplerate = current_active_samplerate();
            double active_rf_bandwidth = current_active_rf_bandwidth();
            double active_bw_ratio = current_active_bw_ratio();
            char json_body[2048];
            snprintf(json_body, sizeof(json_body),
                "{\"status\":\"%s\",\"min_rate_lps\":%u,\"rate_limit_lps\":%u,"
                "\"rate_drop_factor\":%d,\"raw_line_rate\":%.3f,"
                "\"true_line_rate\":%.3f,"
                "\"first_line_ms\":%.3f,"
                "\"traffic_kbytes_s\":%.1f,"
                "\"mode\":\"%s\",\"view_id\":%u,\"effective_fft_size\":%d,"
                "\"decim_factor\":%d,\"decim_hop\":%d,\"overlap_factor\":%.3f,"
                "\"minimum_rate_overlap\":%d,\"minimum_rate_limited\":%d,"
                "\"minimum_rate_achieved\":%d,"
                "\"effective_input_samples\":%u,"
                "\"active_samplerate\":%.0f,\"active_rf_bandwidth\":%.0f,"
                "\"active_bw_ratio\":%.3f,"
                "\"scanning\":%d}",
                (ret == 0) ? "ok" : "error",
                g_min_rate_lps, g_rate_limit_lps, rate_drop_factor, raw_line_rate,
                true_line_rate,
                first_line_ms, traffic_kbytes_s,
                run_mode_name(mode), g_view_id, effective_fft,
                decim_factor, decim_hop, overlap_factor,
                minimum_rate_limited, minimum_rate_limited,
                minimum_rate_achieved,
                line_sample_count,
                active_samplerate, active_rf_bandwidth, active_bw_ratio,
                g_scanning);
            send_json_response(client_fd, 200, "OK", cors, json_body);
        }
        close(client_fd);
        return;
    }

    /* POST /api/min-rate */
    if (strcmp(http.method, "POST") == 0 && strcmp(http.path, "/api/min-rate") == 0) {
        json_doc_t json;
        char err[160];
        uint32_t min_rate_tmp = 0;
        int present = 0;
        int ret = 0;

        if (json_body_for_request(&http, &json, err, sizeof(err)) != 0) {
            send_json_error(client_fd, 400, "Bad Request", cors, err);
            close(client_fd);
            return;
        }
        if (json_get_uint(&json, "min_rate_lps", &min_rate_tmp, &present) != 0 || !present) {
            send_json_error(client_fd, 400, "Bad Request", cors,
                            "min_rate_lps is required");
            close(client_fd);
            return;
        }

        g_min_rate_lps = normalize_min_rate_lps(min_rate_tmp);
        save_config();
        if (g_scanning && planned_run_mode() == RUN_MODE_SINGLE) {
            g_view_id++;
            ret = start_scan();
        }

        {
            double step = 0.0;
            double scan_end = 0.0;
            int total_steps = current_scan_plan(&step, &scan_end);
            run_mode_t mode = planned_run_mode();
            int effective_fft = current_effective_fft_size();
            int decim_factor = current_decim_factor();
            int decim_hop = current_decim_hop();
            double overlap_factor = current_overlap_factor();
            uint32_t line_sample_count = current_line_sample_count();
            double raw_line_rate = 0.0;
            double true_line_rate;
            double first_line_ms;
            int minimum_rate_limited = current_minimum_rate_limited();
            int minimum_rate_achieved;
            int rate_drop_factor = rate_drop_factor_for_plan(current_active_samplerate(), line_sample_count,
                                                             total_steps,
                                                             decim_factor,
                                                             g_rate_limit_lps,
                                                             &raw_line_rate);
            true_line_rate = current_true_line_rate();
            first_line_ms = current_first_line_ms(total_steps, line_sample_count);
            minimum_rate_achieved = mode == RUN_MODE_SINGLE ?
                current_minimum_rate_achieved() :
                (g_min_rate_lps == 0 || raw_line_rate >= (double)g_min_rate_lps);
            double traffic_kbytes_s = measured_frontend_kbytes_s();
            double active_samplerate = current_active_samplerate();
            double active_rf_bandwidth = current_active_rf_bandwidth();
            double active_bw_ratio = current_active_bw_ratio();
            char json_body[2048];
            snprintf(json_body, sizeof(json_body),
                "{\"status\":\"%s\",\"min_rate_lps\":%u,"
                "\"rate_drop_factor\":%d,\"raw_line_rate\":%.3f,"
                "\"true_line_rate\":%.3f,"
                "\"first_line_ms\":%.3f,"
                "\"traffic_kbytes_s\":%.1f,"
                "\"mode\":\"%s\",\"view_id\":%u,\"effective_fft_size\":%d,"
                "\"decim_factor\":%d,\"decim_hop\":%d,\"overlap_factor\":%.3f,"
                "\"minimum_rate_overlap\":%d,\"minimum_rate_limited\":%d,"
                "\"minimum_rate_achieved\":%d,"
                "\"effective_input_samples\":%u,"
                "\"active_samplerate\":%.0f,\"active_rf_bandwidth\":%.0f,"
                "\"active_bw_ratio\":%.3f,\"scanning\":%d}",
                (ret == 0) ? "ok" : "error",
                g_min_rate_lps, rate_drop_factor, raw_line_rate,
                true_line_rate,
                first_line_ms, traffic_kbytes_s,
                run_mode_name(mode), g_view_id, effective_fft,
                decim_factor, decim_hop, overlap_factor,
                minimum_rate_limited, minimum_rate_limited,
                minimum_rate_achieved,
                line_sample_count,
                active_samplerate, active_rf_bandwidth, active_bw_ratio,
                g_scanning);
            send_json_response(client_fd, 200, "OK", cors, json_body);
        }
        close(client_fd);
        return;
    }

    /* POST /api/stop */
    if (strcmp(http.method, "POST") == 0 && strcmp(http.path, "/api/stop") == 0) {
        clear_auto_restart_request(PLUTO_AUTO_RESTART_SUPPRESS_MS);
        stop_scan();
        send_json_response(client_fd, 200, "OK", cors, "{\"status\":\"ok\"}");
        close(client_fd);
        return;
    }

    if (strcmp(http.method, "GET") != 0 &&
        strcmp(http.method, "POST") != 0) {
        send_json_error(client_fd, 405, "Method Not Allowed", cors,
                        "Unsupported HTTP method");
        close(client_fd);
        return;
    }

    /* 404 */
    send_empty_response(client_fd, 404, "Not Found", cors);
    close(client_fd);
}

/* ------------------------------------------------------------------ */
/* Main                                                               */
/* ------------------------------------------------------------------ */
static volatile int g_exit = 0;
static void sigint_handler(int sig) { (void)sig; g_exit = 1; }

/**
 * @brief Create the HTTP listening socket for the configured bind endpoint.
 *
 * `g_bind_address` may be a concrete IPv4/IPv6 address, `localhost`, or `*`.
 * `*` means all local interfaces and is intended for trusted LAN use.
 *
 * @return Listening socket descriptor, or `-1` on bind/listen failure.
 */
static int create_http_server_socket(void)
{
    struct addrinfo hints;
    struct addrinfo *results = NULL;
    char port[16];
    const char *node = g_bind_address;
    int gai;
    int last_error = 0;

    snprintf(port, sizeof(port), "%d", g_http_port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    if (!node || !*node || strcmp(node, "*") == 0)
        node = NULL;

    gai = getaddrinfo(node, port, &hints, &results);
    if (gai != 0) {
        fprintf(stderr, "Invalid bind address/port %s:%s: %s\n",
                node ? node : "*", port, gai_strerror(gai));
        return -1;
    }

    for (struct addrinfo *rp = results; rp; rp = rp->ai_next) {
        int fd = (int)socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        int opt = 1;
        int v6only = 0;

        if (fd < 0)
            continue;
#ifdef _WIN32
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
                   (const char *)&opt, sizeof(opt));
        if (rp->ai_family == AF_INET6)
            setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY,
                       (const char *)&v6only, sizeof(v6only));
#else
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        if (rp->ai_family == AF_INET6)
            setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY,
                       &v6only, sizeof(v6only));
#endif

        if (bind(fd, rp->ai_addr, (socklen_t)rp->ai_addrlen) == 0 &&
            listen(fd, MAX_CLIENTS) == 0) {
            freeaddrinfo(results);
            return fd;
        }
        last_error = errno;
        close(fd);
    }

    freeaddrinfo(results);
    if (last_error)
        errno = last_error;
    return -1;
}

/**
 * @brief Format the browser URL shown in the startup banner.
 *
 * @param out Destination buffer.
 * @param out_len Destination length in bytes.
 */
static void format_listen_url(char *out, size_t out_len)
{
    const char *host = g_bind_address;
    if (!out || out_len == 0)
        return;
    if (!host || !*host || strcmp(host, "*") == 0 ||
        strcmp(host, "0.0.0.0") == 0 || strcmp(host, "::") == 0)
        host = "localhost";
    else if (strcmp(host, "127.0.0.1") == 0 || strcmp(host, "::1") == 0)
        host = "localhost";

    if (strchr(host, ':') && host[0] != '[')
        snprintf(out, out_len, "http://[%s]:%d", host, g_http_port);
    else
        snprintf(out, out_len, "http://%s:%d", host, g_http_port);
}

static long long now_msec(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (long long)ts.tv_sec * 1000LL + (long long)ts.tv_nsec / 1000000LL;
}

static void poll_device_reconnect(void)
{
#if PSEUDO_RANDOM_SAMPLE_SOURCE
    return;
#else
    static long long last_poll = 0;
    long long now = now_msec();

    if (!g_auto_restart_on_reconnect || g_dev || g_scanning ||
        now - last_poll < 5000)
        return;
    if (now < g_auto_restart_suppress_until_msec)
        return;

    last_poll = now;
    if (open_first_device(0) == PLUTO_ERR_OK) {
        printf("[SDR] Device reconnected: %s %s\n", g_manufacturer, g_product);
        if (g_auto_restart_on_reconnect) {
            int ret;
            printf("[SDR] Auto-restarting scan after reconnect\n");
            ret = start_scan();
            if (ret == 0) {
                g_auto_restart_on_reconnect = 0;
                g_last_auto_restart_msec = now_msec();
                g_auto_restart_suppress_until_msec =
                    g_last_auto_restart_msec + PLUTO_AUTO_RESTART_SUPPRESS_MS;
            } else {
                g_auto_restart_on_reconnect = 0;
                fprintf(stderr,
                        "[SDR] Auto-restart after reconnect failed; manual Start required\n");
            }
        }
    }
#endif
}

/**
 * @brief Test whether an argv token is the URI option.
 *
 * Accepts normal `--uri` and common pasted en/em dash variants so copied
 * commands from formatted text do not silently skip the Pluto URI override.
 *
 * @param arg Command-line argument token.
 * @return Non-zero when the token names the URI option.
 */
static int is_uri_option(const char *arg)
{
    return arg &&
        (strcmp(arg, "--uri") == 0 ||
         strcmp(arg, "\342\200\223uri") == 0 ||
         strcmp(arg, "\342\200\224uri") == 0);
}

/**
 * @brief Return the value part of a `--uri=value` style option.
 *
 * Supports normal hyphen, en dash, and em dash prefixes.
 *
 * @param arg Command-line argument token.
 * @return Pointer to the value after `=`, or `NULL` when not a URI assignment.
 */
static const char *uri_option_value(const char *arg)
{
    static const char opt_plain[] = "--uri=";
    static const char opt_en_dash[] = "\342\200\223uri=";
    static const char opt_em_dash[] = "\342\200\224uri=";

    if (!arg)
        return NULL;
    if (strncmp(arg, opt_plain, sizeof(opt_plain) - 1) == 0)
        return arg + sizeof(opt_plain) - 1;
    if (strncmp(arg, opt_en_dash, sizeof(opt_en_dash) - 1) == 0)
        return arg + sizeof(opt_en_dash) - 1;
    if (strncmp(arg, opt_em_dash, sizeof(opt_em_dash) - 1) == 0)
        return arg + sizeof(opt_em_dash) - 1;
    return NULL;
}

/**
 * @brief Parse a TCP port string from the command line.
 *
 * @param text Decimal port text.
 * @param out_port Destination port number.
 * @return Zero on success, `-1` when invalid or outside `1..65535`.
 */
static int parse_http_port(const char *text, int *out_port)
{
    char *end = NULL;
    long value;

    if (!text || !*text || !out_port)
        return -1;
    errno = 0;
    value = strtol(text, &end, 10);
    if (errno != 0 || !end || *end || value < 1 || value > 65535)
        return -1;
    *out_port = (int)value;
    return 0;
}

/**
 * @brief Return the value part of `--name=value`.
 *
 * @param arg Command-line argument token.
 * @param name Long option name including the leading `--`.
 * @return Pointer after `=`, or `NULL` when `arg` is not that assignment.
 */
static const char *long_option_value(const char *arg, const char *name)
{
    size_t n;

    if (!arg || !name)
        return NULL;
    n = strlen(name);
    if (strncmp(arg, name, n) != 0 || arg[n] != '=')
        return NULL;
    return arg + n + 1;
}

/**
 * @brief Print the browser URL in an ASCII-only terminal banner.
 *
 * Windows consoles do not reliably render UTF-8 box-drawing characters unless
 * the active code page and font are configured. ASCII keeps release binaries
 * readable in cmd.exe, PowerShell, Windows Terminal, and POSIX terminals.
 *
 * @param url Browser URL served by the local HTTP backend.
 */
static void print_startup_banner(const char *url)
{
    printf("\n");
    printf("+------------------------------------------------+\n");
    printf("| %-46s |\n", PROGRAM_TITLE);
    printf("| %-46s |\n", url ? url : "");
    printf("+------------------------------------------------+\n");
    printf("\n");
}

int main(int argc, char **argv)
{
    char cli_uri[128] = {0};
    char normalized_uri[128];
    char url[192];
    chdir_to_executable_dir(argv ? argv[0] : NULL);

    for (int i = 1; i < argc; i++) {
        const char *uri_value = uri_option_value(argv[i]);
        const char *port_value = long_option_value(argv[i], "--port");
        const char *bind_value = long_option_value(argv[i], "--bind");
        if (!bind_value)
            bind_value = long_option_value(argv[i], "--listen");
        if (is_uri_option(argv[i])) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing value for --uri\n");
                return 2;
            }
            copy_cstr(cli_uri, sizeof(cli_uri), argv[++i]);
        } else if (uri_value) {
            if (!*uri_value) {
                fprintf(stderr, "Missing value for --uri\n");
                return 2;
            }
            copy_cstr(cli_uri, sizeof(cli_uri), uri_value);
        } else if (strcmp(argv[i], "--port") == 0) {
            int port;
            if (i + 1 >= argc || parse_http_port(argv[i + 1], &port) != 0) {
                fprintf(stderr, "Invalid or missing value for --port\n");
                return 2;
            }
            g_http_port = port;
            i++;
        } else if (port_value) {
            int port;
            if (parse_http_port(port_value, &port) != 0) {
                fprintf(stderr, "Invalid value for --port: %s\n", port_value);
                return 2;
            }
            g_http_port = port;
        } else if (strcmp(argv[i], "--bind") == 0 ||
                   strcmp(argv[i], "--listen") == 0) {
            if (i + 1 >= argc || !argv[i + 1][0]) {
                fprintf(stderr, "Missing value for %s\n", argv[i]);
                return 2;
            }
            copy_cstr(g_bind_address, sizeof(g_bind_address), argv[++i]);
        } else if (bind_value) {
            if (!*bind_value) {
                fprintf(stderr, "Missing value for --bind\n");
                return 2;
            }
            copy_cstr(g_bind_address, sizeof(g_bind_address), bind_value);
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s [--uri ip:192.168.2.1] [--bind 127.0.0.1] [--port 8080]\n", argv[0]);
            printf("       %s [--uri 192.168.2.1]\n", argv[0]);
            printf("       %s [--uri pluto.local]\n", argv[0]);
            printf("       %s [--listen 0.0.0.0] [--port 8080]\n", argv[0]);
            printf("       %s --bind 0.0.0.0 --port 8080   # trusted LAN access\n", argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            fprintf(stderr, "Use --help for usage.\n");
            return 2;
        }
    }

    signal(SIGINT, sigint_handler);
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#else
    {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            fprintf(stderr, "WSAStartup failed\n");
            return 1;
        }
    }
#endif

    load_config();
    if (g_pluto_uri[0]) {
        normalize_pluto_uri(g_pluto_uri, normalized_uri, sizeof(normalized_uri));
        copy_cstr(g_pluto_uri, sizeof(g_pluto_uri), normalized_uri);
    }
    if (cli_uri[0]) {
        normalize_pluto_uri(cli_uri, normalized_uri, sizeof(normalized_uri));
        copy_cstr(g_pluto_uri, sizeof(g_pluto_uri), normalized_uri);
    }

    init_window();
    memset(g_sse_fds, 0, sizeof(g_sse_fds));

    /* Print libiio info. Pluto hardware is opened lazily on Start so the
       browser UI remains reachable even when the default network URI is down. */
    {
        char lib_ver[64], drv_ver[64];
        pluto_sdr_get_api_info(lib_ver, drv_ver);
        printf("[SDR] API: %s (drv: %s), URI: %s\n", lib_ver, drv_ver, pluto_context_uri());
    }

    int server_fd = create_http_server_socket();
    if (server_fd < 0) {
        perror("bind/listen");
        if (g_dev) {
            pluto_sdr_close(g_dev);
            g_dev = NULL;
        }
        return 1;
    }

    format_listen_url(url, sizeof(url));
    print_startup_banner(url);
    printf("[SDR] HTTP listening on %s:%d\n", g_bind_address, g_http_port);

    printf("[SDR] Waiting for scan start from web UI\n");

    while (!g_exit) {
        struct sockaddr_storage client_addr;
        socklen_t client_len = sizeof(client_addr);
        fd_set readfds;
        struct timeval wait_to;
        int ready;
        int client_fd;

        poll_device_reconnect();
        stop_scan_if_frontend_idle();

        FD_ZERO(&readfds);
        FD_SET(server_fd, &readfds);
        wait_to.tv_sec = 1;
        wait_to.tv_usec = 0;
        ready = select(server_fd + 1, &readfds, NULL, NULL, &wait_to);
        if (ready < 0) {
            if (errno == EINTR) continue;
            perror("select"); break;
        }
        if (ready == 0)
            continue;

        client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept"); break;
        }

#ifdef _WIN32
        {
            DWORD timeout_ms = 5000;
            setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO,
                       (const char *)&timeout_ms, sizeof(timeout_ms));
        }
#else
        {
            struct timeval to;
            to.tv_sec = 5; to.tv_usec = 0;
            setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof(to));
        }
#endif

        char req[MAX_REQUEST + 1] = {0};
        int total = 0;
        int header_len = 0;
        int body_len = 0;
        int truncated = 0;

        while (total < MAX_REQUEST) {
            int n = (int)read(client_fd, req + total, (size_t)(MAX_REQUEST - total));
            if (n <= 0) break;
            total += n;
            req[total] = 0;
            if (!header_len) {
                char *header_end = strstr(req, "\r\n\r\n");
                if (header_end) {
                    header_len = (int)(header_end - req) + 4;
                    body_len = http_content_length(req);
                    if (body_len < 0)
                        break;
                }
            }
            if (header_len && body_len >= 0 && total >= header_len + body_len)
                break;
        }
        if (total >= MAX_REQUEST)
            truncated = 1;

        if (total > 0) handle_request(client_fd, req, (size_t)total, truncated);
        else close(client_fd);
    }

    stop_scan();
    if (g_dev) pluto_sdr_close(g_dev);
    close(server_fd);
#ifdef _WIN32
    WSACleanup();
#endif
    printf("\n[Server] Shutdown.\n");
    return 0;
}
