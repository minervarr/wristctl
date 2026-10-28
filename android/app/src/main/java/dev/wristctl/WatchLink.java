package dev.wristctl;

import android.annotation.SuppressLint;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothGatt;
import android.bluetooth.BluetoothGattCallback;
import android.bluetooth.BluetoothGattCharacteristic;
import android.bluetooth.BluetoothGattDescriptor;
import android.bluetooth.BluetoothGattService;
import android.bluetooth.BluetoothManager;
import android.bluetooth.le.BluetoothLeScanner;
import android.bluetooth.le.ScanCallback;
import android.bluetooth.le.ScanResult;
import android.bluetooth.le.ScanSettings;
import android.content.Context;
import android.content.SharedPreferences;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;

import java.util.ArrayDeque;
import java.util.Calendar;
import java.util.Locale;
import java.util.Queue;
import java.util.Random;
import java.util.UUID;

/**
 * Minimal Android BLE client for the GBX-100.
 *
 * The vendor handshake is replayed in the order recorded from the HCI capture:
 * watch name, BLE features, app info, BLE settings, advertise/connection
 * parameters, module, condition, version, then the 0x47 service-discovery
 * exchange. Frames are encoded and decoded by the C core through {@link Native}.
 *
 * This class is deliberately the only place that talks to Android BLE, so it can
 * be swapped for whatever the native activity ends up using.
 */
@SuppressLint("MissingPermission")
final class WatchLink {

    interface Listener {
        void onStatus(String text);

        void onLog(String text);

        /** Model name, or the raw advertised name when the registry has no match. */
        void onModel(String text);

        void onConnection(boolean connected);
    }

    private static final UUID SERVICE = UUID.fromString("26eb000d-b012-49a8-b1f8-394fb2032b0f");
    private static final UUID REQUEST = UUID.fromString("26eb002c-b012-49a8-b1f8-394fb2032b0f");
    private static final UUID SETTINGS = UUID.fromString("26eb002d-b012-49a8-b1f8-394fb2032b0f");
    private static final UUID NOTIFY = UUID.fromString("26eb0030-b012-49a8-b1f8-394fb2032b0f");
    private static final UUID CCCD = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb");
    private static final UUID LINK_LOSS_SERVICE =
            UUID.fromString("00001803-0000-1000-8000-00805f9b34fb");
    private static final UUID LINK_LOSS_ALERT =
            UUID.fromString("00002a06-0000-1000-8000-00805f9b34fb");

    private static final String[] NAME_PREFIXES = {
            "CASIO", "GBX", "GBD", "GPR", "GWF", "GST", "GMW", "MRG", "DW-", "GM-"
    };

    private static final int SCAN_MS = 30_000;
    private static final int STEP_TIMEOUT_MS = 700;
    private static final int TARGET_CHAR = 0;
    private static final int TARGET_SETTINGS = 1;

    private static final class Step {
        final byte[] payload;
        final int target;

        Step(byte[] payload, int target) {
            this.payload = payload;
            this.target = target;
        }
    }

    private final Context context;
    private final Listener listener;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final Queue<Step> queue = new ArrayDeque<>();
    private final byte[] appId;

    private BluetoothAdapter adapter;
    private BluetoothLeScanner scanner;
    private BluetoothGatt gatt;
    private BluetoothGattCharacteristic requestChar;
    private BluetoothGattCharacteristic settingsChar;
    private BluetoothGattCharacteristic notifyChar;
    private BluetoothDevice device;
    private String modelName;
    private boolean awaiting;
    private boolean scanning;

    private final Runnable stepTimeout = this::advance;
    private final Runnable scanTimeout = this::onScanWindowClosed;

    WatchLink(Context context, Listener listener) {
        this.context = context.getApplicationContext();
        this.listener = listener;
        this.appId = loadAppId();
    }

    // -- public surface --------------------------------------------------

