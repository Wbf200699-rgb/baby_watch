package com.example.baby_watch.notification.phone_notice;

import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.os.Build;
import androidx.core.app.NotificationCompat;

public class phone_notice {

    private static final String CHANNEL_ID = "baby_watch_alert";
    private static final String GROUP_KEY = "baby_watch_safety_events";

    public static void show(Context ctx, long eventId, String title, String text) {
        NotificationManager nm = (NotificationManager) ctx.getSystemService(Context.NOTIFICATION_SERVICE);

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            NotificationChannel channel = new NotificationChannel(
                    CHANNEL_ID, "安全告警", NotificationManager.IMPORTANCE_HIGH);
            channel.setDescription("宝宝监护设备上报的安全事件");
            nm.createNotificationChannel(channel);
        }

        Intent intent = ctx.getPackageManager().getLaunchIntentForPackage(ctx.getPackageName());
        PendingIntent pi = PendingIntent.getActivity(ctx, 0, intent,
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);

        android.app.Notification notification = new NotificationCompat.Builder(ctx, CHANNEL_ID)
                .setSmallIcon(android.R.drawable.ic_dialog_alert)
                .setContentTitle(title)
                .setContentText(text)
                .setStyle(new NotificationCompat.BigTextStyle().bigText(text))
                .setPriority(NotificationCompat.PRIORITY_MAX)
                .setCategory(NotificationCompat.CATEGORY_ALARM)
                .setVisibility(NotificationCompat.VISIBILITY_PRIVATE)
                .setGroup(GROUP_KEY)
                .setContentIntent(pi)
                .setAutoCancel(true)
                .build();

        int notificationId = 1000 + (int) Math.floorMod(eventId, 100000L);
        nm.notify(notificationId, notification);
    }
}
