package com.velox.launcher;

import android.content.Context;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.UUID;

/**
 * Persistent desktop layout, folders, taskbar pins, launch history, theme, and
 * wallpaper preferences stored locally in app-private storage (zero network).
 */
public final class DesktopState {
    public static final class DesktopItem {
        public final boolean isFolder;
        public final String id;
        public String name;
        public String packageName;
        public final List<String> folderPackages = new ArrayList<>();

        public static DesktopItem app(String pkg) {
            DesktopItem it = new DesktopItem(false, "app:" + pkg);
            it.packageName = pkg;
            return it;
        }

        public static DesktopItem folder(String id, String name, List<String> pkgs) {
            DesktopItem it = new DesktopItem(true, id == null ? "fld:" + UUID.randomUUID().toString().substring(0, 8) : id);
            it.name = name;
            if (pkgs != null) {
                it.folderPackages.addAll(pkgs);
            }
            return it;
        }

        private DesktopItem(boolean isFolder, String id) {
            this.isFolder = isFolder;
            this.id = id;
        }
    }

    public boolean darkTheme = true;
    public String wallpaperId = "midnight_slate";
    public String customWallpaperPath = "";
    public boolean windowedLaunch = true;
    public String sortMode = "name";
    public final List<DesktopItem> desktopItems = new ArrayList<>();
    public final List<String> taskbarPins = new ArrayList<>();
    public final List<String> sessionRecent = new ArrayList<>();
    public final Map<String, Long> launchTimes = new HashMap<>();
    public final Set<String> gameModePackages = new LinkedHashSet<>();

    private final File file;

    private DesktopState(File file) {
        this.file = file;
    }

    public static DesktopState load(Context ctx, List<AppEntry> installed) {
        File f = new File(ctx.getFilesDir(), "velox_desktop.json");
        DesktopState st = new DesktopState(f);
        boolean loaded = false;
        if (f.isFile()) {
            try (FileInputStream in = new FileInputStream(f)) {
                ByteArrayOutputStream bos = new ByteArrayOutputStream();
                byte[] buf = new byte[4096];
                int n;
                while ((n = in.read(buf)) > 0) bos.write(buf, 0, n);
                JSONObject root = new JSONObject(new String(bos.toByteArray(), StandardCharsets.UTF_8));
                st.darkTheme = root.optBoolean("darkTheme", true);
                st.wallpaperId = root.optString("wallpaperId", "midnight_slate");
                st.customWallpaperPath = root.optString("customWallpaperPath", "");
                st.windowedLaunch = root.optBoolean("windowedLaunch", true);
                st.sortMode = root.optString("sortMode", "name");

                JSONArray items = root.optJSONArray("desktopItems");
                if (items != null) {
                    for (int i = 0; i < items.length(); i++) {
                        JSONObject o = items.optJSONObject(i);
                        if (o == null) continue;
                        if (o.optBoolean("isFolder", false)) {
                            List<String> fp = new ArrayList<>();
                            JSONArray arr = o.optJSONArray("folderPackages");
                            if (arr != null) {
                                for (int j = 0; j < arr.length(); j++) {
                                    String p = arr.optString(j, "");
                                    if (!p.isEmpty()) fp.add(p);
                                }
                            }
                            st.desktopItems.add(DesktopItem.folder(
                                    o.optString("id", null),
                                    o.optString("name", "Folder"),
                                    fp));
                        } else {
                            String pkg = o.optString("packageName", "");
                            if (!pkg.isEmpty()) {
                                st.desktopItems.add(DesktopItem.app(pkg));
                            }
                        }
                    }
                }
                JSONArray pins = root.optJSONArray("taskbarPins");
                if (pins != null) {
                    for (int i = 0; i < pins.length(); i++) {
                        String p = pins.optString(i, "");
                        if (!p.isEmpty() && !st.taskbarPins.contains(p)) st.taskbarPins.add(p);
                    }
                }
                JSONObject lt = root.optJSONObject("launchTimes");
                if (lt != null) {
                    JSONArray names = lt.names();
                    if (names != null) {
                        for (int i = 0; i < names.length(); i++) {
                            String k = names.optString(i, "");
                            if (!k.isEmpty()) st.launchTimes.put(k, lt.optLong(k, 0L));
                        }
                    }
                }
                JSONArray gm = root.optJSONArray("gameModePackages");
                if (gm != null) {
                    for (int i = 0; i < gm.length(); i++) {
                        String p = gm.optString(i, "");
                        if (!p.isEmpty()) st.gameModePackages.add(p);
                    }
                }
                loaded = true;
            } catch (Throwable ignored) {
                loaded = false;
            }
        }

        if (!loaded || (st.desktopItems.isEmpty() && st.taskbarPins.isEmpty())) {
            st.seedDefaults(installed);
            st.save();
        }
        return st;
    }

