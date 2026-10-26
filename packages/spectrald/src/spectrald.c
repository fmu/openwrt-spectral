// SPDX-License-Identifier: MIT
/*
 * spectrald - ath10k spectral-scan FFT daemon for LuCI
 *
 * Reads the TLV stream from
 *   /sys/kernel/debug/ieee80211/phyN/ath10k/spectral_scan0
 * (enabled by CONFIG_ATH10K_SPECTRAL in the ath10k/ath10k-ct build),
 * converts each FFT record to per-bin dBm, aggregates ~10 rows/s and
 * serves them over a ubus object called "spectral".
 *
 * Two modes:
 *   channel : live waterfall of the current channel (continuous samples)
 *   sweep   : periodically scans a channel list and stitches the per-channel
 *             FFTs into a panorama (wideband waterfall); scans briefly take
 *             the radio off-channel, so it is more intrusive than channel mode
 *
 * ubus API:
 *   status -> {ok,running,mode,phy,epoch,samples,rate,rows,uptime,
 *              freq,width,cols,row_ms, sweep_*?, error?}
 *   start  {phy:int, bins?:int, mode?:str, preset?:str, freqs?:str}
 *   stop
 *   get    {cursor?:int} ->
 *          {ok,cursor,reset,epoch,cols,freq,width,rows,row_ms,mode,data:[u16...]}
 *
 * "data" is a flat row-major array of rows*cols values, value = dBm+128.
 * Row flush period is 100 ms; the ring keeps the last 60 s.
 * In sweep mode one row is emitted per 100 ms; pan_max fades by 1 dB per row
 * so the panorama shows a scanning-analyzer style waterfall.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <libubox/blobmsg_json.h>
#include <libubox/uloop.h>
#include <libubus.h>

#define MAX_BINS		256
#define MAX_COLS		1024	/* panorama grid columns */
#define MAX_ROWS		600	/* 60 s at 10 rows/s */
#define MAX_SWEEP_FREQS		64
#define CHANNEL_ROW_MS		100
#define SWEEP_ROW_MS		200
#define AUTO_STOP_MS		60000
#define MAX_REPLY_ROWS		200
#define READ_INTERVAL_MS	50
#define SCAN_INTERVAL_MS	100
#define SWEEP_DWELL_MS		200	/* minimum gap between scan commands */
#define SWEEP_DECAY_DB		1	/* pan_max fade per row */
#define PARSER_BUF		4096
#define DB_OFFSET		128	/* stored value = dBm + 128 */
#define IW_PATH			"/usr/sbin/iw"

enum spectral_mode {
	MODE_CHANNEL = 0,
	MODE_SWEEP,
};

struct row {
	int cols;
	uint8_t data[MAX_COLS];
};

static struct {
	struct ubus_context *ctx;

	struct uloop_timeout read_timer;
	struct uloop_timeout second_timer;
	struct uloop_timeout scan_timer;

	bool running;
	int mode;
	int phy;
	int fd;
	char ctl_path[256];
	char scan_path[256];
	char error[160];

	int freq;
	int width;
	int cols;
	int epoch;

	/* per-channel row building */
	int16_t row_max[MAX_BINS];
	int row_samples;
	int row_ms;
	uint64_t row_start_ms;

	/* ring */
	struct row rows[MAX_ROWS];
	int ring_head;
	int ring_count;
	uint64_t row_seq;

	/* sweep */
	char ifname[64];
	int freqs[MAX_SWEEP_FREQS];
	int nfreqs;
	int freq_idx;
	int scan_freq;
	pid_t scan_pid;
	uint64_t last_scan_ms;
	uint64_t sweep_cycles;
	int grid_lo;
	int grid_step;
	int16_t pan_max[MAX_COLS];

	/* stats */
	uint64_t samples;
	uint64_t rate;
	uint64_t last_samples;
	uint64_t start_ms;
	uint64_t last_get_ms;

	uint8_t pbuf[PARSER_BUF];
	size_t pbhave;
} S = {
	.fd = -1,
	.phy = -1,
	.scan_pid = -1,
};

static int16_t db_lut[8][256];

static void drain_relay(int fd);

