/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#include <stdint.h>
#include "openair1/PHY/CODING/nrLDPC_defs.h"
#include "openair1/PHY/CODING/nrLDPC_decoder/nrLDPC_types.h"
#include "openair1/PHY/CODING/nrLDPC_coding/nrLDPC_coding_interface.h"


/* segment interface */
extern int32_t LDPCinit_cuda();
int32_t LDPCinit()
{
  return LDPCinit_cuda();
}

// LDPCshutdown_cuda
extern int32_t LDPCshutdown_cuda();
int32_t LDPCshutdown()
{
  return LDPCshutdown_cuda();
}


// LDPCdecoder
extern int32_t LDPCdecoder_cuda(t_nrLDPC_dec_params* p_decParams,
                         int8_t* p_llr,
                         uint8_t* p_out,
                         t_nrLDPC_time_stats* p_profiler,
                         decode_abort_t* ab);
int32_t LDPCdecoder(t_nrLDPC_dec_params* p_decParams,
                         int8_t* p_llr,
                         uint8_t* p_out,
                         t_nrLDPC_time_stats* p_profiler,
                         decode_abort_t* ab)
{
  return LDPCdecoder_cuda(p_decParams, p_llr, p_out, p_profiler, ab);
}


// LDPCencoder <= (LDPCencoder32)
extern uint32_t **LDPCencoder32(uint8_t **input, encoder_implemparams_t *impp);
uint32_t LDPCencoder(uint8_t **input, uint8_t *output, encoder_implemparams_t *impp)
{
  uint32_t **output32 = LDPCencoder32(input, impp);
  AssertFatal(impp->n_segments < 8, "LDPC CUDA segment interface does not copy more than 8 segs\n");
  // the following copied from ldpc_encoder_optim8segmulti.c
  int nrows = 46; // assumption BG1
  int rate = 3; // assumption BG1
  int no_punctured_columns = (int)((nrows-2)*impp->Zc+impp->K-impp->K*rate)/impp->Zc;
  int removed_bit = (nrows - no_punctured_columns - 2) * impp->Zc + impp->K - (int)(impp->K * rate);
  int len = impp->K + impp->Zc * (nrows - no_punctured_columns) - removed_bit;
  // copy to output format
  for (int i = 0; i < len; ++i) {
    // condition from ldpctest: (channel_input_optim[i] >> j) == (output32[j>>5][i] >> (j&31))
    // for more than 8 segments, need to spread output32 into output
    uint8_t segs = output32[0][i] & 0xff;
    output[i] = segs;
  }
  return 0;
}


/* slot interface */
extern int nrLDPC_coding_encoder32(nrLDPC_slot_encoding_parameters_t *nrLDPC_slot_encoding_parameters, nrLDPC_TB_encoding_parameters_t *nrLDPC_TB_encoding_parameters);
int nrLDPC_coding_encoder(nrLDPC_slot_encoding_parameters_t *nrLDPC_slot_encoding_parameters)
{
  // this should be the same as previous nrLDPC_coding_encoder() in nrLDPC_coding_segment_encoder.c
  for (int dlsch_id = 0; dlsch_id < nrLDPC_slot_encoding_parameters->nb_TBs; dlsch_id++) {
    nrLDPC_TB_encoding_parameters_t *tbp = &nrLDPC_slot_encoding_parameters->TBs[dlsch_id];
    if (tbp->BG == 1 && tbp->C > 8 && tbp->Z == 384) {
      nrLDPC_coding_encoder32(nrLDPC_slot_encoding_parameters, tbp);
    } else {
      AssertFatal(false, "unhandled job, use CPU\n");
    }
  }
  return 0;
}

void nr_process_decode_segment_cuda(nrLDPC_TB_decoding_parameters_t *);
int32_t nrLDPC_coding_decoder(nrLDPC_slot_decoding_parameters_t *nrLDPC_slot_decoding_parameters)
{
  // this should be the same as previous nrLDPC_coding_decoder() in nrLDPC_coding_segment_decoder.c
  for (int pusch_id = 0; pusch_id < nrLDPC_slot_decoding_parameters->nb_TBs; pusch_id++) {
    nrLDPC_TB_decoding_parameters_t *tbp = &nrLDPC_slot_decoding_parameters->TBs[pusch_id];
    if (tbp->Z >= 128 && tbp->BG == 1) {
      nr_process_decode_segment_cuda(tbp);
    } else {
      AssertFatal(false, "unhandled job, use CPU\n");
    }
  }
  return 0;
}

extern int32_t nrLDPC_coding_init_cuda(int max_num_pxsch);
int32_t nrLDPC_coding_init(int max_num_pxsch)
{
  return nrLDPC_coding_init_cuda(max_num_pxsch);
}

extern int32_t nrLDPC_coding_shutdown_cuda(void);
int32_t nrLDPC_coding_shutdown(void)
{
  return nrLDPC_coding_shutdown_cuda();
}