    void connect() {
        BluetoothManager manager =
                (BluetoothManager) context.getSystemService(Context.BLUETOOTH_SERVICE);
        if (manager == null) {
            listener.onStatus("No Bluetooth service");
            return;
        }
        adapter = manager.getAdapter();
        if (adapter == null) {
            listener.onStatus("No Bluetooth adapter");
            return;
        }
        if (!adapter.isEnabled()) {
            listener.onStatus("Bluetooth is off");
            return;
        }
        scanner = adapter.getBluetoothLeScanner();
        if (scanner == null) {
            listener.onStatus("No BLE scanner");
            return;
        }
        BluetoothDevice bonded = bondedWatch();
        if (bonded != null) {
            listener.onLog("using bonded device " + describe(bonded));
            stopScan();
            connectTo(bonded);
            return;
        }
        listener.onStatus("Scanning; press a watch button to wake it");
        listener.onLog("scan window " + (SCAN_MS / 1000) + " s");
        ScanSettings settings = new ScanSettings.Builder()
                .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
                .build();
        scanning = true;
        scanner.startScan(null, settings, scanCallback);
        handler.postDelayed(scanTimeout, SCAN_MS);
    }

    void disconnect() {
        handler.removeCallbacksAndMessages(null);
        queue.clear();
        awaiting = false;
        if (gatt != null) {
            gatt.disconnect();
            gatt.close();
            gatt = null;
        }
        requestChar = null;
        settingsChar = null;
        notifyChar = null;
        listener.onConnection(false);
        listener.onStatus("Disconnected");
    }

    /** Write the phone's current local time as class 0x09. */
    void syncTime() {
        if (gatt == null || settingsChar == null) {
            listener.onStatus("Not connected");
            return;
        }
        Calendar now = Calendar.getInstance();
        byte[] frame = Native.encodeTime(
                now.get(Calendar.YEAR),
                now.get(Calendar.MONTH) + 1,
                now.get(Calendar.DAY_OF_MONTH),
                now.get(Calendar.HOUR_OF_DAY),
                now.get(Calendar.MINUTE),
                now.get(Calendar.SECOND),
                now.get(Calendar.DAY_OF_WEEK) - 1);
        if (frame == null) {
            listener.onStatus("encodeTime failed: " + Native.lastError());
            return;
        }
        listener.onLog("-> " + hex(frame));
        write(settingsChar, frame, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT);
    }

    // -- scanning --------------------------------------------------------

    private final ScanCallback scanCallback = new ScanCallback() {
        @Override
        public void onScanResult(int callbackType, ScanResult result) {
            BluetoothDevice candidate = result.getDevice();
            String name = result.getScanRecord() == null
                    ? null : result.getScanRecord().getDeviceName();
            if (name == null) {
                name = candidate.getName();
            }
            if (!looksLikeWatch(name)) {
                return;
            }
            listener.onLog("found " + name + " " + candidate.getAddress());
            stopScan();
            connectTo(candidate);
        }

        @Override
        public void onScanFailed(int errorCode) {
            scanning = false;
            listener.onStatus("Scan failed: " + errorCode);
        }
    };

    private void onScanWindowClosed() {
        if (scanning) {
            stopScan();
            listener.onStatus("No watch seen. Press a watch button, then Connect again.");
        }
    }

    private void stopScan() {
        if (scanner != null && scanning) {
            scanner.stopScan(scanCallback);
        }
        scanning = false;
        handler.removeCallbacks(scanTimeout);
    }

    private static boolean looksLikeWatch(String name) {
        if (name == null) {
            return false;
        }
        String upper = name.toUpperCase(Locale.ROOT);
        for (String prefix : NAME_PREFIXES) {
            if (upper.startsWith(prefix)) {
                return true;
            }
        }
        return false;
    }

    // -- connection ------------------------------------------------------

    private void connectTo(BluetoothDevice target) {
        stopScan();
        device = target;
        listener.onStatus("Connecting to " + describe(target));
        gatt = target.connectGatt(context, false, gattCallback,
                BluetoothDevice.TRANSPORT_LE);
    }

