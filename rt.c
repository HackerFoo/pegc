/* Copyright 2012-2013 Dustin DeWeese
   This file is part of pegc.

    pegc is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    pegc is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with pegc.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <string.h>
#include <assert.h>
#include "rt_types.h"
#include "gen/rt.h"
#include "gen/primitive.h"

// make sure &cells > 255
cell_t cells[1<<16];
cell_t *cells_ptr;
uint8_t alt_cnt = 0;

measure_t measure, saved_measure;

// #define CHECK_CYCLE

bool is_cell(void *p) {
  return p >= (void *)&cells && p < (void *)(&cells+1);
}

bool is_closure(void *p) {
  return is_cell(p) && ((cell_t *)p)->func;
}

bool closure_is_ready(cell_t *c) {
  assert(is_closure(c));
  return !is_marked(c->func);
}

void closure_set_ready(cell_t *c, bool r) {
  assert(is_closure(c));
  c->func = (reduce_t *)set_mark(c->func, !r);
}

/* propagate alternatives down to root of expression tree */
/* [TODO] distribute splitting into args */
/* or combine reduce and split, so each arg is reduced */
/* just before split, and alts are stored then zeroed */
cell_t *closure_split(cell_t *c, unsigned int s) {
  cell_t *split(cell_t *c, cell_t *t, unsigned int mask, unsigned int s) {
    unsigned int i, j;
    cell_t *p = t, *cn;

    for(i = mask;
	i != 0;
	i = (i - 1) & mask) {
      cn = closure_alloc(s);
      cn->func = c->func;
      //cn->n = c->n;
      for(j = 0; j < s; j++) {
	cn->arg[j] = (1<<j) & i ?
	  c->arg[j]->alt :
	  mark_ptr(c->arg[j]);
      }
      cn->alt = p;
      p = cn;
    }
    return p;
  }

  assert(is_closure(c) && closure_is_ready(c));
  unsigned int i, n = 0;
  unsigned int alt_mask = 0;

  cell_t *p = c->alt;

  /* calculate a mask for args with alts */
  i = s;
  alt_mask = 0;
  while(i--) {
    alt_mask <<= 1;
    if(is_marked(c->arg[i])) {
      /* clear marks as we go */
      c->arg[i] = clear_ptr(c->arg[i]);
    } else if(c->arg[i] &&
	      c->arg[i]->alt) {
      alt_mask |= 1;
      n++;
    }
  }

  if(n == 0) return p;

  p = split(c, c->alt, alt_mask, s);

  unsigned int nref_alt_alt = 1 << (n-1);
  unsigned int nref_alt = nref_alt_alt - 1;
  unsigned int nref_noalt = (1 << n) - 1;
  for(i = 0; i < s; i++) {
    if((1<<i) & alt_mask) {
      c->arg[i]->alt->n += nref_alt_alt;
      c->arg[i]->n += nref_alt;
    } else {
      c->arg[i]->n += nref_noalt;
    }
  }

  return p;
}

cell_t *closure_split1(cell_t *c, int n) {
  if(!c->arg[n]->alt) return c->alt;
  cell_t *a = copy(c);
  a->arg[n] = 0;
  ref_args(a);
  a->arg[n] = ref(c->arg[n]->alt);
  a->n = 0;
  a->alt = c->alt;
  return a;
}

bool reduce(cell_t *c) {
  c = clear_ptr(c);
  if(!c) return false;
  assert(is_closure(c) &&
	 closure_is_ready(c));
  measure.reduce_cnt++;
  return c->func(c);
}

cell_t *cells_next() {
  cell_t *p = cells_ptr;
  assert(is_cell(p) && !is_closure(p) && is_cell(cells_ptr->next));
  cells_ptr = cells_ptr->next;
  return p;
}

#ifdef CHECK_CYCLE
bool check_cycle() {
  int i = 0;
  cell_t *start = cells_ptr, *ptr = start;
  while(ptr->next != start) {
    if(i > LENGTH(cells)) return false;
    i++;
    assert(is_cell(ptr->next->next));
    ptr = ptr->next;
  }
  return true;
}
#else
bool check_cycle() {
  return true;
}
#endif

void cells_init() {
  int i;
  
  // zero the cells
  bzero(&cells, sizeof(cells));

  // set up doubly-linked pointer ring
  for(i = 0; i < LENGTH(cells); i++) {
    cells[i].prev = &cells[i-1];
    cells[i].next = &cells[i+1];
  }
  cells[0].prev = &cells[LENGTH(cells)-1];
  cells[LENGTH(cells)-1].next = &cells[0];

  cells_ptr = &cells[0];
  assert(check_cycle());
  alt_cnt = 0;
}

