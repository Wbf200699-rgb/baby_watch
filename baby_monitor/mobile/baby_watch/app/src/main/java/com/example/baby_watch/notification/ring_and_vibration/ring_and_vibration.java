package com.example.baby_watch.notification.ring_and_vibration;

import android.content.Context;
import android.media.AudioAttributes;
import android.media.MediaPlayer;
import android.media.Ringtone;
import android.media.RingtoneManager;
import android.net.Uri;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.os.VibratorManager;
import android.util.Log;

public class ring_and_vibration {

    private static final String TAG = "ring_vibrate";
    private static boolean running = false;
    private static MediaPlayer mediaPlayer;

    @SuppressWarnings("deprecation")
    public static void start(Context ctx) {
        if (running) {
            Log.w(TAG, "已在运行，跳过");
            return;
        }
        running = true;
        Log.d(TAG, "启动响铃+震动 10s");

        // 震动
        Vibrator vibrator;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            VibratorManager vm = (VibratorManager) ctx.getSystemService(Context.VIBRATOR_MANAGER_SERVICE);
            vibrator = vm.getDefaultVibrator();
        } else {
            vibrator = (Vibrator) ctx.getSystemService(Context.VIBRATOR_SERVICE);
        }
        long[] pattern = {0, 1000, 500, 1000, 500, 1000, 500, 1000, 500, 1000, 500, 1000};
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            vibrator.vibrate(VibrationEffect.createWaveform(pattern, 0));
        } else {
            vibrator.vibrate(pattern, 0);
        }

        // 响铃 — 优先系统闹钟铃声，失败则用 MediaPlayer 播放默认提示音
        Uri uri = RingtoneManager.getDefaultUri(RingtoneManager.TYPE_ALARM);
        if (uri == null) uri = RingtoneManager.getDefaultUri(RingtoneManager.TYPE_NOTIFICATION);
        if (uri == null) uri = RingtoneManager.getDefaultUri(RingtoneManager.TYPE_RINGTONE);

        if (uri != null) {
            try {
                Ringtone ringtone = RingtoneManager.getRingtone(ctx, uri);
                if (ringtone != null) {
                    ringtone.play();
                    Log.d(TAG, "Ringtone 播放成功");
                    new Handler(Looper.getMainLooper()).postDelayed(() -> {
                        ringtone.stop();
                        vibrator.cancel();
                        running = false;
                        Log.d(TAG, "响铃+震动停止");
                    }, 10000);
                    return;
                }
            } catch (Exception e) {
                Log.e(TAG, "Ringtone 失败: " + e.getMessage());
            }
        }

        // 备用方案：MediaPlayer
        try {
            mediaPlayer = MediaPlayer.create(ctx, android.provider.Settings.System.DEFAULT_ALARM_ALERT_URI);
            if (mediaPlayer == null)
                mediaPlayer = MediaPlayer.create(ctx, android.provider.Settings.System.DEFAULT_NOTIFICATION_URI);
            if (mediaPlayer != null) {
                mediaPlayer.setAudioAttributes(
                        new AudioAttributes.Builder()
                                .setUsage(AudioAttributes.USAGE_ALARM)
                                .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
                                .build());
                mediaPlayer.setLooping(true);
                mediaPlayer.start();
                Log.d(TAG, "MediaPlayer 播放成功");
            }
        } catch (Exception e) {
            Log.e(TAG, "MediaPlayer 失败: " + e.getMessage());
        }

        new Handler(Looper.getMainLooper()).postDelayed(() -> {
            if (mediaPlayer != null) {
                mediaPlayer.stop();
                mediaPlayer.release();
                mediaPlayer = null;
            }
            vibrator.cancel();
            running = false;
            Log.d(TAG, "响铃+震动停止");
        }, 10000);
    }
}
