/**
 * Normalises the source fixtures and generates the reference set the differential tests read.
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
 * Needs `magick`, `cwebp`, `avifenc` and `cjpeg` on PATH.
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
 * colour fixtures exist for, so the exclusion is named rather than blanket. Inserted before the output
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
		.filter((name) => /\.(jpg|jpeg|png|webp)$/i.test(name))
		.sort();
}

function dimensions(path: string): { width: number; height: number } {
	const out = execFileSync('magick', ['identify', '-format', '%w %h', path], {
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

/** The reference PNG, plus its alpha and greyscale companions. */
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

	// tiff, one file per compression we read
	for (const [name, compress] of [
		['uncompressed', 'None'],
		['packbits', 'RLE'],
		['lzw', 'LZW'],
		['deflate', 'Zip']
	] as const) {
		magick([base, '-compress', compress, `TIFF:${derivedPath(`base-${name}.tif`)}`]);
	}

	// jpeg, one file per chroma subsampling plus the awkward variants
	for (const [name, sampling] of [
		['444', '4:4:4'],
		['422', '4:2:2'],
		['420', '4:2:0']
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
	run('cwebp', ['-quiet', '-lossless', alpha, '-o', derivedPath('base-alpha.webp')]);

	// avif, read by probe only
	run('avifenc', ['-q', '60', '--speed', '8', base, derivedPath('base.avif')]);
}

/** Per-source references: one scaled, one full-resolution crop. */
function perFixtureReferences() {
	for (const name of sources()) {
		const path = join(FIXTURES, name);
		const stem = name.replace(/\.[^.]+$/, '');
		const { width, height } = dimensions(path);

		magick([
			path,
			'-resize',
			`${REF_LONG}x${REF_LONG}>`,
			'-colorspace',
			'sRGB',
			'-strip',
			`PNG24:${derivedPath('ref', `${stem}.${REF_LONG}.png`)}`
		]);

		// a fixed offset a third of the way in, so the crop lands on content rather than a corner
		const x = Math.floor(width / 3);
		const y = Math.floor(height / 3);
		magick([
			path,
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
						return [
							name,
							{
								x: Math.floor(width / 3),
								y: Math.floor(height / 3),
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

function derived() {
	requireTools('magick', 'cwebp', 'avifenc');
	console.log('generating tests/fixtures/derived');

	mkdirSync(DERIVED, { recursive: true });
	baseImages();
	codecMatrix();
	perFixtureReferences();
	edgeCases();
	colorProfiles();

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
	requireTools('magick', 'cwebp', 'avifenc');

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
		mkdirSync(DERIVED, { recursive: true });
		baseImages();
		codecMatrix();
		perFixtureReferences();
		edgeCases();
		colorProfiles();
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
		const into = join(BLOBS, 'cascades', `${cascade.id}.xml`);
		if (!existsSync(into)) download(cascade.url, into);
	}
	console.log('  cascades: OpenCV LBP frontal and profile (BSD, repacked in Phase 6)');

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

Optional data tinyimg loads at runtime. None of it is required; tinyimg ships no font, no colour
profile and no cascade, and every feature that reads one degrades without it.

Regenerate with \`bun run blobs\`. This directory is gitignored.

## Contents

| Path | What reads it |
| --- | --- |
| \`fonts/*.ttf\` | \`tiny_image_draw_text\`, after \`tiny_blob_load(TINYIMG_BLOB_FONT, ...)\` |
| \`icc/*.icc\` | colour conversion between tagged profiles and sRGB |
| \`cascades/*.xml\` | \`gravity: face\`, \`blur_faces\`, \`pixelate_faces\` |

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
