package com.velox.launcher;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Color;
import android.graphics.drawable.BitmapDrawable;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.util.TypedValue;

import java.io.File;

/**
 * Velox Design System: calm dark-first PC desktop palette, light theme,
 * curated wallpapers, and crisp rounded surfaces with zero visual clutter.
 */
public final class VeloxTheme {
    public final boolean dark;
    public final int surface;
    public final int surfaceElevated;
    public final int taskbarBg;
    public final int panelBg;
    public final int border;
    public final int hoverBg;
    public final int focusStroke;
    public final int accent;
    public final int accentSoft;
    public final int textPrimary;
    public final int textSecondary;
    public final int chipBg;
    public final int chipActiveBg;
    public final int chipActiveText;
    public final int danger;
    public final int iconShadow;

    private VeloxTheme(boolean dark) {
        this.dark = dark;
        if (dark) {
            this.surface = Color.parseColor("#0B1018");
            this.surfaceElevated = Color.parseColor("#131B29");
            this.taskbarBg = Color.parseColor("#E6101723");
            this.panelBg = Color.parseColor("#F2131C2B");
            this.border = Color.parseColor("#25344B");
            this.hoverBg = Color.parseColor("#333B82F6");
            this.focusStroke = Color.parseColor("#60A5FA");
            this.accent = Color.parseColor("#3B82F6");
            this.accentSoft = Color.parseColor("#263B82F6");
            this.textPrimary = Color.parseColor("#EBF1FA");
            this.textSecondary = Color.parseColor("#94A3B8");
            this.chipBg = Color.parseColor("#1C2738");
            this.chipActiveBg = Color.parseColor("#3B82F6");
            this.chipActiveText = Color.parseColor("#FFFFFF");
            this.danger = Color.parseColor("#EF4444");
            this.iconShadow = Color.parseColor("#AA000000");
        } else {
            this.surface = Color.parseColor("#E9EEF5");
            this.surfaceElevated = Color.parseColor("#FFFFFF");
            this.taskbarBg = Color.parseColor("#EBEEF3F9");
            this.panelBg = Color.parseColor("#F5F8FAFC");
            this.border = Color.parseColor("#CBD5E1");
            this.hoverBg = Color.parseColor("#262563EB");
            this.focusStroke = Color.parseColor("#2563EB");
            this.accent = Color.parseColor("#2563EB");
            this.accentSoft = Color.parseColor("#1E2563EB");
            this.textPrimary = Color.parseColor("#0F172A");
            this.textSecondary = Color.parseColor("#475569");
            this.chipBg = Color.parseColor("#E2E8F0");
            this.chipActiveBg = Color.parseColor("#2563EB");
            this.chipActiveText = Color.parseColor("#FFFFFF");
            this.danger = Color.parseColor("#DC2626");
            this.iconShadow = Color.parseColor("#55000000");
        }
    }

    public static VeloxTheme of(boolean dark) {
        return new VeloxTheme(dark);
    }

    public static int dp(Context ctx, float dp) {
        return Math.round(TypedValue.applyDimension(
                TypedValue.COMPLEX_UNIT_DIP, dp, ctx.getResources().getDisplayMetrics()));
    }

    public static Drawable roundedBox(int fill, int strokeColor, int strokePx, float radiusPx) {
        GradientDrawable d = new GradientDrawable();
        d.setColor(fill);
        d.setCornerRadius(radiusPx);
        if (strokePx > 0) {
            d.setStroke(strokePx, strokeColor);
        }
        return d;
    }

    public static final String[] WALLPAPER_IDS = {
            "midnight_slate",
            "deep_cobalt",
            "nordic_dusk",
            "obsidian_horizon",
            "daylight_mist"
    };

    public static final String[] WALLPAPER_NAMES_JA = {
            "ミッドナイト・スレート（既定）",
            "ディープ・コバルト",
            "ノルディック・ダスク",
            "オブシディアン・ホライズン",
            "デイライト・ミスト（明）"
    };

    public static final String[] WALLPAPER_NAMES_EN = {
            "Midnight Slate (Default)",
            "Deep Cobalt",
            "Nordic Dusk",
            "Obsidian Horizon",
            "Daylight Mist (Light)"
    };

    public static Drawable buildWallpaper(Context ctx, String id, String customPath) {
        if ("custom".equals(id) && customPath != null && !customPath.isEmpty()) {
            File f = new File(customPath);
            if (f.isFile()) {
                Bitmap bmp = BitmapFactory.decodeFile(f.getAbsolutePath());
                if (bmp != null) {
                    return new BitmapDrawable(ctx.getResources(), bmp);
                }
            }
        }
        int[] colors;
        switch (id == null ? "midnight_slate" : id) {
            case "deep_cobalt":
                colors = new int[]{
                        Color.parseColor("#081427"),
                        Color.parseColor("#112A4C"),
                        Color.parseColor("#1B4170")
                };
                break;
            case "nordic_dusk":
                colors = new int[]{
                        Color.parseColor("#0D1321"),
                        Color.parseColor("#1D2D44"),
                        Color.parseColor("#2B4162")
                };
                break;
            case "obsidian_horizon":
                colors = new int[]{
                        Color.parseColor("#090C10"),
                        Color.parseColor("#121923"),
                        Color.parseColor("#1B2638")
                };
                break;
            case "daylight_mist":
                colors = new int[]{
                        Color.parseColor("#DCE7F5"),
                        Color.parseColor("#EBF1F8"),
                        Color.parseColor("#D4E1F2")
                };
                break;
            case "midnight_slate":
            default:
                colors = new int[]{
                        Color.parseColor("#0A101A"),
                        Color.parseColor("#122036"),
                        Color.parseColor("#0E1828")
                };
                break;
        }
        GradientDrawable g = new GradientDrawable(GradientDrawable.Orientation.TL_BR, colors);
        g.setGradientType(GradientDrawable.LINEAR_GRADIENT);
        return g;
    }
}
