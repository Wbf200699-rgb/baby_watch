package com.example.baby_watch.service.log;

import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.List;
import java.util.Locale;

public class LogManager {

    public enum LogType { SYSTEM, NOTIFICATION, ALERT }

    public static class Entry {
        public final String time;
        public final LogType type;
        public final String title;
        public final String detail;

        Entry(String time, LogType type, String title, String detail) {
            this.time = time;
            this.type = type;
            this.title = title;
            this.detail = detail;
        }
    }

    public interface OnLogAddedListener {
        void onLogAdded();
    }

    private static final Object LOCK = new Object();
    private static volatile OnLogAddedListener logListener;
    private static final List<Entry> entries = new ArrayList<>();
    private static final int MAX = 200;
    private static final SimpleDateFormat sdf =
            new SimpleDateFormat("HH:mm:ss", Locale.ROOT);

    public static void setOnLogAddedListener(OnLogAddedListener l) {
        logListener = l;
    }

    public static void system(String title, String detail) {
        add(LogType.SYSTEM, title, detail);
    }

    public static void system(String title) {
        add(LogType.SYSTEM, title, "");
    }

    public static void notification(String title, String detail) {
        add(LogType.NOTIFICATION, title, detail);
    }

    public static void notification(String title) {
        add(LogType.NOTIFICATION, title, "");
    }

    public static void alert(int level, String title, String detail) {
        String prefix = level >= 3 ? "[紧急]" : level >= 2 ? "[安抚]" : "[提醒]";
        add(LogType.ALERT, prefix + " " + title, detail);
    }

    public static void alert(String title, String detail) {
        add(LogType.ALERT, title, detail);
    }

    private static void add(LogType type, String title, String detail) {
        synchronized (LOCK) {
            String now = sdf.format(new Date());
            entries.add(0, new Entry(now, type, title, detail));
            if (entries.size() > MAX) {
                entries.remove(entries.size() - 1);
            }
        }
        OnLogAddedListener listener = logListener;
        if (listener != null) listener.onLogAdded();
    }

    public static List<Entry> getAll() {
        synchronized (LOCK) {
            return new ArrayList<>(entries);
        }
    }

    public static List<Entry> getByType(LogType type) {
        List<Entry> result = new ArrayList<>();
        synchronized (LOCK) {
            for (Entry e : entries) {
                if (e.type == type) result.add(e);
            }
        }
        return result;
    }
}
