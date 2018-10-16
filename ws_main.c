
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
#include <sys/types.h>

#include "pub_tool_basics.h"
#include "pub_tool_tooliface.h"
#include "pub_tool_libcassert.h"
#include "pub_tool_libcprint.h"
#include "pub_tool_libcbase.h"
#include "pub_tool_libcfile.h"
#include "pub_tool_libcproc.h"
#include "pub_tool_options.h"
#include "pub_tool_machine.h"     // VG_(fnptr_to_fnentry)
#include "pub_tool_clientstate.h"  // args + exe name
#include "pub_tool_hashtable.h"
#include "pub_tool_mallocfree.h"
#include "pub_tool_debuginfo.h"
#include "pub_tool_xtree.h"
#include "pub_tool_xarray.h"
#include "valgrind.h"

/*------------------------------------------------------------*/
/*--- version-specific defs                                ---*/
/*------------------------------------------------------------*/

#if defined(__VALGRIND_MAJOR__) && defined(__VALGRIND_MINOR__) \
    && (__VALGRIND_MAJOR__ >= 3 && __VALGRIND_MINOR__ >= 14)
   #define WS_EPOCH 1
#else
   #undef WS_EPOCH
#endif
#warning "VG-major: " __VALGRIND_MAJOR__
#warning "VG-minor: " __VALGRIND_MINOR__

/*------------------------------------------------------------*/
/*--- tool info                                            ---*/
/*------------------------------------------------------------*/

#define WS_NAME "ws"
#define WS_VERSION "0.1"
#define WS_DESC "compute working set for data and instructions"

/*------------------------------------------------------------*/
/*--- type definitions                                     ---*/
/*------------------------------------------------------------*/

struct pageaddr_order
{
  VgHashNode        top;  // page address, must be first
  unsigned long int count;
  Time              last_access;
  #ifdef WS_EPOCH
  DiEpoch           ep;  //< FIXME: why is this timestamp needed for access?
  #endif
};

#define vgPlain_malloc(size) vgPlain_malloc ((const char *) __func__, size)

typedef enum { TimeI, TimeMS } TimeUnit;

typedef
   IRExpr
   IRAtom;

/* --- Operations --- */

typedef enum { OpLoad=0, OpStore=1, OpAlu=2 } Op;

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

typedef
   struct {
      Time t;
      unsigned long pages_insn;
      unsigned long pages_data;
   }
   WorkingSet;

/*------------------------------------------------------------*/
/*--- prototypes                                           ---*/
/*------------------------------------------------------------*/

static void maybe_compute_ws (void);

/*------------------------------------------------------------*/
/*--- globals                                              ---*/
/*------------------------------------------------------------*/

static Long guest_instrs_executed = 0;
static unsigned long num_samples = 0;
static unsigned long drop_samples = 0;

// page access tables
static VgHashTable *ht_data;
static VgHashTable *ht_insn;

// working set at each point in time
static XArray *ws_at_time;

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

/*------------------------------------------------------------*/
/*--- Command line options                                 ---*/
/*------------------------------------------------------------*/

/* Command line options controlling instrumentation kinds, as described at
 * the top of this file. */

#define WS_DEFAULT_PS 4096
#define WS_DEFAULT_EVERY 100000
#define WS_DEFAULT_TAU WS_DEFAULT_EVERY

static Bool clo_locations = True;
static Bool clo_listpages = False;
static Int  clo_pagesize  = WS_DEFAULT_PS;
static Int  clo_every     = WS_DEFAULT_EVERY;
static Int  clo_tau       = 0;
static Int  clo_time_unit = TimeI;

/* The name of the function of which the number of calls (under
 * --basic-counts=yes) is to be counted, with default. Override with command
 * line option --fnname. */
static const HChar* clo_fnname = "main";
static const HChar* clo_filename = "ws.out.%p";
static HChar* int_filename;

