/*
 * Copyright 2013-2020 Software Radio Systems Limited
 *
 * This file is part of srsLTE.
 *
 * srsLTE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * srsLTE is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * A copy of the GNU Affero General Public License can be found in
 * the LICENSE file in the top-level directory of this distribution
 * and at http://www.gnu.org/licenses/.
 *
 */

/*!
 * \file polar_decoder_ssc_f.c
 * \brief Definition of the SSC polar decoder inner functions working with
 * float-valued LLRs.
 *
 * \author Jesus Gomez (CTTC)
 * \date 2020
 *
 * \copyright Software Radio Systems Limited
 *
 */

#include "polar_decoder_ssc_f.h"
#include "../utils_avx2.h"
#include "polar_decoder_vector.h"
#include "srslte/phy/fec/polar/polar_encoder.h"
#include "srslte/phy/utils/vector.h"

/*!
 * \brief Describes an SSC polar decoder (float version).
 */
struct pSSC_f {
  float**                 llr0;    /*!< \brief Pointers to the upper half of LLRs values at all stages. */
  float**                 llr1;    /*!< \brief Pointers to the lower half of LLRs values at all stages. */
  uint8_t*                est_bit; /*!< \brief Pointers to the temporary estimated bits. */
  struct Params*          param;   /*!< \brief Pointer to a Params structure. */
  struct State*           state;   /*!< \brief Pointer to a State. */
  srslte_polar_encoder_t* enc;     /*!< \brief Pointer to a srslte_polar_encoder_t. */
  void (*f)(const float* x, const float* y, float* z, const uint16_t len); /*!< \brief Pointer to the function-f. */
  void (*g)(const uint8_t* b,
            const float*   x,
            const float*   y,
            float*         z,
            const uint16_t len); /*!< \brief Pointer to the function-g. */
  void (*xor)(const uint8_t* x,
              const uint8_t* y,
              uint8_t*       z,
              const uint32_t len);                                  /*!< \brief Pointer to the function-g. */
  void (*hard_bit)(const float* x, uint8_t* z, const uint16_t len); /*!< \brief Pointer to the hard-bit function. */
};

/*!
 * Switches between the different types of node (::RATE_1, ::RATE_0, ::RATE_R) for the SSC algorithm.
 * Nodes in the decoding tree at stage \f$ s\f$ get the \f$2^s\f$ LLRs from the parent node and
 * return the associated \f$2^s\f$ estimated bits.
 *
 */
static void simplified_node(void* p, uint8_t* message);

/*!
 * All decoded bits below a ::RATE_0 node are 0. The function updates the \a p->state->active_node_per_stage
 * pointer to point to the next active node. It is assumed that message bits are initialized to 0.
 *
 */
static void rate_0_node(void* p);

/*!
 * ::RATE_1 nodes at stage \f$ s \f$ return the associated \f$2^s\f$ estimated bits by
 * making a hard decision on them.
 * ::RATE_1 nodes also update message bits vector.
 *
 */
static void rate_1_node(void* p, uint8_t* message);

/*!
 * ::RATE_R nodes at stage \f$ s \f$ return the associated \f$2^s\f$ decoded bit by calling
 * the child nodes to the right and left of the decoding tree and then polar encoding (xor) their output.
 * At stage \f$ s \f$, this function runs function srslte_vec_function_f_fff() and srslte_vec_function_g_bfff()
 * with vector size \f$2^{ s - 1}\f$ and updates \a llr0 and \a llr1 memory space for stage \f$(s - 1)\f$.
 * This function also runs srslte_vec_xor_bbb() with vector size \f$2^{s-1}\f$ and
 * updates \a estbits memory space for stage \f$(s + 1)\f$.
 *
 */
static void rate_r_node(void* p, uint8_t* message);

