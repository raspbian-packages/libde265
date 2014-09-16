/*
 * H.265 video codec.
 * Copyright (c) 2013-2014 struktur AG, Dirk Farin <farin@struktur.de>
 *
 * This file is part of libde265.
 *
 * libde265 is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * libde265 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with libde265.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef DE265_DECCTX_H
#define DE265_DECCTX_H

#include "libde265/vps.h"
#include "libde265/sps.h"
#include "libde265/pps.h"
#include "libde265/nal.h"
#include "libde265/slice.h"
#include "libde265/image.h"
#include "libde265/motion.h"
#include "libde265/de265.h"
#include "libde265/dpb.h"
#include "libde265/sei.h"
#include "libde265/threads.h"
#include "libde265/acceleration.h"
#include "libde265/nal-parser.h"

#define DE265_MAX_VPS_SETS 16   // this is the maximum as defined in the standard
#define DE265_MAX_SPS_SETS 16   // this is the maximum as defined in the standard
#define DE265_MAX_PPS_SETS 64   // this is the maximum as defined in the standard
#define MAX_THREAD_CONTEXTS 68  // enough for 4K @ 32 pixel CTBs, but TODO: make this dynamic

#define MAX_WARNINGS 20


struct slice_segment_header;
struct image_unit;


struct thread_context
{
  thread_context();

  int CtbAddrInRS;
  int CtbAddrInTS;

  int CtbX, CtbY;


  // motion vectors

  int8_t  refIdx[2];
  int16_t mvd[2][2]; // only in top left position
  uint8_t merge_flag;
  uint8_t merge_idx;
  uint8_t mvp_lX_flag[2];
  uint8_t inter_pred_idc; // enum InterPredIdc


  // prediction

  enum IntraPredMode IntraPredModeC; // chroma intra-prediction mode for current CB


  // residual data

  uint8_t cu_transquant_bypass_flag;
  uint8_t transform_skip_flag[3];

  ALIGNED_16(int16_t) _coeffBuf[(32*32)+8]; // alignment required for SSE code !
  int16_t *coeffBuf;

  int16_t coeffList[3][32*32];
  int16_t coeffPos[3][32*32];
  int16_t nCoeff[3];


  // quantization

  int IsCuQpDeltaCoded;
  int CuQpDelta;

  int currentQPY;
  int currentQG_x, currentQG_y;
  int lastQPYinPreviousQG;

  int qPYPrime, qPCbPrime, qPCrPrime;

  CABAC_decoder cabac_decoder;

  context_model ctx_model[CONTEXT_MODEL_TABLE_LENGTH];

  struct decoder_context* decctx;
  struct de265_image *img;
  struct slice_segment_header* shdr;

  struct image_unit* imgunit;
  struct thread_task* task; // executing thread_task or NULL if not multi-threaded

private:
  thread_context(const thread_context&); // not allowed
  const thread_context& operator=(const thread_context&); // not allowed
};



class error_queue
{
 public:
  error_queue();

  void add_warning(de265_error warning, bool once);
  de265_error get_warning();

 private:
  de265_error warnings[MAX_WARNINGS];
  int nWarnings;
  de265_error warnings_shown[MAX_WARNINGS]; // warnings that have already occurred
  int nWarningsShown;
};



struct slice_unit
{
  slice_unit(decoder_context* decctx);
  ~slice_unit();

  NAL_unit* nal;   // we are the owner
  slice_segment_header* shdr;  // not the owner (de265_image is owner)
  bitreader reader;

  struct image_unit* imgunit;

  bool flush_reorder_buffer;

  enum { Unprocessed,
         Inprogress,
         Decoded
  } state;

  void allocate_thread_contexts(int n);
  thread_context* get_thread_context(int n) { return &thread_contexts[n]; }

private:
  thread_context* thread_contexts; /* NOTE: cannot use std::vector, because thread_context has
                                      no copy constructor. */

  decoder_context* ctx;


  slice_unit(const slice_unit&); // not allowed
  const slice_unit& operator=(const slice_unit&); // not allowed
};


struct image_unit
{
  image_unit();
  ~image_unit();

  de265_image* img;
  de265_image  sao_output; // if SAO is used, this is allocated and used as SAO output buffer

  std::vector<slice_unit*> slice_units;
  std::vector<sei_message> suffix_SEIs;

  enum { Invalid, // headers not read yet
         Unknown, // SPS/PPS available
         Reference, // will be used as reference
         Leaf       // not a reference picture
  } role;

