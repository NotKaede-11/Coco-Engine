# Coco homepage maintenance

The one-page homepage is kept in `website/`, independently of the engine.
Its expected public address is https://notkaede-11.github.io/Coco-Engine/.
Only this directory is uploaded by the `Coco homepage` workflow. Engine source,
networks, private notes, and analysis outputs are not part of the Pages artifact.

## Preview and validate

```sh
python -m http.server 8765 --directory website
python scripts/check_website.py
```

Open http://localhost:8765/. No build, package installation, external font
request, analytics service, or client-side rendering is required. The FAQ and
download links work without JavaScript. JavaScript only adds a copy button
when the Clipboard API is available in a secure context.

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
  repository, and current release. A visible FAQ distinguishes CuckooChess.
- `sitemap.xml` contains only the canonical homepage. Submit its URL through
  Google Search Console and Bing Webmaster Tools after verifying ownership.
  Submission helps discovery; it does not guarantee indexing or AI citations.
- HTML permits indexing and link following. Important content is available
  without JavaScript, and FAQ answers are present in the initial HTML.
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

## Visual assets

The hat favicon is a vector interpretation of Coco's existing pointed-hat
identity. The hero is original AI-generated fan artwork made for this page,
inspired by Coco and the world of Witch Hat Atelier. It is not official series
artwork. The original series and its characters belong to their respective
rights holders; this project claims no affiliation.

The illustration reference was the [official series website](https://tongari-anime.com/en/).
Asset provenance and the generation prompt are in `website/assets/provenance.json`.
The self-hosted Alegreya font is distributed under the SIL Open Font License;
the license is included in `website/assets/OFL-Alegreya.txt`.
