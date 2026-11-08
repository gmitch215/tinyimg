/*
 * Generated from bench/baselines/fit/cells.json by scripts/cost-model.ts. Do
 * not hand edit. Rates are microseconds per million samples on the machine the
 * cells were timed on; a multiplier or fraction is in thousandths. Effort rows
 * are the format's value, with row 9 for lossless WebP.
 */

#define COST_COMPRESSION_NONE 327u
#define COST_COMPRESSION_FAST 606u
#define COST_COMPRESSION_BEST 1884u

static const uint32_t cost_alpha[9][2][4] = {
    {0},
    /* png */
    {{5680u, 14783u, 14333u, 13752u}, {5680u, 14783u, 14333u, 13752u}},
    /* jpeg */
    {{5829u, 2384u, 1554u, 475u}, {6240u, 5504u, 4285u, 947u}},
    /* bmp */
    {{5319u, 2834u, 2036u, 1939u}, {5319u, 2834u, 2036u, 1939u}},
    /* gif */
    {{6687u, 6373u, 5898u, 5831u}, {6687u, 6373u, 5898u, 5831u}},
    /* tiff */
    {{4698u, 2746u, 1975u, 1920u}, {4698u, 2746u, 1975u, 1920u}},
    /* webp */
    {{7741u, 8751u, 7726u, 7335u}, {7741u, 8751u, 7726u, 7335u}},
    /* avif */
    {{193612u, 195673u, 193258u, 195634u},
     {193612u, 195673u, 193258u, 195634u}},
    {0},
};

static const uint32_t cost_beta[9][2] = {
    {0u, 0u},
    /* png */
    {7907u, 7907u},
    /* jpeg */
    {15483u, 13199u},
    /* bmp */
    {0u, 0u},
    /* gif */
    {0u, 0u},
    /* tiff */
    {3986u, 3986u},
    /* webp */
    {75758u, 75758u},
    /* avif */
    {0u, 0u},
    {0u, 0u},
};

static const uint8_t cost_rows[9] = {0, 1, 0, 0, 2, 0, 1, 2, 0};

static const uint16_t cost_stored[9] = {0, 0, 0, 900, 0, 900, 0, 0, 0};

static const uint16_t cost_consumed[9][2][4] = {
    {{0}},
    /* png */
    {{1000, 1000, 1000, 1000}, {1000, 1000, 1000, 1000}},
    /* jpeg */
    {{987, 987, 987, 987}, {996, 996, 994, 212}},
    /* bmp */
    {{1000, 1000, 1000, 1000}, {1000, 1000, 1000, 1000}},
    /* gif */
    {{1000, 1000, 1000, 1000}, {1000, 1000, 1000, 1000}},
    /* tiff */
    {{997, 997, 997, 997}, {997, 997, 997, 997}},
    /* webp */
    {{1000, 1000, 1000, 1000}, {1000, 1000, 1000, 1000}},
    /* avif */
    {{995, 995, 995, 995}, {995, 995, 995, 995}},
    {{0}},
};

static const uint32_t cost_encode[9] = {0u,     332880u, 18142u,
                                        6009u,  57618u,  171493u,
                                        40432u, 114308u, 0u};

static const uint16_t cost_effort_decode[10][4] = {
    {1000u, 1000u, 1000u, 1000u},
    /* png */
    {1000u, 1000u, 1000u, 1000u},
    /* jpeg */
    {902u, 1000u, 1000u, 1000u},
    /* bmp */
    {1000u, 1000u, 1000u, 1000u},
    /* gif */
    {1000u, 1000u, 1000u, 1000u},
    /* tiff */
    {1000u, 1000u, 1000u, 1000u},
    /* webp */
    {1000u, 763u, 724u, 767u},
    /* avif */
    {389u, 382u, 380u, 378u},
    /* heif */
    {1000u, 1000u, 1000u, 1000u},
    /* webp lossless */
    {1000u, 1000u, 1000u, 1000u},
};

static const uint16_t cost_effort_encode[10] = {1000u, 599u,  1000u, 1000u,
                                                1000u, 1000u, 843u,  1000u,
                                                1000u, 1000u};

static const uint8_t cost_quality_points[5] = {50, 75, 85, 90, 95};

static const uint16_t cost_quality[9][5] = {
    {1000u, 1000u, 1000u, 1000u, 1000u},
    /* png */
    {1000u, 1000u, 1000u, 1000u, 1000u},
    /* jpeg */
    {727u, 868u, 1000u, 1576u, 1899u},
    /* bmp */
    {1000u, 1000u, 1000u, 1000u, 1000u},
    /* gif */
    {1000u, 1000u, 1000u, 1000u, 1000u},
    /* tiff */
    {1000u, 1000u, 1000u, 1000u, 1000u},
    /* webp */
    {935u, 1000u, 1101u, 1190u, 1358u},
    /* avif */
    {852u, 1000u, 1092u, 1175u, 1312u},
    {1000u, 1000u, 1000u, 1000u, 1000u},
};

#define COST_LOSSLESS_MEDIUM 100000u
#define COST_LOSSLESS_LARGE 1000000u

static const uint16_t cost_lossless[9][3] = {
    {0u, 0u, 0u},           {0u, 0u, 0u}, {0u, 0u, 0u},
    {0u, 0u, 0u},           {0u, 0u, 0u}, {0u, 0u, 0u},
    {3767u, 6609u, 10073u}, {0u, 0u, 0u}, {0u, 0u, 0u},
};

static const uint16_t cost_alpha_factor[9] = {1000u, 1349u, 1020u, 591u, 788u,
                                              1359u, 1828u, 1004u, 1000u};

static const uint16_t cost_q_decode[9] = {1000, 1206, 1251, 1046, 1311,
                                          4320, 2789, 1228, 1000};

static const uint16_t cost_q_encode[9] = {1000, 1220, 1362, 1077, 2092,
                                          1291, 1750, 1247, 1000};

static const uint16_t cost_n_decode[9] = {0, 88, 317, 33, 260, 198, 55, 44, 0};

static const uint16_t cost_n_encode[9] = {0,   671,  1342, 488, 488,
                                          671, 1903, 1342, 0};
