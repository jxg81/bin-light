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

**Do steps 3 and 4 in that order.** Publishing the manifest before the asset
exists means every device that checks in the gap gets a failed download.

## Rolling back

Publish the *older* version in `latest.json`. The device compares for
*difference*, not for "newer", so it will happily move backwards — which is
the fastest way to recover a bad release across devices you don't have
physical access to.

There is also an automatic safety net: a freshly-flashed image is marked
valid only once it has booted, joined Wi-Fi and started serving. An update
that breaks any of that gets rolled back by the bootloader on the next
restart, without anyone touching the device.

## Security note

The image is fetched over HTTPS and validated against ESP-IDF's bundled CA
roots, so its authenticity rests on GitHub's certificate and on who can push
to this repo. There is **no firmware signature check**: anyone able to alter
the release asset could ship code to every device.

For a handful of devices among friends that is a reasonable trade. If this
ever grows, enable `CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT` and sign
images — it verifies a signature at OTA time without the irreversibility of
full secure boot. The signing key must never be committed here.