void cell_alloc(cell_t *c) {
  assert(is_cell(c) && !is_closure(c));
  cell_t *prev = c->prev;
  assert(is_cell(prev) && !is_closure(prev));
  cell_t *next = c->next;
  assert(is_cell(next) && !is_closure(next));
  if(cells_ptr == c) cells_next();
  prev->next = next;
  next->prev = prev;
  measure.alloc_cnt++;
  if(++measure.current_alloc_cnt > measure.max_alloc_cnt)
    measure.max_alloc_cnt = measure.current_alloc_cnt;
  assert(check_cycle());
}

cell_t *closure_alloc(int args) {
  return closure_alloc_cells(calculate_cells(args));
}

cell_t *closure_alloc_cells(int size) {
  cell_t *ptr = cells_next(), *c = ptr;
  cell_t *mark = ptr;
  int cnt = 0;

  // search for contiguous chunk
  while(cnt < size) {
    if(is_cell(ptr) && !is_closure(ptr)) {
      cnt++;
      ptr++;
    } else {
      cnt = 0;
      c = ptr = cells_next();
      assert(c != mark);
    }
  }

  // remove the found chunk
  int i;
  for(i = 0; i < size; i++) {
    cell_alloc(&c[i]);
  }

  bzero(c, sizeof(cell_t)*size);
  assert(check_cycle());
  return c;
}

#define calc_size(f, n)				\
  ((sizeof(cell_t)				\
    + ((intptr_t)&(((cell_t *)0)->f[n]) - 1))	\
   / sizeof(cell_t))

int calculate_cells(int n) {
  return calc_size(arg, n);
}

int calculate_list_size(int n) {
  return calc_size(ptr, n);
}

int calculate_val_size(int n) {
  return calc_size(val, n);
}

int closure_cells(cell_t *c) {
  return calculate_cells(closure_args(c));
}

void cell_free(cell_t *c) {
  c->func = 0;
  c->next = cells_ptr;
  c->prev = cells_ptr->prev;
  cells_ptr->prev = c;
  c->prev->next = c;
}

void closure_shrink(cell_t *c, int s) {
  int i, size = closure_cells(c);
  if(size > s) {
    assert(is_closure(c));
    for(i = s; i < size; i++) {
      c[i].func = 0;
      c[i].prev = &c[i-1];
      c[i].next = &c[i+1];
    }
    c[s].prev = cells_ptr->prev;
    cells_ptr->prev->next = &c[s];
    c[size-1].next = cells_ptr;
    cells_ptr->prev = &c[size-1];
    assert(check_cycle());
    measure.current_alloc_cnt -= size - s;
  }
}

void closure_free(cell_t *c) {
  closure_shrink(c, 0);
}

bool func_reduced(cell_t *c) {
  assert(is_closure(c));
  measure.reduce_cnt--;
  return c->type != T_FAIL;
}

cell_t *val(intptr_t x) {
  cell_t *c = closure_alloc(1);
  c->func = func_reduced;
  c->type = T_INT;
  c->val[0] = x;
  c->val_size = 1;
  return c;
}

cell_t *vector(uint32_t n) {
  cell_t *c = closure_alloc_cells(calculate_val_size(n));
  c->func = func_reduced;
  c->type = T_INT;
  c->val_size = n;
  return c;
}

cell_t *quote(cell_t *x) {
  cell_t *c = closure_alloc(1);
  c->func = func_reduced;
  c->ptr[0] = x;
  return c;
}

cell_t *empty_list() {
  return quote(0);
}

cell_t *ind(cell_t *x) {
  cell_t *c = val((intptr_t)x);
  c->type = T_INDIRECT;
  return c;
}

cell_t *append(cell_t *a, cell_t *b) {
  int n = list_size(b);
  int n_a = list_size(a);
  cell_t *e = expand(a, n);
  while(n--) e->ptr[n + n_a] = b->ptr[n];
  return e;
}

cell_t *compose(cell_t *a, cell_t *b) {
  b = protect(b);
  int n = list_size(b);
  int n_a = list_size(a);
  int i = 0;
  if(n) {
    cell_t *l = b->ptr[n-1];
    while(!closure_is_ready(l) && i < n_a) {
      arg(l, ref(a->ptr[i++]));
    }
  }
  cell_t *e = expand(b, n_a - i);
  int j;
  for(j = n; i < n_a; ++i, ++j) {
    e->ptr[j] = ref(a->ptr[i]);
  }
  unref(a);
  return e;
}

