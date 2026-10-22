package com.halo.decomp;

import android.app.Activity;
import android.app.ActivityManager;
import android.app.AlertDialog;
import android.content.ActivityNotFoundException;
import android.content.Context;
import android.content.Intent;
import android.graphics.Color;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.StatFs;
import android.provider.DocumentsContract;
import android.util.TypedValue;
import android.view.Gravity;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.io.BufferedOutputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.List;
import java.util.Locale;

/**
 * Backs up and restores the player's saved games and settings, so that they
 * outlive an uninstall: the profiles with their saved games, config.toml
 * with the server browsers' favorites and the touch layout go into one zip
 * (BackupArchive) that the player keeps anywhere through the system file
 * picker, with no storage permission. The maps are left out: the disc image
 * gives them back.
 *
 * It is opened by the app icon's "Back up saves" shortcut and by the
 * launcher's "Restore saves and settings" button. It refuses to work while
 * the game's process (":game") is alive, since the game reads and writes the
 * same files, and the game's native library is never loaded here.
 *
 * It runs in a process of its own (":backup"). The touch layout lives in
 * SharedPreferences, which are cached per process, so a process that
 * outlived a game session would read, and a restore would write back, a
 * stale copy of it; a fresh process reads it as it is on disk. So the
 * process ends when the screen is closed and nothing is at work, or when
 * the work ends after the screen was closed; a run the player backed out of
 * shows no result (it is logged). The limit that remains: the preferences
 * are read when a back-up or a restore runs, so a layout the game changes
 * after that, while this screen stays open or stopped in the background, is
 * not seen until the screen is opened fresh (MODE_MULTI_PROCESS is
 * deprecated and not used).
 */
public class BackupActivity extends Activity {
    private static final int EXPORT_ARCHIVE = 501;
    private static final int IMPORT_ARCHIVE = 502;
    private static final int ACTION_NONE = 0;
    private static final int ACTION_EXPORT = 1;
    private static final int ACTION_RESTORE = 2;
    private static final int PART_PROFILES = 0;
    private static final int PART_SETTINGS = 1;
    private static final int PART_LAYOUT = 2;
    private static final long SPARE_BYTES = 64L * 1024 * 1024;
    private static final int BUFFER_BYTES = 64 * 1024;
    private static final long END_DELAY_MS = 2000;
    private static final String PREFERENCES = "touch-layout-v1";
    private static final String KEY_CONFIGURATION = "configuration";
    private static final String TITLE = "Saved games and settings";
    private static final String GAME_RUNNING = "The game is running. Quit it from its main menu first.";

    /** true while a back-up or a restore is at work in this process, so that no one sweeps its staging folder */
    static volatile boolean working;

    /** the text of the last finished back-up or restore, until an instance on screen shows it (UI thread only) */
    private static String lastResult;

    /** the instances of this screen alive in this process, between onCreate and onDestroy (UI thread only) */
    private static int alive;

    /** the instance between onResume and onPause, which shows a result at once (UI thread only) */
    private static BackupActivity resumed;

    private File dataRoot;
    private TextView status;
    private Button backUp;
    private Button restore;
    private int pendingAction = ACTION_NONE;
    private Uri document;
    private boolean[] chosen = {true, true, true};

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        alive++;
        dataRoot = getExternalFilesDir(null);
        if (dataRoot != null && !working) {
            String recovered = BackupArchive.recover(dataRoot);
            if (!recovered.isEmpty())
                android.util.Log.i("halo", "backup: " + recovered);
        }
        if (savedInstanceState != null) {
            pendingAction = savedInstanceState.getInt("pending-backup-action", ACTION_NONE);
            document = savedInstanceState.getParcelable("backup-document");
            boolean[] parts = savedInstanceState.getBooleanArray("backup-parts");
            if (parts != null && parts.length == 3)
                chosen = parts;
        }
        buildInterface();
        if (dataRoot == null) {
            status.setText("The app's storage is not available, so nothing can be backed up or restored.");
            setBusy(true);
            return;
        }
        if (working) {
            // the worker of an earlier instance is still at work; its result
            // is shown here when it ends (lastResult)
            finishAction();
            setBusy(true);
            status.setText("Working...");
        } else if (document != null && pendingAction != ACTION_NONE) {
            // never started again by itself: a back-up that did finish would
            // be overwritten or deleted by a run from this old state
            status.setText(pendingAction == ACTION_EXPORT
                ? "The last back-up may not have finished. Press Back up to make it again."
                : "The last restore may not have finished. Press Restore to choose the back-up again.");
            finishAction();
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        resumed = this;
        showLastResult();
    }

