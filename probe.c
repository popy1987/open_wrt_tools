#include "probe.h"
#include "reg.h"
#include "wifi_uci.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <uci.h>

static struct uci_context *uci_ctx(void)
{
	return wifi_uci_context();
}

static int stristr(const char *hay, const char *needle)
{
	size_t nlen = strlen(needle);
	size_t i;

	if (!nlen)
		return 1;
	for (i = 0; hay[i]; i++) {
		size_t j;

		for (j = 0; j < nlen; j++) {
			if (!hay[i + j])
				return 0;
			if (tolower((unsigned char)hay[i + j]) !=
			    tolower((unsigned char)needle[j]))
				break;
		}
		if (j == nlen)
			return 1;
	}
	return 0;
}

static int have_cmd(const char *cmd)
{
	char buf[128];

	snprintf(buf, sizeof buf, "command -v %s >/dev/null 2>&1", cmd);
	return system(buf) == 0;
}

static void section(const char *title)
{
	puts("");
	puts("================================================================");
	printf("  %s\n", title);
	puts("================================================================");
	puts("");
}

static void pipe_cmd(const char *cmd)
{
	FILE *fp = popen(cmd, "r");
	char line[512];

	if (!fp) {
		puts("  (unavailable)");
		return;
	}
	while (fgets(line, sizeof line, fp)) {
		fputs("  ", stdout);
		fputs(line, stdout);
	}
	pclose(fp);
}

static void read_file_line(const char *path, char *buf, size_t n)
{
	FILE *fp = fopen(path, "r");

	buf[0] = '\0';
	if (!fp)
		return;
	if (fgets(buf, (int)n, fp))
		buf[strcspn(buf, "\r\n")] = '\0';
	fclose(fp);
}

static const char *band_name(band_t b)
{
	switch (b) {
	case BAND_2G: return "2g";
	case BAND_5G: return "5g";
	case BAND_6G: return "6g";
	default: return "?";
	}
}

static int uci_get_iface_wireless(const char *iface, const char *opt,
				  char *buf, size_t n)
{
	struct uci_ptr ptr;
	char path[128];

	snprintf(path, sizeof path, "wireless.%s.%s", iface, opt);
	if (uci_lookup_ptr(uci_ctx(), &ptr, path, true) != UCI_OK)
		return -1;
	if (!ptr.o || !ptr.o->v.string)
		return -1;
	strncpy(buf, ptr.o->v.string, n - 1);
	buf[n - 1] = '\0';
	return 0;
}

static int radio_uci_phy(const char *radio, char *buf, size_t n)
{
	if (uci_get_wireless(radio, "phy", buf, n) == 0)
		return 0;
	buf[0] = '\0';
	return -1;
}

static int uci_channels_list(const char *radio, char *buf, size_t n)
{
	struct uci_package *pkg = NULL;
	struct uci_element *e;
	struct uci_section *s;
	char *p = buf;
	size_t left = n;
	int first = 1;

	buf[0] = '\0';
	if (uci_load(uci_ctx(), "wireless", &pkg) != UCI_OK)
		return -1;

	uci_foreach_element(&pkg->sections, e) {
		s = uci_to_section(e);
		if (strcmp(s->e.name, radio) != 0)
			continue;
		uci_foreach_element(&s->options, e) {
			struct uci_option *o = uci_to_option(e);

			if (strcmp(o->e.name, "channels") != 0)
				continue;
			if (o->type == UCI_TYPE_STRING) {
				int w = snprintf(p, left, "%s%s", first ? "" : " ",
						 o->v.string);
				if (w > 0 && (size_t)w < left) {
					p += w;
					left -= (size_t)w;
					first = 0;
				}
			} else if (o->type == UCI_TYPE_LIST) {
				struct uci_element *le;

				uci_foreach_element(&o->v.list, le) {
					int w = snprintf(p, left, "%s%s",
							 first ? "" : " ",
							 le->name);
					if (w > 0 && (size_t)w < left) {
						p += w;
						left -= (size_t)w;
						first = 0;
					}
				}
			}
		}
	}
	uci_unload(uci_ctx(), pkg);
	return first ? -1 : 0;
}