int init_polar_decoder_ssc_f(void* p, const float* input_llr, uint8_t* data_decoded)
{
  struct pSSC_f* pp = p;

  if (p == NULL) {
    return -1;
  }

  uint8_t code_size_log  = pp->param->code_size_log;
  int16_t code_size      = pp->param->code_stage_size[code_size_log];
  int16_t code_half_size = pp->param->code_stage_size[code_size_log - 1];

  // Initializes the data_decoded_vector to all zeros
  memset(data_decoded, 0, code_size);

  // Initialize est_bit vector to all zeros
  memset(pp->est_bit, 0, code_size);

  // Initializes LLR buffer for the last stage/level with the input LLRs values
  for (uint16_t i = 0; i < code_half_size; i++) {
    pp->llr0[code_size_log][i] = input_llr[i];
    pp->llr1[code_size_log][i] = input_llr[i + code_half_size];
  }

  // Initializes the state of the decoding tree
  pp->state->stage = code_size_log + 1; // start from the only one node at the last stage + 1.
  for (uint16_t i = 0; i < code_size_log + 1; i++) {
    pp->state->active_node_per_stage[i] = 0;
  }
  pp->state->flag_finished = false;

  return 0;
}

int polar_decoder_ssc_f(void* p, uint8_t* data_decoded)
{
  if (p == NULL) {
    return -1;
  }

  simplified_node(p, data_decoded);
  return 0;
}

void delete_polar_decoder_ssc_f(void* p)
{
  struct pSSC_f* pp = p;

  if (p != NULL) {
    free(pp->llr0[0]); // remove LLR buffer.
    free(pp->llr0);
    free(pp->llr1);
    free(pp->param->node_type[0]);
    free(pp->param->node_type);
    free(pp->est_bit); // remove estbits buffer.
    free(pp->param->code_stage_size);
    free(pp->param);
    free(pp->state->active_node_per_stage);
    free(pp->state);
    srslte_polar_encoder_free(pp->enc);
    free(pp->enc);
    free(pp);
  }
}

