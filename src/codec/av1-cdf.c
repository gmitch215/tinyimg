#include "av1.h"

#include "av1-tables.h"
#include "tinyimg/memory.h"

/*
 * Each copy is size checked against the table it comes from at compile time.
 * A dimension that drifts between the generator and this file would otherwise
 * copy the wrong number of bytes and desynchronise a bitstream, which is a
 * failure that shows up as a wrong picture rather than as an error.
 */
_Static_assert(
    sizeof(((TinyAv1Cdf*) 0)->intra_frame_y_mode) ==
        sizeof(tiny_av1_default_intra_frame_y_mode_cdf),
    "intra_frame_y_mode CDF copy does not match the table it is copied from"
);

_Static_assert(
    sizeof(((TinyAv1Cdf*) 0)->y_mode) == sizeof(tiny_av1_default_y_mode_cdf),
    "y_mode CDF copy does not match the table it is copied from"
);

_Static_assert(
    sizeof(((TinyAv1Cdf*) 0)->uv_mode_cfl_allowed) ==
        sizeof(tiny_av1_default_uv_mode_cfl_allowed_cdf),
    "uv_mode_cfl_allowed CDF copy does not match the table it is copied from"
);

_Static_assert(
    sizeof(((TinyAv1Cdf*) 0)->uv_mode_cfl_not_allowed) ==
        sizeof(tiny_av1_default_uv_mode_cfl_not_allowed_cdf),
    "uv_mode_cfl_not_allowed CDF copy does not match the table it is copied "
    "from"
);

_Static_assert(
    sizeof(((TinyAv1Cdf*) 0)->angle_delta) ==
        sizeof(tiny_av1_default_angle_delta_cdf),
    "angle_delta CDF copy does not match the table it is copied from"
);

_Static_assert(
    sizeof(((TinyAv1Cdf*) 0)->cfl_alpha) ==
        sizeof(tiny_av1_default_cfl_alpha_cdf),
    "cfl_alpha CDF copy does not match the table it is copied from"
);

_Static_assert(
    sizeof(((TinyAv1Cdf*) 0)->cfl_sign) ==
        sizeof(tiny_av1_default_cfl_sign_cdf),
    "cfl_sign CDF copy does not match the table it is copied from"
);

_Static_assert(
    sizeof(((TinyAv1Cdf*) 0)->partition_w8) ==
        sizeof(tiny_av1_default_partition_w8_cdf),
    "partition_w8 CDF copy does not match the table it is copied from"
);

_Static_assert(
    sizeof(((TinyAv1Cdf*) 0)->partition_w16) ==
        sizeof(tiny_av1_default_partition_w16_cdf),
    "partition_w16 CDF copy does not match the table it is copied from"
);

_Static_assert(
    sizeof(((TinyAv1Cdf*) 0)->partition_w32) ==
        sizeof(tiny_av1_default_partition_w32_cdf),
    "partition_w32 CDF copy does not match the table it is copied from"
);

_Static_assert(
    sizeof(((TinyAv1Cdf*) 0)->partition_w64) ==
        sizeof(tiny_av1_default_partition_w64_cdf),
    "partition_w64 CDF copy does not match the table it is copied from"
);

_Static_assert(
    sizeof(((TinyAv1Cdf*) 0)->partition_w128) ==
        sizeof(tiny_av1_default_partition_w128_cdf),
    "partition_w128 CDF copy does not match the table it is copied from"
);

_Static_assert(
    sizeof(((TinyAv1Cdf*) 0)->skip) == sizeof(tiny_av1_default_skip_cdf),
    "skip CDF copy does not match the table it is copied from"
);

_Static_assert(
    sizeof(((TinyAv1Cdf*) 0)->segment_id) ==
        sizeof(tiny_av1_default_segment_id_cdf),
    "segment_id CDF copy does not match the table it is copied from"
);

_Static_assert(
    sizeof(((TinyAv1Cdf*) 0)->delta_q) == sizeof(tiny_av1_default_delta_q_cdf),
    "delta_q CDF copy does not match the table it is copied from"
);

_Static_assert(
    sizeof(((TinyAv1Cdf*) 0)->delta_lf) ==
        sizeof(tiny_av1_default_delta_lf_cdf),
    "delta_lf CDF copy does not match the table it is copied from"
);

