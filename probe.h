#ifndef PROBE_H
#define PROBE_H

/* Wi-Fi hardware / regulatory probe (detect_wifi_hw.sh equivalent) */
void probe_run(int summary_only, int verbose);
void probe_print_status(void);

#endif
