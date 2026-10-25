package com.halo.decomp;

import java.io.IOException;
import java.io.StringReader;
import java.io.StringWriter;
import java.util.ArrayList;
import java.util.Properties;

/**
 * The touch controls' layout (TouchControls): each control's kind, place,
 * size and whether it shows, with the settings that go with them, in the
 * overlay's logical units. Places are kept for the display the layout is
 * on and saved on a 960 x 540 grid, so a layout moves between displays of
 * other shapes. A layout file is Java properties text (version 3; 1 and 2
 * are still read).
 */
final class TouchLayout {
    /** the kinds of control: a button of TouchControls's list, by index */
    static final int LEFT = 16;
    static final int FIRE_LEFT = 17;
    static final int BASE_COUNT = 18;
    static final int MAX_CONTROLS = 64;
    static final float MIN_SIZE = 0.5f;
    static final float MAX_SIZE = 2f;
    static final float MIN_OPACITY = 0.2f;
    static final float MAX_OPACITY = 1f;
    static final float MIN_SENSITIVITY = 0.25f;
    static final float MAX_SENSITIVITY = 4f;
    /**
     * the largest minimum radius: a finger's width is capped here on a small,
     * dense display, where the D-pad's buttons (69 apart) would overlap
     */
    static final float MAX_MINIMUM_RADIUS = 34;

    /** the saved grid */
    private static final float GRID_WIDTH = 960;
    private static final float GRID_HEIGHT = 540;
    private static final int VERSION = 3;
    /**
     * on a display taller than the grid (a folding phone's inner screen, a
     * tablet) the top row (the grid's lines down to TOP_ROW) keeps its
     * distance from the top and the rest (from LOWER_PART down) its distance
     * from the bottom, so the controls under the thumbs sit as low as on a
     * 16:9 phone; the lines between take the extra height
     */
    static final float TOP_ROW = 72;
    static final float LOWER_PART = 90;
    private static final int MAX_FILE_LENGTH = 65536;

    /**
     * each kind's default place on the grid, and its radius, by how often it
     * is used: fire the largest, where the right thumb rests; jump and the
     * grenade next; reload, swap and melee smaller; the seldom used ones
     * (the stick clicks, white and black: the flashlight and the grenade's
     * type, back and the D-pad) smallest, the last together in the upper
     * left, away from the thumbs. The second fire (for a claw grip) is hidden
     * until it is added.
     */
    private static final float[][] DEFAULTS = {
        {875, 455}, {760, 465}, {742, 375}, {760, 228}, {862, 310}, {888, 150},
        {236, 465}, {650, 465}, {220, 36}, {300, 36}, {570, 36}, {390, 36},
        {100, 101}, {100, 199}, {51, 150}, {149, 150}, {115, 440}, {255, 239}
    };
    private static final float[] RADII = {
        40, 30, 30, 30, 55, 40, 26, 26, 24, 24, 28, 24, 22, 22, 22, 22, 64, 45
    };
    /** the kinds hidden by default: the second fire */
    private static boolean hiddenByDefault(int type) {
        return type == FIRE_LEFT;
    }

    private static final class Control {
        final int type;
        /** where it is on the display, in logical units: its place, kept inside */
        float x;
        float y;
        /**
         * where the player put it, on the GRID_WIDTH x GRID_HEIGHT grid: a
         * display that pushes it inward (a shorter one, a larger finger)
         * leaves this alone, so it comes back when the display does
         */
        float placeX;
        float placeY;
        boolean shown = true;
        float size = 1f;

        Control(int type, float x, float y) {
            this.type = type;
            this.x = x;
            this.y = y;
            placeX = x;
            placeY = y;
        }
    }

    /** a layout and the sensitivity saved with it */
    static final class Configuration {
        final TouchLayout layout;
        final float sensitivity;

        Configuration(TouchLayout layout, float sensitivity) {
            this.layout = layout;
            this.sensitivity = sensitivity;
        }
    }

    private final ArrayList<Control> controls = new ArrayList<>();
    private float width = GRID_WIDTH;
    private float height = GRID_HEIGHT;
    /** the smallest radius a control is drawn and touched at, in logical units */
    private float minimumRadius;

    boolean rumbleEnabled = true;
    boolean gyroscopeEnabled = false;
    /** how solid the controls are drawn, MIN_OPACITY..MAX_OPACITY */
    float opacity = 1f;
    /** the move stick goes where the thumb lands in the lower left, not at its place */
    boolean floatingStick = false;

    TouchLayout() {
        resetDefaults();
    }

    /** places the layout on a display of this size, in logical units */
    void bounds(float width, float height) {
        if (!Float.isFinite(width) || !Float.isFinite(height) || width <= 0 || height <= 0)
            throw new IllegalArgumentException("Invalid display dimensions");
        this.width = width;
        this.height = height;
        keepAllInside();
    }

    /**
     * the smallest radius, in logical units, at which a control is drawn and
     * touched (a finger's width, whatever the display's density), at most
     * MAX_MINIMUM_RADIUS
     */
    void setMinimumRadius(float radius) {
        minimumRadius = Float.isFinite(radius) && radius > 0 ? Math.min(radius, MAX_MINIMUM_RADIUS) : 0;
        keepAllInside();
    }

