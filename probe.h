#ifndef PROBE_H
#define PROBE_H

/* Wi-Fi hardware / regulatory probe (read-only) */
void probe_run(int summary_only, int verbose);
void probe_print_status(void);

#endif
