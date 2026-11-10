#!/bin/sh
# Builds apps/AetheronLauncher.apk from launcher/ without Gradle.
# Needs a JDK (17) and the Android SDK's build-tools and platform:
#   ANDROID_SDK=/path/to/sdk launcher/build.sh
set -e
cd "$(dirname "$0")"
SDK="${ANDROID_SDK:-/opt/android}"
BT="$SDK/build-tools/${BUILD_TOOLS:-34.0.0}"
JAR="$SDK/platforms/${PLATFORM:-android-34}/android.jar"
OUT=build
rm -rf "$OUT" && mkdir -p "$OUT/classes" "$OUT/dex"

"$BT/aapt2" link -o "$OUT/unsigned.apk" --manifest AndroidManifest.xml -I "$JAR" \
    --min-sdk-version 26 --target-sdk-version 34 --version-code 1 --version-name 1.0
javac -nowarn -source 8 -target 8 -classpath "$JAR" -d "$OUT/classes" $(find src -name '*.java') 2>&1 | grep -v "bootstrap class path\|^1 warning\|^Note:" || true
"$BT/d8" --release --min-api 26 --lib "$JAR" --output "$OUT/dex" $(find "$OUT/classes" -name '*.class')
(cd "$OUT/dex" && "$BT/aapt" add ../unsigned.apk classes.dex > /dev/null)
"$BT/zipalign" -f 4 "$OUT/unsigned.apk" "$OUT/aligned.apk"

# A local signing key, created on first use and never committed. A build
# signed with another key cannot update an installed one; Aetheron then
# removes the old home screen and installs the new one.
KS="${KEYSTORE:-$HOME/.aetheron-launcher.keystore}"
[ -f "$KS" ] || keytool -genkeypair -keystore "$KS" -storepass android -keypass android -alias aetheron \
    -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=Aetheron Launcher" > /dev/null 2>&1
"$BT/apksigner" sign --ks "$KS" --ks-pass pass:android --key-pass pass:android --out ../apps/AetheronLauncher.apk "$OUT/aligned.apk"
rm -f ../apps/AetheronLauncher.apk.idsig
"$BT/apksigner" verify ../apps/AetheronLauncher.apk && ls -l ../apps/AetheronLauncher.apk
