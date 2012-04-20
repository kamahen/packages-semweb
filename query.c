/*  Part of SWI-Prolog

    Author:        Jan Wielemaker
    E-mail:        J.Wielemaker@cs.vu.nl
    WWW:           http://www.swi-prolog.org
    Copyright (C): 2011, VU University Amsterdam

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
RDF-DB query management. This module keeps  track of running queries. We
need this for GC purposes.  In particular, we need to:

    * Find the oldest active generation.
    * Get a signal if all currently active queries have finished.

In addition to queries, this  module   performs  the  necessary logic on
generations:

    * Is an object visible in a query?
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <SWI-Stream.h>
#include <SWI-Prolog.h>
#include <string.h>
#include <assert.h>
#include "rdf_db.h"
#include "query.h"
#include "memory.h"
#include "mutex.h"
#include "buffer.h"

static void	init_query_stack(rdf_db *db, query_stack *qs);


		 /*******************************
		 *	    THREAD DATA		*
		 *******************************/

/* Return the per-thread data for the given DB.  This version uses no
   locks for the common case that the required datastructures are
   already provided.
*/

thread_info *
rdf_thread_info(rdf_db *db, int tid)
{ query_admin *qa = &db->queries;
  per_thread *td = &qa->query.per_thread;
  thread_info *ti;
  size_t idx = MSB(tid);

  if ( !td->blocks[idx] )
  { simpleMutexLock(&qa->query.lock);
    if ( !td->blocks[idx] )
    { size_t bs = BLOCKLEN(idx);
      thread_info **newblock = rdf_malloc(db, bs*sizeof(thread_info*));

      memset(newblock, 0, bs*sizeof(thread_info*));

      td->blocks[idx] = newblock-bs;
    }
    simpleMutexUnlock(&qa->query.lock);
  }

  if ( !(ti=td->blocks[idx][tid]) )
  { simpleMutexLock(&qa->query.lock);
    if ( !(ti=td->blocks[idx][tid]) )
    { ti = rdf_malloc(db, sizeof(*ti));
      memset(ti, 0, sizeof(*ti));
      init_query_stack(db, &ti->queries);
      MemoryBarrier();
      td->blocks[idx][tid] = ti;
      if ( tid > qa->query.thread_max )
	qa->query.thread_max = tid;
    }
    simpleMutexUnlock(&qa->query.lock);
  }

  return ti;
}


gen_t
oldest_query_geneneration(rdf_db *db)
{ int tid;
  gen_t gen = db->snapshots.keep;
  query_admin *qa = &db->queries;
  per_thread *td = &qa->query.per_thread;

  DEBUG(20,
	if ( db->snapshots.keep != GEN_MAX )
	{ char buf[64];
	  Sdprintf("Oldest snapshot gen = %s\n",
		   gen_name(db->snapshots.keep, buf));
	});

  for(tid=1; tid <= qa->query.thread_max; tid++)
  { thread_info **tis;
    thread_info *ti;

    if ( (tis=td->blocks[MSB(tid)]) &&
	 (ti=tis[tid]) )
    { query_stack *qs = &ti->queries;

      if ( qs->top > 0 )
      { query *q = &qs->preallocated[0];

	DEBUG(10,
	      { char buf[20];
		Sdprintf("Thread %d: %d queries; oldest gen %s\n",
			 tid, qs->top, gen_name(q->rd_gen, buf));
	      });

	if ( q->rd_gen < gen )
	  gen = q->rd_gen;
      } else
      { DEBUG(11, Sdprintf("Thread %d: no queries\n", tid));
      }
    }
  }

  return gen;
}



		 /*******************************
		 *	    QUERY-STACK		*
		 *******************************/

static void
preinit_query(rdf_db *db, query_stack *qs, query *q, query *parent, int depth)
{ q->db     = db;
  q->stack  = qs;
  q->parent = q;
  q->depth  = depth;
}


static void
init_query_stack(rdf_db *db, query_stack *qs)
{ int tid = PL_thread_self();
  int i;
  int prealloc = sizeof(qs->preallocated)/sizeof(qs->preallocated[0]);
  query *parent = NULL;

  memset(qs, 0, sizeof(*qs));

  simpleMutexInit(&qs->lock);
  qs->db = db;
  qs->tr_gen_base = GEN_TBASE + tid*GEN_TNEST;
  qs->tr_gen_max  = qs->tr_gen_base + (GEN_TNEST-1);

  for(i=0; i<MSB(prealloc); i++)
    qs->blocks[i] = qs->preallocated;
  for(i=0; i<prealloc; i++)
  { query *q = &qs->preallocated[i];

    preinit_query(db, qs, q, parent, i);
    parent = q;
  }
}


