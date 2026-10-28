/*
 * JNI bridge between the Android app and the C11 core in ../../../c.
 *
 * Keep this file thin: it exposes the generated codecs and the model registry,
 * and nothing else. No BLE, no Android types beyond jni.h, so the same core
 * keeps building for the desktop tests and for a future native activity.
 */

#include <jni.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "wristctl/generated.h"
#include "wristctl/models.h"

#define TEXT_CAP 640

/* Last codec failure, read by Native.lastError(). Thread-unsafe by design:
 * the Java side only calls these from the main thread. */
static int g_last_error = WC_OK;

static void putf(char *buf, size_t cap, size_t *used, const char *fmt, ...)
{
    va_list ap;
    int wrote;

    if (*used + 1 >= cap) {
        return;
    }
    va_start(ap, fmt);
    wrote = vsnprintf(buf + *used, cap - *used, fmt, ap);
    va_end(ap);
    if (wrote < 0) {
        return;
    }
    *used += (size_t)wrote;
    if (*used >= cap) {
        *used = cap - 1;
    }
}

static void put_hex(char *buf, size_t cap, size_t *used,
                    const uint8_t *data, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        putf(buf, cap, used, "%02x", data[i]);
    }
}

static const wc_model *model_by_name(const char *name)
{
    size_t i;

    for (i = 0; i < wc_model_count; i++) {
        if (strcmp(wc_models[i].name, name) == 0) {
            return &wc_models[i];
        }
    }
    return NULL;
}

JNIEXPORT jstring JNICALL
Java_dev_wristctl_Native_version(JNIEnv *env, jclass clazz)
{
    char buf[128];

    (void)clazz;
    snprintf(buf, sizeof buf, "wristctl core: %u class codecs, %u models, %u flags",
             (unsigned)wc_class_count(), (unsigned)wc_model_count,
             (unsigned)wc_flag_count);
    return (*env)->NewStringUTF(env, buf);
}

JNIEXPORT jint JNICALL
Java_dev_wristctl_Native_lastError(JNIEnv *env, jclass clazz)
{
    (void)env;
    (void)clazz;
    return (jint)g_last_error;
}

JNIEXPORT jbyteArray JNICALL
Java_dev_wristctl_Native_encodeTime(JNIEnv *env, jclass clazz, jint year,
                                    jint month, jint day, jint hour,
                                    jint minute, jint second, jint weekday)
{
    wc_current_time value;
    uint8_t frame[WC_LEN_CURRENT_TIME];
    size_t n = 0;
    int rc;
    jbyteArray out;

    (void)clazz;
    memset(&value, 0, sizeof value);
    value.year = (uint16_t)year;
    value.month = (uint8_t)month;
    value.day = (uint8_t)day;
    value.hour = (uint8_t)hour;
    value.minute = (uint8_t)minute;
    value.second = (uint8_t)second;
    value.weekday = (uint8_t)weekday;
    /* The watch stores 1/256 s; sub-second precision is not worth a field. */
    value.nano = 0;

    rc = wc_current_time_encode(&value, frame, sizeof frame, &n);
    g_last_error = rc;
    if (rc != WC_OK) {
        return NULL;
    }
    out = (*env)->NewByteArray(env, (jsize)n);
    if (out == NULL) {
        return NULL;
    }
    (*env)->SetByteArrayRegion(env, out, 0, (jsize)n, (const jbyte *)frame);
    return out;
}

JNIEXPORT jbyteArray JNICALL
Java_dev_wristctl_Native_encodeAppInfo(JNIEnv *env, jclass clazz, jbyteArray app_id,
                                       jint os)
{
    wc_app_info value;
    uint8_t frame[WC_LEN_APP_INFO];
    size_t n = 0;
    int rc;
    jbyteArray out;

    (void)clazz;
    if (app_id == NULL || (*env)->GetArrayLength(env, app_id) != 10) {
        g_last_error = WC_ERR_ARG;
        return NULL;
    }
    memset(&value, 0, sizeof value);
    (*env)->GetByteArrayRegion(env, app_id, 0, 10, (jbyte *)value.app_id);
    value.os = (uint8_t)os;

    rc = wc_app_info_encode(&value, frame, sizeof frame, &n);
    g_last_error = rc;
    if (rc != WC_OK) {
        return NULL;
    }
    out = (*env)->NewByteArray(env, (jsize)n);
    if (out == NULL) {
        return NULL;
    }
    (*env)->SetByteArrayRegion(env, out, 0, (jsize)n, (const jbyte *)frame);
    return out;
}