    @Override
    protected void onPause() {
        if (resumed == this)
            resumed = null;
        super.onPause();
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        // a recreation comes through here too; the count of live instances
        // tells it apart from the screen being closed
        alive--;
        endProcessWhenIdle();
    }

    /** the main thread's handler, which also carries the one pending end check */
    private static final Handler MAIN = new Handler(Looper.getMainLooper());

    /** the end check; every close and every finished worker posts it anew, so the last one sets the delay */
    private static final Runnable END_CHECK = () -> {
        if (alive == 0 && !working) {
            // a run the player backed out of has no screen to show its result
            if (lastResult != null)
                android.util.Log.i("halo", "backup: " + lastResult);
            android.os.Process.killProcess(android.os.Process.myPid());
        }
    };

    /**
     * Ends this process once no instance of the screen is alive and no worker is at work, so that the next
     * open reads the touch layout's SharedPreferences fresh from disk.
     */
    private static void endProcessWhenIdle() {
        // checked two seconds later, not now: the app icon's shortcut starts
        // this screen with FLAG_ACTIVITY_NEW_TASK | FLAG_ACTIVITY_CLEAR_TASK,
        // so a tap on it while an idle instance is stopped finishes that
        // instance while the next one is being created in this same process,
        // and the delay lets the new onCreate raise alive first; a check
        // still pending is dropped, so the delay runs from the last event
        MAIN.removeCallbacks(END_CHECK);
        MAIN.postDelayed(END_CHECK, END_DELAY_MS);
    }