void* create_polar_decoder_ssc_f(uint16_t* frozen_set, const uint8_t code_size_log, const uint16_t frozen_set_size)
{
  struct pSSC_f* pp = NULL; // pointer to the polar decoder instance

  // allocate memory to the polar decoder instance
  if ((pp = malloc(sizeof(struct pSSC_f))) == NULL) {
    return NULL;
  }

  // set functions
  pp->f        = srslte_vec_function_f_fff;
  pp->g        = srslte_vec_function_g_bfff;
  pp->xor      = srslte_vec_xor_bbb;
  pp->hard_bit = srslte_vec_hard_bit_fc;

  // encoder of maximum size
  if ((pp->enc = malloc(sizeof(srslte_polar_encoder_t))) == NULL) {
    free(pp);
    return NULL;
  }
  srslte_polar_encoder_init(pp->enc, SRSLTE_POLAR_ENCODER_PIPELINED, code_size_log);

  // algorithm constants/parameters
  if ((pp->param = malloc(sizeof(struct Params))) == NULL) {
    free(pp->enc);
    free(pp);
    return NULL;
  }

  if ((pp->param->code_stage_size = malloc((code_size_log + 1) * sizeof(uint16_t))) == NULL) {
    free(pp->param);
    free(pp->enc);
    free(pp);
    return NULL;
  }

  pp->param->code_stage_size[0] = 1;
  for (uint8_t i = 1; i < code_size_log + 1; i++) {
    pp->param->code_stage_size[i] = 2 * pp->param->code_stage_size[i - 1];
  }

  pp->param->code_size_log = code_size_log;

  // state  -- initialized in polar_decoder_ssc_init
  if ((pp->state = malloc(sizeof(struct State))) == NULL) {
    free(pp->param->code_stage_size);
    free(pp->param);
    free(pp->enc);
    free(pp);
    return NULL;
  }
  if ((pp->state->active_node_per_stage = malloc((code_size_log + 1) * sizeof(uint16_t))) == NULL) {
    free(pp->state);
    free(pp->param->code_stage_size);
    free(pp->param);
    free(pp->enc);
    free(pp);
    return NULL;
  }

  // allocates memory for estimated bits per stage
  uint16_t est_bits_size = pp->param->code_stage_size[code_size_log];

  pp->est_bit = aligned_alloc(SRSLTE_AVX2_B_SIZE, est_bits_size); // every 32 chars are aligned

  // allocate memory for LLR pointers.
  pp->llr0 = malloc((code_size_log + 1) * sizeof(float*));
  pp->llr1 = malloc((code_size_log + 1) * sizeof(float*));

  // There are LLR buffers for n = 0 to n = code_size_log. Each with size 2^n. Thus,
  // the total memory needed is 2^(n+1)-1.
  // Only the stages starting at multiples of SRSLTE_AVX2_B_SIZE are aligned.

  // Let n_simd_llr be the exponent of the SIMD size in nummer of LLRs.
  // i.e. in a SIMD instruction we can load 2^(n_simd_llr) LLR values
  // then the memory for stages s >= n_simd_llr - 1 is aligned.
  // but only the operations at stages s > n_simd_llr have all the inputs aligned.
  uint8_t  n_llr_all_stages = code_size_log + 1; // there are 2^(n_llr_all_stages) - 1 LLR values summing up all stages.
  uint16_t llr_all_stages   = 1U << n_llr_all_stages;

  pp->llr0[0] = aligned_alloc(SRSLTE_AVX2_B_SIZE, llr_all_stages * sizeof(float)); // 32*8=256

  // allocate memory to the polar decoder instance
  if (pp->llr0[0] == NULL) {
    free(pp->llr1);
    free(pp->llr0);
    free(pp->state);
    free(pp->param->code_stage_size);
    free(pp->param);
    free(pp->enc);
    free(pp);
    return NULL;
  }

  // initialize all LLR pointers
  pp->llr1[0] = pp->llr0[0] + 1;
  for (uint8_t s = 1; s < code_size_log + 1; s++) {
    pp->llr0[s] = pp->llr0[0] + pp->param->code_stage_size[s];
    pp->llr1[s] = pp->llr0[0] + pp->param->code_stage_size[s] + pp->param->code_stage_size[s - 1];
  }

  // allocate memory for node type pointers, one per stage.
  pp->param->frozen_set_size = frozen_set_size;
  pp->param->node_type       = malloc((code_size_log + 1) * sizeof(uint8_t*));

  // allocate memory to node_type_ssc. Stage s has  2^(N-s) nodes s=0,...,N.
  // Thus, same size as LLRs all stages.
  pp->param->node_type[0] = aligned_alloc(SRSLTE_AVX2_B_SIZE, llr_all_stages * sizeof(uint8_t)); // 32*8=256

  if (pp->param->node_type[0] == NULL) {
    free(pp->llr0[0]);
    free(pp->llr1);
    free(pp->llr0);
    free(pp->state);
    free(pp->param->code_stage_size);
    free(pp->param);
    free(pp->enc);
    free(pp);
    return NULL;
  }

  // initialize all node type pointers. (stage 0 is the first, opposite to LLRs)
  for (uint8_t s = 1; s < code_size_log + 1; s++) {
    pp->param->node_type[s] = pp->param->node_type[s - 1] + pp->param->code_stage_size[code_size_log - s + 1];
  }

  init_node_type(frozen_set, pp->param);

  return pp;
}

static void simplified_node(void* p, uint8_t* message)
{

  struct pSSC_f* pp = p;

  pp->state->stage--; // to child node.

  uint8_t  stage   = pp->state->stage;
  uint16_t bit_pos = pp->state->active_node_per_stage[stage];

  switch (pp->param->node_type[stage][bit_pos]) {
    case RATE_1:
      rate_1_node(pp, message);
      break;
    case RATE_0:
      rate_0_node(pp);
      break;
    case RATE_R:
      rate_r_node(pp, message);
      break;
    default:
      printf("ERROR: wrong node type %d\n", pp->param->node_type[stage][bit_pos]);
      exit(-1);
      break;
  }

  pp->state->stage++; // to parent node.
}

