
/*-----------------------------------------------------------------------*/
/*--- A Valgrind tool to compute working sets of a process. ws_main.c ---*/
/*-----------------------------------------------------------------------*/

/*
   This file is part of ws.

   Copyright (C) 2018 Martin Becker

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
   02111-1307, USA.

   The GNU General Public License is contained in the file COPYING.
*/

#include "pub_tool_basics.h"
#include "pub_tool_tooliface.h"
#include "pub_tool_libcassert.h"
#include "pub_tool_libcprint.h"
#include "pub_tool_debuginfo.h"
#include "pub_tool_libcbase.h"
#include "pub_tool_options.h"
#include "pub_tool_machine.h"     // VG_(fnptr_to_fnentry)
#include "pub_tool_hashtable.h"
#include "pub_tool_mallocfree.h"
#include "pub_tool_libcfile.h"

/*------------------------------------------------------------*/
/*--- page access tables                                   ---*/
/*------------------------------------------------------------*/

struct pageaddr_order
{
  VgHashNode top;
  unsigned long int count;
  unsigned long long int last_access;
};

static VgHashTable *ht_data;
static VgHashTable *ht_insn;

#define vgPlain_malloc(size) vgPlain_malloc ((const char *) __func__, size)

/*------------------------------------------------------------*/
/*--- Command line options                                 ---*/
/*------------------------------------------------------------*/

/* Command line options controlling instrumentation kinds, as described at
 * the top of this file. */
static Bool clo_trace_mem       = True;
static Int  clo_pagesize        = 4096;

/* The name of the function of which the number of calls (under
 * --basic-counts=yes) is to be counted, with default. Override with command
 * line option --fnname. */
static const HChar* clo_fnname = "main";
static const HChar* clo_filename = "ws.out.%p";

static Bool ws_process_cmd_line_option(const HChar* arg)
{

   if VG_BOOL_CLO(arg, "--trace-mem",    clo_trace_mem) {}
   else if VG_INT_CLO(arg, "--pagesize", clo_pagesize) {}
   else if VG_STR_CLO(arg, "--ws-file",  clo_filename) {}
   else
      return False;

   tl_assert(clo_fnname);
   tl_assert(clo_fnname[0]);
   tl_assert(clo_pagesize > 0);
   return True;
}

static void ws_print_usage(void)
{
   VG_(printf)(
"    --trace-mem=no|yes        trace all loads and stores [yes]\n"
"    --pagesize=<int>          size of VM pages in bytes [4096]\n"
"    --ws-file=<string>        file name to write results\n"
   );
}

static void ws_print_debug_usage(void)
{
   VG_(printf)(
"    (none)\n"
   );
}

typedef
   IRExpr
   IRAtom;

/* --- Operations --- */

typedef enum { OpLoad=0, OpStore=1, OpAlu=2 } Op;


/*------------------------------------------------------------*/
/*--- Stuff for --trace-mem                                ---*/
/*------------------------------------------------------------*/

#define MAX_DSIZE    512

typedef
   enum { Event_Ir, Event_Dr, Event_Dw, Event_Dm }
   EventKind;

typedef
   struct {
      EventKind  ekind;
      IRAtom*    addr;
      Int        size;
      IRAtom*    guard; /* :: Ity_I1, or NULL=="always True" */
   }
   Event;

/* Up to this many unnotified events are allowed.  Must be at least two,
   so that reads and writes to the same address can be merged into a modify.
   Beyond that, larger numbers just potentially induce more spilling due to
   extending live ranges of address temporaries. */
#define N_EVENTS 4

/* Maintain an ordered list of memory events which are outstanding, in
   the sense that no IR has yet been generated to do the relevant
   helper calls.  The SB is scanned top to bottom and memory events
   are added to the end of the list, merging with the most recent
   notified event where possible (Dw immediately following Dr and
   having the same size and EA can be merged).

   This merging is done so that for architectures which have
   load-op-store instructions (x86, amd64), the instr is treated as if
   it makes just one memory reference (a modify), rather than two (a
   read followed by a write at the same address).

   At various points the list will need to be flushed, that is, IR
   generated from it.  That must happen before any possible exit from
   the block (the end, or an IRStmt_Exit).  Flushing also takes place
   when there is no space to add a new event, and before entering a
   RMW (read-modify-write) section on processors supporting LL/SC.

   If we require the simulation statistics to be up to date with
   respect to possible memory exceptions, then the list would have to
   be flushed before each memory reference.  That's a pain so we don't
   bother.

   Flushing the list consists of walking it start to end and emitting
   instrumentation IR for each event, in the order in which they
   appear. */

