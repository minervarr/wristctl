package dev.wristctl;

import android.Manifest;
import android.app.Activity;
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
import android.content.Intent;
import android.content.pm.PackageManager;
import android.location.LocationManager;
import android.media.Ringtone;
import android.media.RingtoneManager;
import android.app.NotificationManager;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.provider.Settings;

import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Deque;
import java.util.List;
import java.util.Locale;
import java.util.UUID;

/**
 * The Android GATT transport. WatchNotificationListener is the only other
 * Java class and supplies opt-in local notification events.
 *
 * <p>NativeActivity has no BLE API: Android exposes scan/GATT/bond through
 * Java only. This class therefore scans, connects, discovers, subscribes and
 * writes; it deliberately contains no handshake and no frame decoding.
 * {@code wc_session} in the C core owns all of that, and receives raw
 * notifications through the native callbacks below.
 *
 * <p>It is loaded from native code through the activity's classloader — a
 * native thread's FindClass cannot see app classes — and the static block loads
 * libwristctl so the JVM knows where the native callbacks live. NativeActivity
 * already dlopen()s that library, but that does not register it with the JVM.
 *
 * <p>Every GATT call runs on the main looper. The worker thread calls the
 * public methods through JNI and blocks on a condition variable in
 * android_link.cc; the callbacks here wake it.
 */
public final class BleLink {
    static {
        System.loadLibrary("wristctl");
    }

    private static final UUID SERVICE =
            UUID.fromString("26eb000d-b012-49a8-b1f8-394fb2032b0f");
    private static final UUID REQUEST =
            UUID.fromString("26eb002c-b012-49a8-b1f8-394fb2032b0f");
    private static final UUID SETTINGS =
            UUID.fromString("26eb002d-b012-49a8-b1f8-394fb2032b0f");
    private static final UUID NOTIFY_WRITE =
            UUID.fromString("26eb0030-b012-49a8-b1f8-394fb2032b0f");
    private static final UUID CCCD =
            UUID.fromString("00002902-0000-1000-8000-00805f9b34fb");
    private static final UUID LINK_LOSS_SERVICE =
            UUID.fromString("00001803-0000-1000-8000-00805f9b34fb");
    private static final UUID LINK_LOSS_ALERT =
            UUID.fromString("00002a06-0000-1000-8000-00805f9b34fb");
    private static final UUID IMMEDIATE_ALERT_SERVICE =
            UUID.fromString("26eb0005-b012-49a8-b1f8-394fb2032b0f");
    private static final long FINDER_TIMEOUT_MS = 60000;

    private static Ringtone finderRingtone;
    private static Runnable finderTimeout;

    private static final String[] NAME_PREFIXES = {
            "CASIO", "GBX", "GBD", "GPR", "GWF", "GST", "GMW", "MRG", "DW-", "GM-"
    };

    private static final int REQUEST_PERMISSIONS = 0x5753;
    private static final int PERMISSION_POLL_MS = 250;
    private static final int PERMISSION_TIMEOUT_MS = 60000;
    private static final int WRITE_TIMEOUT_MS = 5000;
    private static final int DESCRIPTOR_TIMEOUT_MS = 2000;
    private static final int BOND_TIMEOUT_MS = 30000;

    private final Activity activity;
    private final Context context;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final List<BluetoothGattCharacteristic> notifying = new ArrayList<>();

    private long handle;
    private BluetoothAdapter adapter;
    private BluetoothLeScanner scanner;
    private BluetoothGatt gatt;
    private BluetoothDevice device;
    private String address = "";
    private String deviceName = "";
    private String lastError = "";

    private int generation;
    private boolean scanning;
    private boolean connecting;
    private boolean discovered;
    private boolean closed;
    private boolean bonding;
    private boolean bondReconnect;
    private int bondGeneration;
    private long bondStartedMs;
    private int cccdIndex;

    private BluetoothGattCharacteristic requestChar;
    private BluetoothGattCharacteristic settingsChar;
    private BluetoothGattCharacteristic notifyChar;
    private BluetoothGattCharacteristic linkLossChar;

    private int nextSeq = 1;
    private int pendingSeq;
    private BluetoothGattCharacteristic pendingChar;

    private int pendingScanMs;
    private int pendingConnectMs;
    private boolean pendingStart;
    private boolean scanOnly;
    private long permissionStartedMs;

