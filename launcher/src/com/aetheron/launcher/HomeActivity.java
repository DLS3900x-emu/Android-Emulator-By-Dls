package com.aetheron.launcher;

import android.app.Activity;
import android.content.BroadcastReceiver;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.graphics.Color;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.StateListDrawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.provider.Settings;
import android.text.Editable;
import android.text.InputType;
import android.text.TextUtils;
import android.text.TextWatcher;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.inputmethod.EditorInfo;
import android.widget.BaseAdapter;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.GridView;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.PopupMenu;
import android.widget.TextClock;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Locale;

/**
 * Aetheron's home screen: a clock, a search box and one grid with every
 * installed app. Tap an app to open it, long-press (or right-click) for
 * "App info" and "Uninstall". There are no pages, widgets or dock on purpose.
 */
public class HomeActivity extends Activity {
    private static final int BACKGROUND_TOP = 0xFF1B1C20, BACKGROUND_BOTTOM = 0xFF0D0D0F;
    private static final int TEXT = 0xFFECEEF2, DIM = 0xFF9A9EA6, FIELD = 0xFF2A2B31, HIGHLIGHT = 0x26FFFFFF;

    private static final class App {
        ComponentName component;
        String label;
        Drawable icon;
    }

    private final List<App> all = new ArrayList<>();
    private final List<App> shown = new ArrayList<>();
    private final AppAdapter adapter = new AppAdapter();
    private EditText search;
    private GridView grid;
    private TextView empty;
    private int loadGeneration;

