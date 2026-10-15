package com.example.baby_watch.data.repository;

import android.util.Log;

import com.example.baby_watch.data.ip.ip_ok;

import org.json.JSONObject;

import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetSocketAddress;
import java.util.concurrent.atomic.AtomicBoolean;

public class receive_json {

    private static final String TAG = "receive_json";

    private static final AtomicBoolean running = new AtomicBoolean(false);
    private static Thread thread;
    private static DatagramSocket socket;

    // 记录最后发来数据的客户端地址，供回复用
    public static volatile String lastSenderIp = null;
    public static volatile int lastSenderPort = 0;

    public static class ParsedData {
        public final float temperature;
        public final float humidity;
        public final int co2;
        public final int level;
        public final String title;
        public final String hint;
        public final String status;

        public ParsedData(float t, float h, int c, int lv, String tt, String ht, String st) {
            temperature = t; humidity = h; co2 = c;
            level = lv; title = tt; hint = ht; status = st;
        }
    }

    public interface OnDataListener {
        void onData(ParsedData data, String rawJson);
    }

    private static OnDataListener listener;

    public static void setOnDataListener(OnDataListener l) {
        listener = l;
    }

    public static void start() {
        Log.d(TAG, "start() 被调用, running=" + running.get());
        if (running.getAndSet(true)) {
            Log.w(TAG, "已在运行, 忽略");
            return;
        }

        int port = ip_ok.routerPort;
        Log.d(TAG, "准备监听端口 " + port + " routerPort=" + ip_ok.routerPort);
        thread = new Thread(() -> {
            try {
                Log.d(TAG, "线程启动, 绑定端口 " + port);
                socket = new DatagramSocket(null);
                socket.setReuseAddress(true);
                socket.bind(new InetSocketAddress(port));
                byte[] buf = new byte[1024];
                Log.i(TAG, "UDP 监听启动, 端口 " + port);

                while (running.get()) {
                    try {
                        DatagramPacket packet = new DatagramPacket(buf, buf.length);
                        socket.receive(packet);
                        lastSenderIp = packet.getAddress().getHostAddress();
                        lastSenderPort = packet.getPort();
                        String json = new String(packet.getData(), 0, packet.getLength(), "UTF-8");
                        Log.d(TAG, "收到: " + json + " 来自 " + lastSenderIp + ":" + lastSenderPort);
                        parseAndNotify(json);
                    } catch (Exception e) {
                        if (running.get()) Log.e(TAG, "接收异常: " + e.getMessage());
                    }
                }
            } catch (Exception e) {
                Log.e(TAG, "Socket 失败: " + e.getMessage(), e);
            } finally {
                if (socket != null) { try { socket.close(); } catch (Exception ignored) {} }
            }
            running.set(false);
        });
        thread.setDaemon(true);
        thread.start();
    }

    public static void stop() {
        running.set(false);
        if (thread != null) {
            thread.interrupt();
            thread = null;
        }
        if (socket != null && !socket.isClosed()) {
            try {
                socket.close();
            } catch (Exception e) {
                Log.e(TAG, "关闭 Socket 失败", e);
            }
            socket = null;
        }
    }

    public static void restart() {
        stop();
        start();
    }

    private static void parseAndNotify(String json) {
        try {
            JSONObject obj = new JSONObject(json);
            float t = (float) obj.optDouble("T", 0);
            float h = (float) obj.optDouble("H", 0);
            int c = obj.optInt("CO2", 0);
            int lv = obj.optInt("level", 0);
            String tt = obj.optString("title", "");
            String ht = obj.optString("hint", "");
            String st = obj.optString("status", "酣睡中");

            ParsedData data = new ParsedData(t, h, c, lv, tt, ht, st);
            Log.d(TAG, "解析: T=" + t + " H=" + h + " CO2=" + c +
                    " level=" + lv + " title=" + tt + " status=" + st);

            if (listener != null) {
                listener.onData(data, json);
            }
        } catch (Exception e) {
            Log.e(TAG, "JSON 解析失败: " + json, e);
        }
    }
}