static uint64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static int write_sysfs(const char *path, const char *val)
{
	int fd = open(path, O_WRONLY);
	ssize_t n;

	if (fd < 0)
		return -errno;

	n = write(fd, val, strlen(val));
	close(fd);
	return (n == (ssize_t)strlen(val)) ? 0 : -EIO;
}

static void set_error(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(S.error, sizeof(S.error), fmt, ap);
	va_end(ap);

	fprintf(stderr, "spectrald: %s\n", S.error);
}

static const char *mode_name(void)
{
	return S.mode == MODE_SWEEP ? "sweep" : "channel";
}

static void reset_rows(void)
{
	memset(S.rows, 0, sizeof(S.rows));
	S.ring_head = 0;
	S.ring_count = 0;
	S.row_seq = 0;
	S.row_samples = 0;
	S.row_ms = CHANNEL_ROW_MS;
	S.row_start_ms = now_ms();
	for (int i = 0; i < MAX_BINS; i++)
		S.row_max[i] = INT16_MIN;
	for (int i = 0; i < MAX_COLS; i++)
		S.pan_max[i] = -128;
}

static void kill_scan_child(void)
{
	if (S.scan_pid > 0) {
		kill(S.scan_pid, SIGKILL);
		waitpid(S.scan_pid, NULL, 0);
		S.scan_pid = -1;
	}
}

static void stop_scan(void)
{
	int saved = errno;

	kill_scan_child();

	if (S.fd >= 0) {
		close(S.fd);
		S.fd = -1;
	}

	if (S.ctl_path[0])
		write_sysfs(S.ctl_path, "disable");

	if (S.running) {
		/* flush a final row so the UI does not lose the tail */
		if (S.mode == MODE_SWEEP) {
			if (S.cols > 0) {
				struct row *r = &S.rows[S.ring_head];
				r->cols = S.cols;
				for (int i = 0; i < S.cols; i++)
					r->data[i] = (uint8_t)(S.pan_max[i] + DB_OFFSET);
				S.ring_head = (S.ring_head + 1) % MAX_ROWS;
				if (S.ring_count < MAX_ROWS)
					S.ring_count++;
				S.row_seq++;
			}
		} else if (S.row_samples > 0) {
			struct row *r = &S.rows[S.ring_head];
			r->cols = S.cols;
			for (int i = 0; i < S.cols; i++)
				r->data[i] = (uint8_t)(S.row_max[i] + DB_OFFSET);
			S.ring_head = (S.ring_head + 1) % MAX_ROWS;
			if (S.ring_count < MAX_ROWS)
				S.ring_count++;
			S.row_seq++;
			S.row_samples = 0;
		}
	}

	S.running = false;
	S.phy = -1;
	errno = saved;
}

static int find_ifname(int phy, char *out, size_t len)
{
	FILE *p;
	char line[128];
	int cur = -1;

	p = popen("iw dev 2>/dev/null", "r");
	if (!p)
		return -1;

	while (fgets(line, sizeof(line), p)) {
		char name[64];

		if (sscanf(line, "phy#%d", &cur) == 1)
			continue;
		if (sscanf(line, " Interface %63s", name) == 1 && cur == phy) {
			snprintf(out, len, "%s", name);
			pclose(p);
			return 0;
		}
	}
	pclose(p);
	return -1;
}

static int parse_freqs(const char *str, int *freqs, int *count)
{
	char buf[512];
	char *tok, *save = NULL;
	int n = 0;

	snprintf(buf, sizeof(buf), "%s", str);
	for (tok = strtok_r(buf, " ,\t", &save); tok && n < MAX_SWEEP_FREQS;
	     tok = strtok_r(NULL, " ,\t", &save)) {
		int f = atoi(tok);

		if (f < 2300 || f > 6000)
			return -EINVAL;
		freqs[n++] = f;
	}
	*count = n;
	return n > 0 ? 0 : -EINVAL;
}

