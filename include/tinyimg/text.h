/**
 * @file text.h
 * @author Gregory Mitchell (me@gmitch215.xyz)
 * @brief Font loading, text measurement and text drawing.
 * @version 1.0.0
 * @date 2026-09-03
 *
 * @copyright Copyright (c) 2026
 *
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "tinyimg/image.h"
#include "tinyimg/memory.h"
#include "tinyimg/tinyimg.h"

#ifdef __cplusplus
extern "C" {
#endif

#pragma region font loading

/**
 * @brief Outline and bitmap face formats.
 *
 * Dispatched from the first four bytes, so a caller never says which one they
 * have. The three are genuinely different shapes rather than variants of one:
 * an outline face has a curve per glyph and scales to any size, and a bitmap
 * face has a fixed grid and one size.
 */
typedef enum TinyFontKind
{
    /**
     * @brief Quadratic outlines in a `glyf` table.
     *
     * Both the `.ttf` and the `.otf` wrappers of it. An OpenType file whose
     * outlines are CFF charstrings rather than `glyf` is a different format
     * behind the same extension, and loading one reports
     * TINYIMG_ERR_UNSUPPORTED_VARIANT.
     */
    TINYIMG_FONT_TRUETYPE = 0,
    /** A PC screen font: one fixed cell, one bit per pixel. */
    TINYIMG_FONT_PSF = 1,
    /** Glyph Bitmap Distribution Format, one bitmap per glyph. */
    TINYIMG_FONT_BDF = 2,
} TinyFontKind;

/**
 * @brief A loaded face.
 *
 * Keep one on the stack. It borrows the bytes it was loaded from rather than
 * copying them, so a font stays valid only as long as those bytes do: a blob
 * lives until tiny_blob_free, and bytes handed to tiny_font_load_bytes live as
 * long as the caller keeps them.
 *
 * The fields are read by the drawing code and by nothing else; a host reads a
 * face through tiny_font_metrics rather than out of this structure, so the
 * layout is not part of the ABI.
 */
typedef struct {
    /** The face's bytes, borrowed. */
    const uint8_t* data;
    /** How many. */
    size_t size;
    /** Which format the magic bytes identified. */
    TinyFontKind kind;

    /** Offset of `cmap`, or zero when the face has none. */
    uint32_t cmap;
    /** Offset of `glyf`. */
    uint32_t glyf;
    /** Offset of `loca`. */
    uint32_t loca;
    /** Offset of `hmtx`. */
    uint32_t hmtx;
    /** Offset of `kern`, or zero when the face has none. */
    uint32_t kern;
    /** Length of `glyf`, so a bad `loca` entry cannot read past it. */
    uint32_t glyf_size;
    /** Glyphs the face holds, from `maxp`. */
    uint32_t glyphs;
    /** Entries in `hmtx` before the trailing left side bearings. */
    uint32_t hmetrics;
    /** Font units per em, the divisor that turns a font unit into a pixel. */
    uint32_t units_per_em;
    /** Non-zero when `loca` entries are 32 bit. */
    uint8_t long_loca;

    /** Baseline to the top of the ascenders, in font units. */
    int32_t ascent;
    /** Baseline down to the bottom of the descenders, positive, in font units.
     */
    int32_t descent;
    /** Space between the descenders of one line and the ascenders of the next.
     */
    int32_t line_gap;

    /** Cell width of a bitmap face, in pixels. */
    uint32_t cell_width;
    /** Cell height of a bitmap face, in pixels. */
    uint32_t cell_height;
    /** Bytes one bitmap glyph occupies. */
    uint32_t glyph_bytes;
    /** Offset of the first glyph bitmap. */
    uint32_t bitmap;

    /**
     * @brief Codepoint and offset pairs for a BDF face, owned.
     *
     * BDF is a text format with no index of its own, so one is built at load
     * and released by tiny_font_free. The other two formats leave this NULL,
     * which is why calling tiny_font_free is harmless rather than required for
     * them.
     */
    uint32_t* index;
    /** How many pairs. */
    uint32_t index_count;
} TinyFont;

/**
 * @brief What a face says about itself, in pixels at a size.
 *
 * The numbers a caller needs to place a line of text without knowing anything
 * about font units.
 */
typedef struct {
    /** Baseline to the top of the ascenders. */
    float ascent;
    /** Baseline down to the bottom of the descenders, positive. */
    float descent;
    /** Baseline to baseline. */
    float line_height;
    /** Em size the numbers are for. */
    float size;
    /** Glyphs the face holds. */
    uint32_t glyphs;
    /** Non-zero when the size is fixed, which a bitmap face's is. */
    uint8_t fixed_size;
} TinyFontMetrics;