    private void seedDefaults(List<AppEntry> installed) {
        desktopItems.clear();
        taskbarPins.clear();
        String[] preferredTaskbar = {
                "com.android.chrome",
                "com.google.android.apps.nbu.files",
                "com.android.documentsui",
                "com.android.settings",
                "com.google.android.youtube",
                "com.google.android.apps.photos"
        };
        Map<String, AppEntry> byPkg = new HashMap<>();
        for (AppEntry e : installed) {
            if (!byPkg.containsKey(e.packageName)) byPkg.put(e.packageName, e);
        }
        for (String p : preferredTaskbar) {
            if (byPkg.containsKey(p) && taskbarPins.size() < 5) {
                taskbarPins.add(p);
            }
        }
        if (taskbarPins.isEmpty()) {
            for (int i = 0; i < Math.min(4, installed.size()); i++) {
                taskbarPins.add(installed.get(i).packageName);
            }
        }
        // Populate desktop with up to 10 real installed apps cleanly
        int added = 0;
        for (AppEntry e : installed) {
            if (added >= 10) break;
            if (!hasDesktopApp(e.packageName)) {
                desktopItems.add(DesktopItem.app(e.packageName));
                added++;
            }
        }
    }

    public boolean hasDesktopApp(String pkg) {
        for (DesktopItem it : desktopItems) {
            if (!it.isFolder && pkg.equals(it.packageName)) return true;
        }
        return false;
    }

    public void pinToDesktop(String pkg) {
        if (!hasDesktopApp(pkg)) {
            desktopItems.add(DesktopItem.app(pkg));
            save();
        }
    }

    public void unpinFromDesktop(String pkg) {
        for (int i = desktopItems.size() - 1; i >= 0; i--) {
            DesktopItem it = desktopItems.get(i);
            if (!it.isFolder && pkg.equals(it.packageName)) {
                desktopItems.remove(i);
            }
        }
        save();
    }

    public void pinToTaskbar(String pkg) {
        if (!taskbarPins.contains(pkg)) {
            taskbarPins.add(pkg);
            save();
        }
    }

    public void unpinFromTaskbar(String pkg) {
        if (taskbarPins.remove(pkg)) {
            save();
        }
    }

    public void recordLaunch(String pkg) {
        launchTimes.put(pkg, System.currentTimeMillis());
        sessionRecent.remove(pkg);
        sessionRecent.add(0, pkg);
        while (sessionRecent.size() > 6) {
            sessionRecent.remove(sessionRecent.size() - 1);
        }
        save();
    }

    public void save() {
        try {
            JSONObject root = new JSONObject();
            root.put("darkTheme", darkTheme);
            root.put("wallpaperId", wallpaperId);
            root.put("customWallpaperPath", customWallpaperPath);
            root.put("windowedLaunch", windowedLaunch);
            root.put("sortMode", sortMode);

            JSONArray items = new JSONArray();
            for (DesktopItem it : desktopItems) {
                JSONObject o = new JSONObject();
                o.put("isFolder", it.isFolder);
                o.put("id", it.id);
                if (it.isFolder) {
                    o.put("name", it.name);
                    JSONArray fp = new JSONArray();
                    for (String p : it.folderPackages) fp.put(p);
                    o.put("folderPackages", fp);
                } else {
                    o.put("packageName", it.packageName);
                }
                items.put(o);
            }
            root.put("desktopItems", items);

            JSONArray pins = new JSONArray();
            for (String p : taskbarPins) pins.put(p);
            root.put("taskbarPins", pins);

            JSONObject lt = new JSONObject();
            for (Map.Entry<String, Long> e : launchTimes.entrySet()) {
                lt.put(e.getKey(), e.getValue());
            }
            root.put("launchTimes", lt);

            JSONArray gm = new JSONArray();
            for (String p : gameModePackages) gm.put(p);
            root.put("gameModePackages", gm);

            try (FileOutputStream out = new FileOutputStream(file)) {
                out.write(root.toString(2).getBytes(StandardCharsets.UTF_8));
            }
        } catch (Throwable ignored) {
        }
    }
}
