/* SPDX-License-Identifier: GPL-2.0-only
 * Hostapd-backed iwinfo for HiSilicon's retained vendor cfg80211 driver.
 * Never issue GET_STATION dumps: this driver does not terminate enumeration.
 * Implements the standard iwinfo backend interface; no private RPC API.
 */
#include "iwinfo.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <poll.h>
#include <time.h>
#include <uci.h>
#include <linux/wireless.h>
#include "rax_scan.h"

static int rax_radio(const char *name)
{
	struct uci_context *ctx;
	struct uci_package *pkg;
	struct uci_section *s;
	const char *type, *band;
	int radio = -1;
	if (!strcmp(name, "Hisilicon0") || !strcmp(name, "phy0")) return 0;
	if (!strncmp(name, "vap", 3) && name[3] && !name[3 + strspn(name + 3, "0123456789")]) {
		int n = atoi(name + 3);
		return n < 16 ? (n >= 8) : -1;
	}
	ctx = uci_alloc_context();
	if (!ctx) return -1;
	if (!uci_load(ctx, "wireless", &pkg)) {
		s = uci_lookup_section(ctx, pkg, name);
		if (s && !strcmp(s->type, "wifi-device")) {
			type = uci_lookup_option_string(ctx, s, "type");
			band = uci_lookup_option_string(ctx, s, "band");
			if (type && !strcmp(type, "rax3000h") && band)
				radio = !strcmp(band, "2g") ? 0 : !strcmp(band, "5g") ? 1 : -1;
		}
	}
	uci_free_context(ctx);
	return radio;
}

static const char *rax_iface(const char *name)
{
	return !strncmp(name, "vap", 3) ? name : rax_radio(name) == 1 ? "vap8" : "vap0";
}

static int rax_request(const char *name, const char *cmd, char *buf, size_t size)
{
	struct sockaddr_un local = { .sun_family = AF_UNIX }, peer = { .sun_family = AF_UNIX };
	struct pollfd pfd;
	static unsigned int seq;
	int fd, len, rc = -1;
	fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0) return -1;
	/* Abstract local socket avoids files and works in read-only rpcd callers. */
	snprintf(local.sun_path + 1, sizeof(local.sun_path) - 1, "rax-iwinfo-%d-%u", getpid(), seq++);
	snprintf(peer.sun_path, sizeof(peer.sun_path), "/var/run/hostapd/%s", rax_iface(name));
	if (bind(fd, (struct sockaddr *)&local, sizeof(local)) ||
	    connect(fd, (struct sockaddr *)&peer, sizeof(peer)) ||
	    send(fd, cmd, strlen(cmd), 0) < 0) goto out;
	pfd.fd = fd; pfd.events = POLLIN;
	if (poll(&pfd, 1, 250) <= 0) goto out;
	len = recv(fd, buf, size - 1, 0);
	if (len < 0) goto out;
	buf[len] = 0;
	if (!strncmp(buf, "FAIL", 4) || !strncmp(buf, "UNKNOWN COMMAND", 15)) goto out;
	rc = 0;
out:
	close(fd);
	return rc;
}

static int rax_value(const char *text, const char *key, char *out, size_t size)
{
	size_t k = strlen(key), len;
	while (*text) {
		const char *end = strchr(text, '\n');
		if (!end) end = text + strlen(text);
		if ((size_t)(end - text) > k && !strncmp(text, key, k) && text[k] == '=') {
			len = end - text - k - 1;
			if (len >= size) len = size - 1;
			memcpy(out, text + k + 1, len); out[len] = 0;
			return 0;
		}
		text = *end ? end + 1 : end;
	}
	return -1;
}

