/*
 * Copyright (c) 2003, 2006 Matteo Frigo
 * Copyright (c) 2003, 2006 Massachusetts Institute of Technology
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */


#include "ct-hc2c.h"

typedef struct {
     hc2c_solver super;
     const hc2c_desc *desc;
     int bufferedp;
     khc2c k;
} S;

typedef struct {
     plan_hc2c super;
     khc2c k;
     plan *cld0, *cldm; /* children for 0th and middle butterflies */
     INT r, m, v;
     INT ms, vs;
     stride rs, brs;
     twid *td;
     const S *slv;
} P;

/*************************************************************
  Nonbuffered code
 *************************************************************/
static void apply(const plan *ego_, R *cr, R *ci)
{
     const P *ego = (const P *) ego_;
     plan_rdft2 *cld0 = (plan_rdft2 *) ego->cld0;
     plan_rdft2 *cldm = (plan_rdft2 *) ego->cldm;
     INT i, m = ego->m, v = ego->v;
     INT ms = ego->ms, vs = ego->vs;

     for (i = 0; i < v; ++i, cr += vs, ci += vs) {
	  cld0->apply((plan *) cld0, cr, ci, cr, ci);
	  ego->k(cr + ms, ci + ms, cr + (m-1)*ms, ci + (m-1)*ms,
		 ego->td->W, ego->rs, 1, (m+1)/2, ms);
	  cldm->apply((plan *) cldm, cr + (m/2)*ms, ci + (m/2)*ms, 
		      cr + (m/2)*ms, ci + (m/2)*ms);
     }
}

/*************************************************************
  Buffered code
 *************************************************************/

/* should not be 2^k to avoid associativity conflicts */
static INT compute_batchsize(INT radix)
{
     radix /= 2;

     /* round up to multiple of 4 */
     radix += 3;
     radix &= -4;

     return (radix + 2);
}

static void dobatch(khc2c k, R *Rp, R *Ip, R *Rm, R *Im, const R *W, 
		    INT r, INT rs,
		    INT mb, INT me, INT ms, 
		    R *bufp, stride brs)
{
     INT b = WS(brs, 1);
     R *bufm = bufp + b - 2;

     X(cpy2d_pair_ci)(Rp + mb * ms, Ip + mb * ms, bufp, bufp + 1,
		      r, rs, b,
		      me - mb, ms, 2);
     X(cpy2d_pair_ci)(Rm - mb * ms, Im - mb * ms, bufm, bufm + 1,
		      r, rs, b,
		      me - mb, -ms, -2);
     k(bufp, bufp + 1, bufm, bufm + 1, W, brs, mb, me, 2);
     X(cpy2d_pair_co)(bufp, bufp + 1, Rp + mb * ms, Ip + mb * ms, 
		      r, b, rs,
		      me - mb, 2, ms);
     X(cpy2d_pair_co)(bufm, bufm + 1, Rm - mb * ms, Im - mb * ms,
		      r, b, rs,
		      me - mb, -2, -ms);
}

static void apply_buf(const plan *ego_, R *cr, R *ci)
{
     const P *ego = (const P *) ego_;
     plan_rdft2 *cld0 = (plan_rdft2 *) ego->cld0;
     plan_rdft2 *cldm = (plan_rdft2 *) ego->cldm;
     INT i, j, ms = ego->ms, v = ego->v;
     INT batchsz = compute_batchsize(ego->r);
     R *buf;
     INT mb = 1, me = (ego->m+1) / 2;

     STACK_MALLOC(R *, buf, ego->r * batchsz * 4 * sizeof(R));

     for (i = 0; i < v; ++i, cr += ego->vs, ci += ego->vs) {
	  R *Rp = cr;
	  R *Ip = ci;
	  R *Rm = cr + ego->m * ms;
	  R *Im = ci + ego->m * ms;

	  cld0->apply((plan *) cld0, Rp, Ip, Rp, Ip);

	  for (j = mb; j < me - batchsz; j += batchsz) 
	       dobatch(ego->k, Rp, Ip, Rm, Im, ego->td->W, 
		       ego->r, WS(ego->rs, 1), j, j + batchsz, ms, 
		       buf, ego->brs);

	  dobatch(ego->k, Rp, Ip, Rm, Im, ego->td->W, 
		  ego->r, WS(ego->rs, 1), j, me, ms, buf, ego->brs);

	  cldm->apply((plan *) cldm, 
		      Rp + me * ms, Ip + me * ms,
		      Rp + me * ms, Ip + me * ms);
     }

     STACK_FREE(buf);
}