cell_t *expand(cell_t *c, unsigned int s) {
  int n = closure_args(c);
  int cn_p = calculate_cells(n);
  int cn = calculate_cells(n + s);
  if(c && !c->n && cn == cn_p) {
    return c;
  } else {
    /* copy */
    cell_t *new = closure_alloc_cells(cn);
    memcpy(new, c, cn_p * sizeof(cell_t));
    ref_ptrs(new);
    unref(c);
    return new;
  }
}

bool is_reduced(cell_t *c) {
  return c->func == func_reduced;
}

// max offset is 255
bool is_offset(cell_t *c) {
  return !((intptr_t)c & ~0xff);
}

// args must be >= 1
cell_t *func(reduce_t *f, int args) {
  assert(args >= 0);
  cell_t *c = closure_alloc(args);
  assert(c->func == 0);
  c->func = f;
  c->arg[0] = (cell_t *)(intptr_t)(args - 1);
  closure_set_ready(c, false);
  return c;
}

#define cell_offset(f) ((cell_t **)&(((cell_t *)0)->f))

int list_size(cell_t *c) {
  int i = 0;
  cell_t **p = c->ptr;
  while(*p && is_cell(*p)) {
    ++p;
    ++i;
  }
  return i;
}

int val_size(cell_t *c) {
  return c->val_size;
}

int closure_args(cell_t *c) {
  assert(is_closure(c));
  cell_t **p = c->arg;
  int n = 0;
  if(c->func == func_alt) return 2;
  if(is_reduced(c)) {
    if(is_list(c))
      return list_size(c) +
	cell_offset(ptr[0]) -
	cell_offset(arg[0]);
    else return val_size(c) +
	   cell_offset(val[0]) -
	   cell_offset(arg[0]);
  } else if(is_offset(*p)) {
    intptr_t o = (intptr_t)(*p) + 1;
    p += o;
    n += o;
  }
  /* args must be cells */
  while(is_cell(*p)) {
    p++;
    n++;
  }
  return n;
}

int closure_next_child(cell_t *c) {
  assert(is_closure(c));
  return is_offset(c->arg[0]) ? (intptr_t)c->arg[0] : 0;
}

/* protect against destructive modification */
cell_t *protect(cell_t *c) {
  if(c->n) {
    --c->n;
    c = dup(c);
    c->n = 0;
  }
  return c;
}

/* arg is destructive to c */
void arg(cell_t *c, cell_t *a) {
  assert(is_closure(c) && is_closure(a));
  assert(!closure_is_ready(c));
  int i = closure_next_child(c);
  if(!is_cell(c->arg[i])) {
    c->arg[0] = (cell_t *)(intptr_t)(i - (closure_is_ready(a) ? 1 : 0));
    c->arg[i] = a;
    if(i == 0) closure_set_ready(c, closure_is_ready(a));
  } else {
    arg(c->arg[i], a);
    if(closure_is_ready(c->arg[i])) {
      if(i == 0) closure_set_ready(c, true);
      else --*(intptr_t *)&c->arg[0]; // decrement offset
    }
  }
}

cell_t *copy(cell_t *c) {
  int size = closure_cells(c);
  cell_t *new = closure_alloc_cells(size);
  memcpy(new, c, size * sizeof(cell_t));
  return new;
}

cell_t *ref_args(cell_t *c) {
  int n = closure_args(c);
  while(n--)
    if(is_closure(c->arg[n]))
      c->arg[n] = ref(c->arg[n]);
  return c;
}

cell_t *ref_list(cell_t *c) {
  int n = list_size(c);
  while(n--)
    if(is_closure(c->ptr[n]))
      c->ptr[n] = ref(c->ptr[n]);
  return c;
}

cell_t *ref_all(cell_t *c) {
  c->alt = ref(c->alt);
  if(!is_reduced(c)) ref_args(c);
  else if(is_list(c)) ref_list(c);
  return c;
}

bool to_ref(cell_t *c, cell_t *r, unsigned int size, bool s) {
  if(!s) {
    memcpy(c, r, sizeof(cell_t));
    c->func = func_reduced;
    c->type = T_FAIL;
    unref(r);
    return false;
  } else if(size <= closure_cells(c)) {
    int n = c->n;
    memcpy(c, r, sizeof(cell_t) * size);
    c->n = n;
    shallow_unref(r);
  } else {
    c->type = T_INDIRECT;
    c->val[0] = (intptr_t)r;
  }
  c->func = func_reduced;
  return true;
}