JNIEXPORT jstring JNICALL
Java_dev_wristctl_Native_modelForName(JNIEnv *env, jclass clazz, jstring ble_name)
{
    const char *name;
    const wc_model *model;
    jstring out = NULL;

    (void)clazz;
    if (ble_name == NULL) {
        return NULL;
    }
    name = (*env)->GetStringUTFChars(env, ble_name, NULL);
    if (name == NULL) {
        return NULL;
    }
    model = model_by_name(name);
    if (model == NULL) {
        model = wc_model_lookup(name);
    }
    if (model != NULL) {
        out = (*env)->NewStringUTF(env, model->name);
    }
    (*env)->ReleaseStringUTFChars(env, ble_name, name);
    return out;
}

JNIEXPORT jint JNICALL
Java_dev_wristctl_Native_modelFlag(JNIEnv *env, jclass clazz, jstring model_name,
                                   jstring flag_name)
{
    const wc_model *model;
    const char *name;
    const char *flag;
    jint result;

    (void)clazz;
    if (model_name == NULL || flag_name == NULL) {
        return -1;
    }
    name = (*env)->GetStringUTFChars(env, model_name, NULL);
    flag = (*env)->GetStringUTFChars(env, flag_name, NULL);
    if (name == NULL || flag == NULL) {
        if (name != NULL) {
            (*env)->ReleaseStringUTFChars(env, model_name, name);
        }
        if (flag != NULL) {
            (*env)->ReleaseStringUTFChars(env, flag_name, flag);
        }
        return -1;
    }
    model = model_by_name(name);
    result = (model == NULL) ? -1 : (jint)wc_model_flag(model, flag);
    (*env)->ReleaseStringUTFChars(env, flag_name, flag);
    (*env)->ReleaseStringUTFChars(env, model_name, name);
    return result;
}

