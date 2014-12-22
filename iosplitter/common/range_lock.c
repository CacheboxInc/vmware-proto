#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <string.h>

#include "range_lock.h"

#define SWAP(a, b)			\
	({				\
	         typeof(a) _t = a;	\
	         a = b;			\
	         b = _t;		\
	 })


/*
 * RB Properties
 *
 * 1. A node is either red or black.
 * 2. The root is black.
 * 3. All leaves (NULL) are black.
 * 4. Every red node must have two black child nodes.
 * 5. Every path from a given node to any of its descendant leaves contains the
 *    same number of black nodes.
 */

static inline int check_overlap(unsigned long long s1, unsigned long long e1,
		unsigned long long s2, unsigned long long e2)
{
        if (s1 > e1) {
                SWAP(s1, e1);
        }

        if (s2 > e2) {
                SWAP(s2, e2);
        }

        if ((s1 >= s2 && s1 <= e2) || (e1 >= s2 && e1 <= e2)) {
                return (1);
        }

        if ((s2 >= s1 && s2 <= e1) || (e2 >= s1 && e2 <= e1)) {
                return (1);
        }
        return (0);
}

static inline struct rb_node *new_node(void)
{
	struct rb_node *n;

	n = malloc(sizeof(*n));
	if (n == NULL) {
		return (NULL);
	}

	n->parent	= NULL;
	n->left		= NULL;
	n->right	= NULL;
	n->color	= RED;
	n->start	= 0;
	n->end		= 0;
	memset(&n->rendez, 0, sizeof(n->rendez));
	return (n);
}

static inline void free_node(struct rb_node *node)
{
	free(node);
}

static inline struct rb_node *grand_parent(struct rb_node *n)
{
	if (n == NULL || n->parent == NULL) {
		return (NULL);
	}

	return (n->parent->parent);
}

static inline struct rb_node *uncle(struct rb_node *n)
{
	struct rb_node *gp = grand_parent(n);

	if (gp == NULL) {
		return (NULL);
	}

	if (gp->left == n->parent) {
		return (gp->right);
	}
	return (gp->left);
}

static void rotate_left(struct rb_root *root, struct rb_node *node)
{
	struct rb_node *nr = node->right;
	struct rb_node *np = node->parent;

	assert(nr != NULL);
	assert(nr->parent == node);

	node->right = nr->left;
	nr->left    = node;

	if (np == NULL) {
		assert(root->node == node);
		root->node = nr;
	} else {
		if (np->left == node) {
			np->left = nr;
		} else {
			np->right = nr;
		}
	}

	node->parent = nr;
	nr->parent   = np;
	if (node->right != NULL) {
		node->right->parent = node;
	}
}

static void rotate_right(struct rb_root *root, struct rb_node *node)
{
	struct rb_node *nl = node->left;
	struct rb_node *np = node->parent;

	assert(nl != NULL);
	assert(nl->parent == node);

	node->left = nl->right;
	nl->right  = node;

	if (np == NULL) {
		assert(root->node == node);
		root->node = nl;
	} else {
		if (np->left == node) {
			np->left = nl;
		} else {
			np->right = nl;
		}
	}

	node->parent = nl;
	nl->parent   = np;
	if (node->left != NULL) {
		node->left->parent = node;
	}
}

static inline void rb_transplant(struct rb_root *root, struct rb_node *u,
		struct rb_node *v)
{
	assert(u    != NULL);
	assert(root != NULL);

	if (u->parent == NULL) {
		assert(root->node == u);
		root->node = v;
	} else if (u == u->parent->left) {
		u->parent->left = v;
	} else {
		u->parent->right = v;
	}

	if (v != NULL) {
		v->parent = u->parent;
	}
}

static inline struct rb_node *rb_minimum(struct rb_root *root,
		struct rb_node *node)
{
	struct rb_node *n;

	if (root == NULL || root->node == NULL || node == NULL) {
		return NULL;
	}

	n = node;
	while (n->left != NULL) {
		n = n->left;
	}