static int preset_freqs(const char *preset, int *freqs, int *count)
{
	static const int f2g[] = {
		2412, 2417, 2422, 2427, 2432, 2437, 2442, 2447,
		2452, 2457, 2462, 2467, 2472
	};
	static const int f5g[] = {
		5180, 5200, 5220, 5240, 5260, 5280, 5300, 5320,
		5500, 5520, 5540, 5560, 5580, 5600, 5620, 5640,
		5660, 5680, 5700, 5720, 5745, 5765, 5785, 5805, 5825
	};
	static const int f5g_fast[] = {
		5180, 5260, 5500, 5580, 5660, 5745, 5825
	};
	const int *src;
	int n;

	if (!preset || !preset[0] || !strcmp(preset, "2g")) {
		if (preset && !strcmp(preset, "2g")) {
			src = f2g;
			n = ARRAY_SIZE(f2g);
		} else {
			return -EINVAL;
		}
	} else if (!strcmp(preset, "5g")) {
		src = f5g;
		n = ARRAY_SIZE(f5g);
	} else if (!strcmp(preset, "5g-fast")) {
		src = f5g_fast;
		n = ARRAY_SIZE(f5g_fast);
	} else {
		return -EINVAL;
	}

	if (n > MAX_SWEEP_FREQS)
		n = MAX_SWEEP_FREQS;
	memcpy(freqs, src, n * sizeof(int));
	*count = n;
	return 0;
}

static void setup_grid(void)
{
	int lo = S.freqs[0], hi = S.freqs[0];
	int span, step, cols;

	for (int i = 1; i < S.nfreqs; i++) {
		if (S.freqs[i] < lo)
			lo = S.freqs[i];
		if (S.freqs[i] > hi)
			hi = S.freqs[i];
	}

	/* pad by half the widest FFT window of the band */
	{
		int pad = S.freqs[0] < 3000 ? 12 : 44;

		lo -= pad;
		hi += pad;
	}
	span = hi - lo;
	if (span < 1)
		span = 1;

	step = 1;
	cols = span / step + 1;
	if (cols > MAX_COLS) {
		step = (span + MAX_COLS - 2) / (MAX_COLS - 1);
		if (step < 1)
			step = 1;
		cols = span / step + 1;
	}

	S.grid_lo = lo;
	S.grid_step = step;
	S.cols = cols;
	S.freq = (lo + hi) / 2;
	S.width = hi - lo;
}

