package org.eclipse.stream;

import android.content.Context;
import android.content.Intent;
import android.graphics.PixelFormat;
import android.net.wifi.WifiManager;
import android.os.Bundle;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
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

    private WifiManager.MulticastLock multicastLock;

    /** Called from native code (JNI) on a non-UI thread. Blocks up to 3 s for the Surface. */
    public static Surface getVideoSurface() {
        synchronized (LOCK) {
            long deadline = System.currentTimeMillis() + 3000;
            while (videoSurface == null) {
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
            return videoSurface;
        }
    }

    /** Called from native code (JNI): starts the co-op secondary stream in its own process. */
    public static void startCoopSecondary(String socket, String hostUuid, int appId, int width,
                                          int height) {
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

        SurfaceView video = new SurfaceView(this);
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
        if (multicastLock != null && multicastLock.isHeld()) {
            multicastLock.release();
        }
        super.onDestroy();
    }
}