static int list_wifi_ifaces(char ifaces[][32], int max)
{
	struct uci_package *pkg = NULL;
	struct uci_element *e;
	int n = 0;

	if (uci_load(uci_ctx(), "wireless", &pkg) != UCI_OK)
		return 0;

	uci_foreach_element(&pkg->sections, e) {
		struct uci_section *s = uci_to_section(e);

		if (strcmp(s->type, "wifi-iface") != 0)
			continue;
		if (n >= max)
			break;
		strncpy(ifaces[n], s->e.name, 31);
		ifaces[n][31] = '\0';
		n++;
	}
	uci_unload(uci_ctx(), pkg);
	return n;
}

static int iface_radio(const char *iface, char *radio, size_t n)
{
	return uci_get_iface_wireless(iface, "device", radio, n);
}

static void print_system(void)
{
	char board[128], model[128], release[256], kernel[128], ram[64];
	char hostname[128];

	section("1. System / platform");

	if (have_cmd("ubus")) {
		FILE *fp = popen("ubus call system board 2>/dev/null", "r");
		char line[512];

		board[0] = model[0] = ram[0] = '\0';
		if (fp) {
			while (fgets(line, sizeof line, fp)) {
				char *q;

				if (strstr(line, "\"board_name\"")) {
					q = strchr(line, ':');
					if (q) {
						q = strchr(q, '"');
						if (q) {
							sscanf(q + 1, "%127[^\"]",
							       board);
						}
					}
				}
				if (strstr(line, "\"model\"")) {
					q = strchr(line, ':');
					if (q) {
						q = strchr(q, '"');
						if (q)
							sscanf(q + 1, "%127[^\"]",
							       model);
					}
				}
				if (strstr(line, "\"memory\"")) {
					unsigned long mem = 0;

					if (sscanf(line, "%*[^0-9]%lu", &mem) == 1)
						snprintf(ram, sizeof ram, "%lu MB",
							 mem / 1024 / 1024);
				}
			}
			pclose(fp);
		}
	} else {
		board[0] = model[0] = ram[0] = '\0';
	}

	read_file_line("/tmp/sysinfo/board_name", board, sizeof board);
	read_file_line("/tmp/sysinfo/model", model, sizeof model);
	if (!ram[0]) {
		FILE *fp = fopen("/proc/meminfo", "r");
		unsigned long kb = 0;
		char line[128];

		if (fp) {
			while (fgets(line, sizeof line, fp)) {
				if (sscanf(line, "MemTotal: %lu kB", &kb) == 1)
					break;
			}
			fclose(fp);
			if (kb)
				snprintf(ram, sizeof ram, "%lu MB", kb / 1024);
		}
	}

	read_file_line("/etc/openwrt_release", release, sizeof release);
	read_file_line("/proc/sys/kernel/osrelease", kernel, sizeof kernel);
	read_file_line("/proc/sys/kernel/hostname", hostname, sizeof hostname);

	printf("  Hostname     : %s\n", hostname[0] ? hostname : "?");
	printf("  Board        : %s\n", board[0] ? board : "unknown");
	printf("  Model        : %s\n", model[0] ? model : "unknown");
	printf("  RAM          : %s\n", ram[0] ? ram : "unknown");
	printf("  Kernel       : %s\n", kernel[0] ? kernel : "unknown");
	printf("  OpenWrt      : %s\n", release[0] ? release : "unknown");

	if (stristr(board, "bpi-r4") || stristr(model, "bpi-r4") ||
	    stristr(board, "bananapi") || stristr(model, "bananapi")) {
		puts("");
		puts("  [BPI-R4 note] BE14 Wi-Fi 7: set board SW4=ON for module power.");
		puts("  [BPI-R4 note] Low 6G/5G TX power: U-Boot overlay");
		puts("                mt7988a-bananapi-bpi-r4-wifi-be14 in bootconf_extra");
	}
}

static void print_uboot_overlay(void)
{
	section("2. U-Boot / device-tree overlays (BE14 TX power)");

	if (!have_cmd("fw_printenv")) {
		puts("  fw_printenv not available (install uboot-envtools on target if needed)");
		return;
	}
	puts("  bootconf_extra:");
	pipe_cmd("fw_printenv bootconf_extra 2>/dev/null | sed 's/^/    /'");
	if (system("fw_printenv -n bootconf_extra 2>/dev/null | grep -q wifi-be14") == 0)
		puts("  Status: BE14 overlay present");
	else
		puts("  Status: BE14 overlay NOT found (may cause low TX power on some modules)");
}

