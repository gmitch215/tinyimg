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

/** How many variation axes one face may be set on at once. */
#define TINYIMG_FONT_MAX_AXES 4

/**
 * @brief One variation axis a face declares.
 *
 * The four numbers a caller needs to pick a value: the range, and where the
 * face sits when nothing is asked for.
 */
typedef struct {
    /**
     * @brief The axis tag, as four packed bytes.
     *
     * `wght`, `wdth`, `slnt`, `ital` and `opsz` are the registered ones;
     * TINYIMG_AXIS_WEIGHT and its siblings are those five spelled out.
     */
    uint32_t tag;
    /** Smallest value the axis accepts. */
    float min;
    /** Where the axis sits with nothing asked for. */
    float def;
    /** Largest value the axis accepts. */
    float max;
} TinyFontAxis;

/** The `wght` axis, 1 to 1000 by convention. */
#define TINYIMG_AXIS_WEIGHT 0x77676874u
/** The `wdth` axis, a percentage. */
#define TINYIMG_AXIS_WIDTH 0x77647468u
/** The `slnt` axis, degrees clockwise from upright. */
#define TINYIMG_AXIS_SLANT 0x736C6E74u
/** The `ital` axis, 0 or 1. */
#define TINYIMG_AXIS_ITALIC 0x6974616Cu
/** The `opsz` axis, the size the design is optimized for. */
#define TINYIMG_AXIS_OPTICAL_SIZE 0x6F70737Au

/** How many faces one fallback chain holds. */
#define TINYIMG_FONT_MAX_FALLBACK 4

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
    /**
     * @brief Offset of the GPOS `kern` feature's pair lookup, or zero.
     *
     * Resolved at load rather than searched per pair: the walk from GPOS to a
     * lookup goes through the script list, the language system and the feature
     * list, and none of that changes between glyphs. What is left is the part
     * that does depend on the pair.
     *
     * Modern faces ship this and no `kern` table at all, so a face that looked
     * unkerned through the legacy table alone usually is not.
     */
    uint32_t gpos_kern;
    /** How many subtables that lookup holds. */
    uint32_t gpos_subtables;
    /**
     * @brief Offset of the GSUB `liga` feature's ligature lookup, or zero.
     *
     * Resolved at load for the same reason the GPOS one is: the walk to it does
     * not depend on the glyphs.
     */
    uint32_t gsub_liga;
    /** How many subtables that lookup holds. */
    uint32_t gsub_subtables;
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

    /** Offset of `fvar`, or zero when the face declares no axes. */
    uint32_t fvar;
    /** Offset of `gvar`, or zero when the face carries no outline deltas. */
    uint32_t gvar;
    /** Offset of `avar`, or zero when the axes need no remapping. */
    uint32_t avar;
    /** Axes the face declares, capped at TINYIMG_FONT_MAX_AXES. */
    uint32_t axes;
    /**
     * @brief Where the face is set, one normalized coordinate per axis.
     *
     * -1 to 1, which is the space `gvar` regions are expressed in, rather than
     * the axis' own units. Set through tiny_font_set_axis.
     */
    float coords[TINYIMG_FONT_MAX_AXES];
    /** Non-zero when any coordinate is away from the default. */
    uint8_t varied;

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
 * @brief An ordered chain of faces, tried in turn for each codepoint.
 *
 * What a subset face needs to be usable: the shipped `sans` covers ASCII and
 * eight accents, and a caller who needs one Greek word or a currency symbol
 * loads a second face and puts it behind the first rather than replacing it.
 *
 * The **first** face sets the line box. A fallback glyph is drawn at the same
 * em size, scaled by its own face's units per em, so two faces with different
 * unit grids still come out the same size; what it does not do is change the
 * ascent, descent or line height, because a line's height cannot depend on
 * which characters happen to be on it.
 *
 * Keep one on the stack. It borrows the faces rather than copying them.
 */
typedef struct {
    /** The faces, in the order they are tried. */
    const TinyFont* face[TINYIMG_FONT_MAX_FALLBACK];
    /** How many are in use. */
    uint32_t count;
} TinyFontSet;

/**
 * @brief Starts an empty chain.
 *
 * @param set The chain to initialize.
 * @return int TINYIMG_OK or TINYIMG_ERR_NULL.
 */
int tiny_font_set_init(TinyFontSet* set);

/**
 * @brief Appends a face to the end of a chain.
 *
 * @param set The chain.
 * @param font The face, which the chain borrows.
 * @return int TINYIMG_OK, TINYIMG_ERR_NULL, or TINYIMG_ERR_RANGE past
 * TINYIMG_FONT_MAX_FALLBACK.
 */
int tiny_font_set_add(TinyFontSet* set, const TinyFont* font);