static Bool ws_process_cmd_line_option(const HChar* arg)
{
   if VG_BOOL_CLO(arg, "--ws-locations", clo_locations) {}
   else if VG_BOOL_CLO(arg, "--ws-list-pages", clo_listpages) {}
   else if VG_STR_CLO(arg, "--ws-file", clo_filename) {}
   else if VG_INT_CLO(arg, "--ws-pagesize", clo_pagesize) {}
   else if VG_INT_CLO(arg, "--ws-every", clo_every) {}
   else if VG_INT_CLO(arg, "--ws-tau", clo_tau) { tl_assert(clo_tau > 0); }
   else if VG_XACT_CLO(arg, "--ws-time-unit=i", clo_time_unit, TimeI)  {}
   else if VG_XACT_CLO(arg, "--ws-time-unit=ms", clo_time_unit, TimeMS) {}
   else return False;

   tl_assert(clo_fnname);
   tl_assert(clo_fnname[0]);
   tl_assert(clo_pagesize > 0);
   tl_assert(clo_every > 0);
   return True;
}

static void ws_print_usage(void)
{
   VG_(printf)(
"    --ws-file=<string>       file name to write results\n"
"    --ws-list-pages=no|yes   print list of all accessed pages [no]\n"
"    --ws-locations=no|yes    get location info for insn pages in listing [yes]\n"
"    --ws-pagesize=<int>      size of VM pages in bytes [%d]\n"
"    --ws-time-unit=i|ms      time unit: instructions executed (default), milliseconds\n"
"    --ws-every=<int>         sample WS every <int> time units [%d]\n"
"    --ws-tau=<int>           consider all accesses made in the last tau time units [%d]\n",
   WS_DEFAULT_PS,
   WS_DEFAULT_EVERY,
   WS_DEFAULT_TAU
   );
}

static void ws_print_debug_usage(void)
{
   VG_(printf)(
"    (none)\n"
   );
}

static const HChar* TimeUnit_to_string(TimeUnit time_unit)
{
   switch (time_unit) {
   case TimeI:  return "instructions";
   case TimeMS: return "ms";
   default:     tl_assert2(0, "TimeUnit_to_string: unrecognised TimeUnit");
   }
}

static Time get_time(void)
{
   // Get current time, in whatever time unit we're using.
   if (clo_time_unit == TimeI) {
      return guest_instrs_executed;
   } else if (clo_time_unit == TimeMS) {
      // Some stuff happens between the millisecond timer being initialised
      // to zero and us taking our first snapshot.  We determine that time
      // gap so we can subtract it from all subsequent times so that our
      // first snapshot is considered to be at t = 0ms.  Unfortunately, a
      // bunch of symbols get read after the first snapshot is taken but
      // before the second one (which is triggered by the first allocation),
      // so when the time-unit is 'ms' we always have a big gap between the
      // first two snapshots.  But at least users won't have to wonder why
      // the first snapshot isn't at t=0.
      static Bool is_first_get_time = True;
      static Time start_time_ms;
      if (is_first_get_time) {
         start_time_ms = VG_(read_millisecond_timer)();
         is_first_get_time = False;
         return 0;
      } else {
         return VG_(read_millisecond_timer)() - start_time_ms;
      }
   } else {
      tl_assert2(0, "bad --time-unit value");
   }
}

static inline Addr pageaddr(Addr addr)
{
   return addr & ~(clo_pagesize-1);
}

// TODO: pages shared between processes?
static void pageaccess(Addr pageaddr, VgHashTable *ht) {

   struct pageaddr_order * page = VG_(HT_lookup) (ht, pageaddr);
   if (page == NULL) {
      page = VG_(malloc) (sizeof (*page));
      page->top.key = pageaddr;
      page->count = 0;
      VG_(HT_add_node) (ht, (VgHashNode *) page);
      //VG_(dmsg)("New page: %p\n", pageaddr);
   }

   page->count++;
   page->last_access = (long) get_time();
   #ifdef WS_EPOCH
   page->ep = VG_(current_DiEpoch)();
   #endif

   maybe_compute_ws();
}

