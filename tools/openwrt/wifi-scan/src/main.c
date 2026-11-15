/* SPDX-License-Identifier: GPL-2.0-only */
#include "rax_scan.h"
#include <inttypes.h>

int main(int argc, char **argv)
{
	/* CN: use the observed 36-64 and 149-165 ranges. Do not treat unverified
	 * 100-144 channels as empty spectrum just because the scan lacks them. */
	static const int five[] = {36,40,44,48,52,56,60,64,149,153,157,161,165};
	static const int two[] = {1,6,11};
	struct rax_neighbor neighbors[RAX_SCAN_MAX];
	const int *candidates;
	int width, fallback, band5, count, i, size, best = 0;
	uint64_t best_score = UINT64_MAX;
	if (argc != 4) {
		fprintf(stderr, "Usage: rax-wifi-scan vapN 20|40|80|160 fallback-channel\n");
		return 2;
	}
	if (strncmp(argv[1], "vap", 3) || !argv[1][3] || argv[1][3 + strspn(argv[1] + 3, "0123456789")] || atoi(argv[1] + 3) > 15) return 2;
	width = atoi(argv[2]); fallback = atoi(argv[3]); band5 = atoi(argv[1] + 3) >= 8;
	if ((width != 20 && width != 40 && width != 80 && width != 160) || (!band5 && width > 40)) return 2;
	candidates = band5 ? five : two;
	size = band5 ? sizeof(five) / sizeof(five[0]) : sizeof(two) / sizeof(two[0]);
	count = rax_scan(argv[1], neighbors, 1, stderr);
	if (count < 0) { fprintf(stderr, "startup scan failed; using fallback channel %d\n", fallback); return 1; }
	if (!count) { fprintf(stderr, "startup scan returned no usable neighbors; using fallback channel %d\n", fallback); return 1; }
	fprintf(stderr, "startup scan completed: %d neighbors, width %d MHz\n", count, width);
	for (i = 0; i < size; i++) {
		int channel = candidates[i];
		uint64_t score;
		if (channel == 165 && width != 20) continue;
		if (width == 160 && channel >= 149) continue;
		score = rax_channel_score(neighbors, count, channel, width);
		fprintf(stderr, "candidate channel %d: score=%" PRIu64 "\n", channel, score);
		if (score < best_score || (score == best_score && channel == fallback)) {
			best_score = score; best = channel;
		}
	}
	fprintf(stderr, "selected channel %d: score=%" PRIu64 " (startup only)\n", best, best_score);
	printf("%d\n", best);
	return 0;
}
