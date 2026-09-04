/**
 * Repacks an OpenCV LBP cascade from XML into the flat binary `src/detect.c` reads.
 *
 * The parse happens here rather than in the module because an XML reader in wasm would cost more
 * bytes than the cascade does, and because a cascade is data a caller loads rather than something
 * tinyimg ships. Both shipped cascades come out around 7 KB against 50 KB of XML.
 *
 * The layout, little-endian throughout:
 *
 * | Offset | Type | What |
 * | --- | --- | --- |
 * | 0 | `char[4]` | `TICA` |
 * | 4 | `u32` | format version, 1 |
 * | 8 | `u32` | window width |
 * | 12 | `u32` | window height |
 * | 16 | `u32` | stages |
 * | 20 | `u32` | stumps |
 * | 24 | `u32` | features |
 * | 28 | `f32[stages]` | stage threshold |
 * | .. | `u32[stages]` | first stump of the stage |
 * | .. | `u32[stages]` | stumps in the stage |
 * | .. | `u32[stumps]` | feature the stump reads |
 * | .. | `u32[stumps * 8]` | the 256-bit subset |
 * | .. | `f32[stumps * 2]` | the two leaves |
 * | .. | `u8[features * 4]` | feature rect, x y w h in window coordinates |
 *
 * Every classifier in both cascades is a stump, so the format has no tree and {@link packCascade}
 * refuses a cascade that needs one rather than shipping a format the detector cannot walk.
 */

/** What a repack produced, for the caller to report. */
export interface CascadeSummary {
	window: string;
	stages: number;
	stumps: number;
	features: number;
	bytes: number;
}

const MAGIC = 0x54494341;
const HEADER = 28;
const SUBSET_WORDS = 8;

function one(xml: string, tag: string): string {
	const match = new RegExp(`<${tag}>([\\s\\S]*?)</${tag}>`).exec(xml);
	if (!match) throw new Error(`cascade has no <${tag}>`);
	return match[1]!.trim();
}

function numbers(text: string): number[] {
	const parts = text.trim().split(/\s+/).filter(Boolean);
	return parts.map((part) => {
		const value = Number(part);
		if (!Number.isFinite(value)) throw new Error(`cascade holds a non-number: ${part}`);
		return value;
	});
}

/**
 * Splits the direct `<_>` children of a container, which is how OpenCV writes a sequence.
 *
 * Counted rather than regexed because the elements nest: a stage holds weak classifiers that hold
 * `<_>` of their own, and a non-greedy match would stop at the first inner one.
 */
function items(container: string): string[] {
	const out: string[] = [];
	let depth = 0;
	let start = 0;

	for (const match of container.matchAll(/<(\/?)_>/g)) {
		const closing = match[1] === '/';

		if (!closing) {
			if (depth === 0) start = match.index + match[0].length;
			depth++;
			continue;
		}

		depth--;
		if (depth === 0) out.push(container.slice(start, match.index));
	}

	if (depth !== 0) throw new Error('cascade has unbalanced <_> elements');
	return out;
}

/**
 * Repacks one cascade.
 *
 * @param xml The OpenCV cascade, as text.
 * @returns The packed bytes and what went into them.
 */
