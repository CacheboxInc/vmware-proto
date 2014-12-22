#ifndef STATS_H
#define STATS_H

#include <sys/types.h>
#include <assert.h>
#include <time.h>
#include <stdint.h>
#include "dll.h"

typedef struct stats {
	dll_t   statlist;
	time_t  wall_clock;
	int     show;
	int     skip;
} stats_t;

typedef enum {
	QUEUE,
	COUNTER,
	PROFILER,
} stat_type_t;

typedef struct statinfo {
	dll_t nxt;
	char *name;

	stat_type_t type;

	union {
		struct {
			/* que depth */
			int qd_running;
			int qd_curr;
			int qd_avg;
			time_t last_move;
		};

		struct {
			/* counter */
			int c_running;
			int c_starts;
			int c_avg;
		};

		struct {
			/* function profiler */
			uint64_t s_ns;     /* start nanosecond */
			uint64_t total_ns; /* total nanoseconds */
			uint64_t count;
			double   avg_ns;
		};
	};
} statinfo_t;

void init_stats(stats_t *stats);
void deinit_stats(stats_t *stats);
statinfo_t *stat_create(stats_t *stats, const char *name, stat_type_t type);
void stat_delete(statinfo_t *s);
void stats_update_all(stats_t *stats);

void stat_qd_issue(statinfo_t *s);
void stat_qd_done(statinfo_t *s);
void stat_counter_incr(statinfo_t *s, int count);

void stat_display(stats_t *stats, int show);
void stat_display_toggle(stats_t *stats);

#endif /* STATS_H */