    private final BluetoothGattCallback gattCallback = new BluetoothGattCallback() {
        @Override
        public void onConnectionStateChange(BluetoothGatt g, int status, int newState) {
            if (newState == BluetoothGatt.STATE_CONNECTED) {
                listener.onConnection(true);
                listener.onStatus("Connected; discovering services");
                g.discoverServices();
            } else if (newState == BluetoothGatt.STATE_DISCONNECTED) {
                listener.onConnection(false);
                listener.onStatus("Link lost (status " + status + ")");
                handler.removeCallbacks(stepTimeout);
                queue.clear();
                awaiting = false;
            }
        }

        @Override
        public void onServicesDiscovered(BluetoothGatt g, int status) {
            if (status != BluetoothGatt.GATT_SUCCESS) {
                listener.onStatus("Service discovery failed: " + status);
                return;
            }
            BluetoothGattService service = g.getService(SERVICE);
            if (service == null) {
                listener.onStatus("Casio service not found");
                return;
            }
            requestChar = service.getCharacteristic(REQUEST);
            settingsChar = service.getCharacteristic(SETTINGS);
            notifyChar = service.getCharacteristic(NOTIFY);
            if (requestChar == null || settingsChar == null || notifyChar == null) {
                listener.onStatus("Casio characteristics missing");
                return;
            }
            g.setCharacteristicNotification(notifyChar, true);
            BluetoothGattDescriptor cccd = notifyChar.getDescriptor(CCCD);
            if (cccd != null) {
                writeDescriptor(cccd, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE);
            }
            handler.postDelayed(WatchLink.this::startHandshake, 250);
        }

        @Override
        public void onCharacteristicChanged(BluetoothGatt g,
                                            BluetoothGattCharacteristic ch,
                                            byte[] value) {
            onFrame(value);
        }

        @Override
        public void onCharacteristicChanged(BluetoothGatt g,
                                            BluetoothGattCharacteristic ch) {
            onFrame(ch.getValue());
        }

        @Override
        public void onCharacteristicWrite(BluetoothGatt g,
                                          BluetoothGattCharacteristic ch, int status) {
            if (status != BluetoothGatt.GATT_SUCCESS) {
                listener.onLog("write failed: " + status);
            }
        }
    };

    // -- handshake -------------------------------------------------------

    private void startHandshake() {
        queue.clear();
        queue.add(new Step(new byte[]{0x23}, TARGET_CHAR));
        queue.add(new Step(new byte[]{0x10}, TARGET_CHAR));
        byte[] appInfo = Native.encodeAppInfo(appId, 2);
        if (appInfo != null) {
            queue.add(new Step(appInfo, TARGET_SETTINGS));
        }
        queue.add(new Step(new byte[]{0x11}, TARGET_CHAR));
        queue.add(new Step(new byte[]{0x3B}, TARGET_CHAR));
        queue.add(new Step(new byte[]{0x3A}, TARGET_CHAR));
        queue.add(new Step(new byte[]{0x26}, TARGET_CHAR));
        queue.add(new Step(new byte[]{0x28}, TARGET_CHAR));
        queue.add(new Step(new byte[]{0x20}, TARGET_CHAR));
        advance();
    }

    private void advance() {
        handler.removeCallbacks(stepTimeout);
        if (queue.isEmpty()) {
            awaiting = false;
            listener.onStatus("Ready");
            return;
        }
        Step step = queue.poll();
        awaiting = true;
        handler.postDelayed(stepTimeout, STEP_TIMEOUT_MS);
        BluetoothGattCharacteristic target =
                step.target == TARGET_SETTINGS ? settingsChar : requestChar;
        // The request characteristic is write-without-response; the settings
        // characteristic acknowledges writes, as in the recorded session.
        int writeType = step.target == TARGET_SETTINGS
                ? BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
                : BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE;
        listener.onLog("-> " + hex(step.payload));
        write(target, step.payload, writeType);
    }

