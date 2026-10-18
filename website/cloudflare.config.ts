// openh1.com: the landing page (public/), static assets on Workers
import { defineConfig } from "cf/config";

export default defineConfig({
  worker: {
    "name": "openh1",
    "compatibilityDate": "2026-10-06",
    "domains": ["openh1.com", "www.openh1.com"]
  }
});
