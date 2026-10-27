package dev.picoet.tonguecal;

import android.app.Activity;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Rect;
import android.graphics.RectF;
import android.os.Bundle;
import android.view.MotionEvent;
import android.view.View;

import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Locale;

/**
 * Tongue calibration and personal retraining for the PicoET-Enhance tongue payload. The payload
 * (inside the PICO tracking service) streams the mouth camera, the model's mask, its geometry and
 * the retraining status to 127.0.0.1 after a "TCAM" request on UDP 19031.
 *
 * Calibrate: measures the person's tongue angles (centre/left/right) and a mask-size threshold
 * between their no-tongue mouth and their tongue, saved with "TCFG". Fixes a different face or
 * head-mount reading as always-left.
 *
 * Retrain (advanced): the payload records labelled frames ("TREC"), retrains the model's mask head
 * on the headset ("TTRN") and keeps it only if it tests better on held-out frames; it also sets the
 * calibration for the new head. "Restore standard" ("TSTD") brings back the shipped model and
 * calibration. Steps advance by themselves once a pose has been held.
 */
public class MainActivity extends Activity {
    static final int PORT = 19031, SIDE = 200, GRID = 56, HEADER = 96;
    static final boolean RU = Locale.getDefault().getLanguage().equals("ru");

    static String t(String en, String ru) { return RU ? ru : en; }

    /** One TFR2 datagram. */
    static final class Frame {
        int seq, flags, area, nose, calArea, personal, trainState, recLabel, recCount;
        float rawAngle, position, left, right, out, noseX, noseY, calLeft, calCenter, calRight, progress, oldScore, newScore;
        byte[] pixels = new byte[SIDE * SIDE], mask = new byte[GRID * GRID];
        boolean raw() { return (flags & 2) != 0; }
        boolean active() { return (flags & 4) != 0; }
        boolean defaultCalibration() {
            return Math.abs(calLeft - .33f) < 1e-3f && Math.abs(calCenter - .66f) < 1e-3f && Math.abs(calRight - 1.11f) < 1e-3f && calArea == 100;
        }
    }

