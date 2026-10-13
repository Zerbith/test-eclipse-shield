package org.eclipse.stream;

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
