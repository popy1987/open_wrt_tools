#include "reg.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static const char *INFER_ORDER[] = { "CN", "JP", "US", "DE", NULL };

static const int CN_5G[] = {
	36, 40, 44, 48, 52, 56, 60, 64, 149, 153, 157, 161, 165, -1
};
static const int JP_5G[] = {
	36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124,
	128, 132, 136, 140, -1
};
static const int US_5G[] = {
	36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124,
	128, 132, 136, 140, 144, 149, 153, 157, 161, 165, 169, 173, 177, -1
};

static int ch_in_array(int ch, const int *arr)
{
	for (; *arr >= 0; arr++)
		if (*arr == ch)
			return 1;
	return 0;
}

int is_6g_channel_number(int ch, int max)
{
	if (ch < 1 || ch > max)
		return 0;
	return ((ch - 1) % 4) == 0;
}

int is_6g_psc_channel(int ch)
{
	if (ch < 5 || ch > 229 || !is_6g_channel_number(ch, 233))
		return 0;
	return ((ch - 5) % 16) == 0;
}

static void strtolower_copy(char *dst, size_t n, const char *src)
{
	size_t i;

	for (i = 0; i + 1 < n && src[i]; i++)
		dst[i] = (char)tolower((unsigned char)src[i]);
	dst[i] = '\0';
}

static int cc_in_list(const char *cc, const char *list[])
{
	for (; *list; list++) {
		if (!strcmp(cc, *list))
			return 1;
	}
	return 0;
}

static int is_lpi_6g_country(const char *cc)
{
	static const char *LPI_6G[] = {
		"JP", "DE", "FR", "GB", "BE", "NL", "IT", "ES", "PT", "AT", "CH",
		"SE", "FI", "NO", "DK", "IE", "LU", "PL", "CZ", "EU",
		"AU", "NZ", "KR", "TW", "SG", "HK", "IN", "PH", "VN", "ID", "RU",
		NULL
	};

	return cc_in_list(cc, LPI_6G);
}

static int map_eu_cc(const char *cc, char *buf, size_t n)
{
	if (!strcmp(cc, "EU"))
		strncpy(buf, "DE", n);
	else
		strncpy(buf, cc, n);
	buf[n - 1] = '\0';
	return 0;
}

band_t normalize_band(const char *raw)
{
	char b[32];

	if (!raw || !*raw)
		return BAND_UNKNOWN;
	strtolower_copy(b, sizeof b, raw);
	/* 6 GHz before 5 GHz — 11ax/11be appear on both; band=6g is authoritative */
	if (strstr(b, "6g") || strstr(b, "6ghz") || strstr(b, "6e") ||
	    !strcmp(b, "11be") || strstr(b, "eht"))
		return BAND_6G;
	if (strstr(b, "2g") || !strcmp(b, "11b") || !strcmp(b, "11g") ||
	    !strcmp(b, "11gn") || !strcmp(b, "11ng") || !strcmp(b, "11bgn") ||
	    !strcmp(b, "11bg") || !strcmp(b, "bg") || !strcmp(b, "g") ||
	    !strcmp(b, "mixed") || strstr(b, "2.4"))
		return BAND_2G;
	if (strstr(b, "5g") || !strcmp(b, "11a") || !strcmp(b, "11na") ||
	    !strcmp(b, "11ac") || !strcmp(b, "11ax") || !strcmp(b, "na") ||
	    !strcmp(b, "ac") || !strcmp(b, "ax"))
		return BAND_5G;
	return BAND_UNKNOWN;
}

int country_6g_max_channel(const char *cc)
{
	char c[8];

	if (!cc || !*cc)
		return 0;
	map_eu_cc(cc, c, sizeof c);
	if (!strcmp(c, "CN"))
		return 0;
	if (!strcmp(c, "US") || !strcmp(c, "CA"))
		return 233;
	if (is_lpi_6g_country(c))
		return 93;
	if (!strcmp(c, "BR") || !strcmp(c, "MX"))
		return 93; /* partial U-NII; conservative cap */
	return 0;
}

int cn_6g_prohibited(const char *cc, band_t band)
{
	char c[8];

	if (band != BAND_6G || !cc)
		return 0;
	map_eu_cc(cc, c, sizeof c);
	return !strcmp(c, "CN");
}