static void format_frame(const uint8_t *frame, size_t n, char *buf, size_t cap)
{
    size_t used = 0;
    int rc;

    switch (frame[0]) {
    case 0x09: {
        wc_current_time v;
        rc = wc_current_time_decode(frame, n, &v);
        if (rc != WC_OK) {
            goto failure;
        }
        putf(buf, cap, &used,
             "current_time %04u-%02u-%02u %02u:%02u:%02u weekday=%u nano=%u",
             (unsigned)v.year, (unsigned)v.month, (unsigned)v.day,
             (unsigned)v.hour, (unsigned)v.minute, (unsigned)v.second,
             (unsigned)v.weekday, (unsigned)v.nano);
        break;
    }
    case 0x13: {
        wc_basic_setting v;
        rc = wc_basic_setting_decode(frame, n, &v);
        if (rc != WC_OK) {
            goto failure;
        }
        putf(buf, cap, &used,
             "basic_setting 24h=%u tone_off=%u auto_light_off=%u "
             "power_saving_off=%u light=%u date_ddmm=%u language=%u",
             (unsigned)v.time_format_24h, (unsigned)v.button_tone_off,
             (unsigned)v.auto_light_off, (unsigned)v.power_saving_off,
             (unsigned)v.light_duration, (unsigned)v.date_format_ddmm,
             (unsigned)v.language);
        break;
    }
    case 0x15: {
        wc_alarm_first v;
        rc = wc_alarm_first_decode(frame, n, &v);
        if (rc != WC_OK) {
            goto failure;
        }
        putf(buf, cap, &used, "alarm_first enabled=%u one_time=%u chime=%u %02u:%02u",
             (unsigned)v.enabled, (unsigned)v.one_time, (unsigned)v.hourly_chime,
             (unsigned)v.hour, (unsigned)v.minute);
        break;
    }
    case 0x16: {
        wc_alarm_rest v;
        size_t i;
        rc = wc_alarm_rest_decode(frame, n, &v);
        if (rc != WC_OK) {
            goto failure;
        }
        putf(buf, cap, &used, "alarm_rest");
        for (i = 0; i < 4; i++) {
            putf(buf, cap, &used, " [%u]%s%02u:%02u", (unsigned)(i + 2),
                 v.alarms[i].enabled ? "+" : "-",
                 (unsigned)v.alarms[i].hour, (unsigned)v.alarms[i].minute);
        }
        break;
    }
    case 0x23: {
        wc_watch_name v;
        rc = wc_watch_name_decode(frame, n, &v);
        if (rc != WC_OK) {
            goto failure;
        }
        putf(buf, cap, &used, "watch_name \"%s\"", v.name);
        break;
    }
    case 0x28: {
        wc_watch_condition v;
        rc = wc_watch_condition_decode(frame, n, &v);
        if (rc != WC_OK) {
            goto failure;
        }
        putf(buf, cap, &used,
             "watch_condition battery=%u temperature=%u mtu=%u log=%u flags=0x%02x",
             (unsigned)v.battery, (unsigned)v.temperature, (unsigned)v.mtu,
             (unsigned)v.log_status, (unsigned)v.flags);
        break;
    }
    case 0x2A: {
        wc_program_timer v;
        size_t i;
        rc = wc_program_timer_decode(frame, n, &v);
        if (rc != WC_OK) {
            goto failure;
        }
        putf(buf, cap, &used, "program_timer repeat=%u", (unsigned)v.repeat);
        for (i = 0; i < 6; i++) {
            putf(buf, cap, &used, " [%u]%02u:%02u/%u", (unsigned)(i + 1),
                 (unsigned)v.workouts[i].minutes, (unsigned)v.workouts[i].seconds,
                 (unsigned)v.workouts[i].beep);
        }
        break;
    }
    case 0x1D: {
        wc_dst_watch_state v;
        rc = wc_dst_watch_state_decode(frame, n, &v);
        if (rc != WC_OK) {
            goto failure;
        }
        putf(buf, cap, &used,
             "dst_watch_state home(bool=%u dst=%u auto=%u) world(bool=%u dst=%u auto=%u) "
             "city_home=%u city_world=%u",
             (unsigned)v.state_home, (unsigned)v.dst_on_home,
             (unsigned)v.dst_auto_home, (unsigned)v.state_world,
             (unsigned)v.dst_on_world, (unsigned)v.dst_auto_world,
             (unsigned)v.city_home, (unsigned)v.city_world);
        break;
    }
    case 0x1E: {
        wc_dst_setting v;
        rc = wc_dst_setting_decode(frame, n, &v);
        if (rc != WC_OK) {
            goto failure;
        }
        putf(buf, cap, &used,
             "dst_setting unit=%u city=%u timezone=%+d (15 min) dst_diff=%u rule=%u",
             (unsigned)v.unit, (unsigned)v.city, (int)v.timezone,
             (unsigned)v.dst_diff, (unsigned)v.dst_rule);
        break;
    }
    case 0x1F: {
        wc_city_name v;
        rc = wc_city_name_decode(frame, n, &v);
        if (rc != WC_OK) {
            goto failure;
        }
        putf(buf, cap, &used, "city_name unit=%u \"%s\"", (unsigned)v.unit, v.name);
        break;
    }
    case 0x22: {
        wc_app_info v;
        rc = wc_app_info_decode(frame, n, &v);
        if (rc != WC_OK) {
            goto failure;
        }
        putf(buf, cap, &used, "app_info os=%u id=", (unsigned)v.os);
        put_hex(buf, cap, &used, v.app_id, sizeof v.app_id);
        break;
    }
    case 0x47: {
        wc_service_discovery v;
        rc = wc_service_discovery_decode(frame, n, &v);
        if (rc != WC_OK) {
            goto failure;
        }
        putf(buf, cap, &used, "service_discovery command=%u", (unsigned)v.command);
        break;
    }
    case 0xFF: {
        wc_error v;
        rc = wc_error_decode(frame, n, &v);
        if (rc != WC_OK) {
            goto failure;
        }
        putf(buf, cap, &used, "error status=0x%02x class=0x%02x",
             (unsigned)v.status, (unsigned)v.class_id);
        break;
    }
    default: {
        size_t i;
        putf(buf, cap, &used, "class 0x%02x (no codec) ", (unsigned)frame[0]);
        for (i = 0; i < n; i++) {
            putf(buf, cap, &used, "%02x", (unsigned)frame[i]);
        }
        break;
    }
    }
    buf[used < cap ? used : cap - 1] = '\0';
    return;

failure:
    snprintf(buf, cap, "class 0x%02x decode failed: %d", frame[0], rc);
}

JNIEXPORT jstring JNICALL
Java_dev_wristctl_Native_decodeFrame(JNIEnv *env, jclass clazz, jbyteArray frame)
{
    uint8_t raw[64];
    char text[TEXT_CAP];
    jsize n;

    (void)clazz;
    if (frame == NULL) {
        return NULL;
    }
    n = (*env)->GetArrayLength(env, frame);
    if (n <= 0) {
        return NULL;
    }
    /* The longest spec'd frame is 20 bytes; 64 covers unknown classes too. */
    if (n > (jsize)sizeof raw) {
        n = (jsize)sizeof raw;
    }
    (*env)->GetByteArrayRegion(env, frame, 0, n, (jbyte *)raw);
    format_frame(raw, (size_t)n, text, sizeof text);
    g_last_error = WC_OK;
    return (*env)->NewStringUTF(env, text);
}
