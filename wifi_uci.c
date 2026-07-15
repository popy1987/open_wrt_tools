#include "wifi_uci.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <syslog.h>
#include <unistd.h>

#include <uci.h>

static struct uci_context *ctx;

struct uci_context *wifi_uci_context(void)
{
	return ctx;
}

static void log_msg(const char *fmt, ...)
{
	va_list ap;
	char buf[512];

	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	syslog(LOG_INFO, "%s", buf);
	fputs(buf, stdout);
	fputc('\n', stdout);
}

static void err_msg(const char *fmt, ...)
{
	va_list ap;

	fputs("ERROR: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

static void err_uci(const char *prefix)
{
	char *err = NULL;

	if (!ctx) {
		err_msg("%s (no uci context)", prefix);
		return;
	}
	uci_get_errorstr(ctx, &err, prefix);
	if (err) {
		fprintf(stderr, "ERROR: %s\n", err);
		free(err);
	} else {
		err_msg("%s", prefix);
	}
}

/* system(3) result: 0 ok, -1 on failure (reason already on stderr). */
static int run_cmd(const char *cmd, const char *what)
{
	int st = system(cmd);

	if (st == 0)
		return 0;
	if (st < 0) {
		err_msg("%s: system() failed: %s (cmd: %s)",
			what, strerror(errno), cmd);
		return -1;
	}
	if (WIFEXITED(st)) {
		err_msg("%s failed (exit %d): %s",
			what, WEXITSTATUS(st), cmd);
	} else if (WIFSIGNALED(st)) {
		err_msg("%s killed by signal %d: %s",
			what, WTERMSIG(st), cmd);
	} else {
		err_msg("%s failed (status %d): %s", what, st, cmd);
	}
	return -1;
}

int wifi_uci_init(void)
{
	ctx = uci_alloc_context();
	if (!ctx) {
		err_msg("uci_alloc_context failed (out of memory?)");
		return -1;
	}
	openlog("cwc", LOG_PID, LOG_USER);
	return 0;
}

void wifi_uci_free(void)
{
	if (ctx) {
		uci_free_context(ctx);
		ctx = NULL;
	}
	closelog();
}

static int load_wireless(struct uci_package **pkg)
{
	if (uci_load(ctx, "wireless", pkg) != UCI_OK) {
		err_uci("uci_load wireless");
		return -1;
	}
	return 0;
}

int list_radios(char radios[][32], int max)
{
	struct uci_package *pkg = NULL;
	struct uci_element *e;
	int n = 0;

	if (load_wireless(&pkg) != 0)
		return 0;

	uci_foreach_element(&pkg->sections, e) {
		struct uci_section *s = uci_to_section(e);

		if (strcmp(s->type, "wifi-device") != 0)
			continue;
		if (n >= max)
			break;
		strncpy(radios[n], s->e.name, 31);
		radios[n][31] = '\0';
		n++;
	}
	uci_unload(ctx, pkg);
	return n;
}

int uci_get_wireless(const char *radio, const char *opt, char *buf, size_t n)
{
	struct uci_ptr ptr;
	char path[128];

	snprintf(path, sizeof path, "wireless.%s.%s", radio, opt);
	if (uci_lookup_ptr(ctx, &ptr, path, true) != UCI_OK)
		return -1;
	if (!ptr.o || !ptr.o->v.string)
		return -1;
	strncpy(buf, ptr.o->v.string, n - 1);
	buf[n - 1] = '\0';
	return 0;
}

band_t radio_band(const char *radio)
{
	char raw[64];

	if (uci_get_wireless(radio, "band", raw, sizeof raw) != 0 &&
	    uci_get_wireless(radio, "hwmode", raw, sizeof raw) != 0)
		return BAND_UNKNOWN;
	return normalize_band(raw);
}

int radio_band_raw(const char *radio, char *buf, size_t n)
{
	if (uci_get_wireless(radio, "band", buf, n) == 0)
		return 0;
	if (uci_get_wireless(radio, "hwmode", buf, n) == 0)
		return 0;
	strncpy(buf, "unknown", n);
	return -1;
}

static int uci_set_str(const char *path, const char *val)
{
	struct uci_ptr ptr;
	char buf[256];

	snprintf(buf, sizeof buf, "%s=%s", path, val);
	if (uci_lookup_ptr(ctx, &ptr, buf, true) != UCI_OK) {
		err_uci(buf);
		return -1;
	}
	if (uci_set(ctx, &ptr) != UCI_OK) {
		err_uci(buf);
		return -1;
	}
	return 0;
}

/* Best-effort delete / clear; no stderr on missing options. */
static void uci_try_clear(const char *path)
{
	struct uci_ptr ptr;
	char buf[256];

	snprintf(buf, sizeof buf, "%s=", path);
	if (uci_lookup_ptr(ctx, &ptr, buf, true) != UCI_OK)
		return;
	uci_set(ctx, &ptr);
}

int set_radio_country(const char *radio, const char *cc)
{
	char path[128];

	snprintf(path, sizeof path, "wireless.%s.country", radio);
	if (uci_set_str(path, cc) != 0)
		return -1;
	snprintf(path, sizeof path, "wireless.%s.country_ie", radio);
	uci_try_clear(path);
	return 0;
}

int set_radio_single_channel(const char *radio, int ch)
{
	char path[128], val[16], cmd[256];

	snprintf(path, sizeof path, "wireless.%s.channel", radio);
	snprintf(val, sizeof val, "%d", ch);
	if (uci_set_str(path, val) != 0)
		return -1;

	snprintf(cmd, sizeof cmd,
		 "while uci -q delete wireless.%s.channels 2>/dev/null; do :; done; "
		 "uci add_list wireless.%s.channels=%d",
		 radio, radio, ch);
	if (run_cmd(cmd, "set wireless channels list") != 0)
		return -1;

	snprintf(path, sizeof path, "wireless.%s.acs_chan_bias", radio);
	uci_try_clear(path);
	snprintf(path, sizeof path, "wireless.%s.acs_exclude_dfs", radio);
	uci_try_clear(path);
	return 0;
}

int apply_country_reg(const char *cc)
{
	char cmd[64];

	snprintf(cmd, sizeof cmd, "iw reg set %s", cc);
	if (run_cmd(cmd, "iw reg set") != 0)
		return -1;
	log_msg("Regulatory domain: %s", cc);
	return 0;
}

int commit_wireless(void)
{
	struct uci_package *pkg = NULL;

	log_msg("uci commit wireless ...");
	if (load_wireless(&pkg) != 0)
		return -1;
	if (uci_commit(ctx, &pkg, false) != UCI_OK) {
		err_uci("uci commit wireless");
		uci_unload(ctx, pkg);
		return -1;
	}
	uci_unload(ctx, pkg);
	return 0;
}

int reload_wifi(void)
{
	log_msg("wifi reload ...");
	if (run_cmd("wifi reload", "wifi reload") != 0)
		return -1;
	sleep(3);
	return 0;
}

int do_reboot(void)
{
	log_msg("Rebooting router ...");
	sleep(1);
	if (run_cmd("reboot", "reboot") != 0)
		return -1;
	return 0;
}

int infer_country_for_plan(struct app_state *st, char *out_cc, size_t n)
{
	const char *order[] = { "CN", "JP", "US", "DE", NULL };
	const char **cc;
	int i, ok;

	for (cc = order; *cc; cc++) {
		ok = 1;
		for (i = 0; i < st->plan_count; i++) {
			band_t b = radio_band(st->plan[i].radio);
			if (!channel_allowed_in_country(*cc, b, st->plan[i].channel)) {
				ok = 0;
				break;
			}
		}
		if (ok) {
			strncpy(out_cc, *cc, n - 1);
			out_cc[n - 1] = '\0';
			return 0;
		}
	}
	return -1;
}

static void rebuild_plan_string(struct app_state *st)
{
	int i;
	char *p = st->plan_entries;

	st->plan_entries[0] = '\0';
	for (i = 0; i < st->plan_count; i++) {
		int left = (int)(sizeof st->plan_entries - (size_t)(p - st->plan_entries));
		int w;

		w = snprintf(p, (size_t)left, "%s%s:%d",
			     i ? " " : "", st->plan[i].radio, st->plan[i].channel);
		if (w < 0 || w >= left)
			break;
		p += w;
	}
}

int stage_wifi_plan(struct app_state *st, const char *cc, int infer)
{
	char radios[MAX_RADIOS][32];
	int nr, i;
	char use_cc[8];

	st->country_inferred = 0;
	st->applied_country[0] = '\0';

	if (infer || !cc || !*cc) {
		if (infer_country_for_plan(st, use_cc, sizeof use_cc) != 0) {
			err_msg("cannot infer country for plan (tried CN, JP, US, DE)");
			for (i = 0; i < st->plan_count; i++) {
				band_t b = radio_band(st->plan[i].radio);
				char raw[32];

				radio_band_raw(st->plan[i].radio, raw, sizeof raw);
				err_msg("  plan: %s band=%s (UCI:%s) ch=%d",
					st->plan[i].radio,
					b == BAND_2G ? "2g" :
					b == BAND_5G ? "5g" :
					b == BAND_6G ? "6g" : "?",
					raw, st->plan[i].channel);
			}
			err_msg("hint: pass -c CODE explicitly");
			return -1;
		}
		st->country_inferred = 1;
		log_msg("Country omitted -> inferred: %s (priority CN->JP->US->DE)", use_cc);
	} else {
		strncpy(use_cc, cc, sizeof use_cc - 1);
		use_cc[sizeof use_cc - 1] = '\0';
		for (i = 0; i < st->plan_count; i++) {
			band_t b = radio_band(st->plan[i].radio);
			if (!channel_allowed_in_country(use_cc, b, st->plan[i].channel))
				return -2;
		}
		log_msg("Using explicit country: %s", use_cc);
	}

	strncpy(st->applied_country, use_cc, sizeof st->applied_country - 1);
	if (apply_country_reg(use_cc) != 0)
		return -3;

	nr = list_radios(radios, MAX_RADIOS);
	if (nr <= 0)
		fprintf(stderr,
			"WARNING: no wifi-device sections found in wireless config\n");

	for (i = 0; i < nr; i++) {
		if (set_radio_country(radios[i], use_cc) != 0) {
			err_msg("failed to set country=%s on %s", use_cc, radios[i]);
			return -4;
		}
	}

	for (i = 0; i < st->plan_count; i++) {
		if (set_radio_single_channel(st->plan[i].radio,
					     st->plan[i].channel) != 0) {
			err_msg("failed to lock %s to channel %d",
				st->plan[i].radio, st->plan[i].channel);
			return -5;
		}
		log_msg("Single channel: %s -> ch=%d only (channels=[%d])",
			st->plan[i].radio, st->plan[i].channel,
			st->plan[i].channel);
	}

	rebuild_plan_string(st);
	return 0;
}
