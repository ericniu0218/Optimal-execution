import react from '@vitejs/plugin-react';
import { defineConfig } from 'vitest/config';

/**
 * Social scrapers (LinkedIn, Slack, iMessage) need an ABSOLUTE og:image URL. On Vercel the
 * production hostname is exposed at build time; set SITE_URL yourself for a custom domain.
 * Locally it falls back to a relative path, which is harmless.
 */
const siteUrl = (
  process.env.SITE_URL ??
  (process.env.VERCEL_PROJECT_PRODUCTION_URL ? `https://${process.env.VERCEL_PROJECT_PRODUCTION_URL}` : '')
).replace(/\/$/, '');

export default defineConfig({
  plugins: [
    react(),
    {
      name: 'site-url',
      transformIndexHtml: {
        order: 'pre',
        handler: (html) => html.replaceAll('%SITE_URL%', siteUrl),
      },
    },
  ],
  build: { target: 'es2022', sourcemap: false },
  test: { environment: 'node', include: ['src/**/*.test.ts'] },
});