/**
 * @brief Loads a face from a resident blob.
 *
 * The blob is what a Worker has: imported as a wrangler `Data` module, or
 * fetched from a bucket and handed to tiny_blob_load. Its bytes stay owned by
 * the blob table, so the face borrows them and tiny_blob_free invalidates it.
 *
 * @param font Receives the face.
 * @param blob_id The id it was loaded under, or NULL for the first font blob.
 * @return int TINYIMG_OK, TINYIMG_ERR_BLOB_MISSING when no such blob is
 * resident, TINYIMG_ERR_UNKNOWN_FORMAT when the bytes match no face format,
 * TINYIMG_ERR_UNSUPPORTED_VARIANT for a CFF-outlined OpenType file, or
 * TINYIMG_ERR_CORRUPT.
 */
int tiny_font_load(TinyFont* font, const char* blob_id);

/**
 * @brief Loads a face from bytes the caller keeps.
 *
 * The escape hatch from the blob table, and the difference is ownership:
 * tiny_blob_load takes the bytes and frees them, this borrows them and frees
 * nothing. Use it when the bytes are already somewhere convenient, or when a
 * face is wanted for one call and should not occupy a blob slot.
 *
 * @param font Receives the face.
 * @param data The face's bytes, which must outlive the face.
 * @param size How many.
 * @return int TINYIMG_OK or a negative TinyImageError; see tiny_font_load.
 */
int tiny_font_load_bytes(TinyFont* font, const uint8_t* data, size_t size);

/**
 * @brief Releases whatever a face owns.
 *
 * Never the bytes, which belong to the blob table or to the caller. Only a BDF
 * face owns anything, so this is a no-op for the other two; calling it always
 * is still the right habit and costs a branch.
 *
 * @param font The face, left safe to load into again.
 */
void tiny_font_free(TinyFont* font);

/**
 * @brief Reads a face's vertical metrics at an em size.
 *
 * @param font The face.
 * @param size Em size in pixels. Zero, or any size for a bitmap face, reports
 * the face's own.
 * @param out Receives the metrics.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_font_metrics(const TinyFont* font, float size, TinyFontMetrics* out);

/**
 * @brief Reports whether a face has a glyph for a codepoint.
 *
 * @param font The face.
 * @param codepoint The Unicode codepoint.
 * @return int Non-zero when it does. A codepoint it does not have still draws:
 * it maps to glyph zero, which is what a text renderer does with one.
 */
int tiny_font_has_glyph(const TinyFont* font, uint32_t codepoint);

/**
 * @brief Size of a TinyFont, for a host allocating one across the wasm
 * boundary.
 *
 * @return uint32_t sizeof(TinyFont).
 */
uint32_t tiny_font_sizeof(void);

/**
 * @brief Size of a TinyFontMetrics, for the same reason.
 *
 * @return uint32_t sizeof(TinyFontMetrics).
 */
uint32_t tiny_font_metrics_sizeof(void);

#pragma endregion

#pragma region text drawing

/**
 * @brief How a run of text is set.
 *
 * A zeroed structure is usable and means the same as passing NULL: the face's
 * own em size, no tracking, single line spacing and no kerning.
 * tiny_text_style fills one in with the defaults instead, which differ in one
 * place -- kerning on -- and are what a caller should start from.
 */
typedef struct {
    /**
     * @brief Em size in pixels. Ignored by a bitmap face, which has one size.
     *
     * Zero means the face's own em, so a zeroed style draws. A negative size
     * is a caller error and reports TINYIMG_ERR_RANGE.
     */
    float size;
    /** Extra space between glyphs, in pixels. Negative tightens. */
    float tracking;
    /** Multiple of the face's own line height. Zero reads as one. */
    float line_height;
    /** Non-zero to apply the face's kern pairs. */
    uint8_t kerning;
} TinyTextStyle;

/**
 * @brief What a run of text occupies.
 */
typedef struct {
    /** Advance width of the widest line, in pixels. */
    float width;
    /** Height of the whole run: one line box per line. */
    float height;
    /** Baseline of the first line, measured down from the top of the run. */
    float ascent;
    /** Below that baseline, positive. */
    float descent;
    /** Baseline to baseline. */
    float line_height;
    /** How many lines the run occupies. */
    uint32_t lines;
    /** Codepoints the run holds, which is not its byte length. */
    uint32_t glyphs;
    /** Codepoints the face had no glyph for. */
    uint32_t missing;
} TinyTextMetrics;