query *
alloc_query(query_stack *qs)
{ int depth = qs->top;
  int b = MSB(depth);

  if ( qs->blocks[b] )
    return &qs->blocks[b][depth];

  simpleMutexLock(&qs->lock);
  if ( !qs->blocks[b] )
  { size_t bytes = BLOCKLEN(b) * sizeof(query);
    query *ql = rdf_malloc(qs->db, bytes);
    query *parent;
    int i;

    memset(ql, 0, bytes);
    ql -= depth;			/* rebase */
    parent = &qs->blocks[b-1][depth-1];
    for(i=depth; i<depth*2; i++)
    { query *q = &ql[depth];
      preinit_query(qs->db, qs, q, parent, i);
      parent = q;
    }
    MemoryBarrier();
    qs->blocks[b] = ql;
  }
  simpleMutexUnlock(&qs->lock);

  return &qs->blocks[b][depth];
}


static void
push_query(query *q)
{ q->stack->top++;
}


static void
pop_query(query *q)
{ q->stack->top--;
}


query *
open_query(rdf_db *db)
{ int tid = PL_thread_self();
  thread_info *ti = rdf_thread_info(db, tid);
  query *q = alloc_query(&ti->queries);

  q->type = Q_NORMAL;
  q->transaction = ti->queries.transaction;
  if ( q->transaction )			/* Query inside a transaction */
  { q->rd_gen = q->transaction->rd_gen;
    q->tr_gen = q->transaction->wr_gen;
    q->wr_gen = q->transaction->wr_gen;
  } else
  { q->rd_gen = db->queries.generation;
    q->tr_gen = GEN_TBASE;
    q->wr_gen = GEN_UNDEF;
  }

  push_query(q);

  return q;
}


query *
open_transaction(rdf_db *db,
		 triple_buffer *added,
		 triple_buffer *deleted,
		 triple_buffer *updated,
		 snapshot *ss)
{ int tid = PL_thread_self();
  thread_info *ti = rdf_thread_info(db, tid);
  query *q = alloc_query(&ti->queries);

  q->type = Q_TRANSACTION;
  q->transaction = ti->queries.transaction;

  if ( ss && ss != SNAPSHOT_ANONYMOUS )
  { int ss_tid = snapshot_thread(ss);
    assert(!ss_tid || ss_tid == tid);

    q->rd_gen = ss->rd_gen;
    q->tr_gen = ss->tr_gen;
  } else if ( q->transaction )		/* nested transaction */
  { q->rd_gen = q->transaction->rd_gen;
    q->tr_gen = q->transaction->wr_gen;
  } else
  { q->rd_gen = db->queries.generation;
    q->tr_gen = ti->queries.tr_gen_base;
  }

  q->wr_gen = q->tr_gen;
  ti->queries.transaction = q;

  init_triple_buffer(added);
  init_triple_buffer(deleted);
  init_triple_buffer(updated);
  q->transaction_data.added = added;
  q->transaction_data.deleted = deleted;
  q->transaction_data.updated = updated;

  push_query(q);

  return q;
}


void
close_query(query *q)
{ pop_query(q);
}


int
empty_transaction(query *q)
{ return ( is_empty_buffer(q->transaction_data.added) &&
	   is_empty_buffer(q->transaction_data.deleted) );
}


		 /*******************************
		 *	     ADMIN		*
		 *******************************/

void
init_query_admin(rdf_db *db)
{ query_admin *qa = &db->queries;

  memset(qa, 0, sizeof(*qa));
  simpleMutexInit(&qa->query.lock);
  simpleMutexInit(&qa->write.lock);
}


		 /*******************************
		 *	    GENERATIONS		*
		 *******************************/

/* lifespan() is true if a lifespan is visible inside a query.

   A lifespan is alive if the query generation is inside it,
   but with transactions there are two problems:

	- If the triple is deleted by a parent transaction it is dead
	- If the triple is created by a parent transaction it is alive
*/