_Static_assert(
    sizeof(((TinyAv1Cdf*) 0)->tx_8x8) == sizeof(tiny_av1_default_tx_8x8_cdf),
    "tx_8x8 CDF copy does not match the table it is copied from"
);

_Static_assert(
    sizeof(((TinyAv1Cdf*) 0)->tx_16x16) ==
        sizeof(tiny_av1_default_tx_16x16_cdf),
    "tx_16x16 CDF copy does not match the table it is copied from"
);

_Static_assert(
    sizeof(((TinyAv1Cdf*) 0)->tx_32x32) ==
        sizeof(tiny_av1_default_tx_32x32_cdf),
    "tx_32x32 CDF copy does not match the table it is copied from"
);

_Static_assert(
    sizeof(((TinyAv1Cdf*) 0)->tx_64x64) ==
        sizeof(tiny_av1_default_tx_64x64_cdf),
    "tx_64x64 CDF copy does not match the table it is copied from"
);

_Static_assert(
    sizeof(((TinyAv1Cdf*) 0)->txfm_split) ==
        sizeof(tiny_av1_default_txfm_split_cdf),
    "txfm_split CDF copy does not match the table it is copied from"
);

_Static_assert(
    sizeof(((TinyAv1Cdf*) 0)->filter_intra) ==
        sizeof(tiny_av1_default_filter_intra_cdf),
    "filter_intra CDF copy does not match the table it is copied from"
);

_Static_assert(
    sizeof(((TinyAv1Cdf*) 0)->filter_intra_mode) ==
        sizeof(tiny_av1_default_filter_intra_mode_cdf),
    "filter_intra_mode CDF copy does not match the table it is copied from"
);

_Static_assert(
    sizeof(((TinyAv1Cdf*) 0)->palette_y_size) ==
        sizeof(tiny_av1_default_palette_y_size_cdf),
    "palette_y_size CDF copy does not match the table it is copied from"
);

_Static_assert(
    sizeof(((TinyAv1Cdf*) 0)->palette_uv_size) ==
        sizeof(tiny_av1_default_palette_uv_size_cdf),
    "palette_uv_size CDF copy does not match the table it is copied from"
);

_Static_assert(
    sizeof(((TinyAv1Cdf*) 0)->palette_y_mode) ==
        sizeof(tiny_av1_default_palette_y_mode_cdf),
    "palette_y_mode CDF copy does not match the table it is copied from"
);

_Static_assert(
    sizeof(((TinyAv1Cdf*) 0)->palette_uv_mode) ==
        sizeof(tiny_av1_default_palette_uv_mode_cdf),
    "palette_uv_mode CDF copy does not match the table it is copied from"
);

_Static_assert(
    sizeof(((TinyAv1Cdf*) 0)->intrabc) == sizeof(tiny_av1_default_intrabc_cdf),
    "intrabc CDF copy does not match the table it is copied from"
);

_Static_assert(
    sizeof(((TinyAv1Cdf*) 0)->intra_tx_type_set1) ==
        sizeof(tiny_av1_default_intra_tx_type_set1_cdf),
    "intra_tx_type_set1 CDF copy does not match the table it is copied from"
);

_Static_assert(
    sizeof(((TinyAv1Cdf*) 0)->intra_tx_type_set2) ==
        sizeof(tiny_av1_default_intra_tx_type_set2_cdf),
    "intra_tx_type_set2 CDF copy does not match the table it is copied from"
);

_Static_assert(
    sizeof(((TinyAv1Cdf*) 0)->txb_skip) ==
        sizeof(tiny_av1_default_txb_skip_cdf),
    "txb_skip CDF copy does not match the table it is copied from"
);

_Static_assert(
    sizeof(((TinyAv1Cdf*) 0)->eob_extra) ==
        sizeof(tiny_av1_default_eob_extra_cdf),
    "eob_extra CDF copy does not match the table it is copied from"
);

_Static_assert(
    sizeof(((TinyAv1Cdf*) 0)->coeff_base) ==
        sizeof(tiny_av1_default_coeff_base_cdf),
    "coeff_base CDF copy does not match the table it is copied from"
);

_Static_assert(
    sizeof(((TinyAv1Cdf*) 0)->coeff_base_eob) ==
        sizeof(tiny_av1_default_coeff_base_eob_cdf),
    "coeff_base_eob CDF copy does not match the table it is copied from"
);

