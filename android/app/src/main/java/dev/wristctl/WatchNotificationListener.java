package dev.wristctl;

import android.app.Notification;
import android.service.notification.NotificationListenerService;
import android.service.notification.StatusBarNotification;

public final class WatchNotificationListener extends NotificationListenerService {
    static {
        System.loadLibrary("wristctl");
    }

    @Override
    public void onNotificationPosted(StatusBarNotification posted) {
        if (posted == null) {
            return;
        }
        Notification notification = posted.getNotification();
        if (notification == null || notification.extras == null) {
            return;
        }
        CharSequence title = notification.extras.getCharSequence(
                Notification.EXTRA_TITLE);
        CharSequence text = notification.extras.getCharSequence(
                Notification.EXTRA_TEXT);
        int type = 0;
        if (Notification.CATEGORY_CALL.equals(notification.category)) {
            type = 1;
        } else if (Notification.CATEGORY_EMAIL.equals(notification.category)) {
            type = 2;
        } else if (Notification.CATEGORY_MESSAGE.equals(notification.category)
                || Notification.CATEGORY_SOCIAL.equals(notification.category)) {
            type = 3;
        }
        nativeOnNotification(type, posted.getPackageName(),
                title != null ? title.toString() : "",
                text != null ? text.toString() : "");
    }

    private native void nativeOnNotification(int type, String app,
                                             String title, String text);
}
