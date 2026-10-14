package org.eclipse.stream;

import android.content.Context;
import android.content.Intent;
import android.graphics.PixelFormat;
import android.net.wifi.WifiManager;
import android.os.Bundle;
import android.util.DisplayMetrics;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import android.widget.RelativeLayout;

import org.libsdl.app.SDLActivity;

import java.io.File;
import java.util.ArrayList;
import java.util.List;

/**
 * Hosts the native Eclipse client (libmain.so) through SDL.
 *
 * Layout: a plain SurfaceView for the hardware-decoded video sits BEHIND SDL's own surface. The
 * native UI draws into the SDL surface with an alpha channel and clears to transparent, so the
 * video shows through wherever the UI is empty. The native MediaCodec module fetches the video
 * Surface through {@link #getVideoSurface()}.
 *
 * STATUS: first draft, not run on a device yet.
 */
public class EclipseActivity extends SDLActivity {
    private static final Object LOCK = new Object();
    private static volatile Surface videoSurface;
    /** The second video surface, used by the two-decoder co-op layout. */
    private static volatile Surface videoSurface2;
    private static volatile EclipseActivity instance;

    private WifiManager.MulticastLock multicastLock;
    private SurfaceView video1View;
    private SurfaceView video2View;

    /** Called from native code (JNI) on a non-UI thread. Blocks up to 3 s for the Surface. */
    public static Surface getVideoSurface() {
        return getVideoSurface(0);
    }

    /**
     * Called from native code (JNI) on a non-UI thread. Index 0 is the main video surface, index 1
     * the second one of the two-decoder co-op layout (see {@link #setCoopLayout(int)}). Blocks up
     * to 4 s for the Surface to exist.
     */
    public static Surface getVideoSurface(int index) {
        synchronized (LOCK) {
            long deadline = System.currentTimeMillis() + 4000;
            while ((index == 0 ? videoSurface : videoSurface2) == null) {
                long left = deadline - System.currentTimeMillis();
                if (left <= 0) {
                    break;
                }
                try {
                    LOCK.wait(left);
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                    break;
                }
            }
            return index == 0 ? videoSurface : videoSurface2;
        }
    }

