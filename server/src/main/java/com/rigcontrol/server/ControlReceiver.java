package com.rigcontrol.server;

import android.os.SystemClock;
import android.view.InputDevice;
import android.view.KeyCharacterMap;
import android.view.KeyEvent;
import android.view.MotionEvent;

import com.rigcontrol.server.wrappers.InputManager;

import java.io.DataInputStream;
import java.io.IOException;
import java.io.InputStream;

/**
 * Đọc control message từ desktop (docs/PROTOCOL.md §4) và inject vào hệ thống qua InputManager.
 *
 * Chuột được ánh xạ thành sự kiện cảm ứng (SOURCE_TOUCHSCREEN): nút trái nhấn→ACTION_DOWN,
 * di chuyển khi giữ→ACTION_MOVE, thả→ACTION_UP. Toạ độ nhận theo pixel màn hình thiết bị nên
 * inject thẳng không cần scale. KEY/TEXT inject qua bàn phím ảo; DEVICE_ACTION giao DeviceController.
 */
public final class ControlReceiver implements Runnable {
    private final DataInputStream in;
    private final DeviceController device;

    private InputManager input;
    private long lastDownTime;

    public ControlReceiver(InputStream in, ScreenEncoder encoder, DeviceController device) {
        this.in = new DataInputStream(in);
        this.device = device;
        // encoder không cần cho scale (toạ độ đã theo pixel thiết bị).
    }

    @Override
    public void run() {
        try {
            input = InputManager.create();
        } catch (Throwable e) {
            System.err.println("[rc-server] không tạo được InputManager (control chỉ drain): " + e);
            input = null;
        }
        try {
            while (true) {
                int type = in.read();
                if (type < 0) break; // socket đóng
                dispatch(type);
            }
        } catch (IOException e) {
            System.err.println("[rc-server] control kết thúc: " + e);
        }
    }

    private void dispatch(int type) throws IOException {
        switch (type) {
            case Protocol.CTRL_MOUSE_MOTION: {
                int buttons = in.readInt();
                float x = in.readFloat();
                float y = in.readFloat();
                if ((buttons & Protocol.BUTTON_LEFT) != 0)
                    injectTouch(MotionEvent.ACTION_MOVE, x, y, 1f);
                break;
            }
            case Protocol.CTRL_MOUSE_BUTTON: {
                int action = in.readUnsignedByte();
                int button = in.readInt();
                int buttons = in.readInt();
                float x = in.readFloat();
                float y = in.readFloat();
                if (button == Protocol.BUTTON_LEFT) {
                    if (action == 1) // DOWN
                        injectTouch(MotionEvent.ACTION_DOWN, x, y, 1f);
                    else
                        injectTouch(MotionEvent.ACTION_UP, x, y, 0f);
                }
                break;
            }
            case Protocol.CTRL_SCROLL: {
                float x = in.readFloat();
                float y = in.readFloat();
                float h = in.readFloat();
                float v = in.readFloat();
                injectScroll(x, y, h, v);
                break;
            }
            case Protocol.CTRL_KEY: {
                int action = in.readUnsignedByte();
                int keycode = in.readInt();
                int metastate = in.readInt();
                int repeat = in.readInt();
                injectKey(action == 1 ? KeyEvent.ACTION_DOWN : KeyEvent.ACTION_UP, keycode,
                    metastate, repeat);
                break;
            }
            case Protocol.CTRL_TEXT: {
                int len = in.readInt();
                if (len < 0 || len > (1 << 20)) throw new IOException("TEXT len vô lý: " + len);
                byte[] buf = new byte[len];
                in.readFully(buf);
                injectText(new String(buf, "UTF-8"));
                break;
            }
            case Protocol.CTRL_DEVICE_ACTION: {
                int action = in.readUnsignedByte();
                device.perform(action);
                break;
            }
            default:
                throw new IOException("control type lạ: " + type);
        }
    }

    private void injectTouch(int action, float x, float y, float pressure) {
        if (input == null) return;
        long now = SystemClock.uptimeMillis();
        if (action == MotionEvent.ACTION_DOWN) lastDownTime = now;
        MotionEvent e = MotionEvent.obtain(lastDownTime, now, action, x, y, pressure, 1f, 0, 1f, 1f,
            0, 0);
        e.setSource(InputDevice.SOURCE_TOUCHSCREEN);
        input.inject(e);
        e.recycle();
    }

    private void injectScroll(float x, float y, float h, float v) {
        if (input == null) return;
        long now = SystemClock.uptimeMillis();
        MotionEvent.PointerProperties[] pp = {new MotionEvent.PointerProperties()};
        pp[0].id = 0;
        pp[0].toolType = MotionEvent.TOOL_TYPE_MOUSE;
        MotionEvent.PointerCoords[] pc = {new MotionEvent.PointerCoords()};
        pc[0].x = x;
        pc[0].y = y;
        pc[0].setAxisValue(MotionEvent.AXIS_HSCROLL, h);
        pc[0].setAxisValue(MotionEvent.AXIS_VSCROLL, v);
        MotionEvent e = MotionEvent.obtain(now, now, MotionEvent.ACTION_SCROLL, 1, pp, pc, 0, 0, 1f,
            1f, 0, 0, InputDevice.SOURCE_MOUSE, 0);
        input.inject(e);
        e.recycle();
    }

    private void injectKey(int action, int keycode, int metastate, int repeat) {
        if (input == null) return;
        long now = SystemClock.uptimeMillis();
        KeyEvent e = new KeyEvent(now, now, action, keycode, repeat, metastate,
            KeyCharacterMap.VIRTUAL_KEYBOARD, 0, 0, InputDevice.SOURCE_KEYBOARD);
        input.inject(e);
    }

    private void injectText(String text) {
        if (input == null || text.isEmpty()) return;
        KeyCharacterMap kcm = KeyCharacterMap.load(KeyCharacterMap.VIRTUAL_KEYBOARD);
        KeyEvent[] events = kcm.getEvents(text.toCharArray());
        if (events == null) return; // ký tự không map được sang key event
        for (KeyEvent e : events) input.inject(e);
    }
}
