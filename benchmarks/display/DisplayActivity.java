package org.velox.displaybench;

import android.app.Activity;
import android.opengl.GLES20;
import android.opengl.GLSurfaceView;
import android.os.Bundle;
import android.util.Log;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.WindowManager;
import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.opengles.GL10;

public final class DisplayActivity extends Activity {
    private static final String TAG = "VeloxDisplayBench";
    private GLSurfaceView view;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        final float hz = getIntent().getFloatExtra("requestHz", 0);
        view = new GLSurfaceView(this) {
            @Override
            public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
                super.surfaceChanged(holder, format, width, height);
                if (Float.isFinite(hz) && hz > 0) {
                    holder.getSurface().setFrameRate(hz, Surface.FRAME_RATE_COMPATIBILITY_DEFAULT);
                }
            }
        };
        view.setEGLContextClientVersion(2);
        view.setRenderer(new GLSurfaceView.Renderer() {
            private int totalFrames;
            private int windowFrames;
            private long begin;

            @Override
            public void onSurfaceCreated(GL10 ignored, EGLConfig config) {
                begin = 0;
                totalFrames = 0;
                windowFrames = 0;
            }

            @Override
            public void onSurfaceChanged(GL10 ignored, int width, int height) {
                GLES20.glViewport(0, 0, width, height);
            }

            @Override
            public void onDrawFrame(GL10 ignored) {
                final float phase = (totalFrames & 127) / 127.0f;
                GLES20.glClearColor(phase, 1.0f - phase, 0.25f, 1.0f);
                GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
                if (++totalFrames < 30) return;
                final long now = System.nanoTime();
                if (begin == 0) {
                    begin = now;
                    return;
                }
                ++windowFrames;
                if (now - begin >= 5_000_000_000L) {
                    Log.i(TAG, "rendered_frames=" + windowFrames + " duration_ns=" + (now - begin));
                    begin = now;
                    windowFrames = 0;
                }
            }
        });
        setContentView(view);
    }

    @Override
    protected void onResume() {
        super.onResume();
        view.onResume();
    }

    @Override
    protected void onPause() {
        view.onPause();
        super.onPause();
    }
}
