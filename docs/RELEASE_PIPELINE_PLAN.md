# Release pipeline plan (goal: "release <version>" to public in 2 hours or less)

Source: scratchpad/0.2.9-goals.md sections 0 and 1.

## Done
- ccache is already wired into ci.yml, ci-full.yml and the release.yml Linux job.
- Docker job: the .deb download now retries. Suspected root cause of the 404: release.yml creates the release as a draft, and anonymous downloads of draft assets return 404. The real fix needs a release.yml change (pass a token as a build secret, or run the docker job after the release is public). Not done, release.yml is frozen.

## Windows build in GitHub
The Windows job fails today because it needs `build-windows-deps/src`, which exists only on the owner's Mac.
1. On the Mac, tar `build-windows-deps/src` (sources and prebuilt cross-deps) and note its hash.
2. Upload it as a release asset of a private "deps" release or as a GitHub Actions cache seeded once (`actions/cache` key `winpfx-<hash of tools/package/windows_deps.lock>`), so the job restores it and rebuilds only on a changed lock file.
3. The job restores the cache, runs `tools/package/make_windows_cross.sh <ver>` (already used in release.yml), and attaches the installer to the draft.
4. Better long term: a script that rebuilds the deps from pinned upstream URLs, so the cache can always be regenerated.
Needs from the owner: run the tar/upload once, confirm which deps are not downloadable.

## Android build in GitHub
- Keystore stored only as GitHub secrets: `ANDROID_KEYSTORE_B64`, `ANDROID_KEYSTORE_PASSWORD`, `ANDROID_KEY_ALIAS`, `ANDROID_KEY_PASSWORD`. Never committed.
- The job decodes the keystore into `$RUNNER_TEMP`, runs Gradle with JDK 21 and the rustup stable toolchain first on PATH (Android targets), signs, attaches the APK to the draft, then deletes the keystore file.
- Needs from the owner: add the four secrets.

## Other items
- Lint gate (seconds) as the first job; fast test tier on every push, slow tests nightly; quarantine flaky timing tests with retry.
- Profile report: parse CI logs (about 800 tests per platform, 160-208 CPU-minutes, 34-58 tests over 60 s, Windows build 3177 compile steps) into a per-test time table to choose the slow tier.
- Never change CI files while a release is running.