int jp_eu_6g_lpi_channel(const char *cc, band_t band, int ch)
{
	char c[8];
	int max;

	if (band != BAND_6G || !cc)
		return 0;
	map_eu_cc(cc, c, sizeof c);
	if (!strcmp(c, "US") || !strcmp(c, "CA"))
		return 0;
	max = country_6g_max_channel(c);
	return max > 0 && max <= 93 && ch >= 1 && ch <= max &&
	       is_6g_channel_number(ch, max);
}

int channel_allowed_in_country(const char *cc, band_t band, int ch)
{
	char c[8];
	int max6;

	map_eu_cc(cc, c, sizeof c);

	switch (band) {
	case BAND_2G:
		if (!strcmp(c, "US") || !strcmp(c, "CA"))
			return ch >= 1 && ch <= 11;
		return ch >= 1 && ch <= 13;
	case BAND_5G:
		if (!strcmp(c, "CN"))
			return ch_in_array(ch, CN_5G);
		if (!strcmp(c, "JP"))
			return ch_in_array(ch, JP_5G);
		if (!strcmp(c, "US") || !strcmp(c, "CA"))
			return ch_in_array(ch, US_5G);
		if (!strcmp(c, "DE") || !strcmp(c, "FR") || !strcmp(c, "GB") ||
		    !strcmp(c, "BE") || !strcmp(c, "NL") || !strcmp(c, "IT") ||
		    !strcmp(c, "ES") || !strcmp(c, "PT") || !strcmp(c, "AT") ||
		    !strcmp(c, "CH") || !strcmp(c, "SE") || !strcmp(c, "FI") ||
		    !strcmp(c, "NO") || !strcmp(c, "DK") || !strcmp(c, "IE") ||
		    !strcmp(c, "LU") || !strcmp(c, "PL") || !strcmp(c, "CZ"))
			return ch_in_array(ch, JP_5G);
		return 0;
	case BAND_6G:
		max6 = country_6g_max_channel(c);
		if (max6 <= 0)
			return 0;
		return is_6g_channel_number(ch, max6);
	default:
		return 0;
	}
}

int infer_country_from_channel(band_t band, int ch, char *out_cc, size_t out_len)
{
	const char **cc;

	for (cc = INFER_ORDER; *cc; cc++) {
		if (channel_allowed_in_country(*cc, band, ch)) {
			strncpy(out_cc, *cc, out_len);
			out_cc[out_len - 1] = '\0';
			return 0;
		}
	}
	return -1;
}

int is_dfs_5g_channel(int ch)
{
	if (ch >= 52 && ch <= 64)
		return 1;
	if (ch >= 100 && ch <= 144)
		return 1;
	return 0;
}

int channel_requires_cac(band_t band, int ch)
{
	return band == BAND_5G && is_dfs_5g_channel(ch);
}

int cn_5g_indoor_channel(const char *cc, band_t band, int ch)
{
	return cc && !strcmp(cc, "CN") && band == BAND_5G && ch >= 36 && ch <= 64;
}

static int channel_ambiguous_5g_6g(int ch)
{
	return ch >= 36 && ch <= 64 && is_6g_channel_number(ch, 233);
}

band_t channel_suggests_band(int ch)
{
	if (ch >= 1 && ch <= 14)
		return BAND_2G;
	if (channel_ambiguous_5g_6g(ch))
		return BAND_UNKNOWN;
	if (ch >= 36 && ch <= 196)
		return BAND_5G;
	if (ch >= 1 && ch <= 233 && is_6g_channel_number(ch, 233))
		return BAND_6G;
	return BAND_UNKNOWN;
}

void channel_suggest_text(int ch, band_t radio_band, char *buf, size_t n)
{
	band_t sug = channel_suggests_band(ch);

	if (radio_band != BAND_UNKNOWN) {
		if (radio_band == BAND_6G)
			snprintf(buf, n, "6g (6G ch%d != 5G ch%d)", ch, ch);
		else if (radio_band == BAND_5G)
			snprintf(buf, n, "5g");
		else
			snprintf(buf, n, "2g");
		return;
	}
	if (sug == BAND_UNKNOWN && channel_ambiguous_5g_6g(ch))
		snprintf(buf, n, "5g or 6g (ambiguous — check -i radio)");
	else if (sug == BAND_2G)
		snprintf(buf, n, "2g");
	else if (sug == BAND_5G)
		snprintf(buf, n, "5g");
	else if (sug == BAND_6G)
		snprintf(buf, n, "6g");
	else
		snprintf(buf, n, "?");
}

