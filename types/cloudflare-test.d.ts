declare namespace Cloudflare {
	interface Env {
		/** The auxiliary worker holding the compiled module; see vitest.config.ts. */
		PROBE: Fetcher;
		CODEC: Fetcher;
		/** The worker running the shipped wrapper out of `dist/`. */
		WRAPPER: Fetcher;
	}
}
