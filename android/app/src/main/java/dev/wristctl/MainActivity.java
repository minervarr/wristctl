package dev.wristctl;

import android.Manifest;
import android.app.Activity;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Bundle;
import android.util.Log;
import android.widget.ScrollView;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.List;

/**
 * One screen: connect, set the time, watch the wire traffic.
 *
 * No AndroidX, no fragments, no ViewModel. When this moves to a native activity,
 * delete this class and keep {@link WatchLink} (or replace it) plus {@link Native}.
 */
public final class MainActivity extends Activity implements WatchLink.Listener {

    private static final int REQUEST_PERMISSIONS = 1;
    private static final int LOG_LIMIT = 24_000;

    private final StringBuilder log = new StringBuilder();

    private TextView statusView;
    private TextView modelView;
    private TextView logView;
    private ScrollView logScroll;
    private WatchLink link;
    private boolean pendingConnect;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        statusView = findViewById(R.id.status);
        modelView = findViewById(R.id.model);
        logView = findViewById(R.id.log);
        logScroll = findViewById(R.id.log_scroll);
        link = new WatchLink(getApplicationContext(), this);

        findViewById(R.id.connect).setOnClickListener(v -> ensurePermissions(true));
        findViewById(R.id.sync).setOnClickListener(v -> link.syncTime());
        findViewById(R.id.disconnect).setOnClickListener(v -> link.disconnect());

        append(Native.version());
        append(Native.modelForName("GBX-100") == null
                ? "model registry: no GBX-100" : "model registry: GBX-100 ok");
        selfTest();
    }

    /**
     * Encodes and decodes a frame through the C core at startup.
     *
     * It is a build check that survives into the field: the result goes to the
     * console field and to logcat, so a broken JNI link is visible without a
     * watch.
     */
    private void selfTest() {
        byte[] frame = Native.encodeTime(2026, 10, 27, 18, 52, 0, 2);
        String result = frame == null
                ? "encodeTime failed: " + Native.lastError()
                : Native.decodeFrame(frame);
        String model = Native.modelForName("CASIO GBX-100");
        Log.i("wristctl", "core: " + Native.version());
        Log.i("wristctl", "model lookup: " + model);
        Log.i("wristctl", "self-test: " + result);
        append("self-test: " + result);
    }

    @Override
    protected void onDestroy() {
        link.disconnect();
        super.onDestroy();
    }

    // -- permissions -----------------------------------------------------

    private void ensurePermissions(boolean connectAfter) {
        List<String> missing = new ArrayList<>();
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            if (checkSelfPermission(Manifest.permission.BLUETOOTH_SCAN)
                    != PackageManager.PERMISSION_GRANTED) {
                missing.add(Manifest.permission.BLUETOOTH_SCAN);
            }
            if (checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT)
                    != PackageManager.PERMISSION_GRANTED) {
                missing.add(Manifest.permission.BLUETOOTH_CONNECT);
            }
        } else if (checkSelfPermission(Manifest.permission.ACCESS_FINE_LOCATION)
                != PackageManager.PERMISSION_GRANTED) {
            missing.add(Manifest.permission.ACCESS_FINE_LOCATION);
        }
        if (missing.isEmpty()) {
            if (connectAfter) {
                link.connect();
            }
            return;
        }
        pendingConnect = connectAfter;
        requestPermissions(missing.toArray(new String[0]), REQUEST_PERMISSIONS);
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions,
                                           int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode != REQUEST_PERMISSIONS) {
            return;
        }
        boolean granted = true;
        for (int result : grantResults) {
            granted &= result == PackageManager.PERMISSION_GRANTED;
        }
        if (granted && pendingConnect) {
            link.connect();
        } else if (!granted) {
            onStatus("Bluetooth permissions denied");
        }
        pendingConnect = false;
    }

    // -- WatchLink.Listener ---------------------------------------------

    @Override
    public void onStatus(String text) {
        runOnUiThread(() -> statusView.setText(text));
    }

    @Override
    public void onLog(String text) {
        runOnUiThread(() -> append(text));
    }

    @Override
    public void onModel(String text) {
        runOnUiThread(() -> modelView.setText("Model: " + text));
    }

    @Override
    public void onConnection(boolean connected) {
        // Status text already reflects this; kept as the hook a native UI needs.
    }

    // -- log -------------------------------------------------------------

    private void append(String text) {
        log.append(text).append('\n');
        if (log.length() > LOG_LIMIT) {
            log.delete(0, log.length() - LOG_LIMIT);
        }
        logView.setText(log);
        logScroll.post(() -> logScroll.fullScroll(ScrollView.FOCUS_DOWN));
    }
}
