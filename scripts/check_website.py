"""Check the buildless homepage before publishing; no third-party dependencies."""
import json
import re
import xml.etree.ElementTree as ET
from html.parser import HTMLParser
from pathlib import Path
from urllib.parse import unquote, urlsplit

ROOT = Path(__file__).resolve().parents[1]
SITE = ROOT / 'website'
CANONICAL = 'https://notkaede-11.github.io/Coco-Engine/'


class Page(HTMLParser):
    def __init__(self):
        super().__init__()
        self.tags = []
        self.ids = []

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        self.tags.append((tag, attrs))
        if 'id' in attrs:
            self.ids.append(attrs['id'])


def main():
    text = (SITE / 'index.html').read_text(encoding='utf-8')
    page = Page()
    page.feed(text)
    release = json.loads((ROOT / 'release.json').read_text())
    assert len(page.ids) == len(set(page.ids)), 'Duplicate IDs'
    assert sum(tag == 'h1' for tag, _ in page.tags) == 1, 'Use one project heading'
    assert '<html lang="en">' in text
    assert '<title>Coco Chess Engine' in text
    assert any(a.get('name') == 'description' and a.get('content') for _, a in page.tags)
    assert any(a.get('rel') == 'canonical' and a.get('href') == CANONICAL for _, a in page.tags)
    assert any(a.get('name') == 'robots' and 'index' in a.get('content', '') and 'noindex' not in a.get('content', '') for _, a in page.tags)
    metadata = json.loads(re.search(r'<script type="application/ld\+json">(.*?)</script>', text, re.S)[1])
    assert metadata['name'] == 'Coco Chess Engine'
    assert metadata['softwareVersion'] == release['version'], 'Update homepage for the release'
    assert metadata['url'] == CANONICAL
    assert metadata['downloadUrl'].endswith('/v' + release['version'])
    assert metadata['offers']['price'] == '0'
    assert 'aggregateRating' not in metadata, 'Do not misrepresent Elo as a review rating'
    assert 'CuckooChess are different projects' in text
    assert 'download-link' in page.ids and 'download-platform' in page.ids
    assert 'copy-commands' not in page.ids and 'results' not in page.ids
    assert any(tag == 'script' and a.get('type') == 'module' for tag, a in page.tags)
    links = 0
    for tag, attrs in page.tags:
        if tag == 'img':
            assert 'alt' in attrs and attrs.get('width') and attrs.get('height')
        for attr in ('href', 'src'):
            target = attrs.get(attr)
            if not target:
                continue
            url = urlsplit(target)
            assert url.scheme not in ('javascript', 'file'), target
            if url.scheme:
                assert url.scheme in ('https', 'http'), target
                continue
            if url.path:
                asset = (SITE / unquote(url.path)).resolve()
                assert asset.is_relative_to(SITE.resolve()) and asset.is_file(), target
            elif url.fragment:
                assert url.fragment in page.ids, target
            links += 1
    css = (SITE / 'style.css').read_text()
    for asset in re.findall(r'url\([\"\']?([^\"\')]+)', css):
        assert (SITE / asset).is_file(), asset
    assert 'prefers-reduced-motion' in css and ':focus-visible' in css
    for font in ('amiri-latin.woff2', 'gloock-latin.woff2'):
        assert (SITE / 'assets' / font).read_bytes()[:4] == b'wOF2'
    assert (SITE / 'assets/coco-hat.png').read_bytes()[:8] == bytes([137,80,78,71,13,10,26,10])
    sitemap = ET.parse(SITE / 'sitemap.xml')
    assert [x.text for x in sitemap.findall('.//{*}loc')] == [CANONICAL]
    robots = (SITE / 'robots.txt').read_text()
    assert 'Allow: /' in robots and CANONICAL + 'sitemap.xml' in robots
    provenance = json.loads((SITE / 'assets/provenance.json').read_text())
    assert provenance['coco-hat.png']['source'] == 'assets/logo.png in this repository'
    assert not (SITE / 'assets/coco-atelier.webp').exists()
    assert 'hero-art' not in text and 'AI-generated fan artwork' not in text
    for path in SITE.rglob('*'):
        if not path.is_file():
            continue
        assert path.suffix.lower() not in ('.exe', '.nnue', '.sqlite', '.pgn', '.env'), path
        if path.suffix in ('.html', '.css', '.js', '.mjs', '.json', '.txt', '.xml', '.svg'):
            content = path.read_text(encoding='utf-8')
            assert not re.search(r'[A-Za-z]:[\\/](?:Users|home)[\\/]|/' + r'home/[^/\s]+/', content), path
    print(f'Homepage checks passed: release {release["version"]}, {links} local links/assets, metadata, sitemap, accessibility hooks, provenance, and public-file boundaries.')


if __name__ == '__main__':
    main()