  enum { Unprocessed,
         InProgress,
         Decoded,
         Dropped         // will not be decoded
  } state;

  std::vector<thread_task*> tasks; // we are the owner

  /* Saved context models for WPP.
     There is one saved model for the initialization of each CTB row.
     The array is unused for non-WPP streams. */
  std::vector<context_model> ctx_models;  // TODO: move this into image ?
};



class decoder_context : public error_queue {
 public:
  decoder_context();
  ~decoder_context();

  de265_error start_thread_pool(int nThreads);
  void        stop_thread_pool();

  void reset();

  /* */ seq_parameter_set* get_sps(int id)       { return &sps[id]; }
  const seq_parameter_set* get_sps(int id) const { return &sps[id]; }
  /* */ pic_parameter_set* get_pps(int id)       { return &pps[id]; }
  const pic_parameter_set* get_pps(int id) const { return &pps[id]; }

  /*
  const slice_segment_header* get_SliceHeader_atCtb(int ctb) {
    return img->slices[img->get_SliceHeaderIndex_atIndex(ctb)];
  }
  */

  uint8_t get_nal_unit_type() const { return nal_unit_type; }
  bool    get_RapPicFlag() const { return RapPicFlag; }

  de265_error decode_NAL(NAL_unit* nal);

  de265_error decode(int* more);
  de265_error decode_some();

  de265_error decode_slice_unit_sequential(image_unit* imgunit, slice_unit* sliceunit);
  de265_error decode_slice_unit_parallel(image_unit* imgunit, slice_unit* sliceunit);
  de265_error decode_slice_unit_WPP(image_unit* imgunit, slice_unit* sliceunit);
  de265_error decode_slice_unit_tiles(image_unit* imgunit, slice_unit* sliceunit);

  void process_nal_hdr(nal_header*);
  void process_vps(video_parameter_set*);
  void process_sps(seq_parameter_set*);
  void process_pps(pic_parameter_set*);

  bool process_slice_segment_header(decoder_context*, slice_segment_header*,
                                    de265_error*, de265_PTS pts,
                                    nal_header* nal_hdr, void* user_data);

  //void push_current_picture_to_output_queue();
  de265_error push_picture_to_output_queue(image_unit*);


  // --- parameters ---

  bool param_sei_check_hash;
  bool param_conceal_stream_errors;
  bool param_suppress_faulty_pictures;

  int  param_sps_headers_fd;
  int  param_vps_headers_fd;
  int  param_pps_headers_fd;
  int  param_slice_headers_fd;

  bool param_disable_deblocking;
  bool param_disable_sao;
  //bool param_disable_mc_residual_idct;  // not implemented yet
  //bool param_disable_intra_residual_idct;  // not implemented yet

  void set_image_allocation_functions(de265_image_allocation* allocfunc, void* userdata);

  de265_image_allocation param_image_allocation_functions;
  void*                  param_image_allocation_userdata;


  // --- accelerated DSP functions ---

  void set_acceleration_functions(enum de265_acceleration);

  struct acceleration_functions acceleration; // CPU optimized functions


  // --- input stream data ---

  NAL_Parser nal_parser;


  int get_num_worker_threads() const { return num_worker_threads; }

  /* */ de265_image* get_image(int dpb_index)       { return dpb.get_image(dpb_index); }
  const de265_image* get_image(int dpb_index) const { return dpb.get_image(dpb_index); }

  bool has_image(int dpb_index) const { return dpb_index>=0 && dpb_index<dpb.size(); }

  de265_image* get_next_picture_in_output_queue() { return dpb.get_next_picture_in_output_queue(); }
  int          num_pictures_in_output_queue() const { return dpb.num_pictures_in_output_queue(); }
  void         pop_next_picture_in_output_queue() { dpb.pop_next_picture_in_output_queue(); }

 private:
  de265_error read_vps_NAL(bitreader&);
  de265_error read_sps_NAL(bitreader&);
  de265_error read_pps_NAL(bitreader&);
  de265_error read_sei_NAL(bitreader& reader, bool suffix);
  de265_error read_eos_NAL(bitreader& reader);
  de265_error read_slice_NAL(bitreader&, NAL_unit* nal, nal_header& nal_hdr);

 private:
  // --- internal data ---

  video_parameter_set  vps[ DE265_MAX_VPS_SETS ];
  seq_parameter_set    sps[ DE265_MAX_SPS_SETS ];
  pic_parameter_set    pps[ DE265_MAX_PPS_SETS ];