static VG_REGPARM(2) void trace_data(Addr addr, SizeT size)
{
   const Addr pa = pageaddr(addr);
   pageaccess(pa, ht_data);
}

static VG_REGPARM(2) void trace_instr(Addr addr, SizeT size)
{
   const Addr pa = pageaddr(addr);
   pageaccess(pa, ht_insn);
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
      argv = mkIRExprVec_2( ev->addr, mkIRExpr_HWord( ev->size ));
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
   // ensure we have a separate file for every process
   const HChar *append = ".%p";
   const int appendlen = (VG_(strstr) (clo_filename, "%p") == NULL) ? VG_(strlen) (append) : 0;
   int_filename = (HChar*) VG_(malloc) (VG_(strlen) (clo_filename) + 1 + appendlen);
   VG_(strcpy) (int_filename, clo_filename);
   if (appendlen > 0) VG_(strcat) (int_filename, append);
   VG_(umsg)("Output file: %s\n", int_filename);

   // check intervals and times
   if (clo_tau == 0) clo_tau = clo_every;
   if (clo_time_unit != TimeI) {
      VG_(umsg)("Warning: time unit %s not implemented, yet. Fallback to instructions",
                TimeUnit_to_string(clo_time_unit));
      clo_time_unit = TimeI;
   }

   // verbose a bit
   VG_(umsg)("Page size = %d bytes\n", clo_pagesize);
   VG_(umsg)("Computing WS every %d %s\n", clo_every,
             TimeUnit_to_string(clo_time_unit));
   VG_(umsg)("Considering references in past %d %s\n", clo_tau,
             TimeUnit_to_string(clo_time_unit));
}

static void add_counter_update(IRSB* sbOut, Int n)
{
   #if defined(VG_BIGENDIAN)
   # define END Iend_BE
   #elif defined(VG_LITTLEENDIAN)
   # define END Iend_LE
   #else
   # error "Unknown endianness"
   #endif
   // Add code to increment 'guest_instrs_executed' by 'n', like this:
   //   WrTmp(t1, Load64(&guest_instrs_executed))
   //   WrTmp(t2, Add64(RdTmp(t1), Const(n)))
   //   Store(&guest_instrs_executed, t2)
   IRTemp t1 = newIRTemp(sbOut->tyenv, Ity_I64);
   IRTemp t2 = newIRTemp(sbOut->tyenv, Ity_I64);
   IRExpr* counter_addr = mkIRExpr_HWord( (HWord)&guest_instrs_executed );

   IRStmt* st1 = IRStmt_WrTmp(t1, IRExpr_Load(END, Ity_I64, counter_addr));
   IRStmt* st2 =
      IRStmt_WrTmp(t2,
                   IRExpr_Binop(Iop_Add64, IRExpr_RdTmp(t1),
                                           IRExpr_Const(IRConst_U64(n))));
   IRStmt* st3 = IRStmt_Store(END, counter_addr, IRExpr_RdTmp(t2));

   addStmtToIRSB( sbOut, st1 );
   addStmtToIRSB( sbOut, st2 );
   addStmtToIRSB( sbOut, st3 );
}

// iterate pages and count those accessed within (now_time - tau, now_time)
static
unsigned long recently_used_pages(VgHashTable *ht, Time now_time)
{
   unsigned long cnt = 0;

   Time tmin = 0;
   if (clo_tau < now_time) tmin = now_time - clo_tau;

   VG_(HT_ResetIter)(ht);
   const VgHashNode *nd;
   while ((nd = VG_(HT_Next)(ht))) {
      const struct pageaddr_order *page = (const struct pageaddr_order *) nd;
      if (page->last_access > tmin) cnt++;
   }
   return cnt;
}

static
void compute_ws(Time now_time)
{
   WorkingSet *ws = VG_(malloc) (sizeof(WorkingSet));
   if (!ws) {
      drop_samples++;
      return;
   }

   ws->t = now_time;
   ws->pages_insn = recently_used_pages (ht_insn, now_time);
   ws->pages_data = recently_used_pages (ht_data, now_time);
   num_samples++;
   VG_(addToXA) (ws_at_time, &ws);
}