static void print_regdomain(void)
{
	section("3. Regulatory domain (critical for country / channel)");

	puts("  Source: kernel cfg80211 (global for all radios)");
	puts("");
	if (have_cmd("iw"))
		pipe_cmd("iw reg get 2>/dev/null | sed 's/^/  /'");
	else
		puts("  iw not installed — opkg install iw");
	puts("");
	puts("  UCI country per radio (should match operational intent):");
	if (access("/etc/config/wireless", R_OK) == 0)
		pipe_cmd("uci show wireless 2>/dev/null | grep -E '\\.(country|country_code)=' | sed 's/^/    /'");
	else
		puts("    /etc/config/wireless not found");
}

static void print_kernel_wifi(void)
{
	section("4. Kernel wireless stack");

	puts("  Loaded modules (mt76 / mac80211 / cfg80211):");
	pipe_cmd("lsmod 2>/dev/null | grep -E '^mt76|^mac80211|^cfg80211|^mt799' | sed 's/^/    /'");
	puts("");
	puts("  ieee80211 phys:");
	if (access("/sys/class/ieee80211", F_OK) == 0)
		pipe_cmd("for p in /sys/class/ieee80211/phy*; do [ -d \"$p\" ] && echo \"    $(basename $p)  device=$(readlink -f $p/device 2>/dev/null)\"; done");
	else
		puts("    (no /sys/class/ieee80211)");
}

static void print_uci_wireless(void)
{
	char radios[MAX_RADIOS][32];
	int nr, i;

	section("5. UCI wireless — wifi-device (use with cwc -i)");

	if (access("/etc/config/wireless", R_OK) != 0) {
		puts("  /etc/config/wireless not found");
		return;
	}

	printf("  %-8s %-5s %-8s %-8s %-12s %-8s %s\n",
	       "RADIO", "BAND", "COUNTRY", "CHANNEL", "CHANNELS[]", "PHY(UCI)",
	       "DISABLED");
	printf("  %-8s %-5s %-8s %-8s %-12s %-8s %s\n",
	       "--------", "-----", "--------", "--------", "------------",
	       "--------", "--------");

	nr = list_radios(radios, MAX_RADIOS);
	for (i = 0; i < nr; i++) {
		char cc[16], ch[16], chlist[128], phy[16], dis[8];
		band_t b = radio_band(radios[i]);

		if (uci_get_wireless(radios[i], "country", cc, sizeof cc) != 0)
			strcpy(cc, "-");
		if (uci_get_wireless(radios[i], "channel", ch, sizeof ch) != 0)
			strcpy(ch, "-");
		if (uci_channels_list(radios[i], chlist, sizeof chlist) != 0)
			strcpy(chlist, "-");
		if (radio_uci_phy(radios[i], phy, sizeof phy) != 0)
			strcpy(phy, "-");
		if (uci_get_wireless(radios[i], "disabled", dis, sizeof dis) != 0)
			strcpy(dis, "0");

		printf("  %-8s %-5s %-8s %-8s %-12s %-8s %s\n", radios[i],
		       band_name(b), cc, ch, chlist, phy, dis);
	}

	puts("");
	puts("  Detail per radio:");
	for (i = 0; i < nr; i++) {
		char htmode[32], path[64], txp[16], phy[16];
		char ifaces[16][32];
		int ni, j;

		printf("  --- %s ---\n", radios[i]);
		if (uci_get_wireless(radios[i], "htmode", htmode, sizeof htmode) != 0)
			strcpy(htmode, "-");
		if (uci_get_wireless(radios[i], "path", path, sizeof path) != 0)
			strcpy(path, "-");
		if (uci_get_wireless(radios[i], "txpower", txp, sizeof txp) != 0)
			strcpy(txp, "-");
		radio_uci_phy(radios[i], phy, sizeof phy);
		printf("    band=%s  htmode=%s  txpower=%s\n",
		       band_name(radio_band(radios[i])), htmode, txp);
		printf("    path=%s\n", path);
		printf("    phy(UCI)=%s\n", phy[0] ? phy : "-");

		ni = list_wifi_ifaces(ifaces, 16);
		for (j = 0; j < ni; j++) {
			char dev[32], ssid[64], mode[16], enc[32], idis[8];

			if (iface_radio(ifaces[j], dev, sizeof dev) != 0 ||
			    strcmp(dev, radios[i]) != 0)
				continue;
			printf("    wifi-iface %s:\n", ifaces[j]);
			if (uci_get_iface_wireless(ifaces[j], "ssid", ssid, sizeof ssid) != 0)
				strcpy(ssid, "-");
			if (uci_get_iface_wireless(ifaces[j], "mode", mode, sizeof mode) != 0)
				strcpy(mode, "-");
			if (uci_get_iface_wireless(ifaces[j], "encryption", enc, sizeof enc) != 0)
				strcpy(enc, "-");
			if (uci_get_iface_wireless(ifaces[j], "disabled", idis, sizeof idis) != 0)
				strcpy(idis, "0");
			printf("      ssid=%s\n", ssid);
			printf("      mode=%s\n", mode);
			printf("      encryption=%s\n", enc);
			printf("      disabled=%s\n", idis);
		}
		puts("");
	}
}

