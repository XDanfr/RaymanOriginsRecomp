package io.github.belmantegu.raymanrecomp;

import android.app.AlertDialog;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.ActivityInfo;
import android.os.Bundle;
import android.system.ErrnoException;
import android.system.Os;
import android.util.DisplayMetrics;
import android.util.Log;
import android.view.ViewGroup;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.SeekBar;
import android.widget.TextView;

import org.libsdl.app.SDLActivity;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;

/**
 * Starts the recompiled game: SDLActivity loads the native libraries below and
 * calls SDL_main() in librayman.so.
 *
 * The game data is not part of the APK. It is read from
 * /sdcard/Android/data/io.github.belmantegu.raymanrecomp/files/game/
 * (default.xex and the .ipk bundles from the player's own copy).
 *
 * Extra runtime options can be put one per line in
 * /sdcard/Android/data/io.github.belmantegu.raymanrecomp/files/args.txt;
 * lines starting with # are ignored.
 */
public class RaymanActivity extends SDLActivity implements TouchControls.SettingsListener {
    private static final String TAG = "RaymanRecomp";

    private TouchControls touchControls;

    @Override
    protected String[] getLibraries() {
        // Dependencies first: the linker resolves them from the app's lib dir,
        // but loading them explicitly gives clearer errors.
        return new String[] {
            "c++_shared",
            "rexruntime",
            "rayman",
        };
    }

    @Override
    protected String[] getArguments() {
        List<String> args = new ArrayList<>();
        SharedPreferences prefs = getSharedPreferences(TouchControls.PREFS, MODE_PRIVATE);
        // Native renderer (default): no GPU emulation; the game's draws go to
        // Vulkan with its own shaders as SPIR-V (files/spirv). Otherwise the
        // Xenos GPU emulation renders.
        args.add(nativeRenderer(prefs) ? "--gpu_plugin=null" : "--gpu_plugin=xenos");
        // Physical keyboards map to the controller (same bindings as desktop).
        args.add("--mnk_mode=true");
        if (nativeRenderer(prefs) || prefs.getBoolean(TouchControls.KEY_FILL, false)) {
            // A 720p guest video mode with the display's aspect ratio. With the
            // native renderer the game frames a wider scene (RAYMAN_WIDESCREEN,
            // set in onCreate); with the emulated GPU the 16:9 picture is
            // stretched horizontally instead.
            args.add("--video_mode_width=" + wideWidth());
            args.add("--video_mode_height=720");
        }
        File extra = new File(getExternalFilesDir(null), "args.txt");
        if (extra.isFile()) {
            try (BufferedReader reader = new BufferedReader(new FileReader(extra))) {
                String line;
                while ((line = reader.readLine()) != null) {
                    line = line.trim();
                    if (!line.isEmpty() && !line.startsWith("#")) {
                        args.add(line);
                    }
                }
            } catch (IOException e) {
                Log.w(TAG, "Could not read " + extra, e);
            }
        }
        Log.i(TAG, "Arguments: " + args);
        return args.toArray(new String[0]);
    }

    static final String KEY_NATIVE = "native_renderer";
    /** Render resolution, percent of the screen (native renderer), 50..100. */
    static final String KEY_RENDER_SCALE = "render_scale";
    static final int MIN_RENDER_SCALE = 50;

    /** Native renderer (rex/src/native_renderer.cpp): applies on the next frame. */
    static native void nativeSetRenderScale(float scale);

    private boolean nativeRenderer(SharedPreferences prefs) {
        return prefs.getBoolean(KEY_NATIVE, true);
    }

    private int renderScale(SharedPreferences prefs) {
        return Math.max(MIN_RENDER_SCALE, Math.min(100, prefs.getInt(KEY_RENDER_SCALE, 100)));
    }

    /** "75% (1755 x 810)": the resolution the game is drawn at. */
    private String renderScaleLabel(int percent) {
        DisplayMetrics m = getResources().getDisplayMetrics();
        int w = Math.max(m.widthPixels, m.heightPixels), h = Math.min(m.widthPixels, m.heightPixels);
        return getString(R.string.render_scale) + ": " + percent + "% (" + (w * percent / 100) + " x "
            + (h * percent / 100) + ")";
    }

