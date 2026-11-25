package com.velox.launcher;

import android.app.ActivityOptions;
import android.app.AppOpsManager;
import android.app.usage.UsageStats;
import android.app.usage.UsageStatsManager;
import android.content.Context;
import android.content.Intent;
import android.graphics.Rect;
import android.net.Uri;
import android.os.Build;
import android.os.Process;
import android.provider.Settings;
import android.util.DisplayMetrics;
import android.widget.Toast;

import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.List;

/**
 * Integrates cleanly with standard Android Desktop / Freeform Windowing APIs
 * (ActivityOptions.setLaunchBounds) while falling back safely to normal/fullscreen
 * launch for games or non-resizable apps. Never fabricates running app state.
 */
public final class DesktopWindowHelper {
    public static final int MODE_AUTO = 0;
    public static final int MODE_WINDOWED = 1;
    public static final int MODE_FULLSCREEN = 2;
    public static final int MODE_SNAP_LEFT = 3;
    public static final int MODE_SNAP_RIGHT = 4;

    private static int sCascadeStep = 0;

    private DesktopWindowHelper() {}

    public static boolean isFreeformEnabledInSettings(Context ctx) {
        try {
            int ff = Settings.Global.getInt(ctx.getContentResolver(), "enable_freeform_support", 0);
            int devFf = Settings.Global.getInt(ctx.getContentResolver(),
                    "development_enable_freeform_windows_support", 0);
            return ff == 1 || devFf == 1;
        } catch (Throwable t) {
            return false;
        }
    }

    public static void launchApp(Context ctx, AppEntry entry, DesktopState state, int mode) {
        if (entry == null) return;
        Intent intent = new Intent(Intent.ACTION_MAIN);
        intent.addCategory(Intent.CATEGORY_LAUNCHER);
        intent.setComponent(entry.componentName());
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_RESET_TASK_IF_NEEDED);

        boolean isGameForcedFull = entry.isGame || state.gameModePackages.contains(entry.packageName);
        boolean wantWindowed = (mode == MODE_WINDOWED || mode == MODE_SNAP_LEFT || mode == MODE_SNAP_RIGHT)
                || (mode == MODE_AUTO && state.windowedLaunch && !isGameForcedFull && isFreeformEnabledInSettings(ctx));

        state.recordLaunch(entry.packageName);

        if (wantWindowed) {
            try {
                Rect bounds = computeBounds(ctx, mode);
                ActivityOptions opts = ActivityOptions.makeBasic();
                opts.setLaunchBounds(bounds);
                ctx.startActivity(intent, opts.toBundle());
                return;
            } catch (Throwable ignored) {
                // Safe fallback to normal fullscreen launch
            }
        }

