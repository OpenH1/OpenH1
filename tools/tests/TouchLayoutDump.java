package com.halo.decomp;

/**
 * Prints the default touch layout as TouchControls places it on a display
 * (its width and height in pixels, its density in dots per inch), for
 * tools/touch_preview.py: the scale, the display in logical units, a finger,
 * the toolbar's height (TouchControls.toolbarY, without insets), the
 * toolbar's hide across, and in portrait the picture's top and bottom (else
 * -1 -1), then each control's kind, centre, radius and whether it shows. A
 * display taller than it is wide has the portrait layout, as TouchControls
 * gives it; a fourth argument "16:9" a 16:9 picture.
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
        float toolbar = layout.portrait ? layout.stripBottom() - TouchLayout.PORTRAIT_STRIP / 2 : 38;
        float hide = layout.portrait ? width / scale - 42 - 70 : width / scale / 2;
        // (TouchControls.placePicture: whole pixels)
        boolean wide = args.length > 3 && args[3].equals("16:9");
        float top = -1, bottom = -1;
        if (layout.portrait) {
            int pictureHeight = (int) width * 480 / (wide ? 852 : 640);
            int pixels = Math.round(layout.pictureTop(TouchLayout.pictureHeight(width / scale, wide)) * scale);
            pixels = Math.max(0, Math.min(pixels, (int) height - pictureHeight));
            top = pixels / scale;
            bottom = (pixels + pictureHeight) / scale;
        }
        System.out.println(scale + " " + width / scale + " " + height / scale + " " + 24 * density / scale + " "
            + toolbar + " " + hide + " " + top + " " + bottom);
        for (int i = 0; i < layout.size(); i++) {
            System.out.println(layout.type(i) + " " + layout.x(i) + " " + layout.y(i) + " " + layout.radius(i) + " "
                + layout.sizeScale(i) + " " + layout.shown(i));
        }
    }
}