static Event events[N_EVENTS];
static Int   events_used = 0;

static inline Addr pageaddr(Addr addr)
{
    return addr & ~(clo_pagesize-1);
}

static void pageaccess(Addr pageaddr, VgHashTable *ht) {

    struct pageaddr_order * pa = VG_(HT_lookup) (ht, pageaddr);
    if (pa == NULL) {
      pa = VG_(malloc) (sizeof (struct pageaddr_order));
      pa->top.key = pageaddr;
      pa->count = 0;
      VG_(HT_add_node) (ht, (VgHashNode *) pa);
      //VG_(dmsg)("New page: %p\n", pageaddr);
    }

    pa->count++;

    // FIXME: use VG's cycle counter for access times
    unsigned int low;
    unsigned int high;
    asm volatile ("rdtsc" : "=a" (low), "=d" (high));
    pa->last_access = (((unsigned long long int) high) << 32) | low;
}

static VG_REGPARM(2) void trace_data(Addr addr, SizeT size)
{
   const Addr pa = pageaddr(addr);
   pageaccess(pa, ht_data);
   //VG_(dmsg)(" D %08lx,%lu -> page %lu\n", addr, size, pa);
}

static VG_REGPARM(2) void trace_instr(Addr addr, SizeT size)
{
   const Addr pa = pageaddr(addr);
   pageaccess(pa, ht_insn);
   //VG_(dmsg)("I  %08lx,%lu -> page %lu\n", addr, size, pa);
}

static void flushEvents(IRSB* sb)
{
   Int        i;
   const HChar* helperName;
   void*      helperAddr;
   IRExpr**   argv;
   IRDirty*   di;
   Event*     ev;

   for (i = 0; i < events_used; i++) {

      ev = &events[i];

      // Decide on helper fn to call and args to pass it.
      switch (ev->ekind) {
         case Event_Ir: helperName = "trace_instr";
                        helperAddr =  trace_instr;  break;

         case Event_Dr:
         case Event_Dw:
         case Event_Dm: helperName = "trace_data";
                        helperAddr =  trace_data; break;
         default:
            tl_assert(0);
      }

      // Add the helper.
      argv = mkIRExprVec_2( ev->addr, mkIRExpr_HWord( ev->size ));  // FIXME: need also cycle number
      di   = unsafeIRDirty_0_N( /*regparms*/2,
                                helperName, VG_(fnptr_to_fnentry)( helperAddr ),
                                argv );
      if (ev->guard) {
         di->guard = ev->guard;
      }
      addStmtToIRSB( sb, IRStmt_Dirty(di) );
   }

   events_used = 0;
}

static void addEvent_Ir ( IRSB* sb, IRAtom* iaddr, UInt isize )
{
   Event* evt;
   tl_assert(clo_trace_mem);
   tl_assert( (VG_MIN_INSTR_SZB <= isize && isize <= VG_MAX_INSTR_SZB)
            || VG_CLREQ_SZB == isize );
   if (events_used == N_EVENTS)
      flushEvents(sb);
   tl_assert(events_used >= 0 && events_used < N_EVENTS);
   evt = &events[events_used];
   evt->ekind = Event_Ir;
   evt->addr  = iaddr;
   evt->size  = isize;
   evt->guard = NULL;
   events_used++;
}

/* Add a guarded read event. */
static
void addEvent_Dr_guarded ( IRSB* sb, IRAtom* daddr, Int dsize, IRAtom* guard )
{
   Event* evt;
   tl_assert(clo_trace_mem);
   tl_assert(isIRAtom(daddr));
   tl_assert(dsize >= 1 && dsize <= MAX_DSIZE);
   if (events_used == N_EVENTS)
      flushEvents(sb);
   tl_assert(events_used >= 0 && events_used < N_EVENTS);
   evt = &events[events_used];
   evt->ekind = Event_Dr;
   evt->addr  = daddr;
   evt->size  = dsize;
   evt->guard = guard;
   events_used++;
}

