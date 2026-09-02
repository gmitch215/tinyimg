import { Marked } from 'marked';
import { gfmHeadingId } from 'marked-gfm-heading-id';
import { statSync } from 'node:fs';
import { readFile, writeFile } from 'node:fs/promises';
import { dirname, join } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { createHighlighter, type BundledLanguage } from 'shiki';

const here = dirname(fileURLToPath(import.meta.url));

/** Languages the README fences, plus the ones a future section is likely to reach for. */
const LANGUAGES: BundledLanguage[] = ['c', 'typescript', 'shellscript', 'json', 'cmake'];

/** What shiki is asked to spell a fence as, for the ones the README writes differently. */
const ALIASES: Record<string, BundledLanguage> = {
	sh: 'shellscript',
	bash: 'shellscript',
	ts: 'typescript',
	js: 'typescript',
	jsonc: 'json',
	h: 'c'
};

const PLACEHOLDER = '<!-- readme -->';

export interface SiteOptions {
	/** The README, as Markdown. */
	readme: string;
	/** The page around it, carrying a `<!-- readme -->` line where the rendered Markdown goes. */
	template: string;
	/** The repository every relative link is resolved against. */
	repo: string;
	/** The ref those links point at. */
	ref?: string;
	/** The working tree those links are checked against, to tell a directory from a file. */
	root?: string;
}

/**
 * Renders the README into the landing page.
 *
 * The page is generated rather than written because the two would otherwise say different things: a
 * landing page maintained beside a README drifts from it on the first edit that only touches one.
 *
 * Three things change on the way through. The title and tagline are dropped, because the template
 * already carries them. Every relative link is resolved against the repository, since the published
 * site has no `LICENSE` or `tests/` to reach. And each fence is highlighted here rather than in the
 * browser, so the page needs no script at all.
 *
 * @param options The README, the page around it, and where relative links point.
 * @return The complete page.
 */
export async function renderSite(options: SiteOptions): Promise<string> {
	const ref = options.ref ?? 'master';

	const highlighter = await createHighlighter({
		themes: ['github-light', 'github-dark'],
		langs: LANGUAGES
	});

	const renderer = new Marked({ async: true, gfm: true });
	renderer.use(gfmHeadingId());
	renderer.use({
		async: true,
		walkTokens(token) {
			if (token.type === 'link' && isRelative(token.href)) {
				token.href = repository(options.repo, ref, token.href, options.root);
			}
			if (token.type === 'code') {
				token.text = highlighter.codeToHtml(token.text, {
					lang: resolve(token.lang),
					themes: { light: 'github-light', dark: 'github-dark' },
					defaultColor: false
				});
				// already a complete <pre>, so the renderer below hands it through untouched
				token.escaped = true;
			}
		},
		renderer: {
			code: ({ text }) => text
		}
	});

	const body = await renderer.parse(withoutTitle(options.readme));
	highlighter.dispose();

	if (!options.template.includes(PLACEHOLDER)) {
		throw new Error(`the template has no ${PLACEHOLDER} line to render into`);
	}
	return options.template.replace(PLACEHOLDER, scrollableTables(body).trimEnd());
}

/** Whether a link points inside the repository rather than at the web or at this page. */
function isRelative(href: string): boolean {
	return !/^[a-z][a-z0-9+.-]*:/i.test(href) && !href.startsWith('#') && !href.startsWith('/');
}

/**
 * A repository link for a path in the working tree.
 *
 * A directory takes `tree` and a file takes `blob`, and which one a path is gets read off disk rather
 * than guessed from its name; `LICENSE` carries no extension and `include` does not either. A path
 * that is not there is treated as a file, which is what a link into a generated directory is.
 */
function repository(repo: string, ref: string, path: string, root?: string): string {
	const relative = path.replace(/^\.\//, '').replace(/\/$/, '');
	const directory =
		root !== undefined &&
		statSync(join(root, relative), { throwIfNoEntry: false })?.isDirectory();
	return `${repo}/${directory ? 'tree' : 'blob'}/${ref}/${relative}`;
}

/** Wraps every table so a wide one scrolls inside itself rather than widening the page. */
function scrollableTables(html: string): string {
	return html
		.replaceAll('<table>', '<div class="table"><table>')
		.replaceAll('</table>', '</table></div>');
}

/** A fence's language, as shiki spells it, falling back to no highlighting at all. */
function resolve(lang: string | undefined): BundledLanguage | 'text' {
	if (!lang) return 'text';
	const named = lang.trim().split(/\s+/)[0]?.toLowerCase() ?? '';
	const aliased = ALIASES[named] ?? (named as BundledLanguage);
	return LANGUAGES.includes(aliased) ? aliased : 'text';
}

/**
 * The README without its title and tagline, which the template states itself.
 *
 * Only a leading pair is dropped: a blockquote further down is content.
 */
function withoutTitle(readme: string): string {
	return readme.replace(/^#[^\n]*\n+(>[^\n]*\n+)?/, '');
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
	const root = join(here, '..');
	const page = await renderSite({
		readme: await readFile(join(root, 'README.md'), 'utf8'),
		template: await readFile(join(here, 'template.html'), 'utf8'),
		repo: 'https://github.com/gmitch215/tinyimg',
		root
	});
	await writeFile(join(here, 'index.html'), page);
	console.log(`tinyimg: wrote ${join(here, 'index.html')}`);
}
