package io.github.belmantegu.raymanrecomp;

import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.LightingColorFilter;
import android.graphics.Paint;
import android.graphics.RectF;
import android.util.SparseArray;
import android.view.MotionEvent;
import android.view.View;

/**
 * On-screen Xbox 360 controller for Rayman Origins, drawn over the game.
 *
 * Left side: a floating analog stick (it re-centers where the thumb lands).
 * Right side: jump (A, hold to glide), attack (X), back (B) and run (RT; by
 * default a tap starts running and the next tap stops, KEY_RUN_TOGGLE).
 * Top: select (Back), pause (Start) and the app settings.
 * Button art: res/drawable-nodpi (docs/TOUCH_BUTTONS.md, GamePad/).
 *
 * The state goes to an SDL3 virtual gamepad (android_touch.cpp), so the game
 * reads it like a physical controller. Visibility, opacity and size are stored
 * in SharedPreferences and changed from the settings dialog.
 */
public class TouchControls extends View {
    // SDL_GamepadButton indices.
    static final int BTN_A = 0, BTN_B = 1, BTN_X = 2, BTN_Y = 3, BTN_BACK = 4, BTN_START = 6;
    // Not a gamepad button: the right trigger, sent as an axis.
    static final int BTN_RT = 100;
    static final int BTN_SETTINGS = 101;

    static final String PREFS = "controls";
    static final String KEY_VISIBLE = "visible";
    static final String KEY_OPACITY = "opacity";   // 10..100
    static final String KEY_SCALE = "scale";       // 60..150
    static final String KEY_FILL = "fill_screen";
    // Run (RT) latches: one tap starts running, the next one stops.
    static final String KEY_RUN_TOGGLE = "run_toggle";

    static native void nativeSetEnabled(boolean enabled);
    static native void nativeSetState(int buttons, float leftX, float leftY, float rightTrigger);

    interface SettingsListener {
        void onOpenSettings();
    }

    private static final class Button {
        final int id;
        final int art;  // drawable resource
        Bitmap bitmap;
        float cx, cy, r;

        Button(int id, int art) {
            this.id = id;
            this.art = art;
        }

        boolean contains(float x, float y, float slop) {
            float dx = x - cx, dy = y - cy;
            return dx * dx + dy * dy <= (r + slop) * (r + slop);
        }
    }

    // Y is not used in the game, so it has no button.
    private final Button[] buttons = {
        new Button(BTN_A, R.drawable.btn_jump),
        new Button(BTN_B, R.drawable.btn_back),
        new Button(BTN_X, R.drawable.btn_attack),
        new Button(BTN_RT, R.drawable.btn_run),
        new Button(BTN_BACK, R.drawable.btn_select),
        new Button(BTN_START, R.drawable.btn_pause),
        new Button(BTN_SETTINGS, R.drawable.btn_settings),
    };
    private Bitmap stickBase, stickKnob;

