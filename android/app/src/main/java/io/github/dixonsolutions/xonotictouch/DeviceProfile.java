package io.github.dixonsolutions.xonotictouch;

import android.app.ActivityManager;
import android.content.Context;
import android.os.Build;
import android.util.Log;

import java.io.BufferedReader;
import java.io.FileReader;
import java.util.Locale;

/**
 * What the phone is, read from the system before the engine starts: the SoC,
 * how much memory it has, and how fast its fastest cores run. Nothing here
 * waits for the player to do anything.
 *
 * It decides one thing, the performance profile the first start applies
 * (GameData.writeStartupConfig): a phone that cannot hold 60 fps starts on the
 * 30 fps saver instead of stuttering at 60, and one with memory to spare keeps
 * full-size textures. After the first start the player's own choices stand.
 * The facts also go to the engine as _android_* cvars and to logcat, so a
 * report from a player says which device it came from.
 */
final class DeviceProfile {

    private static final String TAG = "XonoticTouch";

    static final int TIER_LOW = 0;
    static final int TIER_MID = 1;
    static final int TIER_HIGH = 2;

    final String soc;
    final String hardware;
    final int ramMb;
    final int maxCpuMhz;
    final int cores;
    final int tier;

    private DeviceProfile(String soc, String hardware, int ramMb, int maxCpuMhz, int cores) {
        this.soc = soc;
        this.hardware = hardware;
        this.ramMb = ramMb;
        this.maxCpuMhz = maxCpuMhz;
        this.cores = cores;
        this.tier = classify(ramMb, maxCpuMhz, cores);
    }

    static DeviceProfile detect(Context context) {
        String soc = "";
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            soc = (Build.SOC_MANUFACTURER + " " + Build.SOC_MODEL).trim();
        }
        String hardware = (Build.HARDWARE + " " + Build.BOARD).trim();

        int ramMb = 0;
        ActivityManager am = (ActivityManager) context.getSystemService(Context.ACTIVITY_SERVICE);
        if (am != null) {
            ActivityManager.MemoryInfo info = new ActivityManager.MemoryInfo();
            am.getMemoryInfo(info);
            ramMb = (int) (info.totalMem >> 20);
        }

        int cores = Runtime.getRuntime().availableProcessors();
        int maxKhz = 0;
        for (int i = 0; i < Math.max(cores, 16); i++) {
            maxKhz = Math.max(maxKhz, readInt("/sys/devices/system/cpu/cpu" + i + "/cpufreq/cpuinfo_max_freq"));
        }

        DeviceProfile profile = new DeviceProfile(soc, hardware, ramMb, maxKhz / 1000, cores);
        Log.i(TAG, "Device: soc=" + (soc.isEmpty() ? "?" : soc) + " hardware=" + hardware
                + " ram=" + ramMb + "MB cpu=" + profile.maxCpuMhz + "MHz x" + cores
                + " exynos=" + profile.isExynos() + " tier=" + profile.tierName());
        return profile;
    }

    /**
     * Three tiers from what reliably separates phones that hold 60 fps in this
     * game from those that do not: memory (texture residency on shared memory)
     * and the fastest core's clock (the game's main thread is one thread). A
     * reading that failed counts as unknown, never as low.
     */
    static int classify(int ramMb, int maxCpuMhz, int cores) {
        boolean lowRam = ramMb > 0 && ramMb < 3500;
        boolean slowCpu = maxCpuMhz > 0 && maxCpuMhz < 1900;
        if (lowRam || slowCpu || (cores > 0 && cores < 4)) {
            return TIER_LOW;
        }
        if (ramMb >= 7500 && maxCpuMhz >= 2800) {
            return TIER_HIGH;
        }
        return TIER_MID;
    }

    /** Samsung Exynos: big.LITTLE clusters with Mali or (2200 on) Xclipse GPUs. */
    boolean isExynos() {
        String all = (soc + " " + hardware).toLowerCase(Locale.ROOT);
        return all.contains("exynos") || all.contains("samsung s5e") || all.matches(".*\\bs5e\\d+.*")
                || all.contains("universal");
    }

    String tierName() {
        return tier == TIER_LOW ? "low" : tier == TIER_HIGH ? "high" : "mid";
    }

    /**
     * What the first start runs: the profile for the tier, plus full-size
     * textures where there is memory for them.
     */
    String firstRunProfileCommands() {
        if (tier == TIER_LOW) {
            return "exec touch/profiles/battery.cfg";
        }
        if (tier == TIER_HIGH) {
            return "exec touch/profiles/balanced.cfg; gl_picmip 0";
        }
        return "exec touch/profiles/balanced.cfg";
    }

    /** Lines for touch/startup.cfg: the facts as read-only-by-convention cvars. */
    String startupCvars() {
        return "set _android_soc \"" + quoteSafe(soc) + "\"\n"
                + "set _android_hardware \"" + quoteSafe(hardware) + "\"\n"
                + "set _android_ram_mb " + ramMb + "\n"
                + "set _android_cpu_mhz " + maxCpuMhz + "\n"
                + "set _android_cores " + cores + "\n"
                + "set _android_exynos " + (isExynos() ? 1 : 0) + "\n"
                + "set _android_tier " + tier + "\n";
    }

    private static String quoteSafe(String s) {
        return s.replace('"', '\'').replace('\n', ' ').replace(';', ',');
    }

    private static int readInt(String path) {
        try (BufferedReader in = new BufferedReader(new FileReader(path))) {
            String line = in.readLine();
            return line == null ? 0 : Integer.parseInt(line.trim());
        } catch (Exception e) {
            // Missing core (offline or absent), or the file is not readable here.
            return 0;
        }
    }
}