	return (n);
}

/*
 * Search
 */
static struct rb_node *__rb_search(struct rb_node *n, unsigned long long s,
		unsigned long long e)
{
	if (n == NULL) {
		return (NULL);
	}

	if (check_overlap(n->start, n->end, s, e)) {
		return (n);
	} else if (n->start > e) {
		return (__rb_search(n->left, s, e));
	} else if (n->end < s) {
		return (__rb_search(n->right, s, e));
	}
	assert(0);
}

static inline struct rb_node *rb_search(struct rb_root *root,
		unsigned long long s, unsigned long long e)
{
	if (root == NULL || root->node == NULL) {
		return (NULL);
	}

	if (s > e) {
		SWAP(s, e);
	}
	return (__rb_search(root->node, s, e));
}

static int rb_insert_fixup(struct rb_root *root, struct rb_node *node)
{
	struct rb_node *z;
	struct rb_node *p;
	struct rb_node *u;
	struct rb_node *g;

	if (root == NULL || node == NULL) {
		return (-1);
	}

	z = node;
	p = node->parent;
	while (p != NULL && p->color == RED && z->color == RED) {
		u = uncle(z);
		g = grand_parent(z);

		if (u != NULL && u->color == RED) {
			assert(g != NULL);

			p->color = BLACK;
			u->color = BLACK;
			g->color = RED;
			z        = g;
			p        = z->parent;
			continue;
		}

		/*
		 * NULL uncle should be treated as black uncle. Infact all NULL
		 * nodes should be considered as black nodes.
		 */
		assert(u == NULL || u->color == BLACK);
		assert(g != NULL);

		if (p == g->left) {
			if (p->right == z) {
				z = p;
				rotate_left(root, z);
			}
			g                = grand_parent(z);
			z->parent->color = BLACK;
			g->color         = RED;
			rotate_right(root, g);
			p = z->parent;
		} else {
			if (p->left == z) {
				z = p;
				rotate_right(root, z);
			}
			g                = grand_parent(z);
			z->parent->color = BLACK;
			g->color         = RED;
			rotate_left(root, g);
			p = z->parent;
		}
	}

	root->node->color = BLACK;
	return (0);
}

static struct rb_node *rb_insert(struct rb_root *root, unsigned long long s,
		unsigned long long e)
{
	struct rb_node *n  = root->node;
	struct rb_node *t  = NULL;
	struct rb_node *n1 = NULL;

	if (s > e) {
		SWAP(s, e);
	}
	assert(s <= e);

	while (n != NULL) {
		t = n;

		/* check for presence of value */
		if (check_overlap(n->start, n->end, s, e) == 1) {
			/* TODO: let caller know that entry was already added */
			return (n);
		} else if (n->start > s) {
			assert(n->end > e);
			/* go to left side */
			n = n->left;
			continue;
		} else if (n->end < s) {
			/* go to right side */
			n = n->right;
			continue;
		} else  {
			/* should never happen */
			assert(0);
		}
	}

	/* t is parent node */
	n1 = new_node();
	if (n1 == NULL) {
		return (NULL);
	}

	n1->start	= s;
	n1->end		= e;
	n1->parent	= t;
	n1->color	= RED;
	if (t == NULL) {
		/* root node */
		root->node = n1;
	} else {
		if (s < t->start) {
			/* insert left */
			t->left = n1;
		} else if (t->end < s) {
			/* insert right */
			t->right = n1;
		} else {
			/* should never happen */
			assert(0);
		}
	}
	root->count++;
	rb_insert_fixup(root, n1);
	return (n1);
}

#if 0
/*
 * Destroy complete tree
 */
void rb_destroy(struct rb_root *root)
{
	struct rb_node *n = root->node;
	struct rb_node *t;
	
	if (n == NULL) {
		return;
	}

	while (n != NULL) {
		if (n->left != NULL) {
			n = n->left;
			continue;
		}

		if (n->right != NULL) {
			n = n->right;
			continue;
		}

		t = n->parent;
		if ((t != NULL) && (t->left == n)) {
			t->left = NULL;
		} else if ((t != NULL) && (t->right == n)) {
			t->right = NULL;
		}
		free_node(n);
		n = t;
	}
	root->count = 0;
	root->node  = NULL;
}
#endif