/*************************************************************
  common code
 *************************************************************/
static void awake(plan *ego_, enum wakefulness wakefulness)
{
     P *ego = (P *) ego_;

     X(plan_awake)(ego->cld0, wakefulness);
     X(plan_awake)(ego->cldm, wakefulness);
     /* m+3 instead of m+1 is a hack to cover 4-way SIMD */
     X(twiddle_awake)(wakefulness, &ego->td, ego->slv->desc->tw, 
		      ego->r * ego->m, ego->r, (ego->m + 3) / 2);
}

static void destroy(plan *ego_)
{
     P *ego = (P *) ego_;
     X(plan_destroy_internal)(ego->cld0);
     X(plan_destroy_internal)(ego->cldm);
     X(stride_destroy)(ego->rs);
     X(stride_destroy)(ego->brs);
}

static void print(const plan *ego_, printer *p)
{
     const P *ego = (const P *) ego_;
     const S *slv = ego->slv;
     const hc2c_desc *e = slv->desc;

     if (slv->bufferedp)
	  p->print(p, "(hc2c-directbuf/%D-%D/%D%v \"%s\"%(%p%)%(%p%))",
		   compute_batchsize(ego->r), ego->r,
		   X(twiddle_length)(ego->r, e->tw), ego->v, e->nam,
		   ego->cld0, ego->cldm);
     else
	  p->print(p, "(hc2c-direct-%D/%D%v \"%s\"%(%p%)%(%p%))",
		   ego->r, X(twiddle_length)(ego->r, e->tw), ego->v, e->nam,
		   ego->cld0, ego->cldm);
}

static int applicable0(const S *ego, rdft_kind kind,
		       INT r, INT rs,
		       INT m, INT ms, 
		       INT v, INT vs,
		       const R *cr, const R *ci,
		       const planner *plnr)
{
     const hc2c_desc *e = ego->desc;
     UNUSED(v);

     return (
	  1
	  && r == e->radix
	  && kind == e->genus->kind

	  /* first v-loop iteration */
	  && e->genus->okp(cr + ms, ci + ms, cr + (m-1)*ms, ci + (m-1)*ms,
			   rs, 1, (m+1)/2, ms, plnr)
	  
	  /* subsequent v-loop iterations */
	  && (cr += vs, ci += vs, 1)

	  && e->genus->okp(cr + ms, ci + ms, cr + (m-1)*ms, ci + (m-1)*ms,
			   rs, 1, (m+1)/2, ms, plnr)
	  );
}

static int applicable0_buf(const S *ego, rdft_kind kind,
			   INT r, INT rs,
			   INT m, INT ms, 
			   INT v, INT vs,
			   const R *cr, const R *ci,
			   const planner *plnr)
{
     const hc2c_desc *e = ego->desc;
     INT batchsz, brs;
     UNUSED(v); UNUSED(rs); UNUSED(ms); UNUSED(vs);

     return (
	  1
	  && r == e->radix
	  && kind == e->genus->kind

	  /* ignore cr, ci, use buffer */
	  && (cr = (const R *)0, ci = cr + 1, 
	      batchsz = compute_batchsize(r), 
	      brs = 4 * batchsz, 1)

	  && e->genus->okp(cr, ci, cr + brs - 2, ci + brs - 2, 
			   brs, 1, 1+batchsz, 2, plnr)

	  && e->genus->okp(cr, ci, cr + brs - 2, ci + brs - 2, 
			   brs, 1, 1 + (((m-1)/2) % batchsz), 2, plnr)
	  );
}