char *
gen_name(gen_t gen, char *buf)
{ static char tmp[24];

  if ( !buf )
    buf = tmp;
  if ( gen == GEN_UNDEF   ) return "GEN_UNDEF";
  if ( gen == GEN_MAX     ) return "GEN_MAX";
  if ( gen == GEN_PREHIST ) return "GEN_PREHIST";
  if ( gen >= GEN_TBASE )
  { int tid = (gen-GEN_TBASE)/GEN_TNEST;
    gen_t r = (gen-GEN_TBASE)%GEN_TNEST;

    if ( r == GEN_TNEST-1 )
      Ssprintf(buf, "T%d+GEN_TNEST", tid);
    else
      Ssprintf(buf, "T%d+%lld", tid, (int64_t)r);
    return buf;
  }
  Ssprintf(buf, "%lld", (int64_t)gen);
  return buf;
}



int
alive_lifespan(query *q, lifespan *lifespan)
{ DEBUG(2,
	{ char b[4][24];

	  Sdprintf("q: rd_gen=%s; tr_gen=%s; t: %s..%s\n",
		   gen_name(q->rd_gen, b[0]),
		   gen_name(q->tr_gen, b[1]),
		   gen_name(lifespan->born, b[2]),
		   gen_name(lifespan->died, b[3]));
	});

  if ( q->rd_gen >= lifespan->born &&
       q->rd_gen <  lifespan->died )
  { if ( is_wr_transaction_gen(q, lifespan->died) &&
	 q->tr_gen >= lifespan->died )
      return FALSE;

    return TRUE;
  } else				/* created/died in transaction */
  { if ( is_wr_transaction_gen(q, lifespan->born) )
    { if ( q->tr_gen >= lifespan->born &&
	   q->tr_gen <  lifespan->died )
	return TRUE;
    }
  }

  return FALSE;
}


		 /*******************************
		 *     TRIPLE MANIPULATION	*
		 *******************************/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
We have three basic triple manipulations:

  - Add triples
  - Delete triples
  - Copy triples to a new indexing schema (due to cloud split/merge)

add_triples() adds an array of  triples   to  the database, stepping the
database generation by 1. Calls to add triples must be synchronized with
other addition calls, but not  with   read  nor  delete operations. This
synchronization is needed because without we   cannot set the generation
for new queries to a proper value.

