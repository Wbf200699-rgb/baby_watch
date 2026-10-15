package com.example.baby_watch.notification.text;

import android.content.Context;
import android.telephony.SmsManager;
import com.example.baby_watch.notification.EmergencyContact;

public class text {
    public static void send(Context ctx) {
        String phone = EmergencyContact.get(ctx);
        if (phone.isEmpty()) return;

        SmsManager sms = SmsManager.getDefault();
        sms.sendTextMessage(phone, null, "紧急告急！请立即查看宝宝状态！", null, null);
    }
}