static void print_iw_phy_summary(int verbose)
{
	section("6. iw phy — driver & allowed channels (* = TX capable now)");

	if (!have_cmd("iw")) {
		puts("  opkg install iw");
		return;
	}

	pipe_cmd(
		"for phy in $(ls /sys/class/ieee80211 2>/dev/null | sort); do "
		"echo \"  === $phy ===\"; "
		"iw phy \"$phy\" info 2>/dev/null | sed -n '1,3p' | sed 's/^/    /'; "
		"iw phy \"$phy\" info 2>/dev/null | grep -E 'wiphy|max.*SSID|Available channels' | sed 's/^/    /'; "
		"echo '    Channels (current regdomain, * = TX allowed):'; "
		"iw phy \"$phy\" info 2>/dev/null | "
		"grep -E 'Band | MHz \\[' | sed 's/^/    /'; "
		"echo ''; "
		"done");

	if (verbose) {
		puts("  --- raw iw phy dumps ---");
		pipe_cmd("for phy in $(ls /sys/class/ieee80211 2>/dev/null | sort); do "
			 "echo \"    --- raw iw phy $phy info ---\"; "
			 "iw phy \"$phy\" info 2>/dev/null | sed 's/^/    /'; echo ''; "
			 "done");
	}
}

static void print_runtime(void)
{
	char radios[MAX_RADIOS][32];
	char ifaces[16][32];
	int nr, ni, i, j;

	section("7. Runtime interfaces (live signal / channel)");

	if (have_cmd("iw")) {
		puts("  iw dev:");
		pipe_cmd("iw dev 2>/dev/null | sed 's/^/    /'");
		puts("");
	}

	if (have_cmd("iwinfo")) {
		nr = list_radios(radios, MAX_RADIOS);
		ni = list_wifi_ifaces(ifaces, 16);
		for (i = 0; i < nr; i++) {
			for (j = 0; j < ni; j++) {
				char dev[32], ifname[32];

				if (iface_radio(ifaces[j], dev, sizeof dev) != 0 ||
				    strcmp(dev, radios[i]) != 0)
					continue;
				if (uci_get_iface_wireless(ifaces[j], "ifname",
							   ifname, sizeof ifname) != 0)
					ifname[0] = '\0';
				if (!ifname[0]) {
					pipe_cmd("ls /sys/class/net/ 2>/dev/null | "
						 "grep -E '^wlan|^phy' | head -1");
					continue;
				}
				printf("  iwinfo %s (radio=%s band=%s iface=%s):\n",
				       ifname, radios[i], band_name(radio_band(radios[i])),
				       ifaces[j]);
				{
					char cmd[256];

					snprintf(cmd, sizeof cmd,
						 "iwinfo %s info 2>/dev/null | sed 's/^/    /'",
						 ifname);
					pipe_cmd(cmd);
				}
				puts("");
			}
		}
	} else {
		puts("  iwinfo not installed (optional: opkg install iwinfo)");
	}

	if (access("/var/run/hostapd", F_OK) == 0) {
		puts("  hostapd country_code from runtime config:");
		pipe_cmd("grep -h '^country_code=' /var/run/hostapd-*.conf 2>/dev/null | "
			 "sort -u | sed 's/^/    /'");
	}
}

static void print_dmesg_hints(void)
{
	section("8. Driver messages (TX power / EEPROM hints)");

	if (system("dmesg 2>/dev/null | grep -iE 'mt7996|mt76|eeprom|tx_power|regdom' | "
		   "tail -20 | grep -q .") == 0)
		pipe_cmd("dmesg 2>/dev/null | grep -iE 'mt7996|mt76|eeprom|tx_power|regdom' | "
			 "tail -25 | sed 's/^/  /'");
	else
		puts("  (no recent mt76/regdom/eeprom lines in dmesg)");
}

