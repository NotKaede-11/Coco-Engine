# Coco homepage maintenance

The one-page homepage is kept in `website/`, independently of the engine.
Its expected public address is https://notkaede-11.github.io/Coco-Engine/.
Only this directory is uploaded by the `Coco homepage` workflow. Engine source,
networks, private notes, and analysis outputs are not part of the Pages artifact.

## Preview and validate

```sh
python -m http.server 8765 --directory website
python scripts/check_website.py
node --test scripts/test_website_download.mjs
```

Open http://localhost:8765/. No build, package installation, external font
request, analytics service, or client-side rendering is required. The main download link works without JavaScript. JavaScript preselects the
operating system and a conservative build when the browser supplies useful
processor clues. All decisions stay in the browser; no detection data is sent
to a server. Platform and processor choices remain editable. macOS always asks
for Apple Silicon or Intel because an Intel-looking browser identifier is not
reliable hardware evidence. Mobile, ChromeOS, unknown, and unsupported combinations
fall back to the release downloads. CPU instruction support is not detected;
x86-64 choices require POPCNT and do not assume AVX support.

## First publication

In the repository's Settings > Pages, select **GitHub Actions** as the build
source. After the homepage commit is pushed to `main`, the Pages workflow
validates the site and deploys it. Pull requests validate but do not publish.
The workflow can also be started manually on `main`.

After deployment, verify the homepage and sitemap return HTTP 200, then set
the GitHub About website to the public homepage. The Lichess profile remains
linked from the site. Do not claim the website is published before a successful
deployment and HTTP check.

## SEO and crawler controls

- The HTML contains a project-specific title, description, canonical URL,
  social text metadata, and SoftwareApplication JSON-LD matching visible facts.
- Names consistently identify **Coco Chess Engine**, its maintainer, source
  repository, and current release. The brief project introduction distinguishes CuckooChess.
- `sitemap.xml` contains only the canonical homepage. Submit its URL through
  Google Search Console and Bing Webmaster Tools after verifying ownership.
  Submission helps discovery; it does not guarantee indexing or AI citations.
- HTML permits indexing and link following. Important content is available
  without JavaScript.
- **robots.txt is scoped to the host root.** At a project URL such as
  `/Coco-Engine/`, the authoritative file is
  `https://notkaede-11.github.io/robots.txt`, not
  `/Coco-Engine/robots.txt`. The bundled file is a policy ready for root-domain
  hosting; it cannot override a user-site or host-level restriction. At the
  initial read-only check on 2026-09-15, the host-root URL returned HTTP 404.
- On a domain you control, allow Googlebot, Bingbot, and OAI-SearchBot as desired.
  `User-agent: *` with `Allow: /` allows all crawlers that honor robots rules;
  this includes training crawlers, not only search crawlers. Search and training
  preferences can be configured separately. There is no special AI-ranking
  guarantee from robots directives, schema, or an `llms.txt` file.
- If hosting moves, update canonical, JSON-LD identifiers, social URL, sitemap,
  README link, and crawler sitemap URL together. Recheck host-root policy.

References: [Google AI search guidance](https://developers.google.com/search/docs/fundamentals/ai-optimization-guide),
[OpenAI crawlers](https://developers.openai.com/api/docs/bots),
[robots.txt scope](https://developers.google.com/search/docs/crawling-indexing/robots/robots_txt).

## Releases and factual updates

When releasing a new engine version, update the homepage's download links,
visible release version, description, and structured data alongside `release.json`.
Keep historical gauntlet statistics attached to the version actually tested.
Do not replace the local estimate with an official-rating claim without its
corresponding published source. Existing releases and engine binaries are
unaffected by website-only changes.

## Visual assets and typography

The homepage uses the creator's existing turquoise-and-gold hat from
`assets/logo.png`. The website copy is cropped to remove surrounding whitespace;
the original remains unchanged. The favicon, navigation, and page emblems all
use this same drawing. There is no AI-generated illustration on the page.

The [official Witch Hat Atelier website](https://tongari-anime.com/en/) was
inspected for typography: its styles load Amiri and Gloock, among other fonts,
and its English prose uses Amiri. This homepage uses Gloock for display headings
and Amiri for prose, with plain sans-serif navigation. It does not copy the
series' title artwork or claim an official series typeface.

Both fonts are self-hosted under the included SIL Open Font Licenses.
Asset origins are recorded in `website/assets/provenance.json`.

The hero's authored geometric lines draw once over 2.8 seconds. Text is always
visible, and reduced-motion preferences disable the animation. The project is
unaffiliated with Witch Hat Atelier's creators or publishers.

## Content scope

The homepage keeps project identity, a download area, a short introduction, and
source/support links. Detailed gauntlet tables, repeated feature explanations,
FAQ repetitions, and the terminal tutorial were removed at the user's request.
Release results and setup details remain one link away. The current Gloock/Amiri
typography is provisional: the user is gathering references for a more magical
lettering style. Do not treat those fonts as approved.
