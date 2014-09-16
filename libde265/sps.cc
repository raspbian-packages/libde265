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

#include "sps.h"
#include "util.h"
#include "scan.h"
#include "decctx.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#define READ_VLC_OFFSET(variable, vlctype, offset)   \
  if ((vlc = get_ ## vlctype(br)) == UVLC_ERROR) {   \
    ctx->add_warning(DE265_ERROR_CODED_PARAMETER_OUT_OF_RANGE, false);  \
    return DE265_ERROR_CODED_PARAMETER_OUT_OF_RANGE; \
  } \
  variable = vlc + offset;

#define READ_VLC(variable, vlctype)  READ_VLC_OFFSET(variable,vlctype,0)


static int SubWidthC_tab[]  = { -1,2,2,1 };
static int SubHeightC_tab[] = { -1,2,1,1 };


// TODO if (!check_high(ctx, vlc, 15)) return false;
// TODO if (!check_ulvc(ctx, vlc)) return false;


// TODO: should be in some header-file of refpic.c
extern bool read_short_term_ref_pic_set(decoder_context* ctx,
                                        const seq_parameter_set* sps,
                                        bitreader* br,
                                        ref_pic_set* out_set,
                                        int idxRps,  // index of the set to be read
                                        const std::vector<ref_pic_set>& sets,
                                        bool sliceRefPicSet);

seq_parameter_set::seq_parameter_set()
{
  // TODO: this is dangerous
  //memset(this,0,sizeof(seq_parameter_set));

  sps_read = false;
  //ref_pic_sets = NULL;
}


seq_parameter_set::~seq_parameter_set()
{
  //free(ref_pic_sets);
}


de265_error seq_parameter_set::read(decoder_context* ctx, bitreader* br)
{
  int vlc;

  video_parameter_set_id = get_bits(br,4);
  sps_max_sub_layers     = get_bits(br,3) +1;
  if (sps_max_sub_layers>7) {
    return DE265_ERROR_CODED_PARAMETER_OUT_OF_RANGE;
  }

  sps_temporal_id_nesting_flag = get_bits(br,1);

  read_profile_tier_level(br,&profile_tier_level, sps_max_sub_layers);

  READ_VLC(seq_parameter_set_id, uvlc);


  // --- decode chroma type ---

  READ_VLC(chroma_format_idc, uvlc);

  if (chroma_format_idc == 3) {
    separate_colour_plane_flag = get_bits(br,1);
  }
  else {
    separate_colour_plane_flag = 0;
  }

  if (separate_colour_plane_flag) {
    ChromaArrayType = 0;
  }
  else {
    ChromaArrayType = chroma_format_idc;
  }

  if (chroma_format_idc<0 ||
      chroma_format_idc>3) {
    ctx->add_warning(DE265_WARNING_INVALID_CHROMA_FORMAT, false);
    return DE265_ERROR_CODED_PARAMETER_OUT_OF_RANGE;
  }

  SubWidthC  = SubWidthC_tab [chroma_format_idc];
  SubHeightC = SubHeightC_tab[chroma_format_idc];


  // --- picture size ---

  READ_VLC(pic_width_in_luma_samples,  uvlc);
  READ_VLC(pic_height_in_luma_samples, uvlc);

  conformance_window_flag = get_bits(br,1);

  if (conformance_window_flag) {
    READ_VLC(conf_win_left_offset,  uvlc);
    READ_VLC(conf_win_right_offset, uvlc);
    READ_VLC(conf_win_top_offset,   uvlc);
    READ_VLC(conf_win_bottom_offset,uvlc);
  }
  else {
    conf_win_left_offset  = 0;
    conf_win_right_offset = 0;
    conf_win_top_offset   = 0;
    conf_win_bottom_offset= 0;
  }

  if (ChromaArrayType==0) {
    WinUnitX = 1;
    WinUnitY = 1;
  }
  else {
    WinUnitX = SubWidthC_tab [chroma_format_idc];
    WinUnitY = SubHeightC_tab[chroma_format_idc];
  }


  READ_VLC_OFFSET(bit_depth_luma,  uvlc, 8);
  READ_VLC_OFFSET(bit_depth_chroma,uvlc, 8);

  READ_VLC_OFFSET(log2_max_pic_order_cnt_lsb, uvlc, 4);
  MaxPicOrderCntLsb = 1<<(log2_max_pic_order_cnt_lsb);


  // --- sub_layer_ordering_info ---

  sps_sub_layer_ordering_info_present_flag = get_bits(br,1);

  int firstLayer = (sps_sub_layer_ordering_info_present_flag ?
                    0 : sps_max_sub_layers-1 );

  for (int i=firstLayer ; i <= sps_max_sub_layers-1; i++ ) {

    // sps_max_dec_pic_buffering[i]

    vlc=get_uvlc(br);
    if (vlc == UVLC_ERROR ||
        vlc+1 > MAX_NUM_REF_PICS) {
      ctx->add_warning(DE265_ERROR_CODED_PARAMETER_OUT_OF_RANGE, false);
      return DE265_ERROR_CODED_PARAMETER_OUT_OF_RANGE;
    }

    sps_max_dec_pic_buffering[i] = vlc+1;

    // sps_max_num_reorder_pics[i]

    READ_VLC(sps_max_num_reorder_pics[i], uvlc);


    // sps_max_latency_increase[i]

    READ_VLC(sps_max_latency_increase_plus1[i], uvlc);

    SpsMaxLatencyPictures[i] = (sps_max_num_reorder_pics[i] +
                                sps_max_latency_increase_plus1[i]-1);
  }

  // copy info to all layers if only specified once

  if (sps_sub_layer_ordering_info_present_flag) {
    int ref = sps_max_sub_layers-1;
    assert(ref<7);

    for (int i=0 ; i < sps_max_sub_layers-1; i++ ) {
      sps_max_dec_pic_buffering[i] = sps_max_dec_pic_buffering[ref];
      sps_max_num_reorder_pics[i]  = sps_max_num_reorder_pics[ref];
      sps_max_latency_increase_plus1[i]  = sps_max_latency_increase_plus1[ref];
    }
  }


  READ_VLC_OFFSET(log2_min_luma_coding_block_size, uvlc, 3);
  READ_VLC       (log2_diff_max_min_luma_coding_block_size, uvlc);
  READ_VLC_OFFSET(log2_min_transform_block_size, uvlc, 2);
  READ_VLC(log2_diff_max_min_transform_block_size, uvlc);
  READ_VLC(max_transform_hierarchy_depth_inter, uvlc);
  READ_VLC(max_transform_hierarchy_depth_intra, uvlc);
  scaling_list_enable_flag = get_bits(br,1);

  if (scaling_list_enable_flag) {

    sps_scaling_list_data_present_flag = get_bits(br,1);
    if (sps_scaling_list_data_present_flag) {

      de265_error err;
      if ((err=read_scaling_list(br,this, &scaling_list, false)) != DE265_OK) {
        return err;
      }
    }
    else {
      set_default_scaling_lists(&scaling_list);
    }
  }

  amp_enabled_flag = get_bits(br,1);
  sample_adaptive_offset_enabled_flag = get_bits(br,1);
  pcm_enabled_flag = get_bits(br,1);
  if (pcm_enabled_flag) {
    pcm_sample_bit_depth_luma = get_bits(br,4)+1;
    pcm_sample_bit_depth_chroma = get_bits(br,4)+1;
    READ_VLC_OFFSET(log2_min_pcm_luma_coding_block_size, uvlc, 3);
    READ_VLC(log2_diff_max_min_pcm_luma_coding_block_size, uvlc);
    pcm_loop_filter_disable_flag = get_bits(br,1);
  }
  else {
    pcm_sample_bit_depth_luma = 0;
    pcm_sample_bit_depth_chroma = 0;
    log2_min_pcm_luma_coding_block_size = 0;
    log2_diff_max_min_pcm_luma_coding_block_size = 0;
    pcm_loop_filter_disable_flag = 0;
  }

  READ_VLC(num_short_term_ref_pic_sets, uvlc);
  if (num_short_term_ref_pic_sets < 0 ||
      num_short_term_ref_pic_sets > 64) {
    ctx->add_warning(DE265_WARNING_NUMBER_OF_SHORT_TERM_REF_PIC_SETS_OUT_OF_RANGE, false);
    return DE265_ERROR_CODED_PARAMETER_OUT_OF_RANGE;
  }

  // --- allocate reference pic set ---

  // we do not allocate the ref-pic-set for the slice header here, but in the slice header itself

  ref_pic_sets.resize(num_short_term_ref_pic_sets);

  for (int i = 0; i < num_short_term_ref_pic_sets; i++) {

    bool success = read_short_term_ref_pic_set(ctx,this,br,
                                               &ref_pic_sets[i], i,
                                               ref_pic_sets,
                                               false);

    if (!success) {
      return DE265_WARNING_SPS_HEADER_INVALID;
    }

    // dump_short_term_ref_pic_set(&(*ref_pic_sets)[i], fh);
  }

  long_term_ref_pics_present_flag = get_bits(br,1);

  if (long_term_ref_pics_present_flag) {

    READ_VLC(num_long_term_ref_pics_sps, uvlc);
    if (num_long_term_ref_pics_sps > MAX_NUM_LT_REF_PICS_SPS) {
      return DE265_ERROR_CODED_PARAMETER_OUT_OF_RANGE;
    }

    for (int i = 0; i < num_long_term_ref_pics_sps; i++ ) {
      lt_ref_pic_poc_lsb_sps[i] = get_bits(br, log2_max_pic_order_cnt_lsb);
      used_by_curr_pic_lt_sps_flag[i] = get_bits(br,1);
    }
  }
  else {
    num_long_term_ref_pics_sps = 0; // NOTE: missing definition in standard !
  }

  sps_temporal_mvp_enabled_flag = get_bits(br,1);
  strong_intra_smoothing_enable_flag = get_bits(br,1);
  vui_parameters_present_flag = get_bits(br,1);

#if 0
  if (vui_parameters_present_flag) {
    assert(false);
    /*
      vui_parameters()

        sps_extension_flag
        u(1)
        if( sps_extension_flag )

          while( more_rbsp_data() )

            sps_extension_data_flag
              u(1)
              rbsp_trailing_bits()
    */
  }

  sps_extension_flag = get_bits(br,1);
  if (sps_extension_flag) {
    assert(false);
  }

  check_rbsp_trailing_bits(br);
#endif

  // --- compute derived values ---

  BitDepth_Y   = bit_depth_luma;
  QpBdOffset_Y = 6*(bit_depth_luma-8);
  BitDepth_C   = bit_depth_chroma;
  QpBdOffset_C = 6*(bit_depth_chroma-8);

  Log2MinCbSizeY = log2_min_luma_coding_block_size;
  Log2CtbSizeY = Log2MinCbSizeY + log2_diff_max_min_luma_coding_block_size;
  MinCbSizeY = 1 << Log2MinCbSizeY;
  CtbSizeY = 1 << Log2CtbSizeY;
  PicWidthInMinCbsY = pic_width_in_luma_samples / MinCbSizeY;
  PicWidthInCtbsY   = ceil_div(pic_width_in_luma_samples, CtbSizeY);
  PicHeightInMinCbsY = pic_height_in_luma_samples / MinCbSizeY;
  PicHeightInCtbsY   = ceil_div(pic_height_in_luma_samples,CtbSizeY);
  PicSizeInMinCbsY   = PicWidthInMinCbsY * PicHeightInMinCbsY;
  PicSizeInCtbsY = PicWidthInCtbsY * PicHeightInCtbsY;
  PicSizeInSamplesY = pic_width_in_luma_samples * pic_height_in_luma_samples;

  if (chroma_format_idc==0 || separate_colour_plane_flag) {
    CtbWidthC  = 0;
    CtbHeightC = 0;
  }
  else {
    CtbWidthC  = CtbSizeY / SubWidthC;
    CtbHeightC = CtbSizeY / SubHeightC;
  }

  Log2MinTrafoSize = log2_min_transform_block_size;
  Log2MaxTrafoSize = log2_min_transform_block_size + log2_diff_max_min_transform_block_size;

  Log2MinPUSize = Log2MinCbSizeY-1;
  PicWidthInMinPUs  = PicWidthInCtbsY  << (Log2CtbSizeY - Log2MinPUSize);
  PicHeightInMinPUs = PicHeightInCtbsY << (Log2CtbSizeY - Log2MinPUSize);

  Log2MinIpcmCbSizeY = log2_min_pcm_luma_coding_block_size;
  Log2MaxIpcmCbSizeY = (log2_min_pcm_luma_coding_block_size +
                        log2_diff_max_min_pcm_luma_coding_block_size);

  // the following are not in the standard
  PicWidthInTbsY  = PicWidthInCtbsY  << (Log2CtbSizeY - Log2MinTrafoSize);
  PicHeightInTbsY = PicHeightInCtbsY << (Log2CtbSizeY - Log2MinTrafoSize);
  PicSizeInTbsY = PicWidthInTbsY * PicHeightInTbsY;

  sps_read = true;

  return DE265_OK;
}



void seq_parameter_set::dump_sps(int fd) const
{
  //#if (_MSC_VER >= 1500)
  //#define LOG0(t) loginfo(LogHeaders, t)
  //#define LOG1(t,d) loginfo(LogHeaders, t,d)
  //#define LOG2(t,d1,d2) loginfo(LogHeaders, t,d1,d2)
  //#define LOG3(t,d1,d2,d3) loginfo(LogHeaders, t,d1,d2,d3)

  FILE* fh;
  if (fd==1) fh=stdout;
  else if (fd==2) fh=stderr;
  else { return; }

#define LOG0(t) log2fh(fh, t)
#define LOG1(t,d) log2fh(fh, t,d)
#define LOG2(t,d1,d2) log2fh(fh, t,d1,d2)
#define LOG3(t,d1,d2,d3) log2fh(fh, t,d1,d2,d3)


  LOG0("----------------- SPS -----------------\n");
  LOG1("video_parameter_set_id  : %d\n", video_parameter_set_id);
  LOG1("sps_max_sub_layers      : %d\n", sps_max_sub_layers);
  LOG1("sps_temporal_id_nesting_flag : %d\n", sps_temporal_id_nesting_flag);

  dump_profile_tier_level(&profile_tier_level, sps_max_sub_layers, fh);

  LOG1("seq_parameter_set_id    : %d\n", seq_parameter_set_id);
  LOG2("chroma_format_idc       : %d (%s)\n", chroma_format_idc,
       chroma_format_idc == 1 ? "4:2:0" :
       chroma_format_idc == 2 ? "4:2:2" :
       chroma_format_idc == 3 ? "4:4:4" : "unknown");

  if (chroma_format_idc == 3) {
    LOG1("separate_colour_plane_flag : %d\n", separate_colour_plane_flag);
  }

  LOG1("pic_width_in_luma_samples  : %d\n", pic_width_in_luma_samples);
  LOG1("pic_height_in_luma_samples : %d\n", pic_height_in_luma_samples);
  LOG1("conformance_window_flag    : %d\n", conformance_window_flag);

  if (conformance_window_flag) {
    LOG1("conf_win_left_offset  : %d\n", conf_win_left_offset);
    LOG1("conf_win_right_offset : %d\n", conf_win_right_offset);
    LOG1("conf_win_top_offset   : %d\n", conf_win_top_offset);
    LOG1("conf_win_bottom_offset: %d\n", conf_win_bottom_offset);
  }

  LOG1("bit_depth_luma   : %d\n", bit_depth_luma);
  LOG1("bit_depth_chroma : %d\n", bit_depth_chroma);

  LOG1("log2_max_pic_order_cnt_lsb : %d\n", log2_max_pic_order_cnt_lsb);
  LOG1("sps_sub_layer_ordering_info_present_flag : %d\n", sps_sub_layer_ordering_info_present_flag);

  int firstLayer = (sps_sub_layer_ordering_info_present_flag ?
                    0 : sps_max_sub_layers-1 );

  for (int i=firstLayer ; i <= sps_max_sub_layers-1; i++ ) {
    LOG1("Layer %d\n",i);
    LOG1("  sps_max_dec_pic_buffering      : %d\n", sps_max_dec_pic_buffering[i]);
    LOG1("  sps_max_num_reorder_pics       : %d\n", sps_max_num_reorder_pics[i]);
    LOG1("  sps_max_latency_increase_plus1 : %d\n", sps_max_latency_increase_plus1[i]);
  }

  LOG1("log2_min_luma_coding_block_size : %d\n", log2_min_luma_coding_block_size);
  LOG1("log2_diff_max_min_luma_coding_block_size : %d\n",log2_diff_max_min_luma_coding_block_size);
  LOG1("log2_min_transform_block_size   : %d\n", log2_min_transform_block_size);
  LOG1("log2_diff_max_min_transform_block_size : %d\n", log2_diff_max_min_transform_block_size);
  LOG1("max_transform_hierarchy_depth_inter : %d\n", max_transform_hierarchy_depth_inter);
  LOG1("max_transform_hierarchy_depth_intra : %d\n", max_transform_hierarchy_depth_intra);
  LOG1("scaling_list_enable_flag : %d\n", scaling_list_enable_flag);

  if (scaling_list_enable_flag) {

    LOG1("sps_scaling_list_data_present_flag : %d\n", sps_scaling_list_data_present_flag);
    if (sps_scaling_list_data_present_flag) {

      LOG0("scaling list logging output not implemented");
      //assert(0);
      //scaling_list_data()
    }
  }

  LOG1("amp_enabled_flag                    : %d\n", amp_enabled_flag);
  LOG1("sample_adaptive_offset_enabled_flag : %d\n", sample_adaptive_offset_enabled_flag);
  LOG1("pcm_enabled_flag                    : %d\n", pcm_enabled_flag);

  if (pcm_enabled_flag) {
    LOG1("pcm_sample_bit_depth_luma     : %d\n", pcm_sample_bit_depth_luma);
    LOG1("pcm_sample_bit_depth_chroma   : %d\n", pcm_sample_bit_depth_chroma);
    LOG1("log2_min_pcm_luma_coding_block_size : %d\n", log2_min_pcm_luma_coding_block_size);
    LOG1("log2_diff_max_min_pcm_luma_coding_block_size : %d\n", log2_diff_max_min_pcm_luma_coding_block_size);
    LOG1("pcm_loop_filter_disable_flag  : %d\n", pcm_loop_filter_disable_flag);
  }

  LOG1("num_short_term_ref_pic_sets : %d\n", num_short_term_ref_pic_sets);

  for (int i = 0; i < num_short_term_ref_pic_sets; i++) {
    LOG1("ref_pic_set[ %2d ]: ",i);
    dump_compact_short_term_ref_pic_set(&ref_pic_sets[i], 16, fh);
  }

  LOG1("long_term_ref_pics_present_flag : %d\n", long_term_ref_pics_present_flag);

  if (long_term_ref_pics_present_flag) {

    LOG1("num_long_term_ref_pics_sps : %d\n", num_long_term_ref_pics_sps);

    for (int i = 0; i < num_long_term_ref_pics_sps; i++ ) {
      LOG3("lt_ref_pic_poc_lsb_sps[%d] : %d   (used_by_curr_pic_lt_sps_flag=%d)\n",
           i, lt_ref_pic_poc_lsb_sps[i], used_by_curr_pic_lt_sps_flag[i]);
    }
  }

  LOG1("sps_temporal_mvp_enabled_flag      : %d\n", sps_temporal_mvp_enabled_flag);
  LOG1("strong_intra_smoothing_enable_flag : %d\n", strong_intra_smoothing_enable_flag);
  LOG1("vui_parameters_present_flag        : %d\n", vui_parameters_present_flag);

  LOG1("CtbSizeY     : %d\n", CtbSizeY);
  LOG1("MinCbSizeY   : %d\n", MinCbSizeY);
  LOG1("MaxCbSizeY   : %d\n", 1<<(log2_min_luma_coding_block_size + log2_diff_max_min_luma_coding_block_size));
  LOG1("MinTBSizeY   : %d\n", 1<<log2_min_transform_block_size);
  LOG1("MaxTBSizeY   : %d\n", 1<<(log2_min_transform_block_size + log2_diff_max_min_transform_block_size));

  LOG1("SubWidthC               : %d\n", SubWidthC);
  LOG1("SubHeightC              : %d\n", SubHeightC);

  return;

  if (vui_parameters_present_flag) {
    assert(false);
    /*
      vui_parameters()

        sps_extension_flag
        u(1)
        if( sps_extension_flag )

          while( more_rbsp_data() )

            sps_extension_data_flag
              u(1)
              rbsp_trailing_bits()
    */
  }
#undef LOG0
#undef LOG1
#undef LOG2
#undef LOG3
  //#endif
}


static uint8_t default_ScalingList_4x4[16] = {
  16,16,16,16,16,16,16,16,
  16,16,16,16,16,16,16,16
};

static uint8_t default_ScalingList_8x8_intra[64] = {
  16,16,16,16,16,16,16,16,
  16,16,17,16,17,16,17,18,
  17,18,18,17,18,21,19,20,
  21,20,19,21,24,22,22,24,
  24,22,22,24,25,25,27,30,
  27,25,25,29,31,35,35,31,
  29,36,41,44,41,36,47,54,
  54,47,65,70,65,88,88,115
};

static uint8_t default_ScalingList_8x8_inter[64] = {
  16,16,16,16,16,16,16,16,
  16,16,17,17,17,17,17,18,
  18,18,18,18,18,20,20,20,
  20,20,20,20,24,24,24,24,
  24,24,24,24,25,25,25,25,
  25,25,25,28,28,28,28,28,
  28,33,33,33,33,33,41,41,
  41,41,54,54,54,71,71,91
};


void fill_scaling_factor(uint8_t* scalingFactors, const uint8_t* sclist, int sizeId)
{
  const position* scan;
  int width;
  int subWidth;

  switch (sizeId) {
  case 0:
    width=4;
    subWidth=1;
    scan = get_scan_order(2, 0 /* diag */);

    for (int i=0;i<4*4;i++) {
      scalingFactors[scan[i].x + width*scan[i].y] = sclist[i];
    }
    break;

  case 1:
    width=8;
    subWidth=1;
    scan = get_scan_order(3, 0 /* diag */);

    for (int i=0;i<8*8;i++) {
      scalingFactors[scan[i].x + width*scan[i].y] = sclist[i];
    }
    break;

  case 2:
    width=8;
    subWidth=2;
    scan = get_scan_order(3, 0 /* diag */);

    for (int i=0;i<8*8;i++) {
      for (int dy=0;dy<2;dy++)
        for (int dx=0;dx<2;dx++)
          {
            int x = 2*scan[i].x+dx;
            int y = 2*scan[i].y+dy;
            scalingFactors[x+width*subWidth*y] = sclist[i];
          }
    }
    break;

  case 3:
    width=8;
    subWidth=4;
    scan = get_scan_order(3, 0 /* diag */);

    for (int i=0;i<8*8;i++) {
      for (int dy=0;dy<4;dy++)
        for (int dx=0;dx<4;dx++)
          {
            int x = 4*scan[i].x+dx;
            int y = 4*scan[i].y+dy;
            scalingFactors[x+width*subWidth*y] = sclist[i];
          }
    }
    break;

  default:
    assert(0);
    break;
  }


  // --- dump matrix ---

#if 0
  for (int y=0;y<width;y++) {
    for (int x=0;x<width;x++)
      printf("%d,",scalingFactors[x*subWidth + width*subWidth*subWidth*y]);

    printf("\n");
  }
#endif
}


de265_error read_scaling_list(bitreader* br, const seq_parameter_set* sps,
                              scaling_list_data* sclist, bool inPPS)
{
  int dc_coeff[4][6];

  for (int sizeId=0;sizeId<4;sizeId++) {
    int n = ((sizeId==3) ? 2 : 6);
    uint8_t scaling_list[6][32*32];

    for (int matrixId=0;matrixId<n;matrixId++) {
      uint8_t* curr_scaling_list = scaling_list[matrixId];
      int scaling_list_dc_coef;

      int canonicalMatrixId = matrixId;
      if (sizeId==3 && matrixId==1) { canonicalMatrixId=3; }


      //printf("----- matrix %d\n",matrixId);

      char scaling_list_pred_mode_flag = get_bits(br,1);
      if (!scaling_list_pred_mode_flag) {
        int scaling_list_pred_matrix_id_delta = get_uvlc(br);
        if (scaling_list_pred_matrix_id_delta < 0 ||
            scaling_list_pred_matrix_id_delta > matrixId) {
          return DE265_ERROR_CODED_PARAMETER_OUT_OF_RANGE;
        }

        //printf("scaling_list_pred_matrix_id_delta=%d\n", scaling_list_pred_matrix_id_delta);

        dc_coeff[sizeId][matrixId] = 16;
        scaling_list_dc_coef       = 16;

        if (scaling_list_pred_matrix_id_delta==0) {
          if (sizeId==0) {
            memcpy(curr_scaling_list, default_ScalingList_4x4, 16);
          }
          else {
            if (canonicalMatrixId<3)
              { memcpy(curr_scaling_list, default_ScalingList_8x8_intra,64); }
            else
              { memcpy(curr_scaling_list, default_ScalingList_8x8_inter,64); }
          }
        }
        else {
          // TODO: CHECK: for sizeID=3 and the second matrix, should we have delta=1 or delta=3 ?
          if (sizeId==3) { assert(scaling_list_pred_matrix_id_delta==1); }

          int mID = matrixId - scaling_list_pred_matrix_id_delta;

          int len = (sizeId == 0 ? 16 : 64);
          memcpy(curr_scaling_list, scaling_list[mID], len);

          scaling_list_dc_coef       = dc_coeff[sizeId][mID];
          dc_coeff[sizeId][matrixId] = dc_coeff[sizeId][mID];
        }
      }
      else {
        int nextCoef=8;
        int coefNum = (sizeId==0 ? 16 : 64);
        if (sizeId>1) {
          scaling_list_dc_coef = get_svlc(br);
          if (scaling_list_dc_coef < -7 ||
              scaling_list_dc_coef > 247) {
            return DE265_ERROR_CODED_PARAMETER_OUT_OF_RANGE;
          }

          scaling_list_dc_coef += 8;
          nextCoef=scaling_list_dc_coef;
          dc_coeff[sizeId][matrixId] = scaling_list_dc_coef;
        }
        else {
          scaling_list_dc_coef = 16;
        }
        //printf("DC = %d\n",scaling_list_dc_coef);

        for (int i=0;i<coefNum;i++) {
          int scaling_list_delta_coef = get_svlc(br);
          if (scaling_list_delta_coef < -128 ||
              scaling_list_delta_coef >  127) {
            return DE265_ERROR_CODED_PARAMETER_OUT_OF_RANGE;
          }

          nextCoef = (nextCoef + scaling_list_delta_coef + 256) % 256;
          curr_scaling_list[i] = nextCoef;
          //printf("curr %d = %d\n",i,nextCoef);
        }
      }


      // --- generate ScalingFactor arrays ---

      switch (sizeId) {
      case 0:
        fill_scaling_factor(&sclist->ScalingFactor_Size0[matrixId][0][0], curr_scaling_list, 0);
        break;

      case 1:
        fill_scaling_factor(&sclist->ScalingFactor_Size1[matrixId][0][0], curr_scaling_list, 1);
        break;

      case 2:
        fill_scaling_factor(&sclist->ScalingFactor_Size2[matrixId][0][0], curr_scaling_list, 2);
        sclist->ScalingFactor_Size2[matrixId][0][0] = scaling_list_dc_coef;
        //printf("DC coeff: %d\n", scaling_list_dc_coef);
        break;

      case 3:
        fill_scaling_factor(&sclist->ScalingFactor_Size3[matrixId][0][0], curr_scaling_list, 3);
        sclist->ScalingFactor_Size3[matrixId][0][0] = scaling_list_dc_coef;
        //printf("DC coeff: %d\n", scaling_list_dc_coef);
        break;
      }
    }
  }

  return DE265_OK;
}


void set_default_scaling_lists(scaling_list_data* sclist)
{
  // 4x4

  for (int matrixId=0;matrixId<6;matrixId++) {
    fill_scaling_factor(&sclist->ScalingFactor_Size0[matrixId][0][0],
                        default_ScalingList_4x4, 0);
  }

  // 8x8

  for (int matrixId=0;matrixId<3;matrixId++) {
    fill_scaling_factor(&sclist->ScalingFactor_Size1[matrixId+0][0][0],
                        default_ScalingList_8x8_intra, 1);
    fill_scaling_factor(&sclist->ScalingFactor_Size1[matrixId+3][0][0],
                        default_ScalingList_8x8_inter, 1);
  }

  // 16x16

  for (int matrixId=0;matrixId<3;matrixId++) {
    fill_scaling_factor(&sclist->ScalingFactor_Size2[matrixId+0][0][0],
                        default_ScalingList_8x8_intra, 2);
    fill_scaling_factor(&sclist->ScalingFactor_Size2[matrixId+3][0][0],
                        default_ScalingList_8x8_inter, 2);
  }

  // 32x32

  fill_scaling_factor(&sclist->ScalingFactor_Size3[0][0][0],
                      default_ScalingList_8x8_intra, 3);
  fill_scaling_factor(&sclist->ScalingFactor_Size3[1][0][0],
                      default_ScalingList_8x8_inter, 3);
}