static void print_cheat_sheet(void)
{
	char radios[MAX_RADIOS][32];
	int nr, i;

	section("9. Cheat sheet for cwc");

	puts("  Mapping (verify band column — radioN names are device-specific):");
	puts("");
	nr = list_radios(radios, MAX_RADIOS);
	for (i = 0; i < nr; i++) {
		char cc[16], ch[16];

		if (uci_get_wireless(radios[i], "channel", ch, sizeof ch) != 0)
			strcpy(ch, "?");
		if (uci_get_wireless(radios[i], "country", cc, sizeof cc) != 0)
			strcpy(cc, "?");
		printf("    %s  band=%s  current country=%s  current channel=%s\n",
		       radios[i], band_name(radio_band(radios[i])), cc, ch);
		if (radio_band(radios[i]) == BAND_6G) {
			printf("      6G example: cwc -c JP -i %s -n 37 -y\n",
			       radios[i]);
			printf("      (CN has no 6 GHz; 6G ch37 = 6135 MHz, != 5G ch37)\n");
		} else {
			printf("      Example: cwc -i %s -n <CH>\n",
			       radios[i]);
			printf("      Example: cwc -c JP -i %s -n <CH> -y\n",
			       radios[i]);
		}
	}
	puts("");
	puts("  Country inference order (when -c omitted): CN -> JP -> US -> DE");
	puts("  6 GHz: valid ch 1,5,9,... (step 4); CN prohibited; JP/EU LPI ch 1-93");
	puts("  Read reference: cwc -r");
	puts("  Detailed status: cwc -l");
	puts("  Full HW probe:   cwc -p");
}

static void print_summary(void)
{
	char radios[MAX_RADIOS][32];
	char board[64], model[64], release[32], regline[256];
	int nr, i;
	FILE *fp;

	read_file_line("/tmp/sysinfo/board_name", board, sizeof board);
	read_file_line("/tmp/sysinfo/model", model, sizeof model);
	release[0] = '\0';
	fp = fopen("/etc/openwrt_release", "r");
	if (fp) {
		char line[128];

		while (fgets(line, sizeof line, fp)) {
			if (strncmp(line, "DISTRIB_RELEASE=", 16) == 0) {
				strncpy(release, line + 16, sizeof release - 1);
				release[strcspn(release, "'\"\r\n")] = '\0';
				break;
			}
		}
		fclose(fp);
	}

	regline[0] = '\0';
	fp = popen("iw reg get 2>/dev/null | head -1", "r");
	if (fp) {
		fgets(regline, sizeof regline, fp);
		regline[strcspn(regline, "\r\n")] = '\0';
		pclose(fp);
	}

	puts("================================================================");
	puts("  Wi-Fi HW probe summary");
	puts("================================================================");
	printf("  Board    : %s / %s\n", board, model);
	printf("  OpenWrt  : %s\n", release[0] ? release : "?");
	printf("  Regdomain: %s\n\n", regline[0] ? regline : "unknown");

	printf("  %-8s %-5s %-8s %-8s %-8s\n", "RADIO", "BAND", "COUNTRY",
	       "CHANNEL", "PHY");
	nr = list_radios(radios, MAX_RADIOS);
	for (i = 0; i < nr; i++) {
		char cc[16], ch[16], phy[16];

		if (uci_get_wireless(radios[i], "country", cc, sizeof cc) != 0)
			strcpy(cc, "-");
		if (uci_get_wireless(radios[i], "channel", ch, sizeof ch) != 0)
			strcpy(ch, "-");
		if (radio_uci_phy(radios[i], phy, sizeof phy) != 0)
			strcpy(phy, "-");
		printf("  %-8s %-5s %-8s %-8s %-8s\n", radios[i],
		       band_name(radio_band(radios[i])), cc, ch, phy);
	}
	puts("");
	puts("  Next: cwc -l   (status before change)");
	puts("        cwc -p   (re-run full probe)");
	puts("================================================================");
}

/* -------------------------------------------------------------------------- */
/* Machine-readable KEY=VALUE status (channel / regulatory / UCI / runtime)   */
/* -------------------------------------------------------------------------- */

#define UCI_SECTION_NAME_MAX 31
#define STATUS_KEY_LEN 128