_Static_assert(
    sizeof(((TinyAv1Cdf*) 0)->coeff_br) ==
        sizeof(tiny_av1_default_coeff_br_cdf),
    "coeff_br CDF copy does not match the table it is copied from"
);

_Static_assert(
    sizeof(((TinyAv1Cdf*) 0)->dc_sign) == sizeof(tiny_av1_default_dc_sign_cdf),
    "dc_sign CDF copy does not match the table it is copied from"
);

_Static_assert(
    sizeof(((TinyAv1Cdf*) 0)->restoration_type) ==
        sizeof(tiny_av1_default_restoration_type_cdf),
    "restoration_type CDF copy does not match the table it is copied from"
);

_Static_assert(
    sizeof(((TinyAv1Cdf*) 0)->use_wiener) ==
        sizeof(tiny_av1_default_use_wiener_cdf),
    "use_wiener CDF copy does not match the table it is copied from"
);

_Static_assert(
    sizeof(((TinyAv1Cdf*) 0)->use_sgrproj) ==
        sizeof(tiny_av1_default_use_sgrproj_cdf),
    "use_sgrproj CDF copy does not match the table it is copied from"
);

_Static_assert(
    sizeof(((TinyAv1Cdf*) 0)->eob_pt_16) ==
        sizeof(tiny_av1_default_eob_pt_16_cdf),
    "eob_pt_16 CDF copy does not match the table it is copied from"
);

_Static_assert(
    sizeof(((TinyAv1Cdf*) 0)->eob_pt_32) ==
        sizeof(tiny_av1_default_eob_pt_32_cdf),
    "eob_pt_32 CDF copy does not match the table it is copied from"
);

_Static_assert(
    sizeof(((TinyAv1Cdf*) 0)->eob_pt_64) ==
        sizeof(tiny_av1_default_eob_pt_64_cdf),
    "eob_pt_64 CDF copy does not match the table it is copied from"
);

_Static_assert(
    sizeof(((TinyAv1Cdf*) 0)->eob_pt_128) ==
        sizeof(tiny_av1_default_eob_pt_128_cdf),
    "eob_pt_128 CDF copy does not match the table it is copied from"
);

_Static_assert(
    sizeof(((TinyAv1Cdf*) 0)->eob_pt_256) ==
        sizeof(tiny_av1_default_eob_pt_256_cdf),
    "eob_pt_256 CDF copy does not match the table it is copied from"
);

_Static_assert(
    sizeof(((TinyAv1Cdf*) 0)->eob_pt_512) ==
        sizeof(tiny_av1_default_eob_pt_512_cdf),
    "eob_pt_512 CDF copy does not match the table it is copied from"
);

_Static_assert(
    sizeof(((TinyAv1Cdf*) 0)->eob_pt_1024) ==
        sizeof(tiny_av1_default_eob_pt_1024_cdf),
    "eob_pt_1024 CDF copy does not match the table it is copied from"
);

_Static_assert(
    sizeof(((TinyAv1Cdf*) 0)->palette_size_2_y_color) ==
        sizeof(tiny_av1_default_palette_size_2_y_color_cdf),
    "palette_size_2_y_color CDF copy does not match the table it is copied from"
);

_Static_assert(
    sizeof(((TinyAv1Cdf*) 0)->palette_size_3_y_color) ==
        sizeof(tiny_av1_default_palette_size_3_y_color_cdf),
    "palette_size_3_y_color CDF copy does not match the table it is copied from"
);

_Static_assert(
    sizeof(((TinyAv1Cdf*) 0)->palette_size_4_y_color) ==
        sizeof(tiny_av1_default_palette_size_4_y_color_cdf),
    "palette_size_4_y_color CDF copy does not match the table it is copied from"
);

_Static_assert(
    sizeof(((TinyAv1Cdf*) 0)->palette_size_5_y_color) ==
        sizeof(tiny_av1_default_palette_size_5_y_color_cdf),
    "palette_size_5_y_color CDF copy does not match the table it is copied from"
);

_Static_assert(
    sizeof(((TinyAv1Cdf*) 0)->palette_size_6_y_color) ==
        sizeof(tiny_av1_default_palette_size_6_y_color_cdf),
    "palette_size_6_y_color CDF copy does not match the table it is copied from"
);