int channel_to_mhz(band_t band, int ch)
{
	switch (band) {
	case BAND_2G:
		if (ch >= 1 && ch <= 13)
			return 2407 + 5 * ch;
		if (ch == 14)
			return 2484;
		return 0;
	case BAND_5G:
		if (ch >= 36 && ch <= 177)
			return 5000 + 5 * ch;
		return 0;
	case BAND_6G:
		if (is_6g_channel_number(ch, 233))
			return 5950 + 5 * ch;
		return 0;
	default:
		return 0;
	}
}

void print_country_matrix(void)
{
	puts("Legend: i=indoor  D=DFS  OK=allowed  X=prohibited  PSC=preferred scanning (6G US)");
	puts("        6 GHz channel numbers are independent of 2.4/5 GHz (6G ch37 != 5G ch37)");
	puts("");
	puts("=== Country code x band quick reference ===");
	printf("%-4s %-28s %-28s %-20s\n", "CC", "2.4 GHz", "5 GHz", "6 GHz");
	printf("%-4s %-28s %-28s %-20s\n", "CN", "1-13", "36-64,149-165", "X not open");
	printf("%-4s %-28s %-28s %-20s\n", "JP", "1-13", "36-140,no 149", "1-93 LPI indoor");
	printf("%-4s %-28s %-28s %-20s\n", "US", "1-11", "36-165 full", "1-233 (PSC step16)");
	printf("%-4s %-28s %-28s %-20s\n", "DE", "1-13", "36-140,no 149", "1-93 LPI indoor");
	puts("");
	puts("6 GHz valid channel numbers: 1,5,9,...,233 (step 4 from ch 1)");
	puts("US PSC (20 MHz): 5,21,37,53,69,85,101,117,133,149,165,181,197,213,229");
	puts("Inference priority: CN -> JP -> US -> EU (DE)");
	puts("CN regdb: no 6 GHz Wi-Fi — use JP/US/DE for BE14 radio2 tests");
	puts("");
}

void print_band_channel_ref(const char *cc, band_t band)
{
	const char *c = cc ? cc : "?";

	printf("[Regulatory reference — %s / ", c);
	switch (band) {
	case BAND_2G: puts("2g]"); break;
	case BAND_5G: puts("5g]"); break;
	case BAND_6G: puts("6g]"); break;
	default: puts("?]"); break;
	}

	switch (band) {
	case BAND_2G:
		if (cc && (!strcmp(cc, "US") || !strcmp(cc, "CA")))
			puts("  Common: 1,6,11 (regulatory 1-11)");
		else if (cc && !strcmp(cc, "JP"))
			puts("  Allowed: 1-13 (ch14 is 802.11b only)");
		else
			puts("  Allowed: 1-13");
		break;
	case BAND_5G:
		if (cc && !strcmp(cc, "CN")) {
			puts("  Indoor+DFS: 36,40,44,48,52,56,60,64");
			puts("  Outdoor:    149,153,157,161,165");
		} else if (cc && !strcmp(cc, "JP")) {
			puts("  Indoor: 36-48 | DFS: 52-64,100-140 | Prohibited: 149-165");
		} else if (cc && (!strcmp(cc, "US") || !strcmp(cc, "CA"))) {
			puts("  No DFS: 36-48,149-165 | DFS: 52-64,100-144");
		} else if (cc && (!strcmp(cc, "DE") || !strcmp(cc, "EU"))) {
			puts("  Indoor: 36-48 | DFS: 52-64,100-140 | Prohibited: 149-165");
		} else {
			puts("  Typical: 36-48; 52-64(DFS); 100-140(DFS); 149-165(varies)");
		}
		break;
	case BAND_6G:
		if (cn_6g_prohibited(cc, band)) {
			puts("  X CN regdb: 6 GHz Wi-Fi generally not available");
		} else if (cc && (!strcmp(cc, "US") || !strcmp(cc, "CA"))) {
			puts("  20 MHz: ch 1-233 (step 4); PSC every 16 from ch 5");
			puts("  WPA3 required; 6G ch numbers != 5G (e.g. ch37 = 6135 MHz)");
		} else if (country_6g_max_channel(cc) > 0) {
			printf("  LPI indoor 5945-6425 MHz, ch 1-%d (step 4)\n",
			       country_6g_max_channel(cc));
			puts("  WPA3 required; outdoor/VLP rules vary by country");
		} else {
			puts("  See iw phy for live regdomain; try JP/US/DE for tests");
		}
		break;
	default:
		puts("  (unknown band)");
		break;
	}
	puts("");
}

