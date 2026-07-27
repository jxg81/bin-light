# Publishing a firmware update

The devices check [`latest.json`](latest.json) in this directory and, if its
`version` differs from what they are running, download the image at `url` and
flash it. That is the whole backend — a file in the repo and a GitHub Release
asset. Nothing to host, nothing to keep running.

## Release checklist

1. **Bump [`../version.txt`](../version.txt)**. ESP-IDF compiles this into the
   image's `esp_app_desc_t`, and it is what the device compares against the
   manifest. Forgetting it means devices download the update, flash it, and
   still think an update is available — an endless loop.

2. **Build and sanity-check the size:**
   ```
   idf.py build
   ```
   The image must fit `ota_0`/`ota_1` (0x1F0000 = 1.94MB). The build prints
   the margin.

3. **Tag and create a GitHub Release**, attaching `build/sample_project.bin`
   renamed to `bin-light.bin`:
   ```
   git tag v1.1.0 && git push origin v1.1.0
   gh release create v1.1.0 build/sample_project.bin#bin-light.bin \
       --title v1.1.0 --notes "what changed"
   ```

4. **Update `latest.json`** — `version` must match `version.txt` exactly, and
   `url` must point at the asset you just uploaded. Commit and push to `main`.

5. **Check a device**: its Firmware section should offer the new version.
   `raw.githubusercontent.com` caches for a few minutes, so allow for that.

> **⚠️ If the release changes how updating itself works, you cannot test that
> by installing it.** The firmware performing an install is the *old* one, so
> the download, the buffers and the restart-afterwards decision all run in the
> image being replaced. Anything you changed there is first exercised by the
> **next** release. Only boot-time behaviour is proven by the release's own
> arrival. See SPEC.md §3.5.0.

**Do steps 3 and 4 in that order.** Publishing the manifest before the asset
exists means every device that checks in the gap gets a failed download.

> **⚠️ The asset filename must be exactly `bin-light.bin`, and don't fetch the
> URL before it is.** `gh release create file#label` sets the *display label*,
> not the filename — the download URL always uses the real filename, so upload
> a file actually named `bin-light.bin` (copy it, don't rely on `#`).
>
> And GitHub **caches the 404**. Requesting the download URL before the asset
> is in place under that name poisons it for a few minutes, so the URL keeps
> 404ing after you fix the upload. `?cb=1` on the end proves whether it is
> really fixed. Both of these were hit for real publishing 1.0.2.
>
> Always confirm the plain URL returns 200 *before* committing `latest.json`.

## Rolling back

Publish the *older* version in `latest.json`. The device compares for
*difference*, not for "newer", so it will happily move backwards — which is
the fastest way to recover a bad release across devices you don't have
physical access to.

> **⚠️ Never point `latest.json` at 1.0.0 or 1.0.1.** Those builds cannot
> perform an OTA at all — their HTTP client's TX buffer is too small for
> GitHub's signed redirect URL (SPEC.md §6 bug 23), so the download fails at
> the redirect every time. A device that rolls back to one is stuck there and
> needs a USB cable. **1.0.2 is the oldest safe rollback target.**
>
> The general rule this is an instance of: a release can only be rolled *back*
> to if it is itself capable of rolling forward. Anything that breaks OTA is a
> one-way door, which is exactly why the automatic bootloader rollback below
> matters more than the manual kind.

There is also an automatic safety net: a freshly-flashed image is marked
valid only once it has booted, joined Wi-Fi and started serving. An update
that **crashes** therefore reboots, and the bootloader reverts it with nobody
touching the device. Verified on hardware — see SPEC.md §3.5.1.

An image that **hangs** rather than crashing is covered separately, because
rollback is decided at boot and a hang never produces the necessary reset. From
1.0.4 a watchdog handles it: an image still unverified ten minutes after boot
rolls itself back. So a release that breaks the network path recovers on its
own too — it just takes ten minutes rather than seconds.

> Both paths verified on hardware — a crashing image and a hanging one — see
> SPEC.md §3.5.1. **The watchdog needs 1.0.6 or later**; earlier releases
> recover from crashes only.

## Security note

The image is fetched over HTTPS and validated against ESP-IDF's bundled CA
roots, so its authenticity rests on GitHub's certificate and on who can push
to this repo. There is **no firmware signature check**: anyone able to alter
the release asset could ship code to every device.

For a handful of devices among friends that is a reasonable trade. If this
ever grows, enable `CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT` and sign
images — it verifies a signature at OTA time without the irreversibility of
full secure boot. The signing key must never be committed here.
