# Coco 1.5 development versus pre-release SPRT

Source: retained Kaggle console output, reviewed on 2026-09-14. The original PGNs and recovery archive are unavailable. This report records the configuration and decisive console result.

| Setting | Value |
| --- | --- |
| Runner schema | `coco-sprt-v2` |
| Candidate | Coco 1.5 development |
| Baseline | Coco 1.5 pre-release |
| Description | Development search changes vs Coco 1.5 pre-release |
| Time control | `10+0.1` |
| Maximum / chunk games | 10,000 / 20 |
| Concurrency / seed | 2 / 2026091401 |
| SPRT | Logistic, Elo0=0, Elo1=5, alpha=beta=0.05 |
| Common options | Threads=1, Hash=16, Contempt=0, SEE_Pruning_Depth=0, Use PEXT=false, EvalFile=coco.nnue |
| Candidate / baseline overrides | Both empty |

The log starts with `VERIFIED: 45 assets; START`. It records cumulative commits every 20 games through 1,080, followed by the native early stop at 1,092. The final excerpt is:

```text
18017.1s 731 COMMITTED 1092/10000: 332W 221L 539D; LLR 2.96 (100.4%) (-2.94, 2.94) [0.00, 5.00]; PASS
18021.7s 734 SESSION END: PASS; resume retains all committed native statistics.
{
  "status": "PASS",
  "games": 1092,
  "max_games": 10000,
  "score": 0.5508241758241759,
  "llr": "2.96 (100.4%) (-2.94, 2.94) [0.00, 5.00]",
  "mode": "START"
}
```

Arithmetic checks: 332+221+539=1,092; score=(332+539/2)/1,092=55.0824%; logistic estimate=400 log10(score/(1-score))=approximately +35.4 Elo. Acceptance threshold=log(0.95/0.05)=2.94444. Runtime was about five hours.

**Interpretation:** the retained log records a completed PASS for this combined candidate. `100.4%` measures LLR relative to the acceptance boundary; it is not confidence, win percentage, or probability of superiority. A sequential test can legitimately stop well before its maximum game count.

Limits: aggregate W/L/D does not preserve paired outcomes, binary hashes, opening order, or per-game adjudications. We cannot independently replay the native paired LLR or bind the console output cryptographically to the release source/binaries without the recovery assets. The log supports reporting the result with this provenance. It does not establish individual feature gains, SMP strength at multiple threads, or an official rating.