static int start_scan(int phy, int bins, const char *mode, const char *preset,
		      const char *freqs_str, char *err, size_t errlen)
{
	char path[256];
	bool sweep = false;

	if (phy < 0 || phy > 15) {
		snprintf(err, errlen, "invalid phy index %d", phy);
		return -EINVAL;
	}
	if (bins && bins != 64 && bins != 128 && bins != 256) {
		snprintf(err, errlen, "invalid bins value %d (64, 128 or 256)", bins);
		return -EINVAL;
	}
	if (mode && mode[0] && strcmp(mode, "channel") && strcmp(mode, "sweep")) {
		snprintf(err, errlen, "invalid mode '%s' (channel or sweep)", mode);
		return -EINVAL;
	}
	sweep = mode && !strcmp(mode, "sweep");

	stop_scan();

	if (sweep) {
		int ret;

		S.nfreqs = 0;
		if (freqs_str && freqs_str[0])
			ret = parse_freqs(freqs_str, S.freqs, &S.nfreqs);
		else
			ret = preset_freqs(preset, S.freqs, &S.nfreqs);
		if (ret != 0) {
			snprintf(err, errlen,
				 "sweep needs a valid preset (2g, 5g, 5g-fast) "
				 "or a freqs list");
			return -EINVAL;
		}
		if (access(IW_PATH, X_OK) != 0) {
			snprintf(err, errlen, "%s not found (install the 'iw' package)",
				 IW_PATH);
			return -ENOENT;
		}
		if (find_ifname(phy, S.ifname, sizeof(S.ifname)) != 0) {
			snprintf(err, errlen, "no wireless interface found for phy%d", phy);
			return -ENODEV;
		}
		if (!bins)
			bins = S.freqs[0] < 3000 ? 64 : 256;
	}

	snprintf(S.ctl_path, sizeof(S.ctl_path),
		 "/sys/kernel/debug/ieee80211/phy%d/ath10k/spectral_scan_ctl", phy);
	snprintf(S.scan_path, sizeof(S.scan_path),
		 "/sys/kernel/debug/ieee80211/phy%d/ath10k/spectral_scan0", phy);

	if (access(S.ctl_path, W_OK) != 0) {
		snprintf(err, errlen,
			 "%s missing: the ath10k module was built without "
			 "CONFIG_ATH10K_SPECTRAL", S.ctl_path);
		return -ENOENT;
	}

	if (bins) {
		snprintf(path, sizeof(path),
			 "/sys/kernel/debug/ieee80211/phy%d/ath10k/spectral_bins", phy);
		char val[16];

		snprintf(val, sizeof(val), "%d\n", bins);
		if (write_sysfs(path, val) != 0) {
			snprintf(err, errlen, "cannot set spectral_bins=%d: %s",
				 bins, strerror(errno));
			return -errno;
		}
	}

	if (write_sysfs(S.ctl_path, "background\n") != 0) {
		snprintf(err, errlen, "cannot enter background mode: %s",
			 strerror(errno));
		return -errno;
	}

	if (write_sysfs(S.ctl_path, "trigger\n") != 0) {
		snprintf(err, errlen, "cannot trigger spectral scan: %s",
			 strerror(errno));
		write_sysfs(S.ctl_path, "disable\n");
		return -errno;
	}

	S.fd = open(S.scan_path, O_RDONLY | O_NONBLOCK);
	if (S.fd < 0) {
		snprintf(err, errlen, "cannot open %s: %s",
			 S.scan_path, strerror(errno));
		write_sysfs(S.ctl_path, "disable\n");
		return -errno;
	}

	/* the relay ring may hold minutes of pre-trigger samples; drop them */
	drain_relay(S.fd);

	S.running = true;
	S.phy = phy;
	S.mode = sweep ? MODE_SWEEP : MODE_CHANNEL;
	S.error[0] = 0;
	S.samples = 0;
	S.rate = 0;
	S.last_samples = 0;
	S.start_ms = now_ms();
	S.last_get_ms = S.start_ms;
	S.epoch++;
	S.pbhave = 0;
	S.freq = 0;
	S.width = 0;
	S.cols = 0;
	S.scan_pid = -1;
	S.last_scan_ms = 0;
	S.freq_idx = 0;
	S.sweep_cycles = 0;

	if (sweep) {
		setup_grid();
		reset_rows();
		/* kick the first scan right away */
		S.scan_freq = S.freqs[0];
		S.row_ms = SWEEP_ROW_MS;
		S.row_start_ms = now_ms();
	} else {
		S.cols = bins ? bins : 0;
		S.row_ms = CHANNEL_ROW_MS;
		reset_rows();
	}

	fprintf(stderr, "spectrald: %s mode phy%d (bins %d%s)\n",
		mode_name(), phy, bins,
		sweep ? ", sweep enabled" : "");
	return 0;
}

static void flush_row(void)
{
	struct row *r;

	if (!S.running || S.row_samples == 0 || S.cols == 0)
		return;

	r = &S.rows[S.ring_head];
	r->cols = S.cols;
	for (int i = 0; i < S.cols && i < MAX_BINS; i++)
		r->data[i] = (uint8_t)(S.row_max[i] + DB_OFFSET);

	S.ring_head = (S.ring_head + 1) % MAX_ROWS;
	if (S.ring_count < MAX_ROWS)
		S.ring_count++;
	S.row_seq++;

	S.row_samples = 0;
	for (int i = 0; i < MAX_BINS; i++)
		S.row_max[i] = INT16_MIN;
}

static void flush_sweep_row(void)
{
	struct row *r;

	if (!S.running || S.mode != MODE_SWEEP || S.cols == 0)
		return;

	r = &S.rows[S.ring_head];
	r->cols = S.cols;
	for (int i = 0; i < S.cols; i++) {
		int v = S.pan_max[i];

		if (v < -128)
			v = -128;
		r->data[i] = (uint8_t)(v + DB_OFFSET);
		S.pan_max[i] = (int16_t)(v > -128 + SWEEP_DECAY_DB ?
					 v - SWEEP_DECAY_DB : -128);
	}

	S.ring_head = (S.ring_head + 1) % MAX_ROWS;
	if (S.ring_count < MAX_ROWS)
		S.ring_count++;
	S.row_seq++;
}

static void init_db_lut(void)
{
	for (int exp = 0; exp < 8; exp++)
		for (int bin = 0; bin < 256; bin++) {
			uint32_t x = (uint32_t)bin << exp;

			db_lut[exp][bin] = x
				? (int16_t)lround(20.0 * log10((double)x))
				: 0;
		}
}

