import test from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {builds, detectPlatform, detectArchitecture, downloadChoice} from '../website/download-options.mjs';
const release = JSON.parse(readFileSync(new URL('../release.json', import.meta.url)));
const releaseUrl = `https://github.com/NotKaede-11/Coco-Engine/releases/tag/v${release.version}`;
test('desktop detection and conservative processor inference', () => {
  const windows = {userAgent: 'Mozilla/5.0 (Windows NT 10.0; Win64; x64)', platform: 'Win32'};
  assert.equal(detectPlatform(windows), 'windows');
  assert.equal(detectArchitecture('windows', windows), 'x64');
  assert.equal(detectPlatform({hintPlatform: 'macOS'}), 'macos');
  assert.equal(detectArchitecture('macos', {userAgent: 'Macintosh; Intel Mac OS X', platform: 'MacIntel'}), '');
  assert.equal(detectPlatform({userAgent: 'X11; Linux x86_64'}), 'linux');
  assert.equal(detectArchitecture('linux', {platform: 'Linux aarch64'}), 'arm64');
  assert.equal(detectArchitecture('linux', {platform: 'Linux i686'}), '');
});
test('mobile, iPad desktop mode, ChromeOS and unknown clients use generic downloads', () => {
  for (const device of [{userAgent: 'Linux; Android 14'}, {userAgent: 'iPhone'}, {platform: 'MacIntel', maxTouchPoints: 5}, {userAgent: 'X11; CrOS x86_64'}, {hintPlatform: 'Linux', mobile: true}, {}]) assert.equal(detectPlatform(device), '');
});
test('direct downloads match actual release artifacts; unsupported combinations fall back', () => {
  for (const [os, architectures] of Object.entries(builds)) {
    for (const architecture of Object.keys(architectures)) {
      const choice = downloadChoice(os, architecture, releaseUrl);
      assert.ok(release.artifacts.includes(choice.asset));
      assert.equal(choice.href, `https://github.com/NotKaede-11/Coco-Engine/releases/download/v${release.version}/${choice.asset}`);
      assert.doesNotMatch(choice.asset, /avx|bmi|dotprod/);
    }
  }
  for (const [os, architecture] of [['windows','arm64'], ['macos',''], ['linux','x86'], ['unknown','x64']]) assert.deepEqual(downloadChoice(os, architecture, releaseUrl), {href:releaseUrl,label:'View all downloads',asset:null});
});
