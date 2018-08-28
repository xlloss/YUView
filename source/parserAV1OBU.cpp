/*  This file is part of YUView - The YUV player with advanced analytics toolset
*   <https://github.com/IENT/YUView>
*   Copyright (C) 2015  Institut f�r Nachrichtentechnik, RWTH Aachen University, GERMANY
*
*   This program is free software; you can redistribute it and/or modify
*   it under the terms of the GNU General Public License as published by
*   the Free Software Foundation; either version 3 of the License, or
*   (at your option) any later version.
*
*   In addition, as a special exception, the copyright holders give
*   permission to link the code of portions of this program with the
*   OpenSSL library under certain conditions as described in each
*   individual source file, and distribute linked combinations including
*   the two.
*   
*   You must obey the GNU General Public License in all respects for all
*   of the code used other than OpenSSL. If you modify file(s) with this
*   exception, you may extend this exception to your version of the
*   file(s), but you are not obligated to do so. If you do not wish to do
*   so, delete this exception statement from your version. If you delete
*   this exception statement from all source files in the program, then
*   also delete it here.
*
*   This program is distributed in the hope that it will be useful,
*   but WITHOUT ANY WARRANTY; without even the implied warranty of
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
*   GNU General Public License for more details.
*
*   You should have received a copy of the GNU General Public License
*   along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include "parserAV1OBU.h"
#include<algorithm>

#define READFLAG(into) {into=(reader.readBits(1)!=0); if (itemTree) new TreeItem(#into,into,QString("u(1)"),(into!=0)?"1":"0",itemTree);}
#define READFLAG_A(into,i) {bool b=(reader.readBits(1)!=0); into.append(b); if (itemTree) new TreeItem(QString(#into)+QString("[%1]").arg(i),b,QString("u(1)"),b?"1":"0",itemTree);}
#define READBITS(into,numBits) {QString code; into=reader.readBits(numBits, &code); if (itemTree) new TreeItem(#into,into,QString("u(v) -> u(%1)").arg(numBits),code, itemTree);}
#define READBITS_A(into,numBits,i) {QString code; int v=reader.readBits(numBits,&code); into.append(v); if (itemTree) new TreeItem(QString(#into)+QString("[%1]").arg(i),v,QString("u(v) -> u(%1)").arg(numBits),code, itemTree);}
#define READLEB128(into) {QString code; int bit_count; into=reader.readLeb128(&code, &bit_count); if (itemTree) new TreeItem(#into,into,QString("leb128(%1)").arg(bit_count),code, itemTree);}
#define READUVLC(into) {QString code; int bit_count; into=reader.readUVLC(&code, &bit_count); if (itemTree) new TreeItem(#into,into,QString("uvlc(%1)").arg(bit_count),code, itemTree);}
#define READNS(into,numBits) {QString code; int bit_count; into=reader.readNS(numBits, &code, &bit_count); if (itemTree) new TreeItem(#into,into,QString("ns(%1)").arg(bit_count),code, itemTree);}
#define READSU(into,numBits) {QString code; into=reader.readSU(numBits, &code); if (itemTree) new TreeItem(#into,into,QString("ns(%1)").arg(numBits),code, itemTree);}
#define READBITS_M(into,numBits,meanings) {QString code; into=reader.readBits(numBits, &code); if (itemTree) new TreeItem(#into,into,QString("u(v) -> u(%1)").arg(numBits),code, meanings,itemTree);}
#define READBITS_M_E(into,numBits,meanings,enumCast) {QString code; into=(enumCast)reader.readBits(numBits, &code); if (itemTree) new TreeItem(#into,into,QString("u(v) -> u(%1)").arg(numBits),code, meanings,itemTree);}

// Do not actually read anything but also put the value into the tree as a calculated value
#define LOGVAL(val) {if (itemTree) new TreeItem(#val,val,QString("calc"),QString(),itemTree);}
#define LOGSTRVAL(str,val) {if (itemTree) new TreeItem(str,val,QString("info"),QString(),itemTree);}
#define LOGINFO(info) {if (itemTree) new TreeItem("Info", info, "", "", itemTree);}

#define SELECT_SCREEN_CONTENT_TOOLS 2
#define SELECT_INTEGER_MV 2
#define NUM_REF_FRAMES 8
#define REFS_PER_FRAME 7
#define PRIMARY_REF_NONE 7
#define MAX_SEGMENTS 8
#define SEG_LVL_MAX 8
#define SEG_LVL_REF_FRAME 5
#define MAX_LOOP_FILTER 63
#define SEG_LVL_ALT_Q 0
#define TOTAL_REFS_PER_FRAME 8

#define SUPERRES_DENOM_BITS 3
#define SUPERRES_DENOM_MIN 9
#define SUPERRES_NUM 8

// The indices into the RefFrame list
#define INTRA_FRAME 0
#define LAST_FRAME 1
#define LAST2_FRAME 2
#define LAST3_FRAME 3
#define GOLDEN_FRAME 4
#define BWDREF_FRAME 5
#define ALTREF2_FRAME 6
#define ALTREF_FRAME 7

#define MAX_TILE_WIDTH 4096
#define MAX_TILE_AREA 4096 * 2304
#define MAX_TILE_COLS 64
#define MAX_TILE_ROWS 64

const QStringList parserAV1OBU::obu_type_toString = QStringList()
  << "RESERVED" << "OBU_SEQUENCE_HEADER" << "OBU_TEMPORAL_DELIMITER" << "OBU_FRAME_HEADER" << "OBU_TILE_GROUP" 
  << "OBU_METADATA" << "OBU_FRAME" << "OBU_REDUNDANT_FRAME_HEADER" << "OBU_TILE_LIST" << "OBU_PADDING";

parserAV1OBU::parserAV1OBU()
{
  // Reset all values in parserAV1OBU
  memset(&decValues, 0, sizeof(global_decoding_values));
  decValues.PrevFrameID = -1;
}

parserAV1OBU::obu_unit::obu_unit(QSharedPointer<obu_unit> obu_src)
{
  // Copy all members but the payload. The payload is only saved for specific obus.
  filePosStartEnd = obu_src->filePosStartEnd;
  obu_idx = obu_src->obu_idx;
  obu_type = obu_src->obu_type;
  obu_extension_flag = obu_src->obu_extension_flag;
  obu_has_size_field = obu_src->obu_has_size_field;
  temporal_id = obu_src->temporal_id;
  spatial_id = obu_src->spatial_id;
  obu_size = obu_src->obu_size;
}

int parserAV1OBU::obu_unit::parse_obu_header(const QByteArray &header_data, TreeItem *root)
{
  if (header_data.length() == 0)
    throw std::logic_error("The OBU header must have at least one byte");

  // Create a sub byte parser to access the bits
  sub_byte_reader reader(header_data);

  // Create a new TreeItem root for the item
  // The macros will use this variable to add all the parsed variables
  TreeItem *const itemTree = root ? new TreeItem("obu_header()", root) : nullptr;

  // Read forbidden_zeor_bit
  bool obu_forbidden_bit;
  READFLAG(obu_forbidden_bit);
  if (obu_forbidden_bit)
    throw std::logic_error("The obu_forbidden_bit bit was not zero.");

  QStringList obu_type_id_meaning = QStringList()
    << "Reserved"
    << "OBU_SEQUENCE_HEADER"
    << "OBU_TEMPORAL_DELIMITER"
    << "OBU_FRAME_HEADER"
    << "OBU_TILE_GROUP"
    << "OBU_METADATA"
    << "OBU_FRAME"
    << "OBU_REDUNDANT_FRAME_HEADER"
    << "OBU_TILE_LIST"
    << "Reserved"
    << "Reserved"
    << "Reserved"
    << "Reserved"
    << "Reserved"
    << "Reserved"
    << "OBU_PADDING";
  int obu_type_idx;
  READBITS_M(obu_type_idx, 4, obu_type_id_meaning);
  if (obu_type_idx == 15)
    obu_type = OBU_PADDING;
  else if (obu_type_idx > 8)
    obu_type = RESERVED;
  else
    obu_type = (obu_type_enum)obu_type_idx;

  READFLAG(obu_extension_flag);
  READFLAG(obu_has_size_field);

  bool obu_reserved_1bit;
  READFLAG(obu_reserved_1bit);
  if (obu_reserved_1bit)
    throw std::logic_error("The obu_reserved_1bit bit was not zero.");
  
  if (obu_extension_flag)
  {
      // obu_extension_header
      if (header_data.length() == 1)
        throw std::logic_error("The OBU header has an obu_extension_header and must have at least two byte");
      READBITS(temporal_id, 3);
      READBITS(spatial_id, 2);

      int extension_header_reserved_3bits;
      READBITS(extension_header_reserved_3bits, 3);
      if (extension_header_reserved_3bits != 0)
        throw std::logic_error("The extension_header_reserved_3bits must be 0.");
  }

  if (obu_has_size_field)
  {
    // Parse the size field
    READLEB128(obu_size);
  }

  return reader.nrBytesRead();
}

int parserAV1OBU::parseAndAddOBU(int obuID, QByteArray data, TreeItem *parent, QUint64Pair obuStartEndPosFile, QString *obuTypeName)
{
    sub_byte_reader r(data);
    
    // Use the given tree item. If it is not set, use the nalUnitMode (if active). 
    // We don't set data (a name) for this item yet. 
    // We want to parse the item and then set a good description.
    QString specificDescription;
    TreeItem *obuRoot = nullptr;
    if (parent)
      obuRoot = new TreeItem(parent);
    else if (!nalUnitModel.rootItem.isNull())
      obuRoot = new TreeItem(nalUnitModel.rootItem.data());

    // Read the OBU header
    obu_unit obu(obuStartEndPosFile, obuID);
    int nrBytesHeader = obu.parse_obu_header(data, obuRoot);

    // Get the payload of the OBU
    QByteArray obuData = data.mid(nrBytesHeader, obu.obu_size);

    if (obu.obu_type == OBU_TEMPORAL_DELIMITER)
    {
      decValues.SeenFrameHeader = false;
    }
    if (obu.obu_type == OBU_SEQUENCE_HEADER)
    {
      // A sequence parameter set
      auto new_sequence_header = QSharedPointer<sequence_header>(new sequence_header(obu));
      new_sequence_header->parse_sequence_header(obuData, obuRoot);

      active_sequence_header = new_sequence_header;

      if (obuTypeName)
        *obuTypeName = "SEQ_HEAD";
    }
    else if (obu.obu_type == OBU_FRAME || obu.obu_type == OBU_FRAME_HEADER)
    {
      auto new_frame_header = QSharedPointer<frame_header>(new frame_header(obu));
      new_frame_header->parse_frame_header(obuData, obuRoot, active_sequence_header, decValues);

      if (obuTypeName)
        *obuTypeName = "FRAME";
    }

    if (obuRoot)
      // Set a useful name of the TreeItem (the root for this NAL)
      obuRoot->itemData.append(QString("OBU %1: %2").arg(obu.obu_idx).arg(obu_type_toString.value(obu.obu_type)) + specificDescription);

    return nrBytesHeader + obu.obu_size;

}

void parserAV1OBU::sequence_header::parse_sequence_header(const QByteArray &sequenceHeaderData, TreeItem *root)
{
  obuPayload = sequenceHeaderData;
  sub_byte_reader reader(sequenceHeaderData);

  // Create a new TreeItem root for the item
  // The macros will use this variable to add all the parsed variables
  TreeItem *const itemTree = root ? new TreeItem("sequence_header_obu()", root) : nullptr;

  QStringList seq_profile_meaning = QStringList()
    << "Main Profile: Bit depth 8 or 10 bit, Monochrome support, Subsampling YUV 4:2:0"
    << "High Profile: Bit depth 8 or 10 bit, No Monochrome support, Subsampling YUV 4:2:0 or 4:4:4"
    << "Professional Profile: Bit depth 8, 10 or 12 bit, Monochrome support, Subsampling YUV 4:2:0, 4:2:2 or 4:4:4"
    << "Reserved";
  READBITS_M(seq_profile, 3, seq_profile_meaning);
  READFLAG(still_picture);
  READFLAG(reduced_still_picture_header);

  if (reduced_still_picture_header) 
  {
    timing_info_present_flag = false;
    decoder_model_info_present_flag = false;
    initial_display_delay_present_flag = false;
    operating_points_cnt_minus_1 = 0;
    operating_point_idc.append(0);
    READBITS_A(seq_level_idx, 5, 0);
    seq_tier.append(false);
    decoder_model_present_for_this_op.append(false);
    initial_display_delay_present_for_this_op.append(false);
  }
  else
  {
    READFLAG(timing_info_present_flag);
    if (timing_info_present_flag)
    {
      timing_info.read(reader, itemTree);
      READFLAG(decoder_model_info_present_flag);
      if (decoder_model_info_present_flag)
      {
        decoder_model_info.read(reader, itemTree);
      }
    }
    else
    {
      decoder_model_info_present_flag = false;
    }
    READFLAG(initial_display_delay_present_flag);
    READBITS(operating_points_cnt_minus_1, 5);
    for (int i = 0; i <= operating_points_cnt_minus_1; i++)
    {
      READBITS_A(operating_point_idc, 12, i);
      READBITS_A(seq_level_idx, 5, i);
      if (seq_level_idx[i] > 7)
      {
        READFLAG_A(seq_tier, i);
      }
      else 
      {
        seq_tier.append(false);
      }
      if (decoder_model_info_present_flag)
      {
        READFLAG_A(decoder_model_present_for_this_op, i);
        if (decoder_model_present_for_this_op[i])
        {
          operating_parameters_info.read(reader, root, i, decoder_model_info);
        }
      }
      else
      {
        decoder_model_present_for_this_op.append(0);
      }
      if (initial_display_delay_present_flag)
      {
        READFLAG_A(initial_display_delay_present_for_this_op, i);
        if (initial_display_delay_present_for_this_op[i])
        {
          READBITS_A(initial_display_delay_minus_1, 4, i);
        }
      }
    }
  }

  operatingPoint = choose_operating_point();
  OperatingPointIdc = operating_point_idc[operatingPoint];

  READBITS(frame_width_bits_minus_1, 4);
  READBITS(frame_height_bits_minus_1, 4);
  READBITS(max_frame_width_minus_1, frame_width_bits_minus_1+1);
  READBITS(max_frame_height_minus_1, frame_height_bits_minus_1+1);
  if (reduced_still_picture_header)
    frame_id_numbers_present_flag = 0;
  else
    READFLAG(frame_id_numbers_present_flag);
  if (frame_id_numbers_present_flag)
  {
    READBITS(delta_frame_id_length_minus_2, 4);
    READBITS(additional_frame_id_length_minus_1, 3);
  }
  READFLAG(use_128x128_superblock);
  READFLAG(enable_filter_intra);
  READFLAG(enable_intra_edge_filter);

  if (reduced_still_picture_header)
  {
    enable_interintra_compound = false;
    enable_masked_compound = false;
    enable_warped_motion = false;
    enable_dual_filter = false;
    enable_order_hint = false;
    enable_jnt_comp = false;
    enable_ref_frame_mvs = false;
    seq_force_screen_content_tools = SELECT_SCREEN_CONTENT_TOOLS;
    seq_force_integer_mv = SELECT_INTEGER_MV;
    OrderHintBits = 0;
  }
  else
  {
    READFLAG(enable_interintra_compound);
    READFLAG(enable_masked_compound);
    READFLAG(enable_warped_motion);
    READFLAG(enable_dual_filter);
    READFLAG(enable_order_hint);
    if (enable_order_hint)
    {
      READFLAG(enable_jnt_comp);
      READFLAG(enable_ref_frame_mvs);
    }
    else
    {
      enable_jnt_comp = false;
      enable_ref_frame_mvs = false;
    }
    READFLAG(seq_choose_screen_content_tools);
    if (seq_choose_screen_content_tools)
    {
      seq_force_screen_content_tools = SELECT_SCREEN_CONTENT_TOOLS;
      LOGVAL(seq_force_screen_content_tools);
    }
    else
      READBITS(seq_force_screen_content_tools, 1);
    
    if (seq_force_screen_content_tools > 0)
    {
      READFLAG(seq_choose_integer_mv);
      if (seq_choose_integer_mv)
      {
        seq_force_integer_mv = SELECT_INTEGER_MV;
        LOGVAL(seq_force_integer_mv);
      }
      else
        READBITS(seq_force_integer_mv, 1);
    }
    else
    {
      seq_force_integer_mv = SELECT_INTEGER_MV;
      LOGVAL(seq_force_integer_mv);
    }
    if (enable_order_hint)
    {
      READBITS(order_hint_bits_minus_1, 3);
      OrderHintBits = order_hint_bits_minus_1 + 1;
    }
    else
    {
      OrderHintBits = 0;
    }
    LOGVAL(OrderHintBits);
  }

  READFLAG(enable_superres);
  READFLAG(enable_cdef);
  READFLAG(enable_restoration);

  color_config.read(reader, itemTree, seq_profile);

  READFLAG(film_grain_params_present);
}

void parserAV1OBU::sequence_header::timing_info_struct::read(sub_byte_reader &reader, TreeItem *root)
{
  // Create a new TreeItem root for the item. The macros will use this variable to add all the parsed variables.
  TreeItem *const itemTree = root ? new TreeItem("timing_info()", root) : nullptr;

  READBITS(num_units_in_display_tick, 32);
  READBITS(time_scale, 32);
  READFLAG(equal_picture_interval);
  if (equal_picture_interval)
    READUVLC(num_ticks_per_picture_minus_1);
}

void parserAV1OBU::sequence_header::decoder_model_info_struct::read(sub_byte_reader &reader, TreeItem *root)
{
  // Create a new TreeItem root for the item. The macros will use this variable to add all the parsed variables.
  TreeItem *const itemTree = root ? new TreeItem("decoder_model_info()", root) : nullptr;

  READBITS(buffer_delay_length_minus_1, 5);
  READBITS(num_units_in_decoding_tick, 32);
  READBITS(buffer_removal_time_length_minus_1, 5);
  READBITS(frame_presentation_time_length_minus_1, 5);
}

void parserAV1OBU::sequence_header::operating_parameters_info_struct::read(sub_byte_reader &reader, TreeItem *root, int op, decoder_model_info_struct &dmodel)
{
  // Create a new TreeItem root for the item. The macros will use this variable to add all the parsed variables.
  TreeItem *const itemTree = root ? new TreeItem("operating_parameters_info()", root) : nullptr;

  int n = dmodel.buffer_delay_length_minus_1 + 1;
  READBITS_A(decoder_buffer_delay, n, op);
  READBITS_A(encoder_buffer_delay, n, op);
  READFLAG_A(low_delay_mode_flag, op);
}

void parserAV1OBU::sequence_header::color_config_struct::read(sub_byte_reader &reader, TreeItem *root, int seq_profile)
{
  // Create a new TreeItem root for the item. The macros will use this variable to add all the parsed variables.
  TreeItem *const itemTree = root ? new TreeItem("color_config()", root) : nullptr;

  READFLAG(high_bitdepth);
  if (seq_profile == 2 && high_bitdepth) 
  {
    READFLAG(twelve_bit);
    BitDepth = twelve_bit ? 12 : 10;
  }
  else if (seq_profile <= 2) 
  {
    BitDepth = high_bitdepth ? 10 : 8;
  }
  LOGVAL(BitDepth);

  if (seq_profile == 1) 
    mono_chrome = 0;
  else 
    READFLAG(mono_chrome)

  NumPlanes = mono_chrome ? 1 : 3;
  LOGVAL(NumPlanes);

  READFLAG(color_description_present_flag);
  if (color_description_present_flag) 
  {
    QStringList color_primaries_meaning = QStringList()
      << "Unused"
      << "BT.709" 
      << "Unspecified"
      << "Unused"
      << "BT.470 System M (historical)"
      << "BT.470 System B, G (historical)"
      << "BT.601"
      << "SMPTE 240"
      << "Generic film (color filters using illuminant C)"
      << "BT.2020, BT.2100"
      << "SMPTE 428 (CIE 1921 XYZ)"
      << "SMPTE RP 431-2"
      << "SMPTE EG 432-1"
      << "Unused" << "Unused" << "Unused" << "Unused" << "Unused" << "Unused" << "Unused" << "Unused" << "Unused"
      << "EBU Tech. 3213-E"
      << "Unused";
    READBITS_M_E(color_primaries, 8, color_primaries_meaning, color_primaries_enum);

    QStringList transfer_characteristics_meaning = QStringList()
      << "For future use"
      << "BT.709"
      << "Unspecified"
      << "For future use"
      << "BT.470 System M (historical)"
      << "BT.470 System B, G (historical)"
      << "BT.601"
      << "SMPTE 240 M"
      << "Linear"
      << "Logarithmic (100 : 1 range)"
      << "Logarithmic (100 * Sqrt(10) : 1 range)"
      << "IEC 61966-2-4"
      << "BT.1361"
      << "sRGB or sYCC"
      << "BT.2020 10-bit systems"
      << "BT.2020 12-bit systems"
      << "SMPTE ST 2084, ITU BT.2100 PQ"
      << "SMPTE ST 428"
      << "BT.2100 HLG, ARIB STD-B67"
      << "Unused";
    READBITS_M_E(transfer_characteristics, 8, transfer_characteristics_meaning, transfer_characteristics_enum);

    QStringList matrix_coefficients_meaning = QStringList()
      << "Identity matrix"
      << "BT.709"
      << "Unspecified"
      << "For future use"
      << "US FCC 73.628"
      << "BT.470 System B, G (historical)"
      << "BT.601"
      << "SMPTE 240 M"
      << "YCgCo"
      << "BT.2020 non-constant luminance, BT.2100 YCbCr"
      << "BT.2020 constant luminance"
      << "SMPTE ST 2085 YDzDx"
      << "Chromaticity-derived non-constant luminance"
      << "Chromaticity-derived constant luminance"
      << "BT.2100 ICtCp"
      << "Unused";
    READBITS_M_E(matrix_coefficients, 8, matrix_coefficients_meaning, matrix_coefficients_enum);
  }
  else
  {
    color_primaries = CP_UNSPECIFIED;
    transfer_characteristics = TC_UNSPECIFIED;
    matrix_coefficients = MC_UNSPECIFIED;
  }

  if (mono_chrome)
  {
    READFLAG(color_range);
    subsampling_x = true;
    subsampling_y = true;
    chroma_sample_position = CSP_UNKNOWN;
    separate_uv_delta_q = false;
  }
  else if (color_primaries == CP_BT_709 && transfer_characteristics == TC_SRGB && matrix_coefficients == MC_IDENTITY)
  {
    color_range = 1;
    subsampling_x = false;
    subsampling_y = false;
  }
  else
  {
    READFLAG(color_range);
    if (seq_profile == 0) 
    {
      subsampling_x = true;
      subsampling_y = true;
    } 
    else if (seq_profile == 1) 
    {
      subsampling_x = false;
      subsampling_y = false;
    }
    else
    {
      if (BitDepth == 12) 
      {
        READFLAG(subsampling_x);
        if (subsampling_x)
          READFLAG(subsampling_y)
        else
          subsampling_y = false;
      } 
      else 
      {
        subsampling_x = true;
        subsampling_y = false;
      }
    }
    if ( subsampling_x && subsampling_y ) 
    {
      QStringList chroma_sample_position_meaning = QStringList()
        << "Unknown (in this case the source video transfer function must be signaled outside the AV1 bitstream)"
        << "Horizontally co-located with (0, 0) luma sample, vertical position in the middle between two luma samples"
        << "co-located with (0, 0) luma sample"
        << "Reserved";
      READBITS_M_E(chroma_sample_position, 2, chroma_sample_position_meaning, chroma_sample_position_enum);
    }
  }
  if (!mono_chrome)
    READFLAG(separate_uv_delta_q)

  QString subsamplingFormat = "Unknown";
  if (subsampling_x && subsampling_y)
    subsamplingFormat = "4:2:0";
  else if (subsampling_x && !subsampling_y)
    subsamplingFormat = "4:2:2";
  else if (!subsampling_x && !subsampling_y)
    subsamplingFormat = "4:4:4";
  LOGSTRVAL("Subsampling format", subsamplingFormat)
}

void parserAV1OBU::frame_header::parse_frame_header(const QByteArray &frameHeaderData, TreeItem *root, QSharedPointer<sequence_header> seq_header, global_decoding_values &decValues)
{
  obuPayload = frameHeaderData;
  sub_byte_reader reader(frameHeaderData);

  // Create a new TreeItem root for the item
  // The macros will use this variable to add all the parsed variables
  TreeItem *const itemTree = root ? new TreeItem("frame_header_obu()", root) : nullptr;

  if (decValues.SeenFrameHeader)
  {
    // TODO: Is it meant like this?
    //frame_header_copy();
    uncompressed_header(reader, itemTree, seq_header, decValues);
  } 
  else 
  {
    decValues.SeenFrameHeader = true;
    uncompressed_header(reader, itemTree, seq_header, decValues);
    if (show_existing_frame) 
    {
      // decode_frame_wrapup()
      decValues.SeenFrameHeader = false;
    } 
    else 
    {
      //TileNum = 0;
      decValues.SeenFrameHeader = true;
    }
  }
}

void parserAV1OBU::frame_header::uncompressed_header(sub_byte_reader &reader, TreeItem *root, QSharedPointer<sequence_header> seq_header, global_decoding_values &decValues)
{
  // Create a new TreeItem root for the item
  // The macros will use this variable to add all the parsed variables
  TreeItem *const itemTree = root ? new TreeItem("uncompressed_header()", root) : nullptr;

  int idLen;
  if (seq_header->frame_id_numbers_present_flag)
  {
    idLen = seq_header->additional_frame_id_length_minus_1 + seq_header->delta_frame_id_length_minus_2 + 3;
  }
  int allFrames = (1 << NUM_REF_FRAMES) - 1;
  
  if (seq_header->reduced_still_picture_header)
  {
    show_existing_frame = false;
    frame_type = KEY_FRAME;
    FrameIsIntra = true;
    show_frame = true;
    showable_frame = false;
  }
  else
  {
    READFLAG(show_existing_frame);
    if (show_existing_frame)
    {
      READBITS(frame_to_show_map_idx, 3);
      if (seq_header->decoder_model_info_present_flag && !seq_header->timing_info.equal_picture_interval)
      {
        //temporal_point_info();
      }
      refresh_frame_flags = 0;
      if (seq_header->frame_id_numbers_present_flag) 
      {
        READBITS(display_frame_id, idLen);
      }
      frame_type = decValues.RefFrameType[frame_to_show_map_idx];
      if (frame_type == KEY_FRAME)
        refresh_frame_flags = allFrames;
      if (seq_header->film_grain_params_present)
      {
        //load_grain_params(frame_to_show_map_idx);
        assert(false);
      }
      return;
    }

    QStringList frame_type_meaning = QStringList() << "KEY_FRAME" << "INTER_FRAME" << "INTRA_ONLY_FRAME" << "SWITCH_FRAME";
    READBITS_M_E(frame_type, 2, frame_type_meaning, frame_type_enum);
    FrameIsIntra = (frame_type == INTRA_ONLY_FRAME || frame_type == KEY_FRAME);
    READFLAG(show_frame);
    if (show_frame && seq_header->decoder_model_info_present_flag && !seq_header->timing_info.equal_picture_interval)
    {
      //temporal_point_info();
    }
    if (show_frame)
      showable_frame = frame_type != KEY_FRAME;
    else
      READFLAG(showable_frame);
    if (frame_type == SWITCH_FRAME || (frame_type == KEY_FRAME && show_frame))
      error_resilient_mode = true;
    else
      READFLAG(error_resilient_mode);
  }

  if (frame_type == KEY_FRAME && show_frame) 
  {
    for (int i = 0; i < NUM_REF_FRAMES; i++)
    {
      decValues.RefValid[i] = 0;
      decValues.RefOrderHint[i] = 0;
    }
    for (int i = 0; i < REFS_PER_FRAME; i++)
      decValues.OrderHints[LAST_FRAME + i] = 0;
  }

  READFLAG(disable_cdf_update);
  if (seq_header->seq_force_screen_content_tools == SELECT_SCREEN_CONTENT_TOOLS) 
    READBITS(allow_screen_content_tools, 1)
  else
    allow_screen_content_tools = seq_header->seq_force_screen_content_tools;

  if (allow_screen_content_tools)
  {
    if (seq_header->seq_force_integer_mv == SELECT_INTEGER_MV)
      READBITS(force_integer_mv, 1)
    else
      force_integer_mv = seq_header->seq_force_integer_mv;
  }
  else
    force_integer_mv = 0;
  
  if (FrameIsIntra)
    force_integer_mv = 1;
  
  if (seq_header->frame_id_numbers_present_flag)
  {
    decValues.PrevFrameID = decValues.current_frame_id;
    READBITS(current_frame_id, idLen);
    decValues.current_frame_id = current_frame_id;
    mark_ref_frames(idLen, seq_header, decValues);
  }
  else
    current_frame_id = 0;

  if (frame_type == SWITCH_FRAME)
    frame_size_override_flag = true;
  else if (seq_header->reduced_still_picture_header)
    frame_size_override_flag = false;
  else
    READFLAG(frame_size_override_flag)

  READBITS(order_hint, seq_header->OrderHintBits);
  OrderHint = order_hint;

  if (FrameIsIntra || error_resilient_mode)
    primary_ref_frame = PRIMARY_REF_NONE;
  else
    READBITS(primary_ref_frame, 3)

  if (seq_header->decoder_model_info_present_flag)
  {
    READFLAG(buffer_removal_time_present_flag);
    if (buffer_removal_time_present_flag)
    {
      for (int opNum = 0; opNum <= seq_header->operating_points_cnt_minus_1; opNum++)
      {
        if (seq_header->decoder_model_present_for_this_op[opNum])
        {
          opPtIdc = seq_header->operating_point_idc[opNum];
          inTemporalLayer = (opPtIdc >> temporal_id) & 1;
          inSpatialLayer = (opPtIdc >> (spatial_id + 8)) & 1;
          if (opPtIdc == 0 || (inTemporalLayer && inSpatialLayer))
          {
            int n = seq_header->decoder_model_info.buffer_removal_time_length_minus_1 + 1;
            READBITS_A(buffer_removal_time, n, opNum);
          }
        }
      }
    }
  }

  allow_high_precision_mv = false;
  use_ref_frame_mvs = false;
  allow_intrabc = false;

  if (frame_type == SWITCH_FRAME || (frame_type == KEY_FRAME && show_frame))
    refresh_frame_flags = allFrames;
  else
    READBITS(refresh_frame_flags, 8)
  
  if (!FrameIsIntra || refresh_frame_flags != allFrames)
  {
    if (error_resilient_mode && seq_header->enable_order_hint)
    {
      for (int i = 0; i < NUM_REF_FRAMES; i++) 
      {
        READBITS_A(ref_order_hint, seq_header->OrderHintBits, i);
        if (ref_order_hint[i] != decValues.RefOrderHint[i])
          decValues.RefValid[i] = false;
      } 
    }
  }

  if (frame_type == KEY_FRAME)
  {
    frame_size(reader, itemTree, seq_header);
    render_size(reader, itemTree);
    if (allow_screen_content_tools && UpscaledWidth == FrameWidth)
      READFLAG(allow_intrabc);
  }
  else
  {
    if (frame_type == INTRA_ONLY_FRAME)
    {
      frame_size(reader, itemTree, seq_header);
      render_size(reader, itemTree);
      if (allow_screen_content_tools && UpscaledWidth == FrameWidth)
        READFLAG(allow_intrabc);
    }
    else
    {
      if (!seq_header->enable_order_hint)
        frame_refs_short_signaling = false;
      else
      {
        READFLAG(frame_refs_short_signaling);
        if (frame_refs_short_signaling)
        {
          READBITS(last_frame_idx, 3);
          READBITS(gold_frame_idx, 3);
          frame_refs.set_frame_refs(seq_header->OrderHintBits, seq_header->enable_order_hint, last_frame_idx, gold_frame_idx, OrderHint, decValues);
        }
      }
      for (int i = 0; i < REFS_PER_FRAME; i++)
      {
        if (!frame_refs_short_signaling)
          READBITS(frame_refs.ref_frame_idx[i], 3)
        if (seq_header->frame_id_numbers_present_flag)
        {
          int n = seq_header->delta_frame_id_length_minus_2 + 2;
          READBITS(delta_frame_id_minus_1, n);
          int DeltaFrameId = delta_frame_id_minus_1 + 1;
          expectedFrameId[i] = ((current_frame_id + (1 << idLen) - DeltaFrameId) % (1 << idLen));
        }
      }
      if (frame_size_override_flag && !error_resilient_mode)
        frame_size_with_refs(reader, itemTree, seq_header, decValues);
      else
      {
        frame_size(reader, itemTree, seq_header);
        render_size(reader, itemTree);
      }
      if (force_integer_mv)
        allow_high_precision_mv = false;
      else
        READFLAG(allow_high_precision_mv);
      read_interpolation_filter(reader, itemTree);
      READFLAG(is_motion_mode_switchable);
      if (error_resilient_mode || !seq_header->enable_ref_frame_mvs)
        use_ref_frame_mvs = false;
      else
        READFLAG(use_ref_frame_mvs);
    }
  }

  if (seq_header->reduced_still_picture_header || disable_cdf_update)
    disable_frame_end_update_cdf = true;
  else
    READFLAG(disable_frame_end_update_cdf);
  if (primary_ref_frame == PRIMARY_REF_NONE)
  {
    LOGINFO("init_non_coeff_cdfs()");
    LOGINFO("setup_past_independence()");
  }
  else
  {
    LOGINFO("load_cdfs(ref_frame_idx[primary_ref_frame])");
    LOGINFO("load_previous()");
  }
  if (use_ref_frame_mvs)
    LOGINFO("motion_field_estimation()");
  
  tile_info.parse(MiCols, MiRows, reader, itemTree, seq_header);

  quantization_params.parse(reader, itemTree, seq_header);
  segmentation_params.parse(primary_ref_frame, reader, itemTree);
  delta_q_params.parse(quantization_params.base_q_idx, reader, itemTree);
  delta_lf_params.parse(delta_q_params.delta_q_present, allow_intrabc, reader, itemTree);
  // if (primary_ref_frame == PRIMARY_REF_NONE)
  //   init_coeff_cdfs()
  // else
  //   load_previous_segment_ids();
  CodedLossless = true;
  for (int segmentId = 0; segmentId < MAX_SEGMENTS; segmentId++)
  {
    int qindex = get_qindex(true, segmentId);
    LosslessArray[segmentId] = (qindex == 0 && quantization_params.DeltaQYDc == 0 && quantization_params.DeltaQUAc == 0 && quantization_params.DeltaQUDc == 0 && quantization_params.DeltaQVAc == 0 && quantization_params.DeltaQVDc == 0);
    if (!LosslessArray[segmentId])
      CodedLossless = false;
    if (quantization_params.using_qmatrix)
    {
      if (LosslessArray[segmentId]) 
      {
        SegQMLevel[0][segmentId] = 15;
        SegQMLevel[1][segmentId] = 15;
        SegQMLevel[2][segmentId] = 15;
      }
      else
      {
        SegQMLevel[0][segmentId] = quantization_params.qm_y;
        SegQMLevel[1][segmentId] = quantization_params.qm_u;
        SegQMLevel[2][segmentId] = quantization_params.qm_v;
      } 
    }
  }
  AllLossless = CodedLossless && (FrameWidth == UpscaledWidth);
  loop_filter_params.parse(CodedLossless, allow_intrabc, reader, itemTree, seq_header);
  cdef_params.parse(CodedLossless, allow_intrabc, reader, itemTree, seq_header);
  // lr_params
  // read_tx_mode
  // frame_reference_mode
  // skip_mode_params

  if (FrameIsIntra || error_resilient_mode || !seq_header->enable_warped_motion)
    allow_warped_motion = false;
  else
    READFLAG(allow_warped_motion);
  READFLAG(reduced_tx_set);
  //global_motion_params( )
  //film_grain_params( )
}

void parserAV1OBU::frame_header::mark_ref_frames(int idLen, QSharedPointer<sequence_header> seq_header, global_decoding_values &decValues)
{
  int diffLen = seq_header->delta_frame_id_length_minus_2 + 2;
  for (int i = 0; i < NUM_REF_FRAMES; i++)
  {
    if (current_frame_id > (1 << diffLen))
    {
      if (decValues.RefFrameId[i] > current_frame_id || decValues.RefFrameId[i] < (current_frame_id - (1 << diffLen)))
        decValues.RefValid[i] = 0;
    }
    else
    {
      if (decValues.RefFrameId[i] > current_frame_id && decValues.RefFrameId[i] < ((1 << idLen) + current_frame_id - (1 << diffLen)))
        decValues.RefValid[i] = 0;
    }
  }
}

void parserAV1OBU::frame_header::frame_size(sub_byte_reader &reader, TreeItem *root, QSharedPointer<sequence_header> seq_header)
{
  // Create a new TreeItem root for the item
  // The macros will use this variable to add all the parsed variables
  TreeItem *const itemTree = root ? new TreeItem("frame_size()", root) : nullptr;

  if (frame_size_override_flag)
  {
    READBITS(frame_width_minus_1, seq_header->frame_width_bits_minus_1+1);
    READBITS(frame_height_minus_1, seq_header->frame_height_bits_minus_1+1);
    FrameWidth = frame_width_minus_1 + 1;
    FrameHeight = frame_height_minus_1 + 1;
  }
  else
  {
    FrameWidth = seq_header->max_frame_width_minus_1 + 1;
    FrameHeight = seq_header->max_frame_height_minus_1 + 1;
  }

  LOGVAL(FrameWidth);
  LOGVAL(FrameHeight);

  superres_params(reader, root, seq_header);
  compute_image_size();
}

void parserAV1OBU::frame_header::superres_params(sub_byte_reader &reader, TreeItem *root, QSharedPointer<sequence_header> seq_header)
{
  // Create a new TreeItem root for the item
  // The macros will use this variable to add all the parsed variables
  TreeItem *const itemTree = root ? new TreeItem("superres_params()", root) : nullptr;

  if (seq_header->enable_superres)
    READFLAG(use_superres)
  else
    use_superres = false;

  if (use_superres)
  {
    READBITS(coded_denom, SUPERRES_DENOM_BITS);
    SuperresDenom = coded_denom + SUPERRES_DENOM_MIN;
  }
  else
    SuperresDenom = SUPERRES_NUM;

  UpscaledWidth = FrameWidth;
  FrameWidth =  (UpscaledWidth * SUPERRES_NUM + (SuperresDenom / 2)) / SuperresDenom;
  LOGVAL(FrameWidth);
}

void parserAV1OBU::frame_header::compute_image_size()
{
  MiCols = 2 * ((FrameWidth + 7 ) >> 3);
  MiRows = 2 * ((FrameHeight + 7 ) >> 3);
}

void parserAV1OBU::frame_header::render_size(sub_byte_reader &reader, TreeItem *root)
{
  // Create a new TreeItem root for the item
  // The macros will use this variable to add all the parsed variables
  TreeItem *const itemTree = root ? new TreeItem("render_size()", root) : nullptr;

  READFLAG(render_and_frame_size_different);
  if (render_and_frame_size_different)
  {
    READBITS(render_width_minus_1, 16);
    READBITS(render_height_minus_1, 16);
    RenderWidth = render_width_minus_1 + 1;
    RenderHeight = render_height_minus_1 + 1;
  }
  else
  {
    RenderWidth = UpscaledWidth;
    RenderHeight = FrameHeight;
  }
  LOGVAL(RenderWidth);
  LOGVAL(RenderHeight);
}

void parserAV1OBU::frame_header::frame_size_with_refs(sub_byte_reader &reader, TreeItem *root, QSharedPointer<sequence_header> seq_header, global_decoding_values &decValues)
{
  // Create a new TreeItem root for the item
  // The macros will use this variable to add all the parsed variables
  TreeItem *const itemTree = root ? new TreeItem("frame_size_with_refs()", root) : nullptr;

  bool ref_found = false;
  for (int i = 0; i < REFS_PER_FRAME; i++)
  {
    bool found_ref;
    READFLAG(found_ref);
    if (found_ref)
    {
      UpscaledWidth = decValues.RefUpscaledWidth[frame_refs.ref_frame_idx[i]];
      FrameWidth = UpscaledWidth;
      FrameHeight = decValues.RefFrameHeight[frame_refs.ref_frame_idx[i]];
      RenderWidth = decValues.RefRenderWidth[frame_refs.ref_frame_idx[i]];
      RenderHeight = decValues.RefRenderHeight[frame_refs.ref_frame_idx[i]];
      ref_found = true;
      LOGVAL(FrameWidth);
      LOGVAL(FrameHeight);
      LOGVAL(RenderWidth);
      LOGVAL(RenderHeight);
      break;
    }
  }
  if (!ref_found)
  {
    frame_size(reader, itemTree, seq_header);
    render_size(reader, itemTree);
  }
  else
  {
    superres_params(reader, root, seq_header);
    compute_image_size();
  }
}

void parserAV1OBU::frame_header::frame_refs_struct::set_frame_refs(int OrderHintBits, bool enable_order_hint, int last_frame_idx, int gold_frame_idx, int OrderHint, global_decoding_values &decValues)
{
  for (int i = 0; i < REFS_PER_FRAME; i++)
    ref_frame_idx[i] = -1;
  ref_frame_idx[LAST_FRAME - LAST_FRAME] = last_frame_idx;
  ref_frame_idx[GOLDEN_FRAME - LAST_FRAME] = gold_frame_idx;

  for (int i = 0; i < NUM_REF_FRAMES; i++)
    usedFrame[i] = false;
  usedFrame[last_frame_idx] = true;
  usedFrame[gold_frame_idx] = true;

  int curFrameHint = 1 << (OrderHintBits - 1);
  for (int i = 0; i < NUM_REF_FRAMES; i++)
    shiftedOrderHints[i] = curFrameHint + get_relative_dist(decValues.RefOrderHint[i], OrderHint, enable_order_hint, OrderHintBits);

  int lastOrderHint = shiftedOrderHints[last_frame_idx];
  if (lastOrderHint >= curFrameHint)
    throw std::logic_error("It is a requirement of bitstream conformance that lastOrderHint is strictly less than curFrameHint.");

  int goldOrderHint = shiftedOrderHints[gold_frame_idx];
  if (goldOrderHint >= curFrameHint)
    throw std::logic_error("It is a requirement of bitstream conformance that goldOrderHint is strictly less than curFrameHint.");

  int ref = find_latest_backward(curFrameHint);
  if (ref >= 0)
  {
    ref_frame_idx[ALTREF_FRAME - LAST_FRAME] = ref;
    usedFrame[ref] = true;
  }

  ref = find_earliest_backward(curFrameHint);
  if (ref >= 0) 
  {
    ref_frame_idx[BWDREF_FRAME - LAST_FRAME] = ref;
    usedFrame[ref] = true;
  }

  ref = find_earliest_backward(curFrameHint);
  if (ref >= 0)
  {
    ref_frame_idx[ALTREF2_FRAME - LAST_FRAME] = ref;
    usedFrame[ref] = true;
  }

  int Ref_Frame_List[REFS_PER_FRAME - 2] = {LAST2_FRAME, LAST3_FRAME, BWDREF_FRAME, ALTREF2_FRAME, ALTREF_FRAME};
  for (int i = 0; i < REFS_PER_FRAME - 2; i++)
  {
    int refFrame = Ref_Frame_List[i];
    if (ref_frame_idx[refFrame - LAST_FRAME] < 0 )
    {
      ref = find_latest_forward(curFrameHint);
      if (ref >= 0)
      {
        ref_frame_idx[refFrame - LAST_FRAME] = ref;
        usedFrame[ref] = true;
      }
    }
  }

  // Finally, any remaining references are set to the reference frame with smallest output order as follows:
  ref = -1;
  for (int i = 0; i < NUM_REF_FRAMES; i++)
  {
    int hint = shiftedOrderHints[i];
    if (ref < 0 || hint < earliestOrderHint)
    {
      ref = i;
      earliestOrderHint = hint;
    }
  }
  for (int i = 0; i < REFS_PER_FRAME; i++)
  {
    if (ref_frame_idx[i] < 0 )
      ref_frame_idx[i] = ref;
  }
}

int parserAV1OBU::frame_header::frame_refs_struct::find_latest_backward(int curFrameHint)
{
  int ref = -1;
  for (int i = 0; i < NUM_REF_FRAMES; i++) 
  {
    int hint = shiftedOrderHints[i];
    if (!usedFrame[i] && hint >= curFrameHint && (ref < 0 || hint >= latestOrderHint)) 
    {
      ref = i;
      latestOrderHint = hint;
    }
  }
  return ref;
}

int parserAV1OBU::frame_header::frame_refs_struct::find_earliest_backward(int curFrameHint)
{
  int ref = -1;
  for (int i = 0; i < NUM_REF_FRAMES; i++)
  {
    int hint = shiftedOrderHints[i];
    if (!usedFrame[i] && hint >= curFrameHint && (ref < 0 || hint < earliestOrderHint))
    {
      ref = i;
      earliestOrderHint = hint;
    }
  }
  return ref;
}

int parserAV1OBU::frame_header::frame_refs_struct::find_latest_forward(int curFrameHint)
{
  int ref = -1;
  for (int i = 0; i < NUM_REF_FRAMES; i++)
  {
    int hint = shiftedOrderHints[i];
    if (!usedFrame[i] && hint < curFrameHint && (ref < 0 || hint >= latestOrderHint))
    {
      ref = i;
      latestOrderHint = hint;
    }
  }
  return ref;
}

int parserAV1OBU::frame_header::frame_refs_struct::get_relative_dist(int a, int b, bool enable_order_hint, int OrderHintBits)
{
  if (!enable_order_hint)
    return 0;

  int diff = a - b;
  int m = 1 << (OrderHintBits - 1);
  diff = (diff & (m - 1)) - (diff & m);
  return diff;
}

void parserAV1OBU::frame_header::read_interpolation_filter(sub_byte_reader &reader, TreeItem *root)
{
  // Create a new TreeItem root for the item
  // The macros will use this variable to add all the parsed variables
  TreeItem *const itemTree = root ? new TreeItem("read_interpolation_filter()", root) : nullptr;

  READFLAG(is_filter_switchable);
  if (is_filter_switchable)
    interpolation_filter = SWITCHABLE;
  else
  {
    QStringList interpolation_filter_meaning = QStringList() << "EIGHTTAP" << "EIGHTTAP_SMOOTH" << "EIGHTTAP_SHARP" << "BILINEAR" << "SWITCHABLE";
    READBITS_M_E(interpolation_filter, 2, interpolation_filter_meaning, interpolation_filter_enum);
  }
}

int tile_log2(int blkSize, int target)
{ 
  int k;
  for (k=0; (blkSize << k) < target; k++)
  {
  }
  return k;
}

void parserAV1OBU::frame_header::tile_info_struct::parse(int MiCols, int MiRows, sub_byte_reader &reader, TreeItem *root, QSharedPointer<sequence_header> seq_header)
{
  // Create a new TreeItem root for the item
  // The macros will use this variable to add all the parsed variables
  TreeItem *const itemTree = root ? new TreeItem("tile_info()", root) : nullptr;

  sbCols = seq_header->use_128x128_superblock ? ( ( MiCols + 31 ) >> 5 ) : ( ( MiCols + 15 ) >> 4 );
  sbRows = seq_header->use_128x128_superblock ? ( ( MiRows + 31 ) >> 5 ) : ( ( MiRows + 15 ) >> 4 );
  sbShift = seq_header->use_128x128_superblock ? 5 : 4;
  int sbSize = sbShift + 2;
  maxTileWidthSb = MAX_TILE_WIDTH >> sbSize;
  maxTileAreaSb = MAX_TILE_AREA >> ( 2 * sbSize );
  minLog2TileCols = tile_log2(maxTileWidthSb, sbCols);
  maxLog2TileCols = tile_log2(1, std::min(sbCols, MAX_TILE_COLS));
  maxLog2TileRows = tile_log2(1, std::min(sbRows, MAX_TILE_ROWS));
  minLog2Tiles = std::max(minLog2TileCols, tile_log2(maxTileAreaSb, sbRows * sbCols));

  READFLAG(uniform_tile_spacing_flag);
  if (uniform_tile_spacing_flag)
  {
    TileColsLog2 = minLog2TileCols;
    while (TileColsLog2 < maxLog2TileCols)
    {
      READFLAG(increment_tile_cols_log2);
      if (increment_tile_cols_log2)
        TileColsLog2++;
      else
        break;
    }
    tileWidthSb = (sbCols + (1 << TileColsLog2) - 1) >> TileColsLog2;
    TileCols = 0;
    for (int startSb = 0; startSb < sbCols; startSb += tileWidthSb)
    {
      MiColStarts.append(startSb << sbShift);      
      TileCols++;
    }
    MiColStarts.append(MiCols);

    minLog2TileRows = std::max(minLog2Tiles - TileColsLog2, 0);
    TileRowsLog2 = minLog2TileRows;
    while (TileRowsLog2 < maxLog2TileRows)
    {
      bool increment_tile_rows_log2;
      READFLAG(increment_tile_rows_log2);
      if (increment_tile_rows_log2)
        TileRowsLog2++;
      else
        break;
    }
    tileHeightSb = (sbRows + (1 << TileRowsLog2) - 1) >> TileRowsLog2;
    TileRows = 0;
    for (int startSb = 0; startSb < sbRows; startSb += tileHeightSb)
    {
      MiRowStarts.append(startSb << sbShift);
      TileRows++;
    }
    MiRowStarts.append(MiRows);
  }
  else
  {
    widestTileSb = 0;
    TileCols = 0;
    for (int startSb = 0; startSb < sbCols;)
    {
      MiColStarts.append(startSb << sbShift);
      int maxWidth = std::min(sbCols - startSb, maxTileWidthSb);
      READNS(width_in_sbs_minus_1, maxWidth);
      int sizeSb = width_in_sbs_minus_1 + 1;
      widestTileSb = std::max(sizeSb, widestTileSb);
      startSb += sizeSb;
      TileCols++;
    }
    MiColStarts.append(MiCols);
    TileColsLog2 = tile_log2(1, TileCols);

    if (minLog2Tiles > 0)
      maxTileAreaSb = (sbRows * sbCols) >> (minLog2Tiles + 1);
    else
      maxTileAreaSb = sbRows * sbCols;
    maxTileHeightSb = std::max(maxTileAreaSb / widestTileSb, 1);

    TileRows = 0;
    for (int startSb = 0; startSb < sbRows;)
    {
      MiRowStarts.append(startSb << sbShift);
      int maxHeight = std::min(sbRows - startSb, maxTileHeightSb);
      READNS(height_in_sbs_minus_1, maxHeight);
      int sizeSb = height_in_sbs_minus_1 + 1;
      startSb += sizeSb;
      TileRows++;
    }
    MiRowStarts.append(MiRows);
    TileRowsLog2 = tile_log2(1, TileRows);
  }
  if (TileColsLog2 > 0 || TileRowsLog2 > 0)
  {
    READBITS(context_update_tile_id, TileRowsLog2 + TileColsLog2);
    READBITS(tile_size_bytes_minus_1, 2);
    TileSizeBytes = tile_size_bytes_minus_1 + 1;
  }
  else
    context_update_tile_id = 0;
}

void parserAV1OBU::frame_header::quantization_params_struct::parse(sub_byte_reader &reader, TreeItem *root, QSharedPointer<sequence_header> seq_header)
{
  // Create a new TreeItem root for the item
  // The macros will use this variable to add all the parsed variables
  TreeItem *const itemTree = root ? new TreeItem("quantization_params()", root) : nullptr;

  READBITS(base_q_idx, 8);
  DeltaQYDc = read_delta_q("DeltaQYDc", reader, itemTree);
  if (seq_header->color_config.NumPlanes > 1)
  {
    if (seq_header->color_config.separate_uv_delta_q)
      READFLAG(diff_uv_delta)
    else
      diff_uv_delta = false;
    DeltaQUDc = read_delta_q("DeltaQUDc", reader, itemTree);
    DeltaQUAc = read_delta_q("DeltaQUAc", reader, itemTree);
    if (diff_uv_delta)
    {
      DeltaQVDc = read_delta_q("DeltaQVDc", reader, itemTree);
      DeltaQVAc = read_delta_q("DeltaQVAc", reader, itemTree);
    }
    else
    {
      DeltaQVDc = DeltaQUDc;
      DeltaQVAc = DeltaQUAc;
    }
  }
  else
  {
    DeltaQUDc = 0;
    DeltaQUAc = 0;
    DeltaQVDc = 0;
    DeltaQVAc = 0;
  }
  READFLAG(using_qmatrix);
  if (using_qmatrix)
  {
    READBITS(qm_y, 4);
    READBITS(qm_u, 4);
    if (!seq_header->color_config.separate_uv_delta_q)
      qm_v = qm_u;
    else
      READBITS(qm_v, 4);
  }
}

int parserAV1OBU::frame_header::quantization_params_struct::read_delta_q(QString deltaValName, sub_byte_reader &reader, TreeItem *root)
{
  // Use a new reeItem root for the item. Don't set a text yet.
  TreeItem *itemTree = nullptr;
  if (root)
    itemTree = new TreeItem(root);

  bool delta_coded;
  int delta_q;

  READFLAG(delta_coded);
  if (delta_coded)
  {
    READSU(delta_q, 1+6);
  }
  else
    delta_q = 0;
  
  if (itemTree)
  {
      // Set a useful name of the TreeItem (the root for this NAL)
      itemTree->itemData.append(deltaValName);
      itemTree->itemData.append(QString("%1").arg(delta_q));
  }

  return delta_q;
}

void parserAV1OBU::frame_header::segmentation_params_struct::parse(int primary_ref_frame, sub_byte_reader &reader, TreeItem *root)
{
  // Create a new TreeItem root for the item
  // The macros will use this variable to add all the parsed variables
  TreeItem *const itemTree = root ? new TreeItem("segmentation_params()", root) : nullptr;

  int Segmentation_Feature_Bits[SEG_LVL_MAX] = { 8, 6, 6, 6, 6, 3, 0, 0 };
  int Segmentation_Feature_Signed[SEG_LVL_MAX] = { 1, 1, 1, 1, 1, 0, 0, 0 };
  int Segmentation_Feature_Max[SEG_LVL_MAX] = { 255, MAX_LOOP_FILTER, MAX_LOOP_FILTER, MAX_LOOP_FILTER, MAX_LOOP_FILTER, 7, 0, 0 };

  READFLAG(segmentation_enabled);
  if (segmentation_enabled)
  {
    if (primary_ref_frame == PRIMARY_REF_NONE)
    {
      segmentation_update_map = 1;
      segmentation_temporal_update = 0;
      segmentation_update_data = 1;
    }
    else
    {
      READFLAG(segmentation_update_map);
      if (segmentation_update_map)
        READFLAG(segmentation_temporal_update);
      READFLAG(segmentation_update_data);
    }
    if (segmentation_update_data)
    {
      for (int i = 0; i < MAX_SEGMENTS; i++)
      {
        for (int j = 0; j < SEG_LVL_MAX; j++)
        {
          int feature_value = 0;
          bool feature_enabled;
          READFLAG(feature_enabled);
          FeatureEnabled[i][j] = feature_enabled;
          int clippedValue = 0;
          if (feature_enabled)
          {
            int bitsToRead = Segmentation_Feature_Bits[j];
            int limit = Segmentation_Feature_Max[j];
            if (Segmentation_Feature_Signed[j])
            {
              READSU(feature_value, 1+bitsToRead);
              clippedValue = clip(feature_value, -limit, limit);
            }
            else
            {
              READBITS(feature_value, bitsToRead);
              clippedValue = clip(feature_value, 0, limit);
            }
          }
          FeatureData[i][j] = clippedValue;
        }
      }
    }
  }
  else
  {
    for (int i = 0; i < MAX_SEGMENTS; i++)
    {
      for (int j = 0; j < SEG_LVL_MAX; j++)
      {
        FeatureEnabled[i][j] = 0;
        FeatureData[i][j] = 0;
      }
    }
  }
  SegIdPreSkip = false;
  LastActiveSegId = 0;
  for (int i = 0; i < MAX_SEGMENTS; i++)
  {
    for (int j = 0; j < SEG_LVL_MAX; j++)
    {
      if (FeatureEnabled[i][j])
      {
        LastActiveSegId = i;
        if (j >= SEG_LVL_REF_FRAME)
          SegIdPreSkip = true;
      }
    }
  }
}

void parserAV1OBU::frame_header::delta_q_params_struct::parse(int base_q_idx, sub_byte_reader &reader, TreeItem *root)
{
  // Create a new TreeItem root for the item
  // The macros will use this variable to add all the parsed variables
  TreeItem *const itemTree = root ? new TreeItem("delta_q_params()", root) : nullptr;

  delta_q_res = 0;
  delta_q_present = false;
  if (base_q_idx > 0)
    READFLAG(delta_q_present)
  if (delta_q_present)
    READBITS(delta_q_res, 2);
}

void parserAV1OBU::frame_header::delta_lf_params_struct::parse(bool delta_q_present, bool allow_intrabc, sub_byte_reader &reader, TreeItem *root)
{
  // Create a new TreeItem root for the item
  // The macros will use this variable to add all the parsed variables
  TreeItem *const itemTree = root ? new TreeItem("delta_lf_params()", root) : nullptr;

  delta_lf_present = false;
  delta_lf_res = 0;
  delta_lf_multi = false;
  if (delta_q_present)
  {
    if (!allow_intrabc)
      READFLAG(delta_lf_present);
    if (delta_lf_present)
    {
      READBITS(delta_lf_res, 2);
      READFLAG(delta_lf_multi);
    }
  }
}

int parserAV1OBU::frame_header::get_qindex(bool ignoreDeltaQ, int segmentId) const
{
  // TODO: This must be set somewhere!
  int CurrentQIndex = 0;

  if (seg_feature_active_idx(segmentId, SEG_LVL_ALT_Q))
  {
    int data = segmentation_params.FeatureData[segmentId][SEG_LVL_ALT_Q];
    int qindex = quantization_params.base_q_idx + data;
    if (!ignoreDeltaQ && delta_q_params.delta_q_present)
      qindex = CurrentQIndex + data;
    return clip(qindex, 0, 255);
  }
  else if (!ignoreDeltaQ && delta_q_params.delta_q_present)
    return CurrentQIndex;
  return quantization_params.base_q_idx;
}

void parserAV1OBU::frame_header::loop_filter_params_struct::parse(bool CodedLossless, bool allow_intrabc, sub_byte_reader &reader, TreeItem *root, QSharedPointer<sequence_header> seq_header)
{
  // Create a new TreeItem root for the item
  // The macros will use this variable to add all the parsed variables
  TreeItem *const itemTree = root ? new TreeItem("loop_filter_params()", root) : nullptr;

  if (CodedLossless || allow_intrabc)
  {
    loop_filter_level[0] = 0;
    loop_filter_level[1] = 0;
    loop_filter_ref_deltas[INTRA_FRAME] = 1;
    loop_filter_ref_deltas[LAST_FRAME] = 0;
    loop_filter_ref_deltas[LAST2_FRAME] = 0;
    loop_filter_ref_deltas[LAST3_FRAME] = 0;
    loop_filter_ref_deltas[BWDREF_FRAME] = 0;
    loop_filter_ref_deltas[GOLDEN_FRAME] = -1;
    loop_filter_ref_deltas[ALTREF_FRAME] = -1;
    loop_filter_ref_deltas[ALTREF2_FRAME] = -1;
    for (int i = 0; i < 2; i++)
      loop_filter_mode_deltas[i] = 0;
    return;
  }
  READBITS(loop_filter_level[0], 6);
  READBITS(loop_filter_level[1], 6);
  if (seq_header->color_config.NumPlanes > 1)
  {
    if (loop_filter_level[0] || loop_filter_level[1])
    {
      READBITS(loop_filter_level[2], 6);
      READBITS(loop_filter_level[3], 6);
    }
  }
  READBITS(loop_filter_sharpness, 3);
  READFLAG(loop_filter_delta_enabled);
  if (loop_filter_delta_enabled)
  {
    READFLAG(loop_filter_delta_update);
    if (loop_filter_delta_update)
    {
      for (int i = 0; i < TOTAL_REFS_PER_FRAME; i++)
      {
        bool update_ref_delta;
        READFLAG(update_ref_delta);
        if (update_ref_delta)
          READSU(loop_filter_ref_deltas[i], 1+6)
      }
      for (int i = 0; i < 2; i++)
      {
        bool update_mode_delta;
        READFLAG(update_mode_delta);
        if (update_mode_delta)
          READSU(loop_filter_mode_deltas[i], 1+6)
      }
    }
  }
}

void parserAV1OBU::frame_header::cdef_params_struct::parse(bool CodedLossless, bool allow_intrabc, sub_byte_reader &reader, TreeItem *root, QSharedPointer<sequence_header> seq_header)
{
  // Create a new TreeItem root for the item
  // The macros will use this variable to add all the parsed variables
  TreeItem *const itemTree = root ? new TreeItem("cdef_params()", root) : nullptr;

  if (CodedLossless || allow_intrabc || !seq_header->enable_cdef)
  {
    cdef_bits = 0;
    cdef_y_pri_strength[0] = 0;
    cdef_y_sec_strength[0] = 0;
    cdef_uv_pri_strength[0] = 0;
    cdef_uv_sec_strength[0] = 0;
    CdefDamping = 3;
    return;
  }
  READBITS(cdef_damping_minus_3, 2);
  CdefDamping = cdef_damping_minus_3 + 3;
  READBITS(cdef_bits, 2);
  for (int i = 0; i < (1 << cdef_bits); i++)
  {
    READBITS(cdef_y_pri_strength[i], 4);
    READBITS(cdef_y_sec_strength[i], 2);
    if (cdef_y_sec_strength[i] == 3)
      cdef_y_sec_strength[i]++;
    if (seq_header->color_config.NumPlanes > 1)
    {
      READBITS(cdef_uv_pri_strength[i], 4);
      READBITS(cdef_uv_sec_strength[i], 4);
      if (cdef_uv_sec_strength[i] == 3)
        cdef_uv_sec_strength[i]++;
    }
  }
}