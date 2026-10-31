package app.trierarch.devicetest;

import android.app.Activity;
import android.graphics.Color;
import android.os.Bundle;
import android.util.Log;
import android.view.Gravity;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.WindowManager;
import android.widget.FrameLayout;

public final class MainActivity extends Activity {
    private static final String TAG = "TrierarchScanoutTest";
    private SurfaceView surfaceView;
    private boolean ran;

    static {
        System.loadLibrary("trierarch_scanout_test");
    }

    private native int runScanout(Surface surface);

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(Color.BLACK);
        surfaceView = new SurfaceView(this);
        FrameLayout.LayoutParams params = new FrameLayout.LayoutParams(256, 256, Gravity.CENTER);
        root.addView(surfaceView, params);
        setContentView(root);
        surfaceView.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override public void surfaceCreated(SurfaceHolder holder) {}

            @Override public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
                if (ran) return;
                ran = true;
                int result = runScanout(holder.getSurface());
                Log.i(TAG, "scanout-result=" + result + " size=" + width + "x" + height);
            }

            @Override public void surfaceDestroyed(SurfaceHolder holder) {}
        });
    }
}