/**
 * @brief Where a line sits inside the width it was given.
 */
typedef enum TinyTextAlign
{
    /** Against the left edge. */
    TINYIMG_ALIGN_LEFT = 0,
    /** Centered in the width. */
    TINYIMG_ALIGN_CENTER = 1,
    /** Against the right edge. */
    TINYIMG_ALIGN_RIGHT = 2,
} TinyTextAlign;

/**
 * @brief Fills a style in with the defaults.
 *
 * @param style Receives them.
 * @param size Em size in pixels.
 */
void tiny_text_style(TinyTextStyle* style, float size);

/**
 * @brief Draws a line of text.
 *
 * `x` and `y` are the top left of the line box, not the baseline, which is
 * what every other drawing entry point in this library takes and what a caller
 * placing a label wants. The baseline is `y + metrics.ascent`, so a caller who
 * wants to sit text on a baseline subtracts that.
 *
 * A newline in `text` starts a new line at `x`, so a short multi-line string
 * needs no box.
 *
 * @param image The image to draw on.
 * @param font The face.
 * @param text UTF-8, NUL terminated. A malformed sequence draws the
 * replacement glyph rather than failing.
 * @param x Left edge of the line box; may be negative.
 * @param y Top edge of the line box; may be negative.
 * @param style How to set it, or NULL for the defaults at the face's own size.
 * @param color The color, as many channels as the image has.
 * @return int TINYIMG_OK, TINYIMG_ERR_BLOB_MISSING for an unloaded face, or a
 * negative TinyImageError.
 */
int tiny_image_draw_text(
    TinyImage* image, const TinyFont* font, const char* text, int32_t x,
    int32_t y, const TinyTextStyle* style, const uint8_t* color
);

/**
 * @brief Draws text wrapped and aligned inside a rectangle.
 *
 * Wraps on spaces where it can and mid-word where a single word is wider than
 * the box, so a long unbroken string is clipped by the box rather than running
 * out of it. A line whose box has run out of height is not drawn, and the
 * metrics still report every line the text would have taken, so a caller can
 * tell that it overflowed.
 *
 * @param image The image to draw on.
 * @param font The face.
 * @param text UTF-8, NUL terminated.
 * @param x Left edge of the box.
 * @param y Top edge of the box.
 * @param width Width to wrap inside. Zero draws nothing.
 * @param height Height to fill. Zero means no limit.
 * @param style How to set it, or NULL for the defaults.
 * @param align Where each line sits inside `width`.
 * @param color The color, as many channels as the image has.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_image_draw_text_box(
    TinyImage* image, const TinyFont* font, const char* text, int32_t x,
    int32_t y, uint32_t width, uint32_t height, const TinyTextStyle* style,
    TinyTextAlign align, const uint8_t* color
);

/**
 * @brief Measures a run of text without drawing it.
 *
 * Named for the text rather than for an image because it takes none: the size
 * of a string is a property of the face and the style. It is the same layout
 * the drawing entry points use, so measuring and then drawing at the measured
 * position lands where the measurement said.
 *
 * @param font The face.
 * @param text UTF-8, NUL terminated. Newlines count as line breaks.
 * @param style How it would be set, or NULL for the defaults.
 * @param out Receives the metrics.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_text_measure(
    const TinyFont* font, const char* text, const TinyTextStyle* style,
    TinyTextMetrics* out
);

/**
 * @brief Measures a run of text as it would be wrapped inside a width.
 *
 * @param font The face.
 * @param text UTF-8, NUL terminated.
 * @param width Width to wrap inside. Zero measures without wrapping.
 * @param style How it would be set, or NULL for the defaults.
 * @param out Receives the metrics, whose `lines` is what the wrap produced.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_text_measure_wrapped(
    const TinyFont* font, const char* text, uint32_t width,
    const TinyTextStyle* style, TinyTextMetrics* out
);

/**
 * @brief Size of a TinyTextStyle, for a host allocating one across the wasm
 * boundary.
 *
 * @return uint32_t sizeof(TinyTextStyle).
 */
uint32_t tiny_text_style_sizeof(void);

/**
 * @brief Size of a TinyTextMetrics, for the same reason.
 *
 * @return uint32_t sizeof(TinyTextMetrics).
 */
uint32_t tiny_text_metrics_sizeof(void);

#pragma endregion

#ifdef __cplusplus
}
#endif
