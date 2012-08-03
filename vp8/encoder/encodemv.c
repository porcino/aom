/*
 *  Copyright (c) 2010 The WebM project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */


#include "vp8/common/common.h"
#include "encodemv.h"
#include "vp8/common/entropymode.h"
#include "vp8/common/systemdependent.h"

#include <math.h>

#ifdef ENTROPY_STATS
extern unsigned int active_section;
#endif

static void encode_mvcomponent(
  vp8_writer *const w,
  const int v,
  const struct mv_context *mvc
) {
  const vp8_prob *p = mvc->prob;
  const int x = v < 0 ? -v : v;

  if (x < mvnum_short) {   // Small
    vp8_write(w, 0, p [mvpis_short]);
    vp8_treed_write(w, vp8_small_mvtree, p + MVPshort, x, mvnum_short_bits);
    if (!x)
      return;         // no sign bit
  } else {                // Large
    int i = 0;

    vp8_write(w, 1, p [mvpis_short]);

    do
      vp8_write(w, (x >> i) & 1, p [MVPbits + i]);

    while (++i < mvnum_short_bits);

    i = mvlong_width - 1;  /* Skip bit 3, which is sometimes implicit */

    do
      vp8_write(w, (x >> i) & 1, p [MVPbits + i]);

    while (--i > mvnum_short_bits);

    if (x & ~((2 << mvnum_short_bits) - 1))
      vp8_write(w, (x >> mvnum_short_bits) & 1, p [MVPbits + mvnum_short_bits]);
  }

  vp8_write(w, v < 0, p [MVPsign]);
}

void vp8_encode_motion_vector(vp8_writer *w, const MV *mv, const MV_CONTEXT *mvc) {
  encode_mvcomponent(w, mv->row >> 1, &mvc[0]);
  encode_mvcomponent(w, mv->col >> 1, &mvc[1]);
}


static unsigned int cost_mvcomponent(const int v, const struct mv_context *mvc) {
  const vp8_prob *p = mvc->prob;
  const int x = v;   // v<0? -v:v;
  unsigned int cost;

  if (x < mvnum_short) {
    cost = vp8_cost_zero(p [mvpis_short])
           + vp8_treed_cost(vp8_small_mvtree, p + MVPshort, x, mvnum_short_bits);

    if (!x)
      return cost;
  } else {
    int i = 0;
    cost = vp8_cost_one(p [mvpis_short]);

    do
      cost += vp8_cost_bit(p [MVPbits + i], (x >> i) & 1);

    while (++i < mvnum_short_bits);

    i = mvlong_width - 1;  /* Skip bit 3, which is sometimes implicit */

    do
      cost += vp8_cost_bit(p [MVPbits + i], (x >> i) & 1);

    while (--i > mvnum_short_bits);

    if (x & ~((2 << mvnum_short_bits) - 1))
      cost += vp8_cost_bit(p [MVPbits + mvnum_short_bits], (x >> mvnum_short_bits) & 1);
  }

  return cost;   // + vp8_cost_bit( p [MVPsign], v < 0);
}

void vp8_build_component_cost_table(int *mvcost[2], const MV_CONTEXT *mvc, int mvc_flag[2]) {
  int i = 1;   // -mv_max;
  unsigned int cost0 = 0;
  unsigned int cost1 = 0;

  vp8_clear_system_state();

  i = 1;

  if (mvc_flag[0]) {
    mvcost [0] [0] = cost_mvcomponent(0, &mvc[0]);

    do {
      // mvcost [0] [i] = cost_mvcomponent( i, &mvc[0]);
      cost0 = cost_mvcomponent(i, &mvc[0]);

      mvcost [0] [i] = cost0 + vp8_cost_zero(mvc[0].prob[MVPsign]);
      mvcost [0] [-i] = cost0 + vp8_cost_one(mvc[0].prob[MVPsign]);
    } while (++i <= mv_max);
  }

  i = 1;

  if (mvc_flag[1]) {
    mvcost [1] [0] = cost_mvcomponent(0, &mvc[1]);

    do {
      // mvcost [1] [i] = cost_mvcomponent( i, mvc[1]);
      cost1 = cost_mvcomponent(i, &mvc[1]);

      mvcost [1] [i] = cost1 + vp8_cost_zero(mvc[1].prob[MVPsign]);
      mvcost [1] [-i] = cost1 + vp8_cost_one(mvc[1].prob[MVPsign]);
    } while (++i <= mv_max);
  }
}