static int rax_status(const char *name, const char *key, char *out, size_t size)
{
	static char cached_name[IFNAMSIZ], cached[8192];
	static struct timespec stamp;
	struct timespec now;
	const char *iface = rax_iface(name);
	clock_gettime(CLOCK_MONOTONIC, &now);
	if (strcmp(cached_name, iface) || now.tv_sec != stamp.tv_sec ||
	    now.tv_nsec - stamp.tv_nsec > 100000000 || now.tv_nsec < stamp.tv_nsec) {
		cached_name[0] = 0;
		if (rax_request(name, "STATUS", cached, sizeof(cached))) return -1;
		strncpy(cached_name, iface, sizeof(cached_name) - 1);
		stamp = now;
	}
	return rax_value(cached, key, out, size);
}

static int rax_num(const char *name, const char *key, int *out)
{
	char b[64];
	if (rax_status(name, key, b, sizeof(b))) return -1;
	*out = atoi(b); return 0;
}
static int rax_probe(const char *name) { return !access("/usr/share/rax3000h/vendor-modules.list", F_OK) && rax_radio(name) >= 0; }
static int rax_unavailable(const char *name, int *out) { return -1; }
static int rax_zero(const char *name, int *out) { *out = 0; return 0; }
static int rax_vaps(const char *name, int *out) { *out = 1; return 0; }
static int rax_empty(const char *name, char *out, int *len) { *len = 0; return -1; }
static int rax_mode(const char *name, int *out)
{
	char b[32];
	if (rax_status(name, "state", b, sizeof(b)) || strcmp(b, "ENABLED")) return -1;
	*out = IWINFO_OPMODE_MASTER; return 0;
}
static int rax_channel(const char *n, int *v) { return rax_num(n, "channel", v); }
static int rax_frequency(const char *n, int *v) { return rax_num(n, "freq", v); }
static int rax_ssid(const char *n, char *v) { return rax_status(n, "ssid[0]", v, IWINFO_ESSID_MAX_SIZE + 1); }
static int rax_bssid(const char *n, char *v) { return rax_status(n, "bssid[0]", v, 18); }
/* Read-only vendor WEXT ABI. No vendor userspace library is required. */
static int rax_ioctl(const char *name, int cmd, struct iwreq *req)
{
	int fd, rc;
	strncpy(req->ifr_name, rax_iface(name), IFNAMSIZ - 1);
	fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0) return -1;
	rc = ioctl(fd, cmd, req);
	close(fd);
	return rc;
}
static int rax_param(const char *name, int param, int *out)
{
	struct iwreq req = {0};
	memcpy(req.u.name, &param, sizeof(param));
	if (rax_ioctl(name, 0x8be1, &req)) return -1;
	memcpy(out, req.u.name, sizeof(*out));
	return 0;
}
static int rax_txpower(const char *n, int *v) { return rax_param(n, 0x71, v); }
static int rax_noise(const char *n, int *v)
{
	if (rax_param(n, 0x2a4, v) || *v >= 0 || *v < -127) return -1;
	return 0;
}
static int rax_country(const char *n, char *v)
{
	struct iwreq req = {0};
	char country[4] = {0};
	req.u.data.pointer = country; req.u.data.length = 3;
	if (rax_ioctl(n, 0x8be9, &req) || !isupper(country[0]) || !isupper(country[1])) return -1;
	memcpy(v, country, 2); v[2] = 0;
	return 0;
}
static int rax_phyname(const char *n, char *v) { strcpy(v, "phy0"); return 0; }
static int rax_lookup(const char *n, char *v)
{
	if (rax_radio(n) < 0) return -1;
	strcpy(v, rax_iface(n)); return 0;
}
static int rax_hardware_id(const char *n, char *v) { return -1; }
static int rax_hardware_name(const char *n, char *v) { strcpy(v, "HiSilicon Hi5622"); return 0; }
static int rax_hwmodelist(const char *n, int *v)
{
	*v = IWINFO_80211_N | IWINFO_80211_AX |
	     (rax_radio(n) == 1 ? IWINFO_80211_A | IWINFO_80211_AC : IWINFO_80211_B | IWINFO_80211_G);
	return 0;
}
static int rax_htmodelist(const char *n, int *v)
{
	*v = IWINFO_HTMODE_NOHT | IWINFO_HTMODE_HT20 | IWINFO_HTMODE_HT40 | IWINFO_HTMODE_HE20 | IWINFO_HTMODE_HE40;
	if (rax_radio(n) == 1)
		*v |= IWINFO_HTMODE_VHT20 | IWINFO_HTMODE_VHT40 | IWINFO_HTMODE_VHT80 | IWINFO_HTMODE_VHT160 | IWINFO_HTMODE_HE80 | IWINFO_HTMODE_HE160;
	return 0;
}
static int rax_htmode(const char *n, int *v)
{
	int ax = 0, ac = 0, ht = 0, secondary = 0, width = 0;
	char mode[32] = {0};
	struct iwreq req = {0};
	req.u.data.pointer = mode; req.u.data.length = sizeof(mode) - 1;
	/* The driver may fall back to 20 MHz while hostapd still reports HT40. */
	if (!rax_ioctl(n, 0x8bf1, &req) && mode[0]) {
		ax = strstr(mode, "11ax") != NULL; ac = strstr(mode, "11ac") != NULL;
		ht = strstr(mode, "11n") != NULL;
		width = strstr(mode, "160") ? 160 : strstr(mode, "80") ? 80 : strstr(mode, "40") ? 40 : 20;
		*v = ax ? (width == 160 ? IWINFO_HTMODE_HE160 : width == 80 ? IWINFO_HTMODE_HE80 : width == 40 ? IWINFO_HTMODE_HE40 : IWINFO_HTMODE_HE20) :
		     ac ? (width == 160 ? IWINFO_HTMODE_VHT160 : width == 80 ? IWINFO_HTMODE_VHT80 : width == 40 ? IWINFO_HTMODE_VHT40 : IWINFO_HTMODE_VHT20) :
		     ht ? (width == 40 ? IWINFO_HTMODE_HT40 : IWINFO_HTMODE_HT20) : IWINFO_HTMODE_NOHT;
		return 0;
	}
	if (rax_num(n, "ieee80211n", &ht)) return -1;
	rax_num(n, "ieee80211ac", &ac); rax_num(n, "ieee80211ax", &ax);
	rax_num(n, "secondary_channel", &secondary);
	/* Configuration is not evidence of the channel width actually in use. */
	if ((ac || ax) && rax_num(n, ax ? "he_oper_chwidth" : "vht_oper_chwidth", &width))
		return -1;
	*v = ax ? (width == 2 ? IWINFO_HTMODE_HE160 : width == 1 ? IWINFO_HTMODE_HE80 : secondary ? IWINFO_HTMODE_HE40 : IWINFO_HTMODE_HE20) :
	     ac ? (width == 2 ? IWINFO_HTMODE_VHT160 : width == 1 ? IWINFO_HTMODE_VHT80 : secondary ? IWINFO_HTMODE_VHT40 : IWINFO_HTMODE_VHT20) :
	     ht ? (secondary ? IWINFO_HTMODE_HT40 : IWINFO_HTMODE_HT20) : IWINFO_HTMODE_NOHT;
	return 0;
}
static int rax_encryption(const char *n, char *v)
{
	char b[4096], key[128];
	struct iwinfo_crypto_entry *c = (void *)v;
	if (rax_request(n, "GET_CONFIG", b, sizeof(b))) return -1;
	memset(c, 0, sizeof(*c));
	if (!rax_value(b, "wpa", key, sizeof(key))) c->wpa_version = atoi(key);
	c->enabled = c->wpa_version != 0; c->auth_algs = IWINFO_AUTH_OPEN;
	if (c->enabled) {
		c->group_ciphers = c->pair_ciphers = IWINFO_CIPHER_CCMP;
		if (!rax_value(b, "key_mgmt", key, sizeof(key))) {
			if (strstr(key, "PSK")) c->auth_suites |= IWINFO_KMGMT_PSK;
			if (strstr(key, "SAE")) {
				c->auth_suites |= IWINFO_KMGMT_SAE;
				/* hostapd's wpa=2 means RSN, iwinfo uses bit 4 for WPA3. */
				c->wpa_version = (c->auth_suites & IWINFO_KMGMT_PSK) ? 6 : 4;
			}
		}
	}
	return 0;
}
static int rax_freqlist(const char *n, char *v, int *len)
{
	static const int five[] = {36,40,44,48,52,56,60,64,100,104,108,112,116,120,124,128,132,136,140,144,149,153,157,161,165};
	struct iwinfo_freqlist_entry *e = (void *)v;
	int i, count = rax_radio(n) == 1 ? sizeof(five)/sizeof(five[0]) : 13;
	memset(e, 0, count * sizeof(*e));
	for (i = 0; i < count; i++) {
		e[i].channel = rax_radio(n) == 1 ? five[i] : i + 1;
		e[i].band = rax_radio(n) == 1 ? IWINFO_BAND_5 : IWINFO_BAND_24;
		e[i].mhz = rax_radio(n) == 1 ? 5000 + e[i].channel * 5 : 2407 + e[i].channel * 5;
		e[i].flags = IWINFO_FREQ_NO_10MHZ;
		if (rax_radio(n) == 0) e[i].flags |= IWINFO_FREQ_NO_80MHZ | IWINFO_FREQ_NO_160MHZ;
		if (rax_radio(n) == 1 && !((five[i] >= 36 && five[i] <= 64) || (five[i] >= 100 && five[i] <= 128)))
			e[i].flags |= IWINFO_FREQ_NO_160MHZ;
		if (e[i].channel == 165) e[i].flags |= IWINFO_FREQ_NO_HT40PLUS | IWINFO_FREQ_NO_HT40MINUS | IWINFO_FREQ_NO_80MHZ | IWINFO_FREQ_NO_160MHZ;
	}
	*len = count * sizeof(*e); return 0;
}
static int rax_countrylist(const char *n, char *v, int *len)
{
	struct iwinfo_country_entry *e = (void *)v;
	memset(e, 0, sizeof(*e)); e->iso3166 = 0x434e; strcpy(e->ccode, "CN");
	*len = sizeof(*e); return 0;
}
static int rax_scanlist(const char *n, char *v, int *len)
{
	struct rax_neighbor neighbors[RAX_SCAN_MAX];
	struct iwinfo_scanlist_entry *entries = (void *)v;
	int count = rax_scan(rax_iface(n), neighbors, 0, NULL), i;
	*len = 0;
	if (count < 0) return -1;
	if (count > IWINFO_BUFSIZE / sizeof(*entries)) count = IWINFO_BUFSIZE / sizeof(*entries);
	memset(entries, 0, count * sizeof(*entries));
	for (i = 0; i < count; i++) {
		struct iwinfo_scanlist_entry *e = entries + i;
		const struct rax_neighbor *bss = neighbors + i;
		int first = rax_group(bss->channel, bss->width);
		memcpy(e->mac, bss->mac, 6); memcpy(e->ssid, bss->ssid, sizeof(e->ssid));
		e->mode = IWINFO_OPMODE_MASTER; e->channel = bss->channel;
		e->band = bss->channel <= 14 ? IWINFO_BAND_24 : IWINFO_BAND_5;
		e->mhz = bss->channel == 14 ? 2484 : bss->channel <= 13 ? 2407 + bss->channel * 5 : 5000 + bss->channel * 5;
		e->signal = (uint8_t)(int8_t)bss->signal;
		e->quality = bss->signal < -110 ? 0 : bss->signal > -40 ? 70 : bss->signal + 110;
		e->quality_max = 70;
		/* Security IEs are absent from this ABI. Do not advertise a joinable open AP. */
		e->crypto.enabled = 1;
		e->ht_chan_info.primary_chan = bss->channel;
		if (bss->channel > 14 && bss->width >= 40) {
			e->ht_chan_info.chan_width = 1;
			e->ht_chan_info.secondary_chan_off = ((bss->channel - first) / 4 % 2) ? 3 : 1;
			if (bss->width >= 80) {
				e->vht_chan_info.chan_width = bss->width == 160 ? 2 : 1;
				e->vht_chan_info.center_chan_1 = first + bss->width / 10 - 2;
			}
		}
	}
	*len = count * sizeof(*entries); return 0;
}
static void rax_mac(const char *v, uint8_t *mac)
{
	unsigned int b[6]; int i;
	if (sscanf(v, "%x:%x:%x:%x:%x:%x", b,b+1,b+2,b+3,b+4,b+5) == 6)
		for (i=0; i<6; i++) mac[i]=b[i];
}
static void rax_station(const char *name, struct iwinfo_assoclist_entry *entry)
{
	/* GET_ONE_STA_STATUS: 44-byte envelope, 12-byte argument (MAC + pointer).
	 * The retained ARMv7 driver returns a 288-byte station record. RSSI is
	 * encoded as dBm + 100; TX/RX rates at 18/20 are in 100 kbit/s units.
	 * Query by the full MAC, never match the redacted `get_sta_info` table.
	 */
	struct { uint16_t op, len; uint8_t mac[6]; uint16_t pad;
		void *data; uint8_t reserved[28]; } query = { .op = 0x1008, .len = 12 };
	struct iwreq req = {0};
	uint8_t record[288] = {0};
	uint16_t rate;
	if (sizeof(query) != 44) return;
	memcpy(query.mac, entry->mac, 6); query.data = record;
	req.u.data.pointer = &query; req.u.data.length = sizeof(query);
	if (rax_ioctl(name, 0x8bff, &req) || memcmp(record, entry->mac, 6)) return;
	if (record[13] != 255) {
		int signal = (int8_t)(record[13] - 100);
		if (signal < 0 && signal >= -110) entry->signal = signal;
	}
	memcpy(&rate, record + 18, 2); entry->tx_rate.rate = (uint32_t)rate * 100;
	memcpy(&rate, record + 20, 2); entry->rx_rate.rate = (uint32_t)rate * 100;
	if (record[15] == 20 || record[15] == 40 || record[15] == 80 || record[15] == 160)
		entry->tx_rate.mhz = entry->rx_rate.mhz = record[15];
}
static int rax_assoclist(const char *n, char *v, int *len)
{
	char reply[8192], cmd[64], previous[18] = "", val[128];
	struct iwinfo_assoclist_entry *e = (void *)v;
	int count = 0, rc, i;
	struct timespec start, now;
	*len = 0;
	clock_gettime(CLOCK_MONOTONIC, &start);
	rc = rax_request(n, "STA-FIRST", reply, sizeof(reply));
	while (!rc && count < IWINFO_BUFSIZE / sizeof(*e)) {
		uint8_t mac[6] = {0};
		/* Hostapd's station table is finite; bound and deduplicate regardless. */
		if (strlen(reply) < 17 || reply[17] != '\n' || !strncmp(previous, reply, 17)) break;
		rax_mac(reply, mac);
		if (!memcmp(mac, "\0\0\0\0\0\0", 6)) break;
		for (i = 0; i < count; i++)
			if (!memcmp(e[i].mac, mac, 6)) break;
		if (i != count) break;
		memset(e + count, 0, sizeof(*e)); memcpy(e[count].mac, mac, 6);
		if (!rax_value(reply, "flags", val, sizeof(val))) {
			e[count].is_authorized = strstr(val, "[AUTHORIZED]") != NULL;
			e[count].is_authenticated = strstr(val, "[AUTH]") != NULL;
			e[count].is_wme = strstr(val, "[WMM]") != NULL;
			e[count].is_mfp = strstr(val, "[MFP]") != NULL;
		}
		/* Only populate counters actually supplied by hostapd. */
#define RAX_STA_FIELD(k, field) if (!rax_value(reply, k, val, sizeof(val))) e[count].field = strtoull(val, NULL, 10)
		RAX_STA_FIELD("connected_time", connected_time);
		RAX_STA_FIELD("inactive_msec", inactive);
		RAX_STA_FIELD("rx_bytes", rx_bytes); RAX_STA_FIELD("tx_bytes", tx_bytes);
		RAX_STA_FIELD("rx_packets", rx_packets); RAX_STA_FIELD("tx_packets", tx_packets);
		RAX_STA_FIELD("signal", signal);
#undef RAX_STA_FIELD
		rax_station(n, &e[count]);
		int noise;
		if (!rax_noise(n, &noise)) e[count].noise = noise;
		memcpy(previous, reply, 17); previous[17] = 0; count++;
		clock_gettime(CLOCK_MONOTONIC, &now);
		if ((now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000 >= 1500)
			break;
		snprintf(cmd, sizeof(cmd), "STA-NEXT %s", previous);
		rc = rax_request(n, cmd, reply, sizeof(reply));
	}
	*len = count * sizeof(*e); return 0;
}
static int rax_signal(const char *n, int *out)
{
	char buf[IWINFO_BUFSIZE];
	struct iwinfo_assoclist_entry *entry = (void *)buf;
	int len, i, count = 0, sum = 0;
	if (rax_assoclist(n, buf, &len)) return -1;
	for (i = 0; i < len / sizeof(*entry); i++)
		if (entry[i].signal < 0) { sum += entry[i].signal; count++; }
	if (!count) return -1;
	*out = sum / count; return 0;
}
static int rax_bitrate(const char *n, int *out)
{
	char buf[IWINFO_BUFSIZE];
	struct iwinfo_assoclist_entry *entry = (void *)buf;
	int len, i, count = 0;
	uint64_t sum = 0;
	if (rax_assoclist(n, buf, &len)) return -1;
	for (i = 0; i < len / sizeof(*entry); i++)
		if (entry[i].tx_rate.rate) { sum += entry[i].tx_rate.rate; count++; }
	if (!count) return -1;
	*out = sum / count; return 0;
}
static int rax_quality(const char *n, int *out)
{
	int signal;
	if (rax_signal(n, &signal)) return -1;
	*out = signal < -110 ? 0 : signal > -40 ? 70 : signal + 110;
	return 0;
}
static int rax_quality_max(const char *n, int *out) { *out = 70; return 0; }
static void rax_close(void) {}
const struct iwinfo_ops rax3000h_ops = {
	.name = "rax3000h", .probe = rax_probe, .mode = rax_mode,
	.channel = rax_channel, .center_chan1 = rax_unavailable, .center_chan2 = rax_unavailable,
	.frequency = rax_frequency, .frequency_offset = rax_zero,
	.txpower = rax_txpower, .txpower_offset = rax_zero, .bitrate = rax_bitrate,
	.signal = rax_signal, .noise = rax_noise, .quality = rax_quality, .quality_max = rax_quality_max,
	.mbssid_support = rax_vaps, .hwmodelist = rax_hwmodelist, .htmodelist = rax_htmodelist, .htmode = rax_htmode,
	.ssid = rax_ssid, .bssid = rax_bssid, .country = rax_country,
	.hardware_id = rax_hardware_id, .hardware_name = rax_hardware_name, .encryption = rax_encryption,
	.phyname = rax_phyname, .assoclist = rax_assoclist, .txpwrlist = rax_empty, .scanlist = rax_scanlist,
	.freqlist = rax_freqlist, .countrylist = rax_countrylist, .survey = rax_empty,
	.lookup_phy = rax_lookup, .close = rax_close,
};