static
void maybe_compute_ws (void)
{
   tl_assert(clo_time_unit == TimeI);

   // if 'every' time units have passed, determine working set again
   static Time earliest_possible_time_of_next_ws = 0;
   Time now_time = get_time();
   if (now_time < earliest_possible_time_of_next_ws) return;

   compute_ws (now_time);

   earliest_possible_time_of_next_ws = now_time + clo_every;
}

// We increment the instruction count in two places:
// - just before any Ist_Exit statements;
// - just before the IRSB's end.
// In the former case, we zero 'n' and then continue instrumenting.
static
IRSB* ws_instrument ( VgCallbackClosure* closure,
                      IRSB* sbIn,
                      const VexGuestLayout* layout,
                      const VexGuestExtents* vge,
                      const VexArchInfo* archinfo_host,
                      IRType gWordTy, IRType hWordTy )
{
   Int        i, ninsn;
   IRSB*      sbOut;
   IRTypeEnv* tyenv = sbIn->tyenv;

   if (gWordTy != hWordTy) {
      /* We don't currently support this case. */
      VG_(tool_panic)("host/guest word size mismatch");
   }

   sbOut = deepCopyIRSBExceptStmts(sbIn);

   // Copy verbatim any IR preamble preceding the first IMark
   i = 0;
   while (i < sbIn->stmts_used && sbIn->stmts[i]->tag != Ist_IMark) {
      addStmtToIRSB( sbOut, sbIn->stmts[i] );
      i++;
   }

   events_used = ninsn = 0;
   // instrument accesses and insn counter, if needed
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
            if (clo_time_unit == TimeI) ninsn++;
            addEvent_Ir( sbOut, mkIRExpr_HWord( (HWord)st->Ist.IMark.addr ),
                         st->Ist.IMark.len );
            addStmtToIRSB( sbOut, st );
            break;

         case Ist_WrTmp:
            {
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
            addEvent_Dw( sbOut, st->Ist.Store.addr,
                         sizeofIRType(type) );
            addStmtToIRSB( sbOut, st );
            break;
         }

         case Ist_StoreG: {
            IRStoreG* sg   = st->Ist.StoreG.details;
            IRExpr*   data = sg->data;
            IRType    type = typeOfIRExpr(tyenv, data);
            tl_assert(type != Ity_INVALID);
            addEvent_Dw_guarded( sbOut, sg->addr,
                                 sizeofIRType(type), sg->guard );
            addStmtToIRSB( sbOut, st );
            break;
         }

         case Ist_LoadG: {
            IRLoadG* lg       = st->Ist.LoadG.details;
            IRType   type     = Ity_INVALID; /* loaded type */
            IRType   typeWide = Ity_INVALID; /* after implicit widening */
            typeOfIRLoadGOp(lg->cvt, &typeWide, &type);
            tl_assert(type != Ity_INVALID);
            addEvent_Dr_guarded( sbOut, lg->addr,
                                 sizeofIRType(type), lg->guard );
            addStmtToIRSB( sbOut, st );
            break;
         }

         case Ist_Dirty: {
            {
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
            addEvent_Dr( sbOut, cas->addr, dataSize );
            addEvent_Dw( sbOut, cas->addr, dataSize );

            addStmtToIRSB( sbOut, st );
            break;
         }

         case Ist_LLSC: {
            IRType dataTy;
            if (st->Ist.LLSC.storedata == NULL) {
               /* LL */
               dataTy = typeOfIRTemp(tyenv, st->Ist.LLSC.result);
               addEvent_Dr( sbOut, st->Ist.LLSC.addr,
                                   sizeofIRType(dataTy) );
               /* flush events before LL, helps SC to succeed */
               flushEvents(sbOut);
            } else {
               /* SC */
               dataTy = typeOfIRExpr(tyenv, st->Ist.LLSC.storedata);
               addEvent_Dw( sbOut, st->Ist.LLSC.addr,
                            sizeofIRType(dataTy) );
            }
            addStmtToIRSB( sbOut, st );
            break;
         }

         case Ist_Exit:
            if (clo_time_unit == TimeI && ninsn > 0) {
               // Add an increment before the Exit statement, then reset 'n'.
               add_counter_update(sbOut, ninsn);
               ninsn = 0;
            }
            flushEvents(sbOut);
            addStmtToIRSB( sbOut, st );      // Original statement
            break;

         default:
            ppIRStmt(st);
            tl_assert(0);
      }
   }

   /* At the end of the sbIn.  Flush outstandings. */
   if (clo_time_unit == TimeI && ninsn > 0) {
      add_counter_update(sbOut, ninsn);
   }
   flushEvents(sbOut);

   return sbOut;
}

