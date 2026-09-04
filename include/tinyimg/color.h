/**
 * @file color.h
 * @author Gregory Mitchell (me@gmitch215.xyz)
 * @brief Color and pixel manipulation utilities.
 * @version 1.0.0
 * @date 2026-08-31
 *
 * @copyright Copyright (c) 2026
 *
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "tinyimg/image.h"

#ifdef __cplusplus
extern "C" {
#endif

#pragma region color conversion

/**
 * @brief Converts RGB color values to a grayscale value using the luminosity
 * method.
 *
 * The formula used is: grayscale = 0.21 * R + 0.72 * G + 0.07 * B
 *
 * @param r Red component (0-255)
 * @param g Green component (0-255)
 * @param b Blue component (0-255)
 * @return uint8_t Grayscale value (0-255)
 */
int tiny_rgb_to_grayscale(uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief Converts a grayscale value to RGB color values.
 *
 * The grayscale value is replicated across the R, G, and B channels.
 *
 * @param grayscale Grayscale value (0-255)
 * @param r Pointer to store the Red component (0-255)
 * @param g Pointer to store the Green component (0-255)
 * @param b Pointer to store the Blue component (0-255)
 * @return int 0 on success, non-zero on failure
 */
int tiny_grayscale_to_rgb(
    uint8_t grayscale, uint8_t* r, uint8_t* g, uint8_t* b
);

/**
 * @brief Converts RGB color values to HSV (Hue, Saturation, Value) color space.
 *
 * The RGB values are expected to be in the range [0, 255], and the resulting
 * HSV values will be in the ranges:
 * - H: [0, 360)
 * - S: [0, 1]
 * - V: [0, 1]
 *
 * @param r Red component (0-255)
 * @param g Green component (0-255)
 * @param b Blue component (0-255)
 * @param h Pointer to store the Hue value (0-360)
 * @param s Pointer to store the Saturation value (0-1)
 * @param v Pointer to store the Value (Brightness) value (0-1)
 * @return int 0 on success, non-zero on failure
 */
int tiny_rgb_to_hsv(
    uint8_t r, uint8_t g, uint8_t b, float* h, float* s, float* v
);

/**
 * @brief Converts HSV (Hue, Saturation, Value) color space values to RGB color
 * values.
 *
 * The HSV values are expected to be in the ranges:
 * - H: [0, 360)
 * - S: [0, 1]
 * - V: [0, 1]
 * The resulting RGB values will be in the range [0, 255].
 *
 * @param h Hue value (0-360)
 * @param s Saturation value (0-1)
 * @param v Value (Brightness) value (0-1)
 * @param r Pointer to store the Red component (0-255)
 * @param g Pointer to store the Green component (0-255)
 * @param b Pointer to store the Blue component (0-255)
 * @return int 0 on success, non-zero on failure
 */
int tiny_hsv_to_rgb(
    float h, float s, float v, uint8_t* r, uint8_t* g, uint8_t* b
);

/**
 * @brief Converts RGB color values to HSL (Hue, Saturation, Lightness) color
 * space.
 *
 * The RGB values are expected to be in the range [0, 255], and the resulting
 * HSL values will be in the ranges:
 * - H: [0, 360)
 * - S: [0, 1]
 * - L: [0, 1]
 *
 * @param r Red component (0-255)
 * @param g Green component (0-255)
 * @param b Blue component (0-255)
 * @param h Pointer to store the Hue value (0-360)
 * @param s Pointer to store the Saturation value (0-1)
 * @param l Pointer to store the Lightness value (0-1)
 * @return int 0 on success, non-zero on failure
 */
int tiny_rgb_to_hsl(
    uint8_t r, uint8_t g, uint8_t b, float* h, float* s, float* l
);

/**
 * @brief Converts HSL (Hue, Saturation, Lightness) color space values to RGB
 * color values.
 *
 * The HSL values are expected to be in the ranges:
 * - H: [0, 360)
 * - S: [0, 1]
 * - L: [0, 1]
 * The resulting RGB values will be in the range [0, 255].
 *
 * @param h Hue value (0-360)
 * @param s Saturation value (0-1)
 * @param l Lightness value (0-1)
 * @param r Pointer to store the Red component (0-255)
 * @param g Pointer to store the Green component (0-255)
 * @param b Pointer to store the Blue component (0-255)
 * @return int 0 on success, non-zero on failure
 */
int tiny_hsl_to_rgb(
    float h, float s, float l, uint8_t* r, uint8_t* g, uint8_t* b
);

/**
 * @brief Converts RGB color values to CMYK (Cyan, Magenta, Yellow, Key/Black)
 * color space.
 *
 * The RGB values are expected to be in the range [0, 255], and the resulting
 * CMYK values will be in the ranges:
 * - C: [0, 1]
 * - M: [0, 1]
 * - Y: [0, 1]
 * - K: [0, 1]
 *
 * @param r Red component (0-255)
 * @param g Green component (0-255)
 * @param b Blue component (0-255)
 * @param c Pointer to store the Cyan value (0-1)
 * @param m Pointer to store the Magenta value (0-1)
 * @param y Pointer to store the Yellow value (0-1)
 * @param k Pointer to store the Key/Black value (0-1)
 * @return int 0 on success, non-zero on failure
 */
int tiny_rgb_to_cmyk(
    uint8_t r, uint8_t g, uint8_t b, float* c, float* m, float* y, float* k
);

/**
 * @brief Converts CMYK (Cyan, Magenta, Yellow, Key/Black) color space values
 * to RGB color values.
 *
 * The CMYK values are expected to be in the ranges:
 * - C: [0, 1]
 * - M: [0, 1]
 * - Y: [0, 1]
 * - K: [0, 1]
 * The resulting RGB values will be in the range [0, 255].
 *
 * @param c Cyan value (0-1)
 * @param m Magenta value (0-1)
 * @param y Yellow value (0-1)
 * @param k Key/Black value (0-1)
 * @param r Pointer to store the Red component (0-255)
 * @param g Pointer to store the Green component (0-255)
 * @param b Pointer to store the Blue component (0-255)
 * @return int 0 on success, non-zero on failure
 */
int tiny_cmyk_to_rgb(
    float c, float m, float y, float k, uint8_t* r, uint8_t* g, uint8_t* b
);

#pragma endregion

#pragma region color interpolation

/**
 * @brief Interpolates between two RGB colors based on a given interpolation
 * factor t.
 *
 * The interpolation factor t should be in the range [0, 1], where:
 * - t = 0 results in the first color (r1, g1, b1)
 * - t = 1 results in the second color (r2, g2, b2)
 *
 * @param r1 Red component of the first color (0-255)
 * @param g1 Green component of the first color (0-255)
 * @param b1 Blue component of the first color (0-255)
 * @param r2 Red component of the second color (0-255)
 * @param g2 Green component of the second color (0-255)
 * @param b2 Blue component of the second color (0-255)
 * @param t Interpolation factor (0-1)
 * @param r_out Pointer to store the interpolated Red component (0-255)
 * @param g_out Pointer to store the interpolated Green component (0-255)
 * @param b_out Pointer to store the interpolated Blue component (0-255)
 * @return int 0 on success, non-zero on failure
 */
int tiny_interpolate_rgb(
    uint8_t r1, uint8_t g1, uint8_t b1, uint8_t r2, uint8_t g2, uint8_t b2,
    float t, uint8_t* r_out, uint8_t* g_out, uint8_t* b_out
);

/**
 * @brief Interpolates between two HSV colors based on a given interpolation
 * factor t.
 *
 * The interpolation factor t should be in the range [0, 1], where:
 * - t = 0 results in the first color (h1, s1, v1)
 * - t = 1 results in the second color (h2, s2, v2)
 *
 * @param h1 Hue of the first color (0-360)
 * @param s1 Saturation of the first color (0-1)
 * @param v1 Value of the first color (0-1)
 * @param h2 Hue of the second color (0-360)
 * @param s2 Saturation of the second color (0-1)
 * @param v2 Value of the second color (0-1)
 * @param t Interpolation factor (0-1)
 * @param h_out Pointer to store the interpolated Hue value (0-360)
 * @param s_out Pointer to store the interpolated Saturation value (0-1)
 * @param v_out Pointer to store the interpolated Value (0-1)
 * @return int 0 on success, non-zero on failure
 */
int tiny_interpolate_hsv(
    float h1, float s1, float v1, float h2, float s2, float v2, float t,
    float* h_out, float* s_out, float* v_out
);

/**
 * @brief Interpolates between two HSL colors based on a given interpolation
 * factor t.
 *
 * The interpolation factor t should be in the range [0, 1], where:
 * - t = 0 results in the first color (h1, s1, l1)
 * - t = 1 results in the second color (h2, s2, l2)
 *
 * @param h1 Hue of the first color (0-360)
 * @param s1 Saturation of the first color (0-1)
 * @param l1 Lightness of the first color (0-1)
 * @param h2 Hue of the second color (0-360)
 * @param s2 Saturation of the second color (0-1)
 * @param l2 Lightness of the second color (0-1)
 * @param t Interpolation factor (0-1)
 * @param h_out Pointer to store the interpolated Hue value (0-360)
 * @param s_out Pointer to store the interpolated Saturation value (0-1)
 * @param l_out Pointer to store the interpolated Lightness value (0-1)
 * @return int 0 on success, non-zero on failure
 */
int tiny_interpolate_hsl(
    float h1, float s1, float l1, float h2, float s2, float l2, float t,
    float* h_out, float* s_out, float* l_out
);

#pragma endregion

#pragma region color palette generation

/**
 * @brief Generates a gradient of RGB colors between two specified colors over
 * a given number of steps.
 *
 * The function returns a dynamically allocated array of integers, where each
 * integer holds one RGB color as 0xRRGGBB. The memory comes from the arena, so
 * tiny_arena_reset() or a matching tiny_arena_release() reclaims it and
 * nothing frees it individually.
 *
 * @param r1 Red component of the starting color (0-255)
 * @param g1 Green component of the starting color (0-255)
 * @param b1 Blue component of the starting color (0-255)
 * @param r2 Red component of the ending color (0-255)
 * @param g2 Green component of the ending color (0-255)
 * @param b2 Blue component of the ending color (0-255)
 * @param steps Number of steps in the gradient (must be greater than 1)
 * @return int* Pointer to an array of integers representing the gradient
 * colors, or NULL on failure (e.g., if steps is less than 2).
 */
int* tiny_gradient_rgb(
    uint8_t r1, uint8_t g1, uint8_t b1, uint8_t r2, uint8_t g2, uint8_t b2,
    size_t steps
);

/**
 * @brief Generates a gradient of RGBA colors between two specified colors over
 * a given number of steps.
 *
 * The function returns a dynamically allocated array of integers, where each
 * integer holds one RGBA color as 0xRRGGBBAA. The memory comes from the arena,
 * so tiny_arena_reset() or a matching tiny_arena_release() reclaims it and
 * nothing frees it individually.
 *
 * @param r1 Red component of the starting color (0-255)
 * @param g1 Green component of the starting color (0-255)
 * @param b1 Blue component of the starting color (0-255)
 * @param a1 Alpha component of the starting color (0-255)
 * @param r2 Red component of the ending color (0-255)
 * @param g2 Green component of the ending color (0-255)
 * @param b2 Blue component of the ending color (0-255)
 * @param a2 Alpha component of the ending color (0-255)
 * @param steps Number of steps in the gradient (must be greater than 1)
 * @return int* Pointer to an array of integers representing the gradient
 * colors, or NULL on failure.
 */
int* tiny_gradient_rgba(
    uint8_t r1, uint8_t g1, uint8_t b1, uint8_t a1, uint8_t r2, uint8_t g2,
    uint8_t b2, uint8_t a2, size_t steps
);

/**
 * @brief Generates a gradient of HSV colors between two specified colors over
 * a given number of steps.
 *
 * The function returns a dynamically allocated array of integers, where each
 * entry is three floats: hue, saturation and value. The memory comes from the
 * arena, so tiny_arena_reset() or a matching tiny_arena_release() reclaims it
 * and nothing frees it individually.
 *
 * @param h1 Hue of the starting color (0-360)
 * @param s1 Saturation of the starting color (0-1)
 * @param v1 Value (Brightness) of the starting color (0-1)
 * @param h2 Hue of the ending color (0-360)
 * @param s2 Saturation of the ending color (0-1)
 * @param v2 Value (Brightness) of the ending color (0-1)
 * @param steps Number of steps in the gradient (must be greater than 1)
 * @return float* The gradient, or NULL when steps is below two.
 */
float* tiny_gradient_hsv(
    float h1, float s1, float v1, float h2, float s2, float v2, size_t steps
);

/**
 * @brief Generates a gradient of HSL colors between two specified colors over
 * a given number of steps.
 *
 * The function returns a dynamically allocated array of integers, where each
 * entry is three floats: hue, saturation and lightness. The memory comes from
 * the arena, so tiny_arena_reset() or a matching tiny_arena_release() reclaims
 * it and nothing frees it individually.
 *
 * @param h1 Hue of the starting color (0-360)
 * @param s1 Saturation of the starting color (0-1)
 * @param l1 Lightness of the starting color (0-1)
 * @param h2 Hue of the ending color (0-360)
 * @param s2 Saturation of the ending color (0-1)
 * @param l2 Lightness of the ending color (0-1)
 * @param steps Number of steps in the gradient (must be greater than 1)
 * @return float* The gradient, or NULL when steps is below two.
 */
float* tiny_gradient_hsl(
    float h1, float s1, float l1, float h2, float s2, float l2, size_t steps
);

/**
 * @brief Generates a gradient of CMYK colors between two specified colors over
 * a given number of steps.
 *
 * The function returns a dynamically allocated array of integers, where each
 * entry is four floats: cyan, magenta, yellow and key. The memory comes from
 * the arena, so tiny_arena_reset() or a matching tiny_arena_release() reclaims
 * it and nothing frees it individually.
 *
 * @param c1 Cyan of the starting color (0-1)
 * @param m1 Magenta of the starting color (0-1)
 * @param y1 Yellow of the starting color (0-1)
 * @param k1 Key/Black of the starting color (0-1)
 * @param c2 Cyan of the ending color (0-1)
 * @param m2 Magenta of the ending color (0-1)
 * @param y2 Yellow of the ending color (0-1)
 * @param k2 Key/Black of the ending color (0-1)
 * @param steps Number of steps in the gradient (must be greater than 1)
 * @return float* The gradient, or NULL when steps is below two.
 */
float* tiny_gradient_cmyk(
    float c1, float m1, float y1, float k1, float c2, float m2, float y2,
    float k2, size_t steps
);

/**
 * @brief Generates a multi-gradient of RGB colors based on an array of input
 * colors and a specified number of steps equally distributed across the input
 * colors.
 *
 * The function returns a dynamically allocated array of integers, where each
 * integer holds one RGB color as 0xRRGGBB. The memory comes from the arena, so
 * tiny_arena_reset() or a matching tiny_arena_release() reclaims it and
 * nothing frees it individually.
 *
 * @param colors Pointer to an array of uint8_t values representing the input
 * colors. Each color should be represented by three consecutive values (R, G,
 * B).
 * @param num_colors The number of colors in the input array.
 * @param steps The total number of steps in the multi-gradient (must be greater
 * than 1).
 * @return int* Pointer to an array of integers representing the multi-gradient
 * colors, or NULL on failure (e.g., if num_colors is less than 2 or if steps
 * is less than 2).
 */
int* tiny_multigradient_rgb(uint8_t* colors, size_t num_colors, size_t steps);

/**
 * @brief Generates a multi-gradient of RGB colors based on an array of input
 * colors and a specified number of steps distributed based on
 * `color_distribution`.
 *
 * The function returns a dynamically allocated array of integers, where each
 * integer holds one RGB color as 0xRRGGBB. The memory comes from the arena, so
 * tiny_arena_reset() or a matching tiny_arena_release() reclaims it and
 * nothing frees it individually.
 *
 * @param colors Pointer to an array of uint8_t values representing the input
 * colors. Each color should be represented by three consecutive values (R, G,
 * B).
 * @param num_colors The number of colors in the input array.
 * @param steps The total number of steps in the multi-gradient (must be greater
 * than 1).
 * @param color_distribution One weight per transition, so `num_colors - 1`
 * of them, each between 0.0 and 1.0 and together summing to 1.0. A transition
 * given a larger weight takes up more of the gradient.
 * @return int* Pointer to an array of integers representing the multi-gradient
 * colors, or NULL on failure (e.g., if num_colors is less than 2 or if steps
 * is less than 2; if color_distribution is NULL, does not have the correct
 * size, or adds up to 1.0).
 */
int* tiny_multigradient_rgb_sized(
    uint8_t* colors, size_t num_colors, size_t steps, float* color_distribution
);

/**
 * @brief Generates a multi-gradient of RGBA colors based on an array of input
 * colors and a specified number of steps equally distributed across the input
 * colors.
 *
 * The function returns a dynamically allocated array of integers, where each
 * integer holds one RGBA color as 0xRRGGBBAA. The memory comes from the arena,
 * so tiny_arena_reset() or a matching tiny_arena_release() reclaims it and
 * nothing frees it individually.
 *
 * @param colors Pointer to an array of uint8_t values representing the input
 * colors. Each color should be represented by four consecutive values (R, G,
 * B, A).
 * @param num_colors The number of colors in the input array.
 * @param steps The total number of steps in the multi-gradient (must be greater
 * than 1).
 * @return int* Pointer to an array of integers representing the multi-gradient
 * colors, or NULL on failure (e.g., if num_colors is less than 2 or if steps
 * is less than 2).
 */
int* tiny_multigradient_rgba(uint8_t* colors, size_t num_colors, size_t steps);

/**
 * @brief Generates a multi-gradient of RGBA colors based on an array of input
 * colors and a specified number of steps distributed based on
 * `color_distribution`.
 *
 * The function returns a dynamically allocated array of integers, where each
 * integer holds one RGBA color as 0xRRGGBBAA. The memory comes from the arena,
 * so tiny_arena_reset() or a matching tiny_arena_release() reclaims it and
 * nothing frees it individually.
 *
 * @param colors Pointer to an array of uint8_t values representing the input
 * colors. Each color should be represented by four consecutive values (R, G,
 * B, A).
 * @param num_colors The number of colors in the input array.
 * @param steps The total number of steps in the multi-gradient (must be greater
 * than 1).
 * @param color_distribution One weight per transition, so `num_colors - 1`
 * of them, each between 0.0 and 1.0 and together summing to 1.0. A transition
 * given a larger weight takes up more of the gradient.
 * @return int* Pointer to an array of integers representing the multi-gradient
 * colors, or NULL on failure (e.g., if num_colors is less than 2 or if steps
 * is less than 2; if color_distribution is NULL, does not have the correct
 * size, or adds up to 1.0).
 */
int* tiny_multigradient_rgba_sized(
    uint8_t* colors, size_t num_colors, size_t steps, float* color_distribution
);

/**
 * @brief Generates a multi-gradient of HSV colors based on an array of input
 * colors and a specified number of steps equally distributed across the input
 * colors.
 *
 * The function returns a dynamically allocated array of integers, where each
 * entry is three floats: hue, saturation and value. The memory comes from the
 * arena, so tiny_arena_reset() or a matching tiny_arena_release() reclaims it
 * and nothing frees it individually.
 *
 * @param colors Pointer to an array of float values representing the input
 * colors. Each color should be represented by three consecutive values (H, S,
 * V).
 * @param num_colors The number of colors in the input array.
 * @param steps The total number of steps in the multi-gradient (must be greater
 * than 1).
 * @return float* The gradient, or NULL on failure.
 */
float* tiny_multigradient_hsv(float* colors, size_t num_colors, size_t steps);

/**
 * @brief Generates a multi-gradient of HSV colors based on an array of input
 * colors and a specified number of steps distributed based on
 * `color_distribution`.
 *
 * The function returns a dynamically allocated array of integers, where each
 * entry is three floats: hue, saturation and value. The memory comes from the
 * arena, so tiny_arena_reset() or a matching tiny_arena_release() reclaims it
 * and nothing frees it individually.
 *
 * @param colors Pointer to an array of float values representing the input
 * colors. Each color should be represented by three consecutive values (H, S,
 * V).
 * @param num_colors The number of colors in the input array.
 * @param steps The total number of steps in the multi-gradient (must be greater
 * than 1).
 * @param color_distribution One weight per transition, so `num_colors - 1`
 * of them, each between 0.0 and 1.0 and together summing to 1.0. A transition
 * given a larger weight takes up more of the gradient.
 * @return float* The gradient, or NULL on failure.
 */
float* tiny_multigradient_hsv_sized(
    float* colors, size_t num_colors, size_t steps, float* color_distribution
);

/**
 * @brief Generates a multi-gradient of HSL colors based on an array of input
 * colors and a specified number of steps equally distributed across the input
 * colors.
 *
 * The function returns a dynamically allocated array of integers, where each
 * entry is three floats: hue, saturation and lightness. The memory comes from
 * the arena, so tiny_arena_reset() or a matching tiny_arena_release() reclaims
 * it and nothing frees it individually.
 *
 * @param colors Pointer to an array of float values representing the input
 * colors. Each color should be represented by three consecutive values (H, S,
 * L).
 * @param num_colors The number of colors in the input array.
 * @param steps The total number of steps in the multi-gradient (must be greater
 * than 1).
 * @return float* The gradient, or NULL on failure.
 */
float* tiny_multigradient_hsl(float* colors, size_t num_colors, size_t steps);

/**
 * @brief Generates a multi-gradient of HSL colors based on an array of input
 * colors and a specified number of steps distributed based on
 * `color_distribution`.
 *
 * The function returns a dynamically allocated array of integers, where each
 * entry is three floats: hue, saturation and lightness. The memory comes from
 * the arena, so tiny_arena_reset() or a matching tiny_arena_release() reclaims
 * it and nothing frees it individually.
 *
 * @param colors Pointer to an array of float values representing the input
 * colors. Each color should be represented by three consecutive values (H, S,
 * L).
 * @param num_colors The number of colors in the input array.
 * @param steps The total number of steps in the multi-gradient (must be greater
 * than 1).
 * @param color_distribution One weight per transition, so `num_colors - 1`
 * of them, each between 0.0 and 1.0 and together summing to 1.0. A transition
 * given a larger weight takes up more of the gradient.
 * @return float* The gradient, or NULL on failure.
 */
float* tiny_multigradient_hsl_sized(
    float* colors, size_t num_colors, size_t steps, float* color_distribution
);

/**
 * @brief Generates a multi-gradient of CMYK colors based on an array of input
 * colors and a specified number of steps equally distributed across the input
 * colors.
 *
 * The function returns a dynamically allocated array of integers, where each
 * entry is four floats: cyan, magenta, yellow and key. The memory comes from
 * the arena, so tiny_arena_reset() or a matching tiny_arena_release() reclaims
 * it and nothing frees it individually.
 *
 * @param colors Pointer to an array of float values representing the input
 * colors. Each color should be represented by four consecutive values (C, M,
 * Y, K).
 * @param num_colors The number of colors in the input array.
 * @param steps The total number of steps in the multi-gradient (must be greater
 * than 1).
 * @return float* The gradient, or NULL on failure.
 */
float* tiny_multigradient_cmyk(float* colors, size_t num_colors, size_t steps);

/**
 * @brief Generates a multi-gradient of CMYK colors based on an array of input
 * colors and a specified number of steps distributed based on
 * `color_distribution`.
 *
 * The function returns a dynamically allocated array of integers, where each
 * entry is four floats: cyan, magenta, yellow and key. The memory comes from
 * the arena, so tiny_arena_reset() or a matching tiny_arena_release() reclaims
 * it and nothing frees it individually.
 *
 * @param colors Pointer to an array of float values representing the input
 * colors. Each color should be represented by four consecutive values (C, M,
 * Y, K).
 * @param num_colors The number of colors in the input array.
 * @param steps The total number of steps in the multi-gradient (must be greater
 * than 1).
 * @param color_distribution One weight per transition, so `num_colors - 1`
 * of them, each between 0.0 and 1.0 and together summing to 1.0. A transition
 * given a larger weight takes up more of the gradient.
 * @return float* The gradient, or NULL on failure.
 */
float* tiny_multigradient_cmyk_sized(
    float* colors, size_t num_colors, size_t steps, float* color_distribution
);

#pragma endregion

#pragma region icc

/**
 * @brief A parsed matrix and TRC profile.
 *
 * The only kind of ICC profile this reads, and between them these cover sRGB,
 * Display P3, Adobe RGB and Rec. 2020, which is what web images carry. A
 * profile whose transform is a lookup table instead is rejected with
 * TINYIMG_ERR_UNSUPPORTED_VARIANT rather than approximated, because an
 * approximation of a printer profile is a wrong color that looks like a right
 * one.
 */
typedef struct {
    /**
     * @brief Columns of the RGB to XYZ matrix, row major.
     *
     * Already adapted to whatever white point the profile's `chad` tag names,
     * so this maps the profile's RGB straight to D50 connection space.
     */
    float to_xyz[9];
    /** The white point the profile records, as XYZ. */
    float white[3];
    /** One tone curve per channel, linear light out, sampled at 256 points. */
    float curve[3][256];
    /** Non-zero when every channel's curve is the identity. */
    uint8_t linear;
} TinyIccProfile;

/**
 * @brief Reads a matrix and TRC profile.
 *
 * @param profile Receives the parsed profile.
 * @param data The profile bytes.
 * @param size How many bytes.
 * @return int TINYIMG_OK, TINYIMG_ERR_NULL, TINYIMG_ERR_CORRUPT for a profile
 * that does not parse, or TINYIMG_ERR_UNSUPPORTED_VARIANT for one whose
 * transform is a lookup table rather than a matrix.
 */
int tiny_icc_parse(TinyIccProfile* profile, const uint8_t* data, size_t size);

/**
 * @brief The sRGB profile, without needing a file for it.
 *
 * @param profile Receives it.
 * @return int TINYIMG_OK or TINYIMG_ERR_NULL.
 */
int tiny_icc_srgb(TinyIccProfile* profile);

/**
 * @brief Converts one color from a profile's space to sRGB.
 *
 * @param profile The source profile.
 * @param in Three channels in the profile's space.
 * @param out Receives three channels of sRGB.
 * @return int TINYIMG_OK or TINYIMG_ERR_NULL.
 */
int tiny_icc_to_srgb(
    const TinyIccProfile* profile, const uint8_t* in, uint8_t* out
);

/**
 * @brief Converts a whole image from a profile's space to sRGB.
 *
 * @param image The image to convert.
 * @param profile The profile its pixels are in.
 * @return int TINYIMG_OK, TINYIMG_ERR_NULL, or TINYIMG_ERR_NO_CHANNEL when the
 * image has fewer than three channels.
 */
int tiny_icc_convert_image(TinyImage* image, const TinyIccProfile* profile);

/**
 * @brief The matrix taking one profile's RGB to another's.
 *
 * @param out Receives nine weights, row major.
 * @param from The source profile.
 * @param to The destination profile.
 * @return int TINYIMG_OK, TINYIMG_ERR_NULL, or TINYIMG_ERR_RANGE when the
 * destination's matrix cannot be inverted.
 */
int tiny_icc_matrix_between(
    float* out, const TinyIccProfile* from, const TinyIccProfile* to
);

#pragma endregion

#ifdef __cplusplus
}
#endif
