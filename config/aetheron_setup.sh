# Typed into Android's console shell once after Android has booted (and again
# whenever this file changes). Edit freely; lines starting with # are skipped.
# Turn it off with first_boot_setup = off in aetheron.ini. To undo everything,
# use Aetheron menu -> Factory reset.

# Built-in apps that keep their icon on the home screen. Every other built-in
# app with an icon is switched off (not deleted; apps you install are left alone): `pm enable <package>` brings one back.
# com.android.vending is the Play Store; it only exists if your Android image
# ships Google apps. com.android.documentsui is the file picker other apps use.
KEEP="com.android.settings com.android.vending com.android.documentsui"
for p in $(cmd package query-activities --brief -a android.intent.action.MAIN -c android.intent.category.LAUNCHER 2>/dev/null | grep / | cut -d/ -f1 | sort -u); do case " $KEEP " in *" $p "*) ;; *) case "$(pm path $p 2>/dev/null)" in package:/data/*) ;; *) pm disable-user --user 0 $p;; esac;; esac; done

# Look and feel: dark theme, quicker animations, no lock screen, screen stays on.
cmd uimode night yes
settings put global window_animation_scale 0.5
settings put global transition_animation_scale 0.5
settings put global animator_duration_scale 0.5
locksettings set-disabled true
settings put system screen_off_timeout 2147483647

# Back to a fresh home screen.
am start -a android.intent.action.MAIN -c android.intent.category.HOME