  video_parameter_set* current_vps;
  seq_parameter_set*   current_sps;
  pic_parameter_set*   current_pps;

 public:
  struct thread_pool thread_pool;

 private:
  int num_worker_threads;


 public:
  // --- frame dropping ---

  void set_limit_TID(int tid);
  int  get_highest_TID() const;
  int  get_current_TID() const { return current_HighestTid; }
  int  change_framerate(int more_vs_less); // 1: more, -1: less
  void set_framerate_ratio(int percent);

 private:
  // input parameters
  int limit_HighestTid;    // never switch to a layer above this one
  int framerate_ratio;

  // current control parameters
  int goal_HighestTid;     // this is the layer we want to decode at
  int layer_framerate_ratio; // ratio of frames to keep in the current layer

  int current_HighestTid;  // the layer which we are currently decoding

  struct {
    int8_t tid;
    int8_t ratio;
  } framedrop_tab[100+1];
  int framedrop_tid_index[6+1];

  void compute_framedrop_table();
  void calc_tid_and_framerate_ratio();

 private:
  // --- decoded picture buffer ---

  decoded_picture_buffer dpb;

  int current_image_poc_lsb;
  bool first_decoded_picture;
  bool NoRaslOutputFlag;
  bool HandleCraAsBlaFlag;
  bool FirstAfterEndOfSequenceNAL;

  int  PicOrderCntMsb;
  int prevPicOrderCntLsb;  // at precTid0Pic
  int prevPicOrderCntMsb;  // at precTid0Pic

  de265_image* img;

 public:
  const slice_segment_header* previous_slice_header; /* Remember the last slice for a successive
                                                        dependent slice. */


  // --- motion compensation ---

 public:
  int PocLsbLt[MAX_NUM_REF_PICS];
  int UsedByCurrPicLt[MAX_NUM_REF_PICS];
  int DeltaPocMsbCycleLt[MAX_NUM_REF_PICS];
 private:
  int CurrDeltaPocMsbPresentFlag[MAX_NUM_REF_PICS];
  int FollDeltaPocMsbPresentFlag[MAX_NUM_REF_PICS];

  // The number of entries in the lists below.
  int NumPocStCurrBefore;
  int NumPocStCurrAfter;
  int NumPocStFoll;
  int NumPocLtCurr;
  int NumPocLtFoll;

  // These lists contain absolute POC values.
  int PocStCurrBefore[MAX_NUM_REF_PICS]; // used for reference in current picture, smaller POC
  int PocStCurrAfter[MAX_NUM_REF_PICS];  // used for reference in current picture, larger POC
  int PocStFoll[MAX_NUM_REF_PICS]; // not used for reference in current picture, but in future picture
  int PocLtCurr[MAX_NUM_REF_PICS]; // used in current picture
  int PocLtFoll[MAX_NUM_REF_PICS]; // used in some future picture

  // These lists contain indices into the DPB.
  int RefPicSetStCurrBefore[MAX_NUM_REF_PICS];
  int RefPicSetStCurrAfter[MAX_NUM_REF_PICS];
  int RefPicSetStFoll[MAX_NUM_REF_PICS];
  int RefPicSetLtCurr[MAX_NUM_REF_PICS];
  int RefPicSetLtFoll[MAX_NUM_REF_PICS];


  // --- parameters derived from parameter sets ---

  // NAL

  uint8_t nal_unit_type;

  char IdrPicFlag;
  char RapPicFlag;


  // --- image unit queue ---

  std::vector<image_unit*> image_units;

  bool flush_reorder_buffer_at_this_frame;

 private:
  void init_thread_context(class thread_context* tctx);
  void add_task_decode_CTB_row(thread_context* tctx, bool firstSliceSubstream);
  void add_task_decode_slice_segment(thread_context* tctx, bool firstSliceSubstream);


  void process_picture_order_count(decoder_context* ctx, slice_segment_header* hdr);
  int generate_unavailable_reference_picture(decoder_context* ctx, const seq_parameter_set* sps,
                                             int POC, bool longTerm);
  void process_reference_picture_set(decoder_context* ctx, slice_segment_header* hdr);
  bool construct_reference_picture_lists(decoder_context* ctx, slice_segment_header* hdr);


  void remove_images_from_dpb(const std::vector<int>& removeImageList);
  void run_postprocessing_filters_sequential(de265_image* img);
  void run_postprocessing_filters_parallel(image_unit* img);
};


#endif