    /**
     * Called from native code (JNI): where the video surfaces sit. 0 = one surface filling the
     * screen (normal streaming); 1 = co-op horizontal, one surface in each half (top, bottom);
     * 2 = co-op vertical (left, right). Co-op gives every host its own hardware decoder and
     * surface; nothing is composited into one picture.
     */
    public static void setCoopLayout(final int mode) {
        final EclipseActivity activity = instance;
        if (activity == null) {
            return;
        }
        activity.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                activity.applyCoopLayout(mode);
            }
        });
    }

    private void applyCoopLayout(int mode) {
        if (video1View == null || video2View == null || mLayout == null) {
            return;
        }
        int w = mLayout.getWidth();
        int h = mLayout.getHeight();
        if (w <= 0 || h <= 0) {
            DisplayMetrics metrics = getResources().getDisplayMetrics();
            w = metrics.widthPixels;
            h = metrics.heightPixels;
        }
        RelativeLayout.LayoutParams first;
        RelativeLayout.LayoutParams second;
        if (mode == 1) {
            first = new RelativeLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, h / 2);
            first.addRule(RelativeLayout.ALIGN_PARENT_TOP);
            second = new RelativeLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, h - h / 2);
            second.addRule(RelativeLayout.ALIGN_PARENT_BOTTOM);
        } else if (mode == 2) {
            first = new RelativeLayout.LayoutParams(w / 2, ViewGroup.LayoutParams.MATCH_PARENT);
            first.addRule(RelativeLayout.ALIGN_PARENT_LEFT);
            second = new RelativeLayout.LayoutParams(w - w / 2, ViewGroup.LayoutParams.MATCH_PARENT);
            second.addRule(RelativeLayout.ALIGN_PARENT_RIGHT);
        } else {
            first = new RelativeLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.MATCH_PARENT);
            second = new RelativeLayout.LayoutParams(1, 1);
        }
        video1View.setLayoutParams(first);
        video2View.setLayoutParams(second);
        video2View.setVisibility(mode == 0 ? View.GONE : View.VISIBLE);
    }

    /** Called from native code (JNI): starts the co-op secondary stream in its own process. */
    public static void startCoopSecondary(String socket, String hostUuid, int appId, int width,
                                          int height, String environment) {
        Context context = SDLActivity.getContext();
        if (context == null) {
            return;
        }
        Intent intent = new Intent(context, CoopSecondaryService.class);
        intent.putExtra("socket", socket);
        intent.putExtra("host", hostUuid);
        intent.putExtra("appId", appId);
        intent.putExtra("width", width);
        intent.putExtra("height", height);
        intent.putExtra("env", environment);
        context.startService(intent);
    }

    /** Called from native code (JNI): stops the co-op secondary stream. */
    public static void stopCoopSecondary() {
        Context context = SDLActivity.getContext();
        if (context != null) {
            context.stopService(new Intent(context, CoopSecondaryService.class));
        }
    }

    @Override
    protected String[] getLibraries() {
        // Only load libraries that were actually packaged by android/scripts/build-native.sh.
        List<String> libs = new ArrayList<>();
        File dir = new File(getApplicationInfo().nativeLibraryDir);
        if (new File(dir, "libhidapi.so").exists()) {
            libs.add("hidapi");
        }
        libs.add("SDL2");
        libs.add("main");
        return libs.toArray(new String[0]);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        // SDLActivity returns early (without creating its layout) when a native library failed to
        // load. Do not crash on top of that; SDL shows its own error message.
        if (mLayout == null) {
            return;
        }

        // Make SDL's surface translucent and put it above the video surface.
        if (mSurface != null) {
            mSurface.getHolder().setFormat(PixelFormat.TRANSLUCENT);
            mSurface.setZOrderMediaOverlay(true);
        }

        instance = this;

        SurfaceView video = new SurfaceView(this);
        video1View = video;
        video.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override
            public void surfaceCreated(SurfaceHolder holder) {
                synchronized (LOCK) {
                    videoSurface = holder.getSurface();
                    LOCK.notifyAll();
                }
            }

            @Override
            public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
            }

            @Override
            public void surfaceDestroyed(SurfaceHolder holder) {
                synchronized (LOCK) {
                    videoSurface = null;
                }
            }
        });
        ViewGroup.LayoutParams params = new RelativeLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT);
        mLayout.addView(video, 0, params);

        // The second surface only exists (is visible) while a co-op session runs.
        SurfaceView video2 = new SurfaceView(this);
        video2View = video2;
        video2.setVisibility(View.GONE);
        video2.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override
            public void surfaceCreated(SurfaceHolder holder) {
                synchronized (LOCK) {
                    videoSurface2 = holder.getSurface();
                    LOCK.notifyAll();
                }
            }

            @Override
            public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
            }

            @Override
            public void surfaceDestroyed(SurfaceHolder holder) {
                synchronized (LOCK) {
                    videoSurface2 = null;
                }
            }
        });
        mLayout.addView(video2, 1, new RelativeLayout.LayoutParams(1, 1));

        // mDNS host discovery is silently dropped by many Wi-Fi drivers without this lock.
        WifiManager wifi = (WifiManager) getApplicationContext().getSystemService(WIFI_SERVICE);
        if (wifi != null) {
            multicastLock = wifi.createMulticastLock("eclipse-mdns");
            multicastLock.setReferenceCounted(false);
            multicastLock.acquire();
        }
    }

    @Override
    protected void onDestroy() {
        if (instance == this) {
            instance = null;
        }
        if (multicastLock != null && multicastLock.isHeld()) {
            multicastLock.release();
        }
        super.onDestroy();
    }
}
