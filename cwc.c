/*
 * cwc — Change Wi-Fi Channel (OpenWrt, libuci)
 * Short CLI binary; shell menu: change_wifi_channel.sh
 */
#include "reg.h"
#include "probe.h"
#include "wifi_uci.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int assume_yes;
static int no_reboot;
static struct app_state app;

static void die(const char *msg)
{
	fprintf(stderr, "ERROR: %s\n", msg);
	exit(1);
}

static int confirm(const char *prompt)
{
	char line[64];

	if (assume_yes)
		return 1;
	printf("%s [y/N]: ", prompt);
	if (!fgets(line, sizeof line, stdin))
		return 0;
	return line[0] == 'y' || line[0] == 'Y';
}

static int plan_has_cac(void)
{
	int i;

	for (i = 0; i < app.plan_count; i++) {
		band_t b = radio_band(app.plan[i].radio);
		if (channel_requires_cac(b, app.plan[i].channel))
			return 1;
	}
	return 0;
}

static void print_plan_warnings(const char *cc)
{
	int i;

	puts("");
	printf("=== Regulatory warnings (country=%s) ===\n", cc);
	for (i = 0; i < app.plan_count; i++) {
		band_t b = radio_band(app.plan[i].radio);
		print_channel_regulatory_warnings(cc, b, app.plan[i].channel,
						  app.plan[i].radio);
	}
	if (plan_has_cac())
		puts(">>> DFS: expect CAC silence after wifi reload (1-10 min).");
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

static void show_detail_status(const char *phase)
{
	char radios[MAX_RADIOS][32];
	int nr, i;
	char reg[4096];
	FILE *fp;

	printf("\n================================================================\n");
	printf("  Wi-Fi detailed status — %s\n", phase);
	printf("================================================================\n\n");

	puts("-- [1] Kernel regulatory domain (iw reg get) --");
	fp = popen("iw reg get 2>/dev/null", "r");
	if (fp) {
		while (fgets(reg, sizeof reg, fp))
			fputs(reg, stdout);
		pclose(fp);
	} else {
		puts("(unavailable)");
	}
	puts("");

	puts("-- [2] Per-radio UCI --");
	nr = list_radios(radios, MAX_RADIOS);
	for (i = 0; i < nr; i++) {
		char cc[32], ch[32], raw[32];
		band_t b = radio_band(radios[i]);
		int chnum = 0;
		int mhz = 0;
		const char *tag;

		uci_get_wireless(radios[i], "country", cc, sizeof cc);
		if (uci_get_wireless(radios[i], "channel", ch, sizeof ch) != 0)
			strcpy(ch, "-");
		else
			chnum = atoi(ch);
		radio_band_raw(radios[i], raw, sizeof raw);
		if (chnum > 0)
			mhz = channel_to_mhz(b, chnum);
		tag = chnum > 0 ?
		    channel_regulatory_tag_line(app.applied_country[0] ?
						app.applied_country : cc,
						b, chnum) : "";
		printf("  %s  band=%s (UCI:%s)  country=%s  channel=%s",
		       radios[i], band_name(b), raw, cc, ch);
		if (mhz > 0)
			printf("  (%d MHz)", mhz);
		if (tag && *tag)
			printf("%s", tag);
		putchar('\n');
	}

	if (app.applied_country[0]) {
		printf("\n-- Applied plan: country=%s%s --\n",
		       app.applied_country,
		       app.country_inferred ? " (inferred)" : "");
		for (i = 0; i < app.plan_count; i++)
			printf("    %s -> ch %d (band %s)\n", app.plan[i].radio,
			       app.plan[i].channel,
			       band_name(radio_band(app.plan[i].radio)));
	}
	puts("");
}

static void die_band_mismatch(const char *cc, const char *radio, band_t band,
			      int ch)
{
	char raw[32], sug[64];

	radio_band_raw(radio, raw, sizeof raw);
	channel_suggest_text(ch, band, sug, sizeof sug);
	if (cn_6g_prohibited(cc, band)) {
		fprintf(stderr,
			"ERROR: CN regdb has no 6 GHz Wi-Fi — cannot use ch=%d on %s (band=6g).\n"
			"       Use -c JP, US, or DE for BE14 6G radio tests.\n",
			ch, radio);
		exit(1);
	}
	fprintf(stderr,
		"ERROR: country %s disallows band=%s ch=%d on %s (UCI: %s).\n"
		"       Hint: channel %d suggests %s — verify -i radio (6G ch%d != 5G ch%d).\n",
		cc, band_name(band), ch, radio, raw, ch, sug, ch, ch);
	exit(1);
}

static int plan_has_6g(void)
{
	int i;

	for (i = 0; i < app.plan_count; i++) {
		if (radio_band(app.plan[i].radio) == BAND_6G)
			return 1;
	}
	return 0;
}

static int confirm_6g_if_needed(const char *cc, band_t band, int ch,
				  const char *radio)
{
	if (band != BAND_6G)
		return 1;
	if (cn_6g_prohibited(cc, band))
		return 0;
	print_channel_regulatory_warnings(cc, band, ch, radio);
	if (jp_eu_6g_lpi_channel(cc, band, ch)) {
		puts("");
		return confirm("Continue with 6 GHz LPI indoor channel?");
	}
	return 1;
}

static int confirm_dfs(const char *cc, band_t band, int ch, const char *radio)
{
	if (!channel_requires_cac(band, ch))
		return 1;
	print_channel_regulatory_warnings(cc, band, ch, radio);
	puts("");
	return confirm("Continue with this DFS/CAC channel?");
}

static void need_cmd(const char *cmd)
{
	char buf[128];

	snprintf(buf, sizeof buf, "command -v %s >/dev/null 2>&1", cmd);
	if (system(buf) != 0)
		die("missing command on PATH (install via opkg)");
}

static void print_help(void)
{
	puts(
"Usage:\n"
"  cwc -p                       Full HW probe (detect_wifi_hw)\n"
"  cwc -p -s                    Probe summary only\n"
"  cwc --status                  KEY=VALUE status (machine-readable)\n"
"  cwc -p -v                    Probe + verbose iw phy dumps\n"
"  cwc -i radio1 -n 36           Set channel (infer country)\n"
"  cwc -c CN -i radio1 -n 36 -y\n"
"  cwc -c JP -i radio2 -n 37 -y   # 6G (CN has no 6 GHz)\n"
"  cwc -l | -r | -h\n"
"\n"
"Options:\n"
"  -p, --probe          Wi-Fi hardware / regulatory probe (read-only)\n"
"  --status             Channel/regulatory KEY=VALUE dump (for scripts)\n"
"  -s, --summary        With -p: one-screen summary only\n"
"  -v, --verbose        With -p: include raw iw phy dumps\n"
"  -c, --country CODE   ISO country (optional)\n"
"  -i, --radio NAME     wifi-device section\n"
"  -n, --channel NUM    Single numeric channel\n"
"  -y, --yes            Skip confirmations\n"
"  --no-reboot          Skip reboot prompt\n"
"  -l, --list           Pending/applied status (change workflow)\n"
"  -r, --reference      Country reference table\n");
}

static void finalize(void)
{
	const char *msg = "Commit config and reload Wi-Fi?";

	show_detail_status("Pending (UCI staged, not committed)");
	if (plan_has_cac())
		msg = "Commit and reload Wi-Fi? (DFS/CAC silence expected — see warnings above)";
	else if (plan_has_6g())
		msg = "Commit and reload Wi-Fi? (6 GHz — ensure WPA3 on AP/clients)";
	puts("");
	if (!confirm(msg)) {
		puts("Cancelled. Not committed.");
		exit(0);
	}
	if (commit_wireless() != 0)
		die("uci commit failed");
	if (reload_wifi() != 0)
		die("wifi reload failed");
	show_detail_status("Applied (after commit + wifi reload)");
	if (plan_has_cac()) {
		puts("");
		puts("Post-reload: if SSID is missing, CAC may still be running (1-10 min).");
		puts("  Check: iwinfo ; logread -f");
		puts("");
	}
	if (plan_has_6g()) {
		puts("");
		puts("Post-reload (6 GHz): verify with cwc -p -s");
		puts("  iwinfo <iface> ; ensure encryption is WPA3 on 6G SSID");
		puts("");
	}
	if (no_reboot) {
		puts("Reboot skipped (--no-reboot)");
		return;
	}
	puts("");
	if (!confirm("Reboot router so regdomain/driver settings fully take effect?")) {
		puts("No reboot. Config committed.");
		return;
	}
	do_reboot();
}

static void run_cli(const char *country, int country_set, const char *radio,
		    int channel)
{
	band_t band;
	char use_cc[8];
	int infer = !country_set;
	int rc;

	app.plan_count = 1;
	strncpy(app.plan[0].radio, radio, sizeof app.plan[0].radio - 1);
	app.plan[0].channel = channel;
	band = radio_band(radio);

	if (infer) {
		if (infer_country_from_channel(band, channel, use_cc,
					       sizeof use_cc) != 0)
			die("cannot infer country from channel (order: CN JP US DE)");
	} else {
		strncpy(use_cc, country, sizeof use_cc - 1);
		use_cc[sizeof use_cc - 1] = '\0';
		if (!channel_allowed_in_country(use_cc, band, channel))
			die_band_mismatch(use_cc, radio, band, channel);
	}

	if (!confirm_dfs(use_cc, band, channel, radio))
		die("cancelled: DFS/CAC channel not confirmed");

	if (!confirm_6g_if_needed(use_cc, band, channel, radio))
		die("cancelled: 6 GHz channel not confirmed");

	rc = stage_wifi_plan(&app, country_set ? country : NULL, infer);
	if (rc == -1)
		die("cannot infer country for plan");
	if (rc == -2)
		die_band_mismatch(use_cc, radio, band, channel);
	if (rc == -3)
		die("iw reg set failed (need root?)");

	print_plan_warnings(app.applied_country);
	finalize();
}

int main(int argc, char **argv)
{
	const char *country = NULL;
	const char *radio = NULL;
	int country_set = 0;
	int channel = 0;
	int list_only = 0;
	int probe_mode = 0;
	int probe_summary = 0;
	int probe_verbose = 0;
	int probe_status = 0;
	int i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			print_help();
			return 0;
		}
		if (!strcmp(argv[i], "-r") || !strcmp(argv[i], "--reference")) {
			print_country_matrix();
			return 0;
		}
		if (!strcmp(argv[i], "-p") || !strcmp(argv[i], "--probe")) {
			probe_mode = 1;
			continue;
		}
		if (!strcmp(argv[i], "-s") || !strcmp(argv[i], "--summary")) {
			probe_summary = 1;
			continue;
		}
		if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose")) {
			probe_verbose = 1;
			continue;
		}
		if (!strcmp(argv[i], "--status")) {
			probe_status = 1;
			continue;
		}
		if (!strcmp(argv[i], "-l") || !strcmp(argv[i], "--list")) {
			list_only = 1;
			continue;
		}
		if (!strcmp(argv[i], "-y") || !strcmp(argv[i], "--yes")) {
			assume_yes = 1;
			continue;
		}
		if (!strcmp(argv[i], "--no-reboot")) {
			no_reboot = 1;
			continue;
		}
		if (!strcmp(argv[i], "-c") || !strcmp(argv[i], "--country")) {
			if (++i >= argc)
				die("-c requires argument");
			country = argv[i];
			country_set = 1;
			continue;
		}
		if (!strcmp(argv[i], "-i") || !strcmp(argv[i], "--radio")) {
			if (++i >= argc)
				die("-i requires argument");
			radio = argv[i];
			continue;
		}
		if (!strcmp(argv[i], "-n") || !strcmp(argv[i], "--channel")) {
			if (++i >= argc)
				die("-n requires argument");
			channel = atoi(argv[i]);
			if (channel <= 0)
				die("invalid channel");
			continue;
		}
		die("unknown option (use -h)");
	}

	if (geteuid() != 0)
		die("run as root");

	need_cmd("uci");
	need_cmd("iw");
	need_cmd("wifi");

	if (access("/etc/config/wireless", R_OK) != 0)
		die("/etc/config/wireless not found");

	if (wifi_uci_init() != 0)
		die("uci_alloc_context failed");

	if (probe_status && !(radio && channel)) {
		probe_print_status();
		wifi_uci_free();
		return 0;
	}

	if ((probe_mode || probe_summary || probe_verbose) && !(radio && channel)) {
		probe_run(probe_summary, probe_verbose);
		wifi_uci_free();
		return 0;
	}

	if (list_only) {
		show_detail_status("Current");
		wifi_uci_free();
		return 0;
	}

	if (!radio || !channel) {
		print_help();
		puts("Interactive mode: use change_wifi_channel.sh (shell) for now.");
		puts("Read-only probe: cwc -p   (or -p -s / --status)");
		wifi_uci_free();
		return argc == 1 ? 0 : 1;
	}

	run_cli(country, country_set, radio, channel);
	wifi_uci_free();
	return 0;
}