static void drain_relay(int fd)
{
	uint8_t tmp[4096];

	for (;;) {
		ssize_t n = read(fd, tmp, sizeof(tmp));

		if (n > 0)
			continue;
		if (n < 0 && errno == EINTR)
			continue;
		break;
	}
}

static uint16_t be16(const uint8_t *p)
{
	return (uint16_t)((p[0] << 8) | p[1]);
}

static void handle_fft_sweep(const uint8_t *bp, int bins, int cw, int freq,
			     int base, int norm, int max_exp)
{
	double start = (double)freq - (double)cw / 2.0;
	double bw = (double)cw / (double)bins;

	for (int i = 0; i < bins; i++) {
		int d = base + db_lut[max_exp][bp[i]] - norm;
		double f = start + bw * (i + 0.5);
		int gi = (int)lround((f - (double)S.grid_lo) / (double)S.grid_step);

		if (gi < 0 || gi >= S.cols)
			continue;
		if (d < -128)
			d = -128;
		if (d > 0)
			d = 0;
		if (d > S.pan_max[gi])
			S.pan_max[gi] = (int16_t)d;
	}
}

static void handle_fft(const uint8_t *p, int len)
{
	int bins = len - 26;
	uint8_t cw;
	int freq;
	int16_t noise;
	uint8_t rssi, max_exp;
	const uint8_t *bp;
	uint64_t sumsq = 0;
	int norm, base;

	if (len < 26)
		return;
	if (bins != 64 && bins != 128 && bins != 256)
		return;

	cw = p[0];
	freq = be16(p + 1);
	noise = (int16_t)be16(p + 5);
	rssi = p[22];
	max_exp = p[25];
	bp = p + 26;

	/* discard frames with invalid calibration fields (CT firmware emits
	 * rssi=255 sentinel frames that would otherwise show up as +150 dBm) */
	if (noise >= 0 || rssi >= 128 || max_exp > 7)
		return;

	for (int i = 0; i < bins; i++) {
		uint64_t x = (uint64_t)bp[i] << max_exp;

		sumsq += x * x;
	}
	if (!sumsq)
		return;

	norm = (int)lround(10.0 * log10((double)sumsq));
	base = (int)noise + (int)rssi;

	if (S.mode == MODE_SWEEP) {
		handle_fft_sweep(bp, bins, cw, freq, base, norm, max_exp);
		S.samples++;
		return;
	}

	/* channel mode: channel width / bins / centre changed -> fresh ring */
	if (S.cols != bins || S.freq != freq || S.width != cw) {
		S.cols = bins;
		S.freq = freq;
		S.width = cw;
		S.epoch++;
		reset_rows();
	}

	for (int i = 0; i < bins; i++) {
		int d = base + db_lut[max_exp][bp[i]] - norm;

		if (d < -128)
			d = -128;
		if (d > 0)
			d = 0;
		if (d > S.row_max[i])
			S.row_max[i] = (int16_t)d;
	}

	S.row_samples++;
	S.samples++;
}

static void parse_buf(void)
{
	size_t off = 0;

	while (S.pbhave - off >= 3) {
		uint8_t type = S.pbuf[off];
		uint16_t len = (uint16_t)((S.pbuf[off + 1] << 8) | S.pbuf[off + 2]);

		if (len > sizeof(S.pbuf) - 3) {
			/* garbage: resynchronise by dropping everything */
			off = S.pbhave;
			break;
		}
		if (S.pbhave - off - 3 < len)
			break;			/* partial record */

		if (type == 3 /* ATH_FFT_SAMPLE_ATH10K */)
			handle_fft(S.pbuf + off + 3, len);

		off += 3 + len;
	}

	if (off) {
		memmove(S.pbuf, S.pbuf + off, S.pbhave - off);
		S.pbhave -= off;
	}
}