static int applicable(const S *ego, rdft_kind kind,
		      INT r, INT rs,
		      INT m, INT ms, 
		      INT v, INT vs,
		      R *cr, R *ci,
		      const planner *plnr)
{
     if (ego->bufferedp) {
	  if (!applicable0_buf(ego, kind, r, rs, m, ms, v, vs, cr, ci, plnr))
	       return 0;
     } else {
	  if (!applicable0(ego, kind, r, rs, m, ms, v, vs, cr, ci, plnr))
	       return 0;
     }

     if (NO_UGLYP(plnr) && X(ct_uglyp)((ego->bufferedp? (INT)512 : (INT)16),
				       m * r, r))
	  return 0;

     return 1;
}

static plan *mkcldw(const hc2c_solver *ego_, rdft_kind kind,
		    INT r, INT rs,
		    INT m, INT ms, 
		    INT v, INT vs,
		    R *cr, R *ci,
		    planner *plnr)
{
     const S *ego = (const S *) ego_;
     P *pln;
     const hc2c_desc *e = ego->desc;
     plan *cld0 = 0, *cldm = 0;
     INT imid = (m / 2) * ms;

     static const plan_adt padt = {
	  0, awake, print, destroy
     };

     if (!applicable(ego, kind, r, rs, m, ms, v, vs, cr, ci, plnr))
          return (plan *)0;

     cld0 = X(mkplan_d)(
	  plnr, 
	  X(mkproblem_rdft2_d)(X(mktensor_1d)(r, rs, rs),
			       X(mktensor_0d)(),
			       cr, ci, cr, ci, kind));
     if (!cld0) goto nada;

     cldm = X(mkplan_d)(
	  plnr, 
	  X(mkproblem_rdft2_d)(((m % 2) ?
				X(mktensor_0d)() : X(mktensor_1d)(r, rs, rs) ),
			       X(mktensor_0d)(),
			       cr + imid, ci + imid, cr + imid, ci + imid,
			       kind == R2HC ? R2HCII : HC2RIII));
     if (!cldm) goto nada;

     pln = MKPLAN_HC2C(P, &padt, ego->bufferedp ? apply_buf : apply);

     pln->k = ego->k;
     pln->td = 0;
     pln->r = r; pln->rs = X(mkstride)(r, rs);
     pln->m = m; pln->ms = ms;
     pln->v = v; pln->vs = vs;
     pln->slv = ego;
     pln->brs = X(mkstride)(r, 4 * compute_batchsize(r));
     pln->cld0 = cld0;
     pln->cldm = cldm;

     X(ops_zero)(&pln->super.super.ops);
     X(ops_madd2)(v * (((m - 1) / 2) / e->genus->vl),
		  &e->ops, &pln->super.super.ops);
     X(ops_madd2)(v, &cld0->ops, &pln->super.super.ops);
     X(ops_madd2)(v, &cldm->ops, &pln->super.super.ops);

     if (ego->bufferedp) {
	  /* 8 load/stores * N * V */
	  pln->super.super.ops.other += 8 * r * m * v;
     }

     return &(pln->super.super);

 nada:
     X(plan_destroy_internal)(cld0);
     X(plan_destroy_internal)(cldm);
     return 0;
}

static void regone(planner *plnr, khc2c codelet,
		   const hc2c_desc *desc, 
		   hc2c_kind hc2ckind, 
		   int bufferedp)
{
     S *slv = (S *)X(mksolver_hc2c)(sizeof(S), desc->radix, hc2ckind, mkcldw);
     slv->k = codelet;
     slv->desc = desc;
     slv->bufferedp = bufferedp;
     REGISTER_SOLVER(plnr, &(slv->super.super));
}

void X(regsolver_hc2c_direct)(planner *plnr, khc2c codelet,
			      const hc2c_desc *desc,
			      hc2c_kind hc2ckind)
{
     regone(plnr, codelet, desc, hc2ckind, /* bufferedp */0);
     regone(plnr, codelet, desc, hc2ckind, /* bufferedp */1);
}
