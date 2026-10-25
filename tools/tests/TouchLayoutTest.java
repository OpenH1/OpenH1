package com.halo.decomp;

/** Standalone geometry/persistence regression checks; no phone required. */
public final class TouchLayoutTest {
    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }
    public static void main(String[] args) {
        TouchLayout layout = new TouchLayout();
        check(layout.size() == 18, "All 17 buttons and the movement stick must be editable");
        check(layout.x(TouchLayout.LEFT) == 115 && layout.y(TouchLayout.FIRE_LEFT) == 239,
              "Existing layouts must start with the original controls");
        layout.move(TouchLayout.LEFT, -500, -500);
        check(layout.x(TouchLayout.LEFT) >= 64 && layout.y(TouchLayout.LEFT) == 64,
              "Dragging must keep the full stick inside the screen without a toolbar exclusion");
        layout.move(TouchLayout.FIRE_LEFT, 10000, 10000);
        check(layout.x(TouchLayout.FIRE_LEFT)+39 <= 960 && layout.y(TouchLayout.FIRE_LEFT)+39 <= 540,
              "Dragging beyond the bottom/right edge must keep the stick reachable");
        layout.move(0, 230, 320);
        layout.move(5, 440, 390);
        TouchLayout reopened = new TouchLayout();
        for (int i = 0; i < layout.size(); i++) reopened.restore(i, layout.x(i), layout.y(i));
        for (int i = 0; i < layout.size(); i++) {
            check(layout.x(i) == reopened.x(i) && layout.y(i) == reopened.y(i),
                  "Saved coordinates must round-trip for control "+i);
        }
        float before = reopened.x(0);
        reopened.restore(0, Float.NaN, 200);
        reopened.restore(0, Float.POSITIVE_INFINITY, 200);
        reopened.restore(0, -20, 200);
        check(reopened.x(0) == before, "Invalid saved coordinates must not corrupt the layout");
        reopened.move(0, Float.NaN, 200);
        check(reopened.x(0) == before, "Invalid drag coordinates must not corrupt the layout");
        // A saved top-row Pause button is valid even before it has been moved.
        check(reopened.y(10) == 36, "Reopening must preserve unmoved top-row controls");
        reopened.bounds(1200, 540);
        reopened.move(0, 9999, 1);
        check(reopened.x(0) == 1200 - reopened.radius(0) && reopened.y(0) == reopened.radius(0),
              "Controls must reach the full widescreen edge and top edge");
        TouchLayout wide = new TouchLayout();
        wide.restore(0, reopened.savedX(0), reopened.savedY(0));
        wide.bounds(1200, 540);
        check(wide.x(0) == reopened.x(0), "Widescreen positions must survive reload");
        // Duplicates retain their action and are independently placed and hidden.
        int copy = wide.duplicate(4);
        check(copy == 18 && wide.type(copy) == 4, "Fire copies must retain the fire action");
        wide.setShown(4, false);
        wide.move(copy, 300, 100);
        check(!wide.shown(4) && wide.shown(copy), "Hiding an original must not hide its copy");
        check(wide.duplicate(TouchLayout.LEFT) == -1, "There can only be one movement stick");
        String exported = wide.exportConfiguration(2.25f);
        TouchLayout.Configuration imported = TouchLayout.importConfiguration(exported);
        imported.layout.bounds(1200, 540);
        check(imported.sensitivity == 2.25f && imported.layout.size() == 19,
              "Configuration must preserve sensitivity and duplicates");
        for (int i = 0; i < wide.size(); i++) {
            check(imported.layout.type(i) == wide.type(i) && imported.layout.shown(i) == wide.shown(i)
                && Math.abs(imported.layout.x(i)-wide.x(i)) < 0.001f
                && Math.abs(imported.layout.y(i)-wide.y(i)) < 0.001f,
                "Export/import must preserve each control on widescreen");
        }
        check(wide.add(4) == 4 && wide.shown(4), "Add must restore a hidden button first");
        wide.setShown(TouchLayout.LEFT, false);
        check(wide.add(TouchLayout.LEFT) == TouchLayout.LEFT, "Hidden move stick must be restorable");
        reject(exported.replace("version=3", "version=9"));
        reject(exported.replace("control.18.type=4", "control.18.type=16"));
        reject(exported.replace("control.18.x=240.0", "control.18.x=NaN"));
        reject(exported.replace("count=19", "count=10000"));
        reject(exported.replace("sensitivity=2.25", "sensitivity=Infinity"));
        reject(exported.replace("control.0.visible=false", "control.0.visible=maybe")
            .replace("control.0.visible=true", "control.0.visible=maybe"));
        reject("not a layout");
        wide.setSize(copy, 1.6f);
        wide.rumbleEnabled = false; wide.gyroscopeEnabled = true;
        TouchLayout.Configuration sized = TouchLayout.importConfiguration(wide.exportConfiguration(2.25f));
        check(sized.layout.sizeScale(copy) == 1.6f && !sized.layout.rumbleEnabled && sized.layout.gyroscopeEnabled,
              "Sizes, rumble and gyro settings must survive export/import");
        int sizedCopy = wide.duplicate(copy);
        check(wide.sizeScale(sizedCopy) == 1.6f, "Duplicates inherit their source size");
        wide.move(copy, 99999, 99999);
        wide.setSize(copy, 2f);
        check(wide.x(copy)+wide.radius(copy) <= 1200 && wide.y(copy)+wide.radius(copy) <= 540,
              "Growing a button at the edge must keep it reachable");
        String sizedText = sized.layout.exportConfiguration(2.25f);
        reject(sizedText.replace("control.18.size=1.6", "control.18.size=NaN"));
        reject(sizedText.replace("control.18.size=1.6", "control.18.size=3.0"));
        reject(sizedText.replace("rumble=false", "rumble=invalid"));
        reject(sizedText.replace("gyroscope=true", "gyroscope=invalid"));
        String legacy = sizedText.replace("version=3", "version=1").replaceAll("(?m)^.*\\.size=.*\\R", "")
            .replaceAll("(?m)^(rumble|gyroscope|opacity|floating-stick)=.*\\R", "");
        TouchLayout.Configuration old = TouchLayout.importConfiguration(legacy);
        check(old.layout.sizeScale(copy) == 1 && old.layout.rumbleEnabled && !old.layout.gyroscopeEnabled,
              "Legacy layouts must load with default sizes and gyro disabled");
        // Opacity (version 3) round-trips; version 2 files load fully opaque.
        sized.layout.setOpacity(0.45f);
        String translucent = sized.layout.exportConfiguration(2.25f);
        check(TouchLayout.importConfiguration(translucent).layout.opacity == 0.45f, "Opacity must survive export/import");
        reject(translucent.replace("opacity=0.45", "opacity=0.05"));
        reject(translucent.replace("opacity=0.45", "opacity=NaN"));
        sized.layout.floatingStick = true;
        check(TouchLayout.importConfiguration(sized.layout.exportConfiguration(2.25f)).layout.floatingStick,
              "The floating stick must survive export/import");
        sized.layout.floatingStick = false;
        reject(translucent.replace("floating-stick=false", "floating-stick=maybe"));
        String version2 = translucent.replace("version=3", "version=2").replaceAll("(?m)^(opacity|floating-stick)=.*\\R", "");
        check(TouchLayout.importConfiguration(version2).layout.opacity == 1f
              && !TouchLayout.importConfiguration(version2).layout.floatingStick,
              "Version 2 layouts load fully opaque, with the stick in its place");
        reject(translucent.replaceAll("(?m)^opacity=.*\\R", ""));
        try {
            sized.layout.setOpacity(1.5f);
            throw new AssertionError("Out-of-range opacity was accepted");
        } catch (IllegalArgumentException expected) {
            // refused
        }
        // A finger-sized minimum radius enlarges small buttons and keeps them on the display.
        TouchLayout fingers = new TouchLayout();
        fingers.setMinimumRadius(30);
        check(fingers.radius(12) == 30 && fingers.radius(TouchLayout.LEFT) == 64,
              "The minimum radius enlarges small controls only");
        fingers.move(12, 0, 0);
        check(fingers.x(12) == 30 && fingers.y(12) == 30, "Enlarged controls stay inside the display");
        check(fingers.savedX(12) == 30, "The minimum radius does not change saved places");
        fingers.setMinimumRadius(80);
        check(fingers.radius(12) == TouchLayout.MAX_MINIMUM_RADIUS, "The minimum radius is capped");
        TouchLayout dense = new TouchLayout();
        dense.setMinimumRadius(80);
        for (int a = 12; a <= 15; a++) {
            for (int b = a + 1; b <= 15; b++) {
                check(Math.hypot(dense.x(a) - dense.x(b), dense.y(a) - dense.y(b)) >= dense.radius(a) + dense.radius(b),
                      "At the capped minimum radius the D-pad's buttons stay apart");
            }
        }
        // The default controls never overlap: on a phone (2400 x 1080 at 420 dpi, a finger 31.5 units wide), a
        // 21:9 phone (2520 x 1080 at 360 dpi), tablets of 16:10 (2560 x 1600 at 320 dpi) and 4:3 (2560 x 1920 at
        // 360 dpi), folding phones' inner screens from 1.2:1 (2208 x 1840 at 380 dpi) to nearly square
        // (2152 x 2076 at 390 dpi), and at the cap.
        float[][] displays = {{1200, 540, 31.5f}, {1260, 540, 24 * 2.25f / 2f}, {960, 600, 24 * 2 / 2.6667f},
                              {960, 720, 24 * 2.25f / 2.6667f}, {960, 800, 24 * 2.375f / 2.3f},
                              {960, 926.1f, 24 * 2.4375f / 2.2417f}, {960, 540, 80}};
        for (float[] display : displays) {
            TouchLayout spread = new TouchLayout();
            spread.bounds(display[0], display[1]);
            spread.setMinimumRadius(display[2]);
            for (int a = 0; a < spread.size(); a++) {
                for (int b = a + 1; b < spread.size(); b++) {
                    check(Math.hypot(spread.x(a) - spread.x(b), spread.y(a) - spread.y(b))
                          >= spread.radius(a) + spread.radius(b),
                          "Default controls " + a + " and " + b + " overlap on a " + display[0] + " x " + display[1] + " display");
                }
            }
        }
        // A control dragged into a corner of a folding phone's inner screen (2184 x 1968 at
        // 376 dpi) is back in the corner after the cover screen (2520 x 1080) and back: the
        // shorter display and the larger finger push it inward only while they last, and its
        // saved place stays where it was put.
        TouchLayout fold = new TouchLayout();
        fold.bounds(960, 865.05f);
        fold.setMinimumRadius(24 * 2.35f / 2.275f);
        fold.move(0, 99999, 99999);
        float cornerX = fold.x(0), cornerY = fold.y(0), placeX = fold.savedX(0), placeY = fold.savedY(0);
        for (int round = 0; round < 3; round++) {
            fold.bounds(1260, 540);
            fold.setMinimumRadius(24 * 2.35f / 2f);
            check(fold.x(0) + fold.radius(0) <= 1260 && fold.y(0) + fold.radius(0) <= 540,
                  "A control in a corner stays on the cover screen");
            fold.bounds(960, 865.05f);
            fold.setMinimumRadius(24 * 2.35f / 2.275f);
        }
        check(fold.x(0) == cornerX && fold.y(0) == cornerY,
              "A control in a corner comes back to it after folding and unfolding");
        check(fold.savedX(0) == placeX && fold.savedY(0) == placeY, "Folding does not move a saved place");
        // Sizes follow use: fire the largest, then jump and the grenade, then reload,
        // swap and melee; the seldom used (stick clicks, white, black, back, the
        // D-pad) smaller than all of those.
        TouchLayout plain = new TouchLayout();
        for (int i = 0; i < plain.size(); i++)
            check(i == 4 || i == TouchLayout.LEFT || plain.radius(4) > plain.radius(i), "Fire is the largest button");
        check(plain.radius(0) > plain.radius(1) && plain.radius(5) > plain.radius(2) && plain.radius(0) > plain.radius(3),
              "Jump and the grenade are larger than melee, reload and swap");
        int[] minor = {6, 7, 8, 9, 11, 12, 13, 14, 15};
        for (int small : minor) {
            for (int i = 0; i < 6; i++)
                check(plain.radius(small) < plain.radius(i), "Seldom used control " + small + " is smaller than " + i);
        }
        // The second fire is hidden until added; it then shows in its place.
        check(!plain.shown(TouchLayout.FIRE_LEFT), "The second fire is hidden by default");
        check(plain.add(TouchLayout.FIRE_LEFT) == TouchLayout.FIRE_LEFT && plain.shown(TouchLayout.FIRE_LEFT),
              "The second fire can be added");
        fingers.setMinimumRadius(Float.NaN);
        check(fingers.radius(12) == 22, "An invalid minimum radius is none");
        wide.resetDefaults();
        check(wide.size() == 18 && wide.shown(4) && wide.x(4) == 862*1200f/960,
              "Reset restores defaults on the current display and removes all copies");
        check(wide.sizeScale(4) == 1f && !wide.rumbleEnabled && wide.gyroscopeEnabled,
              "Button reset must restore sizes without changing General settings");
        while (wide.duplicate(4) >= 0) {}
        check(wide.size() == TouchLayout.MAX_CONTROLS, "Duplicate count must be bounded");
        System.out.println("Touch layout, visibility, duplication and import/export checks passed");
    }

    private static void reject(String text) {
        try { TouchLayout.importConfiguration(text); }
        catch (IllegalArgumentException expected) { return; }
        throw new AssertionError("Invalid configuration was accepted");
    }
}
