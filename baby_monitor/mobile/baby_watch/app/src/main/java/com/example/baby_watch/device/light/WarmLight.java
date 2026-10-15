package com.example.baby_watch.device.light;

import android.content.Context;
import android.content.SharedPreferences;

import com.example.baby_watch.data.server.MobileServerRepository;

import java.util.HashMap;
import java.util.Map;

public class WarmLight {

    private static final String PREFS_NAME = "warm_light_prefs";
    private static final String KEY_ENABLED = "light_enabled";

    public static void sendLightCommand(boolean turnOn) {
        Map<String, Object> data = new HashMap<>();
        data.put("on", turnOn);
        MobileServerRepository.INSTANCE.sendCommand("light.set", data);
        com.example.baby_watch.service.log.LogManager.notification(
                "暖光灯",
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