    /** Width of a 720-line video mode with the display's aspect ratio. */
    private int wideWidth() {
        DisplayMetrics m = getResources().getDisplayMetrics();
        int w = Math.max(m.widthPixels, m.heightPixels);
        int h = Math.min(m.widthPixels, m.heightPixels);
        return Math.round(720f * w / h / 8f) * 8;
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // Read by the native libraries (rex/src/native_renderer.cpp), so it has
        // to be set before SDLActivity loads them.
        SharedPreferences prefs = getSharedPreferences(TouchControls.PREFS, MODE_PRIVATE);
        try {
            // SDL3 on Android: SDL_WaitEvent pushes a poll sentinel on every
            // pass, every pushed event wakes the Android event wait, so the
            // runtime's UI loop spun on a whole core. The sentinel only matters
            // to SDL_PollEvent loops; turn it off (docs/ANDROID_PERFORMANCE.md).
            Os.setenv("SDL_POLL_SENTINEL", "0", true);
            // Frame dumps: adb creates captures/dump_now here (native_capture.cpp).
            Os.setenv("RAYMAN_CAPTURE_DIR", getExternalFilesDir(null).getPath(), true);
            // Diagnostic: logs the vertex layouts D3D fills into the shaders.
            if (new File(getExternalFilesDir(null), "fetch_log").exists()) {
                Os.setenv("RAYMAN_NATIVE_FETCH_LOG", "1", true);
            }
            if (nativeRenderer(prefs)) {
                Os.setenv("RAYMAN_NATIVE_RENDER", "main", true);
                Os.setenv("RAYMAN_NATIVE_SPIRV", new File(getExternalFilesDir(null), "spirv").getPath(), true);
                // Native widescreen: the game frames the display's aspect ratio.
                Os.setenv("RAYMAN_WIDESCREEN", String.valueOf(wideWidth() / 720f), true);
                Os.setenv("RAYMAN_RENDER_SCALE", String.valueOf(renderScale(prefs) / 100f), true);
            } else {
                Os.unsetenv("RAYMAN_NATIVE_RENDER");
            }
        } catch (ErrnoException e) {
            Log.w(TAG, "setenv failed", e);
        }
        super.onCreate(savedInstanceState);
        if (mLayout == null) {
            return;  // SDL failed to load and is showing its error dialog
        }
        touchControls = new TouchControls(this);
        touchControls.setSettingsListener(this);
        mLayout.addView(touchControls, new ViewGroup.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        touchControls.applyPreferences();
    }

    // Touches in flight when the window loses focus (notification shade, system
    // gestures, dialogs, app switch) never deliver their "up": let go of them.
    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (!hasFocus && touchControls != null) {
            touchControls.releaseAll();
        }
    }

    @Override
    protected void onPause() {
        if (touchControls != null) {
            touchControls.releaseAll();
        }
        super.onPause();
    }