static void status_key_radio(char *key, size_t n, const char *radio, const char *suffix)
{
	snprintf(key, n, "radio.%.*s.%s", UCI_SECTION_NAME_MAX, radio, suffix);
}

static void status_key_radio_iface(char *key, size_t n, const char *radio,
				   const char *iface, const char *suffix)
{
	snprintf(key, n, "radio.%.*s.iface.%.*s.%s",
		 UCI_SECTION_NAME_MAX, radio, UCI_SECTION_NAME_MAX, iface, suffix);
}

static void status_kv(const char *key, const char *val)
{
	const char *p;

	if (!key || !*key)
		return;
	fputs(key, stdout);
	putchar('=');
	if (!val)
		val = "";
	for (p = val; *p; p++) {
		if (*p == '\n' || *p == '\r')
			putchar(' ');
		else
			putchar(*p);
	}
	putchar('\n');
}

static void status_uci_opt(const char *section, const char *opt, char *buf, size_t n)
{
	if (uci_get_wireless(section, opt, buf, n) != 0)
		strncpy(buf, "-", n);
	buf[n - 1] = '\0';
}

static void status_iwinfo_val(const char *ifname, const char *field, char *buf, size_t n)
{
	char cmd[280];
	FILE *fp;

	buf[0] = '\0';
	if (!ifname || !ifname[0] || !have_cmd("iwinfo"))
		return;
	snprintf(cmd, sizeof cmd,
		 "iwinfo %s info 2>/dev/null | sed -n 's/^%s: *//p' | head -1",
		 ifname, field);
	fp = popen(cmd, "r");
	if (!fp)
		return;
	if (fgets(buf, (int)n, fp))
		buf[strcspn(buf, "\r\n")] = '\0';
	pclose(fp);
}

static void status_openwrt_release(char *rel, size_t n)
{
	FILE *fp = fopen("/etc/openwrt_release", "r");
	char line[128];

	rel[0] = '\0';
	if (!fp)
		return;
	while (fgets(line, sizeof line, fp)) {
		if (strncmp(line, "DISTRIB_RELEASE=", 16) == 0) {
			strncpy(rel, line + 16, n - 1);
			rel[n - 1] = '\0';
			rel[strcspn(rel, "'\"\r\n")] = '\0';
			break;
		}
	}
	fclose(fp);
}

static void status_iw_reg(void)
{
	FILE *fp = popen("iw reg get 2>/dev/null", "r");
	char line[256];
	char country[16], flags[128];
	int got_cc = 0;

	status_kv("reg.iw_available", fp ? "1" : "0");
	if (!fp)
		return;

	country[0] = flags[0] = '\0';
	while (fgets(line, sizeof line, fp)) {
		char *p;

		line[strcspn(line, "\r\n")] = '\0';
		if (!strncmp(line, "country ", 8)) {
			p = line + 8;
			if (sscanf(p, "%15[^:]: %127[^\n]", country, flags) >= 1)
				got_cc = 1;
			status_kv("reg.iw_line", line);
			break;
		}
	}
	pclose(fp);
	if (got_cc) {
		status_kv("reg.iw_country", country);
		if (flags[0])
			status_kv("reg.iw_flags", flags);
	}
}

static void status_boot_overlay(void)
{
	char buf[512];
	FILE *fp;

	if (!have_cmd("fw_printenv"))
		return;
	fp = popen("fw_printenv -n bootconf_extra 2>/dev/null", "r");
	if (!fp)
		return;
	buf[0] = '\0';
	if (fgets(buf, sizeof buf, fp))
		buf[strcspn(buf, "\r\n")] = '\0';
	pclose(fp);
	if (buf[0])
		status_kv("boot.bootconf_extra", buf);
	if (stristr(buf, "wifi-be14"))
		status_kv("boot.be14_overlay", "1");
	else
		status_kv("boot.be14_overlay", "0");
}

static void status_hostapd_countries(void)
{
	FILE *fp;
	char line[128];
	int i = 0;

	if (access("/var/run/hostapd", F_OK) != 0)
		return;

	fp = popen("grep -h '^country_code=' /var/run/hostapd-*.conf 2>/dev/null | "
		   "sort -u", "r");
	if (!fp)
		return;

	while (fgets(line, sizeof line, fp)) {
		char key[64];
		char *val = strchr(line, '=');

		line[strcspn(line, "\r\n")] = '\0';
		if (!val)
			continue;
		val++;
		snprintf(key, sizeof key, "runtime.hostapd.%d.country", i++);
		status_kv(key, val);
	}
	pclose(fp);
	{
		char n[16];

		snprintf(n, sizeof n, "%d", i);
		status_kv("runtime.hostapd.count", n);
	}
}