export function packCascade(xml: string): { bytes: Buffer; summary: CascadeSummary } {
	if (one(xml, 'stageType') !== 'BOOST') throw new Error('cascade is not a boosted cascade');
	if (one(xml, 'featureType') !== 'LBP') throw new Error('cascade is not an LBP cascade');

	const categories = Number(one(xml, 'maxCatCount'));
	if (categories !== 256) {
		throw new Error(`cascade has maxCatCount ${categories}, and an LBP code is 8 bits`);
	}

	const windowWidth = Number(one(xml, 'width'));
	const windowHeight = Number(one(xml, 'height'));
	const declared = Number(one(xml, 'stageNum'));

	if (!(windowWidth > 0) || !(windowHeight > 0)) throw new Error('cascade has no window size');

	const stages = items(one(xml, 'stages'));
	if (stages.length !== declared) {
		throw new Error(`cascade declares ${declared} stages and holds ${stages.length}`);
	}

	const thresholds: number[] = [];
	const firsts: number[] = [];
	const counts: number[] = [];
	const stumpFeature: number[] = [];
	const stumpSubset: number[] = [];
	const stumpLeaf: number[] = [];

	for (const stage of stages) {
		const weak = items(one(stage, 'weakClassifiers'));
		const declaredWeak = Number(one(stage, 'maxWeakCount'));

		if (weak.length !== declaredWeak) {
			throw new Error(
				`a stage declares ${declaredWeak} classifiers and holds ${weak.length}`
			);
		}

		thresholds.push(Number(one(stage, 'stageThreshold')));
		firsts.push(stumpFeature.length);
		counts.push(weak.length);

		for (const classifier of weak) {
			const nodes = numbers(one(classifier, 'internalNodes'));
			const leaves = numbers(one(classifier, 'leafValues'));

			// 3 + 8: left, right, feature, then the subset. more than one node is a real tree, and
			// the detector walks none, so it is refused here rather than mis-evaluated there
			if (nodes.length !== 3 + SUBSET_WORDS) {
				throw new Error(
					`a classifier has ${nodes.length} node words; only single-node stumps are supported`
				);
			}
			if (leaves.length !== 2) {
				throw new Error(`a stump has ${leaves.length} leaves, and a stump has two`);
			}
			if (nodes[0] !== 0 || nodes[1] !== -1) {
				throw new Error(
					`a stump points at children ${nodes[0]} and ${nodes[1]}, not 0 and -1`
				);
			}

			const feature = nodes[2]!;
			if (!Number.isInteger(feature) || feature < 0) {
				throw new Error(`a stump reads feature ${feature}`);
			}

			stumpFeature.push(feature);
			// signed in the XML because OpenCV writes an int; the bits are what matter
			for (let i = 0; i < SUBSET_WORDS; i++) stumpSubset.push(nodes[3 + i]! >>> 0);
			stumpLeaf.push(leaves[0]!, leaves[1]!);
		}
	}

	const rects = [...one(xml, 'features').matchAll(/<rect>([\s\S]*?)<\/rect>/g)].map((match) =>
		numbers(match[1]!)
	);

	if (rects.length === 0) throw new Error('cascade has no features');

	const features: number[] = [];

	for (const rect of rects) {
		if (rect.length !== 4) throw new Error(`a feature rect has ${rect.length} numbers`);

		for (const value of rect) {
			// the packed rect is one byte per field, which every cascade fits: the largest block in
			// the two shipped ones is 15, against a window of 45
			if (!Number.isInteger(value) || value < 0 || value > 255) {
				throw new Error(`a feature rect holds ${value}, which does not fit a byte`);
			}
		}

		const [x, y, w, h] = rect as [number, number, number, number];

		if (w === 0 || h === 0) throw new Error('a feature rect has a zero block');
		if (x + 3 * w > windowWidth || y + 3 * h > windowHeight) {
			throw new Error(`a feature at ${x},${y} sized ${w}x${h} reaches outside the window`);
		}

		features.push(x, y, w, h);
	}

	for (const feature of stumpFeature) {
		if (feature >= rects.length) {
			throw new Error(`a stump reads feature ${feature} of ${rects.length}`);
		}
	}

	const stumps = stumpFeature.length;
	const bytes = Buffer.alloc(
		HEADER +
			stages.length * 12 +
			stumps * 4 +
			stumps * SUBSET_WORDS * 4 +
			stumps * 8 +
			features.length
	);

	bytes.writeUInt32LE(MAGIC, 0);
	bytes.writeUInt32LE(1, 4);
	bytes.writeUInt32LE(windowWidth, 8);
	bytes.writeUInt32LE(windowHeight, 12);
	bytes.writeUInt32LE(stages.length, 16);
	bytes.writeUInt32LE(stumps, 20);
	bytes.writeUInt32LE(rects.length, 24);

	let at = HEADER;
	const put = (write: (value: number, at: number) => void, values: number[], stride: number) => {
		for (const value of values) {
			write(value, at);
			at += stride;
		}
	};

	put((v, o) => bytes.writeFloatLE(v, o), thresholds, 4);
	put((v, o) => bytes.writeUInt32LE(v, o), firsts, 4);
	put((v, o) => bytes.writeUInt32LE(v, o), counts, 4);
	put((v, o) => bytes.writeUInt32LE(v, o), stumpFeature, 4);
	put((v, o) => bytes.writeUInt32LE(v, o), stumpSubset, 4);
	put((v, o) => bytes.writeFloatLE(v, o), stumpLeaf, 4);
	put((v, o) => bytes.writeUInt8(v, o), features, 1);

	if (at !== bytes.length) {
		throw new Error(`packed ${at} bytes into a ${bytes.length} byte buffer`);
	}

	return {
		bytes,
		summary: {
			window: `${windowWidth}x${windowHeight}`,
			stages: stages.length,
			stumps,
			features: rects.length,
			bytes: bytes.length
		}
	};
}