    // Screens / steps
    static final int CONNECT = 0, HOME = 1, WARN = 2, FIT = 3, CLOSED = 4, HIDDEN_OPEN = 5, CENTER = 6, LEFT = 7, RIGHT = 8,
            SAVING = 9, TRAINING = 10, TEST = 11, RESULT = 12;
    static final int TRAIN_IDLE = 0, TRAIN_RUNNING = 1, TRAIN_ACCEPTED = 2, TRAIN_REJECTED = 3, TRAIN_FAILED = 4;
    static final int NEG_FRAMES = 30, POSE_FRAMES = 24, SIDE_FRAMES = 40, REC_FRAMES = 60;
    volatile int step = CONNECT;
    boolean retrain;                             // current flow: retraining (true) or calibration
    volatile Frame latest;
    int held;                                    // qualifying frames in the current step
    long stepStarted;
    final ArrayList<Float> window = new ArrayList<>();
    final ArrayList<Integer> negAreas = new ArrayList<>(), poseAreas = new ArrayList<>();
    float angleCenter, angleLeft, angleRight, sideBest;
    int threshold;
    String message = "";
    // Mouth references (50x50 block means): closed for the open-mouth step, open for retraining poses.
    // Recorded sessions: closed-vs-closed differs by 0.4-1.9 (shifted up to ~6), open-vs-closed ~8.6.
    final float[] closedRef = new float[50 * 50], openRef = new float[50 * 50];
    int closedRefFrames, openRefFrames;
    float[] previousBlocks;
    static final float OPEN_CHANGE = 5f, TONGUE_CHANGE = 4f, STILL = 2.5f;
    int sentLabel = -1; long sentAt;
    volatile boolean running;
    DatagramSocket socket;
    CalView view;

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        view = new CalView();
        setContentView(view);
    }

    @Override protected void onResume() {
        super.onResume();
        running = true;
        stepStarted = System.currentTimeMillis();
        new Thread(new Runnable() { public void run() { network(); } }, "tongue-cal-net").start();
    }

    @Override protected void onPause() {
        super.onPause();
        if (step >= CLOSED && step <= RIGHT) setRecording(0);
        running = false;
        if (socket != null) socket.close();
    }

    void send(byte[] data) {
        try { socket.send(new DatagramPacket(data, data.length, InetAddress.getByName("127.0.0.1"), PORT)); }
        catch (Exception ignored) { }
    }

    void command(String c) { send(c.getBytes()); }

    /** Tells the payload which label to record the following frames under (0 = stop). */
    void setRecording(int label) {
        long now = System.currentTimeMillis();
        if (label == sentLabel && now - sentAt < 500) return;
        sentLabel = label; sentAt = now;
        send(new byte[]{'T', 'R', 'E', 'C', (byte) label});
    }

    void network() {
        try {
            socket = new DatagramSocket();
            socket.setSoTimeout(250);
            byte[] buf = new byte[HEADER + SIDE * SIDE + GRID * GRID + 16];
            long lastSub = 0;
            while (running) {
                long now = System.currentTimeMillis();
                if (now - lastSub > 1000) { lastSub = now; command("TCAM"); }
                DatagramPacket p = new DatagramPacket(buf, buf.length);
                try { socket.receive(p); } catch (java.net.SocketTimeoutException e) { tick(null); continue; }
                if (p.getLength() != HEADER + SIDE * SIDE + GRID * GRID || buf[0] != 'T' || buf[1] != 'F' || buf[2] != 'R' || buf[3] != '2') continue;
                Frame f = parse(buf);
                latest = f;
                tick(f);
            }
        } catch (Exception e) {
            message = e.toString();
        }
    }

    static Frame parse(byte[] buf) {
        ByteBuffer h = ByteBuffer.wrap(buf, 0, HEADER).order(ByteOrder.LITTLE_ENDIAN);
        Frame f = new Frame();
        h.position(4);
        f.seq = h.getInt(); f.flags = h.getInt(); f.area = h.getInt();
        f.rawAngle = h.getFloat(); f.position = h.getFloat(); f.left = h.getFloat(); f.right = h.getFloat(); f.out = h.getFloat();
        f.noseX = h.getFloat(); f.noseY = h.getFloat(); f.nose = h.getInt();
        f.calLeft = h.getFloat(); f.calCenter = h.getFloat(); f.calRight = h.getFloat(); f.calArea = h.getInt();
        f.personal = h.getInt(); f.trainState = h.getInt(); f.progress = h.getFloat(); f.oldScore = h.getFloat(); f.newScore = h.getFloat();
        f.recLabel = h.getInt(); f.recCount = h.getInt();
        System.arraycopy(buf, HEADER, f.pixels, 0, SIDE * SIDE);
        System.arraycopy(buf, HEADER + SIDE * SIDE, f.mask, 0, GRID * GRID);
        return f;
    }

    /** Fit: the nose glare (the reference point for direction) must be visible and not flooding. */
    static String fitProblem(Frame f) {
        if (f.nose < 3000) return t("The camera does not see your nose and upper lip. Pull the headset a bit lower and tighter.",
                "Камера не видит нос и верхнюю губу. Опустите шлем чуть ниже и затяните плотнее.");
        if (f.nose > 11000) return t("Too much glare in the mouth camera. Seat the headset so it does not press on your nose.",
                "Слишком много засветки в камере рта. Наденьте шлем так, чтобы он не давил на нос.");
        return null;
    }

    void go(int next) {
        step = next; held = 0; window.clear(); stepStarted = System.currentTimeMillis(); sideBest = angleCenter; previousBlocks = null;
    }

    static float[] blocks(Frame f) {
        float[] b = new float[50 * 50];
        for (int y = 0; y < SIDE; y++) for (int x = 0; x < SIDE; x++) b[(y / 4) * 50 + x / 4] += (f.pixels[y * SIDE + x] & 255) / 16f;
        return b;
    }

    static float difference(float[] a, float[] b) {
        float sum = 0; for (int i = 0; i < a.length; i++) sum += Math.abs(a[i] - b[i]);
        return sum / a.length;
    }

    static int accumulate(float[] ref, int frames, float[] b) {
        for (int i = 0; i < b.length; i++) ref[i] = (ref[i] * frames + b[i]) / (frames + 1);
        return frames + 1;
    }

    static float median(ArrayList<Float> v) {
        float[] a = new float[v.size()]; for (int i = 0; i < a.length; i++) a[i] = v.get(i);
        Arrays.sort(a); return a[a.length / 2];
    }

    static float spread(ArrayList<Float> v) {
        float lo = Float.MAX_VALUE, hi = -Float.MAX_VALUE; for (float x : v) { lo = Math.min(lo, x); hi = Math.max(hi, x); }
        return hi - lo;
    }

    /** Advances the current flow; runs on the network thread for every frame (or timeout). */
    synchronized void tick(Frame f) {
        long now = System.currentTimeMillis();
        if (step == CONNECT) { if (f != null) go(HOME); view.postInvalidate(); return; }
        if (f == null) { view.postInvalidate(); return; }
        switch (step) {
            case FIT:
                held = fitProblem(f) == null ? held + 1 : 0;
                if (held >= 20) go(CLOSED);
                break;
            case CLOSED: {
                if (fitProblem(f) != null || now - stepStarted < 1500) { held = 0; if (retrain) setRecording(0); break; }
                float[] b = blocks(f);
                closedRefFrames = accumulate(closedRef, closedRefFrames, b);
                negAreas.add(f.raw() ? f.area : 0);
                if (retrain) setRecording(1);
                if (++held >= (retrain ? REC_FRAMES : NEG_FRAMES)) { if (retrain) setRecording(0); go(HIDDEN_OPEN); }
                break;
            }
            case HIDDEN_OPEN: {
                float[] b = blocks(f);
                boolean open = fitProblem(f) == null && closedRefFrames > 0 && difference(b, closedRef) >= OPEN_CHANGE;
                if (!open) { held = 0; if (retrain) setRecording(0); break; }   // mouth not open yet
                openRefFrames = accumulate(openRef, openRefFrames, b);
                negAreas.add(f.raw() ? f.area : 0);
                if (retrain) setRecording(2);
                if (++held >= (retrain ? REC_FRAMES : NEG_FRAMES)) { if (retrain) setRecording(0); go(CENTER); }
                break;
            }
            case CENTER: case LEFT: case RIGHT:
                if (retrain) retrainPose(f); else calibrationPose(f);
                break;
            case SAVING:
                if (Math.abs(f.calCenter - angleCenter) < 1e-3f && f.calArea == threshold) go(TEST);
                else if (now - stepStarted > 3000) { message = t("The headset did not confirm the calibration. Is the PicoET-Enhance tongue build installed?",
                        "Шлем не подтвердил калибровку. Установлена сборка PicoET-Enhance с языком?"); go(RESULT); }
                else if ((now - stepStarted) % 500 < 60) sendCalibration();
                break;
            case TRAINING:
                if (now - stepStarted < 1500) break;   // let the payload switch to RUNNING
                if (f.trainState == TRAIN_ACCEPTED) {
                    message = t("Your personal model is installed and calibrated (test score " + score(f.oldScore) + " -> " + score(f.newScore) + ").",
                            "Ваша личная модель установлена и откалибрована (оценка " + score(f.oldScore) + " -> " + score(f.newScore) + ").");
                    go(TEST);
                } else if (f.trainState == TRAIN_REJECTED) {
                    message = t("Training did not improve detection (" + score(f.oldScore) + " -> " + score(f.newScore) + "), so nothing was changed. Your previous model stays.",
                            "Тренировка не улучшила распознавание (" + score(f.oldScore) + " -> " + score(f.newScore) + "), поэтому ничего не изменено. Осталась прежняя модель.");
                    go(RESULT);
                } else if (f.trainState == TRAIN_FAILED || f.trainState == TRAIN_IDLE) {
                    message = t("Not enough usable frames to train. Nothing was changed. Try again with the tongue clearly out.",
                            "Недостаточно подходящих кадров для тренировки. Ничего не изменено. Попробуйте снова, высовывая язык явно.");
                    go(RESULT);
                }
                break;
            default: break;
        }
        view.postInvalidate();
    }

    static String score(float s) { return String.format(Locale.US, "%.2f", s); }

    void calibrationPose(Frame f) {
        if (!f.raw() || f.area < 40) { held = 0; window.clear(); return; }
        if (step != CENTER) {   // must have moved away from centre, to the other side from left
            float d = f.rawAngle - angleCenter;
            if (Math.abs(d) < .1f || (step == RIGHT && d * (angleLeft - angleCenter) > 0)) { held = 0; window.clear(); return; }
        }
        window.add(f.rawAngle);
        if (window.size() > 8) window.remove(0);
        boolean steady = window.size() >= 8 && spread(window) <= .12f;
        // Centre must be held still. On the sides the user keeps pushing further, so movement does
        // not restart the step; only steady stretches update the farthest position.
        if (step == CENTER && window.size() >= 8 && !steady) { held = 0; return; }
        poseAreas.add(f.area);
        if (step != CENTER && steady) {   // farthest steady position reached so far
            float m = median(window);
            if (Math.abs(m - angleCenter) > Math.abs(sideBest - angleCenter)) sideBest = m;
        }
        if (step != CENTER && sideBest == angleCenter) return;   // no steady side position yet
        if (++held >= (step == CENTER ? POSE_FRAMES : SIDE_FRAMES)) {
            float a = step == CENTER ? median(window) : sideBest;
            if (step == CENTER) angleCenter = a; else if (step == LEFT) angleLeft = a; else angleRight = a;
            if (step == RIGHT) finishCalibration(); else go(step + 1);
        }
    }

    /** Retraining cannot rely on the model seeing the tongue: a pose counts while the mouth image
     *  clearly differs from the open-mouth-without-tongue reference and is held still. */
    void retrainPose(Frame f) {
        float[] b = blocks(f);
        boolean changed = openRefFrames > 0 && difference(b, openRef) >= TONGUE_CHANGE;
        boolean still = previousBlocks == null || difference(b, previousBlocks) < STILL;
        previousBlocks = b;
        if (fitProblem(f) != null || !changed || !still || System.currentTimeMillis() - stepStarted < 1500) { held = 0; setRecording(0); return; }
        setRecording(step == CENTER ? 3 : step == LEFT ? 4 : 5);
        if (++held >= REC_FRAMES) {
            setRecording(0);
            if (step == RIGHT) { sentLabel = -1; setRecording(0); command("TTRN"); go(TRAINING); }
            else go(step + 1);
        }
    }

    void finishCalibration() {
        if (Math.abs(angleLeft - angleCenter) < .08f || Math.abs(angleRight - angleCenter) < .08f
                || (angleLeft - angleCenter) * (angleRight - angleCenter) >= 0) {
            message = t("Left and right looked the same to the camera. Try again and push the tongue further to each side.",
                    "Камера не различила лево и право. Попробуйте снова и тяните язык сильнее в каждую сторону.");
            go(RESULT); return;
        }
        // Mask-size threshold between this person's no-tongue mouth (closed, open) and their tongue.
        int[] neg = sorted(negAreas);
        int[] pos = sorted(poseAreas);
        int negHigh = neg.length == 0 ? 0 : neg[Math.min(neg.length - 1, (int) (neg.length * .95))];
        int posLow = pos.length == 0 ? 100 : pos[(int) (pos.length * .05)];
        if (negHigh + 10 < posLow) threshold = Math.max(40, Math.min(300, Math.round(negHigh + (posLow - negHigh) * .4f)));
        else threshold = 100;   // overlap: keep the shipped default
        message = t("Calibration saved.", "Калибровка сохранена.");
        sendCalibration();
        go(SAVING);
    }

    static int[] sorted(ArrayList<Integer> v) {
        int[] a = new int[v.size()]; for (int i = 0; i < a.length; i++) a[i] = v.get(i);
        Arrays.sort(a); return a;
    }

    void sendCalibration() {
        ByteBuffer b = ByteBuffer.allocate(20).order(ByteOrder.LITTLE_ENDIAN);
        b.put(new byte[]{'T', 'C', 'F', 'G'}).putFloat(angleLeft).putFloat(angleCenter).putFloat(angleRight).putFloat(threshold);
        send(b.array());
    }

    synchronized void begin(boolean retraining) {
        retrain = retraining;
        negAreas.clear(); poseAreas.clear(); message = "";
        closedRefFrames = 0; openRefFrames = 0; Arrays.fill(closedRef, 0); Arrays.fill(openRef, 0);
        if (retraining) { command("TCLR"); sentLabel = -1; }
        go(FIT);
    }

    synchronized void home() {
        if (step >= CLOSED && step <= RIGHT && retrain) setRecording(0);
        go(latest == null ? CONNECT : HOME);
    }

    synchronized void restoreStandard() {
        command("TSTD");
        message = t("The standard model and calibration are back.", "Возвращены стандартная модель и калибровка.");
        go(RESULT);
    }

    String status(Frame f) {
        if (f == null) return "";
        String model = f.personal != 0 ? t("personal (retrained)", "личная (переобучена)") : t("standard", "стандартная");
        String cal = f.defaultCalibration() ? t("standard", "стандартная") : t("personal", "личная");
        return t("Model: ", "Модель: ") + model + "    " + t("Calibration: ", "Калибровка: ") + cal;
    }

    String prompt() {
        Frame f = latest;
        switch (step) {
            case CONNECT: return System.currentTimeMillis() - stepStarted > 4000
                    ? t("No camera data. Make sure face tracking is on, the PicoET-Enhance tongue build is installed and the headset is worn.",
                        "Нет данных с камеры. Проверьте, что трекинг лица включён, установлена сборка PicoET-Enhance с языком и шлем надет.")
                    : t("Connecting to the mouth camera...", "Подключаюсь к камере рта...");
            case HOME: return t("Calibrate first: about 20 seconds, fixes a tongue that points the wrong way.\n"
                        + "Retrain only if calibration did not help.",
                    "Сначала калибровка: около 20 секунд, исправляет язык, который смотрит не туда.\n"
                        + "Переобучение — только если калибровка не помогла.");
            case WARN: return t("Retraining teaches the model on your face (about 15 s of poses, then 1-2 minutes on the headset). "
                        + "Use it only if calibration did not help. A poor recording can make tracking worse: the app keeps the new model only "
                        + "if it tests better than the current one, and \"Restore standard\" always brings the original back.",
                    "Переобучение учит модель на вашем лице (около 15 секунд поз, затем 1-2 минуты на шлеме). "
                        + "Используйте его, только если калибровка не помогла. Плохая запись может ухудшить отслеживание: новая модель "
                        + "сохраняется, только если проверка показала улучшение, а «Вернуть стандарт» всегда возвращает исходную.");
            case FIT: { String p = f == null ? null : fitProblem(f);
                return p != null ? p : t("Headset fit looks good. Hold still...", "Шлем сидит хорошо. Не двигайтесь..."); }
            case CLOSED: return t("Close your mouth and relax.", "Закройте рот и расслабьтесь.");
            case HIDDEN_OPEN: return t("Open your mouth, keep the tongue inside.", "Откройте рот, язык внутри.")
                    + (System.currentTimeMillis() - stepStarted > 6000 && held == 0 ? "\n" + t("Open it a bit wider.", "Откройте чуть шире.") : "");
            case CENTER: return hint(t("Stick your tongue straight out and hold it.", "Высуньте язык прямо и держите."));
            case LEFT: return hint(t("Now move your tongue to YOUR left and slightly down, as far as is comfortable, and hold.", "Теперь язык в ВАШУ левую сторону и чуть вниз, насколько удобно, и держите."));
            case RIGHT: return hint(t("Now to YOUR right and slightly down, as far as is comfortable, and hold.", "Теперь в ВАШУ правую сторону и чуть вниз, насколько удобно, и держите."));
            case SAVING: return t("Saving...", "Сохраняю...");
            case TRAINING: return t("Training on the headset... You can relax; keep the headset on.", "Тренировка на шлеме... Можно расслабиться, шлем не снимайте.");
            case TEST: return (message.isEmpty() ? "" : message + "\n") + t("Try it: move your tongue left and right.", "Попробуйте: водите языком влево-вправо.");
            default: return message;
        }
    }

    String hint(String base) {
        if (System.currentTimeMillis() - stepStarted > 8000 && held == 0)
            return base + "\n" + t("I can't see the tongue yet: stick it out further.", "Пока не вижу язык: высуньте его дальше.");
        return base;
    }

    final class CalView extends View {
        final Paint text = new Paint(Paint.ANTI_ALIAS_FLAG), small = new Paint(Paint.ANTI_ALIAS_FLAG), fill = new Paint(Paint.ANTI_ALIAS_FLAG);
        final Bitmap camera = Bitmap.createBitmap(SIDE, SIDE, Bitmap.Config.ARGB_8888);
        final int[] argb = new int[SIDE * SIDE];
        final int[] gamma = new int[256];
        final RectF b1 = new RectF(), b2 = new RectF(), b3 = new RectF();
        int a1, a2, a3;   // actions of the visible buttons
        static final int NONE = 0, CALIBRATE = 1, RETRAIN = 2, RESTORE = 3, CONTINUE = 4, BACK = 5, DONE = 6, REDO = 7, START_OVER = 8;

        CalView() {
            super(MainActivity.this);
            setBackgroundColor(Color.rgb(18, 20, 26));
            text.setColor(Color.WHITE); text.setTextSize(36);
            small.setColor(Color.rgb(170, 176, 190)); small.setTextSize(26);
            for (int i = 0; i < 256; i++) gamma[i] = (int) Math.round(255 * Math.pow(i / 255.0, .55));
        }

        @Override protected void onDraw(Canvas c) {
            int w = getWidth(), h = getHeight();
            float side = Math.min(h - 120, w * .5f);
            float cx = 40, cy = 60;
            Frame f = latest;
            if (f != null) {
                boolean showMask = f.raw() && f.area >= 40;
                for (int y = 0; y < SIDE; y++) for (int x = 0; x < SIDE; x++) {
                    int v = gamma[f.pixels[y * SIDE + x] & 255];
                    int m = showMask ? f.mask[(y * GRID / SIDE) * GRID + x * GRID / SIDE] & 255 : 0;
                    int r = m > 128 ? Math.min(255, v / 2 + 140) : v, g = m > 128 ? v / 2 : v, bl = m > 128 ? v / 2 : v;
                    argb[y * SIDE + x] = Color.rgb(r, g, bl);
                }
                camera.setPixels(argb, 0, SIDE, 0, 0, SIDE, SIDE);
                c.drawBitmap(camera, new Rect(0, 0, SIDE, SIDE), new RectF(cx, cy, cx + side, cy + side), null);
                if (f.nose > 0) { fill.setColor(Color.CYAN); c.drawCircle(cx + f.noseX * side / SIDE, cy + f.noseY * side / SIDE, 6, fill); }
                if (step >= CLOSED && step <= RIGHT && retrain && f.recLabel != 0) {   // recording indicator
                    fill.setColor(Color.rgb(230, 60, 60)); c.drawCircle(cx + side - 24, cy + 24, 12, fill);
                }
            } else {
                fill.setColor(Color.rgb(40, 44, 54)); c.drawRect(cx, cy, cx + side, cy + side, fill);
            }
            float tx = cx + side + 50, tw = w - tx - 40;
            c.drawText(retrain && step != HOME ? t("Tongue retraining", "Переобучение языка") : t("Tongue calibration", "Калибровка языка"), tx, cy + 30, small);
            c.drawText(status(f), tx, cy + 64, small);
            drawWrapped(c, prompt(), tx, cy + 130, tw, text);
            if (step >= CLOSED && step <= RIGHT) drawProgress(c, tx, cy + side * .58f);
            if (step == TRAINING && f != null) drawBar(c, f.trainState == TRAIN_RUNNING ? f.progress : 0, tx, cy + side * .6f, tw);
            if (step == TEST && f != null) drawMeter(c, f, tx, cy + side * .58f, tw);
            float by = cy + side - 70;
            a1 = a2 = a3 = NONE; b1.setEmpty(); b2.setEmpty(); b3.setEmpty();
            switch (step) {
                case HOME:
                    place(c, b1, tx, by, 300, CALIBRATE, t("Calibrate", "Калибровка"), true);
                    place(c, b2, tx + 320, by, 330, RETRAIN, t("Retrain (advanced)", "Переобучение"), false);
                    if (f != null && (f.personal != 0 || !f.defaultCalibration()))
                        place(c, b3, tx, by - 90, 330, RESTORE, t("Restore standard", "Вернуть стандарт"), false);
                    break;
                case WARN:
                    place(c, b1, tx, by, 260, CONTINUE, t("Continue", "Продолжить"), true);
                    place(c, b2, tx + 280, by, 220, BACK, t("Back", "Назад"), false);
                    break;
                case TEST: case RESULT:
                    place(c, b1, tx, by, 260, DONE, t("Done", "Готово"), true);
                    place(c, b2, tx + 280, by, 220, BACK, t("Menu", "Меню"), false);
                    break;
                case TRAINING: case SAVING: case CONNECT:
                    break;
                default:
                    place(c, b1, tx, by, 260, BACK, t("Cancel", "Отмена"), false);
            }
        }

        void place(Canvas c, RectF r, float x, float y, float width, int action, String label, boolean primary) {
            r.set(x, y, x + width, y + 70);
            if (r == b1) a1 = action; else if (r == b2) a2 = action; else a3 = action;
            fill.setColor(primary ? Color.rgb(46, 140, 96) : Color.rgb(52, 60, 78)); c.drawRoundRect(r, 18, 18, fill);
            float lw = small.measureText(label);
            c.drawText(label, r.centerX() - lw / 2, r.centerY() + 9, small);
        }

        void drawProgress(Canvas c, float x, float y) {
            int total = (step == CLOSED || step == HIDDEN_OPEN) ? (retrain ? REC_FRAMES : NEG_FRAMES)
                    : retrain ? REC_FRAMES : step == CENTER ? POSE_FRAMES : SIDE_FRAMES;
            float frac = Math.min(1f, held / (float) total);
            fill.setStyle(Paint.Style.STROKE); fill.setStrokeWidth(14);
            fill.setColor(Color.rgb(50, 56, 70)); c.drawCircle(x + 70, y + 70, 60, fill);
            fill.setColor(Color.rgb(80, 220, 140)); c.drawArc(new RectF(x + 10, y + 10, x + 130, y + 130), -90, 360 * frac, false, fill);
            fill.setStyle(Paint.Style.FILL);
            c.drawText((step - CLOSED + 1) + " / 5", x + 160, y + 85, text);
        }

        void drawBar(Canvas c, float frac, float x, float y, float width) {
            fill.setColor(Color.rgb(50, 56, 70)); c.drawRoundRect(new RectF(x, y, x + width, y + 40), 20, 20, fill);
            fill.setColor(Color.rgb(80, 220, 140)); c.drawRoundRect(new RectF(x, y, x + Math.max(40, width * frac), y + 40), 20, 20, fill);
            c.drawText(Math.round(frac * 100) + "%", x, y + 85, small);
        }

        void drawMeter(Canvas c, Frame f, float x, float y, float width) {
            fill.setColor(Color.rgb(50, 56, 70)); c.drawRoundRect(new RectF(x, y, x + width, y + 50), 25, 25, fill);
            float mid = x + width / 2;
            fill.setColor(Color.rgb(80, 220, 140));
            if (f.left > .02f) c.drawRoundRect(new RectF(mid - width / 2 * f.left, y, mid, y + 50), 25, 25, fill);
            if (f.right > .02f) c.drawRoundRect(new RectF(mid, y, mid + width / 2 * f.right, y + 50), 25, 25, fill);
            fill.setColor(Color.WHITE); c.drawRect(mid - 2, y - 8, mid + 2, y + 58, fill);
            c.drawText(t("left", "лево"), x, y + 95, small);
            c.drawText(t("right", "право"), x + width - 70, y + 95, small);
            c.drawText(f.active() ? t("tongue: out", "язык: высунут") : t("tongue: in", "язык: внутри"), mid - 90, y + 95, small);
        }

        void drawWrapped(Canvas c, String s, float x, float y, float width, Paint p) {
            for (String para : s.split("\n")) {
                StringBuilder line = new StringBuilder();
                for (String word : para.split(" ")) {
                    String trial = line.length() == 0 ? word : line + " " + word;
                    if (p.measureText(trial) > width && line.length() > 0) { c.drawText(line.toString(), x, y, p); y += p.getTextSize() * 1.3f; line = new StringBuilder(word); }
                    else line = new StringBuilder(trial);
                }
                c.drawText(line.toString(), x, y, p); y += p.getTextSize() * 1.5f;
            }
        }

        @Override public boolean onTouchEvent(MotionEvent e) {
            if (e.getAction() != MotionEvent.ACTION_UP) return true;
            float x = e.getX(), y = e.getY();
            int action = b1.contains(x, y) ? a1 : b2.contains(x, y) ? a2 : b3.contains(x, y) ? a3 : NONE;
            switch (action) {
                case CALIBRATE: begin(false); break;
                case RETRAIN: go(WARN); break;
                case CONTINUE: begin(true); break;
                case RESTORE: restoreStandard(); break;
                case BACK: home(); break;
                case DONE: exitApp(); break;
                default: break;
            }
            invalidate();
            return true;
        }
    }

    void exitApp() { running = false; finish(); }
}