static Int
rescompare (const void *p1, const void *p2)
{
   const struct pageaddr_order * const *a1 = (const struct pageaddr_order * const *) p1;
   const struct pageaddr_order * const *a2 = (const struct pageaddr_order * const *) p2;

   if ((*a1)->count > (*a2)->count) return -1;
   if ((*a1)->count < (*a2)->count) return 1;
   return 0;
}

static void print_pagestats(VgHashTable *ht, VgFile *fp)
{
   struct pageaddr_order **res;
   int nres = 0;

   int nentry = VG_(HT_count_nodes) (ht);
   VG_(fprintf) (fp, "%'d entries:\n", nentry);

   res = VG_(malloc) (nentry * sizeof (*res));  // ptr to ptr
   VG_(HT_ResetIter)(ht);
   VgHashNode *nd;
   while ((nd = VG_(HT_Next)(ht)))
      res[nres++] = (struct pageaddr_order *) nd;
   VG_(ssort) (res, nres, sizeof (res[0]), rescompare);

   VG_(fprintf) (fp, "%8s %20s %14s", "count", "page", "last-accessed");
   if (ht == ht_insn && clo_locations) VG_(fprintf) (fp, " location");
   for (int i = 0; i < nres; ++i)
   {
      VG_(fprintf) (fp, "\n%8lu %018p %14llu",
                    res[i]->count,
                    (void*)res[i]->top.key,
                    res[i]->last_access);
      if (ht == ht_insn && clo_locations) {
         #ifdef WS_EPOCH
         const HChar *where = VG_(describe_IP) (res[i]->ep, res[i]->top.key, NULL);
         #else
         const HChar *where = VG_(describe_IP) (res[i]->top.key, NULL);
         #endif
         VG_(fprintf) (fp, " %s", where);
      }
   }
   VG_(fprintf) (fp, "\n");
   // FIXME: leaks (res**)
}

static void print_ws_over_time(XArray *xa, VgFile *fp)
{
   // header
   VG_(fprintf) (fp, "%12s %8s %8s\n", "t", "WSS_insn", "WSS_data");

   // data points
   const int num_t = VG_(sizeXA)(ws_at_time);
   unsigned long peak_i = 0, peak_d = 0;
   unsigned long long sum_i = 0, sum_d = 0;
   for (int i = 0; i < num_t; i++) {
      WorkingSet **ws = VG_(indexXA)(ws_at_time, i);
      const unsigned long t = (unsigned long)(*ws)->t;
      const unsigned long pi = (*ws)->pages_insn;
      const unsigned long pd = (*ws)->pages_data;
      sum_i += pi;
      sum_d += pd;
      if (pi > peak_i) peak_i = pi;
      if (pd > peak_d) peak_d = pd;
      VG_(fprintf) (fp, "%12lu %8lu %8lu\n", t, pi, pd);
   }

   const long unsigned int total_i = (long unsigned int) VG_(HT_count_nodes) (ht_insn);
   const long unsigned int total_d = (long unsigned int) VG_(HT_count_nodes) (ht_data);
   const float avg_i = ((float)sum_i) / (num_t - 1);
   const float avg_d = ((float)sum_d) / (num_t - 1);
   VG_(fprintf) (fp, "\nInsn avg/peak/total:  %'.1f/%'lu/%'lu pages (%'u/%'u/%'u kB)",
                 avg_i, peak_i, total_i,
                 (unsigned int)((avg_i * clo_pagesize) / 1024.f),
                 (unsigned int)((peak_i * clo_pagesize) / 1024.f),
                 (unsigned int)((total_i * clo_pagesize) / 1024.f));
   VG_(fprintf) (fp, "\nData avg/peak/total:  %'.1f/%'lu/%'lu pages (%'u/%'u/%'u kB)",
                 avg_d, peak_d, total_d,
                 (unsigned int)((avg_d * clo_pagesize) / 1024.f),
                 (unsigned int)((peak_d * clo_pagesize) / 1024.f),
                 (unsigned int)((total_d * clo_pagesize) / 1024.f));
}