const char *channel_regulatory_tag_line(const char *cc, band_t band, int ch)
{
	if (channel_requires_cac(band, ch))
		return "  [DFS — expect CAC 1-10 min after reload]";
	if (cn_6g_prohibited(cc, band))
		return "  [CN: 6 GHz not available]";
	if (cn_5g_indoor_channel(cc, band, ch))
		return "  [CN 5.2G — indoor use only]";
	if (band == BAND_6G && jp_eu_6g_lpi_channel(cc, band, ch))
		return "  [6G LPI indoor — ch 1-93]";
	if (band == BAND_6G && cc && (!strcmp(cc, "US") || !strcmp(cc, "CA")) &&
	    is_6g_psc_channel(ch))
		return "  [6G US PSC channel]";
	if (band == BAND_6G && ch >= 1 && ch <= 233)
		return "  [6G — WPA3 required on clients/AP]";
	return "";
}

void print_channel_regulatory_warnings(const char *cc, band_t band, int ch,
				       const char *radio)
{
	int mhz;
	int warned_cac = 0;
	const char *bname = band == BAND_2G ? "2g" :
			    band == BAND_5G ? "5g" :
			    band == BAND_6G ? "6g" : "?";

	mhz = channel_to_mhz(band, ch);

	if (cn_6g_prohibited(cc, band)) {
		puts("");
		puts("================================================================");
		puts("  *** ERROR: CN regdb — 6 GHz Wi-Fi not available ***");
		puts("================================================================");
		if (radio && *radio)
			printf("  Radio     : %s\n", radio);
		printf("  Band      : %s\n", bname);
		printf("  Channel   : %d (%d MHz if 6G numbering)\n", ch,
		       mhz ? mhz : 5950 + 5 * ch);
		puts("  Use -c JP / US / DE for BE14 6 GHz radio tests.");
		puts("================================================================");
		return;
	}

	if (channel_requires_cac(band, ch)) {
		warned_cac = 1;
		puts("");
		puts("================================================================");
		puts("  *** WARNING: DFS channel — radio silence (CAC) required ***");
		puts("================================================================");
		if (radio && *radio)
			printf("  Radio     : %s\n", radio);
		printf("  Band      : %s\n", bname);
		printf("  Channel   : %d (%d MHz)\n", ch, mhz ? mhz : 0);
		printf("  Country   : %s\n", cc ? cc : "?");
		puts("");
		puts("  AP must complete CAC before transmitting (typically 1-10 min).");
		puts("  SSID may be invisible during CAC; do not reboot repeatedly.");
		if (cc && !strcmp(cc, "CN"))
			puts("  CN: ch 52-64 require DFS+TPC; ch 36-48 indoor-only.");
		else if (cc && (!strcmp(cc, "JP") || !strcmp(cc, "DE") ||
				!strcmp(cc, "EU")))
			puts("  JP/EU: ch 100-140 DFS; ch 52-64 often indoor+DFS.");
		else if (cc && (!strcmp(cc, "US") || !strcmp(cc, "CA")))
			puts("  US: ch 52-64 and 100-144 are DFS bands.");
		puts("================================================================");
	}

	if (band == BAND_6G) {
		puts("");
		puts("  *** NOTE (6 GHz) ***");
		printf("  Channel %d = %d MHz (6G numbering; NOT the same as 5G ch %d)\n",
		       ch, mhz ? mhz : 0, ch);
		puts("  Clients and AP must support Wi-Fi 6E/7 and WPA3 on 6 GHz.");
		if (cc && (!strcmp(cc, "US") || !strcmp(cc, "CA"))) {
			if (is_6g_psc_channel(ch))
				puts("  US PSC channel — preferred for discovery/ACS.");
			if (ch > 93)
				puts("  US: ch >93 in U-NII-5~8 (standard power rules apply).");
		} else if (jp_eu_6g_lpi_channel(cc, band, ch)) {
			puts("  JP/EU/AU/KR etc.: LPI indoor 5945-6425 MHz (ch 1-93).");
			puts("  Do not use for outdoor/high-power without local compliance.");
		}
		if (cc && (!strcmp(cc, "BR") || !strcmp(cc, "MX")))
			puts("  BR/MX: partial 6G bands — verify iw phy * before production.");
	}

	if (cn_5g_indoor_channel(cc, band, ch)) {
		puts("");
		puts("  *** NOTE (CN): 5.2 GHz (ch 36-64) — indoor use only ***");
		if (!warned_cac)
			puts("  ch 36-48: usually no long CAC; ch 52-64: DFS+CAC.");
	}
}
