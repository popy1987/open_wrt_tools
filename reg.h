#ifndef REG_H
#define REG_H

#include <stddef.h>

typedef enum {
	BAND_2G,
	BAND_5G,
	BAND_6G,
	BAND_UNKNOWN
} band_t;

band_t normalize_band(const char *raw);
int is_6g_channel_number(int ch, int max);
int country_6g_max_channel(const char *cc);
int is_6g_psc_channel(int ch);
int channel_allowed_in_country(const char *cc, band_t band, int ch);
int infer_country_from_channel(band_t band, int ch, char *out_cc, size_t out_len);
int is_dfs_5g_channel(int ch);
int channel_requires_cac(band_t band, int ch);
int cn_5g_indoor_channel(const char *cc, band_t band, int ch);
int jp_eu_6g_lpi_channel(const char *cc, band_t band, int ch);
int cn_6g_prohibited(const char *cc, band_t band);
int channel_to_mhz(band_t band, int ch);
band_t channel_suggests_band(int ch);
void channel_suggest_text(int ch, band_t radio_band, char *buf, size_t n);
void print_country_matrix(void);
void print_band_channel_ref(const char *cc, band_t band);
void print_channel_regulatory_warnings(const char *cc, band_t band, int ch,
				       const char *radio);
const char *channel_regulatory_tag_line(const char *cc, band_t band, int ch);

#endif