static void read_cb(struct uloop_timeout *t)
{
	if (S.running && S.fd >= 0) {
		for (;;) {
			ssize_t n;

			if (S.pbhave >= sizeof(S.pbuf))
				S.pbhave = 0;	/* garbage, resync */

			n = read(S.fd, S.pbuf + S.pbhave,
				 sizeof(S.pbuf) - S.pbhave);

			if (n > 0) {
				S.pbhave += (size_t)n;
				parse_buf();
			} else if (n == 0) {
				break;
			} else if (errno == EAGAIN || errno == EWOULDBLOCK) {
				break;
			} else if (errno == EINTR) {
				continue;
			} else {
				set_error("spectral_scan0 read failed: %s",
					  strerror(errno));
				stop_scan();
				break;
			}
		}

		if (S.running && now_ms() - S.row_start_ms >= (uint64_t)S.row_ms) {
			if (S.mode == MODE_SWEEP)
				flush_sweep_row();
			else if (S.row_samples > 0)
				flush_row();
			S.row_start_ms = now_ms();
		}
	}

	uloop_timeout_set(t, READ_INTERVAL_MS);
}

static void scan_cb(struct uloop_timeout *t)
{
	if (!S.running || S.mode != MODE_SWEEP) {
		uloop_timeout_set(t, SCAN_INTERVAL_MS);
		return;
	}

	if (S.scan_pid > 0) {
		pid_t r = waitpid(S.scan_pid, NULL, WNOHANG);

		if (r == 0) {
			/* still scanning */
			uloop_timeout_set(t, SCAN_INTERVAL_MS);
			return;
		}
		S.scan_pid = -1;
	}

	if (now_ms() - S.last_scan_ms < SWEEP_DWELL_MS) {
		uloop_timeout_set(t, SCAN_INTERVAL_MS);
		return;
	}

	S.last_scan_ms = now_ms();
	S.scan_freq = S.freqs[S.freq_idx];

	{
		pid_t pid = fork();
		char fbuf[16];

		if (pid == 0) {
			int dn = open("/dev/null", O_WRONLY);

			if (dn >= 0) {
				dup2(dn, 1);
				dup2(dn, 2);
				if (dn > 2)
					close(dn);
			}
			snprintf(fbuf, sizeof(fbuf), "%d", S.scan_freq);
			execl(IW_PATH, "iw", "dev", S.ifname, "scan",
			      "freq", fbuf, (char *)NULL);
			_exit(127);
		}
		if (pid > 0)
			S.scan_pid = pid;
	}

	S.freq_idx++;
	if (S.freq_idx >= S.nfreqs) {
		S.freq_idx = 0;
		S.sweep_cycles++;
	}

	uloop_timeout_set(t, SCAN_INTERVAL_MS);
}

static void second_cb(struct uloop_timeout *t)
{
	S.rate = S.samples - S.last_samples;
	S.last_samples = S.samples;

	if (S.running && now_ms() - S.last_get_ms > AUTO_STOP_MS) {
		fprintf(stderr, "spectrald: no client for %d s, stopping\n",
			AUTO_STOP_MS / 1000);
		stop_scan();
	}

	uloop_timeout_set(t, 1000);
}

/* ------------------------------------------------------------------ */
/* ubus                                                               */

enum {
	START_PHY,
	START_BINS,
	START_MODE,
	START_PRESET,
	START_FREQS,
	__START_MAX
};

static const struct blobmsg_policy start_policy[__START_MAX] = {
	[START_PHY] = { .name = "phy", .type = BLOBMSG_TYPE_INT32 },
	[START_BINS] = { .name = "bins", .type = BLOBMSG_TYPE_INT32 },
	[START_MODE] = { .name = "mode", .type = BLOBMSG_TYPE_STRING },
	[START_PRESET] = { .name = "preset", .type = BLOBMSG_TYPE_STRING },
	[START_FREQS] = { .name = "freqs", .type = BLOBMSG_TYPE_STRING },
};

enum {
	GET_CURSOR,
	__GET_MAX
};

static const struct blobmsg_policy get_policy[__GET_MAX] = {
	[GET_CURSOR] = { .name = "cursor", .type = BLOBMSG_TYPE_INT32 },
};

static void reply_meta(struct blob_buf *b)
{
	blobmsg_add_string(b, "mode", mode_name());
	blobmsg_add_u32(b, "epoch", (uint32_t)S.epoch);
	blobmsg_add_u32(b, "cols", (uint32_t)S.cols);
	blobmsg_add_u32(b, "freq", (uint32_t)S.freq);
	blobmsg_add_u32(b, "width", (uint32_t)S.width);
	blobmsg_add_u32(b, "row_ms", (uint32_t)S.row_ms);
}

