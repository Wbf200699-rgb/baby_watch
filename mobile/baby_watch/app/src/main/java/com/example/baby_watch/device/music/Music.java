package com.example.baby_watch.device.music;

import android.content.Context;
import android.content.SharedPreferences;

import com.example.baby_watch.data.ip.ip_ok;
import com.example.baby_watch.data.send.send_json;

import java.util.HashMap;
import java.util.Map;

public class Music {

    private static final String PREFS_NAME = "music_prefs";
    private static final String KEY_PLAYING = "music_playing";

    public static void sendMusicCommand(boolean play) {
        Map<String, Object> data = new HashMap<>();
        String cmd = play ? "播放摇篮曲" : "暂停摇篮曲";
        data.put("command", cmd);
        send_json.replyToSender(data);
        com.example.baby_watch.service.log.LogManager.notification("摇篮曲", cmd);
    }

    public static void sendTrackCommand(String action) {
        Map<String, Object> data = new HashMap<>();
        data.put("command", action);
        send_json.replyToSender(data);
        com.example.baby_watch.service.log.LogManager.notification("摇篮曲", action);
    }

    public static boolean isPlaying(Context context) {
        SharedPreferences prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE);
        return prefs.getBoolean(KEY_PLAYING, false);
    }

    public static void setPlaying(Context context, boolean playing) {
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
                .edit().putBoolean(KEY_PLAYING, playing).apply();
    }
}
