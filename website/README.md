# openh1.com

The project's landing page: one static page (`index.html`), the menus'
fonts (`fonts/`, from `port/assets/fonts`) and the app's icon (`icon.png`,
`favicon.png`, `apple-touch-icon.png`, from `port/assets/icon`).

It is hosted on Cloudflare Pages (the `openh1` project, with the domains
`openh1.com` and `www.openh1.com`). To publish a change:

```sh
cf pages deploy website --project-name openh1 --branch main
```
