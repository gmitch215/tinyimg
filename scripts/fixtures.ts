/**
 * Normalizes the source fixtures and generates the reference set the differential tests read.
 *
 * ```sh
 * bun scripts/fixtures.ts                 # everything
 * bun scripts/fixtures.ts --normalize     # resize the oversized sources, in place
 * bun scripts/fixtures.ts --derived       # regenerate tests/fixtures/derived
 * bun scripts/fixtures.ts --blobs         # regenerate blobs/, which is gitignored
 * ```
 *
 * This exists as a script rather than a one-off because the differential tests are only worth
 * anything if their references are reproducible from a committed command. Its output is committed, so
 * CI needs none of the image tooling; re-run it when a fixture is replaced or a format is added.
 *
 * Needs `magick`, `cwebp`, `webpmux`, `avifenc` and `exiftool` on PATH.
 */

import { execFileSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import {
	existsSync,
	mkdirSync,
	mkdtempSync,
	readdirSync,
	readFileSync,
	rmSync,
	statSync,
	writeFileSync
} from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { packCascade } from './cascade.ts';
import { buildBdf, buildPsf1, buildPsf2, cffStub, subsetFont } from './fonts.ts';
import { buildProfile, SPACES } from './icc.ts';

const ROOT = join(import.meta.dirname, '..');
const FIXTURES = join(ROOT, 'tests', 'fixtures');
let DERIVED = join(FIXTURES, 'derived');
const BLOBS = join(ROOT, 'blobs');

/**
 * Longest side any source fixture keeps.
 *
 * 3000 puts every one under 6 Mpx, which leaves room for a full RGBA decode plus a same-size scratch
 * buffer inside the 64 MB linear-memory cap. Three of the sources arrived well over that.
 */
const MAX_LONG_SIDE = 3000;

/**
 * Fixtures {@link normalize} leaves alone, and the extent each one is pinned to.
 *
 * `digicam.jpg` exists to be the largest source the library actually supports, so shrinking it to
 * {@link MAX_LONG_SIDE} would remove the only thing it tests. 3600x2700 is 9.72 Mpx, which is 29.2
 * MB of RGB against the 32 MiB TINYIMG_MAX_IMAGE_BYTES cap and peaks at 50.2 of the 64 MiB heap on
 * a full decode. The next step up, 3840x2880, reaches 98.9% of the cap and would fail on any
 * re-encode that chose a different subsampling.
 */
const PINNED: Record<string, [number, number]> = {
	'digicam.jpg': [3600, 2700]
};

/** The canonical small image every codec decode is compared against. */
const BASE = { source: 'sf-24.jpg', width: 320, height: 180 };

/** Longest side of the per-fixture scaled reference. */
const REF_LONG = 256;

/** Side of the per-fixture full-resolution crop reference. */
const CROP_SIDE = 128;

function run(command: string, args: string[]) {
	execFileSync(command, args, { stdio: ['ignore', 'ignore', 'pipe'] });
}

/**
 * ImageMagick, with PNG timestamping suppressed.
 *
 * The writer stamps `tIME` plus three `date:*` text chunks into every PNG, which made 20 of the 79
 * derived files differ byte-for-byte on an identical re-run; committed output that churns like that
 * grows history on every regeneration. `-strip` would also do it and would take the iCCP chunk the
 * color fixtures exist for, so the exclusion is named rather than blanket. Inserted before the output
 * spec, which has to stay last.
 */
function magick(args: string[]) {
	const output = args[args.length - 1]!;
	run('magick', [...args.slice(0, -1), '-define', 'png:exclude-chunk=date,tIME', output]);
}

function have(command: string): boolean {
	try {
		execFileSync('command', ['-v', command], { stdio: 'ignore', shell: '/bin/bash' });
		return true;
	} catch {
		return false;
	}
}

function requireTools(...names: string[]) {
	const missing = names.filter((name) => !have(name));
	if (missing.length > 0) {
		throw new Error(`missing on PATH: ${missing.join(', ')}`);
	}
}

function sources(): string[] {
	return readdirSync(FIXTURES)
		.filter((name) => /\.(jpg|jpeg|png|webp|gif|tiff)$/i.test(name))
		.sort();
}

/**
 * A source path pinned to its first frame.
 *
 * An animation is many images to ImageMagick, so every read of one has to say which. Without this
 * `identify -format '%w %h'` prints the format once per frame and `-resize` writes an animation
 * where a still reference was wanted.
 */
function frameZero(path: string): string {
	return /\.gif$/i.test(path) ? `${path}[0]` : path;
}

function dimensions(path: string): { width: number; height: number } {
	const out = execFileSync('magick', ['identify', '-format', '%w %h', frameZero(path)], {
		encoding: 'utf8'
	});
	const [width, height] = out.trim().split(' ').map(Number);
	return { width: width!, height: height! };
}

// #region normalize

/**
 * Shrinks any source fixture whose long side exceeds {@link MAX_LONG_SIDE}, in place.
 *
 * `>` on the resize geometry means an already-small fixture is left byte-identical rather than
 * re-encoded, so running this twice is a no-op.
 */
function normalize() {
	requireTools('magick');
	console.log('normalizing source fixtures');

	for (const name of sources()) {
		const path = join(FIXTURES, name);
		const before = dimensions(path);
		const pinned = PINNED[name];

		if (pinned) {
			if (before.width === pinned[0] && before.height === pinned[1]) {
				console.log(`  ${name}: ${before.width}x${before.height}, pinned`);
				continue;
			}

			magick([
				path,
				'-resize',
				`${pinned[0]}x${pinned[1]}!`,
				'-quality',
				'92',
				'-strip',
				path
			]);
			console.log(
				`  ${name}: ${before.width}x${before.height} -> ${pinned[0]}x${pinned[1]}, pinned`
			);
			continue;
		}

		if (Math.max(before.width, before.height) <= MAX_LONG_SIDE) {
			console.log(`  ${name}: ${before.width}x${before.height}, unchanged`);
			continue;
		}

		const bytesBefore = statSync(path).size;
		magick([
			path,
			'-resize',
			`${MAX_LONG_SIDE}x${MAX_LONG_SIDE}>`,
			'-quality',
			'92',
			'-strip',
			path
		]);

		const after = dimensions(path);
		const bytesAfter = statSync(path).size;
		console.log(
			`  ${name}: ${before.width}x${before.height} -> ${after.width}x${after.height}, ` +
				`${(bytesBefore / 1024).toFixed(0)} KiB -> ${(bytesAfter / 1024).toFixed(0)} KiB`
		);
	}
}

// #endregion

// #region derived

function derivedPath(...parts: string[]): string {
	const path = join(DERIVED, ...parts);
	mkdirSync(join(path, '..'), { recursive: true });
	return path;
}

/** The reference PNG, plus its alpha and grayscale companions. */
function baseImages() {
	const source = join(FIXTURES, BASE.source);
	const size = `${BASE.width}x${BASE.height}!`;

	magick([
		source,
		'-resize',
		size,
		'-colorspace',
		'sRGB',
		'-strip',
		`PNG24:${derivedPath('base.png')}`
	]);

	const base = derivedPath('base.png');

	// a soft alpha ramp, so compositing and premultiply tests have partial coverage to work with
	magick([
		base,
		'(',
		'-size',
		`${BASE.width}x${BASE.height}`,
		'gradient:white-black',
		')',
		'-alpha',
		'off',
		'-compose',
		'CopyOpacity',
		'-composite',
		`PNG32:${derivedPath('base-alpha.png')}`
	]);

	magick([base, '-colorspace', 'Gray', '-depth', '8', `PNG:${derivedPath('base-gray.png')}`]);
}

/** Every format we decode, all carrying the same picture as `base.png`. */
function codecMatrix() {
	const base = derivedPath('base.png');
	const alpha = derivedPath('base-alpha.png');

	// png variants
	magick([base, '-depth', '8', `PNG24:${derivedPath('base-rgb8.png')}`]);
	magick([alpha, `PNG32:${derivedPath('base-rgba8.png')}`]);
	magick([base, '-colorspace', 'Gray', '-depth', '8', `PNG:${derivedPath('base-gray8.png')}`]);
	magick([
		alpha,
		'-colorspace',
		'Gray',
		'-depth',
		'8',
		`PNG:${derivedPath('base-gray-alpha8.png')}`
	]);
	magick([base, '-depth', '16', `PNG48:${derivedPath('base-rgb16.png')}`]);
	magick([base, '-colors', '256', `PNG8:${derivedPath('base-palette.png')}`]);
	magick([base, '-interlace', 'PNG', `PNG24:${derivedPath('base-interlaced.png')}`]);

	// bmp
	magick([base, `BMP3:${derivedPath('base.bmp')}`]);
	magick([base, '-colors', '256', '-compress', 'RLE', `BMP3:${derivedPath('base-rle8.bmp')}`]);

	// gif
	magick([base, `GIF:${derivedPath('base.gif')}`]);
	magick([base, '-interlace', 'GIF', `GIF:${derivedPath('base-interlaced.gif')}`]);
	magick([base, '-monochrome', `GIF:${derivedPath('base-mono.gif')}`]);

	// a palette with an entry spent on transparency, which is the only way the
	// format carries alpha
	magick([alpha, '-colors', '128', `GIF:${derivedPath('base-transparent.gif')}`]);

	/*
	 * Three frames, so probe has a frame count to report for a codec that reads
	 * only the first. Rolling the picture sideways gives each frame different
	 * content without a second source, which matters because a frame identical
	 * to the one before it is a frame an optimizer may drop.
	 */
	magick([
		base,
		'-resize',
		'160x90!',
		'(',
		'+clone',
		'-roll',
		'+12+0',
		')',
		'(',
		'+clone',
		'-roll',
		'+12+0',
		')',
		'-set',
		'delay',
		'10',
		'-loop',
		'0',
		`GIF:${derivedPath('base-animation.gif')}`
	]);

	/*
	 * A frame smaller than the logical screen and offset inside it, which is the
	 * case where the two plausible answers for "how big is this image" differ. A
	 * viewer shows the screen, so that is what the decode has to produce.
	 */
	magick([
		base,
		'-resize',
		'120x90!',
		'-repage',
		'160x120+40+30',
		`GIF:${derivedPath('base-offset.gif')}`
	]);

	// tiff, one file per compression we read
	for (const [name, compress] of [
		['uncompressed', 'None'],
		['packbits', 'RLE'],
		['lzw', 'LZW'],
		['deflate', 'Zip']
	] as const) {
		magick([base, '-compress', compress, `TIFF:${derivedPath(`base-${name}.tif`)}`]);
	}

	/*
	 * The rest of the TIFF variants, one per thing the reader has to handle that
	 * a compression alone does not reach: a single channel, four channels, a
	 * color map, the horizontal predictor, a strip height that is not the whole
	 * image, and the other byte order.
	 */
	magick([
		base,
		'-colorspace',
		'Gray',
		'-compress',
		'Zip',
		`TIFF:${derivedPath('base-gray.tif')}`
	]);
	magick([
		alpha,
		'-compress',
		'Zip',
		'-define',
		'tiff:predictor=2',
		`TIFF:${derivedPath('base-alpha.tif')}`
	]);
	magick([
		base,
		'-compress',
		'Zip',
		'-define',
		'tiff:predictor=2',
		`TIFF:${derivedPath('base-predictor.tif')}`
	]);
	magick([base, '-colors', '256', '-compress', 'LZW', `TIFF:${derivedPath('base-palette.tif')}`]);
	magick([
		base,
		'-compress',
		'LZW',
		'-define',
		'tiff:rows-per-strip=7',
		`TIFF:${derivedPath('base-strips.tif')}`
	]);
	magick([
		base,
		'-compress',
		'LZW',
		'-define',
		'tiff:endian=msb',
		`TIFF:${derivedPath('base-msb.tif')}`
	]);

	// jpeg, one file per chroma subsampling plus the awkward variants
	for (const [name, sampling] of [
		['444', '4:4:4'],
		['422', '4:2:2'],
		['420', '4:2:0'],
		['411', '4:1:1']
	] as const) {
		magick([
			base,
			'-sampling-factor',
			sampling,
			'-quality',
			'92',
			'-strip',
			`JPEG:${derivedPath(`base-${name}.jpg`)}`
		]);
	}
	magick([
		base,
		'-interlace',
		'JPEG',
		'-quality',
		'92',
		'-strip',
		`JPEG:${derivedPath('base-progressive.jpg')}`
	]);

	// restart markers every four MCU rows, which a decoder has to resynchronize
	// on rather than read through
	magick([
		base,
		'-sampling-factor',
		'4:2:0',
		'-quality',
		'92',
		'-define',
		'jpeg:restart-interval=4',
		`JPEG:${derivedPath('base-restart.jpg')}`
	]);

	/*
	 * Every source fixture was stripped when it was normalized, so the only way
	 * to have EXIF to read is to write some. The tags are fixed rather than taken
	 * from the clock, since a fixture whose bytes change by the day is not a
	 * fixture.
	 */
	for (const [name, orientation] of [
		['base-exif.jpg', 1],
		['base-exif-rotated.jpg', 6]
	] as const) {
		const path = derivedPath(name);

		magick([base, '-sampling-factor', '4:4:4', '-quality', '92', `JPEG:${path}`]);
		run('exiftool', [
			'-overwrite_original',
			'-q',
			'-Make=tinyimg',
			'-Model=fixture generator',
			'-Software=tinyimg fixtures.ts',
			'-Artist=Gregory Mitchell',
			'-DateTimeOriginal=2026:09:02 12:00:00',
			`-Orientation#=${orientation}`,
			path
		]);
	}
	magick([
		base,
		'-colorspace',
		'Gray',
		'-quality',
		'92',
		'-strip',
		`JPEG:${derivedPath('base-gray.jpg')}`
	]);
	magick([
		base,
		'-colorspace',
		'CMYK',
		'-quality',
		'92',
		'-strip',
		`JPEG:${derivedPath('base-cmyk.jpg')}`
	]);

	// webp
	run('cwebp', ['-quiet', '-q', '80', base, '-o', derivedPath('base-lossy.webp')]);
	run('cwebp', ['-quiet', '-lossless', base, '-o', derivedPath('base-lossless.webp')]);

	/*
	 * `-exact` or the color of a fully transparent pixel is rewritten, and
	 * `magick compare` cannot see that because it composites before it measures.
	 * Without it this file looks equal to its source and is not, which breaks the
	 * assertion that a lossless decode is byte identical to the PNG of the same
	 * picture.
	 */
	run('cwebp', ['-quiet', '-lossless', '-exact', alpha, '-o', derivedPath('base-alpha.webp')]);

	/*
	 * One file per branch of the lossy decoder that a plain encode does not
	 * reach: the normal loop filter at a strength that engages it, the simple
	 * filter, a filter sharpness that changes its inner limit, a single
	 * segment, and the three ways alpha can arrive.
	 */
	for (const [name, flags] of [
		['base-lossy-alpha', []],
		['base-strong', ['-f', '40', '-strong']],
		['base-simple', ['-nostrong']],
		['base-sharp', ['-sharpness', '4']],
		['base-onesegment', ['-segments', '1']],
		['base-filtered-alpha', ['-alpha_filter', 'best']],
		['base-raw-alpha', ['-alpha_method', '0']]
	] as const) {
		const source = name.includes('alpha') ? alpha : base;

		run('cwebp', ['-quiet', '-q', '80', ...flags, source, '-o', derivedPath(`${name}.webp`)]);
	}

	/*
	 * Two frames in an extended container, the second a row shorter than the
	 * canvas. Reading the first frame's own extents rather than the canvas is the
	 * mistake this exists to catch, and a second frame that matches the canvas
	 * could not catch it.
	 *
	 * Only the second frame carries alpha, so the container declares it and the
	 * first frame does not have it. A decoder that takes the flag from the
	 * container has to synthesize an opaque channel for that frame rather than
	 * looking for a chunk that is not there.
	 */
	const shorter = derivedPath('base-short.png');

	magick([alpha, '-crop', '320x179+0+0', '+repage', `PNG32:${shorter}`]);
	run('cwebp', ['-quiet', '-q', '80', shorter, '-o', `${shorter}.webp`]);
	run('webpmux', [
		'-frame',
		derivedPath('base-lossy.webp'),
		'+100',
		'-frame',
		`${shorter}.webp`,
		'+100',
		'-loop',
		'0',
		'-o',
		derivedPath('base-animation.webp')
	]);

	// the two intermediates exist only to be muxed together
	rmSync(shorter);
	rmSync(`${shorter}.webp`);

	// avif, read by probe only. the second carries an irot the probe reports and
	// deliberately does not apply
	run('avifenc', ['-q', '60', '--speed', '8', base, derivedPath('base.avif')]);
	run('avifenc', ['-q', '60', '--speed', '8', alpha, derivedPath('base-alpha.avif')]);
	run('avifenc', [
		'-q',
		'60',
		'--speed',
		'8',
		'--irot',
		'1',
		base,
		derivedPath('base-rotated.avif')
	]);
}

/**
 * Where the full-resolution crop is taken from.
 *
 * A third of the way in, so it lands on content rather than a corner, pulled back far enough that
 * {@link CROP_SIDE} still fits. `dartmouth.jpg` is 250x187 and is the first fixture small enough for
 * the plain third to overrun the bottom edge, which produced a 128x125 reference against a manifest
 * promising 128x128.
 */
function cropOrigin(width: number, height: number): { x: number; y: number } {
	return {
		x: Math.max(0, Math.min(Math.floor(width / 3), width - CROP_SIDE)),
		y: Math.max(0, Math.min(Math.floor(height / 3), height - CROP_SIDE))
	};
}

/** Per-source references: one scaled, one full-resolution crop. */
function perFixtureReferences() {
	for (const name of sources()) {
		const path = join(FIXTURES, name);
		const stem = name.replace(/\.[^.]+$/, '');
		const { width, height } = dimensions(path);

		magick([
			frameZero(path),
			'-resize',
			`${REF_LONG}x${REF_LONG}>`,
			'-colorspace',
			'sRGB',
			'-strip',
			`PNG24:${derivedPath('ref', `${stem}.${REF_LONG}.png`)}`
		]);

		const { x, y } = cropOrigin(width, height);
		magick([
			frameZero(path),
			'-crop',
			`${CROP_SIDE}x${CROP_SIDE}+${x}+${y}`,
			'+repage',
			'-colorspace',
			'sRGB',
			'-strip',
			`PNG24:${derivedPath('ref', `${stem}.crop.png`)}`
		]);
	}

	writeFileSync(
		derivedPath('ref', 'crops.json'),
		`${JSON.stringify(
			{
				side: CROP_SIDE,
				scaledLongSide: REF_LONG,
				crops: Object.fromEntries(
					sources().map((name) => {
						const { width, height } = dimensions(join(FIXTURES, name));
						const { x, y } = cropOrigin(width, height);
						return [
							name,
							{
								x,
								y,
								width: CROP_SIDE,
								height: CROP_SIDE,
								sourceWidth: width,
								sourceHeight: height
							}
						];
					})
				)
			},
			null,
			2
		)}\n`
	);
}

/** Awkward inputs: odd dimensions, oversized, degenerate, and deliberately broken. */
function edgeCases() {
	const source = join(FIXTURES, BASE.source);

	// odd dimensions in both axes, which is where chroma subsampling goes wrong
	magick([source, '-resize', '65x33!', '-strip', `PNG24:${derivedPath('tiny-odd.png')}`]);
	magick([
		derivedPath('tiny-odd.png'),
		'-sampling-factor',
		'4:2:0',
		'-quality',
		'92',
		'-strip',
		`JPEG:${derivedPath('tiny-odd.jpg')}`
	]);

	// the same odd extents through WebP, where a bundled lossless row is narrower
	// than the picture and the last macroblock column is partly padding
	run('cwebp', [
		'-quiet',
		'-q',
		'80',
		derivedPath('tiny-odd.png'),
		'-o',
		derivedPath('tiny-odd-lossy.webp')
	]);
	run('cwebp', [
		'-quiet',
		'-lossless',
		derivedPath('tiny-odd.png'),
		'-o',
		derivedPath('tiny-odd-lossless.webp')
	]);

	// 20 Mpx, over any sane pixel budget, but a gradient so the file stays small
	magick([
		'-size',
		'5000x4000',
		'gradient:red-blue',
		'-quality',
		'60',
		`JPEG:${derivedPath('oversized.jpg')}`
	]);

	magick(['-size', '64x64', 'xc:#3366cc', `PNG24:${derivedPath('flat.png')}`]);
	magick(['-size', '1x1', 'xc:#ff0000', `PNG24:${derivedPath('single-pixel.png')}`]);

	// a uniform border for trim, and an alpha logo for the overlay tests
	magick([
		derivedPath('base.png'),
		'-bordercolor',
		'#123456',
		'-border',
		'24',
		`PNG24:${derivedPath('trim.png')}`
	]);
	magick([
		'-size',
		'96x96',
		'xc:none',
		'-fill',
		'#e0552b',
		'-draw',
		'circle 48,48 48,6',
		'-fill',
		'#ffffff',
		'-draw',
		'circle 48,48 48,26',
		`PNG32:${derivedPath('logo.png')}`
	]);

	// a subject on flat surroundings, which is what background removal is asked to find
	magick([
		'-size',
		'200x200',
		'xc:#f2f2f2',
		'-fill',
		'#2c6e49',
		'-draw',
		'circle 100,100 100,40',
		`PNG24:${derivedPath('subject.png')}`
	]);

	malformed();
}

/** Broken inputs, built by byte surgery so no tool is needed and the corruption is exact. */
function malformed() {
	const jpeg = readFileSync(derivedPath('base-420.jpg'));
	const png = readFileSync(derivedPath('base.png'));

	const write = (name: string, bytes: Buffer) =>
		writeFileSync(derivedPath('malformed', name), bytes);

	write('truncated.jpg', jpeg.subarray(0, Math.floor(jpeg.length / 2)));
	write('truncated.png', png.subarray(0, Math.floor(png.length / 2)));
	write('header-only.jpg', jpeg.subarray(0, 4));
	write('empty.jpg', Buffer.alloc(0));

	// a valid signature over garbage, which is the case a sniff-only guard lets through
	const fakeJpeg = Buffer.concat([jpeg.subarray(0, 2), Buffer.alloc(256, 0x5a)]);
	write('signature-only.jpg', fakeJpeg);

	const fakePng = Buffer.concat([png.subarray(0, 8), Buffer.alloc(256, 0x5a)]);
	write('signature-only.png', fakePng);

	// a png whose IHDR claims a size its data cannot fill
	const badIhdr = Buffer.from(png);
	badIhdr.writeUInt32BE(0x7fffffff, 16);
	write('absurd-dimensions.png', badIhdr);

	write('not-an-image.bin', Buffer.from('this is not an image at all', 'utf8'));
}

/** Profiles written beside the fixtures, and images tagged with them. */
function colorProfiles() {
	const base = derivedPath('base.png');

	for (const space of SPACES) {
		const profile = buildProfile(space);
		writeFileSync(derivedPath('icc', `${space.id}.icc`), profile);
	}

	const srgb = derivedPath('icc', 'srgb.icc');

	for (const id of ['display-p3', 'adobe-rgb-1998', 'rec2020']) {
		const profile = derivedPath('icc', `${id}.icc`);

		// tagged but not converted, so the pixels are unchanged and only the profile differs
		magick([base, '-profile', profile, `PNG24:${derivedPath(`base-${id}.png`)}`]);

		// magick's own conversion back to sRGB, which is what our parser is measured against
		magick([
			derivedPath(`base-${id}.png`),
			'-profile',
			srgb,
			`PNG24:${derivedPath('ref', `base-${id}-to-srgb.png`)}`
		]);
	}
}

/**
 * ImageMagick's own answer for the adjustments where the two operations are the same one.
 *
 * Only where they genuinely are. `-gamma` and `-evaluate multiply` are the same functions we
 * apply, and `-function polynomial` is the same affine our contrast is, so those three are
 * like-for-like and a disagreement is a fault. `-blur` is a true gaussian against our three box
 * passes, which is an approximation with a floor rather than an equality.
 *
 * Saturation is deliberately absent. `-modulate` operates in HSL and is a different operation, and
 * `-color-matrix` given our own matrix would compare our arithmetic against itself. Neither is a
 * measurement, so there is no reference for it and the ctest asserts the matrix directly instead.
 */
function filterReferences() {
	const base = derivedPath('base.png');

	// gamma 2.2: magick's -gamma applies the reciprocal exponent, so this is 1/2.2
	magick([base, '-gamma', String(1 / 2.2), `PNG24:${derivedPath('ref', 'base-gamma.png')}`]);

	magick([
		base,
		'-evaluate',
		'multiply',
		'1.25',
		`PNG24:${derivedPath('ref', 'base-brightness.png')}`
	]);

	// contrast 1.4 about mid gray is 1.4u - 0.2 on a normalized channel
	magick([
		base,
		'-function',
		'polynomial',
		'1.4,-0.2',
		`PNG24:${derivedPath('ref', 'base-contrast.png')}`
	]);

	magick([base, '-blur', '0x4', `PNG24:${derivedPath('ref', 'base-blur.png')}`]);

	magick([
		base,
		'-filter',
		'Catrom',
		'-resize',
		'640x360!',
		`PNG24:${derivedPath('ref', 'base-catrom.png')}`
	]);

	magick([
		base,
		'-filter',
		'Box',
		'-resize',
		'80x45!',
		`PNG24:${derivedPath('ref', 'base-box.png')}`
	]);
}

/**
 * Glyphs the text differential compares, one per outline shape worth distinguishing.
 *
 * Straight stems, a closed curve, a curve with a counter and a crossbar, diagonals, a descender and
 * an S-curve. `x` is the baseline probe: it sits entirely between the baseline and the x-height, so
 * a vertical placement error moves all of its ink.
 */
const TEXT_GLYPHS = ['H', 'o', 'e', 'W', 'g', 'S', 'x'];

/**
 * Em size the per-glyph comparison runs at.
 *
 * Large on purpose. ImageMagick renders through FreeType with hinting, which moves stems onto pixel
 * boundaries and rounds advance widths to whole pixels; tinyimg renders the outline where the
 * outline is. The shape difference that causes is a fixed number of pixels, so it shrinks against
 * the glyph as the glyph grows: one `H` agrees at 29 dB at 32 pixels and 41 at 256.
 */
const TEXT_SIZE = 256;

/** Box each glyph is rendered into, and where its baseline sits in that box. */
const TEXT_BOX = { width: 320, height: 384, baseline: 300 };

/** What the string comparison is set at, which is a size a caller would actually use. */
const TEXT_STRING = { text: 'Hamburgefons', size: 32, width: 288, height: 64, baseline: 46 };

/**
 * ImageMagick's own render of the committed subset, for the text differential.
 *
 * Grayscale on black, so the coverage is the pixel value and nothing has to be un-composited before
 * the two can be compared.
 */
function textReferences() {
	const font = derivedPath('fonts', 'dejavu-latin.ttf');

	for (const glyph of TEXT_GLYPHS) {
		magick([
			'-size',
			`${TEXT_BOX.width}x${TEXT_BOX.height}`,
			'xc:black',
			'-font',
			font,
			'-pointsize',
			String(TEXT_SIZE),
			'-fill',
			'white',
			'-annotate',
			`+6+${TEXT_BOX.baseline}`,
			glyph,
			`PNG:${derivedPath('ref', `text-${glyph.charCodeAt(0).toString(16)}.png`)}`
		]);
	}

	magick([
		'-size',
		`${TEXT_STRING.width}x${TEXT_STRING.height}`,
		'xc:black',
		'-font',
		font,
		'-pointsize',
		String(TEXT_STRING.size),
		'-fill',
		'white',
		'-annotate',
		`+6+${TEXT_STRING.baseline}`,
		TEXT_STRING.text,
		`PNG:${derivedPath('ref', 'text-string.png')}`
	]);
}

/** Codepoints the committed font subset covers. */
const SUBSET_ASCII = Array.from({ length: 0x7e - 0x20 + 1 }, (_, index) => 0x20 + index);

/**
 * Latin-1 letters that are composite glyphs.
 *
 * Every one of these is a base letter plus a combining accent, which is the case a reader that only
 * handles simple glyphs renders as a bare letter with no accent and no error.
 */
const SUBSET_ACCENTS = [0xc0, 0xc5, 0xc9, 0xe0, 0xe9, 0xef, 0xf1, 0xfc];

/**
 * A codepoint past the BMP, mapped onto the glyph `A` uses.
 *
 * Only in the format 12 subset, because format 4 cannot express it. It is what makes the two
 * subsets differ in a way a test can see: the same string draws through one and not the other.
 */
const SUBSET_ASTRAL = 0x1f600;

/** Codepoints the synthesized BDF face covers. */
const BDF_CODEPOINTS = [0x20, 0x2e, 0x41, 0x42, 0x43, 0xe9];

/**
 * The font fixtures, committed because `blobs/` is not.
 *
 * A test that only read `blobs/fonts` would not run in CI, and one that skipped when the blob was
 * absent would not gate. The real face is cut down by `scripts/fonts.ts` from 757 KB to 21, and the
 * three bitmap faces are synthesized so the expected pixels are known rather than measured.
 */
function fontFixtures() {
	const source = join(BLOBS, 'fonts', 'DejaVuSans.ttf');

	if (!existsSync(source)) {
		throw new Error(`${source} is missing; run \`bun run blobs\` first`);
	}

	const dejavu = readFileSync(source);
	const wanted = [...SUBSET_ASCII, ...SUBSET_ACCENTS];

	const four = subsetFont(dejavu, wanted, 4);
	writeFileSync(derivedPath('fonts', 'dejavu-latin.ttf'), four.bytes);

	const twelve = subsetFont(dejavu, wanted, 12, { aliases: { [SUBSET_ASTRAL]: 0x41 } });
	writeFileSync(derivedPath('fonts', 'dejavu-latin-cmap12.ttf'), twelve.bytes);

	// format 6 under the Macintosh platform, and the short loca form, both of which the reader
	// claims to handle and neither of which the two variants above reach
	const six = subsetFont(dejavu, wanted, 6, { shortLoca: true });
	writeFileSync(derivedPath('fonts', 'dejavu-latin-cmap6.ttf'), six.bytes);

	// out of specification, and recoverable: the em box is a usable line and every glyph shares
	// the first advance
	const headless = subsetFont(dejavu, wanted, 4, { dropHhea: true });
	writeFileSync(derivedPath('fonts', 'dejavu-latin-no-hhea.ttf'), headless.bytes);

	console.log(
		`  fonts: ${four.summary.glyphs} glyphs, ${four.summary.codepoints} codepoints, ` +
			`${(four.summary.bytes / 1024).toFixed(1)} KiB from ${(dejavu.length / 1024).toFixed(0)}; ` +
			`cmap6 reaches ${six.summary.codepoints}`
	);

	writeFileSync(derivedPath('fonts', 'tiny.psf'), buildPsf2(8, 16, 128));
	writeFileSync(derivedPath('fonts', 'tiny-psf1.psf'), buildPsf1(8));
	writeFileSync(derivedPath('fonts', 'tiny.bdf'), buildBdf(8, 8, BDF_CODEPOINTS));

	// an OpenType wrapper around CFF outlines, which this library refuses rather than parses
	writeFileSync(derivedPath('fonts', 'cff.otf'), cffStub());

	writeFileSync(
		derivedPath('fonts', 'not-a-font.bin'),
		Buffer.from('this is not a font at all, not even a little', 'utf8')
	);

	// a truncated real face: the directory promises tables the file does not hold
	writeFileSync(
		derivedPath('fonts', 'truncated.ttf'),
		four.bytes.subarray(0, Math.floor(four.bytes.length / 3))
	);

	// BDF faces missing a header the reader needs, one for each of the two it checks
	const bdf = buildBdf(8, 8, BDF_CODEPOINTS);

	writeFileSync(
		derivedPath('fonts', 'bdf-no-chars.bdf'),
		bdf.replace(/^CHARS .*$/m, 'COMMENT no chars line')
	);
	writeFileSync(
		derivedPath('fonts', 'bdf-no-bbox.bdf'),
		bdf.replace(/^FONTBOUNDINGBOX .*$/m, 'COMMENT no bounding box')
	);

	// a negative descent, which is the one place the BDF reader reads a signed number
	writeFileSync(
		derivedPath('fonts', 'bdf-descender.bdf'),
		bdf.replace('FONTBOUNDINGBOX 8 8 0 0', 'FONTBOUNDINGBOX 8 10 0 -2')
	);
}

/** The cascades, repacked from the XML the blob step downloads. */
function cascadeFixtures() {
	for (const cascade of CASCADES) {
		const source = join(BLOBS, 'cascades', `${cascade.id}.xml`);

		// the blobs step packs the same download and then removes it, so requiring it to still be
		// there made this depend on which of the two ran last
		const downloaded = !existsSync(source);
		if (downloaded) {
			mkdirSync(join(BLOBS, 'cascades'), { recursive: true });
			download(cascade.url, source);
		}

		const { bytes, summary } = packCascade(readFileSync(source, 'utf8'));
		writeFileSync(derivedPath('cascades', `${cascade.id}.bin`), bytes);

		if (downloaded) rmSync(source);

		console.log(
			`  ${cascade.id}: ${summary.window} window, ${summary.stages} stages, ` +
				`${summary.stumps} stumps, ${summary.features} features, ${summary.bytes} bytes`
		);
	}

	// a header whose magic is right and whose counts are not, for the rejection path
	const good = readFileSync(derivedPath('cascades', `${CASCADES[0]!.id}.bin`));
	const bad = Buffer.from(good.subarray(0, 28));
	bad.writeUInt32LE(9999, 16);
	writeFileSync(derivedPath('cascades', 'malformed.bin'), bad);
}

/**
 * Everything the derived set is made of, in one list.
 *
 * One list because {@link derived} and {@link check} both run it, and when they each had their own
 * the two drifted: a generator added to one and not the other made `--check` report its own output
 * as missing.
 */
function generate() {
	mkdirSync(DERIVED, { recursive: true });
	baseImages();
	codecMatrix();
	perFixtureReferences();
	edgeCases();
	colorProfiles();
	filterReferences();
	fontFixtures();
	textReferences();
	cascadeFixtures();
}

function derived() {
	requireTools('magick', 'cwebp', 'webpmux', 'avifenc', 'exiftool');
	console.log('generating tests/fixtures/derived');

	generate();

	const count = countFiles(DERIVED);
	console.log(`  ${count.files} files, ${(count.bytes / 1024 / 1024).toFixed(1)} MiB`);
}

/**
 * Regenerates the derived set beside the committed one and reports any file that differs.
 *
 * The committed output is the test oracle: `tests/support/golden.ts` pins the digest of decoding
 * `derived/base.bmp`, and every codec test measures itself against these files. Committing them is
 * what stops CI's ImageMagick version from moving the references, and this is what makes that
 * commitment checkable rather than assumed. Needs the same tools the generator does, so it runs as
 * its own job and not on the gate.
 */
function check() {
	requireTools('magick', 'cwebp', 'webpmux', 'avifenc', 'exiftool');

	// the versions are the first thing to look at when this reports differences: the committed set
	// is only reproducible with the tools that wrote it, which is why this is a local check and not
	// a CI job
	for (const [tool, args] of [
		['magick', ['-version']],
		['cwebp', ['-version']],
		['avifenc', ['--version']]
	] as [string, string[]][]) {
		const output = execFileSync(tool, args, { encoding: 'utf8' }).split('\n')[0] ?? '';
		console.log(`  ${tool}: ${output.trim()}`);
	}

	const committed = DERIVED;
	const scratch = mkdtempSync(join(tmpdir(), 'tinyimg-fixtures-'));

	console.log(`regenerating into ${scratch}`);

	DERIVED = scratch;
	try {
		generate();
	} finally {
		DERIVED = committed;
	}

	const before = listFiles(committed);
	const after = listFiles(scratch);

	const missing = before.filter((name) => !after.includes(name));
	const extra = after.filter((name) => !before.includes(name));
	const differing = before
		.filter((name) => after.includes(name))
		.filter((name) => digest(join(committed, name)) !== digest(join(scratch, name)));

	for (const name of missing) console.log(`  only committed: ${name}`);
	for (const name of extra) console.log(`  only regenerated: ${name}`);
	for (const name of differing) console.log(`  differs: ${name}`);

	rmSync(scratch, { recursive: true, force: true });

	const wrong = missing.length + extra.length + differing.length;
	if (wrong > 0) {
		console.error(
			`${wrong} of ${before.length} files do not match a fresh run; commit the regenerated set or fix the generator`
		);
		process.exitCode = 1;
		return;
	}

	console.log(`  ${before.length} files match a fresh run`);
}

function digest(path: string): string {
	return createHash('sha256').update(readFileSync(path)).digest('hex');
}

function listFiles(dir: string, prefix = ''): string[] {
	const names: string[] = [];

	for (const entry of readdirSync(dir, { withFileTypes: true })) {
		const name = prefix ? `${prefix}/${entry.name}` : entry.name;

		if (entry.isDirectory()) names.push(...listFiles(join(dir, entry.name), name));
		else names.push(name);
	}

	return names.sort();
}

function countFiles(dir: string): { files: number; bytes: number } {
	let files = 0;
	let bytes = 0;
	for (const entry of readdirSync(dir, { withFileTypes: true })) {
		const full = join(dir, entry.name);
		if (entry.isDirectory()) {
			const inner = countFiles(full);
			files += inner.files;
			bytes += inner.bytes;
		} else {
			files++;
			bytes += statSync(full).size;
		}
	}
	return { files, bytes };
}

// #endregion

// #region blobs

const DEJAVU =
	'https://github.com/dejavu-fonts/dejavu-fonts/releases/download/version_2_37/dejavu-fonts-ttf-2.37.zip';

const CASCADES = [
	{
		id: 'lbp-frontalface',
		url: 'https://raw.githubusercontent.com/opencv/opencv/4.x/data/lbpcascades/lbpcascade_frontalface_improved.xml'
	},
	{
		id: 'lbp-profileface',
		url: 'https://raw.githubusercontent.com/opencv/opencv/4.x/data/lbpcascades/lbpcascade_profileface.xml'
	}
];

function download(url: string, into: string) {
	run('curl', ['-fsSL', '-o', into, url]);
}

/**
 * Generates the optional data set.
 *
 * Gitignored: it is uploaded to R2 or Workers Assets by hand, and nothing in tinyimg requires it.
 * The cascades land as the OpenCV XML they are downloaded as; repacking them to the flat binary the
 * detector reads needs the detector, so that happens in Phase 6.
 */
function blobs() {
	requireTools('curl', 'unzip');
	console.log('generating blobs');

	mkdirSync(join(BLOBS, 'fonts'), { recursive: true });
	mkdirSync(join(BLOBS, 'icc'), { recursive: true });
	mkdirSync(join(BLOBS, 'cascades'), { recursive: true });

	for (const space of SPACES) {
		writeFileSync(join(BLOBS, 'icc', `${space.id}.icc`), buildProfile(space));
	}
	console.log(`  icc: ${SPACES.length} profiles, generated`);

	const zip = join(BLOBS, 'dejavu.zip');
	if (!existsSync(join(BLOBS, 'fonts', 'DejaVuSans.ttf'))) {
		download(DEJAVU, zip);
		run('unzip', [
			'-o',
			'-j',
			zip,
			'*/ttf/DejaVuSans.ttf',
			'*/ttf/DejaVuSansMono.ttf',
			'*/LICENSE',
			'-d',
			join(BLOBS, 'fonts')
		]);
		run('rm', ['-f', zip]);
	}
	console.log('  fonts: DejaVuSans, DejaVuSansMono (Bitstream Vera license, redistributable)');

	for (const cascade of CASCADES) {
		const xml = join(BLOBS, 'cascades', `${cascade.id}.xml`);
		if (!existsSync(xml)) download(cascade.url, xml);

		const { bytes, summary } = packCascade(readFileSync(xml, 'utf8'));
		writeFileSync(join(BLOBS, 'cascades', `${cascade.id}.bin`), bytes);

		// the XML was the download, not the deliverable; the detector reads the packed form and
		// leaving both would upload 50 KB nothing reads
		rmSync(xml);

		console.log(
			`  ${cascade.id}: ${summary.window} window, ${summary.stages} stages, ${summary.bytes} bytes`
		);
	}

	manifest();
	blobReadme();
}

function manifest() {
	const entries: Record<string, { path: string; bytes: number; sha256: string; type: string }> =
		{};

	const walk = (dir: string, prefix: string) => {
		for (const entry of readdirSync(dir, { withFileTypes: true }).sort((a, b) =>
			a.name.localeCompare(b.name)
		)) {
			const full = join(dir, entry.name);
			const path = prefix ? `${prefix}/${entry.name}` : entry.name;
			if (entry.isDirectory()) {
				walk(full, path);
				continue;
			}
			if (entry.name === 'blobs.json' || entry.name === 'README.md') continue;

			const bytes = readFileSync(full);
			entries[path.replace(/\.[^.]+$/, '').replace(/\//g, ':')] = {
				path,
				bytes: bytes.length,
				sha256: createHash('sha256').update(bytes).digest('hex'),
				type: contentType(entry.name)
			};
		}
	};

	walk(BLOBS, '');
	writeFileSync(join(BLOBS, 'blobs.json'), `${JSON.stringify({ blobs: entries }, null, 2)}\n`);
	console.log(`  manifest: ${Object.keys(entries).length} entries`);
}

function contentType(name: string): string {
	if (name.endsWith('.ttf')) return 'font/ttf';
	if (name.endsWith('.otf')) return 'font/otf';
	if (name.endsWith('.icc')) return 'application/vnd.iccprofile';
	if (name.endsWith('.xml')) return 'application/xml';
	if (name.endsWith('.bin')) return 'application/octet-stream';
	return 'text/plain';
}

function blobReadme() {
	writeFileSync(
		join(BLOBS, 'README.md'),
		`# tinyimg blobs

Optional data tinyimg loads at runtime. None of it is required; tinyimg ships no font, no color
profile and no cascade, and every feature that reads one degrades without it.

Regenerate with \`bun run blobs\`. This directory is gitignored.

## Contents

| Path | What reads it |
| --- | --- |
| \`fonts/*.ttf\` | \`tiny_image_draw_text\`, after \`tiny_blob_load(TINYIMG_BLOB_FONT, ...)\` |
| \`icc/*.icc\` | color conversion between tagged profiles and sRGB |
| \`cascades/*.bin\` | \`gravity: face\`, \`blur_faces\`, \`pixelate_faces\` |

\`fonts/LICENSE\` is the Bitstream Vera and Arev license the DejaVu faces ship under. The cascades are
OpenCV's, BSD licensed. The profiles are generated by \`scripts/icc.ts\` from published primaries and
transfer functions, so they carry no third-party terms.

## Upload

\`\`\`sh
wrangler r2 object put tinyimg-blobs/fonts/DejaVuSans.ttf --file blobs/fonts/DejaVuSans.ttf
\`\`\`

## Bind it

\`\`\`jsonc
{
	"r2_buckets": [{ "binding": "BLOBS", "bucket_name": "tinyimg-blobs" }]
}
\`\`\`

\`\`\`ts
import { Image, loadBlob } from '@gmitch215/tinyimg';

const font = await env.BLOBS.get('fonts/DejaVuSans.ttf');
if (font) await loadBlob('font', 'dejavu-sans', await font.arrayBuffer());
\`\`\`

Static Assets work the same way, with \`env.ASSETS.fetch()\` in place of the bucket read. An import as
a wrangler \`Data\` module also works and costs no subrequest, at the price of bundle bytes.
`
	);
}

// #endregion

if (import.meta.main) {
	const args = process.argv.slice(2);
	const all = args.length === 0;

	try {
		if (args.includes('--check')) {
			check();
		} else {
			if (all || args.includes('--normalize')) normalize();
			if (all || args.includes('--blobs')) blobs();
			if (all || args.includes('--derived')) derived();
		}
	} catch (error) {
		console.error(error instanceof Error ? error.message : error);
		process.exitCode = 1;
	}
}