_Static_assert(
    sizeof(((TinyAv1Cdf*) 0)->palette_size_7_y_color) ==
        sizeof(tiny_av1_default_palette_size_7_y_color_cdf),
    "palette_size_7_y_color CDF copy does not match the table it is copied from"
);

_Static_assert(
    sizeof(((TinyAv1Cdf*) 0)->palette_size_8_y_color) ==
        sizeof(tiny_av1_default_palette_size_8_y_color_cdf),
    "palette_size_8_y_color CDF copy does not match the table it is copied from"
);

_Static_assert(
    sizeof(((TinyAv1Cdf*) 0)->palette_size_2_uv_color) ==
        sizeof(tiny_av1_default_palette_size_2_uv_color_cdf),
    "palette_size_2_uv_color CDF copy does not match the table it is copied "
    "from"
);

_Static_assert(
    sizeof(((TinyAv1Cdf*) 0)->palette_size_3_uv_color) ==
        sizeof(tiny_av1_default_palette_size_3_uv_color_cdf),
    "palette_size_3_uv_color CDF copy does not match the table it is copied "
    "from"
);

_Static_assert(
    sizeof(((TinyAv1Cdf*) 0)->palette_size_4_uv_color) ==
        sizeof(tiny_av1_default_palette_size_4_uv_color_cdf),
    "palette_size_4_uv_color CDF copy does not match the table it is copied "
    "from"
);

_Static_assert(
    sizeof(((TinyAv1Cdf*) 0)->palette_size_5_uv_color) ==
        sizeof(tiny_av1_default_palette_size_5_uv_color_cdf),
    "palette_size_5_uv_color CDF copy does not match the table it is copied "
    "from"
);

_Static_assert(
    sizeof(((TinyAv1Cdf*) 0)->palette_size_6_uv_color) ==
        sizeof(tiny_av1_default_palette_size_6_uv_color_cdf),
    "palette_size_6_uv_color CDF copy does not match the table it is copied "
    "from"
);

_Static_assert(
    sizeof(((TinyAv1Cdf*) 0)->palette_size_7_uv_color) ==
        sizeof(tiny_av1_default_palette_size_7_uv_color_cdf),
    "palette_size_7_uv_color CDF copy does not match the table it is copied "
    "from"
);

_Static_assert(
    sizeof(((TinyAv1Cdf*) 0)->palette_size_8_uv_color) ==
        sizeof(tiny_av1_default_palette_size_8_uv_color_cdf),
    "palette_size_8_uv_color CDF copy does not match the table it is copied "
    "from"
);

_Static_assert(
    sizeof(((TinyAv1Cdf*) 0)->mv_joint) ==
        sizeof(tiny_av1_default_mv_joint_cdf),
    "mv_joint CDF copy does not match the table it is copied from"
);

_Static_assert(
    sizeof(((TinyAv1Cdf*) 0)->mv_class) ==
        sizeof(tiny_av1_default_mv_class_cdf),
    "mv_class CDF copy does not match the table it is copied from"
);

_Static_assert(
    sizeof(((TinyAv1Cdf*) 0)->mv_class0_bit) ==
        sizeof(tiny_av1_default_mv_class0_bit_cdf),
    "mv_class0_bit CDF copy does not match the table it is copied from"
);

_Static_assert(
    sizeof(((TinyAv1Cdf*) 0)->mv_class0_fr) ==
        sizeof(tiny_av1_default_mv_class0_fr_cdf),
    "mv_class0_fr CDF copy does not match the table it is copied from"
);

_Static_assert(
    sizeof(((TinyAv1Cdf*) 0)->mv_class0_hp) ==
        sizeof(tiny_av1_default_mv_class0_hp_cdf),
    "mv_class0_hp CDF copy does not match the table it is copied from"
);

_Static_assert(
    sizeof(((TinyAv1Cdf*) 0)->mv_bit) == sizeof(tiny_av1_default_mv_bit_cdf),
    "mv_bit CDF copy does not match the table it is copied from"
);

_Static_assert(
    sizeof(((TinyAv1Cdf*) 0)->mv_fr) == sizeof(tiny_av1_default_mv_fr_cdf),
    "mv_fr CDF copy does not match the table it is copied from"
);