    private Runnable scanTimeout;
    private Runnable connectTimeout;
    private Runnable writeTimeout;
    private Runnable descriptorTimeout;

    BleLink(Activity activity) {
        this.activity = activity;
        this.context = activity.getApplicationContext();
    }

    // ── Called from android_link.cc ─────────────────────────────────────────

    void setHandle(long value) {
        handle = value;
    }

    boolean connect(long h, int scanMs, int connectMs) {
        if (closed || h != handle) {
            return false;
        }
        scanOnly = false;
        pendingScanMs = scanMs;
        pendingConnectMs = connectMs;
        pendingStart = true;
        final int gen = ++generation;
        handler.post(() -> start(gen));
        return true;
    }

    boolean connectTo(long h, String address, int scanMs, int connectMs) {
        if (closed || h != handle || address == null || address.isEmpty()) {
            return false;
        }
        final int gen = ++generation;
        handler.post(() -> {
            if (gen != generation || closed) {
                return;
            }
            stopScan();
            pendingScanMs = scanMs;
            pendingConnectMs = connectMs;
            scanOnly = false;
            pendingStart = false;
            if (!permissionsGranted()) {
                nativeOnScanState(handle, 2);
                fail("Bluetooth permission denied");
                return;
            }
            BluetoothManager manager = (BluetoothManager) context.getSystemService(
                    Context.BLUETOOTH_SERVICE);
            adapter = manager != null ? manager.getAdapter() : null;
            if (adapter == null) {
                fail("no Bluetooth adapter");
                return;
            }
            if (!adapter.isEnabled()) {
                nativeOnScanState(handle, 3);
                fail("Bluetooth is off");
                return;
            }
            BluetoothDevice target;
            try {
                target = adapter.getRemoteDevice(address);
            } catch (IllegalArgumentException error) {
                fail("invalid watch address");
                return;
            } catch (SecurityException error) {
                fail("Bluetooth permission denied");
                return;
            }
            deviceName = "";
            connectTo(target, gen);
        });
        return true;
    }

    boolean startScan(long h, int scanMs) {
        if (closed || h != handle) {
            return false;
        }
        final int gen = ++generation;
        handler.post(() -> {
            if (gen != generation || closed) {
                return;
            }
            scanOnly = true;
            pendingScanMs = scanMs;
            pendingConnectMs = 20000;
            pendingStart = true;
            start(gen);
        });
        return true;
    }

    void stopScan(long h) {
        if (closed || h != handle) {
            return;
        }
        handler.post(this::stopScan);
    }

    void requestPermissions(long h) {
        if (closed || h != handle) {
            return;
        }
        handler.post(() -> {
            scanOnly = true;
            pendingStart = true;
            nativeOnScanState(handle, 2);
            requestPermissions();
        });
    }

    void openAppSettings(long h) {
        if (closed || h != handle) {
            return;
        }
        handler.post(() -> {
            try {
                Intent intent = new Intent(
                        android.provider.Settings.ACTION_APPLICATION_DETAILS_SETTINGS);
                intent.setData(android.net.Uri.parse("package:" + context.getPackageName()));
                intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                activity.startActivity(intent);
            } catch (Exception error) {
                lastError = "cannot open app settings: " + error.getMessage();
            }
        });
    }

    void openLocationSettings(long h) {
        if (closed || h != handle) {
            return;
        }
        handler.post(() -> {
            try {
                activity.startActivity(new Intent(
                        android.provider.Settings.ACTION_LOCATION_SOURCE_SETTINGS));
            } catch (Exception error) {
                lastError = "cannot open location settings: " + error.getMessage();
            }
        });
    }