// Motion vector probability table update depends on benefit.
// Small correction allows for the fact that an update to an MV probability
// may have benefit in subsequent frames as well as the current one.

#define MV_PROB_UPDATE_CORRECTION   -1


__inline static void calc_prob(vp8_prob *p, const unsigned int ct[2]) {
  const unsigned int tot = ct[0] + ct[1];

  if (tot) {
    const vp8_prob x = ((ct[0] * 255) / tot) & -2;
    *p = x ? x : 1;
  }
}

static void update(
  vp8_writer *const w,
  const unsigned int ct[2],
  vp8_prob *const cur_p,
  const vp8_prob new_p,
  const vp8_prob update_p,
  int *updated
) {
  const int cur_b = vp8_cost_branch(ct, *cur_p);
  const int new_b = vp8_cost_branch(ct, new_p);
  const int cost = 7 + MV_PROB_UPDATE_CORRECTION + ((vp8_cost_one(update_p) - vp8_cost_zero(update_p) + 128) >> 8);

  if (cur_b - new_b > cost) {
    *cur_p = new_p;
    vp8_write(w, 1, update_p);
    vp8_write_literal(w, new_p >> 1, 7);
    *updated = 1;

  } else
    vp8_write(w, 0, update_p);
}

static void write_component_probs(
  vp8_writer *const w,
  struct mv_context *cur_mvc,
  const struct mv_context *default_mvc_,
  const struct mv_context *update_mvc,
  const unsigned int events [MVvals],
  unsigned int rc,
  int *updated
) {
  vp8_prob *Pcur = cur_mvc->prob;
  const vp8_prob *default_mvc = default_mvc_->prob;
  const vp8_prob *Pupdate = update_mvc->prob;
  unsigned int is_short_ct[2], sign_ct[2];

  unsigned int bit_ct [mvlong_width] [2];

  unsigned int short_ct  [mvnum_short];
  unsigned int short_bct [mvnum_short - 1] [2];

  vp8_prob Pnew [MVPcount];

  (void) rc;
  vp8_copy_array(Pnew, default_mvc, MVPcount);

  vp8_zero(is_short_ct)
  vp8_zero(sign_ct)
  vp8_zero(bit_ct)
  vp8_zero(short_ct)
  vp8_zero(short_bct)


  // j=0
  {
    const int c = events [mv_max];

    is_short_ct [0] += c;    // Short vector
    short_ct [0] += c;       // Magnitude distribution
  }

  // j: 1 ~ mv_max (1023)
  {
    int j = 1;

    do {
      const int c1 = events [mv_max + j];  // positive
      const int c2 = events [mv_max - j];  // negative
      const int c  = c1 + c2;
      int a = j;

      sign_ct [0] += c1;
      sign_ct [1] += c2;

      if (a < mvnum_short) {
        is_short_ct [0] += c;     // Short vector
        short_ct [a] += c;       // Magnitude distribution
      } else {
        int k = mvlong_width - 1;
        is_short_ct [1] += c;     // Long vector

        /*  bit 3 not always encoded. */
        do
          bit_ct [k] [(a >> k) & 1] += c;

        while (--k >= 0);
      }
    } while (++j <= mv_max);
  }

  calc_prob(Pnew + mvpis_short, is_short_ct);

  calc_prob(Pnew + MVPsign, sign_ct);

  {
    vp8_prob p [mvnum_short - 1];    /* actually only need branch ct */
    int j = 0;

    vp8_tree_probs_from_distribution(
      mvnum_short, vp8_small_mvencodings, vp8_small_mvtree,
      p, short_bct, short_ct,
      256, 1
    );

    do
      calc_prob(Pnew + MVPshort + j, short_bct[j]);

    while (++j < mvnum_short - 1);
  }

  {
    int j = 0;

    do
      calc_prob(Pnew + MVPbits + j, bit_ct[j]);

    while (++j < mvlong_width);
  }

  update(w, is_short_ct, Pcur + mvpis_short, Pnew[mvpis_short], *Pupdate++, updated);

  update(w, sign_ct, Pcur + MVPsign, Pnew[MVPsign], *Pupdate++, updated);

  {
    const vp8_prob *const new_p = Pnew + MVPshort;
    vp8_prob *const cur_p = Pcur + MVPshort;

    int j = 0;

    do

      update(w, short_bct[j], cur_p + j, new_p[j], *Pupdate++, updated);

    while (++j < mvnum_short - 1);
  }

  {
    const vp8_prob *const new_p = Pnew + MVPbits;
    vp8_prob *const cur_p = Pcur + MVPbits;

    int j = 0;

    do

      update(w, bit_ct[j], cur_p + j, new_p[j], *Pupdate++, updated);

    while (++j < mvlong_width);
  }
}

