package dev.wristctl;

/**
 * The only surface between Java and the C11 core.
 *
 * Everything here is a thin wrapper over libs/firstparty/wristctl_core/include/wristctl/{generated,models}.h;
 * the codecs are the product, this class just carries bytes across JNI.
 */
final class Native {
    static {
        System.loadLibrary("wristctl");
    }

    private Native() {
    }

    /** "wristctl core: 13 class codecs, 137 models, 107 flags" */
    static native String version();

    /** Last codec error code from {@code proto.h}; 0 is success. */
    static native int lastError();

    /** Encode class 0x09. The watch expects local time. Null on failure. */
    static native byte[] encodeTime(int year, int month, int day, int hour,
                                    int minute, int second, int weekday);

    /** Encode class 0x22 from a 10-byte app id. Null on failure. */
    static native byte[] encodeAppInfo(byte[] appId, int os);

    /** Human-readable decode of one frame, for the console. */
    static native String decodeFrame(byte[] frame);

    /** Model name for an advertised BLE name, or null when unknown. */
    static native String modelForName(String bleName);

    /** 1/0 for a capability flag, -1 when the model or flag is unknown. */
    static native int modelFlag(String modelName, String flagName);
}
