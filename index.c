#include <assert.h>
#include "mgpriv.h"
#include "khash.h"
#include "kthread.h"
#include "kvec.h"

#define idx_hash(a) ((a)>>1)
#define idx_eq(a, b) ((a)>>1 == (b)>>1)
KHASH_INIT(idx, uint64_t, uint64_t, 1, idx_hash, idx_eq)
typedef khash_t(idx) idxhash_t;

typedef struct mg_idx_bucket_s {
	mg128_v a;   // (minimizer, position) array
	int32_t n;   // size of the _p_ array
	uint64_t *p; // position array for minimizers appearing >1 times
	void *h;     // hash table indexing _p_ and minimizers appearing once
} mg_idx_bucket_t;

void mg_idx_destroy(mg_idx_t *gi)
{
	uint32_t i;
	if (gi == 0) return;
	if (gi->B) {
		for (i = 0; i < 1U<<gi->b; ++i) {
			free(gi->B[i].p);
			free(gi->B[i].a.a);
			kh_destroy(idx, (idxhash_t*)gi->B[i].h);
		}
		free(gi->B);
	}
	gfa_destroy(gi->g);
	free(gi);
}

/****************
 * Index access *
 ****************/

const uint64_t *mg_idx_get(const mg_idx_t *gi, uint64_t minier, int *n)
{
	int mask = (1<<gi->b) - 1;
	khint_t k;
	mg_idx_bucket_t *b = &gi->B[minier&mask];
	idxhash_t *h = (idxhash_t*)b->h;
	*n = 0;
	if (h == 0) return 0;
	k = kh_get(idx, h, minier>>gi->b<<1);
	if (k == kh_end(h)) return 0;
	if (kh_key(h, k)&1) { // special casing when there is only one k-mer
		*n = 1;
		return &kh_val(h, k);
	} else {
		*n = (uint32_t)kh_val(h, k);
		return &b->p[kh_val(h, k)>>32];
	}
}

/***************
 * Index build *
 ***************/

static void mg_idx_add(mg_idx_t *gi, int n, const mg128_t *a)
{
	int i, mask = (1<<gi->b) - 1;
	for (i = 0; i < n; ++i) {
		mg128_v *p = &gi->B[a[i].x>>8&mask].a;
		kv_push(mg128_t, 0, *p, a[i]);
	}
}

static void worker_post(void *g, long i, int tid)
{
	int n, n_keys;
	size_t j, start_a, start_p;
	idxhash_t *h;
	mg_idx_t *gi = (mg_idx_t*)g;
	mg_idx_bucket_t *b = &gi->B[i];
	if (b->a.n == 0) return;

	// sort by minimizer
	radix_sort_128x(b->a.a, b->a.a + b->a.n);

	// count and preallocate
	for (j = 1, n = 1, n_keys = 0, b->n = 0; j <= b->a.n; ++j) {
		if (j == b->a.n || b->a.a[j].x>>8 != b->a.a[j-1].x>>8) {
			++n_keys;
			if (n > 1) b->n += n;
			n = 1;
		} else ++n;
	}
	h = kh_init(idx);
	kh_resize(idx, h, n_keys);
	b->p = (uint64_t*)calloc(b->n, 8);

	// create the hash table
	for (j = 1, n = 1, start_a = start_p = 0; j <= b->a.n; ++j) {
		if (j == b->a.n || b->a.a[j].x>>8 != b->a.a[j-1].x>>8) {
			khint_t itr;
			int absent;
			mg128_t *p = &b->a.a[j-1];
			itr = kh_put(idx, h, p->x>>8>>gi->b<<1, &absent);
			assert(absent && j == start_a + n);
			if (n == 1) {
				kh_key(h, itr) |= 1;
				kh_val(h, itr) = p->y;
			} else {
				int k;
				for (k = 0; k < n; ++k)
					b->p[start_p + k] = b->a.a[start_a + k].y;
				radix_sort_64(&b->p[start_p], &b->p[start_p + n]); // sort by position; needed as in-place radix_sort_128x() is not stable
				kh_val(h, itr) = (uint64_t)start_p<<32 | n;
				start_p += n;
			}
			start_a = j, n = 1;
		} else ++n;
	}
	b->h = h;
	assert(b->n == (int32_t)start_p);

	// deallocate and clear b->a
	kfree(0, b->a.a);
	b->a.n = b->a.m = 0, b->a.a = 0;
}

int mg_gfa_overlap(const gfa_t *g)
{
	int64_t i;
	for (i = 0; i < g->n_arc; ++i) // non-zero overlap
		if (g->arc[i].ov != 0 || g->arc[i].ow != 0)
			return 1;
	return 0;
}

mg_idx_t *mg_index_gfa(gfa_t *g, int k, int w, int b, int flag, int n_threads)
{
	mg_idx_t *gi;
	mg128_v a = {0,0,0};
	int i;

	if (mg_gfa_overlap(g)) return 0;
	gi = KCALLOC(0, mg_idx_t, 1);
	gi->w = w, gi->k = k, gi->b = b, gi->flag = flag;
	gi->g = g;

	for (i = 0; i < g->n_seg; ++i) {
		gfa_seg_t *s = &g->seg[i];
		a.n = 0;
		mg_sketch(0, s->seq, s->len, w, k, i, flag&MG_I_HPC, &a); // TODO: this can be parallelized
		mg_idx_add(gi, a.n, a.a);
	}
	free(a.a);
	kt_for(n_threads, worker_post, gi, 1<<gi->b);
	return gi;
}

mg_idx_t *mg_index_file(const char *fn, int k, int w, int b, int flag, int n_threads)
{
	gfa_t *g;
	g = gfa_read(fn);
	if (g == 0) return 0;
	return mg_index_gfa(g, k, w, b, flag, n_threads);
}
