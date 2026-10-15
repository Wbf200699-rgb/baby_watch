package com.example.baby_watch.data.ip;

import android.content.Context;
import android.content.SharedPreferences;

public class ip_ok {

    private static final String PREFS_NAME = "ip_ok_prefs";
    private static final String KEY_IP = "router_ip";
    private static final String KEY_PORT = "router_port";

    public static String routerIp = "192.168.31.43";
    public static int routerPort = 12300;

    public static void load(Context context) {
        SharedPreferences prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE);
        routerIp = prefs.getString(KEY_IP, routerIp);
        routerPort = prefs.getInt(KEY_PORT, routerPort);
    }

    public static void save(Context context, String ip, int port) {
        routerIp = ip;
        routerPort = port;
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
                .edit()
                .putString(KEY_IP, ip)
                .putInt(KEY_PORT, port)
                .apply();
    }
}