/* Add an ordinary read event, by adding a guarded read event with an
   always-true guard. */
static
void addEvent_Dr ( IRSB* sb, IRAtom* daddr, Int dsize )
{
   addEvent_Dr_guarded(sb, daddr, dsize, NULL);
}

/* Add a guarded write event. */
static
void addEvent_Dw_guarded ( IRSB* sb, IRAtom* daddr, Int dsize, IRAtom* guard )
{
   Event* evt;
   tl_assert(clo_trace_mem);
   tl_assert(isIRAtom(daddr));
   tl_assert(dsize >= 1 && dsize <= MAX_DSIZE);
   if (events_used == N_EVENTS)
      flushEvents(sb);
   tl_assert(events_used >= 0 && events_used < N_EVENTS);
   evt = &events[events_used];
   evt->ekind = Event_Dw;
   evt->addr  = daddr;
   evt->size  = dsize;
   evt->guard = guard;
   events_used++;
}

/* Add an ordinary write event.  Try to merge it with an immediately
   preceding ordinary read event of the same size to the same
   address. */
static
void addEvent_Dw ( IRSB* sb, IRAtom* daddr, Int dsize )
{
   Event* lastEvt;
   Event* evt;
   tl_assert(clo_trace_mem);
   tl_assert(isIRAtom(daddr));
   tl_assert(dsize >= 1 && dsize <= MAX_DSIZE);

   // Is it possible to merge this write with the preceding read?
   lastEvt = &events[events_used-1];
   if (events_used > 0
       && lastEvt->ekind == Event_Dr
       && lastEvt->size  == dsize
       && lastEvt->guard == NULL
       && eqIRAtom(lastEvt->addr, daddr))
   {
      lastEvt->ekind = Event_Dm;
      return;
   }

   // No.  Add as normal.
   if (events_used == N_EVENTS)
      flushEvents(sb);
   tl_assert(events_used >= 0 && events_used < N_EVENTS);
   evt = &events[events_used];
   evt->ekind = Event_Dw;
   evt->size  = dsize;
   evt->addr  = daddr;
   evt->guard = NULL;
   events_used++;
}


/*------------------------------------------------------------*/
/*--- Basic tool functions                                 ---*/
/*------------------------------------------------------------*/

static void ws_post_clo_init(void)
{

}

