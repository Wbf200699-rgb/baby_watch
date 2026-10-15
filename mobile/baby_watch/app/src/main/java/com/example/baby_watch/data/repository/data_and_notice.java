package com.example.baby_watch.data.repository;

import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import java.util.ArrayList;
import java.util.List;

public class data_and_notice {

    private static final String TAG = "data_and_notice";
    private static final int MAX_NOTICES = 50;

    // ── 最新数据 ──
    public static volatile float temperature = 0f;
    public static volatile float humidity = 0f;
    public static volatile int co2 = 0;
    public static volatile int alertLevel = 0;
    public static volatile String alertTitle = "";
    public static volatile String alertHint = "";
    public static volatile String babyStatus = "酣睡中";
    public static volatile long lastUpdate = 0;

    // ── 通知列表 ──
    private static final List<String> noticeList = new ArrayList<>();

    // ── 监听器 ──
    public interface OnUpdateListener {
        void onDataUpdate();
    }

    private static OnUpdateListener updateListener;

    public static void setOnUpdateListener(OnUpdateListener l) {
        updateListener = l;
    }

    // ── 启动 / 停止 ──

    public static void start() {
        Log.d(TAG, "start() 调用 receive_json.setOnDataListener...");
        receive_json.setOnDataListener((data, raw) -> {
            temperature = data.temperature;
            humidity = data.humidity;
            co2 = data.co2;
            alertLevel = data.level;
            alertTitle = data.title;
            alertHint = data.hint;
            babyStatus = data.status;
            lastUpdate = System.currentTimeMillis();

            Log.d(TAG, "更新: T=" + temperature + " H=" + humidity +
                    " CO2=" + co2 + " level=" + alertLevel + " status=" + babyStatus);

            // 记录通知
            String notice = String.format("[Lv%d] %s | T:%.1f H:%.0f CO2:%d",
                    alertLevel, alertTitle, temperature, humidity, co2);
            noticeList.add(0, notice);
            if (noticeList.size() > MAX_NOTICES) {
                noticeList.remove(noticeList.size() - 1);
            }

            // 通知 UI
            new Handler(Looper.getMainLooper()).post(() -> {
                if (updateListener != null) {
                    updateListener.onDataUpdate();
                }
            });
        });
        Log.d(TAG, "监听器已设置, 调用 receive_json.start()...");
        receive_json.start();
        Log.i(TAG, "数据通知桥接已启动");
    }

    public static void stop() {
        receive_json.stop();
    }

    public static List<String> getNotices() {
        return new ArrayList<>(noticeList);
    }

    public static String getLatestNotice() {
        return noticeList.isEmpty() ? "" : noticeList.get(0);
    }
}
