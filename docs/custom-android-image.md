# Building your own Android image for Aetheron

BlueStacks looks clean out of the box because it ships an Android it built
itself: its own launcher, wallpaper, boot animation and app set, plus Google
apps under a licence from Google. Aetheron boots whatever image you give it,
so the same is possible here. This is done on Linux, not in Aetheron.

## What you need

- A Linux PC or VM (Ubuntu 22.04 or newer), about 400 GB of free disk, 32 GB
  of RAM or more, and several hours for the first build.
- The Android source (AOSP). Follow Google's guide for installing `repo` and
  the build packages: <https://source.android.com/docs/setup/start>.

## 1. Build the stock image first

```sh
mkdir aosp && cd aosp
repo init -u https://android.googlesource.com/platform/manifest -b <an Android 16 release branch>
repo sync -c -j8
source build/envsetup.sh
lunch            # pick the aosp_cf_x86_64_only_phone ... userdebug entry
m -j$(nproc) dist
```

`out/dist/` then contains `aosp_cf_x86_64_only_phone-img-*.zip`. Unzip it into
Aetheron's `android16_ci\images\` folder (`boot.img`, `init_boot.img`,
`vendor_boot.img`, `super.img`, `userdata.img`, `vbmeta*.img`). Check that it
boots before changing anything. Use Aetheron menu -> Factory reset when you
switch images.

## 2. Make it yours

Create your own product that inherits the Cuttlefish one, for example
`device/aetheron/aetheron_x86_64.mk`:

```make
$(call inherit-product, device/google/cuttlefish/vsoc_x86_64_only/phone/aosp_cf.mk)
PRODUCT_NAME := aetheron_x86_64
PRODUCT_BRAND := Aetheron
PRODUCT_MODEL := Aetheron

# Default screen density and other defaults
PRODUCT_PRODUCT_PROPERTIES += ro.sf.lcd_density=160

# Your boot animation
PRODUCT_COPY_FILES += device/aetheron/bootanimation.zip:$(TARGET_COPY_OUT_PRODUCT)/media/bootanimation.zip

# Your apps and the module that removes the stock ones (see below)
PRODUCT_PACKAGES += AetheronLauncher AetheronRemovals
```

(The exact path of the Cuttlefish product file differs between Android
versions; look in `device/google/cuttlefish/` for `aosp_cf.mk`. The product
also has to be listed in an `AndroidProducts.mk` next to it so `lunch` shows it.)

- **Remove stock apps**: a module that overrides them, in
  `device/aetheron/Android.bp`:

  ```
  android_app_import {            // or any app module of your own
      name: "AetheronRemovals",
      ...
      overrides: ["Gallery2", "Camera2", "Dialer", "messaging", "Music",
                  "Calendar", "DeskClock", "Contacts", "QuickSearchBox", "Browser2"],
  }
  ```

  An overridden app is simply not put into the image.
- **Launcher**: fork `packages/apps/Launcher3` (rename the package), change
  its default layout in `res/xml/default_workspace_*.xml`, and override
  `Launcher3QuickStep` with it. This is where a BlueStacks-style home screen
  (search bar, big icon grid) is made.
- **Wallpaper and system colours**: a resource overlay (RRO) for
  `frameworks/base` replacing `default_wallpaper` and the theme colours.
- **Boot animation**: `bootanimation.zip` as above (format:
  <https://android.googlesource.com/platform/frameworks/base/+/main/cmds/bootanimation/FORMAT.md>).
  Then set `androidboot.enable_bootanimation = 1` in
  `config\aetheron_bootconfig.txt` and `boot_splash = off` in `aetheron.ini`.

Build it the same way, choosing your product in `lunch`.

## 3. Google Play

AOSP contains no Google apps. Google licenses the Play Store and Play
services to device makers (BlueStacks has such an agreement); they are not
open source and Aetheron does not include them.

- For a product you distribute, you need that licence from Google.
- Without Google apps, open-source stores work on a plain image: build
  their `.apk` into your image, or install it over adb (`adb_bridge = on` in
  `aetheron.ini`, then `adb connect 127.0.0.1:6520`).
- Community "GApps" packages can be built into a private image, but Google
  then treats the device as uncertified and Play may refuse to sign in until
  the device is registered. That is between you and Google's terms.

