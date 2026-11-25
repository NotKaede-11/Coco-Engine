# Third-party notices

## Ravager 3 evaluator

The selected evaluator includes network inference and attack-table code from
[Ravager 3](https://github.com/erensh27/Ravager), by erensh27, pinned to
`2470d945fccc3876b6a2bea2b9bffbd249d033e3`.
Retained source and its original notices are in `third_party/ravager/`.
The source is AGPLv3-covered; its complete license is preserved at
`third_party/ravager/LICENSE`. Original Coco components retain their GPLv3
terms in `LICENSE`. Distribution of the combined work must preserve the
applicable notices and matching corresponding source; see GPLv3 section 13
and AGPLv3. Release-page attribution does not replace these requirements.

For the November 17, 2026 integration, Coco added x86/scalar platform
guards and an equivalent scalar square-difference fallback to
`third_party/ravager/nnue/inference.c` so supported non-x86 builds can
compile. Coco's adapter and embedded-byte loader are separate
files under `src/`. Source hashes and evaluation/search parity are recorded
with the release-integration evidence. No Ravager search or clock prototype
is selected for this release.

## Network weights

Ravager 3's `Ravager_NET.nnue` is published under CC0 1.0 in its pinned
`docs/CREDITS.md`. Coco retains the original 89,315,360 bytes unchanged at
`networks/coco.nnue`, then embeds them in each release executable. The filename
was changed for Coco packaging; the weight bytes and architecture are identical
to the original Ravager v3 network.

SHA-256: `ffaf35d6c38389dcf30fcce12ff9abd7b46ab79dc3614fe1fd3eea8d804317b0`.

## Fathom and Syzygy

The retained `incbin.h`, by Dale Weiler, is the public-domain embedding
utility credited in Ravager's pinned component notices.

Fathom provides the retained Syzygy probe implementation. Preserve the
copyright and MIT permission notice at `Fathom/src/LICENSE`, including
Ronald de Man, basil00 and Jon Dart. Tablebase files are not included.

## Search and time-allocation references

Calvin 5.1.0 informed the selected aspiration, quiet-reduction and clock
formulas. Coco's fixed policy is expressed through `src/search_policy.*`
and its board/search/clock interfaces. The original all-donor experimental
policy implementation and weight trials are not release modules.

The retained policy formulas were checked against Calvin's MIT-licensed
source by Dan Kelsey at [revision `f80bd56075731688a80e94d928ac19e263df3a9d`](https://github.com/kelseyde/calvin-chess-engine/commit/f80bd56075731688a80e94d928ac19e263df3a9d).
The complete copyright and MIT notice is in `third_party/calvin/LICENSE`.
The historical 5.1.0 revision `44efc26e17170162984ceb22de45f2e35728bb1c`
documents earlier parameter values. Its standalone license remains
unverified. Coco 2.0 uses Ravager weights.