static void rate_0_node(void* p)
{
  struct pSSC_f* pp = p;

  uint8_t  code_size_log = pp->param->code_size_log; // code_size_log.
  int16_t  code_size     = pp->param->code_stage_size[code_size_log];
  uint16_t bit_pos       = pp->state->active_node_per_stage[0];
  uint8_t  stage         = pp->state->stage;

  if (bit_pos == code_size - 1) {
    pp->state->flag_finished = true;
  } else {

    // update active node at all the stages
    for (uint8_t i = 0; i <= stage; i++) {
      pp->state->active_node_per_stage[i] = pp->state->active_node_per_stage[i] + pp->param->code_stage_size[stage - i];
    }
  }
}

static void rate_1_node(void* p, uint8_t* message)
{
  struct pSSC_f* pp    = p;
  uint8_t        stage = pp->state->stage; // for SSC decoder rate 1 nodes are always at stage 0.

  uint16_t bit_pos         = pp->state->active_node_per_stage[0];
  uint16_t code_size       = pp->param->code_stage_size[pp->param->code_size_log];
  uint16_t code_stage_size = pp->param->code_stage_size[stage];

  uint8_t* codeword = pp->est_bit + bit_pos;
  float*   LLR      = pp->llr0[stage];

  pp->hard_bit(LLR, codeword, code_stage_size);

  if (stage != 0) {
    srslte_polar_encoder_encode(pp->enc, codeword, message + bit_pos, stage);
  } else {
    message[bit_pos] = codeword[0];
  }

  // update active node at all the stages
  for (uint8_t i = 0; i <= stage; i++) {
    pp->state->active_node_per_stage[i] = pp->state->active_node_per_stage[i] + pp->param->code_stage_size[stage - i];
  }

  // check if this is the last bit
  if (pp->state->active_node_per_stage[0] == code_size) {
    pp->state->flag_finished = true;
  }
}

static void rate_r_node(void* p, uint8_t* message)
{
  struct pSSC_f* pp              = p;
  uint8_t*       estbits0        = NULL;
  uint8_t*       estbits1        = NULL;
  uint16_t       bit_pos         = 0;
  int16_t        offset0         = 0;
  int16_t        offset1         = 0;
  uint8_t        stage           = pp->state->stage;
  uint16_t       stage_size      = pp->param->code_stage_size[stage];
  uint16_t       stage_half_size = pp->param->code_stage_size[stage - 1];

  pp->f(pp->llr0[stage], pp->llr1[stage], pp->llr0[stage - 1], stage_half_size);

  // move to the child node to the left (up) of the tree.
  simplified_node(pp, message);
  if (pp->state->flag_finished == true) { // (just in case). However for 5G frozen sets, the code can never end here.
    return;
  }

  bit_pos  = pp->state->active_node_per_stage[0];
  offset0  = bit_pos - stage_half_size;
  estbits0 = pp->est_bit + offset0;

  pp->g(estbits0, pp->llr0[stage], pp->llr1[stage], pp->llr0[stage - 1], stage_half_size);
  // move to the child node to the right (down) of the tree.
  simplified_node(pp, message);
  if (pp->state->flag_finished == true) {
    return;
  }

  // compute_xor(pp);
  bit_pos = pp->state->active_node_per_stage[0];

  offset0  = bit_pos - stage_size;
  offset1  = offset0 + stage_half_size;
  estbits0 = pp->est_bit + offset0;
  estbits1 = pp->est_bit + offset1;

  pp->xor (estbits0, estbits1, estbits0, stage_half_size);

  // update this node index
  pp->state->active_node_per_stage[stage] = pp->state->active_node_per_stage[stage] + 1; // return to the father node
}