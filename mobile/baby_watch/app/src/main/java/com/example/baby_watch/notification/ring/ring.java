package com.example.baby_watch.notification.ring;

import android.content.Context;
import android.media.AudioAttributes;
import android.media.MediaPlayer;
import android.media.Ringtone;
import android.media.RingtoneManager;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

public class ring {

    private static final String TAG = "ring";

    public static void start(Context ctx) {
        start(ctx, 2000);
    }

    public static void start(Context ctx, int durationMs) {
        Log.d(TAG, "响铃 " + durationMs + "ms");

        Uri uri = RingtoneManager.getDefaultUri(RingtoneManager.TYPE_ALARM);
        if (uri == null) uri = RingtoneManager.getDefaultUri(RingtoneManager.TYPE_NOTIFICATION);
        if (uri == null) uri = RingtoneManager.getDefaultUri(RingtoneManager.TYPE_RINGTONE);

        if (uri != null) {
            try {
                Ringtone ringtone = RingtoneManager.getRingtone(ctx, uri);
                if (ringtone != null) {
                    ringtone.play();
                    new Handler(Looper.getMainLooper()).postDelayed(ringtone::stop, durationMs);
                    return;
                }
            } catch (Exception e) {
                Log.e(TAG, "Ringtone 失败: " + e.getMessage());
            }
        }

        // 备用 MediaPlayer
        try {
            MediaPlayer mp = MediaPlayer.create(ctx,
                    android.provider.Settings.System.DEFAULT_ALARM_ALERT_URI);
            if (mp == null) {
                mp = MediaPlayer.create(ctx,
                        android.provider.Settings.System.DEFAULT_NOTIFICATION_URI);
            }
            final MediaPlayer player = mp;
            if (player != null) {
                player.setAudioAttributes(new AudioAttributes.Builder()
                        .setUsage(AudioAttributes.USAGE_ALARM)
                        .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
                        .build());
                player.start();
                new Handler(Looper.getMainLooper()).postDelayed(() -> {
                    player.stop();
                    player.release();
                }, durationMs);
            }
        } catch (Exception e) {
            Log.e(TAG, "MediaPlayer 失败: " + e.getMessage());
        }
    }
}