void vp8_write_mvprobs(VP8_COMP *cpi) {
  vp8_writer *const w  = & cpi->bc;
  MV_CONTEXT *mvc = cpi->common.fc.mvc;
  int flags[2] = {0, 0};
#ifdef ENTROPY_STATS
  active_section = 4;
#endif
  write_component_probs(
    w, &mvc[0], &vp8_default_mv_context[0], &vp8_mv_update_probs[0], cpi->MVcount[0], 0, &flags[0]
  );
  write_component_probs(
    w, &mvc[1], &vp8_default_mv_context[1], &vp8_mv_update_probs[1], cpi->MVcount[1], 1, &flags[1]
  );

  if (flags[0] || flags[1])
    vp8_build_component_cost_table(cpi->mb.mvcost, (const MV_CONTEXT *) cpi->common.fc.mvc, flags);

#ifdef ENTROPY_STATS
  active_section = 5;
#endif
}


static void encode_mvcomponent_hp(
  vp8_writer *const w,
  const int v,
  const struct mv_context_hp *mvc
) {
  const vp8_prob *p = mvc->prob;
  const int x = v < 0 ? -v : v;

  if (x < mvnum_short_hp) {   // Small
    vp8_write(w, 0, p [mvpis_short_hp]);
    vp8_treed_write(w, vp8_small_mvtree_hp, p + MVPshort_hp, x,
                    mvnum_short_bits_hp);
    if (!x)
      return;         // no sign bit
  } else {                // Large
    int i = 0;

    vp8_write(w, 1, p [mvpis_short_hp]);

    do
      vp8_write(w, (x >> i) & 1, p [MVPbits_hp + i]);

    while (++i < mvnum_short_bits_hp);

    i = mvlong_width_hp - 1;  /* Skip bit 3, which is sometimes implicit */

    do
      vp8_write(w, (x >> i) & 1, p [MVPbits_hp + i]);

    while (--i > mvnum_short_bits_hp);

    if (x & ~((2 << mvnum_short_bits_hp) - 1))
      vp8_write(w, (x >> mvnum_short_bits_hp) & 1,
                p [MVPbits_hp + mvnum_short_bits_hp]);
  }

  vp8_write(w, v < 0, p [MVPsign_hp]);
}

void vp8_encode_motion_vector_hp(vp8_writer *w, const MV *mv,
                                 const MV_CONTEXT_HP *mvc) {

  encode_mvcomponent_hp(w, mv->row, &mvc[0]);
  encode_mvcomponent_hp(w, mv->col, &mvc[1]);
}


