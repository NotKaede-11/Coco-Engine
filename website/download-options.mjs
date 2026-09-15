// OS clues do not prove CPU instruction support. Never guess AVX capability.
export const builds = {
  windows: {x64: 'coco-chess-windows-x86-64-popcnt.exe'},
  macos: {arm64: 'coco-chess-macos-apple-silicon', x64: 'coco-chess-macos-x86-64-popcnt'},
  linux: {x64: 'coco-chess-linux-x86-64-popcnt', arm64: 'coco-chess-linux-arm64'},
};
export const names = {windows: 'Windows', macos: 'macOS', linux: 'Linux'};
export function detectPlatform({userAgent = '', platform = '', hintPlatform = '', maxTouchPoints = 0, mobile = false} = {}) {
  const identity = `${userAgent} ${platform} ${hintPlatform}`;
  if (mobile || /Android|iPhone|iPad|iPod|CrOS/i.test(identity) || (/Mac/i.test(identity) && maxTouchPoints > 1)) return '';
  if (/Windows|Win32|Win64/i.test(identity)) return 'windows';
  if (/macOS|MacIntel|Macintosh|MacPPC/i.test(identity)) return 'macos';
  if (/Linux/i.test(identity)) return 'linux';
  return '';
}
export function detectArchitecture(os, {userAgent = '', platform = ''} = {}) {
  const identity = `${userAgent} ${platform}`;
  // Safari's Intel-looking UA can also run on Apple Silicon.
  if (os === 'macos') return '';
  if (/aarch64|arm64/i.test(identity)) return 'arm64';
  if (/x86_64|x64|Win64|WOW64|amd64/i.test(identity)) return 'x64';
  return '';
}
export function downloadChoice(os, architecture, releaseUrl) {
  const asset = builds[os]?.[architecture];
  if (!asset) return {href: releaseUrl, label: 'View all downloads', asset: null};
  return {href: `${releaseUrl.replace('/tag/', '/download/')}/${asset}`, label: `Download for ${names[os]}`, asset};
}
