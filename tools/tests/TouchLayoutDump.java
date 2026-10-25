package com.halo.decomp;

/**
 * Prints the default touch layout as TouchControls places it on a display
 * (its width and height in pixels, its density in dots per inch), for
 * tools/touch_preview.py: the scale, the display in logical units, a finger,
 * the toolbar's height (TouchControls.toolbarY, without insets), then each
 * control's kind, centre, radius and whether it shows. A display taller than
 * it is wide has the portrait layout, as TouchControls gives it.
 */
public final class TouchLayoutDump {
    public static void main(String[] args) {
        float width = Float.parseFloat(args[0]);
        float height = Float.parseFloat(args[1]);
        float density = Float.parseFloat(args[2]) / 160f;
        // as TouchControls.layoutControls and minimumRadius
        TouchLayout layout = new TouchLayout(height > width);
        float scale = Math.max(0.01f, Math.min(width / layout.gridWidth(), height / layout.gridHeight()));
        layout.bounds(width / scale, height / scale);
        layout.setMinimumRadius(Math.min(24 * density / scale, TouchLayout.MAX_MINIMUM_RADIUS));
        float toolbar = layout.portrait ? Math.max(38, height / scale - TouchLayout.PORTRAIT_TOOLBAR_FROM_BOTTOM) : 38;
        System.out.println(scale + " " + width / scale + " " + height / scale + " " + 24 * density / scale + " "
            + toolbar);
        for (int i = 0; i < layout.size(); i++) {
            System.out.println(layout.type(i) + " " + layout.x(i) + " " + layout.y(i) + " " + layout.radius(i) + " "
                + layout.sizeScale(i) + " " + layout.shown(i));
        }
    }
}