    private void onFrame(byte[] value) {
        if (value == null || value.length == 0) {
            return;
        }
        listener.onLog("<- " + Native.decodeFrame(value));
        if (value[0] == 0x23) {
            String name = watchName(value);
            String model = Native.modelForName(name);
            modelName = model;
            listener.onModel(model != null ? model : name);
        }
        if (value[0] == 0x47) {
            handleServiceDiscovery(value);
        }
        if (awaiting) {
            awaiting = false;
            handler.removeCallbacks(stepTimeout);
            advance();
        }
    }

    private void handleServiceDiscovery(byte[] value) {
        if (value.length < 2) {
            return;
        }
        if (value[1] == 2) {
            boolean wantsBond = modelName != null
                    && Native.modelFlag(modelName, "isPairingInsteadOfLlsAl") == 1;
            if (wantsBond) {
                listener.onStatus("Bonding");
                if (device != null) {
                    device.createBond();
                }
            } else if (!writeLinkLossAlert()) {
                listener.onLog("link-loss alert characteristic absent");
            }
        } else if (value[1] == 1) {
            listener.onStatus("Handshake complete");
        }
    }

    private boolean writeLinkLossAlert() {
        if (gatt == null) {
            return false;
        }
        BluetoothGattService service = gatt.getService(LINK_LOSS_SERVICE);
        if (service == null) {
            return false;
        }
        BluetoothGattCharacteristic alert = service.getCharacteristic(LINK_LOSS_ALERT);
        if (alert == null) {
            return false;
        }
        write(alert, new byte[]{1}, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT);
        return true;
    }

    // -- write helpers ---------------------------------------------------

    private void write(BluetoothGattCharacteristic ch, byte[] value, int writeType) {
        if (gatt == null || ch == null) {
            return;
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            gatt.writeCharacteristic(ch, value, writeType);
        } else {
            ch.setWriteType(writeType);
            ch.setValue(value);
            gatt.writeCharacteristic(ch);
        }
    }

    private void writeDescriptor(BluetoothGattDescriptor descriptor, byte[] value) {
        if (gatt == null) {
            return;
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            gatt.writeDescriptor(descriptor, value);
        } else {
            descriptor.setValue(value);
            gatt.writeDescriptor(descriptor);
        }
    }

    // -- misc ------------------------------------------------------------

    private BluetoothDevice bondedWatch() {
        if (adapter == null) {
            return null;
        }
        for (BluetoothDevice bond : adapter.getBondedDevices()) {
            if (looksLikeWatch(bond.getName())) {
                return bond;
            }
        }
        return null;
    }

    private String watchName(byte[] frame) {
        int end = Math.min(frame.length, 20);
        StringBuilder text = new StringBuilder();
        for (int i = 1; i < end; i++) {
            int b = frame[i] & 0xFF;
            if (b == 0) {
                break;
            }
            text.append((char) b);
        }
        return text.toString();
    }

    private byte[] loadAppId() {
        SharedPreferences prefs = context.getSharedPreferences("wristctl", Context.MODE_PRIVATE);
        String stored = prefs.getString("app_id", null);
        if (stored != null && stored.length() == 20) {
            byte[] id = new byte[10];
            for (int i = 0; i < 10; i++) {
                id[i] = (byte) Integer.parseInt(stored.substring(i * 2, i * 2 + 2), 16);
            }
            return id;
        }
        byte[] id = new byte[10];
        new Random().nextBytes(id);
        prefs.edit().putString("app_id", hex(id)).apply();
        return id;
    }

    private static String describe(BluetoothDevice device) {
        return device.getAddress();
    }

    private static String hex(byte[] data) {
        StringBuilder text = new StringBuilder(data.length * 2);
        for (byte b : data) {
            text.append(String.format("%02x", b));
        }
        return text.toString();
    }
}
