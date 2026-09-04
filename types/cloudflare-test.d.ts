declare namespace Cloudflare {
	interface Env {
		/** The auxiliary worker holding the compiled module; see vitest.config.ts. */
		PROBE: Fetcher;
		CODEC: Fetcher;
	}
}