    private int dp(float value) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, value,
            getResources().getDisplayMetrics());
    }

    private void buildInterface() {
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setGravity(Gravity.CENTER);
        layout.setPadding(dp(48), dp(24), dp(48), dp(24));
        layout.setBackgroundColor(Color.rgb(12, 16, 20));

        TextView title = new TextView(this);
        title.setText(TITLE);
        title.setTextColor(Color.WHITE);
        title.setTextSize(TypedValue.COMPLEX_UNIT_SP, 24);
        title.setGravity(Gravity.CENTER);
        layout.addView(title);

        TextView message = new TextView(this);
        message.setText("A back-up holds your profiles with their saved games, the settings (config.toml "
            + "and the server browser's favorites) and the touch layout. It does not hold the maps: the disc "
            + "image gives those back. Quit the game from its main menu before you back up or restore.");
        message.setTextColor(Color.rgb(200, 205, 210));
        message.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
        message.setGravity(Gravity.CENTER);
        message.setPadding(0, dp(16), 0, dp(16));
        layout.addView(message);

        LinearLayout buttons = new LinearLayout(this);
        buttons.setOrientation(LinearLayout.HORIZONTAL);
        buttons.setGravity(Gravity.CENTER);

        backUp = new Button(this);
        backUp.setText("Back up");
        backUp.setOnClickListener(v -> chooseExport());
        buttons.addView(backUp, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT,
            LinearLayout.LayoutParams.WRAP_CONTENT));

        restore = new Button(this);
        restore.setText("Restore");
        restore.setOnClickListener(v -> chooseRestore());
        LinearLayout.LayoutParams restoreLayout = new LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        restoreLayout.leftMargin = dp(16);
        buttons.addView(restore, restoreLayout);

        layout.addView(buttons, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT,
            LinearLayout.LayoutParams.WRAP_CONTENT));

        status = new TextView(this);
        status.setTextColor(Color.rgb(160, 200, 160));
        status.setGravity(Gravity.CENTER);
        status.setPadding(0, dp(16), 0, 0);
        layout.addView(status);

        setContentView(layout);
        backUp.requestFocus();
    }

    private void setBusy(boolean busy) {
        backUp.setEnabled(!busy);
        restore.setEnabled(!busy);
    }

    /** Whether the game's process is alive. */
    private boolean gameRunning() {
        ActivityManager manager = (ActivityManager) getSystemService(Context.ACTIVITY_SERVICE);
        if (manager == null)
            return false;
        List<ActivityManager.RunningAppProcessInfo> processes = manager.getRunningAppProcesses();
        if (processes == null)
            return false;
        String game = getPackageName() + ":game";
        for (ActivityManager.RunningAppProcessInfo process : processes)
            if (game.equals(process.processName))
                return true;
        return false;
    }

    private void gameRunningDialog(Runnable again) {
        new AlertDialog.Builder(this)
            .setTitle("The game is running")
            .setMessage("Quit Halo from its main menu first, then try again.")
            .setPositiveButton("Check again", (dialog, which) -> again.run())
            .setNegativeButton("Cancel", null)
            .show();
    }

    private void chooseExport() {
        if (working)
            return;
        if (gameRunning()) {
            gameRunningDialog(this::chooseExport);
            return;
        }
        Intent intent = new Intent(Intent.ACTION_CREATE_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("application/zip");
        intent.putExtra(Intent.EXTRA_TITLE,
            "openh1-saves-" + new SimpleDateFormat("yyyy-MM-dd", Locale.ROOT).format(new Date()) + ".zip");
        startPicker(intent, EXPORT_ARCHIVE, ACTION_EXPORT);
    }

    private void chooseRestore() {
        if (working)
            return;
        if (gameRunning()) {
            gameRunningDialog(this::chooseRestore);
            return;
        }
        // (any file, as the layout import does: a zip's MIME type differs
        // between providers, and the scan refuses anything else)
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        startPicker(intent, IMPORT_ARCHIVE, ACTION_RESTORE);
    }

    private void startPicker(Intent intent, int request, int action) {
        pendingAction = action;
        document = null;
        status.setText("");
        try {
            startActivityForResult(intent, request);
        } catch (ActivityNotFoundException e) {
            pendingAction = ACTION_NONE;
            Toast.makeText(this, "This device has no file picker.", Toast.LENGTH_LONG).show();
        }
    }

    @Override
    protected void onSaveInstanceState(Bundle state) {
        super.onSaveInstanceState(state);
        state.putInt("pending-backup-action", pendingAction);
        state.putParcelable("backup-document", document);
        state.putBooleanArray("backup-parts", chosen);
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        if (request != EXPORT_ARCHIVE && request != IMPORT_ARCHIVE) {
            super.onActivityResult(request, result, data);
            return;
        }
        if (result != RESULT_OK || data == null || data.getData() == null) {
            finishAction();
            return;
        }
        document = data.getData();
        chosen = new boolean[] {true, true, true};
        if (request == EXPORT_ARCHIVE) {
            pendingAction = ACTION_EXPORT;
            startExport(document);
        } else {
            pendingAction = ACTION_RESTORE;
            startScan(document);
        }
    }

    private void finishAction() {
        pendingAction = ACTION_NONE;
        document = null;
    }

    private void startExport(Uri target) {
        if (working)
            return;
        setBusy(true);
        status.setText("Writing the back-up...");
        working = true;
        new Thread(() -> {
            boolean opened = false;
            try {
                // checked before the document is opened, so a refused back-up
                // leaves the picked file as it was
                if (gameRunning())
                    throw new IOException(GAME_RUNNING);
                String layoutText = getSharedPreferences(PREFERENCES, MODE_PRIVATE)
                    .getString(KEY_CONFIGURATION, null);
                OutputStream stream = getContentResolver().openOutputStream(target, "wt");
                if (stream == null)
                    throw new IOException("The file could not be opened.");
                opened = true;
                BackupArchive.Summary summary;
                try (OutputStream out = new BufferedOutputStream(stream, BUFFER_BYTES)) {
                    summary = BackupArchive.write(out, dataRoot, layoutText, BuildConfig.HALO_BUILD_NUMBER,
                        new Date());
                }
                // a game started during the write may have changed the files
                // under it, so the back-up cannot be trusted
                if (gameRunning())
                    throw new IOException("The game was started while the back-up was written, so the back-up "
                        + "was removed. Quit the game and back up again.");
                String text = "The back-up is written" + partsText(summary.profiles, summary.settings,
                    summary.touchLayout, true) + ".";
                if (summary.skipped == 1)
                    text += " One profile folder could not be read and was left out.";
                else if (summary.skipped > 1)
                    text += " " + summary.skipped + " profile folders could not be read and were left out.";
                finished(text);
            } catch (Exception e) {
                // a half-written back-up is worse than none, but a file this
                // screen never wrote to is not its to delete
                if (opened) {
                    try {
                        DocumentsContract.deleteDocument(getContentResolver(), target);
                    } catch (Exception ignored) {
                    }
                }
                failed(e);
            }
        }, "halo-backup-write").start();
    }

    private void startScan(Uri source) {
        if (working)
            return;
        setBusy(true);
        status.setText("Reading the back-up...");
        new Thread(() -> {
            try {
                BackupArchive.Summary summary;
                try (InputStream in = openArchive(source)) {
                    summary = BackupArchive.scan(in);
                }
                runOnUiThread(() -> {
                    status.setText("");
                    setBusy(false);
                    confirmRestore(source, summary);
                });
            } catch (Exception e) {
                failed(e);
            }
        }, "halo-backup-scan").start();
    }

    private InputStream openArchive(Uri source) throws IOException {
        InputStream in = getContentResolver().openInputStream(source);
        if (in == null)
            throw new IOException("The file could not be opened.");
        return in;
    }

    private void confirmRestore(Uri source, BackupArchive.Summary summary) {
        if (isFinishing() || isDestroyed())
            return;
        boolean[] present = {summary.profiles > 0, summary.settings, summary.touchLayout};
        if (!present[PART_PROFILES] && !present[PART_SETTINGS] && !present[PART_LAYOUT]) {
            finishAction();
            doneDialog("This back-up is empty: it holds no profiles, no settings and no touch layout.");
            return;
        }
        StringBuilder text = new StringBuilder();
        if (summary.profiles > 0) {
            text.append(summary.profiles == 1 ? "It holds this profile:" : "It holds these profiles:");
            for (String name : summary.profileNames)
                text.append('\n').append(name);
        } else {
            text.append("It holds no profiles.");
        }
        if (summary.build > 0 && !summary.made.isEmpty())
            text.append("\n\nMade by build ").append(summary.build).append(" on ").append(summary.made)
                .append('.');
        else if (summary.build > 0)
            text.append("\n\nMade by build ").append(summary.build).append('.');
        else if (!summary.made.isEmpty())
            text.append("\n\nMade on ").append(summary.made).append('.');

        AlertDialog.Builder builder = new AlertDialog.Builder(this);
        Context context = builder.getContext();
        // (a dialog with a message hides its list, so the parts are check
        // boxes under the message)
        LinearLayout content = new LinearLayout(context);
        content.setOrientation(LinearLayout.VERTICAL);
        content.setPadding(dp(24), dp(8), dp(24), 0);
        TextView message = new TextView(context);
        message.setText(text);
        message.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
        message.setPadding(0, 0, 0, dp(8));
        content.addView(message);
        String[] labels = {"Profiles and saved games", "Settings", "Touch layout"};
        for (int part = 0; part < 3; part++) {
            chosen[part] = chosen[part] && present[part];
            if (!present[part])
                continue;
            int index = part;
            CheckBox box = new CheckBox(context);
            box.setText(labels[part]);
            box.setChecked(chosen[part]);
            box.setOnCheckedChangeListener((view, checked) -> chosen[index] = checked);
            content.addView(box);
        }
        ScrollView scroll = new ScrollView(context);
        scroll.addView(content);

        builder
            .setTitle("Restore from this back-up?")
            .setView(scroll)
            .setPositiveButton("Restore", (dialog, which) -> startRestore(source, summary))
            .setNegativeButton("Cancel", (dialog, which) -> finishAction())
            .setOnCancelListener(dialog -> finishAction())
            .show();
    }

    private void startRestore(Uri source, BackupArchive.Summary scanned) {
        if (working)
            return;
        boolean profiles = chosen[PART_PROFILES];
        boolean settings = chosen[PART_SETTINGS];
        boolean touchLayout = chosen[PART_LAYOUT];
        if (!profiles && !settings && !touchLayout) {
            finishAction();
            status.setText("Nothing was chosen, so nothing was restored.");
            return;
        }
        setBusy(true);
        status.setText("Restoring...");
        working = true;
        new Thread(() -> {
            try {
                restoreArchive(source, scanned, profiles, settings, touchLayout);
                String text = "Restored" + partsText(profiles ? scanned.profiles : 0, settings, touchLayout, false)
                    + ".";
                if (profiles)
                    text += " Profiles with the same name here were replaced; the others stay.";
                finished(text);
            } catch (Exception e) {
                failed(e);
            }
        }, "halo-backup-restore").start();
    }

    private void restoreArchive(Uri source, BackupArchive.Summary scanned, boolean profiles, boolean settings,
                                boolean touchLayout) throws IOException {
        if (gameRunning())
            throw new IOException(GAME_RUNNING);
        long free = new StatFs(dataRoot.getPath()).getAvailableBytes();
        if (free < scanned.bytes + SPARE_BYTES)
            throw new IOException("Not enough space: the back-up needs " + ((scanned.bytes + SPARE_BYTES) >> 20)
                + " MB and the phone has " + (free >> 20) + " MB free.");
        // held from before the staging folder is made until the swap is done,
        // so the launcher's sweep (another process) never takes it half made
        try (BackupArchive.Lock lock = BackupArchive.tryLock(dataRoot)) {
            if (lock == null)
                throw new IOException("Another restore is running. Wait for it to finish.");
            File staging = new File(dataRoot, "save/u.restoring");
            try {
                BackupArchive.deleteRecursively(staging);
                if (!staging.mkdirs())
                    throw new IOException("Could not create " + staging.getPath() + ".");
                BackupArchive.Summary extracted;
                try (InputStream in = openArchive(source)) {
                    extracted = BackupArchive.extract(in, staging, profiles, settings, touchLayout);
                }
                String layoutText = touchLayout ? extracted.layoutText : null;
                // checked before anything is moved, so a bad layout file
                // changes nothing
                if (layoutText != null) {
                    try {
                        TouchLayout.importConfiguration(layoutText);
                    } catch (RuntimeException e) {
                        throw new IOException("The touch layout in this back-up cannot be read. "
                            + "Untick Touch layout and restore again.", e);
                    }
                }
                if (gameRunning())
                    throw new IOException(GAME_RUNNING);
                BackupArchive.swapIn(dataRoot, staging);
                if (layoutText != null && !getSharedPreferences(PREFERENCES, MODE_PRIVATE).edit()
                        .putString(KEY_CONFIGURATION, layoutText).commit())
                    throw new IOException(profiles
                        ? "The profiles were restored, but the touch layout could not be saved."
                        : "The touch layout could not be saved.");
            } catch (IOException | RuntimeException e) {
                // removed while the lock is still held, so it is never another
                // restore's staging folder
                BackupArchive.deleteRecursively(staging);
                throw e;
            }
        }
    }

    /** The parts named in a done message, such as ": 3 profiles, the settings and the touch layout". */
    private static String partsText(int profiles, boolean settings, boolean touchLayout, boolean written) {
        List<String> parts = new ArrayList<>();
        if (profiles > 0)
            parts.add(profiles == 1 ? "1 profile" : profiles + " profiles");
        if (settings)
            parts.add("the settings");
        if (touchLayout)
            parts.add("the touch layout");
        if (parts.isEmpty())
            return written ? ", but there was nothing to put in it" : ": nothing";
        StringBuilder text = new StringBuilder(": ");
        for (int i = 0; i < parts.size(); i++) {
            if (i > 0)
                text.append(i == parts.size() - 1 ? " and " : ", ");
            text.append(parts.get(i));
        }
        return text.toString();
    }

    private void doneDialog(String text) {
        if (isFinishing() || isDestroyed())
            return;
        new AlertDialog.Builder(this)
            .setTitle(TITLE)
            .setMessage(text)
            .setPositiveButton("OK", null)
            .show();
    }

    /**
     * Ends a worker's run: its text waits in lastResult for the instance on screen, now or once one resumes;
     * when the screen was closed meanwhile, the process ends.
     */
    private static void finished(String text) {
        MAIN.post(() -> {
            lastResult = text;
            working = false;
            if (resumed != null)
                resumed.showLastResult();
            endProcessWhenIdle();
        });
    }

    private void failed(Exception error) {
        android.util.Log.i("halo", "backup: failed", error);
        String reason = error.getMessage() != null ? error.getMessage() : error.getClass().getSimpleName();
        finished("The operation did not complete: " + reason);
    }

    /** Shows a finished worker's result once, and frees the buttons. */
    private void showLastResult() {
        if (lastResult == null || working || isFinishing() || isDestroyed())
            return;
        String text = lastResult;
        lastResult = null;
        finishAction();
        status.setText("");
        setBusy(dataRoot == null);
        doneDialog(text);
    }
}