        try {
            ctx.startActivity(intent);
        } catch (Throwable t) {
            Toast.makeText(ctx, entry.label + ": " + t.getMessage(), Toast.LENGTH_SHORT).show();
        }
    }

    private static Rect computeBounds(Context ctx, int mode) {
        DisplayMetrics dm = ctx.getResources().getDisplayMetrics();
        int sw = dm.widthPixels;
        int sh = dm.heightPixels;
        int taskbarReserve = VeloxTheme.dp(ctx, 56);
        int topReserve = VeloxTheme.dp(ctx, 28);
        int usableH = Math.max(300, sh - taskbarReserve - topReserve);

        if (mode == MODE_SNAP_LEFT) {
            return new Rect(0, topReserve, sw / 2, topReserve + usableH);
        }
        if (mode == MODE_SNAP_RIGHT) {
            return new Rect(sw / 2, topReserve, sw, topReserve + usableH);
        }
        // Cascading desktop window (approx 58% x 76% on landscape)
        int w = Math.max(VeloxTheme.dp(ctx, 420), (int) (sw * 0.56f));
        int h = Math.max(VeloxTheme.dp(ctx, 480), (int) (usableH * 0.84f));
        w = Math.min(w, sw - VeloxTheme.dp(ctx, 32));
        h = Math.min(h, usableH - VeloxTheme.dp(ctx, 16));
        int offset = (sCascadeStep++ % 5) * VeloxTheme.dp(ctx, 32);
        int left = Math.min(sw - w - 16, Math.max(16, (sw - w) / 2 - VeloxTheme.dp(ctx, 64) + offset));
        int top = Math.min(topReserve + usableH - h, topReserve + VeloxTheme.dp(ctx, 16) + offset / 2);
        return new Rect(left, top, left + w, top + h);
    }

    public static void openAppInfo(Context ctx, String pkg) {
        Intent i = new Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, Uri.parse("package:" + pkg));
        i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        ctx.startActivity(i);
    }

    public static void requestUninstall(Context ctx, String pkg) {
        Intent i = new Intent(Intent.ACTION_DELETE, Uri.parse("package:" + pkg));
        i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        ctx.startActivity(i);
    }

    public static void openHomeChooser(Context ctx) {
        Intent i = new Intent(Settings.ACTION_HOME_SETTINGS);
        i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        ctx.startActivity(i);
    }

    public static void expandNotifications(Context ctx) {
        try {
            Object sb = ctx.getSystemService("statusbar");
            if (sb != null) {
                Method m = Class.forName("android.app.StatusBarManager").getMethod("expandNotificationsPanel");
                m.invoke(sb);
                return;
            }
        } catch (Throwable ignored) {
        }
        // Honest fallback when non-system app cannot invoke hidden StatusBarManager API
        Intent i = new Intent(Settings.ACTION_NOTIFICATION_ASSISTANT_SETTINGS);
        i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        try {
            ctx.startActivity(i);
        } catch (Throwable t) {
            Intent fallback = new Intent(Settings.ACTION_SETTINGS);
            fallback.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            ctx.startActivity(fallback);
        }
    }

    /**
     * Checks whether PACKAGE_USAGE_STATS permission is actually granted.
     * Never guesses running tasks without real OS data.
     */
    public static boolean hasUsageStatsPermission(Context ctx) {
        try {
            AppOpsManager aom = (AppOpsManager) ctx.getSystemService(Context.APP_OPS_SERVICE);
            if (aom == null) return false;
            int mode = aom.unsafeCheckOpNoThrow(AppOpsManager.OPSTR_GET_USAGE_STATS,
                    Process.myUid(), ctx.getPackageName());
            return mode == AppOpsManager.MODE_ALLOWED;
        } catch (Throwable t) {
            return false;
        }
    }

    /**
     * Returns recently active packages from UsageStatsManager when permitted;
     * returns empty list otherwise so UI clearly shows Session Recent only.
     */
    public static List<String> queryRealRecentPackages(Context ctx, String selfPkg) {
        if (!hasUsageStatsPermission(ctx)) return Collections.emptyList();
        try {
            UsageStatsManager usm = (UsageStatsManager) ctx.getSystemService(Context.USAGE_STATS_SERVICE);
            if (usm == null) return Collections.emptyList();
            long now = System.currentTimeMillis();
            List<UsageStats> stats = usm.queryUsageStats(
                    UsageStatsManager.INTERVAL_DAILY, now - 30L * 60L * 1000L, now);
            if (stats == null || stats.isEmpty()) return Collections.emptyList();
            Collections.sort(stats, Comparator.comparingLong(UsageStats::getLastTimeUsed).reversed());
            List<String> out = new ArrayList<>();
            for (UsageStats u : stats) {
                String p = u.getPackageName();
                if (p == null || p.equals(selfPkg) || p.equals("android")
                        || p.equals("com.android.systemui") || out.contains(p)) continue;
                out.add(p);
                if (out.size() >= 6) break;
            }
            return out;
        } catch (Throwable t) {
            return Collections.emptyList();
        }
    }
}