    @Override
    public void onOpenSettings() {
        final SharedPreferences prefs = getSharedPreferences(TouchControls.PREFS, MODE_PRIVATE);
        final boolean fillBefore = prefs.getBoolean(TouchControls.KEY_FILL, false);
        final boolean nativeBefore = nativeRenderer(prefs);
        int pad = Math.round(20 * getResources().getDisplayMetrics().density);

        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(pad, pad / 2, pad, 0);

        final CheckBox visible = new CheckBox(this);
        visible.setText(R.string.controls_visible);
        visible.setChecked(prefs.getBoolean(TouchControls.KEY_VISIBLE, true));
        box.addView(visible);

        final CheckBox runToggle = new CheckBox(this);
        runToggle.setText(R.string.run_toggle);
        runToggle.setChecked(prefs.getBoolean(TouchControls.KEY_RUN_TOGGLE, true));
        box.addView(runToggle);

        final SeekBar opacity = addSlider(box, R.string.controls_opacity,
            prefs.getInt(TouchControls.KEY_OPACITY, 55) - 10, 90);
        final SeekBar size = addSlider(box, R.string.controls_size,
            prefs.getInt(TouchControls.KEY_SCALE, 100) - 60, 90);

        final CheckBox fill = new CheckBox(this);
        fill.setText(R.string.fill_screen);
        fill.setChecked(fillBefore);
        box.addView(fill);

        final CheckBox nativeBox = new CheckBox(this);
        nativeBox.setText(R.string.native_renderer);
        nativeBox.setChecked(nativeBefore);
        box.addView(nativeBox);

        // Render resolution: previewed live while sliding (the game is behind
        // the dialog), kept on OK, restored on cancel.
        final int scaleBefore = renderScale(prefs);
        final TextView scaleTitle = new TextView(this);
        scaleTitle.setText(renderScaleLabel(scaleBefore));
        scaleTitle.setPadding(0, scaleTitle.getPaddingTop() + 16, 0, 0);
        final SeekBar scale = new SeekBar(this);
        scale.setMax(100 - MIN_RENDER_SCALE);
        scale.setProgress(scaleBefore - MIN_RENDER_SCALE);
        scale.setEnabled(nativeBefore);
        scale.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar bar, int progress, boolean fromUser) {
                int percent = progress + MIN_RENDER_SCALE;
                scaleTitle.setText(renderScaleLabel(percent));
                if (nativeBefore) {
                    nativeSetRenderScale(percent / 100f);
                }
            }

            @Override
            public void onStartTrackingTouch(SeekBar bar) {}

            @Override
            public void onStopTrackingTouch(SeekBar bar) {}
        });
        box.addView(scaleTitle);
        box.addView(scale);

        new AlertDialog.Builder(this)
            .setTitle(R.string.settings_title)
            .setView(box)
            .setPositiveButton(android.R.string.ok, (dialog, which) -> {
                prefs.edit()
                    .putBoolean(TouchControls.KEY_VISIBLE, visible.isChecked())
                    .putBoolean(TouchControls.KEY_RUN_TOGGLE, runToggle.isChecked())
                    .putInt(TouchControls.KEY_OPACITY, opacity.getProgress() + 10)
                    .putInt(TouchControls.KEY_SCALE, size.getProgress() + 60)
                    .putBoolean(TouchControls.KEY_FILL, fill.isChecked())
                    .putBoolean(KEY_NATIVE, nativeBox.isChecked())
                    .putInt(KEY_RENDER_SCALE, scale.getProgress() + MIN_RENDER_SCALE)
                    .apply();
                touchControls.applyPreferences();
                if (fill.isChecked() != fillBefore || nativeBox.isChecked() != nativeBefore) {
                    new AlertDialog.Builder(this)
                        .setMessage(R.string.restart_needed)
                        .setPositiveButton(android.R.string.ok, null)
                        .show();
                }
            })
            .setNegativeButton(android.R.string.cancel, (dialog, which) -> {
                if (nativeBefore) {
                    nativeSetRenderScale(scaleBefore / 100f);
                }
            })
            .setOnCancelListener(dialog -> {
                if (nativeBefore) {
                    nativeSetRenderScale(scaleBefore / 100f);
                }
            })
            .setNeutralButton(R.string.home_screen, (dialog, which) ->
                startActivity(new Intent(this, LauncherActivity.class)
                    .putExtra(LauncherActivity.EXTRA_HOME, true)))
            .show();
    }

    // SDL asks for any orientation when it creates a resizable window (the
    // user's rotation lock decides). The game is landscape only.
    @Override
    public void setOrientationBis(int w, int h, boolean resizable, String hint) {
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
    }

    private SeekBar addSlider(LinearLayout box, int label, int value, int max) {
        TextView title = new TextView(this);
        title.setText(label);
        title.setPadding(0, title.getPaddingTop() + 16, 0, 0);
        box.addView(title);
        SeekBar bar = new SeekBar(this);
        bar.setMax(max);
        bar.setProgress(Math.max(0, Math.min(max, value)));
        box.addView(bar);
        return bar;
    }
}
