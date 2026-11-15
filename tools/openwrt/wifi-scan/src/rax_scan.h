/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef RAX_SCAN_H
#define RAX_SCAN_H

#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define RAX_SCAN_MAX 100
#define RAX_SCAN_RECORD 136
#define RAX_SCAN_TTL 20

struct rax_neighbor {
	uint8_t mac[6];
	char ssid[33];
	int channel, signal, width;
};

/* No vendor library: the same ARM32 neighbor-cache ABI as the stock getter. */
static inline int rax_scan_read(const char *iface, uint8_t *records)
{
	struct { uint32_t command; uint8_t count, reserved[3]; void *records; }
		query = {0x8ef5, RAX_SCAN_MAX, {0}, records};
	struct ifreq req = {0};
	int fd, rc, saved_errno;
	if (sizeof(void *) != 4 || sizeof(query) != 12) { errno = ENOTSUP; return -1; }
	strncpy(req.ifr_name, iface, IFNAMSIZ - 1);
	req.ifr_data = (void *)&query;
	fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0) return -1;
	memset(records, 0, RAX_SCAN_MAX * RAX_SCAN_RECORD);
	rc = ioctl(fd, 0x89f0, &req);
	saved_errno = errno;
	close(fd);
	errno = saved_errno;
	return rc;
}

static inline int rax_scan_parse(const uint8_t *records, int band5,
	struct rax_neighbor *out)
{
	int i, j, count = 0;
	for (i = 0; i < RAX_SCAN_MAX; i++) {
		const uint8_t *r = records + i * RAX_SCAN_RECORD;
		int channel = r[0x25], signal = (int8_t)r[0xc];
		unsigned int len = r[0x4c] | r[0x4d] << 8 | r[0x4e] << 16 | (uint32_t)r[0x4f] << 24;
		if (!memcmp(r + 0x12, "\0\0\0\0\0\0", 6) || (r[0x12] & 1) ||
		    signal >= 0 || signal < -127 || channel < 1 || channel > 196 ||
		    (channel > 14) != band5) continue;
		for (j = 0; j < count; j++) if (!memcmp(out[j].mac, r + 0x12, 6)) break;
		if (j != count) continue;
		memset(out + count, 0, sizeof(*out));
		memcpy(out[count].mac, r + 0x12, 6);
		memcpy(out[count].ssid, r + 0x29, len > 32 ? 32 : len);
		out[count].channel = channel; out[count].signal = signal;
		/* Firmware width class; not enough information to describe 80+80. */
		out[count].width = r[0x26] <= (band5 ? 3 : 1) ? 20 << r[0x26] : 20;
		count++;
	}
	return count;
}

/* Run the already-tested private command, rather than guess its WEXT set ABI. */
static inline int rax_scan_command(const char *iface, int start)
{
	int pipes[2], status, length = 0;
	pid_t pid;
	char reply[256], *colon;
	if (pipe(pipes)) return -1;
	pid = fork();
	if (!pid) {
		dup2(pipes[1], STDOUT_FILENO); dup2(pipes[1], STDERR_FILENO);
		close(pipes[0]); close(pipes[1]);
		if (start) execl("/bin/iwpriv", "iwpriv", iface, "start_ap_scan", "1", (char *)NULL);
		else execl("/bin/iwpriv", "iwpriv", iface, "get_ap_scan_st", (char *)NULL);
		_exit(127);
	}
	close(pipes[1]);
	if (pid < 0) { close(pipes[0]); return -1; }
	while (length < (int)sizeof(reply) - 1) {
		int n = read(pipes[0], reply + length, sizeof(reply) - 1 - length);
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) break;
		length += n;
	}
	close(pipes[0]); reply[length] = 0;
	while (waitpid(pid, &status, 0) < 0) if (errno != EINTR) return -1;
	if (!WIFEXITED(status) || WEXITSTATUS(status)) return -1;
	if (start) return 0;
	colon = strchr(reply, ':');
	if (!colon || (colon[1] != '0' && colon[1] != '1')) return -1;
	return colon[1] - '0';
}

static inline int rax_scan_cached(const char *path, uint8_t *records, int fresh)
{
	struct stat st;
	int fd = open(path, O_RDONLY | O_CLOEXEC), rc = -1;
	if (fd < 0) return -1;
	if (!fstat(fd, &st) && st.st_size == RAX_SCAN_MAX * RAX_SCAN_RECORD &&
	    (!fresh || (time(NULL) >= st.st_mtime && time(NULL) - st.st_mtime < RAX_SCAN_TTL)) &&
	    read(fd, records, RAX_SCAN_MAX * RAX_SCAN_RECORD) == RAX_SCAN_MAX * RAX_SCAN_RECORD) rc = 0;
	close(fd);
	return rc;
}

