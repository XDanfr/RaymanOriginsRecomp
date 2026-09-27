package io.github.belmantegu.raymanrecomp;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.res.AssetFileDescriptor;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.ColorFilter;
import android.graphics.LinearGradient;
import android.graphics.Matrix;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.PixelFormat;
import android.graphics.RadialGradient;
import android.graphics.Rect;
import android.graphics.Shader;
import android.graphics.SurfaceTexture;
import android.graphics.Typeface;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.media.MediaPlayer;
import android.net.Uri;
import android.os.Bundle;
import android.provider.DocumentsContract;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.TextureView;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.List;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * Home screen, the layout shared by all the ports (docs/PORT_HOME.md): the
 * game's logo, a menu (Play, Options, Game files, Quit), the game files'
 * status and the version, over a looping background video, styled after the
 * game's own options menu. Touch, controllers and keyboards drive it.
 *
 * Nothing from the game ships with the app. The player imports their own copy:
 * a .zip made by tools/make_game_pack.py (Game files), or a folder (Options).
 * A personal build can pack art from the player's copy (android/build_apk.sh,
 * private/launcher): assets/launcher/logo.png, bg.mp4, background.jpg.
 */
public class LauncherActivity extends Activity {
    private static final int PICK_FOLDER = 1, PICK_ZIP = 2, PICK_SAVES = 3, EXPORT_SAVES = 4;

    // Theme: the game's options menu.
    private static final int PLANK = 0xFF7D1710, PLANK_TOP = 0xFF8A1B12, PLANK_BOTTOM = 0xFF6C130D, PLANK_RIM = 0xFFA3301C;
    private static final int BUTTON_TOP = 0xFFEF7535, BUTTON = 0xFFE3642B, BUTTON_BOTTOM = 0xFFC14A1F;
    private static final int PICK_TOP = 0xFFFFD35A, PICK = 0xFFF6B02C, PICK_BOTTOM = 0xFFE28A1B;
    private static final int CREAM = 0xFFFDEBC8, CREAM_MUTED = 0xFFE6B98A, DARK_TEXT = 0xFF5B1A0B, ARROW = 0xFF8C1D0F;
    private static final int READY = 0xFFB9E36A, MISSING = 0xFFFFB46A;

    // Rough plank and button outlines (fractions of the bounds, clockwise).
    private static final float[] PLANK_SHAPE = {.012f, .06f, .18f, 0, .55f, .03f, .84f, 0, .99f, .05f, 1, .48f, .985f, .94f, .70f, 1, .34f, .97f, .03f, 1, 0, .60f};
    private static final float[] BUTTON_SHAPE = {0, .12f, .06f, 0, .5f, .04f, .94f, 0, 1, .14f, .99f, .88f, .93f, 1, .5f, .96f, .05f, 1, .01f, .86f};
    private static final float[] CHOOSER_SHAPE = {0, .10f, .04f, 0, .96f, .03f, 1, .14f, .99f, .90f, .95f, 1, .05f, .97f, .01f, .86f};

    private Typeface display, ui;
    private float u;  // layout unit: 1% of the screen width (bounded by the height)

    private final List<MenuButton> buttons = new ArrayList<>();
    private int selected;
    private LinearLayout options;
    private final List<OptionRow> rows = new ArrayList<>();
    private int optionSel;
    private TextView stateText, metaText, toast;
    private ProgressLine progressLine;
    private TextureView video;
    private MediaPlayer player;
    private volatile boolean busy;

    private File gameDir() { return new File(getExternalFilesDir(null), "game"); }
    private File spirvDir() { return new File(getExternalFilesDir(null), "spirv"); }
    // ReXGlue's user data root: $HOME/.local/share/rayman, HOME = internal files dir.
    private File saveDir() { return new File(getFilesDir(), ".local/share/rayman"); }

    // The Compact app keeps the whole game in one deduplicated pack, read by the
    // runtime as the original files (tools/make_compact_pack.py).
    private File packFile() { return new File(gameDir(), "game.rdpk"); }

    private boolean gameReady() {
        return packFile().isFile()
                || (new File(gameDir(), "default.xex").isFile() && new File(gameDir(), "bootsequence_X360.ipk").isFile());
    }

    /** Set when the home screen is opened from the in-game settings. */
    static final String EXTRA_HOME = "home";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        display = getResources().getFont(R.font.titan_one);
        ui = getResources().getFont(R.font.barlow_semi_condensed_semibold);
        android.util.DisplayMetrics m = getResources().getDisplayMetrics();
        float w = Math.max(m.widthPixels, m.heightPixels), h = Math.min(m.widthPixels, m.heightPixels);
        u = Math.min(w / 100f, h * 19.5f / 9f / 100f);

        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(0xFF120806);
        addBackground(root);

        TextView presents = text("BELMANTEGU PRESENTS", 1.25f, CREAM_MUTED, ui);
        presents.setLetterSpacing(0.12f);
        root.addView(presents, at(3.6f, 1.2f, -2, -2));

        Bitmap logo = loadAsset("launcher/logo.png");
        if (logo != null) {
            ImageView logoView = new ImageView(this);
            logoView.setImageBitmap(logo);
            logoView.setAdjustViewBounds(true);
            logoView.setElevation(u * .6f);
            root.addView(logoView, at(3f, 3.2f, 33f, -2));
        } else {
            TextView title = text("Rayman\nOrigins", 5.2f, PICK, display);
            title.setLineSpacing(0, .85f);
            title.setShadowLayer(u * .3f, 0, u * .25f, 0xFF3A1D08);
            root.addView(title, at(3.4f, 3.4f, -2, -2));
        }