Hmmm. Really long writes will block  short   ones.  Can we do something?
Introduce a priority, possibly  automatically   on  #triples, less means
higher.

  - If blocked, set a `wants-priority-write'
  - Super checks this and
    1. Numbers already numbered triples into the future
    2. Release lock
    3. Re-acquire lock
    4. If all have been added, find the proper generation,
       renumber all triples and step the generation.

Or, hand them to a separate thread?

  + Only this thread manages the chains: no need for locking.
  - If a thread adds triples, it has to wait.  This means two-way
    communication.  --> probably too high overhead.

What should we do when writing inside a transaction?

	- As long as there is no nested transaction using the new
	  triple, there is little point doing anything.



- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

int
add_triples(query *q, triple **triples, size_t count)
{ rdf_db *db = q->db;
  gen_t gen, gen_max;
  triple **ep = triples+count;
  triple **tp;

					/* pre-lock phase */
  for(tp=triples; tp < ep; tp++)
  { triple *t = *tp;

    if ( t->resolve_pred )
    { t->predicate.r = lookup_predicate(db, t->predicate.u, q);
      t->resolve_pred = FALSE;
    }
  }

					/* locked phase */
  simpleMutexLock(&db->queries.write.lock);
  gen = queryWriteGen(q)+1;
  gen_max = query_max_gen(q);

  for(tp=triples; tp < ep; tp++)
  { triple *t = *tp;

    t->lifespan.born = gen;
    t->lifespan.died = gen_max;
    if ( link_triple(db, t, q) )
    { if ( q->transaction )
	buffer_triple(q->transaction->transaction_data.added, t);
    } else
    { *tp = NULL;			/* duplicate is deleted */
    }
  }

  setWriteGen(q, gen);
  simpleMutexUnlock(&db->queries.write.lock);

  if ( !q->transaction && rdf_is_broadcasting(EV_ASSERT|EV_ASSERT_LOAD) )
  { for(tp=triples; tp < ep; tp++)
    { triple *t = *tp;

      if ( t )
      { broadcast_id id = t->loaded ? EV_ASSERT_LOAD : EV_ASSERT;

	if ( !rdf_broadcast(id, t, NULL) )
	  return FALSE;
      }
    }
  }

  return TRUE;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
del_triples() deletes triples from the database.  There are two actions:

  - del_triple_consequences() deletes (entailment) consequences of
    erasing the triple.  Currently this is handling subPropertyOf
    entailment.  This doesn't remove the triple, but merely invalidates
    the subPropertyOf reachability matrix for subsequent generations.
  - erase_triple() is called on the final commit and updates statistics.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

int
del_triples(query *q, triple **triples, size_t count)
{ rdf_db *db = q->db;
  gen_t gen;
  triple **ep = triples+count;
  triple **tp;

  simpleMutexLock(&db->queries.write.lock);
  gen = queryWriteGen(q) + 1;

  for(tp=triples; tp < ep; tp++)
  { triple *t = *tp;

    t->lifespan.died = gen;
    del_triple_consequences(db, t, q);

    if ( q->transaction )
      buffer_triple(q->transaction->transaction_data.deleted, t);
    else
      erase_triple(db, t, q);
  }

  setWriteGen(q, gen);
  simpleMutexUnlock(&db->queries.write.lock);

  if ( !q->transaction && rdf_is_broadcasting(EV_RETRACT) )
  { for(tp=triples; tp < ep; tp++)
    { triple *t = *tp;

      if ( !rdf_broadcast(EV_RETRACT, t, NULL) )
	return FALSE;
    }
  }

  return TRUE;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
update_triples() updates an array of triples.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

int
update_triples(query *q,
	       triple **old, triple **new,
	       size_t count)
{ rdf_db *db = q->db;
  gen_t gen, gen_max;
  triple **eo = old+count;
  triple **to, **tn;
  size_t updated = 0;

  simpleMutexLock(&db->queries.write.lock);
  gen = queryWriteGen(q) + 1;
  gen_max = query_max_gen(q);

  for(to=old,tn=new; to < eo; to++,tn++)
  { if ( *tn )
    { (*to)->lifespan.died = gen;
      (*tn)->lifespan.born = gen;
      (*tn)->lifespan.died = gen_max;
      link_triple(db, *tn, q);
      del_triple_consequences(db, *to, q);
      if ( q->transaction )
      { buffer_triple(q->transaction->transaction_data.updated, *to);
	buffer_triple(q->transaction->transaction_data.updated, *tn);
      } else
      { erase_triple(db, *to, q);
      }

      updated++;
    }
  }

  setWriteGen(q, gen);
  simpleMutexUnlock(&db->queries.write.lock);

  if ( !q->transaction && rdf_is_broadcasting(EV_UPDATE) )
  { for(to=old,tn=new; to < eo; to++,tn++)
    { if ( !rdf_broadcast(EV_UPDATE, *to, *tn) )
	return FALSE;
    }
  }

  return TRUE;
}


static void
invalidate_matrices_transaction(query *q)
{ cell *c, *next;

  for(c=q->transaction_data.r_matrices.head; c; c=next)
  { sub_p_matrix *rm = c->value;

    next = c->next;
    rm->lifespan.died = GEN_PREHIST;
    rdf_free(q->db, c, sizeof(*c));
  }

  q->transaction_data.r_matrices.head = NULL;
  q->transaction_data.r_matrices.tail = NULL;
}


void
close_transaction(query *q)
{ assert(q->type == Q_TRANSACTION);

  free_triple_buffer(q->transaction_data.added);
  free_triple_buffer(q->transaction_data.deleted);
  free_triple_buffer(q->transaction_data.updated);
  invalidate_matrices_transaction(q);

  q->stack->transaction = q->transaction;

  close_query(q);
}


static void
commit_add(query *q, gen_t gen_max, gen_t gen, triple *t)
{ if ( t->lifespan.died == gen_max )
  { t->lifespan.born = gen;
    add_triple_consequences(q->db, t, q);
    if ( q->transaction )
      buffer_triple(q->transaction->transaction_data.added, t);
    else
      t->lifespan.died = GEN_MAX;
  }
}


static void
commit_del(query *q, gen_t gen, triple *t)
{ if ( t->lifespan.died == q->wr_gen )
  { t->lifespan.died = gen;
    if ( q->transaction )
    { del_triple_consequences(q->db, t, q);
      buffer_triple(q->transaction->transaction_data.deleted, t);
    } else
    { erase_triple(q->db, t, q);
    }
  }
}


int
commit_transaction(query *q)
{ rdf_db *db = q->db;
  triple **tp;
  gen_t gen, gen_max;

  simpleMutexLock(&db->queries.write.lock);
  gen = queryWriteGen(q) + 1;
  gen_max = transaction_max_gen(q);
					/* added triples */
  for(tp=q->transaction_data.added->base;
      tp<q->transaction_data.added->top;
      tp++)
  { commit_add(q, gen_max, gen, *tp);
  }
					/* deleted triples */
  for(tp=q->transaction_data.deleted->base;
      tp<q->transaction_data.deleted->top;
      tp++)
  { commit_del(q, gen, *tp);
  }

					/* updated triples */
  for(tp=q->transaction_data.updated->base;
      tp<q->transaction_data.updated->top;
      tp += 2)
  { triple *to = tp[0];
    triple *tn = tp[1];

    commit_del(q, gen, to);
    commit_add(q, gen_max, gen, tn);
  }

  setWriteGen(q, gen);
  simpleMutexUnlock(&db->queries.write.lock);

  q->stack->transaction = q->transaction; /* do not nest monitor calls */
					  /* inside the transaction */

					/* Broadcast new triples */
  if ( !q->transaction )
  { if ( rdf_is_broadcasting(EV_RETRACT) )
    { for(tp=q->transaction_data.deleted->base;
	  tp<q->transaction_data.deleted->top;
	  tp++)
      { triple *t = *tp;

	if ( t->lifespan.died == gen )
	{ if ( !rdf_broadcast(EV_RETRACT, t, NULL) )
	    return FALSE;
	}
      }
    }

    if ( rdf_is_broadcasting(EV_ASSERT|EV_ASSERT_LOAD) )
    { for(tp=q->transaction_data.added->base;
	  tp<q->transaction_data.added->top;
	  tp++)
      { triple *t = *tp;

	if ( t->lifespan.born == gen )
	{ broadcast_id id = t->loaded ? EV_ASSERT_LOAD : EV_ASSERT;

	  if ( !rdf_broadcast(id, t, NULL) )
	    return FALSE;
	}
      }
    }

    if ( rdf_is_broadcasting(EV_UPDATE) )
    { for(tp=q->transaction_data.updated->base;
	  tp<q->transaction_data.updated->top;
	  tp += 2)
      { triple *to = tp[0];
	triple *tn = tp[1];

	if ( to->lifespan.died == gen &&
	     tn->lifespan.born == gen )
	{ if ( !rdf_broadcast(EV_UPDATE, to, tn) )
	    return FALSE;
	}
      }
    }
  }

  close_transaction(q);

  return TRUE;
}


/* TBD: What if someone else deleted this triple too?  We can check
   that by discovering multiple changes to the died generation.
*/

int
discard_transaction(query *q)
{ rdf_db *db = q->db;
  triple **tp;
  gen_t gen_max = transaction_max_gen(q);

  for(tp=q->transaction_data.added->base;
      tp<q->transaction_data.added->top;
      tp++)
  { triple *t = *tp;

					/* revert creation of new */
    if ( is_wr_transaction_gen(q, t->lifespan.born) &&
	 t->lifespan.died == gen_max )
    { t->lifespan.died = GEN_PREHIST;
      erase_triple(db, t, q);
    }
  }

  for(tp=q->transaction_data.deleted->base;
      tp<q->transaction_data.deleted->top;
      tp++)
  { triple *t = *tp;

					/* revert deletion of old */
    if ( is_wr_transaction_gen(q, t->lifespan.died) )
    { t->lifespan.died = GEN_MAX;
    }
  }

  for(tp=q->transaction_data.updated->base;
      tp<q->transaction_data.updated->top;
      tp += 2)
  { triple *to = tp[0];
    triple *tn = tp[1];

					/* revert deletion of old */
    if ( is_wr_transaction_gen(q, to->lifespan.died) )
    { to->lifespan.died = GEN_MAX;
    }
					/* revert creation of new */
    if ( is_wr_transaction_gen(q, tn->lifespan.born) &&
	 tn->lifespan.died == gen_max )
    { tn->lifespan.died = GEN_PREHIST;
      erase_triple(db, tn, q);
    }
  }

  close_transaction(q);

  return TRUE;
}
