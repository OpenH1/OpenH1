// goath1.com: the landing page (public/), static assets on Workers. The
// Worker keeps its name, openh1, and serves the old domains too.
import { defineConfig } from "cf/config";

export default defineConfig({
  worker: {
    "name": "openh1",
    "compatibilityDate": "2026-10-06",
    "domains": ["goath1.com", "www.goath1.com", "openh1.com", "www.openh1.com"]
  }
});
