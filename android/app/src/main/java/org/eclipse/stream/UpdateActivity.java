package org.eclipse.stream;

import android.app.Activity;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.SharedPreferences;
import android.content.pm.PackageInfo;
import android.content.pm.PackageInstaller;
import android.graphics.Color;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.provider.Settings;
import android.text.InputType;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;

/**
 * In-app updater: asks GitHub for the newest APK the CI published as a release of the
 * (private) repo, downloads it and hands it to Android's installer. Because every build
 * is signed with the same key this is a real update: pairing and settings are kept.
 *
 * The repo is private, so a read-only GitHub token is needed. It is typed in once here and
 * stored on the device only.
 */
public class UpdateActivity extends Activity {
    private static final String REPO = "Zerbith/test-eclipse-shield";
    private static final String TAG = "android-latest";
    private static final String ACTION_STATUS = "org.eclipse.stream.UPDATE_STATUS";
    private static final String PREFS = "updater";

    private EditText tokenField;
    private TextView status;
    private Button checkButton;
    private Button installButton;
    private long latestBuild = -1;
    private long latestAssetId = -1;
    private String latestTitle = "";
    private volatile boolean busy;

    private final BroadcastReceiver installReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            int code = intent.getIntExtra(PackageInstaller.EXTRA_STATUS, -1);
            if (code == PackageInstaller.STATUS_PENDING_USER_ACTION) {
                Intent confirm = intent.getParcelableExtra(Intent.EXTRA_INTENT);
                if (confirm != null) {
                    startActivity(confirm);
                }
            } else if (code == PackageInstaller.STATUS_SUCCESS) {
                say("Installed.");
            } else {
                say("Install failed: " + intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE));
                setBusy(false);
            }
        }
    };

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(80, 60, 80, 60);
        root.setBackgroundColor(Color.rgb(18, 18, 24));

        TextView title = new TextView(this);
        title.setText("Eclipse update");
        title.setTextSize(30);
        title.setTextColor(Color.WHITE);
        root.addView(title);

        TextView installed = new TextView(this);
        installed.setText("Installed build: " + installedBuild() + "  (" + installedName() + ")");
        installed.setTextColor(Color.LTGRAY);
        installed.setTextSize(18);
        installed.setPadding(0, 20, 0, 20);
        root.addView(installed);

        TextView tokenHint = new TextView(this);
        tokenHint.setText("GitHub token (read-only access to " + REPO + ", entered once):");
        tokenHint.setTextColor(Color.LTGRAY);
        tokenHint.setTextSize(16);
        root.addView(tokenHint);

        tokenField = new EditText(this);
        tokenField.setSingleLine(true);
        tokenField.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_PASSWORD);
        tokenField.setTextColor(Color.WHITE);
        tokenField.setText(prefs().getString("token", ""));
        root.addView(tokenField);

        Button save = button("Save token");
        save.setOnClickListener(v -> {
            prefs().edit().putString("token", tokenField.getText().toString().trim()).apply();
            say("Token saved.");
        });
        root.addView(save);

        checkButton = button("Check for update");
        checkButton.setOnClickListener(v -> check());
        root.addView(checkButton);

        installButton = button("Download and install");
        installButton.setEnabled(false);
        installButton.setOnClickListener(v -> download());
        root.addView(installButton);

        Button close = button("Back");
        close.setOnClickListener(v -> finish());
        root.addView(close);

        status = new TextView(this);
        status.setTextColor(Color.WHITE);
        status.setTextSize(18);
        status.setPadding(0, 30, 0, 0);
        root.addView(status);

        ScrollView scroll = new ScrollView(this);
        scroll.addView(root);
        setContentView(scroll);
        checkButton.requestFocus();
        say(prefs().getString("token", "").isEmpty()
                ? "Enter your GitHub token first, then choose Save token."
                : "Choose Check for update.");
    }

    @Override
    protected void onStart() {
        super.onStart();
        IntentFilter filter = new IntentFilter(ACTION_STATUS);
        if (Build.VERSION.SDK_INT >= 33) {
            registerReceiver(installReceiver, filter, Context.RECEIVER_NOT_EXPORTED);
        } else {
            registerReceiver(installReceiver, filter);
        }
    }

    @Override
    protected void onStop() {
        super.onStop();
        try {
            unregisterReceiver(installReceiver);
        } catch (IllegalArgumentException ignored) {
        }
    }

    private Button button(String text) {
        Button b = new Button(this);
        b.setText(text);
        b.setAllCaps(false);
        b.setFocusable(true);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.topMargin = 16;
        lp.gravity = Gravity.START;
        b.setLayoutParams(lp);
        return b;
    }

    private SharedPreferences prefs() {
        return getSharedPreferences(PREFS, MODE_PRIVATE);
    }

    private long installedBuild() {
        try {
            PackageInfo info = getPackageManager().getPackageInfo(getPackageName(), 0);
            return Build.VERSION.SDK_INT >= 28 ? info.getLongVersionCode() : info.versionCode;
        } catch (Exception e) {
            return -1;
        }
    }

    private String installedName() {
        try {
            return getPackageManager().getPackageInfo(getPackageName(), 0).versionName;
        } catch (Exception e) {
            return "?";
        }
    }

    private void say(String text) {
        runOnUiThread(() -> status.setText(text));
    }

    private void setBusy(boolean value) {
        busy = value;
        runOnUiThread(() -> {
            checkButton.setEnabled(!value);
            installButton.setEnabled(!value && latestBuild > installedBuild());
        });
    }

    /* ---- GitHub ---- */

    private HttpURLConnection open(String url, String accept) throws Exception {
        HttpURLConnection c = (HttpURLConnection) new URL(url).openConnection();
        c.setConnectTimeout(15000);
        c.setReadTimeout(30000);
        c.setInstanceFollowRedirects(false);
        c.setRequestProperty("Accept", accept);
        c.setRequestProperty("User-Agent", "eclipse-shield-updater");
        return c;
    }

    private void check() {
        final String token = prefs().getString("token", "");
        if (token.isEmpty()) {
            say("Enter your GitHub token first, then choose Save token.");
            return;
        }
        if (busy) {
            return;
        }
        setBusy(true);
        say("Checking...");
        new Thread(() -> {
            try {
                HttpURLConnection c = open("https://api.github.com/repos/" + REPO + "/releases/tags/" + TAG,
                        "application/vnd.github+json");
                c.setRequestProperty("Authorization", "Bearer " + token);
                int code = c.getResponseCode();
                if (code == 401 || code == 403) {
                    say("GitHub refused the token (HTTP " + code + "). Check it and its access to the repo.");
                    setBusy(false);
                    return;
                }
                if (code == 404) {
                    say("No published build found yet (HTTP 404), or the token has no access to the repo.");
                    setBusy(false);
                    return;
                }
                if (code != 200) {
                    say("GitHub answered HTTP " + code + ".");
                    setBusy(false);
                    return;
                }
                JSONObject release = new JSONObject(readAll(c.getInputStream()));
                JSONArray assets = release.getJSONArray("assets");
                long best = -1;
                long bestId = -1;
                for (int i = 0; i < assets.length(); i++) {
                    JSONObject a = assets.getJSONObject(i);
                    String name = a.getString("name");   // eclipse-android-<build>.apk
                    if (name.startsWith("eclipse-android-") && name.endsWith(".apk")) {
                        try {
                            long n = Long.parseLong(name.substring("eclipse-android-".length(), name.length() - 4));
                            if (n > best) {
                                best = n;
                                bestId = a.getLong("id");
                            }
                        } catch (NumberFormatException ignored) {
                        }
                    }
                }
                if (best < 0) {
                    say("The release has no APK.");
                    setBusy(false);
                    return;
                }
                latestBuild = best;
                latestAssetId = bestId;
                latestTitle = release.optString("name", "build " + best);
                long mine = installedBuild();
                if (best > mine) {
                    say("Update available: " + latestTitle + "\n(installed build " + mine + ")");
                } else {
                    say("You are up to date: " + latestTitle);
                }
                setBusy(false);
            } catch (Exception e) {
                say("Check failed: " + e);
                setBusy(false);
            }
        }).start();
    }

    private static String readAll(InputStream in) throws Exception {
        java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream();
        byte[] buf = new byte[8192];
        int n;
        while ((n = in.read(buf)) > 0) {
            out.write(buf, 0, n);
        }
        in.close();
        return out.toString("UTF-8");
    }

    private void download() {
        if (busy || latestAssetId < 0) {
            return;
        }
        if (Build.VERSION.SDK_INT >= 26 && !getPackageManager().canRequestPackageInstalls()) {
            say("Android must allow Eclipse to install apps. Switch it on in the next screen, then come back and choose Download again.");
            startActivity(new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,
                    Uri.parse("package:" + getPackageName())));
            return;
        }
        final String token = prefs().getString("token", "");
        setBusy(true);
        say("Downloading...");
        new Thread(() -> {
            try {
                String url = "https://api.github.com/repos/" + REPO + "/releases/assets/" + latestAssetId;
                HttpURLConnection c = open(url, "application/octet-stream");
                c.setRequestProperty("Authorization", "Bearer " + token);
                int code = c.getResponseCode();
                // The asset is served from another host with a signed address: follow it by hand
                // WITHOUT the token, which that host would reject.
                if (code >= 300 && code < 400) {
                    String location = c.getHeaderField("Location");
                    c.disconnect();
                    c = open(location, "application/octet-stream");
                    code = c.getResponseCode();
                }
                if (code != 200) {
                    say("Download failed: HTTP " + code);
                    setBusy(false);
                    return;
                }
                File apk = new File(getCacheDir(), "update.apk");
                long total = c.getContentLengthLong();
                long done = 0;
                try (InputStream in = c.getInputStream(); OutputStream out = new FileOutputStream(apk)) {
                    byte[] buf = new byte[65536];
                    int n;
                    long lastShown = 0;
                    while ((n = in.read(buf)) > 0) {
                        out.write(buf, 0, n);
                        done += n;
                        if (done - lastShown > 1_000_000) {
                            lastShown = done;
                            say("Downloading... " + (done / 1000000) + (total > 0 ? " / " + (total / 1000000) : "") + " MB");
                        }
                    }
                }
                say("Installing...");
                install(apk);
            } catch (Exception e) {
                say("Download failed: " + e);
                setBusy(false);
            }
        }).start();
    }

    private void install(File apk) throws Exception {
        PackageInstaller installer = getPackageManager().getPackageInstaller();
        PackageInstaller.SessionParams params =
                new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
        params.setAppPackageName(getPackageName());
        int id = installer.createSession(params);
        try (PackageInstaller.Session session = installer.openSession(id);
             InputStream in = new java.io.FileInputStream(apk);
             OutputStream out = session.openWrite("update", 0, apk.length())) {
            byte[] buf = new byte[65536];
            int n;
            while ((n = in.read(buf)) > 0) {
                out.write(buf, 0, n);
            }
            session.fsync(out);
            Intent intent = new Intent(ACTION_STATUS).setPackage(getPackageName());
            int flags = PendingIntent.FLAG_UPDATE_CURRENT | (Build.VERSION.SDK_INT >= 31 ? PendingIntent.FLAG_MUTABLE : 0);
            PendingIntent pending = PendingIntent.getBroadcast(this, id, intent, flags);
            session.commit(pending.getIntentSender());
        }
    }
}
