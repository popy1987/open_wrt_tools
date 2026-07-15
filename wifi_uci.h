#ifndef WIFI_UCI_H
#define WIFI_UCI_H

#include "reg.h"

#define MAX_RADIOS 8
#define MAX_PLAN 8

struct plan_item {
	char radio[32];
	int channel;
};

struct app_state {
	char applied_country[8];
	char plan_entries[256];
	struct plan_item plan[MAX_PLAN];
	int plan_count;
	int country_inferred;
	char selected_radio[32];
	int selected_channel;
};

int wifi_uci_init(void);
void wifi_uci_free(void);
struct uci_context *wifi_uci_context(void);

int list_radios(char radios[][32], int max);
band_t radio_band(const char *radio);
int radio_band_raw(const char *radio, char *buf, size_t n);

int uci_get_wireless(const char *radio, const char *opt, char *buf, size_t n);
int set_radio_country(const char *radio, const char *cc);
int set_radio_single_channel(const char *radio, int ch);
int apply_country_reg(const char *cc);
int commit_wireless(void);
int reload_wifi(void);
int do_reboot(void);

int infer_country_for_plan(struct app_state *st, char *out_cc, size_t n);
int stage_wifi_plan(struct app_state *st, const char *cc, int infer);

#endif
