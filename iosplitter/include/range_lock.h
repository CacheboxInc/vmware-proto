#if !defined(__RANGE_LOCK_H__)
#define __RANGE_LOCK_H__

#include "libtask/task.h"
//#define DEBUG

typedef enum {
	BLACK,
	RED,
	DUMMY
} COLOR;

struct rb_node {
	struct rb_node		*parent;
	struct rb_node		*left;
	struct rb_node		*right;
	COLOR			color;

	/* Data */
	unsigned long long	start;
	unsigned long long	end;

#if defined(DEBUG)
	/* black height */
	int lh;
	int rh;
#endif

	int			refcount;
	Rendez			rendez;
};

struct rb_root {
	struct rb_node	*node;
	unsigned long	count;
};

struct range {
	unsigned long long	start;
	unsigned long long	end;

	struct rb_node		*node;
};

struct range_lock {
	struct rb_root root;
};

struct range *range_lock(struct range_lock *rl, unsigned long long s,
		unsigned long long e);
void range_unlock(struct range_lock *rl, struct range *r);
#endif
