import {builds, names, detectPlatform, detectArchitecture, downloadChoice} from './download-options.mjs';

const controls = document.querySelector('#download-controls');
const osSelect = document.querySelector('#download-platform');
const processorSelect = document.querySelector('#download-processor');
const download = document.querySelector('#download-link');
const status = document.querySelector('#download-status');
const heroDownload = document.querySelector('#hero-download');
const heroLabel = heroDownload.textContent.trim();
const buildNote = document.querySelector('#build-note');
const releaseUrl = download.href;
const identity = {
  userAgent: navigator.userAgent, platform: navigator.platform,
  hintPlatform: navigator.userAgentData?.platform || '',
  mobile: navigator.userAgentData?.mobile || false, maxTouchPoints: navigator.maxTouchPoints || 0,
};
const detected = detectPlatform(identity);

function updateDownload() {
  const os = osSelect.value;
  const architecture = processorSelect.value;
  const choice = downloadChoice(os, architecture, releaseUrl);
  download.href = choice.href;
  download.textContent = choice.label;
  heroDownload.href = choice.asset ? choice.href : '#download';
  heroDownload.textContent = choice.asset ? choice.label : heroLabel;
  buildNote.textContent = choice.asset
    ? (architecture === 'arm64' ? (os === 'macos' ? 'Apple Silicon build' : 'ARM64 build') : `${os === 'macos' ? 'Intel' : 'Intel / AMD'} 64-bit build · requires POPCNT`)
    : (os ? 'Choose your processor, or browse all builds.' : 'Desktop releases for Windows, macOS, and Linux.');
}
function choosePlatform(automatic = false) {
  const os = osSelect.value;
  processorSelect.replaceChildren(new Option('Choose processor', ''));
  for (const architecture of Object.keys(builds[os] || {})) {
    const label = architecture === 'arm64' ? (os === 'macos' ? 'Apple Silicon (M-series)' : 'ARM64') : (os === 'macos' ? 'Intel 64-bit' : 'Intel / AMD 64-bit');
    processorSelect.add(new Option(label, architecture));
  }
  processorSelect.disabled = !os;
  const architecture = automatic ? detectArchitecture(os, identity) : '';
  processorSelect.value = builds[os]?.[architecture] ? architecture : '';
  status.textContent = automatic && os ? `${names[os]} detected. You can change the selection.` : (os ? `Showing ${names[os]} builds.` : 'Choose a desktop platform.');
  updateDownload();
}
osSelect.value = detected;
choosePlatform(true);
controls.hidden = false;
osSelect.addEventListener('change', () => choosePlatform(false));
processorSelect.addEventListener('change', updateDownload);

// An unknown-device hero button and the navigation link reveal the native chooser.
for (const link of document.querySelectorAll('a[href="#download"], #hero-download')) {
  link.addEventListener('click', () => {
    if (link.getAttribute('href') === '#download') document.querySelector('#download').open = true;
  });
}
