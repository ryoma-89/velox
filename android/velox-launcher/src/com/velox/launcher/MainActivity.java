package com.velox.launcher;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.BroadcastReceiver;
import android.content.ClipData;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.res.Configuration;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.Drawable;
import android.media.AudioManager;
import android.net.ConnectivityManager;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.provider.Settings;
import android.text.Editable;
import android.text.TextUtils;
import android.text.TextWatcher;
import android.util.DisplayMetrics;
import android.view.DragEvent;
import android.view.Gravity;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.widget.Button;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.GridLayout;
import android.widget.HorizontalScrollView;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.PopupMenu;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.Date;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/**
 * Velox Launcher — Lightweight native PC-oriented Android desktop HOME Activity.
 * Zero background CPU when hidden, zero ads/telemetry, full keyboard/mouse/touch support.
 */
public final class MainActivity extends Activity {
    private static final int REQ_PICK_WALLPAPER = 4021;

    private VeloxTheme theme;
    private DesktopState state;
    private List<AppEntry> installedApps = new ArrayList<>();
    private final Map<String, AppEntry> appByPkg = new HashMap<>();

    private FrameLayout rootLayout;
    private GridLayout desktopGrid;
    private ScrollView desktopScroll;
    private LinearLayout taskbar;
    private LinearLayout pinnedContainer;
    private LinearLayout recentContainer;
    private HorizontalScrollView recentScroll;
    private TextView netBadge;
    private TextView clockView;
    private TextView dateView;
    private TextView startBtn;
    private int insetTopPx = 0;
    private int insetBottomPx = 0;
    private ConnectivityManager.NetworkCallback networkCallback = null;

    // Popups (only visible on demand)
    private LinearLayout appMenuPanel;
    private EditText searchInput;
    private GridLayout appMenuGrid;
    private LinearLayout categoryBar;
    private LinearLayout sortBar;
    private LinearLayout quickSettingsPanel;
    private LinearLayout volumePanel;
    private LinearLayout folderPanel;
    private String activeCategory = AppEntry.CAT_ALL;
    private String activeSearch = "";