static inline int rax_scan(const char *iface, struct rax_neighbor *out, int force, FILE *trace)
{
	uint8_t records[RAX_SCAN_MAX * RAX_SCAN_RECORD];
	char cache[96], temporary[100];
	int band5, fd, setupfd = -1, cachefd, rc = -1, i, state;
	struct timespec pause = {0, 250000000};
	struct timespec read_start, read_end;
	if (strncmp(iface, "vap", 3) || !iface[3] || iface[3 + strspn(iface + 3, "0123456789")] || atoi(iface + 3) > 15)
		return -1;
	band5 = atoi(iface + 3) >= 8;
	snprintf(cache, sizeof(cache), "/var/run/rax-wifi/scan-%dg.cache", band5 ? 5 : 2);
	snprintf(temporary, sizeof(temporary), "%s.new", cache);
	if (!force && !rax_scan_cached(cache, records, 1)) return rax_scan_parse(records, band5, out);
	mkdir("/var/run/rax-wifi", 0700);
	/* Startup callers already hold the exclusive setup lock in netifd.
	 * UI scans may use cached data, but must not scan during AP startup. */
	if (!force) {
		setupfd = open("/var/run/rax-wifi/setup.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0600);
		if (setupfd < 0) return -1;
		if (flock(setupfd, LOCK_SH | LOCK_NB)) {
			close(setupfd);
			if (!rax_scan_cached(cache, records, 0)) return rax_scan_parse(records, band5, out);
			return -1;
		}
	}
	/* Both radios share the vendor scan engine/state. Keep the lock until
	 * the requested band's results have been read, not merely until start. */
	fd = open("/var/run/rax-wifi/scan.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0600);
	if (fd < 0) { if (setupfd >= 0) close(setupfd); return -1; }
	for (i = 0; flock(fd, LOCK_EX | LOCK_NB); i++) {
		if (!force || i >= 48 || (errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR)) {
			close(fd);
			if (setupfd >= 0) close(setupfd);
			if (!force && !rax_scan_cached(cache, records, 0)) return rax_scan_parse(records, band5, out);
			errno = EBUSY; return -1;
		}
		if (!i && trace) fprintf(trace, "waiting for shared scan engine\n");
		nanosleep(&pause, NULL);
	}
	if (trace) fprintf(trace, "shared scan engine acquired for %s after %d ms\n", iface, i * 250);
	if (!force && !rax_scan_cached(cache, records, 1)) goto parsed;
	/* Drain any scan started outside this helper, then request our own band.
	 * An already-active scan may belong to the other radio. */
	state = rax_scan_command(iface, 0);
	if (trace) fprintf(trace, "pre-AP scan initial state=%d\n", state);
	for (i = 0; state > 0 && i < 32; i++) {
		nanosleep(&pause, NULL);
		state = rax_scan_command(iface, 0);
	}
	if (state < 0) goto done;
	if (state) { errno = ETIMEDOUT; goto done; }
	if (trace) fprintf(trace, "starting dedicated scan for %s\n", iface);
	if (rax_scan_command(iface, 1)) goto done;
	for (i = 0; i < 32; i++) {
		nanosleep(&pause, NULL);
		state = rax_scan_command(iface, 0);
		if (state < 0) goto done;
		if (!state) break;
	}
	if (trace) fprintf(trace, "scan completion state=%d after %d ms\n", state, (i + 1) * 250);
	if (state) { errno = ETIMEDOUT; goto done; }
	clock_gettime(CLOCK_MONOTONIC, &read_start);
	state = rax_scan_read(iface, records);
	i = errno;
	clock_gettime(CLOCK_MONOTONIC, &read_end);
	if (trace) fprintf(trace, "scan result read: rc=%d errno=%d (%s), elapsed=%ld ms\n",
		state, state < 0 ? i : 0, state < 0 ? strerror(i) : "none",
		(long)((read_end.tv_sec - read_start.tv_sec) * 1000 +
		(read_end.tv_nsec - read_start.tv_nsec) / 1000000));
	if (state) goto done;
	/* Publish atomically: concurrent callers can keep using the previous cache. */
	cachefd = open(temporary, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600);
	if (cachefd >= 0) {
		if (write(cachefd, records, sizeof(records)) == sizeof(records)) rename(temporary, cache);
		close(cachefd);
		unlink(temporary);
	}
parsed:
	rc = rax_scan_parse(records, band5, out);
done:
	close(fd);
	if (setupfd >= 0) close(setupfd);
	return rc;
}

/* Contiguous 5 GHz channel groups. Return the first 20 MHz primary. */
static inline int rax_group(int channel, int width)
{
	int base = channel >= 149 ? 149 : channel >= 100 ? 100 : 36;
	int span = width / 5;
	int first = base + (channel - base) / span * span;
	int end = base == 149 ? 165 : base == 100 ? 144 : 64;
	return channel >= base && (channel - base) % 4 == 0 && first + span - 4 <= end ? first : channel;
}

static inline void rax_neighbor_span(const struct rax_neighbor *n, int *low, int *high)
{
	int center = n->channel == 14 ? 2484 : n->channel <= 13 ? 2407 + n->channel * 5 : 5000 + n->channel * 5;
	*low = center - 10; *high = center + 10;
	if (n->channel > 14) {
		int first = rax_group(n->channel, n->width);
		*low = 5000 + first * 5 - 10; *high = *low + n->width;
	} else if (n->width == 40) {
		/* Secondary direction is absent: cover both possibilities conservatively. */
		if (n->channel <= 9) *high += 20;
		if (n->channel >= 5) *low -= 20;
	}
}

static inline uint64_t rax_channel_score(const struct rax_neighbor *neighbors,
	int count, int channel, int width)
{
	uint64_t score = 0;
	int low, high, i;
	if (channel <= 14) {
		low = 2407 + channel * 5 - 10;
		if (width == 40 && channel > 7) low -= 20;
	} else low = 5000 + rax_group(channel, width) * 5 - 10;
	high = low + width;
	for (i = 0; i < count; i++) {
		int nl, nh, overlap, dbm = neighbors[i].signal, weight = 1;
		rax_neighbor_span(neighbors + i, &nl, &nh);
		overlap = (high < nh ? high : nh) - (low > nl ? low : nl);
		if (overlap <= 0) continue;
		/* 10 dB buckets approximate received power; no floating-point model. */
		while (dbm >= -90 && weight < 100000000) { weight *= 10; dbm -= 10; }
		score += (uint64_t)weight * overlap * 100 / (nh - nl);
	}
	return score;
}

#endif