static void reply_sweep(struct blob_buf *b)
{
	blobmsg_add_u32(b, "sweep_n", (uint32_t)S.nfreqs);
	blobmsg_add_u32(b, "sweep_idx", (uint32_t)S.freq_idx);
	blobmsg_add_u32(b, "sweep_freq", (uint32_t)S.scan_freq);
	blobmsg_add_u32(b, "sweep_cycles", (uint32_t)S.sweep_cycles);
}

static int spectral_status(struct ubus_context *ctx, struct ubus_object *obj,
			   struct ubus_request_data *req, const char *method,
			   struct blob_attr *msg)
{
	struct blob_buf b = { 0 };

	blob_buf_init(&b, 0);
	blobmsg_add_u8(&b, "ok", true);
	blobmsg_add_u8(&b, "running", S.running);
	blobmsg_add_u32(&b, "phy", (uint32_t)S.phy);
	blobmsg_add_u32(&b, "samples", (uint32_t)S.samples);
	blobmsg_add_u32(&b, "rate", (uint32_t)S.rate);
	blobmsg_add_u32(&b, "rows", (uint32_t)S.row_seq);
	blobmsg_add_u32(&b, "uptime",
			S.running ? (uint32_t)((now_ms() - S.start_ms) / 1000) : 0);
	reply_meta(&b);
	if (S.mode == MODE_SWEEP && S.running)
		reply_sweep(&b);
	if (S.error[0])
		blobmsg_add_string(&b, "error", S.error);

	ubus_send_reply(ctx, req, b.head);
	blob_buf_free(&b);
	return UBUS_STATUS_OK;
}

static int spectral_start(struct ubus_context *ctx, struct ubus_object *obj,
			  struct ubus_request_data *req, const char *method,
			  struct blob_attr *msg)
{
	struct blob_attr *tb[__START_MAX] = {};
	struct blob_buf b = { 0 };
	char err[160] = "";
	int phy = 0, bins = 0, ret;
	const char *mode = NULL, *preset = NULL, *freqs = NULL;

	if (msg)
		blobmsg_parse(start_policy, __START_MAX, tb,
			      blob_data(msg), blob_len(msg));

	if (tb[START_PHY])
		phy = (int)blobmsg_get_u32(tb[START_PHY]);
	if (tb[START_BINS])
		bins = (int)blobmsg_get_u32(tb[START_BINS]);
	if (tb[START_MODE])
		mode = blobmsg_get_string(tb[START_MODE]);
	if (tb[START_PRESET])
		preset = blobmsg_get_string(tb[START_PRESET]);
	if (tb[START_FREQS])
		freqs = blobmsg_get_string(tb[START_FREQS]);

	ret = start_scan(phy, bins, mode, preset, freqs, err, sizeof(err));

	blob_buf_init(&b, 0);
	blobmsg_add_u8(&b, "ok", ret == 0);
	blobmsg_add_u32(&b, "phy", (uint32_t)phy);
	blobmsg_add_string(&b, "mode", mode && !strcmp(mode, "sweep") ?
			  "sweep" : "channel");
	if (ret != 0)
		blobmsg_add_string(&b, "error", err);
	ubus_send_reply(ctx, req, b.head);
	blob_buf_free(&b);
	return UBUS_STATUS_OK;
}

static int spectral_stop(struct ubus_context *ctx, struct ubus_object *obj,
			 struct ubus_request_data *req, const char *method,
			 struct blob_attr *msg)
{
	struct blob_buf b = { 0 };

	stop_scan();

	blob_buf_init(&b, 0);
	blobmsg_add_u8(&b, "ok", true);
	ubus_send_reply(ctx, req, b.head);
	blob_buf_free(&b);
	return UBUS_STATUS_OK;
}

static int spectral_get(struct ubus_context *ctx, struct ubus_object *obj,
			struct ubus_request_data *req, const char *method,
			struct blob_attr *msg)
{
	struct blob_attr *tb[__GET_MAX] = {};
	struct blob_buf b = { 0 };
	uint32_t cursor = 0;
	uint64_t oldest, first, last, n;
	bool reset;