    int size() {
        return controls.size();
    }

    int type(int control) {
        return controls.get(control).type;
    }

    float x(int control) {
        return controls.get(control).x;
    }

    float y(int control) {
        return controls.get(control).y;
    }

    float savedX(int control) {
        return controls.get(control).placeX;
    }

    float savedY(int control) {
        return controls.get(control).placeY;
    }

    float radius(int control) {
        return Math.max(RADII[type(control)] * sizeScale(control), minimumRadius);
    }

    float sizeScale(int control) {
        return controls.get(control).size;
    }

    void setSize(int control, float size) {
        if (!Float.isFinite(size) || size < MIN_SIZE || size > MAX_SIZE)
            throw new IllegalArgumentException("Button size must be between 50% and 200%");
        controls.get(control).size = size;
        move(control, x(control), y(control));
    }

    boolean shown(int control) {
        return controls.get(control).shown;
    }

    void setShown(int control, boolean shown) {
        controls.get(control).shown = shown;
    }

    void setOpacity(float opacity) {
        if (!validOpacity(opacity))
            throw new IllegalArgumentException("Opacity must be between 20% and 100%");
        this.opacity = opacity;
    }

    /** the default controls, at their default places, sizes and visibility */
    void resetDefaults() {
        controls.clear();
        for (int i = 0; i < DEFAULTS.length; i++) {
            Control control = new Control(i, DEFAULTS[i][0], DEFAULTS[i][1]);
            control.shown = !hiddenByDefault(i);
            controls.add(control);
        }
        keepAllInside();
    }

    /** adds a copy of a control beside it; -1 if there may be no more (or it is the stick) */
    int duplicate(int source) {
        if (size() >= MAX_CONTROLS || type(source) == LEFT)
            return -1;
        float offset = radius(source) * 2 + 12;
        Control added = new Control(type(source), x(source) + offset, y(source));
        added.size = sizeScale(source);
        controls.add(added);
        int index = size() - 1;
        // where there is no room to the right, the copy goes to the left
        if (added.x > width - radius(index))
            added.x = x(source) - offset;
        move(index, added.x, added.y);
        return index;
    }

    /** shows a hidden control of this kind, or else adds a copy; -1 if neither can be */
    int add(int type) {
        if (type < 0 || type >= BASE_COUNT)
            return -1;
        for (int i = 0; i < size(); i++) {
            if (type(i) == type && !shown(i)) {
                setShown(i, true);
                return i;
            }
        }
        return duplicate(type);
    }

    /** moves a control, keeping all of it on the display; where it lands is its place */
    void move(int control, float x, float y) {
        if (!Float.isFinite(x) || !Float.isFinite(y))
            return;
        Control moved = controls.get(control);
        float radius = radius(control);
        moved.x = Math.max(radius, Math.min(width - radius, x));
        moved.y = Math.max(radius, Math.min(height - radius, y));
        moved.placeX = moved.x * GRID_WIDTH / width;
        moved.placeY = gridY(moved.y, height);
    }

    /** puts a control at a saved place on the grid, if it is a valid one */
    void restore(int control, float x, float y) {
        if (Float.isFinite(x) && Float.isFinite(y) && x >= 0 && x <= GRID_WIDTH && y >= 0 && y <= GRID_HEIGHT) {
            Control restored = controls.get(control);
            restored.placeX = x;
            restored.placeY = y;
            restored.x = x * width / GRID_WIDTH;
            restored.y = displayY(y, height);
        }
    }

    /**
     * puts every control at its place on this display, pushed inward as far
     * as it must be to stay on it; the places stay as they are
     */
    private void keepAllInside() {
        for (int i = 0; i < size(); i++) {
            Control control = controls.get(i);
            float radius = radius(i);
            control.x = Math.max(radius, Math.min(width - radius, control.placeX * width / GRID_WIDTH));
            control.y = Math.max(radius, Math.min(height - radius, displayY(control.placeY, height)));
        }
    }

    /** a line of the grid on a display this high, in logical units (TOP_ROW, LOWER_PART) */
    static float displayY(float placeY, float height) {
        float extra = height - GRID_HEIGHT;
        if (!(extra > 0))
            return placeY * height / GRID_HEIGHT;
        if (placeY <= TOP_ROW)
            return placeY;
        if (placeY >= LOWER_PART)
            return placeY + extra;
        return TOP_ROW + (placeY - TOP_ROW) * (LOWER_PART - TOP_ROW + extra) / (LOWER_PART - TOP_ROW);
    }

    /** the grid's line at this height on a display this high: displayY's inverse */
    static float gridY(float y, float height) {
        float extra = height - GRID_HEIGHT;
        if (!(extra > 0))
            return y * GRID_HEIGHT / height;
        if (y <= TOP_ROW)
            return y;
        if (y >= LOWER_PART + extra)
            return y - extra;
        return TOP_ROW + (y - TOP_ROW) * (LOWER_PART - TOP_ROW) / (LOWER_PART - TOP_ROW + extra);
    }