static unsigned int cost_mvcomponent_hp(const int v,
                                        const struct mv_context_hp *mvc) {
  const vp8_prob *p = mvc->prob;
  const int x = v;   // v<0? -v:v;
  unsigned int cost;

  if (x < mvnum_short_hp) {
    cost = vp8_cost_zero(p [mvpis_short_hp])
           + vp8_treed_cost(vp8_small_mvtree_hp, p + MVPshort_hp, x,
                            mvnum_short_bits_hp);

    if (!x)
      return cost;
  } else {
    int i = 0;
    cost = vp8_cost_one(p [mvpis_short_hp]);

    do
      cost += vp8_cost_bit(p [MVPbits_hp + i], (x >> i) & 1);

    while (++i < mvnum_short_bits_hp);

    i = mvlong_width_hp - 1;  /* Skip bit 3, which is sometimes implicit */

    do
      cost += vp8_cost_bit(p [MVPbits_hp + i], (x >> i) & 1);

    while (--i > mvnum_short_bits_hp);

    if (x & ~((2 << mvnum_short_bits_hp) - 1))
      cost += vp8_cost_bit(p [MVPbits_hp + mvnum_short_bits_hp],
                           (x >> mvnum_short_bits_hp) & 1);
  }

  return cost;   // + vp8_cost_bit( p [MVPsign], v < 0);
}

void vp8_build_component_cost_table_hp(int *mvcost[2],
                                       const MV_CONTEXT_HP *mvc,
                                       int mvc_flag[2]) {
  int i = 1;   // -mv_max;
  unsigned int cost0 = 0;
  unsigned int cost1 = 0;

  vp8_clear_system_state();

  i = 1;

  if (mvc_flag[0]) {
    mvcost [0] [0] = cost_mvcomponent_hp(0, &mvc[0]);

    do {
      // mvcost [0] [i] = cost_mvcomponent( i, &mvc[0]);
      cost0 = cost_mvcomponent_hp(i, &mvc[0]);

      mvcost [0] [i] = cost0 + vp8_cost_zero(mvc[0].prob[MVPsign_hp]);
      mvcost [0] [-i] = cost0 + vp8_cost_one(mvc[0].prob[MVPsign_hp]);
    } while (++i <= mv_max_hp);
  }

  i = 1;

  if (mvc_flag[1]) {
    mvcost [1] [0] = cost_mvcomponent_hp(0, &mvc[1]);

    do {
      // mvcost [1] [i] = cost_mvcomponent( i, mvc[1]);
      cost1 = cost_mvcomponent_hp(i, &mvc[1]);

      mvcost [1] [i] = cost1 + vp8_cost_zero(mvc[1].prob[MVPsign_hp]);
      mvcost [1] [-i] = cost1 + vp8_cost_one(mvc[1].prob[MVPsign_hp]);
    } while (++i <= mv_max_hp);
  }
}


