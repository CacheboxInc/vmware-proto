#include "stats.h"
#include <time.h>
#include <assert.h>
#include "../qemu-queue.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include "dll.h"
#include "except.h"

void init_stats(stats_t *stats)
{
	int rc;

	memset(stats, 0, sizeof(*stats));
	DLL_INIT(&stats->statlist);
	rc = time(&stats->wall_clock);
	ASSERT_SIGN(rc, !=, -1);
}

void deinit_stats(stats_t *stats)
{
	ASSERT_SIGN(DLL_ISEMPTY(&stats->statlist), !=, 0);
}

statinfo_t *stat_create(stats_t *stats, const char *name, stat_type_t type)
{
	statinfo_t *s;
	int        rc;

	s = calloc(1, sizeof(*s));
	ASSERT_PTR(s, !=, NULL);

	switch (type) {
	case COUNTER:
		rc = time(&s->last_move);
		ASSERT_SIGN(rc, !=, -1);
		break;
	case QUEUE:
	case PROFILER:
		break;
	default:
		assert(0);
	}

	s->name = strdup(name);
	s->type = type;
	DLL_INIT(&s->nxt);

	DLL_ADD(&stats->statlist, &s->nxt);
	return s;
}

void stat_delete(statinfo_t *s)
{
	DLL_REM(&s->nxt);
	free(s->name);
	free(s);
}

static void stat_print(statinfo_t *s)
{
	char b[128] = {0};

	switch (s->type) {
	case QUEUE:
		snprintf(b, sizeof(b), "=== queue depth %s avg %d cur %d",
				s->name, s->qd_avg, s->qd_curr);
		break;
	case COUNTER:
		snprintf(b, sizeof(b), "--- counter %s avg %d starts %d",
				s->name, s->c_avg, s->c_starts);
		break;
	case PROFILER:
		snprintf(b, sizeof(b), "### profiler %s avg %f starts %"PRIu64"",
				s->name, s->avg_ns, s->count);
		break;
	default:
		assert(0);
	}

	printf("%s\n", b);
}

void stat_display_toggle(stats_t *stats)
{
	stats->show = !stats->show;
}

void stat_display(stats_t *stats, int show_it)
{
	stats->show = !!show_it;
}

static void update_queue(stats_t *stats, statinfo_t *s)
{
     time_t diff;

     /* update queue depth statistics
	qd_running - is the stats we are accumulating for the last second, after
	             1 second this moves to curr and is the current stat
	qd_curr    - moved from running after normalizing to a second.
        qd_avg     - moving average avg = (2*avg+curr)/3
      */
     if (stats->wall_clock == s->last_move)
	  return;
     diff = difftime(stats->wall_clock, s->last_move);
     assert(diff >= 1);

     //might be more efficient with bit arithmetics
     s->qd_avg = (2*s->qd_avg + s->qd_curr) / 3;

     s->qd_curr = s->qd_running;
}

static void update_counter(stats_t *stats, statinfo_t *s)
{
}

static void update_profiler(stats_t *stats, statinfo_t *s)
{
	if (s->count) {
		s->avg_ns = s->total_ns / s->count;
	}
}

static void update(stats_t *stats, statinfo_t *s)
{
	switch (s->type) {
	case QUEUE:
		update_queue(stats, s);
		break;
	case COUNTER:
		update_counter(stats, s);
		break;
	case PROFILER:
		update_profiler(stats, s);
		break;
	default:
		assert(0);
	}
}

void stats_update_all(stats_t *stats)
{
	time_t curr;
	statinfo_t *cur;
	dll_t *f;

	assert(time(&curr) != -1);
	if (difftime(curr, stats->wall_clock) == 0)
		return;

	stats->wall_clock = curr;
	stats->skip++;
	stats->skip %= 5;
	for (f = DLL_NEXT(&stats->statlist); f != &stats->statlist;
			f = DLL_NEXT(f)) {
		cur = container_of(f, statinfo_t, nxt);
		update(stats, cur);
		if (stats->show) {
			if (stats->skip == 0) {
				stat_print(cur);
			}
		}
	}
}

/* QUEUE */
void stat_qd_issue(statinfo_t *s)
{
	assert(s->type == QUEUE);
	s->qd_running++;
}

void stat_qd_done(statinfo_t *s)
{
	assert(s->type == QUEUE);
	assert(s->qd_running > 0);
	s->qd_running--;
}

/* COUNTER */
void stat_counter_incr(statinfo_t *s, int count)
{
	s->c_starts++;
	s->c_running += count;
	s->c_avg = s->c_running / s->c_starts;
}

/* PROFILER */
void stat_prof_enter(statinfo_t *s)
{
	uint64_t        sec_to_nsec = 1000000000;
	int             rc;
	struct timespec ts;

	rc = clock_gettime(CLOCK_MONOTONIC, &ts);
	assert(rc == 0);

	s->s_ns = ts.tv_sec * sec_to_nsec + ts.tv_nsec;
	s->count++;
}

void stat_prof_exit(statinfo_t *s)
{
	uint64_t        sec_to_nsec = 1000000000;
	int             rc;
	struct timespec ts;
	uint64_t        e_ns;

	rc = clock_gettime(CLOCK_MONOTONIC, &ts);
	assert(rc == 0);

	e_ns = ts.tv_sec * sec_to_nsec + ts.tv_nsec;

	s->total_ns += (e_ns - s->s_ns);
}