void probe_print_status(void)
{
	char radios[MAX_RADIOS][32];
	char ifaces[16][32];
	char board[128], model[128], release[32], kernel[64], tbuf[64];
	int nr, ni, i, j, iface_idx;
	time_t now = time(NULL);
	struct tm *tm = localtime(&now);

	strftime(tbuf, sizeof tbuf, "%Y-%m-%dT%H:%M:%S", tm);

	puts("# cwc probe status (KEY=VALUE, channel/regulatory related)");
	status_kv("format", "cwc_status");
	status_kv("schema_version", "1");
	status_kv("timestamp", tbuf);

	read_file_line("/tmp/sysinfo/board_name", board, sizeof board);
	read_file_line("/tmp/sysinfo/model", model, sizeof model);
	read_file_line("/proc/sys/kernel/osrelease", kernel, sizeof kernel);
	status_openwrt_release(release, sizeof release);

	status_kv("system.board_name", board[0] ? board : "-");
	status_kv("system.model", model[0] ? model : "-");
	status_kv("system.openwrt_release", release[0] ? release : "-");
	status_kv("system.kernel", kernel[0] ? kernel : "-");

	status_iw_reg();
	status_boot_overlay();
	status_hostapd_countries();

	nr = list_radios(radios, MAX_RADIOS);
	ni = list_wifi_ifaces(ifaces, 16);
	{
		char n[16];

		snprintf(n, sizeof n, "%d", nr);
		status_kv("radio.count", n);
	}

	for (i = 0; i < nr; i++) {
		char key[STATUS_KEY_LEN];
		char cc[32], ch[32], chlist[256], phy[32], dis[8];
		char htmode[32], hwmode[32], path[128], txp[16];
		char band_raw[32], mhz[16];
		band_t band;
		int chnum;

		band = radio_band(radios[i]);
		radio_band_raw(radios[i], band_raw, sizeof band_raw);

		status_uci_opt(radios[i], "country", cc, sizeof cc);
		status_uci_opt(radios[i], "channel", ch, sizeof ch);
		status_uci_opt(radios[i], "disabled", dis, sizeof dis);
		status_uci_opt(radios[i], "htmode", htmode, sizeof htmode);
		status_uci_opt(radios[i], "hwmode", hwmode, sizeof hwmode);
		status_uci_opt(radios[i], "path", path, sizeof path);
		status_uci_opt(radios[i], "txpower", txp, sizeof txp);
		if (uci_channels_list(radios[i], chlist, sizeof chlist) != 0)
			strcpy(chlist, "-");
		if (radio_uci_phy(radios[i], phy, sizeof phy) != 0)
			strcpy(phy, "-");

		status_key_radio(key, sizeof key, radios[i], "band");
		status_kv(key, band_name(band));
		status_key_radio(key, sizeof key, radios[i], "band_uci");
		status_kv(key, band_raw);
		status_key_radio(key, sizeof key, radios[i], "country");
		status_kv(key, cc);
		status_key_radio(key, sizeof key, radios[i], "channel");
		status_kv(key, ch);
		status_key_radio(key, sizeof key, radios[i], "channels");
		status_kv(key, chlist);
		status_key_radio(key, sizeof key, radios[i], "disabled");
		status_kv(key, dis);
		status_key_radio(key, sizeof key, radios[i], "htmode");
		status_kv(key, htmode);
		status_key_radio(key, sizeof key, radios[i], "hwmode");
		status_kv(key, hwmode);
		status_key_radio(key, sizeof key, radios[i], "path");
		status_kv(key, path);
		status_key_radio(key, sizeof key, radios[i], "txpower");
		status_kv(key, txp);
		status_key_radio(key, sizeof key, radios[i], "phy");
		status_kv(key, phy);

		/* ACS / auto-channel options (absent when single-channel locked) */
		{
			char acs[64];

			if (uci_get_wireless(radios[i], "acs_chan_bias", acs, sizeof acs) == 0) {
				status_key_radio(key, sizeof key, radios[i], "acs_chan_bias");
				status_kv(key, acs);
			}
			if (uci_get_wireless(radios[i], "acs_exclude_dfs", acs, sizeof acs) == 0) {
				status_key_radio(key, sizeof key, radios[i], "acs_exclude_dfs");
				status_kv(key, acs);
			}
			if (uci_get_wireless(radios[i], "country_ie", acs, sizeof acs) == 0) {
				status_key_radio(key, sizeof key, radios[i], "country_ie");
				status_kv(key, acs);
			}
		}

		chnum = atoi(ch);
		if (chnum > 0 && band != BAND_UNKNOWN) {
			snprintf(mhz, sizeof mhz, "%d", channel_to_mhz(band, chnum));
			status_key_radio(key, sizeof key, radios[i], "channel_mhz");
			status_kv(key, mhz);
			{
				const char *rt = channel_regulatory_tag_line(cc, band, chnum);

				if (rt && *rt) {
					while (*rt == ' ')
						rt++;
					status_key_radio(key, sizeof key, radios[i], "regulatory_tag");
					status_kv(key, rt);
				}
			}
			if (band == BAND_5G && channel_requires_cac(band, chnum)) {
				status_key_radio(key, sizeof key, radios[i], "dfs_cac");
				status_kv(key, "1");
			}
			if (band == BAND_6G && is_6g_psc_channel(chnum)) {
				status_key_radio(key, sizeof key, radios[i], "6g_psc");
				status_kv(key, "1");
			}
		}

		/* wifi-iface sections bound to this radio */
		iface_idx = 0;
		for (j = 0; j < ni; j++) {
			char dev[32], ssid[64], mode[16], enc[64], ifname[32];
			char idis[8];

			if (iface_radio(ifaces[j], dev, sizeof dev) != 0 ||
			    strcmp(dev, radios[i]) != 0)
				continue;

			status_uci_opt(ifaces[j], "ssid", ssid, sizeof ssid);
			status_uci_opt(ifaces[j], "mode", mode, sizeof mode);
			status_uci_opt(ifaces[j], "encryption", enc, sizeof enc);
			status_uci_opt(ifaces[j], "disabled", idis, sizeof idis);
			if (uci_get_iface_wireless(ifaces[j], "ifname", ifname, sizeof ifname) != 0)
				ifname[0] = '\0';

			status_key_radio_iface(key, sizeof key, radios[i], ifaces[j], "ssid");
			status_kv(key, ssid);
			status_key_radio_iface(key, sizeof key, radios[i], ifaces[j], "mode");
			status_kv(key, mode);
			status_key_radio_iface(key, sizeof key, radios[i], ifaces[j], "encryption");
			status_kv(key, enc);
			status_key_radio_iface(key, sizeof key, radios[i], ifaces[j], "disabled");
			status_kv(key, idis);
			if (ifname[0]) {
				char live_ch[64], live_freq[64];

				status_key_radio_iface(key, sizeof key, radios[i], ifaces[j], "ifname");
				status_kv(key, ifname);
				status_iwinfo_val(ifname, "Channel", live_ch, sizeof live_ch);
				status_iwinfo_val(ifname, "Frequency", live_freq, sizeof live_freq);
				if (live_ch[0]) {
					status_key_radio_iface(key, sizeof key, radios[i], ifaces[j],
							       "runtime_channel");
					status_kv(key, live_ch);
				}
				if (live_freq[0]) {
					status_key_radio_iface(key, sizeof key, radios[i], ifaces[j],
							       "runtime_frequency");
					status_kv(key, live_freq);
				}
			}
			iface_idx++;
		}
		{
			char n[16];

			snprintf(n, sizeof n, "%d", iface_idx);
			status_key_radio(key, sizeof key, radios[i], "iface.count");
			status_kv(key, n);
		}
	}
}

void probe_run(int summary_only, int verbose)
{
	time_t now = time(NULL);
	struct tm *tm = localtime(&now);
	char tbuf[64];

	strftime(tbuf, sizeof tbuf, "%Y-%m-%d %H:%M:%S", tm);
	puts("");
	puts("================================================================");
	puts("  OpenWrt Wi-Fi hardware probe");
	printf("  %s\n", tbuf);
	puts("================================================================");

	if (summary_only) {
		print_summary();
		return;
	}

	print_system();
	print_uboot_overlay();
	print_regdomain();
	print_kernel_wifi();
	print_uci_wireless();
	print_iw_phy_summary(verbose);
	print_runtime();
	print_dmesg_hints();
	print_cheat_sheet();
	print_summary();
}