// TODO: interesting part
static
IRSB* ws_instrument ( VgCallbackClosure* closure,
                      IRSB* sbIn,
                      const VexGuestLayout* layout,
                      const VexGuestExtents* vge,
                      const VexArchInfo* archinfo_host,
                      IRType gWordTy, IRType hWordTy )
{
   Int        i;
   IRSB*      sbOut;
   IRTypeEnv* tyenv = sbIn->tyenv;

   if (gWordTy != hWordTy) {
      /* We don't currently support this case. */
      VG_(tool_panic)("host/guest word size mismatch");
   }

   /* Set up SB */
   sbOut = deepCopyIRSBExceptStmts(sbIn);

   // Copy verbatim any IR preamble preceding the first IMark
   i = 0;
   while (i < sbIn->stmts_used && sbIn->stmts[i]->tag != Ist_IMark) {
      addStmtToIRSB( sbOut, sbIn->stmts[i] );
      i++;
   }

   if (clo_trace_mem) {
      events_used = 0;
   }

   for (/*use current i*/; i < sbIn->stmts_used; i++) {
      IRStmt* st = sbIn->stmts[i];
      if (!st || st->tag == Ist_NoOp) continue;

      switch (st->tag) {
         case Ist_NoOp:
         case Ist_AbiHint:
         case Ist_Put:
         case Ist_PutI:
         case Ist_MBE:
            addStmtToIRSB( sbOut, st );
            break;

         case Ist_IMark:
            if (clo_trace_mem) {
               addEvent_Ir( sbOut, mkIRExpr_HWord( (HWord)st->Ist.IMark.addr ),
                            st->Ist.IMark.len );
            }
            addStmtToIRSB( sbOut, st );
            break;

         case Ist_WrTmp:
            // Add a call to trace_load() if --trace-mem=yes.
            if (clo_trace_mem) {
               IRExpr* data = st->Ist.WrTmp.data;
               if (data->tag == Iex_Load) {
                  addEvent_Dr( sbOut, data->Iex.Load.addr,
                               sizeofIRType(data->Iex.Load.ty) );
               }
            }
            addStmtToIRSB( sbOut, st );
            break;

         case Ist_Store: {
            IRExpr* data = st->Ist.Store.data;
            IRType  type = typeOfIRExpr(tyenv, data);
            tl_assert(type != Ity_INVALID);
            if (clo_trace_mem) {
               addEvent_Dw( sbOut, st->Ist.Store.addr,
                            sizeofIRType(type) );
            }
            addStmtToIRSB( sbOut, st );
            break;
         }

         case Ist_StoreG: {
            IRStoreG* sg   = st->Ist.StoreG.details;
            IRExpr*   data = sg->data;
            IRType    type = typeOfIRExpr(tyenv, data);
            tl_assert(type != Ity_INVALID);
            if (clo_trace_mem) {
               addEvent_Dw_guarded( sbOut, sg->addr,
                                    sizeofIRType(type), sg->guard );
            }
            addStmtToIRSB( sbOut, st );
            break;
         }

         case Ist_LoadG: {
            IRLoadG* lg       = st->Ist.LoadG.details;
            IRType   type     = Ity_INVALID; /* loaded type */
            IRType   typeWide = Ity_INVALID; /* after implicit widening */
            typeOfIRLoadGOp(lg->cvt, &typeWide, &type);
            tl_assert(type != Ity_INVALID);
            if (clo_trace_mem) {
               addEvent_Dr_guarded( sbOut, lg->addr,
                                    sizeofIRType(type), lg->guard );
            }
            addStmtToIRSB( sbOut, st );
            break;
         }

         case Ist_Dirty: {
            if (clo_trace_mem) {
               Int      dsize;
               IRDirty* d = st->Ist.Dirty.details;
               if (d->mFx != Ifx_None) {
                  // This dirty helper accesses memory.  Collect the details.
                  tl_assert(d->mAddr != NULL);
                  tl_assert(d->mSize != 0);
                  dsize = d->mSize;
                  if (d->mFx == Ifx_Read || d->mFx == Ifx_Modify)
                     addEvent_Dr( sbOut, d->mAddr, dsize );
                  if (d->mFx == Ifx_Write || d->mFx == Ifx_Modify)
                     addEvent_Dw( sbOut, d->mAddr, dsize );
               } else {
                  tl_assert(d->mAddr == NULL);
                  tl_assert(d->mSize == 0);
               }
            }
            addStmtToIRSB( sbOut, st );
            break;
         }

         case Ist_CAS: {
            /* We treat it as a read and a write of the location.  I
               think that is the same behaviour as it was before IRCAS
               was introduced, since prior to that point, the Vex
               front ends would translate a lock-prefixed instruction
               into a (normal) read followed by a (normal) write. */
            Int    dataSize;
            IRType dataTy;
            IRCAS* cas = st->Ist.CAS.details;
            tl_assert(cas->addr != NULL);
            tl_assert(cas->dataLo != NULL);
            dataTy   = typeOfIRExpr(tyenv, cas->dataLo);
            dataSize = sizeofIRType(dataTy);
            if (cas->dataHi != NULL)
               dataSize *= 2; /* since it's a doubleword-CAS */
            if (clo_trace_mem) {
               addEvent_Dr( sbOut, cas->addr, dataSize );
               addEvent_Dw( sbOut, cas->addr, dataSize );
            }

            addStmtToIRSB( sbOut, st );
            break;
         }

         case Ist_LLSC: {
            IRType dataTy;
            if (st->Ist.LLSC.storedata == NULL) {
               /* LL */
               dataTy = typeOfIRTemp(tyenv, st->Ist.LLSC.result);
               if (clo_trace_mem) {
                  addEvent_Dr( sbOut, st->Ist.LLSC.addr,
                                      sizeofIRType(dataTy) );
                  /* flush events before LL, helps SC to succeed */
                  flushEvents(sbOut);
               }
            } else {
               /* SC */
               dataTy = typeOfIRExpr(tyenv, st->Ist.LLSC.storedata);
               if (clo_trace_mem)
                  addEvent_Dw( sbOut, st->Ist.LLSC.addr,
                                      sizeofIRType(dataTy) );
            }
            addStmtToIRSB( sbOut, st );
            break;
         }

         case Ist_Exit:
            if (clo_trace_mem) {
               flushEvents(sbOut);
            }
            addStmtToIRSB( sbOut, st );      // Original statement
            break;

         default:
            ppIRStmt(st);
            tl_assert(0);
      }
   }

   if (clo_trace_mem) {
      /* At the end of the sbIn.  Flush outstandings. */
      flushEvents(sbOut);
   }

   return sbOut;
}