static void write_component_probs_hp(
  vp8_writer *const w,
  struct mv_context_hp *cur_mvc,
  const struct mv_context_hp *default_mvc_,
  const struct mv_context_hp *update_mvc,
  const unsigned int events [MVvals_hp],
  unsigned int rc,
  int *updated
) {
  vp8_prob *Pcur = cur_mvc->prob;
  const vp8_prob *default_mvc = default_mvc_->prob;
  const vp8_prob *Pupdate = update_mvc->prob;
  unsigned int is_short_ct[2], sign_ct[2];

  unsigned int bit_ct [mvlong_width_hp] [2];

  unsigned int short_ct  [mvnum_short_hp];
  unsigned int short_bct [mvnum_short_hp - 1] [2];

  vp8_prob Pnew [MVPcount_hp];

  (void) rc;
  vp8_copy_array(Pnew, default_mvc, MVPcount_hp);

  vp8_zero(is_short_ct)
  vp8_zero(sign_ct)
  vp8_zero(bit_ct)
  vp8_zero(short_ct)
  vp8_zero(short_bct)


  // j=0
  {
    const int c = events [mv_max_hp];

    is_short_ct [0] += c;    // Short vector
    short_ct [0] += c;       // Magnitude distribution
  }

  // j: 1 ~ mv_max (1023)
  {
    int j = 1;

    do {
      const int c1 = events [mv_max_hp + j];  // positive
      const int c2 = events [mv_max_hp - j];  // negative
      const int c  = c1 + c2;
      int a = j;

      sign_ct [0] += c1;
      sign_ct [1] += c2;

      if (a < mvnum_short_hp) {
        is_short_ct [0] += c;     // Short vector
        short_ct [a] += c;       // Magnitude distribution
      } else {
        int k = mvlong_width_hp - 1;
        is_short_ct [1] += c;     // Long vector

        /*  bit 3 not always encoded. */
        do
          bit_ct [k] [(a >> k) & 1] += c;

        while (--k >= 0);
      }
    } while (++j <= mv_max_hp);
  }

  calc_prob(Pnew + mvpis_short_hp, is_short_ct);

  calc_prob(Pnew + MVPsign_hp, sign_ct);

  {
    vp8_prob p [mvnum_short_hp - 1];    /* actually only need branch ct */
    int j = 0;

    vp8_tree_probs_from_distribution(
      mvnum_short_hp, vp8_small_mvencodings_hp, vp8_small_mvtree_hp,
      p, short_bct, short_ct,
      256, 1
    );

    do
      calc_prob(Pnew + MVPshort_hp + j, short_bct[j]);

    while (++j < mvnum_short_hp - 1);
  }

  {
    int j = 0;

    do
      calc_prob(Pnew + MVPbits_hp + j, bit_ct[j]);

    while (++j < mvlong_width_hp);
  }

  update(w, is_short_ct, Pcur + mvpis_short_hp, Pnew[mvpis_short_hp],
         *Pupdate++, updated);

  update(w, sign_ct, Pcur + MVPsign_hp, Pnew[MVPsign_hp], *Pupdate++,
         updated);

  {
    const vp8_prob *const new_p = Pnew + MVPshort_hp;
    vp8_prob *const cur_p = Pcur + MVPshort_hp;

    int j = 0;

    do

      update(w, short_bct[j], cur_p + j, new_p[j], *Pupdate++, updated);

    while (++j < mvnum_short_hp - 1);
  }

  {
    const vp8_prob *const new_p = Pnew + MVPbits_hp;
    vp8_prob *const cur_p = Pcur + MVPbits_hp;

    int j = 0;

    do

      update(w, bit_ct[j], cur_p + j, new_p[j], *Pupdate++, updated);

    while (++j < mvlong_width_hp);
  }
}

void vp8_write_mvprobs_hp(VP8_COMP *cpi) {
  vp8_writer *const w  = & cpi->bc;
  MV_CONTEXT_HP *mvc = cpi->common.fc.mvc_hp;
  int flags[2] = {0, 0};
#ifdef ENTROPY_STATS
  active_section = 4;
#endif
  write_component_probs_hp(
    w, &mvc[0], &vp8_default_mv_context_hp[0], &vp8_mv_update_probs_hp[0],
    cpi->MVcount_hp[0], 0, &flags[0]
  );
  write_component_probs_hp(
    w, &mvc[1], &vp8_default_mv_context_hp[1], &vp8_mv_update_probs_hp[1],
    cpi->MVcount_hp[1], 1, &flags[1]
  );

  if (flags[0] || flags[1])
    vp8_build_component_cost_table_hp(cpi->mb.mvcost_hp,
                                      (const MV_CONTEXT_HP *)
                                      cpi->common.fc.mvc_hp, flags);

#ifdef ENTROPY_STATS
  active_section = 5;
#endif
}