/**
 * @brief Which face in a chain covers a codepoint.
 *
 * @param set The chain.
 * @param codepoint The character.
 * @return const TinyFont* The first face with a glyph for it, or the first face
 * in the chain when none has one, so a caller always has something to draw.
 */
const TinyFont* tiny_font_set_for(const TinyFontSet* set, uint32_t codepoint);

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
 * @brief How many variation axes a face declares.
 *
 * Zero for a static face, which is every face that carries no `fvar` table.
 *
 * @param font The face.
 * @return uint32_t The count, capped at TINYIMG_FONT_MAX_AXES.
 */
uint32_t tiny_font_axis_count(const TinyFont* font);

/**
 * @brief Reads one of a face's variation axes.
 *
 * @param font The face.
 * @param index Zero based, below tiny_font_axis_count.
 * @param out Receives the axis.
 * @return int TINYIMG_OK, TINYIMG_ERR_NULL, or TINYIMG_ERR_BOUNDS.
 */
int tiny_font_axis(const TinyFont* font, uint32_t index, TinyFontAxis* out);

/**
 * @brief Sets one axis, in the axis' own units.
 *
 * `tiny_font_set_axis(&font, TINYIMG_AXIS_WEIGHT, 700.0f)` is a bold instance
 * of one variable file, drawn from the same bytes as the regular one. The value
 * is clamped to the axis' range, so 900 on an axis that stops at 800 is 800
 * rather than an error.
 *
 * The outlines are interpolated from the face's `gvar` deltas, which is what
 * the format defines rather than an approximation of it: the deltas of every
 * region the coordinate falls in are scaled and summed, and points no region
 * mentions are inferred from their neighbors along the contour. **Advances
 * vary too**, from the same deltas' phantom points, so a bolder instance is
 * spaced as its designer set it.
 *
 * A face with no `fvar` reports TINYIMG_ERR_UNSUPPORTED_VARIANT, and a tag it
 * does not declare reports TINYIMG_ERR_NOT_FOUND. Neither is a failure a
 * caller has to prevent: ask the face what it has with tiny_font_axis first, or
 * ignore the result and get the default instance.
 *
 * @param font The face, whose variation state this changes.
 * @param tag The axis tag.
 * @param value The value, in the axis' units.
 * @return int TINYIMG_OK, TINYIMG_ERR_NULL,
 * TINYIMG_ERR_UNSUPPORTED_VARIANT for a static face, or
 * TINYIMG_ERR_NOT_FOUND for an axis the face does not declare.
 */
int tiny_font_set_axis(TinyFont* font, uint32_t tag, float value);

/**
 * @brief Returns a face to its default instance.
 *
 * @param font The face.
 * @return int TINYIMG_OK or TINYIMG_ERR_NULL.
 */
int tiny_font_reset_axes(TinyFont* font);

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
    /**
     * @brief Outline width in pixels, or zero for none.
     *
     * **A dilation of the glyph's coverage rather than an offset of its
     * outline**, which is what makes it one pass over a small mask instead of
     * a second rasterization. The difference shows at a sharp interior corner,
     * where a true offset curve would extend the two edges to a point and this
     * rounds it. At the one to four pixels an OG card uses it is not visible;
     * at twenty it is.
     *
     * Drawn under the fill for the whole run, so a stroke never prints over a
     * neighboring glyph's fill.
     */
    float stroke;
    /** Horizontal shadow offset in pixels. Negative is left. */
    float shadow_x;
    /** Vertical shadow offset in pixels. Negative is up. */
    float shadow_y;
    /**
     * @brief Shadow softness in pixels, or zero for a hard offset copy.
     *
     * Three box passes over the coverage, which is the usual approximation of
     * a gaussian and is within a level of one at this radius.
     */
    float shadow_blur;
    /** Non-zero to apply the face's kern pairs. */
    uint8_t kerning;
    /**
     * @brief The outline's color, as many channels as the image has.
     *
     * Read only when `stroke` is above zero. A zeroed style therefore needs no
     * color at all.
     */
    uint8_t stroke_color[4];
    /**
     * @brief The shadow's color, as many channels as the image has.
     *
     * Read only when a shadow is asked for, which is any non-zero offset or
     * blur. An alpha below 255 in the fourth channel is honored on an image
     * that has alpha.
     */
    uint8_t shadow_color[4];
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
    /**
     * @brief Both edges, by widening the spaces.
     *
     * The last line of a paragraph is set left, which is what every typesetter
     * does: stretching four words across a full measure is worse than the
     * ragged edge it was avoiding. A line with no space to widen is also set
     * left rather than having its letters spaced.
     */
    TINYIMG_ALIGN_JUSTIFY = 3,
} TinyTextAlign;