static void ws_fini(Int exitcode)
{
   // force one last data point
   compute_ws(get_time());

   tl_assert(clo_fnname);
   tl_assert(clo_fnname[0]);

   VG_(umsg)("Number of instructions: %'lu\n", (unsigned long) guest_instrs_executed);
   VG_(umsg)("Number of WS samples:   %'lu\n", num_samples);
   VG_(umsg)("Dropped WS samples:     %'lu\n", drop_samples);

   HChar* outfile = VG_(expand_file_name)("--ws-file", int_filename);
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
       // preamble
      VG_(fprintf) (fp, "Working Set Measurement by valgrind-%s-%s\n\n", WS_NAME, WS_VERSION);
      VG_(fprintf) (fp, "Command:        %s", VG_(args_the_exename));
      for (int i = 0; i < VG_(sizeXA)( VG_(args_for_client) ); i++) {
         HChar* arg = * (HChar**) VG_(indexXA)( VG_(args_for_client), i );
         VG_(fprintf)(fp, " %s", arg);
      }
      VG_(fprintf) (fp, "\nInstructions:   %'lu\n", (unsigned long) guest_instrs_executed);
      VG_(fprintf) (fp, "Page size:      %d B\n", clo_pagesize);
      VG_(fprintf) (fp, "Time Unit:      %s\n", TimeUnit_to_string(clo_time_unit));
      VG_(fprintf) (fp, "Every:          %'d units\n", clo_every);
      VG_(fprintf) (fp, "Tau:            %'d units\n\n", clo_tau);
      VG_(fprintf) (fp, "--\n\n");

      // page listing
      if (clo_listpages) {
         VG_(fprintf) (fp, "Code pages, ");
         print_pagestats (ht_insn, fp);
         VG_(fprintf) (fp, "\nData pages, ");
         print_pagestats (ht_data, fp);
         VG_(fprintf) (fp, "\n--\n\n");
      }

      // working set data
      VG_(fprintf) (fp, "Working sets:\n");
      print_ws_over_time (ws_at_time, fp);
      VG_(fprintf) (fp, "\n--\n");
   }

   // cleanup
   VG_(fclose)(fp);
   VG_(HT_destruct) (ht_data, VG_(free));
   VG_(HT_destruct) (ht_insn, VG_(free));
   VG_(deleteXA) (ws_at_time);
   if (int_filename != clo_filename) VG_(free) ((void*)int_filename);
   VG_(umsg)("ws finished\n");
}

// DONE
static void ws_pre_clo_init(void)
{
   VG_(details_name)            (WS_NAME);
   VG_(details_version)         (WS_VERSION);
   VG_(details_description)     (WS_DESC);
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
   ws_at_time = VG_(newXA) (VG_(malloc), "arr_ws", VG_(free), sizeof(WorkingSet*));

}

VG_DETERMINE_INTERFACE_VERSION(ws_pre_clo_init)

/*--------------------------------------------------------------------*/
/*--- end                                                ws_main.c ---*/
/*--------------------------------------------------------------------*/