    /** the layout as a layout file's text, with the look sensitivity */
    String exportConfiguration(float sensitivity) {
        if (!validSensitivity(sensitivity))
            throw new IllegalArgumentException("Invalid sensitivity");
        Properties values = new Properties();
        values.setProperty("format", "halo-touch-layout");
        values.setProperty("version", Integer.toString(VERSION));
        values.setProperty("rumble", Boolean.toString(rumbleEnabled));
        values.setProperty("gyroscope", Boolean.toString(gyroscopeEnabled));
        values.setProperty("opacity", Float.toString(opacity));
        values.setProperty("floating-stick", Boolean.toString(floatingStick));
        values.setProperty("count", Integer.toString(size()));
        values.setProperty("sensitivity", Float.toString(sensitivity));
        for (int i = 0; i < size(); i++) {
            String key = "control." + i + ".";
            values.setProperty(key + "type", Integer.toString(type(i)));
            values.setProperty(key + "x", Float.toString(savedX(i)));
            values.setProperty(key + "y", Float.toString(savedY(i)));
            values.setProperty(key + "visible", Boolean.toString(shown(i)));
            values.setProperty(key + "size", Float.toString(sizeScale(i)));
        }
        StringWriter text = new StringWriter();
        try {
            values.store(text, "Halo Android touch layout");
        } catch (IOException e) {
            throw new IllegalStateException(e);
        }
        return text.toString();
    }

    /** reads a layout file's text; throws IllegalArgumentException for anything not valid */
    static Configuration importConfiguration(String text) {
        if (text == null || text.length() > MAX_FILE_LENGTH)
            throw new IllegalArgumentException("Invalid layout file");
        Properties values = new Properties();
        try {
            values.load(new StringReader(text));
            int version = readVersion(values);
            int count = Integer.parseInt(values.getProperty("count"));
            float sensitivity = Float.parseFloat(values.getProperty("sensitivity"));
            if (count < BASE_COUNT || count > MAX_CONTROLS || !validSensitivity(sensitivity))
                throw new IllegalArgumentException("Invalid layout settings");
            TouchLayout layout = new TouchLayout();
            layout.controls.clear();
            if (version >= 2) {
                layout.rumbleEnabled = readBoolean(values, "rumble");
                layout.gyroscopeEnabled = readBoolean(values, "gyroscope");
            }
            if (version >= 3) {
                float opacity = Float.parseFloat(values.getProperty("opacity"));
                if (!validOpacity(opacity))
                    throw new IllegalArgumentException("Invalid opacity");
                layout.opacity = opacity;
                layout.floatingStick = readBoolean(values, "floating-stick");
            }
            for (int i = 0; i < count; i++)
                layout.controls.add(readControl(values, i, version));
            return new Configuration(layout, sensitivity);
        } catch (IOException | NullPointerException e) {
            throw new IllegalArgumentException("Incomplete layout file", e);
        }
    }

    private static int readVersion(Properties values) {
        if (!"halo-touch-layout".equals(values.getProperty("format")))
            throw new IllegalArgumentException("Unsupported layout file");
        String version = values.getProperty("version");
        for (int known = 1; known <= VERSION; known++) {
            if (Integer.toString(known).equals(version))
                return known;
        }
        throw new IllegalArgumentException("Unsupported layout file");
    }

    private static Control readControl(Properties values, int index, int version) {
        String key = "control." + index + ".";
        int type = Integer.parseInt(values.getProperty(key + "type"));
        float x = Float.parseFloat(values.getProperty(key + "x"));
        float y = Float.parseFloat(values.getProperty(key + "y"));
        String shown = values.getProperty(key + "visible");
        float size = version >= 2 ? Float.parseFloat(values.getProperty(key + "size")) : 1f;
        boolean validType = type >= 0 && type < BASE_COUNT && (index >= BASE_COUNT ? type != LEFT : type == index);
        boolean validPlace = Float.isFinite(x) && Float.isFinite(y) && x >= 0 && x <= GRID_WIDTH
            && y >= 0 && y <= GRID_HEIGHT;
        boolean validSize = Float.isFinite(size) && size >= MIN_SIZE && size <= MAX_SIZE;
        if (!validType || !validPlace || !validSize || !("true".equals(shown) || "false".equals(shown)))
            throw new IllegalArgumentException("Invalid control in layout file");
        Control control = new Control(type, x, y);
        control.shown = Boolean.parseBoolean(shown);
        control.size = size;
        return control;
    }

    private static boolean readBoolean(Properties values, String key) {
        String value = values.getProperty(key);
        if (!("true".equals(value) || "false".equals(value)))
            throw new IllegalArgumentException("Invalid " + key + " option");
        return Boolean.parseBoolean(value);
    }

    static boolean validSensitivity(float value) {
        return Float.isFinite(value) && value >= MIN_SENSITIVITY && value <= MAX_SENSITIVITY;
    }

    static boolean validOpacity(float value) {
        return Float.isFinite(value) && value >= MIN_OPACITY && value <= MAX_OPACITY;
    }
}