/**
 * @brief Where a run sits inside the height it was given.
 *
 * Needs a box height to mean anything; with none, every value is the same as
 * TINYIMG_VALIGN_TOP.
 */
typedef enum TinyTextVAlign
{
    /** First line box against the top edge. */
    TINYIMG_VALIGN_TOP = 0,
    /** The run centered in the height. */
    TINYIMG_VALIGN_MIDDLE = 1,
    /** Last line box against the bottom edge. */
    TINYIMG_VALIGN_BOTTOM = 2,
} TinyTextVAlign;

/**
 * @brief What happens to text the box cannot hold.
 */
typedef enum TinyTextOverflow
{
    /** Lines past the box are not drawn. */
    TINYIMG_OVERFLOW_CLIP = 0,
    /**
     * @brief The last line that fits ends in an ellipsis.
     *
     * A single U+2026, and a face without that glyph gets three full stops
     * instead, so a subset face that never included the character still
     * produces something a reader recognizes rather than a missing glyph box.
     */
    TINYIMG_OVERFLOW_ELLIPSIS = 1,
} TinyTextOverflow;

/**
 * @brief The rectangle a run is set inside, and how it sits there.
 *
 * A zeroed structure means no box at all: no wrapping, no height limit, left
 * and top, and nothing to overflow. That is what tiny_image_draw_text passes.
 */
typedef struct {
    /** Width to wrap inside, or zero for no wrapping. */
    uint32_t width;
    /** Height to fill, or zero for no limit. */
    uint32_t height;
    /** Where each line sits inside `width`. */
    TinyTextAlign align;
    /** Where the run sits inside `height`. */
    TinyTextVAlign valign;
    /** What happens to what does not fit. */
    TinyTextOverflow overflow;
} TinyTextBox;

/**
 * @brief What one line of a run occupies, and where it sits.
 *
 * Reported by tiny_text_lines so a caller can place something beside a line, or
 * measure how much of a string was actually drawn. The coordinates are relative
 * to the run's own origin, so adding the `x` and `y` a draw was given puts them
 * in image space.
 */
typedef struct {
    /** Byte offset of the line's first character in the run. */
    uint32_t at;
    /** Bytes the line holds, without the break that ended it. */
    uint32_t length;
    /** Advance width of the line in pixels. */
    float width;
    /** Left edge of the line box once the alignment is applied. */
    float x;
    /** Top edge of the line box. */
    float y;
    /** Baseline, measured down from the top of the run. */
    float baseline;
    /** Codepoints the line holds. */
    uint32_t glyphs;
    /** Codepoints the face had no glyph for. */
    uint32_t missing;
    /** Non-zero when the box's height stopped this line being drawn. */
    uint8_t clipped;
    /** Non-zero when this line was cut short and ends in an ellipsis. */
    uint8_t ellipsized;
} TinyTextLine;

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
 * @brief Draws text inside a rectangle, with the box's own settings.
 *
 * What tiny_image_draw_text_box does, plus vertical alignment, justification
 * and an ellipsis. The two share every line of layout, so a caller who
 * measures with tiny_text_lines and then draws gets the lines the measurement
 * described.
 *
 * A stroke or a shadow makes this three passes over the run rather than one:
 * every shadow, then every outline, then every fill. Drawing each glyph's three
 * layers together would let one glyph's outline print over the previous
 * glyph's fill, which is visible wherever two letters touch.
 *
 * @param image The image to draw on.
 * @param font The face.
 * @param text UTF-8, NUL terminated.
 * @param x Left edge of the box.
 * @param y Top edge of the box.
 * @param box The rectangle and how the run sits in it, or NULL for none.
 * @param style How to set it, or NULL for the defaults.
 * @param color The fill color, as many channels as the image has.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_image_draw_text_in(
    TinyImage* image, const TinyFont* font, const char* text, int32_t x,
    int32_t y, const TinyTextBox* box, const TinyTextStyle* style,
    const uint8_t* color
);

/** How many styled spans one rich run holds. */
#define TINYIMG_TEXT_MAX_RUNS 16

/**
 * @brief One styled span of a rich run.
 *
 * The unit a caller builds a mixed-style line out of: a bold word inside a
 * sentence, a colored number after a label, a second face for a script the
 * first one has no glyphs for.
 */
typedef struct {
    /** The face this span is set in. */
    const TinyFont* font;
    /** UTF-8, NUL terminated. */
    const char* text;
    /** How to set it, or NULL for the defaults at the face's own size. */
    const TinyTextStyle* style;
    /** The fill color, or NULL for the one the draw was given. */
    const uint8_t* color;
} TinyTextRun;