    private final BroadcastReceiver packagesChanged = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            loadApps();
        }
    };

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        GradientDrawable background = new GradientDrawable(GradientDrawable.Orientation.TOP_BOTTOM,
                new int[] {BACKGROUND_TOP, BACKGROUND_BOTTOM});
        getWindow().setBackgroundDrawable(background);
        getWindow().setStatusBarColor(BACKGROUND_TOP);
        getWindow().setNavigationBarColor(BACKGROUND_BOTTOM);
        setContentView(buildUi());

        IntentFilter filter = new IntentFilter();
        filter.addAction(Intent.ACTION_PACKAGE_ADDED);
        filter.addAction(Intent.ACTION_PACKAGE_REMOVED);
        filter.addAction(Intent.ACTION_PACKAGE_CHANGED);
        filter.addDataScheme("package");
        if (Build.VERSION.SDK_INT >= 33) {
            registerReceiver(packagesChanged, filter, Context.RECEIVER_NOT_EXPORTED);
        } else {
            registerReceiver(packagesChanged, filter);
        }
        loadApps();
    }

    @Override
    protected void onDestroy() {
        unregisterReceiver(packagesChanged);
        super.onDestroy();
    }

    /** Home was pressed while the home screen is already in front: start over. */
    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        if (search.length() > 0) search.setText("");
        grid.setSelection(0);
        grid.requestFocus();
    }

    @Override
    public void onBackPressed() {
        // Home is the bottom of the stack; Back only clears the search.
        if (search.length() > 0) search.setText("");
    }

    private int dp(float value) {
        return Math.round(TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, value, getResources().getDisplayMetrics()));
    }

    private static GradientDrawable rounded(int color, float radius) {
        GradientDrawable d = new GradientDrawable();
        d.setColor(color);
        d.setCornerRadius(radius);
        return d;
    }

    private View buildUi() {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setFitsSystemWindows(true);
        root.setPadding(dp(28), dp(14), dp(28), 0);
        // Takes the initial focus so the keyboard does not land in the search box.
        root.setFocusableInTouchMode(true);

        FrameLayout header = new FrameLayout(this);
        root.addView(header, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(64)));

        LinearLayout clock = new LinearLayout(this);
        clock.setOrientation(LinearLayout.VERTICAL);
        TextClock time = new TextClock(this);
        time.setFormat12Hour("h:mm");
        time.setFormat24Hour("HH:mm");
        time.setTextColor(TEXT);
        time.setTextSize(TypedValue.COMPLEX_UNIT_SP, 26);
        clock.addView(time);
        TextClock date = new TextClock(this);
        date.setFormat12Hour("EEE, d MMM");
        date.setFormat24Hour("EEE, d MMM");
        date.setTextColor(DIM);
        date.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        clock.addView(date);
        header.addView(clock, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.START | Gravity.CENTER_VERTICAL));

        search = new EditText(this);
        search.setHint("Search apps");
        search.setHintTextColor(DIM);
        search.setTextColor(TEXT);
        search.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
        search.setSingleLine(true);
        search.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        search.setImeOptions(EditorInfo.IME_ACTION_SEARCH | EditorInfo.IME_FLAG_NO_EXTRACT_UI);
        search.setBackground(rounded(FIELD, dp(22)));
        search.setPadding(dp(20), 0, dp(20), 0);
        search.addTextChangedListener(new TextWatcher() {
            @Override
            public void beforeTextChanged(CharSequence s, int start, int count, int after) {}

            @Override
            public void onTextChanged(CharSequence s, int start, int before, int count) {}

            @Override
            public void afterTextChanged(Editable s) {
                applyFilter();
            }
        });
        // Enter opens the only match.
        search.setOnEditorActionListener((view, action, event) -> {
            if (shown.size() == 1) launch(shown.get(0));
            return true;
        });
        int searchWidth = Math.min(dp(460), getResources().getDisplayMetrics().widthPixels - dp(320));
        header.addView(search, new FrameLayout.LayoutParams(Math.max(searchWidth, dp(180)), dp(44), Gravity.CENTER));

        FrameLayout body = new FrameLayout(this);
        root.addView(body, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1));

        grid = new GridView(this);
        grid.setNumColumns(GridView.AUTO_FIT);
        grid.setColumnWidth(dp(128));
        grid.setStretchMode(GridView.STRETCH_COLUMN_WIDTH);
        grid.setVerticalSpacing(dp(10));
        grid.setSelector(rounded(Color.TRANSPARENT, 0));
        grid.setPadding(0, dp(22), 0, dp(22));
        grid.setClipToPadding(false);
        grid.setVerticalScrollBarEnabled(false);
        grid.setAdapter(adapter);
        grid.setOnItemClickListener((parent, view, position, id) -> launch(shown.get(position)));
        grid.setOnItemLongClickListener((parent, view, position, id) -> {
            showMenu(view, shown.get(position));
            return true;
        });
        body.addView(grid, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT));

        empty = new TextView(this);
        empty.setTextColor(DIM);
        empty.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
        empty.setVisibility(View.GONE);
        body.addView(empty, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.CENTER));
        return root;
    }

    /** Reads the installed apps off the main thread; icons can be slow to decode. */
    private void loadApps() {
        final int generation = ++loadGeneration;
        new Thread(() -> {
            PackageManager pm = getPackageManager();
            Intent main = new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER);
            List<App> apps = new ArrayList<>();
            for (ResolveInfo info : pm.queryIntentActivities(main, 0)) {
                if (info.activityInfo == null) continue;
                App app = new App();
                app.component = new ComponentName(info.activityInfo.packageName, info.activityInfo.name);
                CharSequence label = info.loadLabel(pm);
                app.label = label == null ? info.activityInfo.packageName : label.toString();
                try {
                    app.icon = info.loadIcon(pm);
                } catch (RuntimeException | OutOfMemoryError e) {
                    app.icon = pm.getDefaultActivityIcon();
                }
                apps.add(app);
            }
            Collections.sort(apps, (a, b) -> a.label.compareToIgnoreCase(b.label));
            runOnUiThread(() -> {
                if (generation != loadGeneration || isDestroyed()) return;  // a newer load is on its way
                all.clear();
                all.addAll(apps);
                applyFilter();
            });
        }, "load-apps").start();
    }

    private void applyFilter() {
        String query = search.getText().toString().trim().toLowerCase(Locale.ROOT);
        shown.clear();
        for (App app : all) {
            if (query.isEmpty() || app.label.toLowerCase(Locale.ROOT).contains(query)) shown.add(app);
        }
        adapter.notifyDataSetChanged();
        empty.setText(all.isEmpty() ? "No apps installed yet" : "No app matches \u201C" + query + "\u201D");
        empty.setVisibility(shown.isEmpty() ? View.VISIBLE : View.GONE);
    }

    private void launch(App app) {
        Intent intent = new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER);
        intent.setComponent(app.component);
        intent.setFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_RESET_TASK_IF_NEEDED);
        try {
            startActivity(intent);
        } catch (RuntimeException e) {
            loadApps();  // the app was removed or disabled in the meantime
        }
    }

    private void showMenu(View anchor, final App app) {
        PopupMenu menu = new PopupMenu(this, anchor);
        menu.getMenu().add(0, 1, 0, "App info");
        menu.getMenu().add(0, 2, 1, "Uninstall");
        menu.setOnMenuItemClickListener(item -> {
            Uri uri = Uri.fromParts("package", app.component.getPackageName(), null);
            Intent intent = item.getItemId() == 1
                    ? new Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, uri)
                    : new Intent(Intent.ACTION_DELETE, uri);
            intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            try {
                startActivity(intent);
            } catch (RuntimeException e) {
                // nothing can handle it on this image; leave the menu closed
            }
            return true;
        });
        menu.show();
    }

    private final class AppAdapter extends BaseAdapter {
        @Override
        public int getCount() {
            return shown.size();
        }

        @Override
        public Object getItem(int position) {
            return shown.get(position);
        }

        @Override
        public long getItemId(int position) {
            return position;
        }

        @Override
        public View getView(int position, View convert, ViewGroup parent) {
            LinearLayout cell = (LinearLayout) convert;
            if (cell == null) {
                cell = new LinearLayout(HomeActivity.this);
                cell.setOrientation(LinearLayout.VERTICAL);
                cell.setGravity(Gravity.CENTER_HORIZONTAL);
                cell.setPadding(dp(8), dp(14), dp(8), dp(12));
                StateListDrawable states = new StateListDrawable();
                states.addState(new int[] {android.R.attr.state_pressed}, rounded(HIGHLIGHT, dp(16)));
                states.addState(new int[] {android.R.attr.state_hovered}, rounded(HIGHLIGHT, dp(16)));
                states.addState(new int[] {android.R.attr.state_focused}, rounded(HIGHLIGHT, dp(16)));
                states.addState(new int[] {}, rounded(Color.TRANSPARENT, dp(16)));
                cell.setBackground(states);

                ImageView icon = new ImageView(HomeActivity.this);
                icon.setScaleType(ImageView.ScaleType.FIT_CENTER);
                cell.addView(icon, new LinearLayout.LayoutParams(dp(64), dp(64)));

                TextView label = new TextView(HomeActivity.this);
                label.setTextColor(TEXT);
                label.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
                label.setGravity(Gravity.CENTER_HORIZONTAL);
                label.setMaxLines(2);
                label.setEllipsize(TextUtils.TruncateAt.END);
                LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                        ViewGroup.LayoutParams.WRAP_CONTENT);
                lp.topMargin = dp(8);
                cell.addView(label, lp);
            }
            App app = shown.get(position);
            ((ImageView) cell.getChildAt(0)).setImageDrawable(app.icon);
            ((TextView) cell.getChildAt(1)).setText(app.label);
            return cell;
        }
    }
}