    void enableBluetooth(long h) {
        if (closed || h != handle) {
            return;
        }
        handler.post(() -> {
            try {
                activity.startActivity(new Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE));
            } catch (SecurityException error) {
                lastError = "Bluetooth permission denied";
            } catch (Exception error) {
                lastError = "cannot request Bluetooth: " + error.getMessage();
            }
        });
    }

    void openNotificationSettings(long h) {
        if (closed || h != handle) {
            return;
        }
        handler.post(() -> {
            try {
                activity.startActivity(new Intent(
                        Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS));
            } catch (Exception error) {
                lastError = "cannot open notification access: " + error.getMessage();
            }
        });
    }

    boolean notificationAccess(long h) {
        if (closed || h != handle) {
            return false;
        }
        if (Build.VERSION.SDK_INT >= 27) {
            NotificationManager manager = (NotificationManager) context.getSystemService(
                    Context.NOTIFICATION_SERVICE);
            return manager != null && manager.isNotificationListenerAccessGranted(
                    new android.content.ComponentName(
                            context, WatchNotificationListener.class));
        }
        String enabled = Settings.Secure.getString(context.getContentResolver(),
                "enabled_notification_listeners");
        return enabled != null && enabled.contains(context.getPackageName());
    }

    void stopPhoneFinder(long h) {
        if (h == handle) {
            handler.post(BleLink::stopFinderSound);
        }
    }

    void disconnect(long h) {
        if (h != handle) {
            return;
        }
        closed = true;
        handler.post(this::shutdown);
    }

    int write(long h, int kind, byte[] data) {
        if (closed || h != handle || data == null || data.length == 0) {
            return -1;
        }
        final int seq = nextSeq++;
        final int gen = generation;
        handler.post(() -> doWrite(h, gen, seq, kind, data, 1));
        return seq;
    }

    boolean writeLinkLossAlert(long h, int level) {
        if (closed || h != handle || linkLossChar == null || gatt == null) {
            return false;
        }
        handler.post(() -> writeCharacteristic(linkLossChar,
                new byte[]{(byte) level},
                BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT));
        return true;
    }

    boolean isBonded(long h) {
        if (closed || h != handle || device == null) {
            return false;
        }
        try {
            return device.getBondState() == BluetoothDevice.BOND_BONDED;
        } catch (SecurityException denied) {
            return false;
        }
    }

    int bondState(long h) {
        if (closed || h != handle || device == null) {
            return BluetoothDevice.BOND_NONE;
        }
        try {
            return device.getBondState();
        } catch (SecurityException denied) {
            return BluetoothDevice.BOND_NONE;
        }
    }

    boolean pair(long h) {
        if (closed || h != handle || device == null) {
            return false;
        }
        final int gen = ++bondGeneration;
        handler.post(() -> startBond(h, gen));
        return true;
    }

    String address(long h) {
        return h == handle ? address : "";
    }

    String name(long h) {
        return h == handle ? deviceName : "";
    }

    String lastError(long h) {
        return h == handle ? lastError : "";
    }

    // ── Connection ──────────────────────────────────────────────────────────

    private void start(int gen) {
        if (gen != generation || closed) {
            return;
        }
        lastError = "";
        address = "";
        deviceName = "";
        if (!permissionsGranted()) {
            pendingStart = true;
            nativeOnScanState(handle, 2);
            requestPermissions();
            return;
        }
        pendingStart = false;

        BluetoothManager manager =
                (BluetoothManager) context.getSystemService(Context.BLUETOOTH_SERVICE);
        adapter = manager != null ? manager.getAdapter() : null;
        if (adapter == null) {
            fail("no Bluetooth adapter");
            return;
        }
        if (!adapter.isEnabled()) {
            nativeOnScanState(handle, 3);
            try {
                activity.startActivity(new Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE));
            } catch (SecurityException ignored) {
                lastError = "Bluetooth permission denied";
            } catch (Exception ignored) {
                // The status line still says Bluetooth is off; the worker retries.
            }
            fail("Bluetooth is off");
            return;
        }
        if (Build.VERSION.SDK_INT <= 30 && !locationEnabled()) {
            nativeOnScanState(handle, 4);
            fail("location services are off (needed for BLE scanning)");
            return;
        }
        scanner = adapter.getBluetoothLeScanner();
        if (scanner == null) {
            fail("no BLE scanner");
            return;
        }
        startScan(gen);
    }

    private void startScan(int gen) {
        // Bonded watches first: they are usable without seeing an
        // advertisement, and the user can reconnect to one directly.
        try {
            for (BluetoothDevice bonded : adapter.getBondedDevices()) {
                String name;
                try {
                    name = bonded.getName();
                } catch (SecurityException ignored) {
                    name = null;
                }
                if (name != null && looksLikeWatch(name)) {
                    nativeOnDevice(handle, name, bonded.getAddress(), 0, true);
                }
            }
        } catch (SecurityException ignored) {
            // The scan below will report them again if they advertise.
        }

        ScanSettings settings = new ScanSettings.Builder()
                .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
                .build();
        scanning = true;
        nativeOnScanState(handle, 1);
        try {
            scanner.startScan(null, settings, scanCallback);
        } catch (SecurityException error) {
            scanning = false;
            nativeOnScanState(handle, 2);
            fail("Bluetooth permission denied");
            return;
        }
        scanTimeout = () -> {
            if (gen == generation && scanning) {
                stopScan();
                if (!scanOnly) {
                    fail("watch not found");
                }
            }
        };
        handler.postDelayed(scanTimeout, Math.max(1000, pendingScanMs));
    }

    private void stopScan() {
        scanning = false;
        if (scanTimeout != null) {
            handler.removeCallbacks(scanTimeout);
            scanTimeout = null;
        }
        if (scanner != null) {
            try {
                scanner.stopScan(scanCallback);
            } catch (SecurityException denied) {
                lastError = "Bluetooth permission denied";
            } catch (Exception ignored) {
                // Already stopped by the stack.
            }
        }
        if (!closed && handle != 0) {
            nativeOnScanState(handle, 0);
        }
    }

    private final ScanCallback scanCallback = new ScanCallback() {
        @Override
        public void onScanResult(int callbackType, ScanResult result) {
            if (!scanning) {
                return;
            }
            BluetoothDevice candidate = result.getDevice();
            String name = result.getScanRecord() != null
                    ? result.getScanRecord().getDeviceName() : null;
            if (name == null) {
                try {
                    name = candidate.getName();
                } catch (SecurityException ignored) {
                    name = null;
                }
            }
            if (name == null || name.isEmpty()) {
                return;
            }
            final String address;
            final boolean bonded;
            try {
                address = candidate.getAddress();
                bonded = candidate.getBondState() == BluetoothDevice.BOND_BONDED;
            } catch (SecurityException denied) {
                return;
            }
            nativeOnDevice(handle, name, address, result.getRssi(), bonded);

            if (scanOnly || !looksLikeWatch(name)) {
                return;
            }
            stopScan();
            deviceName = name;
            connectTo(candidate, generation);
        }

        @Override
        public void onScanFailed(int errorCode) {
            scanning = false;
            nativeOnScanState(handle, 0);
            fail("scan failed: " + errorCode);
        }
    };

    private void connectTo(BluetoothDevice target, int gen) {
        stopScan();
        connecting = true;
        device = target;
        address = target.getAddress();
        try {
            gatt = target.connectGatt(context, false, gattCallback,
                    BluetoothDevice.TRANSPORT_LE);
        } catch (SecurityException error) {
            fail("connectGatt denied: " + error.getMessage());
            return;
        }
        if (gatt == null) {
            fail("connectGatt returned null");
            return;
        }
        connectTimeout = () -> {
            if (gen == generation && connecting && !discovered) {
                fail("connect timed out");
            }
        };
        handler.postDelayed(connectTimeout, Math.max(2000, pendingConnectMs));
    }

    private final BluetoothGattCallback gattCallback = new BluetoothGattCallback() {
        @Override
        public void onConnectionStateChange(BluetoothGatt g, int status, int newState) {
            if (newState == BluetoothGatt.STATE_CONNECTED) {
                connecting = true;
                try {
                    if (!g.discoverServices()) {
                        fail("discoverServices failed");
                    }
                } catch (SecurityException denied) {
                    fail("Bluetooth permission denied");
                }
                return;
            }
            if (newState == BluetoothGatt.STATE_DISCONNECTED) {
                if (bonding || bondReconnect) {
                    // createBond often drops the link; the bond poll reconnects.
                    closeGatt();
                    return;
                }
                final boolean wasUp = discovered || connecting;
                final int code = status;
                connecting = false;
                discovered = false;
                closeGatt();
                stopScan();
                if (wasUp) {
                    lastError = "link lost (status " + code + ")";
                    nativeOnDisconnected(handle, code);
                } else {
                    fail("connect failed (status " + code + ")");
                }
            }
        }

        @Override
        public void onServicesDiscovered(BluetoothGatt g, int status) {
            if (status != BluetoothGatt.GATT_SUCCESS) {
                fail("service discovery failed: " + status);
                return;
            }
            mapService(g);
        }

        @Override
        public void onCharacteristicChanged(BluetoothGatt g,
                                            BluetoothGattCharacteristic ch,
                                            byte[] value) {
            deliver(ch, value);
        }

        @Override
        @SuppressWarnings("deprecation")
        public void onCharacteristicChanged(BluetoothGatt g,
                                            BluetoothGattCharacteristic ch) {
            deliver(ch, ch.getValue());
        }

        @Override
        public void onCharacteristicWrite(BluetoothGatt g,
                                          BluetoothGattCharacteristic ch,
                                          int status) {
            if (writeTimeout != null) {
                handler.removeCallbacks(writeTimeout);
                writeTimeout = null;
            }
            if (pendingSeq != 0 && ch == pendingChar) {
                final int seq = pendingSeq;
                pendingSeq = 0;
                pendingChar = null;
                nativeOnWrite(handle, seq,
                        status == BluetoothGatt.GATT_SUCCESS ? 0 : -1);
            }
        }

        @Override
        public void onDescriptorWrite(BluetoothGatt g,
                                      BluetoothGattDescriptor descriptor,
                                      int status) {
            if (descriptorTimeout != null) {
                handler.removeCallbacks(descriptorTimeout);
                descriptorTimeout = null;
            }
            if (status != BluetoothGatt.GATT_SUCCESS) {
                lastError = "CCCD write failed: " + status;
            }
            cccdIndex++;
            enableNextNotification();
        }
    };

    private void deliver(BluetoothGattCharacteristic ch, byte[] value) {
        if (value == null || value.length == 0 || closed) {
            return;
        }
        if (ch.getService() != null
                && IMMEDIATE_ALERT_SERVICE.equals(ch.getService().getUuid())
                && LINK_LOSS_ALERT.equals(ch.getUuid())) {
            if (value[0] == 2) {
                startFinderSound();
            } else if (value[0] == 0) {
                stopFinderSound();
            }
            return;
        }
        nativeOnFrame(handle, value);
    }

    private void startFinderSound() {
        stopFinderSound();
        try {
            android.net.Uri uri = RingtoneManager.getDefaultUri(
                    RingtoneManager.TYPE_ALARM);
            if (uri == null) {
                uri = RingtoneManager.getDefaultUri(RingtoneManager.TYPE_NOTIFICATION);
            }
            finderRingtone = RingtoneManager.getRingtone(context, uri);
            if (finderRingtone == null) {
                return;
            }
            finderRingtone.play();
            finderTimeout = BleLink::stopFinderSound;
            handler.postDelayed(finderTimeout, FINDER_TIMEOUT_MS);
        } catch (Exception error) {
            lastError = "cannot play phone finder sound: " + error.getMessage();
            stopFinderSound();
        }
    }

    private static void stopFinderSound() {
        if (finderTimeout != null) {
            new Handler(Looper.getMainLooper()).removeCallbacks(finderTimeout);
            finderTimeout = null;
        }
        if (finderRingtone != null) {
            try {
                finderRingtone.stop();
            } catch (Exception ignored) {
                // The audio track may already have been released by Android.
            }
            finderRingtone = null;
        }
    }

    private void mapService(BluetoothGatt g) {
        BluetoothGattService service = g.getService(SERVICE);
        if (service == null) {
            fail("Casio service not found");
            return;
        }
        requestChar = service.getCharacteristic(REQUEST);
        settingsChar = service.getCharacteristic(SETTINGS);
        notifyChar = service.getCharacteristic(NOTIFY_WRITE);
        if (requestChar == null || settingsChar == null) {
            fail("Casio characteristics missing");
            return;
        }

        notifying.clear();
        for (BluetoothGattCharacteristic ch : service.getCharacteristics()) {
            final int properties = ch.getProperties();
            if ((properties & (BluetoothGattCharacteristic.PROPERTY_NOTIFY
                    | BluetoothGattCharacteristic.PROPERTY_INDICATE)) != 0) {
                notifying.add(ch);
            }
        }
        if (notifying.isEmpty()) {
            fail("no notifying characteristics");
            return;
        }

        BluetoothGattService lla = g.getService(LINK_LOSS_SERVICE);
        linkLossChar = lla != null ? lla.getCharacteristic(LINK_LOSS_ALERT) : null;
        BluetoothGattService immediate = g.getService(IMMEDIATE_ALERT_SERVICE);
        BluetoothGattCharacteristic immediateChar = immediate != null
                ? immediate.getCharacteristic(LINK_LOSS_ALERT) : null;
        if (immediateChar != null
                && (immediateChar.getProperties()
                        & BluetoothGattCharacteristic.PROPERTY_NOTIFY) != 0) {
            notifying.add(immediateChar);
        }

        cccdIndex = 0;
        enableNextNotification();
    }

    private void enableNextNotification() {
        if (gatt == null || closed) {
            return;
        }
        try {
            while (cccdIndex < notifying.size()) {
                BluetoothGattCharacteristic ch = notifying.get(cccdIndex);
                if (!gatt.setCharacteristicNotification(ch, true)) {
                    lastError = "notification enable failed for " + ch.getUuid();
                    cccdIndex++;
                    continue;
                }
                BluetoothGattDescriptor descriptor = ch.getDescriptor(CCCD);
                if (descriptor == null) {
                    cccdIndex++;
                    continue;
                }
                final boolean ok;
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                    ok = gatt.writeDescriptor(descriptor,
                            BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE)
                            == android.bluetooth.BluetoothStatusCodes.SUCCESS;
                } else {
                    //noinspection deprecation
                    descriptor.setValue(BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE);
                    ok = gatt.writeDescriptor(descriptor);
                }
                if (ok) {
                    descriptorTimeout = () -> {
                        lastError = "CCCD write timed out";
                        cccdIndex++;
                        enableNextNotification();
                    };
                    handler.postDelayed(descriptorTimeout, DESCRIPTOR_TIMEOUT_MS);
                    return;
                }
                lastError = "CCCD write rejected for " + ch.getUuid();
                cccdIndex++;
            }
            onReady();
        } catch (SecurityException denied) {
            fail("Bluetooth permission denied");
        }
    }

    private void onReady() {
        if (gatt == null || closed) {
            return;
        }
        if (connectTimeout != null) {
            handler.removeCallbacks(connectTimeout);
            connectTimeout = null;
        }
        discovered = true;
        connecting = false;
        lastError = "";
        if (bondReconnect) {
            bondReconnect = false;
            bonding = false;
            nativeOnBond(handle, 1);
            return;
        }
        nativeOnConnected(handle, address, deviceName);
    }

    // ── Writes ──────────────────────────────────────────────────────────────

    private void doWrite(long h, int gen, int seq, int kind, byte[] data,
                         int attempt) {
        if (closed || h != handle || gen != generation || gatt == null
                || !discovered) {
            nativeOnWrite(h, seq, -1);
            return;
        }
        if (pendingSeq != 0) {
            nativeOnWrite(h, seq, -1);
            return;
        }
        BluetoothGattCharacteristic ch;
        switch (kind) {
            case 0:  // WC_WRITE_REQUEST
            case 2:  // WC_WRITE_NOTIFICATION
                ch = kind == 0 ? requestChar : notifyChar;
                break;
            case 1:  // WC_WRITE_ALL
            case 3:  // WC_WRITE_ALL_COMMAND
                ch = settingsChar;
                break;
            default:
                nativeOnWrite(h, seq, -1);
                return;
        }
        if (ch == null) {
            nativeOnWrite(h, seq, -1);
            return;
        }

        final int writeType = kind == 1
                ? BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
                : BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE;

        boolean ok;
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                ok = gatt.writeCharacteristic(ch, data, writeType)
                        == android.bluetooth.BluetoothStatusCodes.SUCCESS;
            } else {
                //noinspection deprecation
                ch.setWriteType(writeType);
                //noinspection deprecation
                ch.setValue(data);
                //noinspection deprecation
                ok = gatt.writeCharacteristic(ch);
            }
        } catch (SecurityException denied) {
            nativeOnWrite(h, seq, -1);
            return;
        }
        if (!ok) {
            if (attempt < 8) {
                handler.postDelayed(
                        () -> doWrite(h, gen, seq, kind, data, attempt + 1), 25);
            } else {
                nativeOnWrite(h, seq, -1);
            }
            return;
        }

        if (writeType == BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE) {
            // No ATT response is coming. The next native write may still
            // collide with the controller if the watch answered before the
            // write completed, so the retry path above remains the safety net.
            pendingChar = null;
            nativeOnWrite(h, seq, 0);
            return;
        }

        pendingSeq = seq;
        pendingChar = ch;
        writeTimeout = () -> {
            if (pendingSeq == seq) {
                pendingSeq = 0;
                pendingChar = null;
                nativeOnWrite(h, seq, -1);
            }
        };
        handler.postDelayed(writeTimeout, WRITE_TIMEOUT_MS);
    }

    private void writeCharacteristic(BluetoothGattCharacteristic ch,
                                     byte[] data, int writeType) {
        if (gatt == null) {
            return;
        }
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                gatt.writeCharacteristic(ch, data, writeType);
            } else {
                //noinspection deprecation
                ch.setWriteType(writeType);
                //noinspection deprecation
                ch.setValue(data);
                //noinspection deprecation
                gatt.writeCharacteristic(ch);
            }
        } catch (SecurityException denied) {
            lastError = "Bluetooth permission denied";
        }
    }

    // ── Bond ────────────────────────────────────────────────────────────────

    private void startBond(long h, int gen) {
        if (closed || h != handle || device == null) {
            nativeOnBond(h, 0);
            return;
        }
        try {
            if (device.getBondState() == BluetoothDevice.BOND_BONDED) {
                nativeOnBond(h, 1);
                return;
            }
        } catch (SecurityException denied) {
            nativeOnBond(h, 0);
            return;
        }
        if (bonding) {
            return;
        }
        bonding = true;
        bondStartedMs = SystemClock.uptimeMillis();
        boolean started;
        try {
            started = device.createBond();
        } catch (SecurityException error) {
            lastError = "createBond denied: " + error.getMessage();
            started = false;
        }
        if (!started) {
            bonding = false;
            nativeOnBond(h, 0);
            return;
        }
        pollBond(h, gen);
    }

    private void pollBond(long h, int gen) {
        if (closed || h != handle || gen != bondGeneration || !bonding
                || device == null) {
            return;
        }
        final int state;
        try {
            state = device.getBondState();
        } catch (SecurityException denied) {
            bonding = false;
            lastError = "Bluetooth permission denied";
            nativeOnBond(h, 0);
            return;
        }
        final long elapsed = SystemClock.uptimeMillis() - bondStartedMs;
        if (state == BluetoothDevice.BOND_BONDED) {
            bonding = false;
            if (gatt == null || !discovered) {
                reconnectAfterBond(h, gen);
            } else {
                nativeOnBond(h, 1);
            }
            return;
        }
        if (state == BluetoothDevice.BOND_NONE && elapsed > 2000) {
            bonding = false;
            lastError = "bond failed";
            nativeOnBond(h, 0);
            return;
        }
        if (elapsed > BOND_TIMEOUT_MS) {
            bonding = false;
            lastError = "bond timed out";
            nativeOnBond(h, 0);
            return;
        }
        handler.postDelayed(() -> pollBond(h, gen), 250);
    }

    private void reconnectAfterBond(long h, int gen) {
        if (device == null || closed) {
            nativeOnBond(h, 0);
            return;
        }
        bondReconnect = true;
        closeGatt();
        discovered = false;
        connecting = true;
        try {
            gatt = device.connectGatt(context, false, gattCallback,
                    BluetoothDevice.TRANSPORT_LE);
        } catch (SecurityException error) {
            bondReconnect = false;
            lastError = "reconnect after bond denied: " + error.getMessage();
            nativeOnBond(h, 0);
            return;
        }
        if (gatt == null) {
            bondReconnect = false;
            nativeOnBond(h, 0);
            return;
        }
        connectTimeout = () -> {
            if (gen == generation && bondReconnect) {
                bondReconnect = false;
                closeGatt();
                lastError = "reconnect after bond timed out";
                nativeOnBond(h, 0);
            }
        };
        handler.postDelayed(connectTimeout, Math.max(4000, pendingConnectMs));
    }

    // ── Permissions ─────────────────────────────────────────────────────────

    private boolean permissionsGranted() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            return check(Manifest.permission.BLUETOOTH_SCAN)
                    && check(Manifest.permission.BLUETOOTH_CONNECT);
        }
        return check(Manifest.permission.ACCESS_FINE_LOCATION);
    }

    private boolean check(String permission) {
        return activity.checkSelfPermission(permission)
                == PackageManager.PERMISSION_GRANTED;
    }

    private void requestPermissions() {
        List<String> missing = new ArrayList<>();
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            if (!check(Manifest.permission.BLUETOOTH_SCAN)) {
                missing.add(Manifest.permission.BLUETOOTH_SCAN);
            }
            if (!check(Manifest.permission.BLUETOOTH_CONNECT)) {
                missing.add(Manifest.permission.BLUETOOTH_CONNECT);
            }
        } else if (!check(Manifest.permission.ACCESS_FINE_LOCATION)) {
            missing.add(Manifest.permission.ACCESS_FINE_LOCATION);
        }
        if (missing.isEmpty()) {
            if (pendingStart) {
                pendingStart = false;
                final int gen = generation;
                handler.post(() -> start(gen));
            }
            return;
        }
        permissionStartedMs = SystemClock.uptimeMillis();
        try {
            activity.requestPermissions(missing.toArray(new String[0]),
                    REQUEST_PERMISSIONS);
        } catch (Exception error) {
            fail("permission request failed: " + error.getMessage());
            return;
        }
        handler.postDelayed(this::pollPermissions, PERMISSION_POLL_MS);
    }

    private void pollPermissions() {
        if (closed) {
            return;
        }
        if (permissionsGranted()) {
            if (pendingStart) {
                pendingStart = false;
                final int gen = generation;
                handler.post(() -> start(gen));
            }
            return;
        }
        if (SystemClock.uptimeMillis() - permissionStartedMs
                < PERMISSION_TIMEOUT_MS) {
            handler.postDelayed(this::pollPermissions, PERMISSION_POLL_MS);
        } else {
            fail("Bluetooth permission denied");
        }
    }

    private boolean locationEnabled() {
        LocationManager manager =
                (LocationManager) context.getSystemService(Context.LOCATION_SERVICE);
        if (manager == null) {
            return true;
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            return manager.isLocationEnabled();
        }
        try {
            return manager.isProviderEnabled(LocationManager.GPS_PROVIDER)
                    || manager.isProviderEnabled(LocationManager.NETWORK_PROVIDER);
        } catch (Exception ignored) {
            return true;
        }
    }

    // ── Teardown / helpers ──────────────────────────────────────────────────

    private void fail(String message) {
        lastError = message;
        connecting = false;
        discovered = false;
        stopScan();
        closeGatt();
        if (!closed) {
            nativeOnError(handle, message);
        }
    }

    private void shutdown() {
        closed = true;
        stopScan();
        if (connectTimeout != null) {
            handler.removeCallbacks(connectTimeout);
            connectTimeout = null;
        }
        if (writeTimeout != null) {
            handler.removeCallbacks(writeTimeout);
            writeTimeout = null;
        }
        if (descriptorTimeout != null) {
            handler.removeCallbacks(descriptorTimeout);
            descriptorTimeout = null;
        }
        bonding = false;
        bondReconnect = false;
        connecting = false;
        discovered = false;
        pendingSeq = 0;
        pendingChar = null;
        closeGatt();
    }

    private void closeGatt() {
        stopFinderSound();
        if (gatt != null) {
            try {
                gatt.disconnect();
            } catch (SecurityException ignored) {
                // Permission was revoked while tearing down the link.
            } catch (Exception ignored) {
                // Already gone.
            }
            try {
                gatt.close();
            } catch (SecurityException ignored) {
                // Permission was revoked while tearing down the link.
            } catch (Exception ignored) {
                // Already closed.
            }
            gatt = null;
        }
    }

    private static boolean looksLikeWatch(String name) {
        if (name == null) {
            return false;
        }
        final String upper = name.toUpperCase(Locale.ROOT);
        for (String prefix : NAME_PREFIXES) {
            if (upper.startsWith(prefix)) {
                return true;
            }
        }
        return false;
    }

    // ── Native down-calls ───────────────────────────────────────────────────

    private native void nativeOnConnected(long handle, String address,
                                          String name);

    private native void nativeOnDisconnected(long handle, int status);

    private native void nativeOnFrame(long handle, byte[] data);

    private native void nativeOnWrite(long handle, int seq, int status);

    private native void nativeOnBond(long handle, int bonded);

    private native void nativeOnError(long handle, String message);

    private native void nativeOnScanState(long handle, int state);

    private native void nativeOnDevice(long handle, String name, String address,
                                       int rssi, boolean bonded);
}