/**
 * @brief Draws text through a fallback chain.
 *
 * The same layout as tiny_image_draw_text_in, with the face chosen per
 * codepoint instead of once for the run. A chain of one face is exactly the
 * single-face path, so nothing about a caller who does not need this changes.
 *
 * @param image The image to draw on.
 * @param set The chain.
 * @param text UTF-8, NUL terminated.
 * @param x Left edge of the box.
 * @param y Top edge of the box.
 * @param box The rectangle and how the run sits in it, or NULL for none.
 * @param style How to set it, or NULL for the defaults.
 * @param color The fill color, as many channels as the image has.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_image_draw_text_set(
    TinyImage* image, const TinyFontSet* set, const char* text, int32_t x,
    int32_t y, const TinyTextBox* box, const TinyTextStyle* style,
    const uint8_t* color
);

/**
 * @brief Measures text as a fallback chain would set it.
 *
 * @param set The chain.
 * @param text UTF-8, NUL terminated.
 * @param box The rectangle it would be set in, or NULL for none.
 * @param style How it would be set, or NULL for the defaults.
 * @param out Receives the metrics. `missing` counts the codepoints no face in
 * the chain had a glyph for.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_text_measure_set(
    const TinyFontSet* set, const char* text, const TinyTextBox* box,
    const TinyTextStyle* style, TinyTextMetrics* out
);

/**
 * @brief Draws a sequence of styled spans as one run of text.
 *
 * The spans flow into each other rather than being drawn one after another: a
 * wrap can land in the middle of a span, and a line that mixes sizes sits on
 * one baseline, which is the largest ascent on that line. Two spans of
 * different sizes therefore share a baseline rather than each having their own,
 * which is what makes a superscript or a larger first letter look right.
 *
 * Every span carries its own face, size, tracking, kerning, outline and shadow.
 * **Kerning does not cross a span boundary**, because a kern pair is a property
 * of one face and the two glyphs either side of the boundary may be in
 * different ones.
 *
 * @param image The image to draw on.
 * @param runs The spans, in order.
 * @param count How many, up to TINYIMG_TEXT_MAX_RUNS.
 * @param x Left edge of the box.
 * @param y Top edge of the box.
 * @param box The rectangle and how the run sits in it, or NULL for none.
 * @param color The fill color for any span that does not name one.
 * @return int TINYIMG_OK, TINYIMG_ERR_RANGE past TINYIMG_TEXT_MAX_RUNS, or a
 * negative TinyImageError.
 */
int tiny_image_draw_text_runs(
    TinyImage* image, const TinyTextRun* runs, uint32_t count, int32_t x,
    int32_t y, const TinyTextBox* box, const uint8_t* color
);

/**
 * @brief Measures a sequence of styled spans without drawing them.
 *
 * The same layout the draw uses. `ascent` and `line_height` are the largest of
 * the spans on the first line, which is what the run actually occupies.
 *
 * @param runs The spans, in order.
 * @param count How many, up to TINYIMG_TEXT_MAX_RUNS.
 * @param box The rectangle they would be set in, or NULL for none.
 * @param out Receives the metrics.
 * @return int TINYIMG_OK, TINYIMG_ERR_RANGE, or a negative TinyImageError.
 */
int tiny_text_measure_runs(
    const TinyTextRun* runs, uint32_t count, const TinyTextBox* box,
    TinyTextMetrics* out
);

/**
 * @brief Measures a run line by line, without drawing it.
 *
 * The layout a draw would produce, one entry per line, including the lines the
 * box's height leaves out and which line an ellipsis lands on. What a caller
 * needs to place a badge beside the second line, or to tell that a headline
 * was truncated and pick a smaller size.
 *
 * @param font The face.
 * @param text UTF-8, NUL terminated.
 * @param box The rectangle it would be set in, or NULL for none.
 * @param style How it would be set, or NULL for the defaults.
 * @param lines Receives up to `capacity` entries. May be NULL to count only.
 * @param capacity How many entries `lines` holds.
 * @param count Receives the number of lines the run takes, which may be more
 * than `capacity`.
 * @return int TINYIMG_OK or a negative TinyImageError.
 */
int tiny_text_lines(
    const TinyFont* font, const char* text, const TinyTextBox* box,
    const TinyTextStyle* style, TinyTextLine* lines, uint32_t capacity,
    uint32_t* count
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
 * @brief Size of a TinyTextBox, for the same reason.
 *
 * @return uint32_t sizeof(TinyTextBox).
 */
uint32_t tiny_text_box_sizeof(void);

/**
 * @brief Size of a TinyTextLine, for the same reason.
 *
 * @return uint32_t sizeof(TinyTextLine).
 */
uint32_t tiny_text_line_sizeof(void);

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
