package com.example.baby_watch.data.send;

import android.util.Log;

import org.json.JSONObject;

import com.example.baby_watch.data.ip.ip_ok;
import com.example.baby_watch.data.repository.receive_json;

import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.nio.charset.StandardCharsets;
import java.util.Map;

public class send_json {

    private static final String TAG = "send_json";

    /**
     * 发送到指定 IP 和端口
     */
    public static void sendData(String ip, int port, Map<String, Object> data) {
        send(ip, port, data);
    }

    /**
     * 回复给最后发传感器数据来的客户端（实现双向通信）
     */
    public static void replyToSender(Map<String, Object> data) {
        String ip = ip_ok.routerIp;
        int port = ip_ok.routerPort;
        if (ip == null || port == 0) {
            ip = receive_json.lastSenderIp;
            port = receive_json.lastSenderPort;
            if (ip == null || port == 0) {
                Log.w(TAG, "无客户端地址，无法回复");
                return;
            }
        }
        send(ip, port, data);
    }

    private static void send(String ip, int port, Map<String, Object> data) {
        new Thread(() -> {
            try (DatagramSocket socket = new DatagramSocket()) {
                JSONObject json = new JSONObject(data);
                byte[] bytes = json.toString().getBytes(StandardCharsets.UTF_8);

                Log.d(TAG, "发送 UDP → " + ip + ":" + port + " 数据: " + json);

                InetAddress address = InetAddress.getByName(ip);
                DatagramPacket packet = new DatagramPacket(bytes, bytes.length, address, port);
                socket.send(packet);

                Log.d(TAG, "发送成功, " + bytes.length + " bytes");
            } catch (Exception e) {
                Log.e(TAG, "发送失败: " + e.getMessage(), e);
            }
        }).start();
    }
}
