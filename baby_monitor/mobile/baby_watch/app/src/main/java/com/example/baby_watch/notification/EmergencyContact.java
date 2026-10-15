package com.example.baby_watch.notification;

import android.content.Context;
import android.content.SharedPreferences;

public class EmergencyContact {
    private static final String PREFS = "emergency_prefs";
    private static final String KEY = "contact";

    public static String get(Context ctx) {
        return ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
                .getString(KEY, "");
    }

    public static void save(Context ctx, String contact) {
        ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
                .edit().putString(KEY, contact).apply();
    }
}
