package com.velox.launcher;

import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.graphics.drawable.Drawable;
import android.os.Build;

import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/**
 * Real installed launchable application discovered via Android PackageManager.
 */
public final class AppEntry {
    public static final String CAT_ALL = "all";
    public static final String CAT_GAMES = "games";
    public static final String CAT_TOOLS = "tools";
    public static final String CAT_MEDIA = "media";
    public static final String CAT_SYSTEM = "system";

    public final String packageName;
    public final String activityName;
    public final String label;
    public final String category;
    public final boolean isSystem;
    public final boolean isGame;
    public final Drawable icon;
    public long lastLaunchedMs;

    public AppEntry(String packageName, String activityName, String label,
                    String category, boolean isSystem, boolean isGame, Drawable icon) {
        this.packageName = packageName;
        this.activityName = activityName;
        this.label = label;
        this.category = category;
        this.isSystem = isSystem;
        this.isGame = isGame;
        this.icon = icon;
    }

    public ComponentName componentName() {
        return new ComponentName(packageName, activityName);
    }

    public static List<AppEntry> loadInstalled(PackageManager pm, String selfPackage,
                                               Map<String, Long> launchTimes) {
        Intent query = new Intent(Intent.ACTION_MAIN);
        query.addCategory(Intent.CATEGORY_LAUNCHER);
        List<ResolveInfo> resolved = pm.queryIntentActivities(query, 0);
        List<AppEntry> out = new ArrayList<>();
        Map<String, Boolean> seen = new HashMap<>();

        for (ResolveInfo ri : resolved) {
            if (ri.activityInfo == null) continue;
            String pkg = ri.activityInfo.packageName;
            if (pkg == null || pkg.equals(selfPackage)) continue;
            String act = ri.activityInfo.name;
            String key = pkg + "/" + act;
            if (seen.containsKey(key)) continue;
            seen.put(key, Boolean.TRUE);

            CharSequence rawLabel = ri.loadLabel(pm);
            String label = rawLabel != null ? rawLabel.toString().trim() : pkg;
            if (label.isEmpty()) label = pkg;

            ApplicationInfo ai = ri.activityInfo.applicationInfo;
            boolean isSystem = ai != null && ((ai.flags & ApplicationInfo.FLAG_SYSTEM) != 0);
            boolean isGame = false;
            String cat = classify(ai, pkg, isSystem);
            if (CAT_GAMES.equals(cat)) {
                isGame = true;
            }

            Drawable icon;
            try {
                icon = ri.loadIcon(pm);
            } catch (Throwable t) {
                icon = pm.getDefaultActivityIcon();
            }

            AppEntry entry = new AppEntry(pkg, act, label, cat, isSystem, isGame, icon);
            Long ts = launchTimes != null ? launchTimes.get(pkg) : null;
            entry.lastLaunchedMs = ts != null ? ts : 0L;
            out.add(entry);
        }

        Collections.sort(out, Comparator.comparing(a -> a.label.toLowerCase(Locale.ROOT)));
        return out;
    }

    private static String classify(ApplicationInfo ai, String pkg, boolean isSystem) {
        if (ai != null) {
            if ((ai.flags & ApplicationInfo.FLAG_IS_GAME) != 0) {
                return CAT_GAMES;
            }
            if (Build.VERSION.SDK_INT >= 26) {
                switch (ai.category) {
                    case ApplicationInfo.CATEGORY_GAME:
                        return CAT_GAMES;
                    case ApplicationInfo.CATEGORY_AUDIO:
                    case ApplicationInfo.CATEGORY_VIDEO:
                    case ApplicationInfo.CATEGORY_IMAGE:
                    case ApplicationInfo.CATEGORY_SOCIAL:
                    case ApplicationInfo.CATEGORY_NEWS:
                        return CAT_MEDIA;
                    case ApplicationInfo.CATEGORY_PRODUCTIVITY:
                    case ApplicationInfo.CATEGORY_MAPS:
                        return CAT_TOOLS;
                    default:
                        break;
                }
            }
        }
        String low = pkg.toLowerCase(Locale.ROOT);
        if (low.contains("game") || low.contains("unity") || low.contains("antutu")) {
            return CAT_GAMES;
        }
        if (low.contains("youtube") || low.contains("music") || low.contains("photos")
                || low.contains("camera") || low.contains("gallery") || low.contains("video")) {
            return CAT_MEDIA;
        }
        if (low.contains("settings") || low.contains("phone") || low.contains("dialer")
                || low.contains("messaging") || low.contains("contacts")) {
            return CAT_SYSTEM;
        }
        return isSystem ? CAT_TOOLS : CAT_TOOLS;
    }
}