_Static_assert(
    sizeof(((TinyAv1Cdf*) 0)->mv_hp) == sizeof(tiny_av1_default_mv_hp_cdf),
    "mv_hp CDF copy does not match the table it is copied from"
);

_Static_assert(
    sizeof(((TinyAv1Cdf*) 0)->mv_sign) == sizeof(tiny_av1_default_mv_sign_cdf),
    "mv_sign CDF copy does not match the table it is copied from"
);

_Static_assert(
    sizeof(((TinyAv1Cdf*) 0)->delta_lf_multi[0]) ==
        sizeof(tiny_av1_default_delta_lf_cdf),
    "each delta_lf_multi copy has to match the single delta_lf default"
);

void tiny_av1_cdf_init(TinyAv1Cdf* cdf) {
    if (!cdf) return;

    tiny_memcpy(
        cdf->intra_frame_y_mode, tiny_av1_default_intra_frame_y_mode_cdf,
        sizeof(cdf->intra_frame_y_mode)
    );

    tiny_memcpy(cdf->y_mode, tiny_av1_default_y_mode_cdf, sizeof(cdf->y_mode));

    tiny_memcpy(
        cdf->uv_mode_cfl_allowed, tiny_av1_default_uv_mode_cfl_allowed_cdf,
        sizeof(cdf->uv_mode_cfl_allowed)
    );

    tiny_memcpy(
        cdf->uv_mode_cfl_not_allowed,
        tiny_av1_default_uv_mode_cfl_not_allowed_cdf,
        sizeof(cdf->uv_mode_cfl_not_allowed)
    );

    tiny_memcpy(
        cdf->angle_delta, tiny_av1_default_angle_delta_cdf,
        sizeof(cdf->angle_delta)
    );

    tiny_memcpy(
        cdf->cfl_alpha, tiny_av1_default_cfl_alpha_cdf, sizeof(cdf->cfl_alpha)
    );

    tiny_memcpy(
        cdf->cfl_sign, tiny_av1_default_cfl_sign_cdf, sizeof(cdf->cfl_sign)
    );

    tiny_memcpy(
        cdf->partition_w8, tiny_av1_default_partition_w8_cdf,
        sizeof(cdf->partition_w8)
    );

    tiny_memcpy(
        cdf->partition_w16, tiny_av1_default_partition_w16_cdf,
        sizeof(cdf->partition_w16)
    );

    tiny_memcpy(
        cdf->partition_w32, tiny_av1_default_partition_w32_cdf,
        sizeof(cdf->partition_w32)
    );

    tiny_memcpy(
        cdf->partition_w64, tiny_av1_default_partition_w64_cdf,
        sizeof(cdf->partition_w64)
    );

    tiny_memcpy(
        cdf->partition_w128, tiny_av1_default_partition_w128_cdf,
        sizeof(cdf->partition_w128)
    );

    tiny_memcpy(cdf->skip, tiny_av1_default_skip_cdf, sizeof(cdf->skip));

    tiny_memcpy(
        cdf->segment_id, tiny_av1_default_segment_id_cdf,
        sizeof(cdf->segment_id)
    );

    tiny_memcpy(
        cdf->delta_q, tiny_av1_default_delta_q_cdf, sizeof(cdf->delta_q)
    );

    tiny_memcpy(
        cdf->delta_lf, tiny_av1_default_delta_lf_cdf, sizeof(cdf->delta_lf)
    );

    tiny_memcpy(cdf->tx_8x8, tiny_av1_default_tx_8x8_cdf, sizeof(cdf->tx_8x8));

    tiny_memcpy(
        cdf->tx_16x16, tiny_av1_default_tx_16x16_cdf, sizeof(cdf->tx_16x16)
    );

    tiny_memcpy(
        cdf->tx_32x32, tiny_av1_default_tx_32x32_cdf, sizeof(cdf->tx_32x32)
    );

    tiny_memcpy(
        cdf->tx_64x64, tiny_av1_default_tx_64x64_cdf, sizeof(cdf->tx_64x64)
    );

    tiny_memcpy(
        cdf->txfm_split, tiny_av1_default_txfm_split_cdf,
        sizeof(cdf->txfm_split)
    );

    tiny_memcpy(
        cdf->filter_intra, tiny_av1_default_filter_intra_cdf,
        sizeof(cdf->filter_intra)
    );

    tiny_memcpy(
        cdf->filter_intra_mode, tiny_av1_default_filter_intra_mode_cdf,
        sizeof(cdf->filter_intra_mode)
    );

    tiny_memcpy(
        cdf->palette_y_size, tiny_av1_default_palette_y_size_cdf,
        sizeof(cdf->palette_y_size)
    );

    tiny_memcpy(
        cdf->palette_uv_size, tiny_av1_default_palette_uv_size_cdf,
        sizeof(cdf->palette_uv_size)
    );

    tiny_memcpy(
        cdf->palette_y_mode, tiny_av1_default_palette_y_mode_cdf,
        sizeof(cdf->palette_y_mode)
    );

    tiny_memcpy(
        cdf->palette_uv_mode, tiny_av1_default_palette_uv_mode_cdf,
        sizeof(cdf->palette_uv_mode)
    );

    tiny_memcpy(
        cdf->intrabc, tiny_av1_default_intrabc_cdf, sizeof(cdf->intrabc)
    );

    tiny_memcpy(
        cdf->intra_tx_type_set1, tiny_av1_default_intra_tx_type_set1_cdf,
        sizeof(cdf->intra_tx_type_set1)
    );

    tiny_memcpy(
        cdf->intra_tx_type_set2, tiny_av1_default_intra_tx_type_set2_cdf,
        sizeof(cdf->intra_tx_type_set2)
    );

    tiny_memcpy(
        cdf->txb_skip, tiny_av1_default_txb_skip_cdf, sizeof(cdf->txb_skip)
    );

    tiny_memcpy(
        cdf->eob_extra, tiny_av1_default_eob_extra_cdf, sizeof(cdf->eob_extra)
    );

    tiny_memcpy(
        cdf->coeff_base, tiny_av1_default_coeff_base_cdf,
        sizeof(cdf->coeff_base)
    );

    tiny_memcpy(
        cdf->coeff_base_eob, tiny_av1_default_coeff_base_eob_cdf,
        sizeof(cdf->coeff_base_eob)
    );

    tiny_memcpy(
        cdf->coeff_br, tiny_av1_default_coeff_br_cdf, sizeof(cdf->coeff_br)
    );

    tiny_memcpy(
        cdf->dc_sign, tiny_av1_default_dc_sign_cdf, sizeof(cdf->dc_sign)
    );

    tiny_memcpy(
        cdf->restoration_type, tiny_av1_default_restoration_type_cdf,
        sizeof(cdf->restoration_type)
    );

    tiny_memcpy(
        cdf->use_wiener, tiny_av1_default_use_wiener_cdf,
        sizeof(cdf->use_wiener)
    );

    tiny_memcpy(
        cdf->use_sgrproj, tiny_av1_default_use_sgrproj_cdf,
        sizeof(cdf->use_sgrproj)
    );

    tiny_memcpy(
        cdf->eob_pt_16, tiny_av1_default_eob_pt_16_cdf, sizeof(cdf->eob_pt_16)
    );

    tiny_memcpy(
        cdf->eob_pt_32, tiny_av1_default_eob_pt_32_cdf, sizeof(cdf->eob_pt_32)
    );

    tiny_memcpy(
        cdf->eob_pt_64, tiny_av1_default_eob_pt_64_cdf, sizeof(cdf->eob_pt_64)
    );

    tiny_memcpy(
        cdf->eob_pt_128, tiny_av1_default_eob_pt_128_cdf,
        sizeof(cdf->eob_pt_128)
    );

    tiny_memcpy(
        cdf->eob_pt_256, tiny_av1_default_eob_pt_256_cdf,
        sizeof(cdf->eob_pt_256)
    );

    tiny_memcpy(
        cdf->eob_pt_512, tiny_av1_default_eob_pt_512_cdf,
        sizeof(cdf->eob_pt_512)
    );

    tiny_memcpy(
        cdf->eob_pt_1024, tiny_av1_default_eob_pt_1024_cdf,
        sizeof(cdf->eob_pt_1024)
    );

    tiny_memcpy(
        cdf->palette_size_2_y_color,
        tiny_av1_default_palette_size_2_y_color_cdf,
        sizeof(cdf->palette_size_2_y_color)
    );

    tiny_memcpy(
        cdf->palette_size_3_y_color,
        tiny_av1_default_palette_size_3_y_color_cdf,
        sizeof(cdf->palette_size_3_y_color)
    );

    tiny_memcpy(
        cdf->palette_size_4_y_color,
        tiny_av1_default_palette_size_4_y_color_cdf,
        sizeof(cdf->palette_size_4_y_color)
    );

    tiny_memcpy(
        cdf->palette_size_5_y_color,
        tiny_av1_default_palette_size_5_y_color_cdf,
        sizeof(cdf->palette_size_5_y_color)
    );

    tiny_memcpy(
        cdf->palette_size_6_y_color,
        tiny_av1_default_palette_size_6_y_color_cdf,
        sizeof(cdf->palette_size_6_y_color)
    );

    tiny_memcpy(
        cdf->palette_size_7_y_color,
        tiny_av1_default_palette_size_7_y_color_cdf,
        sizeof(cdf->palette_size_7_y_color)
    );

    tiny_memcpy(
        cdf->palette_size_8_y_color,
        tiny_av1_default_palette_size_8_y_color_cdf,
        sizeof(cdf->palette_size_8_y_color)
    );

    tiny_memcpy(
        cdf->palette_size_2_uv_color,
        tiny_av1_default_palette_size_2_uv_color_cdf,
        sizeof(cdf->palette_size_2_uv_color)
    );

    tiny_memcpy(
        cdf->palette_size_3_uv_color,
        tiny_av1_default_palette_size_3_uv_color_cdf,
        sizeof(cdf->palette_size_3_uv_color)
    );

    tiny_memcpy(
        cdf->palette_size_4_uv_color,
        tiny_av1_default_palette_size_4_uv_color_cdf,
        sizeof(cdf->palette_size_4_uv_color)
    );

    tiny_memcpy(
        cdf->palette_size_5_uv_color,
        tiny_av1_default_palette_size_5_uv_color_cdf,
        sizeof(cdf->palette_size_5_uv_color)
    );

    tiny_memcpy(
        cdf->palette_size_6_uv_color,
        tiny_av1_default_palette_size_6_uv_color_cdf,
        sizeof(cdf->palette_size_6_uv_color)
    );

    tiny_memcpy(
        cdf->palette_size_7_uv_color,
        tiny_av1_default_palette_size_7_uv_color_cdf,
        sizeof(cdf->palette_size_7_uv_color)
    );

    tiny_memcpy(
        cdf->palette_size_8_uv_color,
        tiny_av1_default_palette_size_8_uv_color_cdf,
        sizeof(cdf->palette_size_8_uv_color)
    );

    tiny_memcpy(
        cdf->mv_joint, tiny_av1_default_mv_joint_cdf, sizeof(cdf->mv_joint)
    );

    tiny_memcpy(
        cdf->mv_class, tiny_av1_default_mv_class_cdf, sizeof(cdf->mv_class)
    );

    tiny_memcpy(
        cdf->mv_class0_bit, tiny_av1_default_mv_class0_bit_cdf,
        sizeof(cdf->mv_class0_bit)
    );

    tiny_memcpy(
        cdf->mv_class0_fr, tiny_av1_default_mv_class0_fr_cdf,
        sizeof(cdf->mv_class0_fr)
    );

    tiny_memcpy(
        cdf->mv_class0_hp, tiny_av1_default_mv_class0_hp_cdf,
        sizeof(cdf->mv_class0_hp)
    );

    tiny_memcpy(cdf->mv_bit, tiny_av1_default_mv_bit_cdf, sizeof(cdf->mv_bit));

    tiny_memcpy(cdf->mv_fr, tiny_av1_default_mv_fr_cdf, sizeof(cdf->mv_fr));

    tiny_memcpy(cdf->mv_hp, tiny_av1_default_mv_hp_cdf, sizeof(cdf->mv_hp));

    tiny_memcpy(
        cdf->mv_sign, tiny_av1_default_mv_sign_cdf, sizeof(cdf->mv_sign)
    );

    // four independent copies of one default, so each of the four loop filter
    // strengths adapts on its own
    for (uint32_t i = 0; i < 4u; i++) {
        tiny_memcpy(
            cdf->delta_lf_multi[i], tiny_av1_default_delta_lf_cdf,
            sizeof(cdf->delta_lf_multi[i])
        );
    }
}