    private final SharedPreferences prefs;
    private final Paint art = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG);
    // Pressed buttons are drawn brighter (the art has no pressed state yet).
    private final LightingColorFilter pressedFilter = new LightingColorFilter(0xFFFFFFFF, 0x00503A00);
    private final RectF rect = new RectF();
    private SettingsListener settingsListener;

    // Pointer id -> what it is holding (a Button, or STICK).
    private final SparseArray<Object> pointers = new SparseArray<>();
    private static final Object STICK = new Object();
    // A finger that lands away from every button (a resting thumb) stays inert
    // until it is lifted: sliding onto a button doesn't press it.
    private static final Object NOTHING = new Object();
    private float stickBaseX, stickBaseY, stickX, stickY, stickRadius;
    private boolean stickActive;
    private int stickPointer = -1;
    private int pressed;         // bitmask of SDL buttons
    private boolean rtPressed;
    private boolean runLatched;  // toggle mode: running until tapped again

    public TouchControls(Context context) {
        super(context);
        prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        for (Button b : buttons) b.bitmap = BitmapFactory.decodeResource(getResources(), b.art);
        stickBase = BitmapFactory.decodeResource(getResources(), R.drawable.stick_base);
        stickKnob = BitmapFactory.decodeResource(getResources(), R.drawable.stick_knob);
        setFocusable(false);
    }

    void setSettingsListener(SettingsListener listener) {
        settingsListener = listener;
    }

    /** Re-reads the preferences (after the settings dialog) and re-lays out. */
    void applyPreferences() {
        boolean visible = prefs.getBoolean(KEY_VISIBLE, true);
        nativeSetEnabled(visible);
        releaseAll();
        requestLayout();
        invalidate();
    }

    private float scale() {
        return prefs.getInt(KEY_SCALE, 100) / 100f;
    }

    private int alpha() {
        return Math.round(prefs.getInt(KEY_OPACITY, 80) * 2.55f);
    }

    @Override
    protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        layoutButtons(w, h);
    }

    @Override
    protected void onLayout(boolean changed, int l, int t, int r, int b) {
        layoutButtons(r - l, b - t);
    }

    private void layoutButtons(int w, int h) {
        float unit = Math.min(w, h) / 1080f * scale();  // 1 unit = 1px at 1080p height
        float face = 70 * unit;                           // face button radius
        float margin = 60 * unit;
        float diamondX = w - margin - face * 3.2f;
        float diamondY = h - margin - face * 3.2f;
        float spread = face * 2.1f;
        place(BTN_A, diamondX, diamondY + spread, face * 1.1f);
        place(BTN_B, diamondX + spread, diamondY, face * 0.85f);
        place(BTN_X, diamondX - spread, diamondY, face);
        place(BTN_RT, diamondX - spread * 2.2f, diamondY + spread, face * 0.9f);
        float small = 42 * unit;
        place(BTN_BACK, w / 2f - small * 2.5f, margin, small);
        place(BTN_START, w / 2f + small * 2.5f, margin, small);
        place(BTN_SETTINGS, w - margin, margin, small);
        stickRadius = 150 * unit;
        if (!stickActive) {
            stickBaseX = stickX = margin + stickRadius * 1.3f;
            stickBaseY = stickY = h - margin - stickRadius * 1.3f;
        }
    }

    private void place(int id, float x, float y, float r) {
        for (Button b : buttons) {
            if (b.id == id) {
                b.cx = x;
                b.cy = y;
                b.r = r;
            }
        }
    }

    @Override
    protected void onDraw(Canvas canvas) {
        boolean visible = prefs.getBoolean(KEY_VISIBLE, true);
        int a = alpha();
        // The settings button stays reachable (faintly) even when hidden.
        for (Button b : buttons) {
            if (!visible && b.id != BTN_SETTINGS) {
                continue;
            }
            boolean down = isDown(b.id);
            // The art's paint splashes reach past the touch circle: draw it a bit larger.
            float half = b.r * (down ? 1.32f : 1.25f);
            art.setAlpha(visible ? a : Math.min(a, 60));
            art.setColorFilter(down ? pressedFilter : null);
            rect.set(b.cx - half, b.cy - half, b.cx + half, b.cy + half);
            canvas.drawBitmap(b.bitmap, null, rect, art);
        }
        art.setColorFilter(null);
        if (!visible) {
            return;
        }
        // Stick: bubble base and stone knob.
        art.setAlpha(stickActive ? Math.min(255, a + 40) : a);
        float base = stickRadius * 1.15f;
        rect.set(stickBaseX - base, stickBaseY - base, stickBaseX + base, stickBaseY + base);
        canvas.drawBitmap(stickBase, null, rect, art);
        float knob = stickRadius * 0.55f;
        rect.set(stickX - knob, stickY - knob, stickX + knob, stickY + knob);
        canvas.drawBitmap(stickKnob, null, rect, art);
    }

    private boolean isDown(int id) {
        if (id == BTN_RT) {
            return rtPressed;
        }
        if (id == BTN_SETTINGS) {
            return false;
        }
        return (pressed & (1 << id)) != 0;
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        boolean visible = prefs.getBoolean(KEY_VISIBLE, true);
        int action = e.getActionMasked();
        int index = e.getActionIndex();

        if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) {
            // A new gesture: nothing from a previous one (a lost "up") survives.
            if (action == MotionEvent.ACTION_DOWN) {
                pointers.clear();
                stickPointer = -1;
            }
            pointers.remove(e.getPointerId(index));
            float x = e.getX(index), y = e.getY(index);
            Button hit = hitTest(x, y, null);
            if (hit != null && hit.id == BTN_SETTINGS) {
                // The dialog takes the focus and this view may never see the
                // matching "up" events: let go of everything first.
                releaseAll();
                if (settingsListener != null) {
                    settingsListener.onOpenSettings();
                }
                return true;
            }
            if (!visible) {
                return false;
            }
            if (hit == null && stickPointer < 0 && x < getWidth() * 0.45f) {
                // Floating stick: re-center under the thumb.
                stickPointer = e.getPointerId(index);
                stickBaseX = stickX = x;
                stickBaseY = stickY = y;
            } else if (hit == null) {
                pointers.put(e.getPointerId(index), NOTHING);
            }
        }
        if (!visible) {
            return false;
        }
        if (action == MotionEvent.ACTION_CANCEL) {
            releaseAll();
            return true;
        }

        // The state is rebuilt from the pointers still on the screen on every
        // event, so a lost "up" can never leave a button stuck. On UP and
        // POINTER_UP the lifted pointer is still reported, so it is skipped.
        int lifted = (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP)
                ? e.getPointerId(index) : -1;
        SparseArray<Object> next = new SparseArray<>();
        boolean stickAlive = false;
        for (int i = 0; i < e.getPointerCount(); i++) {
            int id = e.getPointerId(i);
            if (id == lifted) {
                continue;
            }
            float x = e.getX(i), y = e.getY(i);
            if (id == stickPointer) {
                stickAlive = true;
                float dx = x - stickBaseX, dy = y - stickBaseY;
                float len = (float) Math.hypot(dx, dy);
                if (len > stickRadius) {
                    dx *= stickRadius / len;
                    dy *= stickRadius / len;
                }
                stickX = stickBaseX + dx;
                stickY = stickBaseY + dy;
                next.put(id, STICK);
                continue;
            }
            Object current = pointers.get(id);
            if (current == NOTHING) {
                next.put(id, NOTHING);
                continue;
            }
            if (current == null) {
                // Only fingers that just landed get here without an entry.
                if (action != MotionEvent.ACTION_DOWN && action != MotionEvent.ACTION_POINTER_DOWN
                        || id != e.getPointerId(index)) {
                    next.put(id, NOTHING);
                    continue;
                }
            }
            Button b = hitTest(x, y, current instanceof Button ? (Button) current : null);
            // A finger may slide from one button straight onto a neighbour;
            // once it is off every button it goes inert until lifted.
            if (b != null && b.id != BTN_SETTINGS) {
                if (b.id == BTN_RT && current != b) {
                    runLatched = !runLatched;  // a finger reaching run: a tap
                }
                next.put(id, b);
            } else {
                next.put(id, NOTHING);
            }
        }
        if (!stickAlive) {
            stickPointer = -1;
        }
        pointers.clear();
        for (int i = 0; i < next.size(); i++) {
            pointers.put(next.keyAt(i), next.valueAt(i));
        }
        publish();
        invalidate();
        return true;
    }

    /**
     * The button under (x, y). A finger already holding `current` keeps it
     * while it stays within 1.6x its radius, so holding jump to glide survives
     * a drifting thumb; otherwise the closest button within 1.25x wins.
     */
    private Button hitTest(float x, float y, Button current) {
        if (current != null && Math.hypot(x - current.cx, y - current.cy) <= current.r * 1.6f) {
            return current;
        }
        Button best = null;
        float bestDist = Float.MAX_VALUE;
        for (Button b : buttons) {
            if (b.contains(x, y, b.r * 0.25f)) {
                float d = (float) Math.hypot(x - b.cx, y - b.cy);
                if (d < bestDist) {
                    bestDist = d;
                    best = b;
                }
            }
        }
        return best;
    }

    /** Lets go of everything: on cancel, focus loss, pause and settings. */
    void releaseAll() {
        pointers.clear();
        runLatched = false;
        stickPointer = -1;
        publish();
        invalidate();
    }

    @Override
    protected void onDetachedFromWindow() {
        releaseAll();
        super.onDetachedFromWindow();
    }

    private void publish() {
        int mask = 0;
        boolean rt = false;
        for (int i = 0; i < pointers.size(); i++) {
            Object held = pointers.valueAt(i);
            if (held instanceof Button) {
                Button b = (Button) held;
                if (b.id == BTN_RT) {
                    rt = true;
                } else if (b.id != BTN_SETTINGS) {
                    mask |= 1 << b.id;
                }
            }
        }
        if (prefs.getBoolean(KEY_RUN_TOGGLE, true)) {
            rt = runLatched;
        } else {
            runLatched = false;
        }
        pressed = mask;
        rtPressed = rt;
        stickActive = stickPointer >= 0;
        if (!stickActive) {
            layoutButtons(getWidth(), getHeight());  // knob back to its rest spot
        }
        float lx = 0, ly = 0;
        if (stickActive && stickRadius > 0) {
            lx = (stickX - stickBaseX) / stickRadius;
            ly = (stickY - stickBaseY) / stickRadius;
            // Radial dead zone: a resting thumb must not walk.
            float len = (float) Math.hypot(lx, ly);
            float dead = 0.18f;
            float k = len <= dead ? 0 : (len - dead) / (1 - dead) / len;
            lx *= k;
            ly *= k;
        }
        nativeSetState(mask, lx, ly, rt ? 1f : 0f);
    }
}
