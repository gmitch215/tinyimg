/**
 * SHA-256 of decoded pixel buffers, recorded so both lanes have to agree with each other and with
 * every future build.
 *
 * The node lane compiles the module from bytes and the workers lane instantiates a pre-compiled one
 * inside workerd. Structural assertions pass in both even if a decode is subtly wrong in one, so the
 * only check that catches a runtime specific difference is comparing the pixels themselves against a
 * fixed value.
 *
 * A change here is a change to decoder output. Update it only alongside the reason, never to make a
 * failing test pass.
 */
export const golden = {
	/**
	 * The reference picture at 320x180x3.
	 *
	 * Twelve fixtures across four codecs land on this value, which is what makes it worth more
	 * than any single reference file: a 24 bit BMP, an 8 bit PNG, a 16 bit PNG, an Adam7 interlaced
	 * PNG, seven TIFFs covering four compressions, both byte orders, the horizontal predictor and a
	 * seven row strip height, and a WebP lossless stream.
	 *
	 * Codecs disagreeing about the same picture is exactly what a shared digest catches. BMP stores
	 * rows bottom-up in BGR, PNG stores them top-down in RGB, TIFF can do either while differencing
	 * each row against the one before it, and WebP lossless subtracts green from red and blue and
	 * then predicts every pixel from its neighbors through a mode chosen per tile. A row order,
	 * channel order, predictor or transform mistake in any of them breaks the match rather than
	 * producing a plausible-looking image.
	 */
	reference: '9478f8be468a7d42d1f6ac68e849a28bb91dff558fa3434b6ad47f18e8e1059a',

	/** The reference picture box averaged to an eighth, 40x23x3. Also shared by BMP and PNG. */
	referenceEighth: 'f21a2940ba76a7d6dc5b0fc0ca796091a197b922927ae543d2ddb8fddd7f8450',

	/**
	 * The reference picture through a 256 color palette, at 320x180x3.
	 *
	 * Shared by `derived/base-rle8.bmp`, both GIFs and `derived/base-palette.tif`, which is not a
	 * coincidence worth glossing over: all four went through the same quantizer in ImageMagick, so
	 * they carry the same palette and the same indices, and four unrelated container formats have
	 * to arrive at one answer. GIF reads its indices least significant bit first out of a chained
	 * LZW stream, RLE8 reads runs of them, and TIFF reads them against a color map of 16 bit
	 * values in three separate channel runs, so agreement rules out an indexing or scaling mistake
	 * in any of them.
	 */
	bmpRle8: '89d2b141f75855d464a55a655605b3b13d4cfa5fb3ce3db493d82185e6e6f6c9',

	/** webassembly.png, a 4 bit palette with tRNS, at 512x512x4. */
	pngPalette4Bit: 'f08ee05ed726233c6e64e09853e9c27368c797d0e09b95cbd766014db9671c59',

	/** forest.png, RGBA across 119 IDAT chunks, at 2000x831x4. */
	pngMultiIdat: '61bc5df785587293fe3410cccff680e05918f48765e15cd1e5f69ba3d6d9ae64',

	/**
	 * JPEG decodes, every one of which equals what ImageMagick's own libjpeg produces.
	 *
	 * These were checked against `magick <file> -depth 8 RGB:-` byte for byte when they were
	 * recorded, so each is libjpeg's answer rather than merely this decoder's. That holds because
	 * the inverse transform, the chroma filter and the color conversion all follow libjpeg down to
	 * the placement of their rounding terms; a change that breaks one of those breaks a digest here.
	 *
	 * Two of them are shared by two fixtures apiece. A 4:4:4 baseline stream and a progressive one
	 * carry the same coefficients, so only the entropy coding differs and the pixels cannot: that
	 * pins spectral selection, successive approximation and the EOB run. The same holds for a 4:2:0
	 * stream against the same image with a restart marker every four MCUs.
	 */
	jpeg444: '5dd46f262e7dcb3ce1076fb2f763806d6cee5081c9235eb2fb4b917799df30a6',
	jpeg422: 'dd47001edcc8de6a5d02c5b3c82cc21ebeb208021ef562e6f54da52a5ab5262e',
	jpeg420: '7f4dbee36097b069b280156b6c86ae3854e632839fc84135246252c0b5ebcc02',
	jpeg411: 'ea0b86765445ef578757638d3d8556b7b0b82165a5fc880539b7483be3d76ed3',
	jpegCmyk: 'c5b95ee93b2082b764920fd9b720d6d608485ae6bac29df14a77c86e6311e2f5',
	jpegGray: '9d1bb08f10d69403d444e39044a2e2ce27c7b64ddc2791623585adfa60829676',

	/** sf-24.jpg, the reference photograph, at 1835x1032x3. */
	jpegPhoto: '25434511eef0f371785ac5f7caafad54962ea3fe3fd1f8ee7ecb56849cac412d',

	/** road.jpg, the progressive source fixture, at 1281x1920x3. */
	jpegProgressivePhoto: '222e2a11280b7de35717dcc69defd2a65ed5052cdd1da4873b27992eec8407ba',

	/** sf-24.jpg at an eighth, 230x129x3, which is the scale statistics run on. */
	jpegPhotoEighth: 'aa83554e57bb99d631d9fd04ddef0fc58d6de939cdb65007195fdc6a5f30965f',

	/**
	 * GIF decodes, all but the last checked against ImageMagick byte for byte.
	 *
	 * `derived/base.gif` is not listed here because it decodes to {@link bmpRle8}; that it shares a
	 * digest with a BMP is the point.
	 */
	gifTransparent: '2e065a7545341bd7c12c5f8bc48df94869d323098683441127fa828a7cd3e66b',
	gifMono: '8905745faf9323631dae0a282acbfb5c94324e517c757738e81f8e66f13aebde',
	/**
	 * The first of derived/base-animation.gif's three frames, at 160x90x3.
	 *
	 * A codec that reads one frame of a many-frame file has to read the first one, and reading the
	 * last or a composite of all three would still give the right extent. Verified byte exact
	 * against the first frame of `magick -coalesce`, which hands back all three.
	 */
	gifAnimationFirstFrame: 'b96ab3c698e6e0b8b9c4cb88c7087dd3cef5a0ad67a62029fb0ef8e0ccfa4cc3',

	/**
	 * derived/base-offset.gif, a 120x90 frame inside a 160x120 logical screen.
	 *
	 * Asking ImageMagick for the image gives the frame and keeps the screen as page metadata, so it
	 * disagrees on the extent; asking it to coalesce gives the screen with the frame composited at
	 * its offset and the declared background elsewhere, which is what a browser renders and what
	 * this decodes. Verified byte exact against `magick -coalesce` at 160x120x3.
	 */
	gifOffsetFrame: '87abacdb0f94019b8358b985cb29d68aec34037b43c8cd53a2d7b9dbbd44ce6a',

	/**
	 * TIFF decodes, both checked against ImageMagick byte for byte.
	 *
	 * The seven RGB variants are not listed here because they all decode to {@link reference}, and
	 * `derived/base-palette.tif` decodes to {@link bmpRle8}.
	 *
	 * {@link tiffAlpha} is shared with `derived/base-alpha.webp`, so a lossless WebP and a
	 * TIFF have to agree on an image with an alpha ramp as well as on one without.
	 */
	tiffGray: '25597e191f9760ba3d28826034a0c0c493f3cca9c820959760672482addf4dc7',
	tiffAlpha: '007cbcfb4537427e4a7ed898cf2d2bfc2bda7e791e005e62a7cc6ecbab9651fe',

	/**
	 * WebP lossy decodes, every one of which equals what `dwebp` produces.
	 *
	 * These were checked against `dwebp` byte for byte when they were recorded, so each is
	 * libwebp's answer rather than merely this decoder's. Holding that means matching libwebp on
	 * the whole chain: the arithmetic decoder, both transforms, all fourteen prediction modes, both
	 * loop filters, the chroma upsampler's weights and the color conversion's rounding.
	 *
	 * `derived/base-lossless.webp` is not listed, because it decodes to {@link reference}, and
	 * `derived/base-alpha.webp` decodes to {@link tiffAlpha}.
	 */
	webpLossy: '4a68203c52322f492ffd374a1a3b66fe332a201eebf0a8094488cf8e8592aeb6',

	/**
	 * The reference picture with an alpha ramp, at 320x180x4.
	 *
	 * Shared by `derived/base-lossy-alpha.webp` and `derived/base-raw-alpha.webp`, which is the
	 * point of having both: the same lossy frame carries the same alpha plane through the lossless
	 * coder in one and as plain bytes in the other, and the two have to arrive at one answer.
	 */
	webpLossyAlpha: 'deb3240c85eb01fc35a2f46d4ef163ac1f468a38a7898a3c25a6f894f50de7f3',

	/**
	 * The same picture through four encoder settings that each reach different decoder code.
	 *
	 * The two filters are separate algorithms rather than two strengths of one, and the simple one
	 * leaves chroma alone entirely. A non-zero sharpness reduces the filter's interior limit, and
	 * one segment means the header carries no segment map for the macroblock loop to read.
	 */
	webpStrong: '3c388b81446cc4917339ab877f1c5e770d5c735909bebf7a1af1f12a12b4fdf8',
	webpSimple: 'b1c1a9e5adf514ad657fb162bd9a476a74c35ae84d36f04b159884dd831947e2',
	webpSharp: 'eb9f325b614d2f2c69b53842a6e439d46e5abf2c6029be7729fe214036408501',
	webpOneSegment: 'c046134569574ad18211330d0c394d17d54c652d9cec449ad3e9065e3f5bfd6c',

	/**
	 * 65x33, which is odd in both axes.
	 *
	 * Chroma is stored at half resolution rounded up, so an odd width leaves the upsampler a column
	 * with no pair and an odd height leaves it a row; both are their own branch. The lossless
	 * stream is here for the same reason at a different layer: its bundled rows are wider than the
	 * picture and the padding must not reach it.
	 */
	webpOddLossy: '047d2f5819ce2486b69300ff22896b480de6606c3b1c06bc0156743a56e0a31a',
	webpOddLossless: '1765b5b86ac855e5ad2f8ba44cbf02089a5b9ace69640fea49e45cc614411878',

	/**
	 * The first frame of an animation, composited onto the canvas the file declares.
	 *
	 * Its color channels are {@link webpLossy}: the frame is the same lossy encode, reached
	 * through a frame chunk inside an extended container rather than as a bare stream. The alpha is
	 * opaque, since this frame declares none, and the canvas is the one the file gives rather than
	 * the frame's own extents, which the second frame differs from by a row.
	 */
	webpAnimationFirstFrame: 'e254569f82187f28841a7ff6383fc755e9ccf7f9256d44994841f4e3b9228f46',

	/**
	 * The worked chain of the plan, at 300x200x3.
	 *
	 * A 900x600 rectangle of `sf-24.jpg` resampled to 300x200 with a brightness, a contrast, a
	 * saturation and a gamma over it. Six operations reach the output in one pass over 60,000
	 * pixels, having decoded a quarter of a rectangle rather than the whole 1.9 megapixels, so the
	 * digest covers the region arithmetic, the scale propagation, the sample map and the collapse
	 * into one matrix and one table at once.
	 *
	 * Verified by computing it from the native build before recording it, so the value is the
	 * agreement of two compiler targets rather than whatever the wasm module happened to emit.
	 *
	 * Re-recorded when the reduced JPEG transform folded the box average into itself. The chain
	 * takes a scaled decode, so its pixels moved: against the exact area average of the continuous
	 * reconstruction the previous implementation scored 42.6 dB with a worst sample 18 levels out,
	 * and this one scores 64.4 dB and is never more than one level out. The digest changed because
	 * the output got closer to its own definition, not because the definition moved.
	 */
	planWorkedChain: '6111209738889a595f449d702e46d00ad934498685f48afd5c99cecdfdee71e5',

	/**
	 * `sf-24.jpg` turned a quarter clockwise and then mirrored, at 1032x1835x3.
	 *
	 * Two orientations composed into one signed permutation, which is either exactly right or
	 * visibly transposed; there is no way for it to be subtly wrong. Recorded the same way as the
	 * chain above.
	 */
	planTurned: 'c7348406d2fa5491791283aa28a9b4d23e4ab5573bca52fb8bd46aa3692fd151'
} as const;

/**
 * Hashes a buffer with the Web Crypto API, which both node and workerd provide.
 *
 * @param bytes The buffer to hash.
 * @return The digest as lowercase hex.
 */
export async function sha256(bytes: Uint8Array): Promise<string> {
	const digest = await crypto.subtle.digest('SHA-256', bytes as unknown as BufferSource);

	return Array.from(new Uint8Array(digest))
		.map((byte) => byte.toString(16).padStart(2, '0'))
		.join('');
}