    private boolean receiversRegistered = false;
    private final BroadcastReceiver systemReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            String act = intent.getAction();
            if (Intent.ACTION_TIME_TICK.equals(act) || Intent.ACTION_TIME_CHANGED.equals(act)) {
                updateClock();
            } else if (Intent.ACTION_PACKAGE_ADDED.equals(act)
                    || Intent.ACTION_PACKAGE_REMOVED.equals(act)
                    || Intent.ACTION_PACKAGE_REPLACED.equals(act)) {
                reloadAppsAndRender();
            }
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        requestWindowFeature(Window.FEATURE_NO_TITLE);
        loadAppsAndState();
        buildUi();
        renderAll();
    }

    @Override
    protected void onResume() {
        super.onResume();
        registerReceivers();
        reloadAppsAndRender();
        updateClock();
        updateNetworkBadge();
    }

    @Override
    protected void onPause() {
        super.onPause();
        // Zero background CPU while user is inside apps or games
        unregisterReceivers();
        hideAllPanels();
    }

    @Override
    public void onConfigurationChanged(Configuration newConfig) {
        super.onConfigurationChanged(newConfig);
        // Responsive reflow on resolution, DPI, or orientation change without flicker
        renderAll();
    }

    @Override
    public void onBackPressed() {
        if (anyPanelVisible()) {
            hideAllPanels();
        }
        // Standard HOME activity stays on desktop
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (event.getAction() == KeyEvent.ACTION_DOWN) {
            int code = event.getKeyCode();
            if (code == KeyEvent.KEYCODE_ESCAPE && anyPanelVisible()) {
                hideAllPanels();
                return true;
            }
            if ((code == KeyEvent.KEYCODE_SPACE && event.isCtrlPressed())
                    || (code == KeyEvent.KEYCODE_SLASH && !isVisible(appMenuPanel))) {
                openAppMenu(true);
                return true;
            }
            if (code == KeyEvent.KEYCODE_META_LEFT || code == KeyEvent.KEYCODE_META_RIGHT) {
                toggleAppMenu(false);
                return true;
            }
        }
        return super.dispatchKeyEvent(event);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == REQ_PICK_WALLPAPER && resultCode == RESULT_OK && data != null && data.getData() != null) {
            Uri uri = data.getData();
            File dst = new File(getFilesDir(), "wallpaper_custom.img");
            try (InputStream in = getContentResolver().openInputStream(uri);
                 FileOutputStream out = new FileOutputStream(dst)) {
                if (in != null) {
                    byte[] buf = new byte[8192];
                    int n;
                    while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
                    state.wallpaperId = "custom";
                    state.customWallpaperPath = dst.getAbsolutePath();
                    state.save();
                    applyThemeAndWallpaper();
                }
            } catch (Throwable t) {
                Toast.makeText(this, "Wallpaper load failed: " + t.getMessage(), Toast.LENGTH_SHORT).show();
            }
        }
    }

    private void registerReceivers() {
        if (receiversRegistered) return;
        IntentFilter tf = new IntentFilter();
        tf.addAction(Intent.ACTION_TIME_TICK);
        tf.addAction(Intent.ACTION_TIME_CHANGED);
        registerReceiver(systemReceiver, tf);

        IntentFilter pf = new IntentFilter();
        pf.addAction(Intent.ACTION_PACKAGE_ADDED);
        pf.addAction(Intent.ACTION_PACKAGE_REMOVED);
        pf.addAction(Intent.ACTION_PACKAGE_REPLACED);
        pf.addDataScheme("package");
        registerReceiver(systemReceiver, pf);

        try {
            ConnectivityManager cm = (ConnectivityManager) getSystemService(Context.CONNECTIVITY_SERVICE);
            if (cm != null && networkCallback == null) {
                networkCallback = new ConnectivityManager.NetworkCallback() {
                    @Override public void onAvailable(Network n) { runOnUiThread(() -> updateNetworkBadge()); }
                    @Override public void onLost(Network n) { runOnUiThread(() -> updateNetworkBadge()); }
                    @Override public void onCapabilitiesChanged(Network n, NetworkCapabilities c) { runOnUiThread(() -> updateNetworkBadge()); }
                };
                cm.registerDefaultNetworkCallback(networkCallback);
            }
        } catch (Throwable ignored) {
        }
        receiversRegistered = true;
    }

    private void unregisterReceivers() {
        if (!receiversRegistered) return;
        try {
            unregisterReceiver(systemReceiver);
        } catch (Throwable ignored) {
        }
        try {
            ConnectivityManager cm = (ConnectivityManager) getSystemService(Context.CONNECTIVITY_SERVICE);
            if (cm != null && networkCallback != null) {
                cm.unregisterNetworkCallback(networkCallback);
                networkCallback = null;
            }
        } catch (Throwable ignored) {
        }
        receiversRegistered = false;
    }

    private void loadAppsAndState() {
        Map<String, Long> prevTimes = state != null ? state.launchTimes : null;
        installedApps = AppEntry.loadInstalled(getPackageManager(), getPackageName(), prevTimes);
        appByPkg.clear();
        for (AppEntry e : installedApps) {
            if (!appByPkg.containsKey(e.packageName)) {
                appByPkg.put(e.packageName, e);
            }
        }
        if (state == null) {
            state = DesktopState.load(this, installedApps);
            for (AppEntry e : installedApps) {
                Long ts = state.launchTimes.get(e.packageName);
                if (ts != null) e.lastLaunchedMs = ts;
            }
        }
        theme = VeloxTheme.of(state.darkTheme);
    }

    private void reloadAppsAndRender() {
        loadAppsAndState();
        renderAll();
    }

    private void buildUi() {
        rootLayout = new FrameLayout(this);
        rootLayout.setLayoutParams(new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));

        // Desktop scroll + responsive grid
        desktopScroll = new ScrollView(this);
        desktopScroll.setFillViewport(true);
        desktopScroll.setVerticalScrollBarEnabled(false);
        FrameLayout.LayoutParams dslp = new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT);
        dslp.bottomMargin = VeloxTheme.dp(this, 56);
        desktopScroll.setLayoutParams(dslp);

        desktopGrid = new GridLayout(this);
        int pad = VeloxTheme.dp(this, 16);
        desktopGrid.setPadding(pad, VeloxTheme.dp(this, 24), pad, pad);
        desktopGrid.setOnClickListener(v -> hideAllPanels());
        attachDesktopContextTriggers(desktopGrid);
        desktopScroll.addView(desktopGrid, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        rootLayout.addView(desktopScroll);

        // Bottom PC Taskbar
        taskbar = new LinearLayout(this);
        taskbar.setOrientation(LinearLayout.HORIZONTAL);
        taskbar.setGravity(Gravity.CENTER_VERTICAL);
        int tbPadH = VeloxTheme.dp(this, 10);
        taskbar.setPadding(tbPadH, VeloxTheme.dp(this, 4), tbPadH, VeloxTheme.dp(this, 4));
        FrameLayout.LayoutParams tblp = new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, VeloxTheme.dp(this, 54), Gravity.BOTTOM);
        taskbar.setLayoutParams(tblp);
        taskbar.setElevation(VeloxTheme.dp(this, 8));

        // Left: Velox Start button + Search button + Pinned apps
        startBtn = makeTaskbarButton("V  " + getString(R.string.apps_menu), true);
        startBtn.setOnClickListener(v -> toggleAppMenu(false));
        taskbar.addView(startBtn);

        TextView searchBtn = makeTaskbarButton("⌕", false);
        searchBtn.setContentDescription(getString(R.string.search_apps_hint));
        searchBtn.setOnClickListener(v -> toggleAppMenu(true));
        taskbar.addView(searchBtn);

        HorizontalScrollView pinScroll = new HorizontalScrollView(this);
        pinScroll.setHorizontalScrollBarEnabled(false);
        pinnedContainer = new LinearLayout(this);
        pinnedContainer.setOrientation(LinearLayout.HORIZONTAL);
        pinnedContainer.setGravity(Gravity.CENTER_VERTICAL);
        pinScroll.addView(pinnedContainer);
        LinearLayout.LayoutParams pslp = new LinearLayout.LayoutParams(
                0, ViewGroup.LayoutParams.MATCH_PARENT, 1.0f);
        pslp.leftMargin = VeloxTheme.dp(this, 6);
        taskbar.addView(pinScroll, pslp);

        // Center: Running / Recent apps strip (hidden on narrow portrait to avoid crowding)
        recentScroll = new HorizontalScrollView(this);
        recentScroll.setHorizontalScrollBarEnabled(false);
        recentContainer = new LinearLayout(this);
        recentContainer.setOrientation(LinearLayout.HORIZONTAL);
        recentContainer.setGravity(Gravity.CENTER_VERTICAL);
        recentScroll.addView(recentContainer);
        LinearLayout.LayoutParams rslp = new LinearLayout.LayoutParams(
                0, ViewGroup.LayoutParams.MATCH_PARENT, 1.0f);
        rslp.leftMargin = VeloxTheme.dp(this, 8);
        rslp.rightMargin = VeloxTheme.dp(this, 8);
        taskbar.addView(recentScroll, rslp);

        // Right: Network, Volume, Quick Settings, Clock
        LinearLayout rightTray = new LinearLayout(this);
        rightTray.setOrientation(LinearLayout.HORIZONTAL);
        rightTray.setGravity(Gravity.CENTER_VERTICAL);

        netBadge = makeTaskbarButton("Wi-Fi", false);
        netBadge.setOnClickListener(v -> openSettingsAction(Settings.ACTION_WIFI_SETTINGS));
        rightTray.addView(netBadge);

        TextView volBtn = makeTaskbarButton("♪", false);
        volBtn.setContentDescription(getString(R.string.volume));
        volBtn.setOnClickListener(v -> toggleVolumePanel());
        rightTray.addView(volBtn);

        TextView qsBtn = makeTaskbarButton("⚙", false);
        qsBtn.setContentDescription(getString(R.string.quick_settings));
        qsBtn.setOnClickListener(v -> toggleQuickSettings());
        rightTray.addView(qsBtn);

        LinearLayout clockBox = new LinearLayout(this);
        clockBox.setOrientation(LinearLayout.VERTICAL);
        clockBox.setGravity(Gravity.END | Gravity.CENTER_VERTICAL);
        clockBox.setPadding(VeloxTheme.dp(this, 10), VeloxTheme.dp(this, 2),
                VeloxTheme.dp(this, 8), VeloxTheme.dp(this, 2));
        clockBox.setFocusable(true);
        clockBox.setOnClickListener(v -> toggleQuickSettings());
        attachHoverFocus(clockBox, false);

        clockView = new TextView(this);
        clockView.setTextSize(13f);
        clockView.setTypeface(Typeface.DEFAULT_BOLD);
        clockBox.addView(clockView);

        dateView = new TextView(this);
        dateView.setTextSize(10f);
        clockBox.addView(dateView);

        rightTray.addView(clockBox);
        taskbar.addView(rightTray);
        rootLayout.addView(taskbar);

        // Popup panels (hidden by default)
        buildAppMenuPanel();
        buildQuickSettingsPanel();
        buildVolumePanel();
        buildFolderPanel();

        setContentView(rootLayout);
        rootLayout.setOnApplyWindowInsetsListener((v, insets) -> {
            if (Build.VERSION.SDK_INT >= 30) {
                android.graphics.Insets sb = insets.getInsets(WindowInsets.Type.systemBars());
                insetTopPx = sb.top;
                insetBottomPx = sb.bottom;
                applyResponsiveTaskbarLayout();
            }
            return insets;
        });
        applySystemBars();
    }

    private void applyResponsiveTaskbarLayout() {
        DisplayMetrics dm = getResources().getDisplayMetrics();
        float widthDp = dm.widthPixels / dm.density;
        boolean compact = widthDp < 540f;

        int tbBaseH = VeloxTheme.dp(this, 54);
        FrameLayout.LayoutParams tblp = (FrameLayout.LayoutParams) taskbar.getLayoutParams();
        tblp.height = tbBaseH + insetBottomPx;
        taskbar.setLayoutParams(tblp);
        int tbPadH = VeloxTheme.dp(this, compact ? 6 : 10);
        taskbar.setPadding(tbPadH, VeloxTheme.dp(this, 4), tbPadH, VeloxTheme.dp(this, 4) + insetBottomPx);

        FrameLayout.LayoutParams dslp = (FrameLayout.LayoutParams) desktopScroll.getLayoutParams();
        dslp.topMargin = insetTopPx;
        dslp.bottomMargin = tblp.height;
        desktopScroll.setLayoutParams(dslp);

        if (recentScroll != null) {
            recentScroll.setVisibility(compact ? View.GONE : View.VISIBLE);
        }
        if (netBadge != null) {
            netBadge.setVisibility(compact ? View.GONE : View.VISIBLE);
        }
        if (startBtn != null) {
            startBtn.setText(compact ? "V" : ("V  " + getString(R.string.apps_menu)));
        }
        int popupBottom = tblp.height + VeloxTheme.dp(this, 8);
        for (LinearLayout p : new LinearLayout[]{appMenuPanel, quickSettingsPanel, volumePanel}) {
            if (p != null) {
                FrameLayout.LayoutParams plp = (FrameLayout.LayoutParams) p.getLayoutParams();
                plp.bottomMargin = popupBottom;
                p.setLayoutParams(plp);
            }
        }
    }

    private void applySystemBars() {
        try {
            getWindow().setStatusBarColor(Color.TRANSPARENT);
            getWindow().setNavigationBarColor(Color.TRANSPARENT);
            if (Build.VERSION.SDK_INT >= 30) {
                WindowInsetsController c = getWindow().getInsetsController();
                if (c != null) {
                    int flags = state.darkTheme ? 0
                            : (WindowInsetsController.APPEARANCE_LIGHT_STATUS_BARS
                            | WindowInsetsController.APPEARANCE_LIGHT_NAVIGATION_BARS);
                    c.setSystemBarsAppearance(flags,
                            WindowInsetsController.APPEARANCE_LIGHT_STATUS_BARS
                                    | WindowInsetsController.APPEARANCE_LIGHT_NAVIGATION_BARS);
                }
            }
        } catch (Throwable ignored) {
        }
    }

    private TextView makeTaskbarButton(String text, boolean accent) {
        TextView tv = new TextView(this);
        tv.setText(text);
        tv.setTextSize(13f);
        tv.setTypeface(accent ? Typeface.DEFAULT_BOLD : Typeface.DEFAULT);
        tv.setGravity(Gravity.CENTER);
        tv.setPadding(VeloxTheme.dp(this, 12), VeloxTheme.dp(this, 7),
                VeloxTheme.dp(this, 12), VeloxTheme.dp(this, 7));
        tv.setFocusable(true);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.rightMargin = VeloxTheme.dp(this, 4);
        tv.setLayoutParams(lp);
        tv.setTag(accent ? "accent_btn" : "tray_btn");
        attachHoverFocus(tv, accent);
        return tv;
    }

    private void attachHoverFocus(View v, boolean accent) {
        v.setOnHoverListener((view, ev) -> {
            int act = ev.getAction();
            if (act == MotionEvent.ACTION_HOVER_ENTER) {
                view.setBackground(VeloxTheme.roundedBox(
                        accent ? theme.accent : theme.hoverBg,
                        theme.focusStroke, VeloxTheme.dp(this, 1), VeloxTheme.dp(this, 8)));
            } else if (act == MotionEvent.ACTION_HOVER_EXIT && !view.hasFocus()) {
                resetInteractiveBg(view, accent);
            }
            return false;
        });
        v.setOnFocusChangeListener((view, hasFocus) -> {
            if (hasFocus) {
                view.setBackground(VeloxTheme.roundedBox(
                        accent ? theme.accent : theme.hoverBg,
                        theme.focusStroke, VeloxTheme.dp(this, 1.5f), VeloxTheme.dp(this, 8)));
            } else {
                resetInteractiveBg(view, accent);
            }
        });
    }

    private void resetInteractiveBg(View view, boolean accent) {
        if (accent) {
            view.setBackground(VeloxTheme.roundedBox(
                    theme.accentSoft, theme.accent, VeloxTheme.dp(this, 1), VeloxTheme.dp(this, 8)));
        } else {
            view.setBackground(null);
        }
    }

    private void applyThemeAndWallpaper() {
        theme = VeloxTheme.of(state.darkTheme);
        rootLayout.setBackground(VeloxTheme.buildWallpaper(this, state.wallpaperId, state.customWallpaperPath));
        taskbar.setBackgroundColor(theme.taskbarBg);
        clockView.setTextColor(theme.textPrimary);
        dateView.setTextColor(theme.textSecondary);
        styleTaskbarChildren(taskbar);
        stylePanel(appMenuPanel);
        stylePanel(quickSettingsPanel);
        stylePanel(volumePanel);
        stylePanel(folderPanel);
        if (searchInput != null) {
            searchInput.setTextColor(theme.textPrimary);
            searchInput.setHintTextColor(theme.textSecondary);
            searchInput.setBackground(VeloxTheme.roundedBox(
                    theme.chipBg, theme.border, VeloxTheme.dp(this, 1), VeloxTheme.dp(this, 8)));
        }
        applySystemBars();
    }

    private void styleTaskbarChildren(ViewGroup vg) {
        for (int i = 0; i < vg.getChildCount(); i++) {
            View c = vg.getChildAt(i);
            if (c instanceof TextView) {
                TextView tv = (TextView) c;
                boolean acc = "accent_btn".equals(tv.getTag());
                tv.setTextColor(theme.textPrimary);
                resetInteractiveBg(tv, acc);
            } else if (c instanceof ViewGroup) {
                styleTaskbarChildren((ViewGroup) c);
            }
        }
    }

    private void stylePanel(LinearLayout panel) {
        if (panel == null) return;
        panel.setBackground(VeloxTheme.roundedBox(
                theme.panelBg, theme.border, VeloxTheme.dp(this, 1), VeloxTheme.dp(this, 14)));
    }

    private void renderAll() {
        applyThemeAndWallpaper();
        applyResponsiveTaskbarLayout();
        renderDesktopGrid();
        renderTaskbarPins();
        renderTaskbarRecent();
        if (isVisible(appMenuPanel)) {
            renderAppMenuContent();
        }
        updateClock();
        updateNetworkBadge();
    }

    // ---- Desktop Grid (Drag & Drop + Folders + Right-click + Keyboard) -----

    private void renderDesktopGrid() {
        desktopGrid.removeAllViews();
        DisplayMetrics dm = getResources().getDisplayMetrics();
        float widthDp = dm.widthPixels / dm.density;
        int tileDp = widthDp >= 900 ? 104 : (widthDp >= 600 ? 96 : 84);
        int cols = Math.max(3, Math.min(10, (int) ((widthDp - 32f) / tileDp)));
        desktopGrid.setColumnCount(cols);

        int tileW = VeloxTheme.dp(this, tileDp);
        int tileH = VeloxTheme.dp(this, tileDp + 6);
        int margin = VeloxTheme.dp(this, 4);

        for (int idx = 0; idx < state.desktopItems.size(); idx++) {
            DesktopState.DesktopItem item = state.desktopItems.get(idx);
            View tile = buildDesktopTile(item, idx, tileW, tileH);
            if (tile == null) continue;
            GridLayout.LayoutParams glp = new GridLayout.LayoutParams();
            glp.width = tileW;
            glp.height = tileH;
            glp.setMargins(margin, margin, margin, margin);
            desktopGrid.addView(tile, glp);
        }
    }

    private View buildDesktopTile(DesktopState.DesktopItem item, int index, int w, int h) {
        AppEntry entry = item.isFolder ? null : appByPkg.get(item.packageName);
        if (!item.isFolder && entry == null) return null;

        LinearLayout tile = new LinearLayout(this);
        tile.setOrientation(LinearLayout.VERTICAL);
        tile.setGravity(Gravity.CENTER);
        tile.setPadding(VeloxTheme.dp(this, 6), VeloxTheme.dp(this, 8),
                VeloxTheme.dp(this, 6), VeloxTheme.dp(this, 6));
        tile.setFocusable(true);
        tile.setClickable(true);
        attachHoverFocus(tile, false);

        int iconPx = VeloxTheme.dp(this, 44);
        if (item.isFolder) {
            FrameLayout folderBox = new FrameLayout(this);
            folderBox.setBackground(VeloxTheme.roundedBox(
                    theme.accentSoft, theme.accent, VeloxTheme.dp(this, 1), VeloxTheme.dp(this, 12)));
            TextView badge = new TextView(this);
            badge.setText(String.valueOf(item.folderPackages.size()));
            badge.setTextColor(theme.textPrimary);
            badge.setTextSize(16f);
            badge.setTypeface(Typeface.DEFAULT_BOLD);
            badge.setGravity(Gravity.CENTER);
            folderBox.addView(badge, new FrameLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
            tile.addView(folderBox, new LinearLayout.LayoutParams(iconPx, iconPx));
        } else {
            ImageView iv = new ImageView(this);
            iv.setImageDrawable(entry.icon);
            iv.setScaleType(ImageView.ScaleType.FIT_CENTER);
            tile.addView(iv, new LinearLayout.LayoutParams(iconPx, iconPx));
        }

        TextView label = new TextView(this);
        label.setText(item.isFolder ? item.name : entry.label);
        label.setTextColor(state.darkTheme ? Color.parseColor("#F1F5F9") : theme.textPrimary);
        label.setTextSize(12f);
        label.setMaxLines(2);
        label.setEllipsize(TextUtils.TruncateAt.END);
        label.setGravity(Gravity.CENTER);
        label.setShadowLayer(3f, 0f, 1f, theme.iconShadow);
        LinearLayout.LayoutParams llp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        llp.topMargin = VeloxTheme.dp(this, 5);
        tile.addView(label, llp);

        tile.setOnClickListener(v -> {
            hideAllPanels();
            if (item.isFolder) {
                openFolderPanel(item);
            } else {
                DesktopWindowHelper.launchApp(this, entry, state, DesktopWindowHelper.MODE_AUTO);
                renderTaskbarRecent();
            }
        });

        // Keyboard shortcuts on focused desktop icon
        tile.setOnKeyListener((v, keyCode, ev) -> {
            if (ev.getAction() != KeyEvent.ACTION_DOWN) return false;
            if (keyCode == KeyEvent.KEYCODE_FORWARD_DEL || keyCode == KeyEvent.KEYCODE_DEL) {
                if (item.isFolder) {
                    dissolveFolder(item);
                } else {
                    state.unpinFromDesktop(item.packageName);
                    renderDesktopGrid();
                }
                return true;
            }
            if (keyCode == KeyEvent.KEYCODE_MENU
                    || (keyCode == KeyEvent.KEYCODE_F10 && ev.isShiftPressed())) {
                showDesktopItemMenu(v, item, entry);
                return true;
            }
            return false;
        });

        // Right-click (native mouse / VirtioTablet)
        attachRightClick(tile, () -> showDesktopItemMenu(tile, item, entry));

        // Long-press: start Drag & Drop reordering / folder merge (or menu if touch user cancels drag)
        tile.setOnLongClickListener(v -> {
            ClipData clip = ClipData.newPlainText("velox_item", String.valueOf(index));
            View.DragShadowBuilder shadow = new View.DragShadowBuilder(v);
            v.startDragAndDrop(clip, shadow, index, 0);
            return true;
        });

        // Drop target: reorder or merge into folder
        tile.setOnDragListener((v, event) -> {
            switch (event.getAction()) {
                case DragEvent.ACTION_DRAG_ENTERED:
                    v.setBackground(VeloxTheme.roundedBox(
                            theme.accentSoft, theme.accent, VeloxTheme.dp(this, 2), VeloxTheme.dp(this, 10)));
                    return true;
                case DragEvent.ACTION_DRAG_EXITED:
                case DragEvent.ACTION_DRAG_ENDED:
                    v.setBackground(null);
                    return true;
                case DragEvent.ACTION_DROP:
                    Object ls = event.getLocalState();
                    if (ls instanceof Integer) {
                        int from = (Integer) ls;
                        handleDesktopDrop(from, index);
                    }
                    return true;
                default:
                    return true;
            }
        });

        return tile;
    }

    private void handleDesktopDrop(int from, int to) {
        if (from == to || from < 0 || to < 0
                || from >= state.desktopItems.size() || to >= state.desktopItems.size()) {
            return;
        }
        DesktopState.DesktopItem src = state.desktopItems.get(from);
        DesktopState.DesktopItem dst = state.desktopItems.get(to);
        if (!src.isFolder && dst.isFolder) {
            if (!dst.folderPackages.contains(src.packageName)) {
                dst.folderPackages.add(src.packageName);
            }
            state.desktopItems.remove(from);
            state.save();
            renderDesktopGrid();
            return;
        }
        // Reorder cleanly
        state.desktopItems.remove(from);
        int insertAt = Math.min(to, state.desktopItems.size());
        state.desktopItems.add(insertAt, src);
        state.save();
        renderDesktopGrid();
    }

    private void attachRightClick(View v, Runnable onContext) {
        v.setOnGenericMotionListener((view, ev) -> {
            if (ev.getAction() == MotionEvent.ACTION_BUTTON_PRESS
                    && ev.getActionButton() == MotionEvent.BUTTON_SECONDARY) {
                onContext.run();
                return true;
            }
            return false;
        });
        v.setOnContextClickListener(view -> {
            onContext.run();
            return true;
        });
    }

    private void attachDesktopContextTriggers(View v) {
        attachRightClick(v, () -> showEmptyDesktopMenu(v));
        v.setOnLongClickListener(view -> {
            showEmptyDesktopMenu(view);
            return true;
        });
    }

    private void showDesktopItemMenu(View anchor, DesktopState.DesktopItem item, AppEntry entry) {
        PopupMenu pm = new PopupMenu(this, anchor);
        if (item.isFolder) {
            pm.getMenu().add(0, 1, 0, getString(R.string.rename_folder));
            pm.getMenu().add(0, 2, 1, getString(R.string.dissolve_folder));
            pm.setOnMenuItemClickListener(mi -> {
                if (mi.getItemId() == 1) promptRenameFolder(item);
                else if (mi.getItemId() == 2) dissolveFolder(item);
                return true;
            });
        } else if (entry != null) {
            pm.getMenu().add(0, 1, 0, getString(R.string.open_windowed));
            pm.getMenu().add(0, 2, 1, getString(R.string.open_fullscreen));
            pm.getMenu().add(0, 3, 2, getString(R.string.snap_left));
            pm.getMenu().add(0, 4, 3, getString(R.string.snap_right));
            boolean pinnedTb = state.taskbarPins.contains(entry.packageName);
            pm.getMenu().add(0, 5, 4, getString(pinnedTb ? R.string.unpin_taskbar : R.string.pin_taskbar));
            pm.getMenu().add(0, 6, 5, getString(R.string.add_to_folder));
            pm.getMenu().add(0, 7, 6, getString(R.string.unpin_desktop));
            pm.getMenu().add(0, 8, 7, getString(R.string.app_info));
            if (!entry.isSystem) {
                pm.getMenu().add(0, 9, 8, getString(R.string.uninstall));
            }
            pm.setOnMenuItemClickListener(mi -> {
                switch (mi.getItemId()) {
                    case 1:
                        DesktopWindowHelper.launchApp(this, entry, state, DesktopWindowHelper.MODE_WINDOWED);
                        renderTaskbarRecent();
                        return true;
                    case 2:
                        DesktopWindowHelper.launchApp(this, entry, state, DesktopWindowHelper.MODE_FULLSCREEN);
                        renderTaskbarRecent();
                        return true;
                    case 3:
                        DesktopWindowHelper.launchApp(this, entry, state, DesktopWindowHelper.MODE_SNAP_LEFT);
                        renderTaskbarRecent();
                        return true;
                    case 4:
                        DesktopWindowHelper.launchApp(this, entry, state, DesktopWindowHelper.MODE_SNAP_RIGHT);
                        renderTaskbarRecent();
                        return true;
                    case 5:
                        if (pinnedTb) state.unpinFromTaskbar(entry.packageName);
                        else state.pinToTaskbar(entry.packageName);
                        renderTaskbarPins();
                        return true;
                    case 6:
                        promptAddToFolder(entry.packageName);
                        return true;
                    case 7:
                        state.unpinFromDesktop(entry.packageName);
                        renderDesktopGrid();
                        return true;
                    case 8:
                        DesktopWindowHelper.openAppInfo(this, entry.packageName);
                        return true;
                    case 9:
                        DesktopWindowHelper.requestUninstall(this, entry.packageName);
                        return true;
                    default:
                        return false;
                }
            });
        }
        pm.show();
    }

    private void showEmptyDesktopMenu(View anchor) {
        PopupMenu pm = new PopupMenu(this, anchor);
        pm.getMenu().add(0, 1, 0, getString(R.string.new_folder));
        pm.getMenu().add(0, 2, 1, getString(R.string.change_wallpaper));
        pm.getMenu().add(0, 3, 2, getString(R.string.toggle_theme));
        pm.getMenu().add(0, 4, 3, getString(state.windowedLaunch ? R.string.desktop_mode_on : R.string.desktop_mode_off));
        pm.getMenu().add(0, 5, 4, getString(R.string.sort_desktop));
        pm.getMenu().add(0, 6, 5, getString(R.string.display_settings));
        pm.getMenu().add(0, 7, 6, getString(R.string.switch_stock_launcher));
        pm.setOnMenuItemClickListener(mi -> {
            switch (mi.getItemId()) {
                case 1:
                    promptNewFolder();
                    return true;
                case 2:
                    showWallpaperDialog();
                    return true;
                case 3:
                    state.darkTheme = !state.darkTheme;
                    state.save();
                    renderAll();
                    return true;
                case 4:
                    state.windowedLaunch = !state.windowedLaunch;
                    state.save();
                    Toast.makeText(this,
                            getString(state.windowedLaunch ? R.string.desktop_mode_on : R.string.desktop_mode_off),
                            Toast.LENGTH_SHORT).show();
                    return true;
                case 5:
                    sortDesktopIcons();
                    return true;
                case 6:
                    openSettingsAction(Settings.ACTION_DISPLAY_SETTINGS);
                    return true;
                case 7:
                    DesktopWindowHelper.openHomeChooser(this);
                    return true;
                default:
                    return false;
            }
        });
        pm.show();
    }

    private void sortDesktopIcons() {
        Collections.sort(state.desktopItems, (a, b) -> {
            if (a.isFolder != b.isFolder) return a.isFolder ? -1 : 1;
            String la = a.isFolder ? a.name : (appByPkg.containsKey(a.packageName) ? appByPkg.get(a.packageName).label : a.packageName);
            String lb = b.isFolder ? b.name : (appByPkg.containsKey(b.packageName) ? appByPkg.get(b.packageName).label : b.packageName);
            return la.toLowerCase(Locale.ROOT).compareTo(lb.toLowerCase(Locale.ROOT));
        });
        state.save();
        renderDesktopGrid();
    }

    private void promptNewFolder() {
        EditText input = new EditText(this);
        input.setHint(getString(R.string.folder_default_name));
        new AlertDialog.Builder(this)
                .setTitle(R.string.new_folder)
                .setView(input)
                .setPositiveButton(R.string.ok, (d, w) -> {
                    String name = input.getText().toString().trim();
                    if (name.isEmpty()) name = getString(R.string.folder_default_name);
                    state.desktopItems.add(DesktopState.DesktopItem.folder(null, name, null));
                    state.save();
                    renderDesktopGrid();
                })
                .setNegativeButton(R.string.cancel, null)
                .show();
    }

    private void promptRenameFolder(DesktopState.DesktopItem folder) {
        EditText input = new EditText(this);
        input.setText(folder.name);
        new AlertDialog.Builder(this)
                .setTitle(R.string.rename_folder)
                .setView(input)
                .setPositiveButton(R.string.ok, (d, w) -> {
                    String name = input.getText().toString().trim();
                    if (!name.isEmpty()) {
                        folder.name = name;
                        state.save();
                        renderDesktopGrid();
                    }
                })
                .setNegativeButton(R.string.cancel, null)
                .show();
    }

    private void dissolveFolder(DesktopState.DesktopItem folder) {
        for (String p : folder.folderPackages) {
            if (!state.hasDesktopApp(p)) {
                state.desktopItems.add(DesktopState.DesktopItem.app(p));
            }
        }
        state.desktopItems.remove(folder);
        state.save();
        hideAllPanels();
        renderDesktopGrid();
    }

    private void promptAddToFolder(String pkg) {
        List<DesktopState.DesktopItem> folders = new ArrayList<>();
        for (DesktopState.DesktopItem it : state.desktopItems) {
            if (it.isFolder) folders.add(it);
        }
        String[] labels = new String[folders.size() + 1];
        for (int i = 0; i < folders.size(); i++) labels[i] = folders.get(i).name;
        labels[folders.size()] = "+ " + getString(R.string.new_folder);

        new AlertDialog.Builder(this)
                .setTitle(R.string.add_to_folder)
                .setItems(labels, (d, which) -> {
                    if (which < folders.size()) {
                        DesktopState.DesktopItem f = folders.get(which);
                        if (!f.folderPackages.contains(pkg)) f.folderPackages.add(pkg);
                        state.unpinFromDesktop(pkg);
                        state.save();
                        renderDesktopGrid();
                    } else {
                        List<String> init = new ArrayList<>();
                        init.add(pkg);
                        state.unpinFromDesktop(pkg);
                        state.desktopItems.add(DesktopState.DesktopItem.folder(
                                null, getString(R.string.folder_default_name), init));
                        state.save();
                        renderDesktopGrid();
                    }
                })
                .show();
    }

    private void showWallpaperDialog() {
        boolean isJa = Locale.getDefault().getLanguage().startsWith("ja");
        String[] names = isJa ? VeloxTheme.WALLPAPER_NAMES_JA : VeloxTheme.WALLPAPER_NAMES_EN;
        String[] choices = new String[names.length + 1];
        System.arraycopy(names, 0, choices, 0, names.length);
        choices[names.length] = getString(R.string.wallpaper_pick_image);

        new AlertDialog.Builder(this)
                .setTitle(R.string.change_wallpaper)
                .setItems(choices, (d, which) -> {
                    if (which < VeloxTheme.WALLPAPER_IDS.length) {
                        state.wallpaperId = VeloxTheme.WALLPAPER_IDS[which];
                        if ("daylight_mist".equals(state.wallpaperId)) {
                            state.darkTheme = false;
                        }
                        state.save();
                        renderAll();
                    } else {
                        Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                        pick.addCategory(Intent.CATEGORY_OPENABLE);
                        pick.setType("image/*");
                        try {
                            startActivityForResult(pick, REQ_PICK_WALLPAPER);
                        } catch (Throwable t) {
                            Toast.makeText(this, t.getMessage(), Toast.LENGTH_SHORT).show();
                        }
                    }
                })
                .show();
    }

    // ---- Taskbar Pinned & Recent Strips ------------------------------------

    private void renderTaskbarPins() {
        pinnedContainer.removeAllViews();
        int iconSz = VeloxTheme.dp(this, 30);
        int pad = VeloxTheme.dp(this, 6);
        for (String pkg : state.taskbarPins) {
            AppEntry entry = appByPkg.get(pkg);
            if (entry == null) continue;
            FrameLayout wrap = new FrameLayout(this);
            wrap.setPadding(pad, pad, pad, pad);
            wrap.setFocusable(true);
            wrap.setTooltipText(entry.label);
            attachHoverFocus(wrap, false);

            ImageView iv = new ImageView(this);
            iv.setImageDrawable(entry.icon);
            iv.setScaleType(ImageView.ScaleType.FIT_CENTER);
            wrap.addView(iv, new FrameLayout.LayoutParams(iconSz, iconSz, Gravity.CENTER));

            wrap.setOnClickListener(v -> {
                hideAllPanels();
                DesktopWindowHelper.launchApp(this, entry, state, DesktopWindowHelper.MODE_AUTO);
                renderTaskbarRecent();
            });
            attachRightClick(wrap, () -> showTaskbarPinMenu(wrap, entry));
            wrap.setOnLongClickListener(v -> {
                showTaskbarPinMenu(wrap, entry);
                return true;
            });

            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            lp.rightMargin = VeloxTheme.dp(this, 3);
            pinnedContainer.addView(wrap, lp);
        }
    }

    private void showTaskbarPinMenu(View anchor, AppEntry entry) {
        PopupMenu pm = new PopupMenu(this, anchor);
        pm.getMenu().add(0, 1, 0, getString(R.string.open_windowed));
        pm.getMenu().add(0, 2, 1, getString(R.string.open_fullscreen));
        pm.getMenu().add(0, 3, 2, getString(R.string.unpin_taskbar));
        pm.getMenu().add(0, 4, 3, getString(R.string.app_info));
        pm.setOnMenuItemClickListener(mi -> {
            switch (mi.getItemId()) {
                case 1:
                    DesktopWindowHelper.launchApp(this, entry, state, DesktopWindowHelper.MODE_WINDOWED);
                    renderTaskbarRecent();
                    return true;
                case 2:
                    DesktopWindowHelper.launchApp(this, entry, state, DesktopWindowHelper.MODE_FULLSCREEN);
                    renderTaskbarRecent();
                    return true;
                case 3:
                    state.unpinFromTaskbar(entry.packageName);
                    renderTaskbarPins();
                    return true;
                case 4:
                    DesktopWindowHelper.openAppInfo(this, entry.packageName);
                    return true;
                default:
                    return false;
            }
        });
        pm.show();
    }

    private void renderTaskbarRecent() {
        recentContainer.removeAllViews();
        // Honest policy: use real UsageStats when permitted; otherwise clearly display Session Recent
        List<String> usagePkgs = DesktopWindowHelper.queryRealRecentPackages(this, getPackageName());
        boolean realUsage = !usagePkgs.isEmpty();
        List<String> pkgs = realUsage ? usagePkgs : state.sessionRecent;

        if (pkgs.isEmpty()) return;

        TextView tag = new TextView(this);
        tag.setText(getString(realUsage ? R.string.running_apps_note : R.string.recent_apps_note));
        tag.setTextColor(theme.textSecondary);
        tag.setTextSize(10f);
        tag.setPadding(0, 0, VeloxTheme.dp(this, 6), 0);
        recentContainer.addView(tag);

        int iconSz = VeloxTheme.dp(this, 22);
        for (String pkg : pkgs) {
            AppEntry entry = appByPkg.get(pkg);
            if (entry == null) continue;
            LinearLayout chip = new LinearLayout(this);
            chip.setOrientation(LinearLayout.HORIZONTAL);
            chip.setGravity(Gravity.CENTER_VERTICAL);
            chip.setPadding(VeloxTheme.dp(this, 8), VeloxTheme.dp(this, 4),
                    VeloxTheme.dp(this, 10), VeloxTheme.dp(this, 4));
            chip.setBackground(VeloxTheme.roundedBox(
                    theme.chipBg, theme.border, VeloxTheme.dp(this, 1), VeloxTheme.dp(this, 8)));
            chip.setFocusable(true);
            chip.setOnClickListener(v -> {
                hideAllPanels();
                DesktopWindowHelper.launchApp(this, entry, state, DesktopWindowHelper.MODE_AUTO);
            });

            ImageView iv = new ImageView(this);
            iv.setImageDrawable(entry.icon);
            chip.addView(iv, new LinearLayout.LayoutParams(iconSz, iconSz));

            TextView tv = new TextView(this);
            tv.setText(entry.label);
            tv.setTextColor(theme.textPrimary);
            tv.setTextSize(11.5f);
            tv.setMaxLines(1);
            tv.setEllipsize(TextUtils.TruncateAt.END);
            LinearLayout.LayoutParams tlp = new LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            tlp.leftMargin = VeloxTheme.dp(this, 6);
            tv.setMaxWidth(VeloxTheme.dp(this, 96));
            chip.addView(tv, tlp);

            LinearLayout.LayoutParams clp = new LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            clp.rightMargin = VeloxTheme.dp(this, 6);
            recentContainer.addView(chip, clp);
        }
    }

    // ---- App Menu (Start Menu) Panel ---------------------------------------

    private void buildAppMenuPanel() {
        appMenuPanel = new LinearLayout(this);
        appMenuPanel.setOrientation(LinearLayout.VERTICAL);
        int pad = VeloxTheme.dp(this, 14);
        appMenuPanel.setPadding(pad, pad, pad, pad);
        appMenuPanel.setElevation(VeloxTheme.dp(this, 16));
        appMenuPanel.setVisibility(View.GONE);
        appMenuPanel.setClickable(true);

        searchInput = new EditText(this);
        searchInput.setHint(R.string.search_apps_hint);
        searchInput.setSingleLine(true);
        searchInput.setTextSize(14f);
        searchInput.setPadding(VeloxTheme.dp(this, 12), VeloxTheme.dp(this, 8),
                VeloxTheme.dp(this, 12), VeloxTheme.dp(this, 8));
        searchInput.addTextChangedListener(new TextWatcher() {
            @Override public void beforeTextChanged(CharSequence s, int a, int b, int c) {}
            @Override public void onTextChanged(CharSequence s, int a, int b, int c) {
                activeSearch = s == null ? "" : s.toString().trim().toLowerCase(Locale.ROOT);
                renderAppMenuGrid();
            }
            @Override public void afterTextChanged(Editable s) {}
        });
        appMenuPanel.addView(searchInput, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        // Category + Sort rows
        categoryBar = new LinearLayout(this);
        categoryBar.setOrientation(LinearLayout.HORIZONTAL);
        LinearLayout.LayoutParams cblp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        cblp.topMargin = VeloxTheme.dp(this, 10);
        HorizontalScrollView catScroll = new HorizontalScrollView(this);
        catScroll.setHorizontalScrollBarEnabled(false);
        catScroll.addView(categoryBar);
        appMenuPanel.addView(catScroll, cblp);

        sortBar = new LinearLayout(this);
        sortBar.setOrientation(LinearLayout.HORIZONTAL);
        LinearLayout.LayoutParams sblp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        sblp.topMargin = VeloxTheme.dp(this, 6);
        sblp.bottomMargin = VeloxTheme.dp(this, 8);
        appMenuPanel.addView(sortBar, sblp);

        ScrollView scroll = new ScrollView(this);
        appMenuGrid = new GridLayout(this);
        scroll.addView(appMenuGrid);
        appMenuPanel.addView(scroll, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1.0f));

        FrameLayout.LayoutParams flp = new FrameLayout.LayoutParams(
                VeloxTheme.dp(this, 520), VeloxTheme.dp(this, 500), Gravity.BOTTOM | Gravity.START);
        flp.leftMargin = VeloxTheme.dp(this, 12);
        flp.bottomMargin = VeloxTheme.dp(this, 62);
        rootLayout.addView(appMenuPanel, flp);
    }

    private void toggleAppMenu(boolean focusSearch) {
        if (isVisible(appMenuPanel)) {
            hideAllPanels();
        } else {
            openAppMenu(focusSearch);
        }
    }

    private void openAppMenu(boolean focusSearch) {
        hideAllPanels();
        // Adapt panel size cleanly to current display (including portrait & 720p)
        DisplayMetrics dm = getResources().getDisplayMetrics();
        int maxW = Math.min(VeloxTheme.dp(this, 560), dm.widthPixels - VeloxTheme.dp(this, 24));
        int maxH = Math.min(VeloxTheme.dp(this, 520), dm.heightPixels - VeloxTheme.dp(this, 96));
        FrameLayout.LayoutParams flp = (FrameLayout.LayoutParams) appMenuPanel.getLayoutParams();
        flp.width = maxW;
        flp.height = maxH;
        appMenuPanel.setLayoutParams(flp);
        renderAppMenuContent();
        appMenuPanel.setVisibility(View.VISIBLE);
        if (focusSearch) {
            searchInput.requestFocus();
        }
    }

    private void renderAppMenuContent() {
        renderChips(categoryBar, new String[][]{
                {AppEntry.CAT_ALL, getString(R.string.cat_all)},
                {AppEntry.CAT_GAMES, getString(R.string.cat_games)},
                {AppEntry.CAT_TOOLS, getString(R.string.cat_tools)},
                {AppEntry.CAT_MEDIA, getString(R.string.cat_media)},
                {AppEntry.CAT_SYSTEM, getString(R.string.cat_system)},
        }, activeCategory, id -> {
            activeCategory = id;
            renderAppMenuContent();
        });
        renderChips(sortBar, new String[][]{
                {"name", getString(R.string.sort_name)},
                {"recent", getString(R.string.sort_recent)},
                {"category", getString(R.string.sort_category)},
        }, state.sortMode, id -> {
            state.sortMode = id;
            state.save();
            renderAppMenuContent();
        });
        renderAppMenuGrid();
    }

    private interface StrCb { void call(String id); }

    private void renderChips(LinearLayout bar, String[][] items, String selected, StrCb cb) {
        bar.removeAllViews();
        for (String[] it : items) {
            boolean act = it[0].equals(selected);
            TextView tv = new TextView(this);
            tv.setText(it[1]);
            tv.setTextSize(11.5f);
            tv.setTextColor(act ? theme.chipActiveText : theme.textSecondary);
            tv.setPadding(VeloxTheme.dp(this, 10), VeloxTheme.dp(this, 4),
                    VeloxTheme.dp(this, 10), VeloxTheme.dp(this, 4));
            tv.setBackground(VeloxTheme.roundedBox(
                    act ? theme.chipActiveBg : theme.chipBg,
                    theme.border, act ? 0 : VeloxTheme.dp(this, 1), VeloxTheme.dp(this, 12)));
            tv.setOnClickListener(v -> cb.call(it[0]));
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            lp.rightMargin = VeloxTheme.dp(this, 6);
            bar.addView(tv, lp);
        }
    }

    private void renderAppMenuGrid() {
        appMenuGrid.removeAllViews();
        List<AppEntry> filtered = new ArrayList<>();
        for (AppEntry e : installedApps) {
            if (!AppEntry.CAT_ALL.equals(activeCategory) && !activeCategory.equals(e.category)) continue;
            if (!activeSearch.isEmpty()
                    && !e.label.toLowerCase(Locale.ROOT).contains(activeSearch)
                    && !e.packageName.toLowerCase(Locale.ROOT).contains(activeSearch)) continue;
            filtered.add(e);
        }
        if ("recent".equals(state.sortMode)) {
            Collections.sort(filtered, (a, b) -> {
                int c = Long.compare(b.lastLaunchedMs, a.lastLaunchedMs);
                return c != 0 ? c : a.label.compareToIgnoreCase(b.label);
            });
        } else if ("category".equals(state.sortMode)) {
            Collections.sort(filtered, (a, b) -> {
                int c = a.category.compareTo(b.category);
                return c != 0 ? c : a.label.compareToIgnoreCase(b.label);
            });
        } else {
            Collections.sort(filtered, Comparator.comparing(a -> a.label.toLowerCase(Locale.ROOT)));
        }

        if (filtered.isEmpty()) {
            appMenuGrid.setColumnCount(1);
            TextView empty = new TextView(this);
            empty.setText(R.string.no_apps_found);
            empty.setTextColor(theme.textSecondary);
            empty.setPadding(VeloxTheme.dp(this, 12), VeloxTheme.dp(this, 24),
                    VeloxTheme.dp(this, 12), VeloxTheme.dp(this, 24));
            appMenuGrid.addView(empty);
            return;
        }

        int panelW = appMenuPanel.getLayoutParams().width;
        int tileW = VeloxTheme.dp(this, 114);
        int cols = Math.max(2, Math.min(5, (panelW - VeloxTheme.dp(this, 36)) / tileW));
        appMenuGrid.setColumnCount(cols);
        int iconSz = VeloxTheme.dp(this, 38);

        for (AppEntry e : filtered) {
            LinearLayout cell = new LinearLayout(this);
            cell.setOrientation(LinearLayout.VERTICAL);
            cell.setGravity(Gravity.CENTER);
            cell.setPadding(VeloxTheme.dp(this, 6), VeloxTheme.dp(this, 8),
                    VeloxTheme.dp(this, 6), VeloxTheme.dp(this, 8));
            cell.setFocusable(true);
            attachHoverFocus(cell, false);

            ImageView iv = new ImageView(this);
            iv.setImageDrawable(e.icon);
            cell.addView(iv, new LinearLayout.LayoutParams(iconSz, iconSz));

            TextView tv = new TextView(this);
            tv.setText(e.label);
            tv.setTextColor(theme.textPrimary);
            tv.setTextSize(11.5f);
            tv.setMaxLines(2);
            tv.setEllipsize(TextUtils.TruncateAt.END);
            tv.setGravity(Gravity.CENTER);
            LinearLayout.LayoutParams tlp = new LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            tlp.topMargin = VeloxTheme.dp(this, 4);
            cell.addView(tv, tlp);

            cell.setOnClickListener(v -> {
                hideAllPanels();
                DesktopWindowHelper.launchApp(this, e, state, DesktopWindowHelper.MODE_AUTO);
                renderTaskbarRecent();
            });
            attachRightClick(cell, () -> showAppMenuContext(cell, e));
            cell.setOnLongClickListener(v -> {
                showAppMenuContext(cell, e);
                return true;
            });

            GridLayout.LayoutParams glp = new GridLayout.LayoutParams();
            glp.width = tileW;
            glp.height = VeloxTheme.dp(this, 96);
            appMenuGrid.addView(cell, glp);
        }
    }

    private void showAppMenuContext(View anchor, AppEntry entry) {
        PopupMenu pm = new PopupMenu(this, anchor);
        pm.getMenu().add(0, 1, 0, getString(R.string.open_windowed));
        pm.getMenu().add(0, 2, 1, getString(R.string.open_fullscreen));
        pm.getMenu().add(0, 3, 2, getString(state.hasDesktopApp(entry.packageName) ? R.string.unpin_desktop : R.string.pin_desktop));
        pm.getMenu().add(0, 4, 3, getString(state.taskbarPins.contains(entry.packageName) ? R.string.unpin_taskbar : R.string.pin_taskbar));
        pm.getMenu().add(0, 5, 4, getString(R.string.app_info));
        if (!entry.isSystem) {
            pm.getMenu().add(0, 6, 5, getString(R.string.uninstall));
        }
        pm.setOnMenuItemClickListener(mi -> {
            switch (mi.getItemId()) {
                case 1:
                    hideAllPanels();
                    DesktopWindowHelper.launchApp(this, entry, state, DesktopWindowHelper.MODE_WINDOWED);
                    renderTaskbarRecent();
                    return true;
                case 2:
                    hideAllPanels();
                    DesktopWindowHelper.launchApp(this, entry, state, DesktopWindowHelper.MODE_FULLSCREEN);
                    renderTaskbarRecent();
                    return true;
                case 3:
                    if (state.hasDesktopApp(entry.packageName)) state.unpinFromDesktop(entry.packageName);
                    else state.pinToDesktop(entry.packageName);
                    renderDesktopGrid();
                    return true;
                case 4:
                    if (state.taskbarPins.contains(entry.packageName)) state.unpinFromTaskbar(entry.packageName);
                    else state.pinToTaskbar(entry.packageName);
                    renderTaskbarPins();
                    return true;
                case 5:
                    DesktopWindowHelper.openAppInfo(this, entry.packageName);
                    return true;
                case 6:
                    DesktopWindowHelper.requestUninstall(this, entry.packageName);
                    return true;
                default:
                    return false;
            }
        });
        pm.show();
    }

    // ---- Folder Popup Panel ------------------------------------------------

    private void buildFolderPanel() {
        folderPanel = new LinearLayout(this);
        folderPanel.setOrientation(LinearLayout.VERTICAL);
        int pad = VeloxTheme.dp(this, 14);
        folderPanel.setPadding(pad, pad, pad, pad);
        folderPanel.setElevation(VeloxTheme.dp(this, 16));
        folderPanel.setVisibility(View.GONE);
        folderPanel.setClickable(true);
        FrameLayout.LayoutParams flp = new FrameLayout.LayoutParams(
                VeloxTheme.dp(this, 360), ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.CENTER);
        rootLayout.addView(folderPanel, flp);
    }

    private void openFolderPanel(DesktopState.DesktopItem folder) {
        hideAllPanels();
        folderPanel.removeAllViews();
        stylePanel(folderPanel);

        LinearLayout header = new LinearLayout(this);
        header.setOrientation(LinearLayout.HORIZONTAL);
        header.setGravity(Gravity.CENTER_VERTICAL);
        TextView title = new TextView(this);
        title.setText(folder.name);
        title.setTextColor(theme.textPrimary);
        title.setTextSize(15f);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        header.addView(title, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));

        TextView close = new TextView(this);
        close.setText("✕");
        close.setTextColor(theme.textSecondary);
        close.setPadding(VeloxTheme.dp(this, 8), VeloxTheme.dp(this, 4),
                VeloxTheme.dp(this, 8), VeloxTheme.dp(this, 4));
        close.setOnClickListener(v -> hideAllPanels());
        header.addView(close);
        folderPanel.addView(header);

        GridLayout grid = new GridLayout(this);
        grid.setColumnCount(3);
        grid.setPadding(0, VeloxTheme.dp(this, 10), 0, VeloxTheme.dp(this, 6));
        int iconSz = VeloxTheme.dp(this, 38);
        for (String pkg : new ArrayList<>(folder.folderPackages)) {
            AppEntry e = appByPkg.get(pkg);
            if (e == null) continue;
            LinearLayout cell = new LinearLayout(this);
            cell.setOrientation(LinearLayout.VERTICAL);
            cell.setGravity(Gravity.CENTER);
            cell.setPadding(VeloxTheme.dp(this, 6), VeloxTheme.dp(this, 6),
                    VeloxTheme.dp(this, 6), VeloxTheme.dp(this, 6));
            attachHoverFocus(cell, false);

            ImageView iv = new ImageView(this);
            iv.setImageDrawable(e.icon);
            cell.addView(iv, new LinearLayout.LayoutParams(iconSz, iconSz));

            TextView tv = new TextView(this);
            tv.setText(e.label);
            tv.setTextColor(theme.textPrimary);
            tv.setTextSize(11f);
            tv.setMaxLines(1);
            tv.setEllipsize(TextUtils.TruncateAt.END);
            cell.addView(tv);

            cell.setOnClickListener(v -> {
                hideAllPanels();
                DesktopWindowHelper.launchApp(this, e, state, DesktopWindowHelper.MODE_AUTO);
                renderTaskbarRecent();
            });
            attachRightClick(cell, () -> {
                PopupMenu pm = new PopupMenu(this, cell);
                pm.getMenu().add(0, 1, 0, getString(R.string.remove_from_folder));
                pm.setOnMenuItemClickListener(mi -> {
                    folder.folderPackages.remove(pkg);
                    state.pinToDesktop(pkg);
                    state.save();
                    renderDesktopGrid();
                    openFolderPanel(folder);
                    return true;
                });
                pm.show();
            });
            GridLayout.LayoutParams glp = new GridLayout.LayoutParams();
            glp.width = VeloxTheme.dp(this, 106);
            glp.height = VeloxTheme.dp(this, 82);
            grid.addView(cell, glp);
        }
        folderPanel.addView(grid);
        folderPanel.setVisibility(View.VISIBLE);
    }

    // ---- Volume & Quick Settings Panels ------------------------------------

    private void buildVolumePanel() {
        volumePanel = new LinearLayout(this);
        volumePanel.setOrientation(LinearLayout.VERTICAL);
        int pad = VeloxTheme.dp(this, 14);
        volumePanel.setPadding(pad, pad, pad, pad);
        volumePanel.setElevation(VeloxTheme.dp(this, 16));
        volumePanel.setVisibility(View.GONE);
        volumePanel.setClickable(true);
        FrameLayout.LayoutParams flp = new FrameLayout.LayoutParams(
                VeloxTheme.dp(this, 260), ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.BOTTOM | Gravity.END);
        flp.rightMargin = VeloxTheme.dp(this, 12);
        flp.bottomMargin = VeloxTheme.dp(this, 62);
        rootLayout.addView(volumePanel, flp);
    }

    private void toggleVolumePanel() {
        if (isVisible(volumePanel)) {
            hideAllPanels();
            return;
        }
        hideAllPanels();
        volumePanel.removeAllViews();
        stylePanel(volumePanel);

        AudioManager am = (AudioManager) getSystemService(Context.AUDIO_SERVICE);
        int max = am != null ? Math.max(1, am.getStreamMaxVolume(AudioManager.STREAM_MUSIC)) : 15;
        int cur = am != null ? am.getStreamVolume(AudioManager.STREAM_MUSIC) : 8;

        TextView title = new TextView(this);
        title.setText(getString(R.string.volume) + "  (" + Math.round(cur * 100f / max) + "%)");
        title.setTextColor(theme.textPrimary);
        title.setTextSize(13f);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        volumePanel.addView(title);

        SeekBar sb = new SeekBar(this);
        sb.setMax(max);
        sb.setProgress(cur);
        sb.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                if (fromUser && am != null) {
                    am.setStreamVolume(AudioManager.STREAM_MUSIC, progress, 0);
                    title.setText(getString(R.string.volume) + "  (" + Math.round(progress * 100f / max) + "%)");
                }
            }
            @Override public void onStartTrackingTouch(SeekBar seekBar) {}
            @Override public void onStopTrackingTouch(SeekBar seekBar) {}
        });
        LinearLayout.LayoutParams sblp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        sblp.topMargin = VeloxTheme.dp(this, 10);
        sblp.bottomMargin = VeloxTheme.dp(this, 10);
        volumePanel.addView(sb, sblp);

        TextView muteBtn = makeQuickTile(cur == 0 ? getString(R.string.unmute) : getString(R.string.mute));
        muteBtn.setOnClickListener(v -> {
            if (am != null) {
                int now = am.getStreamVolume(AudioManager.STREAM_MUSIC);
                int next = now == 0 ? Math.max(1, max / 2) : 0;
                am.setStreamVolume(AudioManager.STREAM_MUSIC, next, 0);
                sb.setProgress(next);
                title.setText(getString(R.string.volume) + "  (" + Math.round(next * 100f / max) + "%)");
                muteBtn.setText(next == 0 ? getString(R.string.unmute) : getString(R.string.mute));
            }
        });
        volumePanel.addView(muteBtn);
        volumePanel.setVisibility(View.VISIBLE);
    }

    private void buildQuickSettingsPanel() {
        quickSettingsPanel = new LinearLayout(this);
        quickSettingsPanel.setOrientation(LinearLayout.VERTICAL);
        int pad = VeloxTheme.dp(this, 14);
        quickSettingsPanel.setPadding(pad, pad, pad, pad);
        quickSettingsPanel.setElevation(VeloxTheme.dp(this, 16));
        quickSettingsPanel.setVisibility(View.GONE);
        quickSettingsPanel.setClickable(true);
        FrameLayout.LayoutParams flp = new FrameLayout.LayoutParams(
                VeloxTheme.dp(this, 310), ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.BOTTOM | Gravity.END);
        flp.rightMargin = VeloxTheme.dp(this, 12);
        flp.bottomMargin = VeloxTheme.dp(this, 62);
        rootLayout.addView(quickSettingsPanel, flp);
    }

    private void toggleQuickSettings() {
        if (isVisible(quickSettingsPanel)) {
            hideAllPanels();
            return;
        }
        hideAllPanels();
        quickSettingsPanel.removeAllViews();
        stylePanel(quickSettingsPanel);

        TextView title = new TextView(this);
        title.setText(R.string.quick_settings);
        title.setTextColor(theme.textPrimary);
        title.setTextSize(14f);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        LinearLayout.LayoutParams tlp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        tlp.bottomMargin = VeloxTheme.dp(this, 10);
        quickSettingsPanel.addView(title, tlp);

        addQuickAction(getString(R.string.notifications), () -> {
            hideAllPanels();
            DesktopWindowHelper.expandNotifications(this);
        });
        addQuickAction(getString(state.windowedLaunch ? R.string.desktop_mode_on : R.string.desktop_mode_off), () -> {
            state.windowedLaunch = !state.windowedLaunch;
            state.save();
            toggleQuickSettings();
            toggleQuickSettings();
        });
        addQuickAction(getString(R.string.toggle_theme), () -> {
            state.darkTheme = !state.darkTheme;
            state.save();
            renderAll();
            toggleQuickSettings();
        });
        addQuickAction(getString(R.string.change_wallpaper), () -> {
            hideAllPanels();
            showWallpaperDialog();
        });
        addQuickAction(getString(R.string.wifi_settings), () -> openSettingsAction(Settings.ACTION_WIFI_SETTINGS));
        addQuickAction(getString(R.string.display_settings), () -> openSettingsAction(Settings.ACTION_DISPLAY_SETTINGS));
        addQuickAction(getString(R.string.sound_settings), () -> openSettingsAction(Settings.ACTION_SOUND_SETTINGS));
        addQuickAction(getString(R.string.all_settings), () -> openSettingsAction(Settings.ACTION_SETTINGS));
        addQuickAction(getString(R.string.switch_stock_launcher), () -> {
            hideAllPanels();
            DesktopWindowHelper.openHomeChooser(this);
        });

        quickSettingsPanel.setVisibility(View.VISIBLE);
    }

    private void addQuickAction(String label, Runnable action) {
        TextView btn = makeQuickTile(label);
        btn.setOnClickListener(v -> action.run());
        quickSettingsPanel.addView(btn);
    }

    private TextView makeQuickTile(String label) {
        TextView tv = new TextView(this);
        tv.setText(label);
        tv.setTextColor(theme.textPrimary);
        tv.setTextSize(13f);
        tv.setPadding(VeloxTheme.dp(this, 12), VeloxTheme.dp(this, 9),
                VeloxTheme.dp(this, 12), VeloxTheme.dp(this, 9));
        tv.setBackground(VeloxTheme.roundedBox(
                theme.chipBg, theme.border, VeloxTheme.dp(this, 1), VeloxTheme.dp(this, 8)));
        tv.setFocusable(true);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.bottomMargin = VeloxTheme.dp(this, 6);
        tv.setLayoutParams(lp);
        return tv;
    }

    private void openSettingsAction(String action) {
        hideAllPanels();
        try {
            Intent i = new Intent(action);
            i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            startActivity(i);
        } catch (Throwable t) {
            Intent fb = new Intent(Settings.ACTION_SETTINGS);
            fb.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            startActivity(fb);
        }
    }

    // ---- Clock, Network, Panel State ---------------------------------------

    private void updateClock() {
        Date now = new Date();
        clockView.setText(new SimpleDateFormat("HH:mm", Locale.getDefault()).format(now));
        dateView.setText(new SimpleDateFormat("yyyy/MM/dd", Locale.getDefault()).format(now));
    }

    private void updateNetworkBadge() {
        String label = getString(R.string.net_offline);
        try {
            ConnectivityManager cm = (ConnectivityManager) getSystemService(Context.CONNECTIVITY_SERVICE);
            if (cm != null) {
                Network active = cm.getActiveNetwork();
                NetworkCapabilities nc = active != null ? cm.getNetworkCapabilities(active) : null;
                if (nc != null) {
                    if (nc.hasTransport(NetworkCapabilities.TRANSPORT_ETHERNET)) {
                        label = getString(R.string.net_eth);
                    } else if (nc.hasTransport(NetworkCapabilities.TRANSPORT_WIFI)) {
                        label = getString(R.string.net_wifi);
                    } else if (nc.hasTransport(NetworkCapabilities.TRANSPORT_CELLULAR)) {
                        label = getString(R.string.net_mobile);
                    }
                }
            }
        } catch (Throwable ignored) {
        }
        netBadge.setText(label);
    }

    private boolean isVisible(View v) {
        return v != null && v.getVisibility() == View.VISIBLE;
    }

    private boolean anyPanelVisible() {
        return isVisible(appMenuPanel) || isVisible(quickSettingsPanel)
                || isVisible(volumePanel) || isVisible(folderPanel);
    }

    private void hideAllPanels() {
        if (appMenuPanel != null) appMenuPanel.setVisibility(View.GONE);
        if (quickSettingsPanel != null) quickSettingsPanel.setVisibility(View.GONE);
        if (volumePanel != null) volumePanel.setVisibility(View.GONE);
        if (folderPanel != null) folderPanel.setVisibility(View.GONE);
    }
}
