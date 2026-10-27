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
 * other shapes. A portrait layout (a display taller than it is wide, the
 * game's picture near its top) is one of its own, on a 540 x 960 grid: a
 * strip along the top with back and pause, the picture under it, and the
 * controls in the space under the picture. A layout file is Java
 * properties text (version 3; 1 and 2 are still read).
 */
final class TouchLayout {
    /** the kinds of control: a button of TouchControls's list, by index */
    static final int WHITE = 8;
    static final int BLACK = 9;
    static final int PAUSE = 10;
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
    /** a portrait layout's file's version, and only its: one the builds before portrait (and iOS's, at 3) refuse,
     * rather than read it as a landscape layout */
    private static final int PORTRAIT_VERSION = 4;
    /**
     * on a display taller than the grid (a folding phone's inner screen, a
     * tablet) the top row (the grid's lines down to TOP_ROW) keeps its
     * distance from the top and the rest (from LOWER_PART down) its distance
     * from the bottom, so the controls under the thumbs sit as low as on a
     * 16:9 phone; the lines between take the extra height
     */
    static final float TOP_ROW = 72;
    static final float LOWER_PART = 90;
    /**
     * but white and black (the flashlight and the grenade's type), used in
     * play, come down with the rest: their lines down to WHITE_BLACK_ROW
     * share the extra height and the ones below it keep their distance from
     * the bottom; and they move across to TALL_PLACES, beside the D-pad at
     * its right, one above the other (as in portrait), by the left thumb,
     * fully once the display is TALL_EXTRA taller than 16:9 (the iOS layout
     * has them there on every display), with the second fire (hidden until
     * added) moved out of their way; back and pause, the menus' buttons,
     * stay at the top
     */
    static final float WHITE_BLACK_ROW = 36;
    /** the kinds that move on a display taller than 16:9, and where to: white and black's lines measured as
     * WHITE_BLACK_ROW's is, the second fire's as the rest; apart by a finger's width (MAX_MINIMUM_RADIUS) twice
     * over, with room for a caption */
    private static final int[] TALL_KINDS = {WHITE, BLACK, FIRE_LEFT};
    private static final float[][] TALL_PLACES = {{220, 108}, {220, 192}, {320, 239}};
    static final float TALL_EXTRA = 60;
    /** the portrait grid, and its placement: the strip along the top (its
     * lines down to PORTRAIT_STRIP) keeps its distance from the top, under
     * the display's top inset (a camera's cutout), the rest (from
     * PORTRAIT_LOWER_PART down) its distance from the bottom of a taller
     * display, and the columns from the nearer side of a wider one (the
     * ones between LEFT_PART and RIGHT_PART take the extra width) */
    private static final float PORTRAIT_GRID_WIDTH = 540;
    private static final float PORTRAIT_GRID_HEIGHT = 960;
    static final float PORTRAIT_STRIP = 72;
    private static final float PORTRAIT_LOWER_PART = 90;
    /** the space between the picture and the default controls under it, at the least */
    private static final float PICTURE_MARGIN = 10;
    /** portrait, the picture behind the strip: pause and the toolbar's hide this far either side of the strip's
     * middle, clear of the HUD's top corners (the ammunition at the left, the shields at the right) */
    static final float GATHERED_APART = 40;
    static final float LEFT_PART = 240;
    static final float RIGHT_PART = 300;
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
    /**
     * the portrait defaults, under a 4:3 picture across the grid's width
     * (405 high) under the strip: the stick at the lower left, the right
     * thumb's cluster at the lower right, crouch and zoom between them at
     * the bottom, the D-pad over the stick with white and black (the
     * flashlight and the grenade's type) beside it, and back and pause, the
     * menus' buttons, in the strip at the top with the toolbar
     */
    private static final float[][] PORTRAIT_DEFAULTS = {
        {470, 870}, {382, 890}, {370, 760}, {375, 638}, {460, 728}, {475, 588},
        {230, 898}, {305, 898}, {215, 560}, {215, 640}, {112, 36}, {44, 36},
        {95, 551}, {95, 649}, {46, 600}, {144, 600}, {125, 800}, {250, 700}
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
    /** a portrait layout (its grid PORTRAIT_GRID_WIDTH x PORTRAIT_GRID_HEIGHT) */
    final boolean portrait;
    private float width;
    private float height;
    /** the smallest radius a control is drawn and touched at, in logical units */
    private float minimumRadius;
    /** portrait: the display's top inset (a camera's cutout), in logical units, which the strip stays under */
    private float topInset;
    /** portrait: the picture is behind the strip, pause in its middle (setPictureTop) */
    private boolean gathered;

    boolean rumbleEnabled = true;
    boolean gyroscopeEnabled = false;
    /** how solid the controls are drawn, MIN_OPACITY..MAX_OPACITY */
    float opacity = 1f;
    /** the move stick goes where the thumb lands in the lower left, not at its place */
    boolean floatingStick = false;

    TouchLayout() {
        this(false);
    }

    TouchLayout(boolean portrait) {
        this.portrait = portrait;
        width = gridWidth();
        height = gridHeight();
        resetDefaults();
    }

    /** the grid the places are on, and saved on */
    float gridWidth() {
        return portrait ? PORTRAIT_GRID_WIDTH : GRID_WIDTH;
    }

    float gridHeight() {
        return portrait ? PORTRAIT_GRID_HEIGHT : GRID_HEIGHT;
    }

    /** takes the settings saved with every layout (vibration, gyroscope, opacity, the floating stick) */
    void takeSettings(TouchLayout other) {
        rumbleEnabled = other.rumbleEnabled;
        gyroscopeEnabled = other.gyroscopeEnabled;
        opacity = other.opacity;
        floatingStick = other.floatingStick;
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

    /** portrait: the display's top inset (a camera's cutout), in logical units, which the strip stays under */
    void setTopInset(float inset) {
        topInset = Float.isFinite(inset) && inset > 0 ? Math.min(inset, height / 4) : 0;
        keepAllInside();
    }

    /** the bottom of the portrait strip, in logical units: the picture's highest top */
    float stripBottom() {
        return topInset + PORTRAIT_STRIP;
    }

    /** portrait: where the picture's top is, in logical units; above the strip's bottom, pause moves to the strip's middle */
    void setPictureTop(float top) {
        gathered = portrait && top < stripBottom() - 0.5f;
        keepAllInside();
    }

    /** portrait: whether the picture is behind the strip, pause and the toolbar's hide in its middle */
    boolean stripGathered() {
        return gathered;
    }

    /** the height of the game's picture across a portrait display this wide: 480 lines of 640 columns (4:3) or 852 (16:9), as the game makes it */
    static float pictureHeight(float width, boolean wide) {
        return width * 480 / (wide ? 852 : 640);
    }

    /**
     * portrait: where the game's picture's top goes, in logical units, for a
     * picture this high across the display: half way down the space between
     * the strip and the default controls (not the ones the player moved, so
     * the picture stays put as they move); with no room there, up as far as
     * it must go to clear the controls, at most to the top inset (the
     * strip's buttons then over its upper edge), the controls over its lower
     * part only when even that is not enough
     */
    float pictureTop(float pictureHeight) {
        float controlsTop = height;
        for (int i = 0; i < PORTRAIT_DEFAULTS.length; i++) {
            if (PORTRAIT_DEFAULTS[i][1] > PORTRAIT_STRIP && !hiddenByDefault(i))
                controlsTop = Math.min(controlsTop, displayY(i, PORTRAIT_DEFAULTS[i][1]) - Math.max(RADII[i], minimumRadius));
        }
        float room = controlsTop - PICTURE_MARGIN - stripBottom() - pictureHeight;
        if (room >= 0)
            return stripBottom() + room / 2;
        return Math.max(topInset, controlsTop - PICTURE_MARGIN - pictureHeight);
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
        // (pushed inward if it must be, its place kept: made smaller again, it goes back)
        keepAllInside();
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
        float[][] defaults = portrait ? PORTRAIT_DEFAULTS : DEFAULTS;
        for (int i = 0; i < defaults.length; i++) {
            Control control = new Control(i, defaults[i][0], defaults[i][1]);
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
        moved.placeX = gridX(moved.type, moved.x);
        moved.placeY = gridY(moved.type, moved.y);
    }

    /** puts a control at a saved place on the grid, if it is a valid one */
    void restore(int control, float x, float y) {
        if (Float.isFinite(x) && Float.isFinite(y) && x >= 0 && x <= gridWidth() && y >= 0 && y <= gridHeight()) {
            Control restored = controls.get(control);
            restored.placeX = x;
            restored.placeY = y;
            restored.x = displayX(restored.type, x);
            restored.y = displayY(restored.type, y);
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
            control.x = Math.max(radius, Math.min(width - radius, displayX(control.type, control.placeX)));
            control.y = Math.max(radius, Math.min(height - radius, displayY(control.type, control.placeY)));
        }
    }

    /** a place on this layout's grid on the display, each way, and back */
    private float displayX(int type, float placeX) {
        return portrait ? edgeKept(placeX, width, PORTRAIT_GRID_WIDTH, LEFT_PART, RIGHT_PART) + gatherShift(type)
            : placeX * width / GRID_WIDTH + tallShift(type, placeX, height, 0);
    }

    private float displayY(int type, float placeY) {
        return portrait ? topInset + edgeKept(placeY, height - topInset, PORTRAIT_GRID_HEIGHT, PORTRAIT_STRIP,
            PORTRAIT_LOWER_PART) : displayY(type, placeY, height);
    }

    private float gridX(int type, float x) {
        return portrait ? edgeKeptBack(x - gatherShift(type), width, PORTRAIT_GRID_WIDTH, LEFT_PART, RIGHT_PART)
            : tallShifted(type, height) ? tallLineBack(type, x, width, height, 0)
            : Math.max(0, Math.min(GRID_WIDTH, x * GRID_WIDTH / width));
    }

    private float gridY(int type, float y) {
        return portrait ? Math.max(0, edgeKeptBack(y - topInset, height - topInset, PORTRAIT_GRID_HEIGHT,
            PORTRAIT_STRIP, PORTRAIT_LOWER_PART)) : gridY(type, y, height);
    }

    /**
     * a line of a grid this long on a display this long: on a longer one the
     * lines to near keep their distance from its start and those from far
     * from its end, and the ones between take the extra length; on a shorter
     * one, its share
     */
    static float edgeKept(float line, float length, float grid, float near, float far) {
        float extra = length - grid;
        if (!(extra > 0))
            return line * length / grid;
        if (line <= near)
            return line;
        if (line >= far)
            return line + extra;
        return near + (line - near) * (far - near + extra) / (far - near);
    }

    /** edgeKept's inverse */
    static float edgeKeptBack(float at, float length, float grid, float near, float far) {
        float extra = length - grid;
        if (!(extra > 0))
            return at * grid / length;
        if (at <= near)
            return at;
        if (at >= far + extra)
            return at - extra;
        return near + (at - near) * (far - near) / (far - near + extra);
    }

    /** a line of the landscape grid on a display this high, in logical units (TOP_ROW, LOWER_PART) */
    static float displayY(float placeY, float height) {
        return edgeKept(placeY, height, GRID_HEIGHT, TOP_ROW, LOWER_PART);
    }

    /** the landscape grid's line at this height on a display this high: displayY's inverse */
    static float gridY(float y, float height) {
        return edgeKeptBack(y, height, GRID_HEIGHT, TOP_ROW, LOWER_PART);
    }

    /** a kind of control's line of the landscape grid on a display this high (white and black: WHITE_BLACK_ROW; TALL_PLACES) */
    static float displayY(int type, float placeY, float height) {
        return (type == WHITE || type == BLACK ? edgeKept(placeY, height, GRID_HEIGHT, 0, WHITE_BLACK_ROW)
            : displayY(placeY, height)) + tallShift(type, placeY, height, 1);
    }

    /** the kind's line at this height: displayY's inverse */
    static float gridY(int type, float y, float height) {
        if (tallShifted(type, height))
            return tallLineBack(type, y, 0, height, 1);
        return Math.max(0, type == WHITE || type == BLACK ? edgeKeptBack(y, height, GRID_HEIGHT, 0, WHITE_BLACK_ROW)
            : gridY(y, height));
    }

    /** portrait: how far pause moves across, from its place, to the strip's middle with the picture behind the strip; else 0 */
    private float gatherShift(int type) {
        if (!gathered || type != PAUSE)
            return 0;
        return width / 2 - GATHERED_APART
            - edgeKept(PORTRAIT_DEFAULTS[PAUSE][0], width, PORTRAIT_GRID_WIDTH, LEFT_PART, RIGHT_PART);
    }

    /**
     * how far a kind of TALL_KINDS at a line of the grid moves toward its
     * TALL_PLACES place on a landscape display this high, across (axis 0)
     * or down (1): none up to 16:9, all of it from TALL_EXTRA taller; all
     * of that at the kind's own line, less toward the grid's edges and none
     * at them, so that one dragged to an edge keeps to it; 0 for the other
     * kinds
     */
    static float tallShift(int type, float line, float height, int axis) {
        for (int i = 0; i < TALL_KINDS.length; i++) {
            if (TALL_KINDS[i] != type)
                continue;
            float share = Math.max(0, Math.min(1, (height - GRID_HEIGHT) / TALL_EXTRA));
            float from = axis == 0 ? DEFAULTS[type][0] : type == FIRE_LEFT ? DEFAULTS[type][1] : WHITE_BLACK_ROW;
            float end = axis == 0 ? GRID_WIDTH : GRID_HEIGHT;
            float edge = Math.max(0, Math.min(1, line <= from ? line / from : (end - line) / (end - from)));
            return share * edge * (TALL_PLACES[i][axis] - from);
        }
        return 0;
    }

    /** whether a kind moves on a landscape display this high (tallShift) */
    private static boolean tallShifted(int type, float height) {
        if (!(height > GRID_HEIGHT))
            return false;
        for (int kind : TALL_KINDS) {
            if (kind == type)
                return true;
        }
        return false;
    }

    /**
     * the grid's line, across (axis 0) or down (1), that a kind of
     * TALL_KINDS is placed at this point from, on a landscape display this
     * wide and high: displayX's and displayY's inverse, found by halving
     * (the placing goes one way, as tallShift's edges fall less steeply
     * than the grid's lines rise), the grid's edge beyond it
     */
    private static float tallLineBack(int type, float at, float width, float height, int axis) {
        float low = 0, high = axis == 0 ? GRID_WIDTH : GRID_HEIGHT;
        for (int i = 0; i < 40; i++) {
            float middle = (low + high) / 2;
            float placed = axis == 0 ? middle * width / GRID_WIDTH + tallShift(type, middle, height, 0)
                : displayY(type, middle, height);
            if (placed < at)
                low = middle;
            else
                high = middle;
        }
        return (low + high) / 2;
    }

    /** the layout as a layout file's text, with the look sensitivity */
    String exportConfiguration(float sensitivity) {
        if (!validSensitivity(sensitivity))
            throw new IllegalArgumentException("Invalid sensitivity");
        Properties values = new Properties();
        values.setProperty("format", "halo-touch-layout");
        values.setProperty("version", Integer.toString(portrait ? PORTRAIT_VERSION : VERSION));
        values.setProperty("rumble", Boolean.toString(rumbleEnabled));
        values.setProperty("gyroscope", Boolean.toString(gyroscopeEnabled));
        values.setProperty("opacity", Float.toString(opacity));
        values.setProperty("floating-stick", Boolean.toString(floatingStick));
        // (a landscape layout's file is as it was)
        if (portrait)
            values.setProperty("orientation", "portrait");
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
            String orientation = values.getProperty("orientation", "landscape");
            if (!"landscape".equals(orientation) && !"portrait".equals(orientation)
                    || "portrait".equals(orientation) != (version == PORTRAIT_VERSION))
                throw new IllegalArgumentException("Invalid orientation");
            TouchLayout layout = new TouchLayout("portrait".equals(orientation));
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
                layout.controls.add(readControl(values, i, version, layout.gridWidth(), layout.gridHeight()));
            return new Configuration(layout, sensitivity);
        } catch (IOException | NullPointerException e) {
            throw new IllegalArgumentException("Incomplete layout file", e);
        }
    }

    private static int readVersion(Properties values) {
        if (!"halo-touch-layout".equals(values.getProperty("format")))
            throw new IllegalArgumentException("Unsupported layout file");
        String version = values.getProperty("version");
        for (int known = 1; known <= PORTRAIT_VERSION; known++) {
            if (Integer.toString(known).equals(version))
                return known;
        }
        throw new IllegalArgumentException("Unsupported layout file");
    }

    private static Control readControl(Properties values, int index, int version, float gridWidth,
                                       float gridHeight) {
        String key = "control." + index + ".";
        int type = Integer.parseInt(values.getProperty(key + "type"));
        float x = Float.parseFloat(values.getProperty(key + "x"));
        float y = Float.parseFloat(values.getProperty(key + "y"));
        String shown = values.getProperty(key + "visible");
        float size = version >= 2 ? Float.parseFloat(values.getProperty(key + "size")) : 1f;
        boolean validType = type >= 0 && type < BASE_COUNT && (index >= BASE_COUNT ? type != LEFT : type == index);
        boolean validPlace = Float.isFinite(x) && Float.isFinite(y) && x >= 0 && x <= gridWidth
            && y >= 0 && y <= gridHeight;
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
