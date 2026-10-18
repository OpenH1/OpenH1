# openh1.com

The project's landing page: one static page (`public/index.html`), the
menus' fonts (`public/fonts/`, from `port/assets/fonts`) and the app's icon
(`public/icon.png`, `favicon.png`, `apple-touch-icon.png`, from
`port/assets/icon`).

It is a Cloudflare Worker of static assets, `openh1`, on the custom domains
`openh1.com` and `www.openh1.com` (`cloudflare.config.ts`), in Paul's
Cloudflare account. To publish a change, from this folder:

```sh
npm install                                   # once: the Cloudflare CLI, Wrangler and Node 22
PATH="$PWD/node_modules/.bin:$PATH" npx cf deploy
```

The CLI needs Node 22.18 or later to load `cloudflare.config.ts`;
`node_modules/.bin` puts the `node` package's first, as the system's may be
older.
