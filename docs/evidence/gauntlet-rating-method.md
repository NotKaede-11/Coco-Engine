# Gauntlet rating method — 2026-09-14

Use Ordo 1.2.6 on the frozen, completed gauntlet. Preserve every raw game. The headline calibration excludes the Coco 1.4 matchup because its external network was substituted. Report that matchup separately without a historical Elo claim.

## Calibration

The following exact-version entries were found in the same [CCRL Blitz complete list](https://computerchess.org.uk/404/rating_list_all.html), retrieved through the search index on 2026-09-14 (index crawl: two days earlier). Direct table retrieval was unavailable. They are calibration references, not results measured under this gauntlet's conditions.

| Reference | Rating | Listed error | Reference games |
| --- | ---: | ---: | ---: |
| Lynx 1.5.0 64-bit | 2819 | ±15 | 1257 |
| Sable 1.1.0 64-bit | 2933 | ±18 | 924 |
| Stash 25.0 64-bit | 2932 | ±18 | 874 |

Use these three ratings as fixed anchors for the conditional estimates. The release headline now uses the filtered fit (2977.0 over 4442 games); the original nine-opponent fit (2978.6 over 4500) remains the unfiltered comparison. Ordo's error bar excludes reference-rating uncertainty and systematic differences in hardware, time control, opening suite and opponent build. It is not an official CCRL rating or a full uncertainty interval for transfer to CCRL. Do not average the ten roster estimates or combine Ordo and BayesElo outputs.

The [Crustik 0.3.0 release notes](https://github.com/Dejon51/Crustik/releases/tag/0.3.0) describe 3050 as an author estimate. A 40/15 rating belongs to a different list and must not be mixed into these Blitz anchors. Other roster entries remain unanchored in this analysis; their ratings are inferred relative to Coco within this pool.

Fit white advantage and draw rate with `-W -D`; use the default Ordo scale (202 points at 76% expectancy), `-M`, and one CPU. Retain text/CSV output and exact commands. Run 1000 native error simulations at `-F 95`. Those simulations are model-based and do not explicitly preserve shared-opening correlations. Retain actual opening-pair counts and repeated-FEN counts to make that limitation visible.

## Required sensitivity checks

1. Repeat the fit with each of the three anchors individually. Their spread shows calibration dependence; it is not a confidence interval.
2. Repeat the three-anchor fit after excluding both games of every opening pair containing a time forfeit or abandoned game. Keep Coco losses and opponent losses in the same exclusion rule. This is a sensitivity analysis, not a replacement score table: removing games after seeing their termination can itself introduce bias.
3. Keep the original all-game opponent table, including the historical matchup and all forfeits, alongside both fits.

The completed run contained only 25 distinct FENs per opponent, each used for ten colour pairs. A 1000-sample bootstrap resamples whole opponent/FEN clusters within each opponent, retaining all remaining games per selected cluster. Seed: 20260914. The original bootstrap retains 20 games per cluster; the filtered bootstrap retains 16–20 and gives a conditional 95% percentile interval of 2959.3–2995.3. It estimates opening-sample variation conditional on fixed anchors; it does not establish coverage for the full chess-position population or transfer to another time control. Run `scripts/bootstrap_gauntlet_openings.py` with `--normal-pairs` for the filtered result. Post-run exclusion can introduce selection bias and is disclosed alongside the original fit.

The original gauntlet used concurrency 8 and no runner time margin. Other CPU workloads overlapped parts of the match. Attribution of individual forfeits to engine time management versus host scheduling is unresolved. Do not describe this as a clean, isolated strength measurement.

## Historical opponent identity

The [official v1.4.0 release](https://github.com/NotKaede-11/Coco-Engine/releases/tag/v1.4.0) asset digest matches the tested Windows executable:

- Executable: 4,243,362 bytes; SHA-256 `af849c4df119c1521b54b8b7bdb25ca3672671d602a9b2b27230796fb8afa068`.
- Official release network: 789,508 bytes; SHA-256 `392be46c8e06c6d0cb6bedf00d8e3d08950d11daa883362de98f0c1deee68055`.
- Network in the tested Coco 1.4 directory: 394,756 bytes; SHA-256 `e939164e853bff4e212eb4cf684a720750d8686863db211fcd82b50287de2ae4`.

Thus the executable is authenticated, but the tested binary/network package differs from the official download. File size alone does not validate the substituted network's feature semantics or strength. The matchup cannot establish gain over a faithfully reproduced official Coco 1.4 release.
