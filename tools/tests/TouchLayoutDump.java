package com.halo.decomp;

/**
 * Prints the default touch layout as TouchControls places it on a display
 * (its width and height in pixels, its density in dots per inch), for
 * tools/touch_preview.py: the scale, the display in logical units, then each
 * control's kind, centre, radius and whether it shows.
 */
public final class TouchLayoutDump {
    public static void main(String[] args) {
        float width = Float.parseFloat(args[0]);
        float height = Float.parseFloat(args[1]);
        float density = Float.parseFloat(args[2]) / 160f;
        // as TouchControls.layoutControls and minimumRadius
        float scale = Math.max(0.01f, Math.min(width / 960f, height / 540f));
        TouchLayout layout = new TouchLayout();
        layout.bounds(width / scale, height / scale);
        layout.setMinimumRadius(Math.min(24 * density / scale, TouchLayout.MAX_MINIMUM_RADIUS));
        System.out.println(scale + " " + width / scale + " " + height / scale + " " + 24 * density / scale);
        for (int i = 0; i < layout.size(); i++) {
            System.out.println(layout.type(i) + " " + layout.x(i) + " " + layout.y(i) + " " + layout.radius(i) + " "
                + layout.sizeScale(i) + " " + layout.shown(i));
        }
    }
}