/*
 * Removing a node
 */
static void rb_delete_fixup(struct rb_root *root, struct rb_node *node,
		struct rb_node *parent)
{
	struct rb_node *x;
	struct rb_node *w;

	if (root == NULL || root->node == NULL) {
		return;
	}

	if (node == NULL && parent == NULL) {
		return;
	}

	x = node;
	while ((root->node != x) && (x == NULL || x->color == BLACK)) {
		if (x != NULL) {
			parent = x->parent;
		}

		if (x == parent->left) {
			w = parent->right;
			assert(w != NULL);

			if (w->color == RED) {
				w->color      = BLACK;
				parent->color = RED;
				assert(x == NULL || x->parent == parent);
				rotate_left(root, parent);
				assert(x == NULL || x->parent == parent);
				w = parent->right;
				assert(w != NULL);
			}

			assert(w != NULL);
			if ((w->left == NULL || w->left->color == BLACK) &&
			    (w->right == NULL || w->right->color == BLACK)) {
				w->color = RED;
				x        = parent;
			} else {
				assert(x == NULL || x->parent == parent);
				if (w->right == NULL ||
						w->right->color == BLACK) {
					w->left->color	= BLACK;
					w->color	= RED;
					rotate_right(root, w);
					w = parent->right;
					assert(w != NULL);
				}

				assert(w != NULL);
				w->color         = parent->color;
				parent->color    = BLACK;
				w->right->color  = BLACK;
				rotate_left(root, parent);
				x = root->node;
			}
		} else {
			w = parent->left;
			assert(w != NULL);

			if (w->color == RED) {
				w->color      = BLACK;
				parent->color = RED;
				rotate_right(root, parent);
				w = parent->left;
				assert(w != NULL);
			}

			assert(w != NULL);
			if ((w->left == NULL || w->left->color == BLACK) &&
			    (w->right == NULL || w->right->color == BLACK)) {
				w->color = RED;
				x        = parent;
			} else  {
				assert(x == NULL || x->parent == parent);
				if (w->left == NULL ||
						w->left->color == BLACK) {
					w->right->color	= BLACK;
					w->color	= RED;
					rotate_left(root, w);
					w = parent->left;
					assert(w != NULL);
				}
				assert(w != NULL);
				w->color         = parent->color;
				parent->color = BLACK;
				w->left->color   = BLACK;
				rotate_right(root, parent);
				x = root->node;
			}
		}
	}

	if (x != NULL) {
		x->color = BLACK;
	}

	root->node->color = BLACK;
}

static void rb_delete(struct rb_root *root, struct rb_node *node)
{
	struct rb_node	*z;
	struct rb_node	*y;
	COLOR		y_color;
	struct rb_node	*x;
	struct rb_node	*p;

	if (root == NULL || root->node == NULL || node == NULL) {
		return;
	}

	y	= z = node;
	y_color	= y->color;

	if (z->left == NULL) {
		x = z->right;
		p = z->parent;
		rb_transplant(root, z, z->right);
	} else if (z->right == NULL) {
		x = z->left;
		p = z->parent;
		rb_transplant(root, z, z->left);
	} else {
		assert(z->left != NULL && z->right != NULL);

		y = rb_minimum(root, z->right);
		assert(y->left == NULL);

		y_color	= y->color;
		x	= y->right;
		p	= y->parent;

		if (y->parent == z) {
			assert(z->right == y);
			if (x != NULL) {
				assert(x->parent == y);
				x->parent = y;
			}
			p = y;
		} else {
			rb_transplant(root, y, y->right);
			y->right         = z->right;
			y->right->parent = y;
		}

		rb_transplant(root, z, y);
		y->left		= z->left;
		y->left->parent	= y;
		y->color	= z->color;
	}

	if (y_color == BLACK) {
		rb_delete_fixup(root, x, p);
	}
	root->count--;
}

