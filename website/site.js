// The page, links, and FAQ work without JavaScript.
const copyButton = document.querySelector('#copy-commands');
const status = document.querySelector('#copy-status');
if (copyButton && navigator.clipboard && window.isSecureContext) {
  copyButton.hidden = false;
  copyButton.addEventListener('click', async () => {
    try {
      await navigator.clipboard.writeText(document.querySelector('#uci-commands').textContent);
      status.textContent = 'Commands copied to clipboard.';
      copyButton.textContent = 'Copied';
    } catch {
      status.textContent = 'Copy was unavailable. Select and copy the commands below.';
      copyButton.textContent = 'Select text below';
    }
  });
}
