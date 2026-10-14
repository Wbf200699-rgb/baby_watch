package com.example.baby_watch.service.keep_connecting;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Intent;
import android.os.Build;
import android.os.IBinder;

import androidx.core.app.NotificationCompat;

import com.example.baby_watch.MainActivity;
import com.example.baby_watch.data.server.MobileServerRepository;

public class KeepAliveService extends Service {

    private static final String CHANNEL_ID = "keep_alive";
    private static final int NOTIFY_ID = 2001;

    @Override
    public void onCreate() {
        super.onCreate();
        createNotificationChannel();
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        startForeground(NOTIFY_ID, buildNotification());
        MobileServerRepository.INSTANCE.start(getApplicationContext());
        return START_STICKY;
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    /**
     * Android 15+ limits the accumulated runtime of data-sync foreground services.
     * Stop immediately inside the grace period instead of allowing the process to ANR.
     */
    @Override
    public void onTimeout(int startId, int fgsType) {
        stopMonitoring();
    }

    @Override
    @SuppressWarnings("deprecation")
    public void onDestroy() {
        MobileServerRepository.INSTANCE.stop();
        removeForegroundNotification();
        super.onDestroy();
    }

    private void stopMonitoring() {
        MobileServerRepository.INSTANCE.stop();
        removeForegroundNotification();
        stopSelf();
    }

    @SuppressWarnings("deprecation")
    private void removeForegroundNotification() {
        stopForeground(STOP_FOREGROUND_REMOVE);
    }

    private Notification buildNotification() {
        Intent launchIntent = new Intent(this, MainActivity.class);
        PendingIntent pi = PendingIntent.getActivity(this, 0, launchIntent,
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);

        return new NotificationCompat.Builder(this, CHANNEL_ID)
                .setContentTitle("宝宝守护")
                .setContentText("正在通过服务端持续接收监护数据与安全告警")
                .setSmallIcon(android.R.drawable.ic_lock_idle_alarm)
                .setOngoing(true)
                .setContentIntent(pi)
                .setPriority(NotificationCompat.PRIORITY_LOW)
                .build();
    }

    private void createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            NotificationChannel channel = new NotificationChannel(
                    CHANNEL_ID,
                    "后台监护",
                    NotificationManager.IMPORTANCE_LOW
            );
            channel.setDescription("持续连接宝宝监护服务端并接收安全告警");
            NotificationManager nm = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
            nm.createNotificationChannel(channel);
        }
    }
}