        // Menu plank
        LinearLayout menu = new LinearLayout(this);
        menu.setOrientation(LinearLayout.VERTICAL);
        menu.setBackground(new ShapeDrawable(PLANK_SHAPE, PLANK_TOP, PLANK, PLANK_BOTTOM, PLANK_RIM, true));
        menu.setPadding(px(1.2f), px(1.2f), px(1.2f), px(1.5f));
        String[][] items = {{"Play", ""}, {"Options", ""}, {"Game files", BuildConfig.COMPACT ? ".rdpk" : ".zip"}, {"Quit", ""}};
        for (int i = 0; i < items.length; ++i) {
            MenuButton b = new MenuButton(items[i][0], items[i][1]);
            final int index = i;
            b.setOnClickListener(v -> { select(index); activate(); });
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, px(3.6f));
            if (i > 0) lp.topMargin = px(.65f);
            menu.addView(b, lp);
            buttons.add(b);
        }
        root.addView(menu, at(3.4f, 17.6f, 24f, -2));

        // Status plank
        LinearLayout status = new LinearLayout(this);
        status.setOrientation(LinearLayout.VERTICAL);
        status.setBackground(new ShapeDrawable(PLANK_SHAPE, PLANK_TOP, PLANK, PLANK_BOTTOM, PLANK_RIM, true));
        status.setPadding(px(1.4f), px(.9f), px(1.8f), px(1.1f));
        stateText = text("", 1.4f, READY, display);
        metaText = text("", 1.05f, CREAM_MUTED, ui);
        metaText.setLetterSpacing(0.04f);
        progressLine = new ProgressLine();
        status.addView(stateText);
        status.addView(metaText);
        status.addView(progressLine, new LinearLayout.LayoutParams(px(26), px(.9f)));
        FrameLayout.LayoutParams statusLp = new FrameLayout.LayoutParams(-2, -2, Gravity.BOTTOM | Gravity.START);
        statusLp.leftMargin = px(3.4f);
        statusLp.bottomMargin = px(2.2f);
        root.addView(status, statusLp);

        TextView version = text("Version " + versionName(), 1.2f, CREAM, ui);
        version.setLetterSpacing(0.05f);
        FrameLayout.LayoutParams versionLp = new FrameLayout.LayoutParams(-2, -2, Gravity.BOTTOM | Gravity.END);
        versionLp.rightMargin = px(2.6f);
        versionLp.bottomMargin = px(2.4f);
        root.addView(version, versionLp);

        LinearLayout pads = new LinearLayout(this);
        pads.addView(padHint("A", "Select", 0xFF5FAE2F));
        pads.addView(padHint("B", "Back", 0xFFC8321E));
        FrameLayout.LayoutParams padsLp = new FrameLayout.LayoutParams(-2, -2, Gravity.TOP | Gravity.END);
        padsLp.rightMargin = px(2.6f);
        padsLp.topMargin = px(2f);
        root.addView(pads, padsLp);

        buildOptions(root);

        toast = text("", 1.4f, CREAM, display);
        toast.setBackground(new ShapeDrawable(PLANK_SHAPE, PLANK_TOP, PLANK, PLANK_BOTTOM, PLANK_RIM, true));
        toast.setPadding(px(2.2f), px(.9f), px(2.2f), px(.9f));
        toast.setAlpha(0);
        FrameLayout.LayoutParams toastLp = new FrameLayout.LayoutParams(-2, -2, Gravity.BOTTOM | Gravity.CENTER_HORIZONTAL);
        toastLp.bottomMargin = px(6.5f);
        root.addView(toast, toastLp);

        setContentView(root);
        hideSystemBars(root);
        new Thread(this::extractShadersIfUpdated).start();
        select(0);
        refresh();
    }

    // ---- Background: the game's video (or picture), dark ground, light from above ----

    private void addBackground(FrameLayout root) {
        boolean hasVideo;
        try (AssetFileDescriptor ignored = getAssets().openFd("launcher/bg.mp4")) {
            hasVideo = true;
        } catch (IOException e) {
            hasVideo = false;
        }
        if (hasVideo) {
            video = new TextureView(this);
            video.setSurfaceTextureListener(new TextureView.SurfaceTextureListener() {
                @Override public void onSurfaceTextureAvailable(SurfaceTexture st, int w, int h) { startVideo(st); }
                @Override public void onSurfaceTextureSizeChanged(SurfaceTexture st, int w, int h) { fitVideo(); }
                @Override public boolean onSurfaceTextureDestroyed(SurfaceTexture st) { stopVideo(); return true; }
                @Override public void onSurfaceTextureUpdated(SurfaceTexture st) {}
            });
            root.addView(video, new FrameLayout.LayoutParams(-1, -1));
        } else {
            Bitmap picture = loadAsset("launcher/background.jpg");
            if (picture != null) {
                ImageView bg = new ImageView(this);
                bg.setImageBitmap(picture);
                bg.setScaleType(ImageView.ScaleType.CENTER_CROP);
                root.addView(bg, new FrameLayout.LayoutParams(-1, -1));
            } else {
                View bg = new View(this);
                bg.setBackground(new GradientDrawable(GradientDrawable.Orientation.TL_BR,
                        new int[] {0xFF0D2A1A, 0xFF1F4A2A, 0xFF0A1A10}));
                root.addView(bg, new FrameLayout.LayoutParams(-1, -1));
            }
        }
        View light = new View(this);
        light.setBackground(new LightDrawable());
        root.addView(light, new FrameLayout.LayoutParams(-1, -1));
    }

    private void startVideo(SurfaceTexture st) {
        try (AssetFileDescriptor fd = getAssets().openFd("launcher/bg.mp4")) {
            player = new MediaPlayer();
            player.setDataSource(fd.getFileDescriptor(), fd.getStartOffset(), fd.getLength());
            player.setSurface(new Surface(st));
            player.setLooping(true);
            player.setVolume(0, 0);
            player.setOnPreparedListener(mp -> { fitVideo(); mp.start(); });
            player.prepareAsync();
        } catch (IOException | RuntimeException e) {
            stopVideo();
        }
    }

    // Center-crop the video in its view.
    private void fitVideo() {
        if (player == null || video == null || video.getWidth() == 0) return;
        float vw = player.getVideoWidth(), vh = player.getVideoHeight();
        if (vw <= 0 || vh <= 0) return;
        float w = video.getWidth(), h = video.getHeight();
        float scale = Math.max(w / vw, h / vh);
        Matrix m = new Matrix();
        m.setScale(vw * scale / w, vh * scale / h, w / 2, h / 2);
        video.setTransform(m);
    }

    private void stopVideo() {
        if (player != null) {
            player.release();
            player = null;
        }
    }

    /** Dark edges and menu side, and a shaft of light from above. */
    private static class LightDrawable extends Drawable {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);

        @Override
        public void draw(Canvas c) {
            Rect b = getBounds();
            float w = b.width(), h = b.height();
            paint.setShader(new RadialGradient(w * .62f, -h * .12f, Math.max(w * .6f, h * .9f),
                    new int[] {0x6BFFF2C4, 0x29FFE4A0, 0x00FFE4A0}, new float[] {0, .38f, 1}, Shader.TileMode.CLAMP));
            c.drawRect(b, paint);
            paint.setShader(new RadialGradient(w * .6f, h * .5f, w * .75f,
                    new int[] {0x00080302, 0x00080302, 0xB8080302}, new float[] {0, .45f, 1}, Shader.TileMode.CLAMP));
            c.drawRect(b, paint);
            paint.setShader(new LinearGradient(0, 0, w, 0,
                    new int[] {0xE0280704, 0xA8280704, 0x2E280704, 0x00280704}, new float[] {0, .24f, .46f, .62f}, Shader.TileMode.CLAMP));
            c.drawRect(b, paint);
            paint.setShader(new LinearGradient(0, h, 0, h * .76f, 0xCC140503, 0x00140503, Shader.TileMode.CLAMP));
            c.drawRect(b, paint);
        }

        @Override public void setAlpha(int alpha) {}
        @Override public void setColorFilter(ColorFilter cf) {}
        @Override public int getOpacity() { return PixelFormat.TRANSLUCENT; }
    }

    // ---- Planks, buttons, choosers ----

    /** A rough polygon with a vertical gradient, a lighter rim and a drop shadow. */
    private class ShapeDrawable extends Drawable {
        private final float[] shape;
        private final int top, mid, bottom, rim;
        private final boolean shadow;
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Path path = new Path();

        ShapeDrawable(float[] shape, int top, int mid, int bottom, int rim, boolean shadow) {
            this.shape = shape;
            this.top = top;
            this.mid = mid;
            this.bottom = bottom;
            this.rim = rim;
            this.shadow = shadow;
        }

        private void outline(Rect b, float dx, float dy) {
            path.reset();
            for (int i = 0; i < shape.length; i += 2) {
                float x = b.left + shape[i] * b.width() + dx, y = b.top + shape[i + 1] * b.height() + dy;
                if (i == 0) path.moveTo(x, y);
                else path.lineTo(x, y);
            }
            path.close();
        }

        @Override
        public void draw(Canvas c) {
            Rect b = getBounds();
            paint.setStyle(Paint.Style.FILL);
            if (shadow) {
                outline(b, 0, u * .45f);
                paint.setShader(null);
                paint.setColor(0x88000000);
                c.drawPath(path, paint);
            }
            outline(b, 0, 0);
            paint.setShader(new LinearGradient(0, b.top, 0, b.bottom, new int[] {top, mid, bottom},
                    new float[] {0, .55f, 1}, Shader.TileMode.CLAMP));
            c.drawPath(path, paint);
            if (rim != 0) {
                paint.setShader(null);
                paint.setStyle(Paint.Style.STROKE);
                paint.setStrokeWidth(u * .28f);
                paint.setColor(rim);
                c.save();
                c.clipPath(path);
                c.drawPath(path, paint);
                c.restore();
            }
        }

        @Override public void setAlpha(int alpha) {}
        @Override public void setColorFilter(ColorFilter cf) {}
        @Override public int getOpacity() { return PixelFormat.TRANSLUCENT; }
    }

    /** A menu button: orange, or yellow-orange with ◀ ▶ when selected. */
    private class MenuButton extends View {
        private final String label, hint;
        private boolean on;
        private final ShapeDrawable normal = new ShapeDrawable(BUTTON_SHAPE, BUTTON_TOP, BUTTON, BUTTON_BOTTOM, 0, false);
        private final ShapeDrawable picked = new ShapeDrawable(BUTTON_SHAPE, PICK_TOP, PICK, PICK_BOTTOM, 0, false);
        private final Paint text = new Paint(Paint.ANTI_ALIAS_FLAG), arrow = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Path tri = new Path();

        MenuButton(String label, String hint) {
            super(LauncherActivity.this);
            this.label = label;
            this.hint = hint;
            // Not focusable: keys and pads go to the activity, which moves the
            // highlight; a focused button would take Enter / A as its own click.
            setClickable(true);
            setFocusable(false);
            text.setTextAlign(Paint.Align.CENTER);
            arrow.setColor(ARROW);
        }

        void setOn(boolean on) {
            this.on = on;
            setScaleX(on ? 1.04f : 1f);
            setScaleY(on ? 1.04f : 1f);
            invalidate();
        }

        @Override
        protected void onDraw(Canvas c) {
            ShapeDrawable bg = on ? picked : normal;
            bg.setBounds(0, 0, getWidth(), getHeight());
            bg.draw(c);
            float cx = getWidth() / 2f, cy = getHeight() / 2f;
            text.setTypeface(display);
            text.setTextSize(u * 1.9f);
            text.setColor(on ? DARK_TEXT : CREAM);
            text.setShadowLayer(0.01f, 0, u * .15f, on ? 0x99FFECAA : 0x8C5A1408);
            float hintWidth = 0;
            if (!hint.isEmpty()) {
                Paint small = new Paint(text);
                small.setTypeface(ui);
                small.setTextSize(u * .95f);
                hintWidth = small.measureText(hint) + u * .7f;
                float labelWidth = text.measureText(label);
                small.setTextAlign(Paint.Align.LEFT);
                c.drawText(hint, cx + (labelWidth - hintWidth) / 2f + u * .7f, cy + u * .65f, small);
            }
            c.drawText(label, cx - hintWidth / 2f, cy - (text.descent() + text.ascent()) / 2f, text);
            if (on) {
                float a = u * .75f, l = u * 1.1f, m = u * 1.1f;
                tri.reset();
                tri.moveTo(m, cy);
                tri.lineTo(m + l, cy - a);
                tri.lineTo(m + l, cy + a);
                tri.close();
                tri.moveTo(getWidth() - m, cy);
                tri.lineTo(getWidth() - m - l, cy - a);
                tri.lineTo(getWidth() - m - l, cy + a);
                tri.close();
                c.drawPath(tri, arrow);
            }
        }
    }

    /** A thin progress bar for imports, drawn on the status plank. */
    private class ProgressLine extends View {
        float value = -1;  // 0..1, or < 0 hidden
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);

        ProgressLine() { super(LauncherActivity.this); }

        void set(float v) {
            value = v;
            setVisibility(v < 0 ? GONE : VISIBLE);
            invalidate();
        }

        @Override
        protected void onDraw(Canvas c) {
            float h = getHeight(), r = h / 2f;
            paint.setColor(0x66000000);
            c.drawRoundRect(0, 0, getWidth(), h, r, r, paint);
            paint.setShader(new LinearGradient(0, 0, 0, h, PICK_TOP, PICK_BOTTOM, Shader.TileMode.CLAMP));
            c.drawRoundRect(0, 0, Math.max(h, getWidth() * Math.min(1, value)), h, r, r, paint);
            paint.setShader(null);
        }
    }

    // ---- Options: a plank like the game's "Display Options" ----

    private class OptionRow extends LinearLayout {
        final String key;
        final TextView value;

        OptionRow(String key, String label, String sub) {
            super(LauncherActivity.this);
            this.key = key;
            setOrientation(HORIZONTAL);
            setGravity(Gravity.CENTER_VERTICAL);
            setPadding(px(.6f), px(.3f), px(.6f), px(.3f));
            LinearLayout texts = new LinearLayout(LauncherActivity.this);
            texts.setOrientation(VERTICAL);
            texts.addView(text(label, 1.5f, PICK, display));
            if (sub != null) texts.addView(text(sub, .95f, CREAM_MUTED, ui));
            addView(texts, new LayoutParams(0, -2, 1));
            value = new TextView(LauncherActivity.this) {
                private final Paint arrow = new Paint(Paint.ANTI_ALIAS_FLAG);
                private final Path tri = new Path();

                @Override
                protected void onDraw(Canvas c) {
                    super.onDraw(c);
                    // ◀ ▶ at the ends, like the game's choosers.
                    float cy = getHeight() / 2f, a = u * .6f, l = u * .85f, m = u * .55f, w = getWidth();
                    tri.reset();
                    tri.moveTo(m, cy);
                    tri.lineTo(m + l, cy - a);
                    tri.lineTo(m + l, cy + a);
                    tri.close();
                    tri.moveTo(w - m, cy);
                    tri.lineTo(w - m - l, cy - a);
                    tri.lineTo(w - m - l, cy + a);
                    tri.close();
                    arrow.setColor(ARROW);
                    c.drawPath(tri, arrow);
                }
            };
            // Dark brown on yellow-orange, as in the game; no text shadow, which blurred it.
            value.setTextSize(TypedValue.COMPLEX_UNIT_PX, 1.5f * u);
            value.setTextColor(DARK_TEXT);
            value.setTypeface(display);
            value.setIncludeFontPadding(false);
            value.setGravity(Gravity.CENTER);
            value.setBackground(new ShapeDrawable(CHOOSER_SHAPE, PICK_TOP, PICK, PICK_BOTTOM, 0, false));
            value.setPadding(px(1.6f), px(.35f), px(1.6f), px(.35f));
            value.setMinWidth(px(12));
            addView(value);
            setOnClickListener(v -> { optionSel = rows.indexOf(this); step(1); });
            setFocusable(false);  // keys go to the activity (see MenuButton)
        }

        void setOn(boolean on) {
            GradientDrawable g = new GradientDrawable();
            g.setCornerRadius(u * .3f);
            g.setColor(on ? 0x24FFD278 : 0);
            if (on) g.setStroke(Math.max(1, px(.15f)), 0x73FFD278);
            setBackground(g);
        }
    }

    private void buildOptions(FrameLayout root) {
        options = new LinearLayout(this);
        options.setOrientation(LinearLayout.VERTICAL);
        options.setBackground(new ShapeDrawable(PLANK_SHAPE, PLANK_TOP, PLANK, PLANK_BOTTOM, PLANK_RIM, true));
        options.setPadding(px(2.4f), px(2f), px(2.4f), px(2.6f));
        TextView title = text("Options", 2.1f, CREAM, display);
        title.setGravity(Gravity.CENTER);
        options.addView(title, new LinearLayout.LayoutParams(-1, -2));
        String[][] spec = {
            {"res", "Resolution", "Lower is faster on weak phones"},
            {"gfx", "Graphics", "Native renderer or GPU emulation"},
            {"touch", "Touch controls", null},
            {"opacity", "Controls opacity", null},
            {"folder", "Game folder", "Import from a folder instead"},
            {"backup", "Back up saves", "Your progress as a .zip, anywhere"},
            {"restore", "Restore saves", "From a backup .zip (also from the other app)"},
        };
        for (String[] s : spec) {
            OptionRow r = new OptionRow(s[0], s[1], s[2]);
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, -2);
            lp.topMargin = px(.8f);
            options.addView(r, lp);
            rows.add(r);
        }
        options.setVisibility(View.GONE);
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(px(36), -2, Gravity.END | Gravity.CENTER_VERTICAL);
        lp.rightMargin = px(4);
        root.addView(options, lp);
    }

    private static final int[] SCALES = {50, 67, 75, 100}, OPACITIES = {35, 55, 75, 100};

    private SharedPreferences prefs() { return getSharedPreferences(TouchControls.PREFS, MODE_PRIVATE); }

    private static int nearest(int[] values, int v) {
        int best = 0;
        for (int i = 1; i < values.length; ++i) if (Math.abs(values[i] - v) < Math.abs(values[best] - v)) best = i;
        return best;
    }

    private void paintOptions() {
        SharedPreferences p = prefs();
        for (int i = 0; i < rows.size(); ++i) {
            OptionRow r = rows.get(i);
            r.setOn(i == optionSel);
            switch (r.key) {
                case "res": r.value.setText(SCALES[nearest(SCALES, p.getInt(RaymanActivity.KEY_RENDER_SCALE, 100))] + "%"); break;
                case "gfx": r.value.setText(p.getBoolean(RaymanActivity.KEY_NATIVE, true) ? "Vulkan" : "Emulated"); break;
                case "touch": r.value.setText(p.getBoolean(TouchControls.KEY_VISIBLE, true) ? "On" : "Off"); break;
                case "opacity": r.value.setText(OPACITIES[nearest(OPACITIES, p.getInt(TouchControls.KEY_OPACITY, 55))] + "%"); break;
                default: r.value.setText("Choose"); break;
            }
        }
    }

    private void step(int d) {
        SharedPreferences p = prefs();
        SharedPreferences.Editor e = p.edit();
        switch (rows.get(optionSel).key) {
            case "res": {
                int i = nearest(SCALES, p.getInt(RaymanActivity.KEY_RENDER_SCALE, 100));
                e.putInt(RaymanActivity.KEY_RENDER_SCALE, SCALES[(i + d + SCALES.length) % SCALES.length]);
                break;
            }
            case "gfx": e.putBoolean(RaymanActivity.KEY_NATIVE, !p.getBoolean(RaymanActivity.KEY_NATIVE, true)); break;
            case "touch": e.putBoolean(TouchControls.KEY_VISIBLE, !p.getBoolean(TouchControls.KEY_VISIBLE, true)); break;
            case "opacity": {
                int i = nearest(OPACITIES, p.getInt(TouchControls.KEY_OPACITY, 55));
                e.putInt(TouchControls.KEY_OPACITY, OPACITIES[(i + d + OPACITIES.length) % OPACITIES.length]);
                break;
            }
            case "folder": pick(PICK_FOLDER); return;
            case "backup": backUpSaves(); return;
            case "restore": pick(PICK_SAVES); return;
        }
        e.apply();
        paintOptions();
    }

    private boolean optionsOpen() { return options.getVisibility() == View.VISIBLE; }

    private void showOptions(boolean show) {
        options.setVisibility(show ? View.VISIBLE : View.GONE);
        optionSel = 0;
        if (show) paintOptions();
    }

    // ---- Menu ----

    private void select(int i) {
        selected = (i + buttons.size()) % buttons.size();
        for (int j = 0; j < buttons.size(); ++j) buttons.get(j).setOn(j == selected);
    }

    private void activate() {
        switch (selected) {
            case 0:
                if (gameReady()) startGame();
                else say("Import your game files first");
                break;
            case 1: showOptions(!optionsOpen()); break;
            case 2: pick(PICK_ZIP); break;
            case 3: finishAffinity(); break;
        }
    }

    private void say(String message) {
        toast.setText(message);
        toast.animate().cancel();
        toast.setAlpha(1);
        toast.animate().alpha(0).setStartDelay(1600).setDuration(300).start();
    }

    // Controllers and keyboards: D-pad / stick / arrows move, A / Enter pick, B / Esc back.
    private int lastStick;

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        switch (keyCode) {
            case KeyEvent.KEYCODE_DPAD_UP: move(-1); return true;
            case KeyEvent.KEYCODE_DPAD_DOWN: move(1); return true;
            case KeyEvent.KEYCODE_DPAD_LEFT: if (optionsOpen()) step(-1); return true;
            case KeyEvent.KEYCODE_DPAD_RIGHT: if (optionsOpen()) step(1); return true;
            case KeyEvent.KEYCODE_BUTTON_A:
            case KeyEvent.KEYCODE_ENTER:
            case KeyEvent.KEYCODE_DPAD_CENTER:
            case KeyEvent.KEYCODE_SPACE:
                if (optionsOpen()) step(1);
                else activate();
                return true;
            case KeyEvent.KEYCODE_BUTTON_B:
            case KeyEvent.KEYCODE_ESCAPE:
            case KeyEvent.KEYCODE_BACK:
                if (optionsOpen()) {
                    showOptions(false);
                    return true;
                }
                return super.onKeyDown(keyCode, event);
        }
        return super.onKeyDown(keyCode, event);
    }

    @Override
    public boolean onGenericMotionEvent(MotionEvent e) {
        if ((e.getSource() & InputDevice.SOURCE_JOYSTICK) != 0 && e.getAction() == MotionEvent.ACTION_MOVE) {
            float y = e.getAxisValue(MotionEvent.AXIS_Y), hat = e.getAxisValue(MotionEvent.AXIS_HAT_Y);
            float x = e.getAxisValue(MotionEvent.AXIS_X), hatX = e.getAxisValue(MotionEvent.AXIS_HAT_X);
            int dir = (y < -.6f || hat < -.5f) ? -1 : (y > .6f || hat > .5f) ? 1 : 0;
            int side = (x < -.6f || hatX < -.5f) ? -1 : (x > .6f || hatX > .5f) ? 1 : 0;
            int state = dir != 0 ? dir * 10 : side;
            if (state != lastStick) {
                if (dir != 0) move(dir);
                else if (side != 0 && optionsOpen()) step(side);
            }
            lastStick = state;
            return true;
        }
        return super.onGenericMotionEvent(e);
    }

    private void move(int d) {
        if (optionsOpen()) {
            optionSel = (optionSel + d + rows.size()) % rows.size();
            paintOptions();
        } else {
            select(selected + d);
        }
    }

    private void hideSystemBars(View root) {
        if (android.os.Build.VERSION.SDK_INT >= 30) {
            WindowInsetsController c = root.getWindowInsetsController();
            if (c != null) {
                c.hide(WindowInsets.Type.systemBars());
                c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (player != null) player.start();
        refresh();
    }

    @Override
    protected void onPause() {
        if (player != null && player.isPlaying()) player.pause();
        super.onPause();
    }

    @Override
    protected void onDestroy() {
        stopVideo();
        super.onDestroy();
    }

    // Game files status: ready (platform, files, size) or missing.
    private void refresh() {
        if (busy) return;
        boolean ready = gameReady();
        stateText.setText(ready ? "✓  Game ready" : "Game files not found");
        stateText.setTextColor(ready ? READY : MISSING);
        metaText.setText(ready ? "Xbox 360 • Retail" : BuildConfig.COMPACT
                ? "Game files → import your compact pack (.rdpk)" : "Game files → import your game pack (.zip)");
        progressLine.set(-1);
        buttons.get(0).setAlpha(ready ? 1f : .55f);
        if (ready && packFile().isFile()) {
            metaText.setText(String.format(java.util.Locale.US, "Xbox 360 • Retail • compact pack • %.2f GB",
                    packFile().length() / 1073741824.0));
        } else if (ready) {
            new Thread(() -> {
                long[] totals = new long[2];
                tally(gameDir(), totals);
                final String meta = String.format(java.util.Locale.US, "Xbox 360 • Retail • %d files • %.1f GB", totals[0], totals[1] / 1073741824.0);
                runOnUiThread(() -> { if (!busy) metaText.setText(meta); });
            }).start();
        }
    }

    private static void tally(File dir, long[] totals) {
        File[] files = dir.listFiles();
        if (files == null) return;
        for (File f : files) {
            if (f.isDirectory()) tally(f, totals);
            else { totals[0]++; totals[1] += f.length(); }
        }
    }

    private void startGame() {
        if (!gameReady() || busy) return;
        startActivity(new Intent(this, RaymanActivity.class));
        finish();  // the app icon then goes back to the game, not to this screen
    }

    private String versionName() {
        try {
            return getPackageManager().getPackageInfo(getPackageName(), 0).versionName;
        } catch (android.content.pm.PackageManager.NameNotFoundException e) {
            return "";
        }
    }

    // ---- Importing the player's files ----

    private void pick(int request) {
        if (busy) return;
        Intent intent;
        if (request == PICK_ZIP) {
            intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            intent.addCategory(Intent.CATEGORY_OPENABLE);
            // A .rdpk has no MIME type of its own: let the player pick any file.
            intent.setType(BuildConfig.COMPACT ? "*/*" : "application/zip");
        } else if (request == PICK_SAVES) {
            intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            intent.addCategory(Intent.CATEGORY_OPENABLE);
            intent.setType("application/zip");
        } else {
            intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        }
        startActivityForResult(intent, request);
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (result != RESULT_OK || data == null || data.getData() == null) return;
        Uri uri = data.getData();
        if (request == EXPORT_SAVES) {
            runSaves("Backing up saves…", () -> zipSaves(uri), "Saves backed up");
            return;
        }
        if (request == PICK_SAVES) {
            new AlertDialog.Builder(this)
                    .setMessage("Replace the saves in this app with the ones in the backup? The current progress will be lost.")
                    .setPositiveButton("Restore", (d, w) -> runSaves("Restoring saves…", () -> restoreSaves(uri),
                            "Saves restored: close and reopen the game"))
                    .setNegativeButton(android.R.string.cancel, null)
                    .show();
            return;
        }
        busy = true;
        showOptions(false);
        stateText.setTextColor(CREAM);
        stateText.setText("Copying game files…");
        metaText.setText("");
        progressLine.set(0);
        new Thread(() -> {
            String error = null;
            try {
                if (request == PICK_ZIP && isPack(uri)) copyPack(uri);
                else if (request == PICK_ZIP) unzip(uri, gameDir());
                else copyTree(uri, gameDir());
            } catch (Exception e) {
                error = e.getMessage();
            }
            final String message = error;
            runOnUiThread(() -> {
                busy = false;
                refresh();
                if (message != null) {
                    new AlertDialog.Builder(this).setMessage("Import failed: " + message)
                            .setPositiveButton(android.R.string.ok, null).show();
                } else if (!gameReady()) {
                    new AlertDialog.Builder(this)
                            .setMessage("default.xex or bootsequence_X360.ipk not found. Pick the game pack (.zip) or the folder that contains the game's files.")
                            .setPositiveButton(android.R.string.ok, null).show();
                } else {
                    startGame();  // game files in place: straight into the game
                }
            });
        }).start();
    }

    private void setStatus(String s) {
        runOnUiThread(() -> metaText.setText(s));
    }

    // ---- Saves: back up to a .zip, restore from one ----

    private interface SaveTask {
        void run() throws IOException;
    }

    private void backUpSaves() {
        File[] saves = saveDir().listFiles();
        if (saves == null || saves.length == 0) {
            say("No saves yet: play first");
            return;
        }
        Intent intent = new Intent(Intent.ACTION_CREATE_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("application/zip");
        String date = new java.text.SimpleDateFormat("yyyy-MM-dd", java.util.Locale.US).format(new java.util.Date());
        intent.putExtra(Intent.EXTRA_TITLE, "RaymanOrigins-saves-" + date + ".zip");
        startActivityForResult(intent, EXPORT_SAVES);
    }

    private void runSaves(String working, SaveTask task, String done) {
        busy = true;
        showOptions(false);
        stateText.setTextColor(CREAM);
        stateText.setText(working);
        metaText.setText("");
        progressLine.set(-1);
        new Thread(() -> {
            String error = null;
            try {
                task.run();
            } catch (Exception e) {
                error = e.getMessage();
            }
            final String message = error;
            runOnUiThread(() -> {
                busy = false;
                refresh();
                if (message != null) {
                    new AlertDialog.Builder(this).setMessage("Saves: " + message)
                            .setPositiveButton(android.R.string.ok, null).show();
                } else {
                    say(done);
                }
            });
        }).start();
    }

    private void zipSaves(Uri uri) throws IOException {
        OutputStream raw = getContentResolver().openOutputStream(uri);
        if (raw == null) throw new IOException("cannot write the backup");
        try (java.util.zip.ZipOutputStream zip = new java.util.zip.ZipOutputStream(raw)) {
            zipDir(saveDir(), "", zip);
        }
    }

    private static void zipDir(File dir, String prefix, java.util.zip.ZipOutputStream zip) throws IOException {
        File[] files = dir.listFiles();
        if (files == null) return;
        for (File f : files) {
            String name = prefix + f.getName();
            if (f.isDirectory()) {
                zip.putNextEntry(new ZipEntry(name + "/"));
                zip.closeEntry();
                zipDir(f, name + "/", zip);
            } else {
                zip.putNextEntry(new ZipEntry(name));
                try (InputStream in = new java.io.FileInputStream(f)) {
                    copy(in, zip);
                }
                zip.closeEntry();
            }
        }
    }

    // Extracts the backup next to the saves, then swaps it in: a failure midway
    // leaves the current saves untouched.
    private void restoreSaves(Uri uri) throws IOException {
        File saves = saveDir(), parent = saves.getParentFile();
        File incoming = new File(parent, "rayman.restore"), old = new File(parent, "rayman.old");
        deleteTree(incoming);
        deleteTree(old);
        int files = 0;
        try (ZipInputStream in = new ZipInputStream(getContentResolver().openInputStream(uri))) {
            ZipEntry e;
            while ((e = in.getNextEntry()) != null) {
                String name = e.getName();
                if (name.contains("..") || name.startsWith("/")) continue;
                File out = new File(incoming, name);
                if (e.isDirectory()) { out.mkdirs(); continue; }
                out.getParentFile().mkdirs();
                try (OutputStream o = new FileOutputStream(out)) {
                    copy(in, o);
                }
                ++files;
            }
        } catch (IOException e) {
            deleteTree(incoming);
            throw e;
        }
        if (files == 0) {
            deleteTree(incoming);
            throw new IOException("the backup has no saves in it");
        }
        parent.mkdirs();
        if (saves.exists() && !saves.renameTo(old)) {
            deleteTree(incoming);
            throw new IOException("cannot move the current saves aside");
        }
        if (!incoming.renameTo(saves)) {
            old.renameTo(saves);  // put the current saves back
            throw new IOException("cannot put the backup in place");
        }
        deleteTree(old);
    }

    private static void deleteTree(File f) {
        File[] children = f.listFiles();
        if (children != null) for (File c : children) deleteTree(c);
        f.delete();
    }

    // Copies a document tree (the folder the player picked) into `dest`.
    private void copyTree(Uri tree, File dest) throws IOException {
        String rootId = DocumentsContract.getTreeDocumentId(tree);
        copyChildren(tree, rootId, dest, new long[] {0});
    }

    private void copyChildren(Uri tree, String parentId, File dest, long[] copied) throws IOException {
        if (!dest.isDirectory() && !dest.mkdirs()) throw new IOException("cannot create " + dest);
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, parentId);
        String[] cols = {DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                         DocumentsContract.Document.COLUMN_MIME_TYPE};
        try (android.database.Cursor c = getContentResolver().query(children, cols, null, null, null)) {
            while (c != null && c.moveToNext()) {
                String id = c.getString(0), name = c.getString(1), mime = c.getString(2);
                if (DocumentsContract.Document.MIME_TYPE_DIR.equals(mime)) {
                    copyChildren(tree, id, new File(dest, name), copied);
                } else {
                    Uri doc = DocumentsContract.buildDocumentUriUsingTree(tree, id);
                    try (InputStream in = getContentResolver().openInputStream(doc);
                         OutputStream out = new FileOutputStream(new File(dest, name))) {
                        copied[0] += copy(in, out);
                    }
                    setStatus("Copying " + name + "  (" + (copied[0] >> 20) + " MB)");
                }
            }
        }
    }

    // A compact game pack starts with "RDPK".
    private boolean isPack(Uri uri) throws IOException {
        try (InputStream in = getContentResolver().openInputStream(uri)) {
            byte[] magic = new byte[4];
            return in != null && in.read(magic) == 4 && new String(magic, "US-ASCII").equals("RDPK");
        }
    }

    // Copies the compact pack to game/game.rdpk (under a temporary name first).
    private void copyPack(Uri uri) throws IOException {
        long size = 0;
        try (android.database.Cursor c = getContentResolver().query(
                uri, new String[] {android.provider.OpenableColumns.SIZE}, null, null, null)) {
            if (c != null && c.moveToFirst() && !c.isNull(0)) size = c.getLong(0);
        }
        File dir = gameDir();
        dir.mkdirs();
        if (size > dir.getUsableSpace()) {
            throw new IOException("not enough free space: " + (size >> 20) + " MB needed, "
                    + (dir.getUsableSpace() >> 20) + " MB free");
        }
        File tmp = new File(dir, "game.rdpk.part");
        final long total = size;
        try (InputStream in = getContentResolver().openInputStream(uri); OutputStream out = new FileOutputStream(tmp)) {
            byte[] buf = new byte[1 << 20];
            long done = 0;
            int n, step = 0;
            while ((n = in.read(buf)) > 0) {
                out.write(buf, 0, n);
                done += n;
                if (++step % 16 == 0 || done == total) {
                    final float fraction = total > 0 ? Math.min(1f, (float) done / total) : 0;
                    final String line = "Copying the pack…  " + (done >> 20) + " / " + (total >> 20) + " MB  ("
                            + Math.round(fraction * 100) + "%)";
                    runOnUiThread(() -> {
                        metaText.setText(line);
                        progressLine.set(fraction);
                    });
                }
            }
        }
        if (!tmp.renameTo(packFile())) throw new IOException("cannot write " + packFile());
    }

    private void unzip(Uri zip, File dest) throws IOException {
        long copied = 0;
        // The pack's size drives the progress bar (stored entries: about the
        // same as what gets extracted).
        long total = 0;
        try (android.database.Cursor c = getContentResolver().query(
                zip, new String[] {android.provider.OpenableColumns.SIZE}, null, null, null)) {
            if (c != null && c.moveToFirst() && !c.isNull(0)) total = c.getLong(0);
        }
        final long size = total;
        if (size > 0 && size > dest.getParentFile().getUsableSpace()) {
            throw new IOException("not enough free space: " + (size >> 20) + " MB needed, "
                    + (dest.getParentFile().getUsableSpace() >> 20) + " MB free");
        }
        try (ZipInputStream in = new ZipInputStream(getContentResolver().openInputStream(zip))) {
            ZipEntry e;
            while ((e = in.getNextEntry()) != null) {
                String name = e.getName();
                if (name.contains("..")) continue;
                File out = new File(dest, name);
                if (e.isDirectory()) { out.mkdirs(); continue; }
                out.getParentFile().mkdirs();
                try (OutputStream o = new FileOutputStream(out)) {
                    copied += copy(in, o);
                }
                final float fraction = size > 0 ? Math.min(1f, (float) copied / size) : 0;
                final String line = size > 0
                        ? "Extracting…  " + (copied >> 20) + " / " + (size >> 20) + " MB  (" + Math.round(fraction * 100) + "%)"
                        : "Extracting " + out.getName() + "  (" + (copied >> 20) + " MB)";
                runOnUiThread(() -> {
                    metaText.setText(line);
                    progressLine.set(fraction);
                });
            }
        }
        // Flatten a single top-level folder: game files may sit inside one.
        File[] top = dest.listFiles();
        if (top != null && top.length == 1 && top[0].isDirectory() && new File(top[0], "default.xex").isFile()) {
            File[] inner = top[0].listFiles();
            if (inner != null) for (File f : inner) f.renameTo(new File(dest, f.getName()));
            top[0].delete();
        }
    }

    private static long copy(InputStream in, OutputStream out) throws IOException {
        byte[] buf = new byte[1 << 20];
        long total = 0;
        int n;
        while ((n = in.read(buf)) > 0) {
            out.write(buf, 0, n);
            total += n;
        }
        return total;
    }

    // SPIR-V shaders packed in a personal build (assets/spirv) go where the
    // native renderer reads them: once per installed version of the APK.
    private synchronized void extractShadersIfUpdated() {
        long installed;
        try {
            installed = getPackageManager().getPackageInfo(getPackageName(), 0).lastUpdateTime;
        } catch (android.content.pm.PackageManager.NameNotFoundException e) {
            installed = -1;
        }
        SharedPreferences prefs = getSharedPreferences("launcher", MODE_PRIVATE);
        if (installed != -1 && prefs.getLong("shaders_from", 0) == installed) return;
        extractShaders();
        prefs.edit().putLong("shaders_from", installed).commit();
    }

    private void extractShaders() {
        try {
            String[] names = getAssets().list("spirv");
            if (names == null || names.length == 0) return;
            File dir = spirvDir();
            dir.mkdirs();
            for (String name : names) {
                File out = new File(dir, name);
                try (InputStream in = getAssets().open("spirv/" + name);
                     OutputStream o = new FileOutputStream(out)) {
                    copy(in, o);
                }
            }
        } catch (IOException ignored) {
        }
    }

    // ---- Small helpers ----

    private int px(float units) { return Math.round(units * u); }

    // Top-left placement in layout units; w/h in units, or -1/-2 (match/wrap).
    private FrameLayout.LayoutParams at(float x, float y, float w, float h) {
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(w < 0 ? (int) w : px(w), h < 0 ? (int) h : px(h),
                Gravity.TOP | Gravity.START);
        lp.leftMargin = px(x);
        lp.topMargin = px(y);
        return lp;
    }

    private TextView text(String s, float size, int color, Typeface face) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(TypedValue.COMPLEX_UNIT_PX, size * u);
        t.setTextColor(color);
        t.setTypeface(face);
        t.setIncludeFontPadding(false);
        t.setShadowLayer(u * .3f, 0, u * .1f, 0xAA000000);
        return t;
    }

    private View padHint(String letter, String label, int color) {
        LinearLayout row = new LinearLayout(this);
        row.setGravity(Gravity.CENTER_VERTICAL);
        TextView badge = text(letter, .95f, 0xFFFFFFFF, display);
        badge.setGravity(Gravity.CENTER);
        badge.setShadowLayer(0, 0, 0, 0);
        GradientDrawable circle = new GradientDrawable();
        circle.setShape(GradientDrawable.OVAL);
        circle.setColor(color);
        badge.setBackground(circle);
        row.addView(badge, new LinearLayout.LayoutParams(px(1.7f), px(1.7f)));
        TextView t = text(label, 1.05f, CREAM, ui);
        t.setPadding(px(.4f), 0, px(1.2f), 0);
        row.addView(t);
        return row;
    }

    private Bitmap loadAsset(String path) {
        try (InputStream in = getAssets().open(path)) {
            return BitmapFactory.decodeStream(in);
        } catch (IOException e) {
            return null;
        }
    }
}