cell_t *ref(cell_t *c) {
  return(refn(c, 1));
}

cell_t *refn(cell_t *c, unsigned int n) {
  if(c) {
    assert(is_closure(c));
    c->n += n;
  }
  return c;
}

cell_t *dup(cell_t *c) {
  assert(is_closure(c));
  if(closure_is_ready(c)) {
    if(is_list(c)) {
      int n = list_size(c);
      cell_t *l = c->ptr[n-1];
      if(!closure_is_ready(l)) {
	cell_t *tmp = copy(c);
	int i;
	for(i = 0; i < n-1; ++i)
	  tmp->ptr[i] = ref(tmp->ptr[i]);
	tmp->ptr[n-1] = dup(tmp->ptr[n-1]);
	return tmp;
      }
    }
    return ref(c);
  }
  int args = closure_args(c);
  cell_t *tmp = copy(c);
  /* c->arg[<i] are empty and c->arg[>i] are ready, *
   * so no need to copy                             *
   * c->arg[i] only needs copied if filled          */
  int i = closure_next_child(c);
  if(is_closure(c->arg[i])) tmp->arg[i] = dup(c->arg[i]);
  while(++i < args) tmp->arg[i] = ref(c->arg[i]);
  return tmp;
}

bool is_nil(cell_t *c) {
  return !c;
}

void *clear_ptr(void *p) {
  return (void *)((intptr_t)p & ~1);
}

void *mark_ptr(void *p) {
  return (void *)((intptr_t)p | 1);
}

void *set_mark(void *p, bool f) {
  return f ? mark_ptr(p) : clear_ptr(p);
}

bool is_marked(void *p) {
  return (intptr_t)p & 1;
}

bool is_list(cell_t *c) {
  return c->func == func_reduced &&
    (c->type == 0 || c->type > 255);
}

void unref(cell_t *c) {
  //return;
  if(is_closure(c)) {
    //printf("UNREF(%d) to %d\n", (int)(c - &cells[0]), c->n);
    if(!c->n) {
      if(is_reduced(c)) {
	if(c->type == T_INDIRECT) {
	  unref((cell_t *)c->val[0]);
	} else if(is_list(c)) {
	  int n = list_size(c);
	  while(n) unref(c->ptr[--n]);
	}
	unref(c->alt);
      }
      closure_free(c);
    }
    else --c->n;
  }
}

void shallow_unref(cell_t *c) {
  if(is_closure(c)) {
    if(c->n) closure_free(c);
    else --c->n;
  }
}

/* TODO: needs more work */
void drop(cell_t *c) {
  if(!is_closure(c)) return;
  if(!c->n) {
    if(is_reduced(c)) {
      if(c->type == T_INDIRECT) {
	drop((cell_t *)c->val[0]);
      } else if(is_list(c)) {
	int n = list_size(c);
	while(n) drop(c->ptr[--n]);
      }
      drop(c->alt);
    } else {
      int i = closure_args(c);
      while(i) drop(c->arg[--i]);
    }
    closure_free(c);
  } else --c->n;
}

#define APPEND(field)				\
  do {						\
    if(!a) return b;				\
    if(!b) return a;				\
						\
    cell_t *p = a, *r;				\
    while((r = p->field)) p = r;		\
    p->field = b;				\
    return a;					\
  } while(0)

cell_t *conc_alt(cell_t *a, cell_t *b) { APPEND(alt); }
/* append is destructive to a! */
//cell_t *append(cell_t *a, cell_t *b) { APPEND(next); }

cell_t *dep(cell_t *c) {
  cell_t *n = closure_alloc(1);
  n->func = func_dep;
  n->arg[0] = c;
  return n;
}

bool func_dep(cell_t *c) {
  /* rely on another cell for reduction */
  /* don't need to unref arg, handled by other function */
  cell_t *p = c->arg[0];
  c->arg[0] = 0;
  return reduce(p) && c->func(c);
}
/*
void copy_val(cell_t *dest, unsigned int size, cell_t *src) {
  src = data(src);
  if(is_list(src)) {
    dest->ptr = ref(src->ptr);
  } else {
    dest->type = src->type;
    dest->val = src->val;
  }
}
*/

void ref_ptrs(cell_t *c) {
  if(is_list(c)) {
    int i, n = list_size(c);
    for(i = 0; i < n; i++)
      c->ptr[i] = ref(c->ptr[i]);
  }
}

bool is_dep(cell_t *c) {
  return c->func == func_dep;
}

cell_t *data(cell_t *c) {
  if(!c->type == T_INDIRECT)
    return c;
  else return (cell_t *)c->val[0];
}

