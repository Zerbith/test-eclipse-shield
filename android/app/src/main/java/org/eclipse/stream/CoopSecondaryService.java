package org.eclipse.stream;

import android.app.Service;
import android.content.Intent;
import android.os.IBinder;
import android.os.Process;
import android.util.Log;

/**
 * Hosts the SECOND stream of a co-op session.
 *
 * Eclipse's co-op mode runs each host's Moonlight session in its own process, because
 * moonlight-common-c keeps its connection state in file-scope globals. On desktop and webOS the
 * client forks and execs itself with "--coop-secondary <socket>". Android cannot do that (the
 * client is a shared library inside the app process), so the same thing is done with an Android
 * process: this service is declared with android:process=":coop", which gives it a process of its
 * own. It loads the same native library and runs the headless secondary entry point, which only
 * receives network data and forwards it to the coordinator (the main process) over a socket.
 *
 * The process exits when the secondary finishes or the service is stopped. That is deliberate:
 * the native code keeps process-wide state, and a reused process would start the next co-op
 * session on top of the last one's leftovers.
 */
public class CoopSecondaryService extends Service {
    private static final String TAG = "EclipseCoop";

    static {
        // SDL2 must be loaded explicitly (not only as a dependency of libmain) so that its
        // JNI_OnLoad runs and registers the Java VM with SDL's thread helpers.
        System.loadLibrary("SDL2");
        System.loadLibrary("main");
    }

    private Thread worker;

    private static native int nativeRun(String filesDir, String socketPath, String hostUuid,
                                        int appId, int width, int height);

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent == null || worker != null) {
            return START_NOT_STICKY;
        }
        final String socket = intent.getStringExtra("socket");
        final String host = intent.getStringExtra("host");
        final int appId = intent.getIntExtra("appId", 0);
        final int width = intent.getIntExtra("width", 0);
        final int height = intent.getIntExtra("height", 0);
        final String filesDir = getFilesDir().getAbsolutePath();

        worker = new Thread(() -> {
            int rc = -1;
            try {
                rc = nativeRun(filesDir, socket, host, appId, width, height);
            } catch (Throwable t) {
                Log.e(TAG, "co-op secondary failed", t);
            }
            Log.i(TAG, "co-op secondary finished, rc=" + rc);
            stopSelf();
            Process.killProcess(Process.myPid());
        }, "eclipse-coop-secondary");
        worker.start();
        return START_NOT_STICKY;
    }

    @Override
    public void onDestroy() {
        super.onDestroy();
        Process.killProcess(Process.myPid());
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