#if defined(DEBUG)
/*
 * Sorted display
 */
static void __rb_display(struct rb_node *n)
{
	if (n == NULL) {
		return;
	}
	__rb_display(n->left);
	printf("start = %llu, end = %llu\n", n->start, n->end);
	__rb_display(n->right);
}

static void rb_display(struct rb_root *root)
{
	struct rb_node *n = root->node;

	if (n == NULL) {
		return;
	}

	__rb_display(n);
}

static void __rb_verify_parent(struct rb_node *n)
{
	struct rb_node *l;
	struct rb_node *r;

	if (n == NULL) {
		return;
	}

	l = n->left;
	r = n->right;
	n->lh = n->left ? 0 : 1;
	n->rh = n->right? 0 : 1;

	assert(n->start <= n->end);

	if (n->color == RED) {
		assert(l == NULL || l->color == BLACK);
		assert(r == NULL || r->color == BLACK);
	}

	if (l != NULL) {
		if (l->parent != n) {
			printf("n(s=%llu,e=%llu)\n", n->start, n->end);
			printf("l(s=%llu,e=%llu)\n", l->start, l->end);
			printf("lp(s=%llu,e=%llu)\n", l->parent->start, l->parent->end);
			assert(l->parent == n);
		}
		__rb_verify_parent(n->left);

		assert(l->lh == l->rh);
		n->lh = l->lh;
		if (l->color == BLACK) {
			n->lh++;
		}
	}

	if (r != NULL) {
		if (r->parent != n) {
			printf("n(s=%llu,e=%llu)\n", n->start, n->end);
			printf("r(s=%llu,e=%llu)\n", r->start, r->end);
			if (r->parent) {
				printf("rp(s=%llu,e=%llu)\n", r->parent->start, r->parent->end);
			} else {
				printf("rp == NULL\n");
			}
			assert(r->parent == n);
		}
		__rb_verify_parent(n->right);

		assert(r->lh == r->rh);
		n->rh = r->rh;
		if (r->color == BLACK) {
			n->rh++;
		}
	}

	assert(n->lh == n->rh);
}

static void rb_verify_parent(struct rb_root *root)
{
	if (root == NULL || root->node == NULL) {
		return;
	}

	__rb_verify_parent(root->node);
}
#endif

/* Range lock implementation */
struct range *range_lock(struct range_lock *rl, unsigned long long s,
		unsigned long long e)
{
	struct rb_root *root;
	struct rb_node *n;
	struct range   *r;

	root = &rl->root;
	while (1) {
		n = rb_search(root, s, e);
		if (n == NULL) {
			/* not found - NOTE root lock is not released */
			break;
		}

		/* wait till range is freed */
#if defined(EXTRA_PRINTS)
		printf("locked(%llu, %llu), trying to lock(%llu, %llu)\n",
			n->start, n->end, s, e);
#endif
		n->refcount++;
		tasksleep(&n->rendez);
		n->refcount--;
		if (n->refcount == 0) {
			free_node(n);
		}
	}

	/* XXX: root is locked */
	n = rb_insert(root, s, e);
	n->refcount = 1;

	r = malloc(sizeof(*r));
	assert(r != NULL);
	r->node		= n;
	r->start	= n->start;
	r->end		= n->end;

#if defined(DEBUG)
	rb_verify_parent(&rl->root);
#endif
	return (r);
}

void range_unlock(struct range_lock *rl, struct range *r)
{
	struct rb_root *root;
	struct rb_node *n;

	root = &rl->root;
	n    = r->node;

	rb_delete(root, n);	/* remove mapping from tree */

	n->refcount--;
	if (n->refcount == 0) {
		free_node(n);
	} else {
		taskwakeupall(&n->rendez);
	}

#if defined(DEBUG)
	rb_verify_parent(&rl->root);
#endif
	free(r);
}
