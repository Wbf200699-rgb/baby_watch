package com.example.baby_watch.notification.call;

import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import com.example.baby_watch.notification.EmergencyContact;

public class call {
    public static void dial(Context ctx) {
        String phone = EmergencyContact.get(ctx);
        if (phone.isEmpty()) return;

        Intent intent = new Intent(Intent.ACTION_CALL);
        intent.setData(Uri.parse("tel:" + phone));
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        ctx.startActivity(intent);
    }
}