// *** fix arg()'s
cell_t *compose_expand(cell_t *a, unsigned int n, cell_t *b) {
  assert(is_closure(a) &&
	 is_closure(b) && is_list(b));
  assert(n);
  int bs = list_size(b);
  if(bs) {
    cell_t *l = b->ptr[bs-1];
    while(n-1 && !closure_is_ready(l)) {
      cell_t *d = dep(ref(a));
      arg(l, d);
      arg(a, ref(d));
      --n;
    }
    if(!closure_is_ready(l)) {
      arg(l, a);
      return b;
    }
  }

  b = expand(b, n);
  --n;
  b->ptr[bs+n] = a;

  while(n) {
    --n;
    cell_t *d = dep(ref(a));
    b->ptr[bs+n] = d;
    arg(a, ref(d));
  }
  return b;
}

cell_t *pushl_val(intptr_t x, cell_t *c) {
  int n = c->val_size++;
  c = expand(c, 1);
  c->val[n] = x;
  return c;
}

/* b is a list, a is a closure */
cell_t *pushl(cell_t *a, cell_t *b) {
  assert(is_closure(a) &&
	 is_closure(b) && is_list(b));

  int n = list_size(b);
  if(n) {
    cell_t *l = b->ptr[n - 1];
    if(!closure_is_ready(l)) {
      arg(l, a);
      return b;
    }
  }

  cell_t *e = expand(b, 1);
  e->ptr[n] = a;
  return e;
}

#define BM_SIZE (sizeof(intptr_t) * 4)

intptr_t bm(int k, int v) {
  assert(k < BM_SIZE);
  return ((intptr_t)1 << (k + BM_SIZE)) |
    (((intptr_t)v & 1) << k);
}

uintptr_t bm_intersect(uintptr_t a, uintptr_t b) {
  return a & b;
}
  
uintptr_t bm_union(uintptr_t a, uintptr_t b) {
  return a | b;
}
  
uintptr_t bm_conflict(uintptr_t a, uintptr_t b) {
  return ((a & b) >> BM_SIZE) &
    ((a ^ b) & (((intptr_t)1<<BM_SIZE)-1));
}

uintptr_t bm_overlap(uintptr_t a, uintptr_t b) {
  return a | (b & (a >> BM_SIZE));
}

void set_bit(uint8_t *m, unsigned int x) {
  m[x >> 3] |= 1 << (x & 7);
}

void clear_bit(uint8_t *m, unsigned int x) {
  m[x >> 3] &= ~(1 << (x & 7));
}

bool check_bit(uint8_t *m, unsigned int x) {
  return m[x >> 3] & (1 << (x & 7));
}
cell_t *collect;

/* reassemble a fragmented cell */
/*
bool func_collect(cell_t *c) {
  collect->alt = c->alt;
  collect->n = c->n;
  collect->func = (reduce_t *)c->arg[0];
  cell_t **dest = collect->arg;
  cell_t **src = c->arg+1;
  cell_t *prev = 0;
  int n = sizeof(c->arg)-1;
  do {
    while(--n > 0) {
      *dest++ = *src++;
    }
    src = (cell_t **)*src;
    cell_free(prev);
    prev = (cell_t *)src;
    n = sizeof(cell_t)-1;
    src++;
  } while(src);
  bool b = reduce(collect);
  return to_ref(c, collect, b);
}
*/
/*
cell_t *func2(reduce_t *f, unsigned int args, unsigned int out) {
  assert(out >= 1);
  cell_t *c, *p, *n = func(f, args + out - 1);
  if(out > 1) {
    --out;
    c = p = dep(n);
    arg(n, ref(p));
    while(--out) {
      p->next = dep(ref(n));
      p = p->next;
      arg(n, ref(p));
    }
    p->next = n;
  } else {
    c = n;
  }
  return c;
}
*/
void *lookup(void *table, unsigned int width, unsigned int rows, const char *key) {
  unsigned int low = 0, high = rows, pivot;
  int c;
  void *entry, *ret = 0;
  int key_length = strnlen(key, width);
  while(high > low) {
    pivot = low + ((high - low) >> 1);
    entry = table + width * pivot;
    c = strncmp(key, entry, key_length);
    if(c == 0) {
      /* keep looking for a lower key */
      ret = entry;
      high = pivot;
    } else if(c < 0) high = pivot;
    else low = pivot + 1;
  }
  return ret;
}

cell_t *get(cell_t *c) {
  if(c->type == T_INDIRECT)
    return get((cell_t *)c->val[0]);
  else return c;
}
