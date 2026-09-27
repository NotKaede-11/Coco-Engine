# Release procedure

1. Review correctness and playing-strength results. Retain engine/network identities, match conditions, original scores and any exclusions. Publish the relevant results and limitations with the release notes.
2. Check `release.json`: version, UCI identity, network size/hash, deterministic signature and expected artifacts must match the intended source. Run `python scripts/check_release.py` and `python testing/release_pipeline_test.py`. Investigate changed signatures before updating the expected value.
3. Run `python scripts/check_public_paths.py` on the staged files. Public documentation should describe engine behavior, setup and results without personal paths or private session records.
4. Run correctness CI and the release build matrix. Review protocol, perft, state/search fixtures and sanitizer results. Keep performance and timed-match measurements isolated from other CPU workloads. Inspect the manifests: cross-compilation does not establish runtime validation.
5. Set each `readiness_checks` entry only after completing that check. Set `release_ready` to true only when all checks pass. Run `python scripts/check_release.py --ready --tag v1.5.1` for this version.
6. Update the README release status and badge, the documentation index, release metadata and release notes. Change the changelog's unreleased heading to the final version and actual release date. Preserve previous version entries and tags.
7. Review the final changes, commit and create the matching version tag. The tag-triggered workflow builds the artifacts, runs correctness CI and checks readiness before creating a draft release titled with the version only. Review its binaries, checksums, provenance, licenses and notes before publishing.

The packaging checks reject missing or duplicate artifacts, changed binary hashes, conflicting source revisions, wrong networks and wrong signatures. They complement correctness and strength tests; they do not replace them.

For v1.5.1, strength-review flags retain the published v1.5.0 evidence because
search and network bytes are unchanged. No new Elo result is implied. Keep
`release_platform_checks_passed` and `release_ready` false until the hotfix
platform matrix and correctness checks have been reviewed. Run the new
`testing/nnue_loading_test.py` against each runnable release build.
