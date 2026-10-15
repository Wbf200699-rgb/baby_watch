package com.example.baby_watch.device.pat;

import android.content.Context;
import android.content.SharedPreferences;

import com.example.baby_watch.data.server.MobileServerRepository;

import java.util.HashMap;
import java.util.Map;

public class pat {

    private static final String PREFS_NAME = "pat_prefs";
    private static final String KEY_ENABLED = "pat_enabled";

    public static void sendPatCommand(boolean turnOn) {
        Map<String, Object> data = new HashMap<>();
        data.put("on", turnOn);
        MobileServerRepository.INSTANCE.sendCommand("soother.set", data);
        com.example.baby_watch.service.log.LogManager.notification(
                "拍一拍电机",
                turnOn ? "请求开启" : "请求关闭"
        );
    }

    public static boolean isEnabled(Context context) {
        SharedPreferences prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE);
        return prefs.getBoolean(KEY_ENABLED, false);
    }

    public static void setEnabled(Context context, boolean enabled) {
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
                .edit().putBoolean(KEY_ENABLED, enabled).apply();
    }
}