static Int
rescompare (const void *p1, const void *p2)
{
  const struct pageaddr_order * const *a1 = (const struct pageaddr_order * const *) p1;
  const struct pageaddr_order * const *a2 = (const struct pageaddr_order * const *) p2;

  if ((*a1)->count > (*a2)->count)
    return -1;
  if ((*a1)->count < (*a2)->count)
    return 1;
  return 0;
}

static void print_table(VgHashTable *ht, VgFile *fp)
{
  struct pageaddr_order **res;
  int nres = 0;

  int nentry = VG_(HT_count_nodes) (ht);
  VG_(fprintf) (fp, "%4d entries:\n", nentry);

  VG_(umsg)("Sorting results...\n");
  res = VG_(malloc) (nentry * sizeof (*res));  // ptr to ptr
  VG_(HT_ResetIter)(ht);
  VgHashNode *nd;
  while ((nd = VG_(HT_Next)(ht)))
       res[nres++] = (struct pageaddr_order *) nd;
  VG_(ssort) (res, nres, sizeof (res[0]), rescompare);

  VG_(umsg)("Writing results...\n");

  VG_(fprintf) (fp, "   count                 page  last accessed\n");
  for (int i = 0; i < nres; ++i)
  {
    VG_(fprintf) (fp, "%8d %018p %12llu\n",
                  res[i]->count,
                  (void*)res[i]->top.key,
                  res[i]->last_access);
  }
  // FIXME: leaks
}

static void ws_fini(Int exitcode)
{
   tl_assert(clo_fnname);
   tl_assert(clo_fnname[0]);

   HChar* outfile = VG_(expand_file_name)("--ws-file", clo_filename);
   VG_(umsg)("Writing results to file '%s'\n", outfile);
   VgFile *fp = VG_(fopen)(outfile, VKI_O_CREAT|VKI_O_TRUNC|VKI_O_WRONLY,
                                    VKI_S_IRUSR|VKI_S_IWUSR);
   if (fp == NULL) {
      // If the file can't be opened for whatever reason (conflict
      // between multiple cachegrinded processes?), give up now.
      VG_(umsg)("error: can't open simulation output file '%s'\n",
                outfile );
      VG_(umsg)("       ... so simulation results will be missing.\n");
      VG_(free)(outfile);
      return;
   } else {
      VG_(free)(outfile);
   }

   if (fp != NULL) {
       VG_(fprintf) (fp, "Code pages:\n");
       print_table(ht_insn, fp);

       VG_(fprintf) (fp, "\nData pages:\n");
       print_table(ht_data, fp);
   }

   // cleanup
   VG_(fclose)(fp);
   // FIXME: teardown hash table?
}

// DONE
static void ws_pre_clo_init(void)
{
   VG_(details_name)            ("ws");
   VG_(details_version)         ("0.1");
   VG_(details_description)     ("compute working set for data and instructions");
   VG_(details_copyright_author)(
      "Copyright (C) 2018, and GNU GPL'd, by Martin Becker.");
   VG_(details_bug_reports_to)  (VG_BUGS_TO);
   VG_(details_avg_translation_sizeB) ( 200 );

   VG_(basic_tool_funcs)          (ws_post_clo_init,
                                   ws_instrument,
                                   ws_fini);
   VG_(needs_command_line_options)(ws_process_cmd_line_option,
                                   ws_print_usage,
                                   ws_print_debug_usage);

   ht_data = VG_(HT_construct) ("ht_data");
   ht_insn = VG_(HT_construct) ("ht_insn");
}

VG_DETERMINE_INTERFACE_VERSION(ws_pre_clo_init)

/*--------------------------------------------------------------------*/
/*--- end                                                ws_main.c ---*/
/*--------------------------------------------------------------------*/