	if (msg)
		blobmsg_parse(get_policy, __GET_MAX, tb,
			      blob_data(msg), blob_len(msg));
	if (tb[GET_CURSOR])
		cursor = blobmsg_get_u32(tb[GET_CURSOR]);

	S.last_get_ms = now_ms();

	oldest = (S.row_seq > (uint64_t)S.ring_count) ?
		 S.row_seq - (uint64_t)S.ring_count + 1 : 1;
	reset = (cursor + 1 < oldest) || (cursor > S.row_seq);
	first = reset ? oldest : (uint64_t)cursor + 1;
	n = (S.row_seq >= first) ? (S.row_seq - first + 1) : 0;

	if (n > MAX_REPLY_ROWS) {
		first = S.row_seq - MAX_REPLY_ROWS + 1;
		n = MAX_REPLY_ROWS;
		reset = true;
	}
	last = (n > 0) ? first + n - 1 : (uint64_t)cursor;

	blob_buf_init(&b, 0);
	blobmsg_add_u8(&b, "ok", true);
	blobmsg_add_u8(&b, "reset", reset);
	blobmsg_add_u32(&b, "running", (uint32_t)S.running);
	blobmsg_add_u32(&b, "phy", (uint32_t)S.phy);
	blobmsg_add_u32(&b, "samples", (uint32_t)S.samples);
	blobmsg_add_u32(&b, "rate", (uint32_t)S.rate);
	blobmsg_add_u32(&b, "cursor", (uint32_t)last);
	reply_meta(&b);
	if (S.mode == MODE_SWEEP && S.running)
		reply_sweep(&b);
	blobmsg_add_u32(&b, "rows", (uint32_t)n);

	if (n > 0) {
		void *arr = blobmsg_open_array(&b, "data");

		for (uint64_t s = first; s <= last; s++) {
			const struct row *r = &S.rows[(s - 1) % MAX_ROWS];
			int cols = r->cols ? r->cols : S.cols;

			for (int i = 0; i < cols && i < MAX_COLS; i++)
				blobmsg_add_u16(&b, NULL, r->data[i]);
		}
		blobmsg_close_array(&b, arr);
	}

	ubus_send_reply(ctx, req, b.head);
	blob_buf_free(&b);
	return UBUS_STATUS_OK;
}

static const struct ubus_method spectral_methods[] = {
	UBUS_METHOD_NOARG("status", spectral_status),
	UBUS_METHOD("start", spectral_start, start_policy),
	UBUS_METHOD_NOARG("stop", spectral_stop),
	UBUS_METHOD("get", spectral_get, get_policy),
};

static struct ubus_object_type spectral_obj_type =
	UBUS_OBJECT_TYPE("spectral", spectral_methods);

static struct ubus_object spectral_obj = {
	.name = "spectral",
	.type = &spectral_obj_type,
	.methods = spectral_methods,
	.n_methods = ARRAY_SIZE(spectral_methods),
};

/* ------------------------------------------------------------------ */

static void sig_handler(int sig)
{
	uloop_end();
}

int main(int argc, char **argv)
{
	struct sigaction sa = {};

	(void)argc;
	(void)argv;

	uloop_init();
	init_db_lut();

	S.ctx = ubus_connect(NULL);
	if (!S.ctx) {
		fprintf(stderr, "spectrald: cannot connect to ubus\n");
		return 1;
	}
	ubus_add_uloop(S.ctx);
	if (ubus_add_object(S.ctx, &spectral_obj) != 0) {
		fprintf(stderr, "spectrald: cannot register ubus object\n");
		ubus_free(S.ctx);
		return 1;
	}

	reset_rows();

	S.read_timer.cb = read_cb;
	uloop_timeout_set(&S.read_timer, READ_INTERVAL_MS);

	S.second_timer.cb = second_cb;
	uloop_timeout_set(&S.second_timer, 1000);

	S.scan_timer.cb = scan_cb;
	uloop_timeout_set(&S.scan_timer, SCAN_INTERVAL_MS);

	sa.sa_handler = sig_handler;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);

	fprintf(stderr, "spectrald: ready\n");
	uloop_run();

	stop_scan();
	ubus_free(S.ctx);
	uloop_done();
	return 0;
}
