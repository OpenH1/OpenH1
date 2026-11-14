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
        fold.setSize(0, 2f);
        check(fold.x(0) + fold.radius(0) <= 960 && fold.savedX(0) == placeX && fold.savedY(0) == placeY,
              "A control in a corner made larger is pushed inward, its place kept");
        fold.setSize(0, 1f);
        check(fold.x(0) == cornerX && fold.y(0) == cornerY, "Made smaller again, it goes back to the corner");
        // A taller display keeps the top row at the top and the rest as far
        // from the bottom as on a 16:9 one (a folding phone's inner screen,
        // a 4:3 tablet, a tall window); white and black (the flashlight and
        // the grenade's type) come down with the rest, above the D-pad.
        TouchLayout flat = new TouchLayout();
        TouchLayout notTaller = new TouchLayout();
        notTaller.bounds(960, 540);
        TouchLayout wider = new TouchLayout();
        wider.bounds(1260, 540);
        for (int kind : new int[] {TouchLayout.WHITE, TouchLayout.BLACK})
            check(notTaller.x(kind) == flat.x(kind) && notTaller.y(kind) == flat.y(kind)
                  && wider.y(kind) == flat.y(kind) && wider.x(kind) == flat.x(kind) * 1260 / 960,
                  "White and black stay in the top row up to 16:9 and on wider displays");
        for (float tall : new float[] {600, 720, 865.05f, 926.1f, 2133.3f}) {
            TouchLayout high = new TouchLayout();
            high.bounds(960, tall);
            for (int i = 0; i < high.size(); i++) {
                boolean whiteOrBlack = high.type(i) == TouchLayout.WHITE || high.type(i) == TouchLayout.BLACK;
                if (flat.y(i) <= TouchLayout.TOP_ROW && !whiteOrBlack)
                    check(high.y(i) == flat.y(i), "The top row stays at the top of a taller display");
                else if (!whiteOrBlack)
                    check(Math.abs((tall - high.y(i)) - (540 - flat.y(i))) < 0.01f,
                          "The controls under the thumbs keep their distance from the bottom of a taller display");
                if (!whiteOrBlack && high.type(i) != TouchLayout.FIRE_LEFT)
                    check(high.x(i) == flat.x(i), "A taller display keeps each control's distance from the sides");
            }
            for (int kind : new int[] {0, TouchLayout.WHITE, TouchLayout.FIRE_LEFT}) {
                for (float line = 0; line <= 540; line += 0.5f) {
                    float y = TouchLayout.displayY(kind, line, tall);
                    check(Math.abs(TouchLayout.gridY(kind, y, tall) - line) < 0.001f,
                          "A line of the grid comes back from the display");
                    check(line == 0 || y > TouchLayout.displayY(kind, line - 0.5f, tall),
                          "Lower lines of the grid stay lower on the display");
                }
            }
            // (white and black beside the D-pad's right arrow, one above the other)
            check(high.x(TouchLayout.WHITE) - high.radius(TouchLayout.WHITE) > high.x(15) + high.radius(15)
                  && high.x(TouchLayout.BLACK) == high.x(TouchLayout.WHITE)
                  && high.y(TouchLayout.WHITE) < high.y(15) && high.y(TouchLayout.BLACK) > high.y(15)
                  && high.y(TouchLayout.WHITE) > high.y(12) - high.radius(12)
                  && high.y(TouchLayout.BLACK) < high.y(13) + high.radius(13),
                  "White and black are beside the D-pad, one above the other, on a taller display");
            // (and at a finger's largest width, as in a small window: every control, the second fire too)
            for (float finger : new float[] {0, TouchLayout.MAX_MINIMUM_RADIUS}) {
                high.setMinimumRadius(finger);
                for (int a = 0; a < high.size(); a++)
                    for (int b = a + 1; b < high.size(); b++)
                        check(Math.hypot(high.x(a) - high.x(b), high.y(a) - high.y(b)) >= high.radius(a) + high.radius(b),
                              "Controls do not overlap on a taller display (" + a + ", " + b + " at " + tall + ")");
            }
            high.setMinimumRadius(0);
            high.move(TouchLayout.WHITE, 480, tall / 3);
            float whiteMiddle = high.y(TouchLayout.WHITE);
            high.bounds(960, 540);
            high.bounds(960, tall);
            check(Math.abs(high.y(TouchLayout.WHITE) - whiteMiddle) < 0.01f,
                  "White put high on a tall display comes back there");
            high.move(0, 480, tall / 2);
            float middle = high.y(0);
            high.bounds(960, 540);
            high.bounds(960, tall);
            check(Math.abs(high.y(0) - middle) < 0.01f, "A control put in the middle of a tall display comes back there");
            // (white, black and the second fire, moved toward their places
            // beside the D-pad, dragged to each edge stay at it)
            for (int kind : new int[] {TouchLayout.WHITE, TouchLayout.BLACK, TouchLayout.FIRE_LEFT}) {
                for (float[] edge : new float[][] {{-99999, tall / 2}, {99999, tall / 2}, {480, -99999}, {480, 99999}}) {
                    TouchLayout dragged = new TouchLayout();
                    dragged.bounds(960, tall);
                    dragged.move(kind, edge[0], edge[1]);
                    float x = dragged.x(kind), y = dragged.y(kind);
                    dragged.bounds(960, 540);
                    dragged.bounds(960, tall);
                    check(Math.abs(dragged.x(kind) - x) < 0.01f && Math.abs(dragged.y(kind) - y) < 0.01f,
                          "A control dragged to an edge of a tall display comes back there (" + kind + ", "
                          + edge[0] + ", " + edge[1] + " at " + tall + ")");
                }
            }
        }
        // The portrait layout: its own grid and defaults, apart at every
        // portrait shape (a 16:9 phone, 20:9, the Fold7's cover, a split
        // screen half, a folding phone's inner screen), as low as on 16:9 on
        // a taller one and the right thumb's cluster at the right of a wider
        // one; its file says it is portrait, a landscape one's is as it was.
        TouchLayout upright = new TouchLayout(true);
        check(upright.portrait && upright.gridWidth() == 540 && upright.gridHeight() == 960,
              "A portrait layout is on its own grid");
        check(upright.size() == 18 && !upright.shown(TouchLayout.FIRE_LEFT), "A portrait layout has every control");
        for (float[] display : new float[][] {
                {540, 960, 24 * 2.25f / 2.25f}, {540, 1200, 24 * 2.625f / 2f}, {540, 1260, 24 * 2.35f / 2f},
                {901.5f, 960, 24 * 2.625f / 1.198f}, {865, 960, 24 * 2.35f / 2.275f}, {540, 960, 34}}) {
            TouchLayout tall = new TouchLayout(true);
            tall.bounds(display[0], display[1]);
            tall.setMinimumRadius(display[2]);
            for (int a = 0; a < tall.size(); a++) {
                if (!tall.shown(a))
                    continue;
                check(tall.x(a) - tall.radius(a) >= 0 && tall.x(a) + tall.radius(a) <= display[0]
                      && tall.y(a) - tall.radius(a) >= 0 && tall.y(a) + tall.radius(a) <= display[1],
                      "A portrait control stays on the display");
                for (int b = a + 1; b < tall.size(); b++) {
                    if (tall.shown(b))
                        check(Math.hypot(tall.x(a) - tall.x(b), tall.y(a) - tall.y(b)) >= tall.radius(a) + tall.radius(b),
                              "Portrait controls do not overlap at " + display[0] + " x " + display[1]);
                }
            }
        }
        TouchLayout portraitBase = new TouchLayout(true);
        TouchLayout portraitTall = new TouchLayout(true);
        portraitTall.bounds(540, 1200);
        TouchLayout portraitWide = new TouchLayout(true);
        portraitWide.bounds(865, 960);
        for (int i = 0; i < portraitBase.size(); i++) {
            if (portraitBase.y(i) <= TouchLayout.PORTRAIT_STRIP)
                check(portraitTall.y(i) == portraitBase.y(i) && portraitTall.x(i) == portraitBase.x(i),
                      "The portrait strip's controls keep their distance from the top of a taller display");
            else
                check(Math.abs((1200 - portraitTall.y(i)) - (960 - portraitBase.y(i))) < 0.01f
                      && portraitTall.x(i) == portraitBase.x(i),
                      "Portrait controls keep their distance from the bottom of a taller display");
            if (portraitBase.x(i) >= TouchLayout.RIGHT_PART)
                check(Math.abs((865 - portraitWide.x(i)) - (540 - portraitBase.x(i))) < 0.01f,
                      "The right thumb's portrait controls keep their distance from the right of a wider display");
            else if (portraitBase.x(i) <= TouchLayout.LEFT_PART)
                check(portraitWide.x(i) == portraitBase.x(i),
                      "The left thumb's portrait controls keep their distance from the left of a wider display");
        }
        // The strip: back and pause in it, at the top under a camera's
        // cutout; the picture under it, lowered half way into the free space
        // and clear of the default controls where there is room (20:9, the
        // Fold7's cover), under the strip where there is none (16:9, a
        // folding phone's inner screen); a moved control does not move it.
        for (float[] display : new float[][] {{540, 960}, {540, 1200}, {540, 1260}, {865, 960}, {901.5f, 960}}) {
            for (float inset : new float[] {0, 40}) {
                TouchLayout strip = new TouchLayout(true);
                strip.bounds(display[0], display[1]);
                strip.setTopInset(inset);
                for (int i : new int[] {10, 11})
                    check(strip.y(i) + strip.radius(i) <= strip.stripBottom() && strip.y(i) - strip.radius(i) >= inset,
                          "Back and pause are in the strip, under the cutout");
                float lowest = display[1];
                for (int i = 0; i < strip.size(); i++)
                    if (strip.shown(i) && strip.y(i) > strip.stripBottom())
                        lowest = Math.min(lowest, strip.y(i) - strip.radius(i));
                for (boolean sixteenNine : new boolean[] {false, true}) {
                    float high = TouchLayout.pictureHeight(display[0], sixteenNine);
                    float top = strip.pictureTop(high);
                    float room = lowest - 10 - strip.stripBottom() - high;
                    if (room >= 0)
                        check(top >= strip.stripBottom() && top + high <= lowest
                              && Math.abs((top - strip.stripBottom()) - (lowest - 10 - top - high)) < 0.01f,
                              "A lowered picture is half way between the strip and the controls, clear of them");
                    else
                        check(top == Math.max(inset, lowest - 10 - high) && top < strip.stripBottom(),
                              "Without room the picture moves up as far as it must to clear the controls, to the top");
                    // (and with the picture behind the strip, pause moves to its middle, off the HUD's
                    // ammunition, and back to its place when the picture is lowered again; one dragged
                    // there stays where it was put)
                    TouchLayout plain = new TouchLayout(true);
                    plain.bounds(display[0], display[1]);
                    plain.setTopInset(inset);
                    strip.setPictureTop(top);
                    if (room >= 0)
                        check(!strip.stripGathered() && strip.x(TouchLayout.PAUSE) == plain.x(TouchLayout.PAUSE),
                              "Pause stays at its place with the picture under the strip");
                    else
                        check(strip.stripGathered()
                              && strip.x(TouchLayout.PAUSE) == display[0] / 2 - TouchLayout.GATHERED_APART
                              && strip.y(TouchLayout.PAUSE) == plain.y(TouchLayout.PAUSE),
                              "Pause is in the strip's middle with the picture behind the strip");
                    strip.move(TouchLayout.PAUSE, display[0] / 2 - 90, strip.y(TouchLayout.PAUSE));
                    float dragged = strip.x(TouchLayout.PAUSE);
                    strip.setPictureTop(strip.stripBottom());
                    strip.setPictureTop(top);
                    check(Math.abs(strip.x(TouchLayout.PAUSE) - dragged) < 0.01f, "A dragged pause stays where it was put");
                    strip.setPictureTop(strip.stripBottom());
                    check(!strip.stripGathered(), "A picture under the strip leaves pause at its place");
                    strip.resetDefaults();
                    TouchLayout moved = new TouchLayout(true);
                    moved.bounds(display[0], display[1]);
                    moved.setTopInset(inset);
                    moved.move(TouchLayout.WHITE, 30, moved.stripBottom() + 5);
                    check(moved.pictureTop(high) == top, "A moved control does not move the picture");
                }
            }
        }
        TouchLayout tallStrip = new TouchLayout(true);
        tallStrip.bounds(540, 1200);
        check(tallStrip.pictureTop(TouchLayout.pictureHeight(540, false)) > TouchLayout.PORTRAIT_STRIP + 100,
              "On a 20:9 phone the picture is lowered");
        portraitWide.move(4, 600, 300);
        float portraitX = portraitWide.x(4), portraitY = portraitWide.y(4);
        TouchLayout.Configuration uprightCopy = TouchLayout.importConfiguration(portraitWide.exportConfiguration(1.5f));
        check(uprightCopy.layout.portrait, "A portrait layout's file says it is portrait");
        uprightCopy.layout.bounds(865, 960);
        check(Math.abs(uprightCopy.layout.x(4) - portraitX) < 0.01f && Math.abs(uprightCopy.layout.y(4) - portraitY) < 0.01f,
              "A portrait layout comes back from its file");
        check(!new TouchLayout().exportConfiguration(1f).contains("orientation")
              && new TouchLayout().exportConfiguration(1f).contains("version=3"),
              "A landscape layout's file is as it was");
        // (a portrait layout's file has a version the builds before portrait, and iOS's, refuse)
        String portraitFile = portraitWide.exportConfiguration(1f);
        check(portraitFile.contains("version=4"), "A portrait layout's file is version 4");
        for (String mismatched : new String[] {portraitFile.replace("version=4", "version=3"),
                new TouchLayout().exportConfiguration(1f).replace("version=3", "version=4")}) {
            try {
                TouchLayout.importConfiguration(mismatched);
                check(false, "Version 4 is a portrait layout's, and only its");
            } catch (IllegalArgumentException expected) {
            }
        }
        check(!TouchLayout.importConfiguration(new TouchLayout().exportConfiguration(1f)).layout.portrait,
              "A layout file without an orientation is landscape");
        String sideways = portraitWide.exportConfiguration(1f).replace("orientation=portrait", "orientation=sideways");
        try {
            TouchLayout.importConfiguration(sideways);
            check(false, "An unknown orientation must be refused");
        } catch (IllegalArgumentException expected) {
        }
        TouchLayout settingsFrom = new TouchLayout();
        settingsFrom.rumbleEnabled = false;
        settingsFrom.gyroscopeEnabled = true;
        settingsFrom.floatingStick = true;
        settingsFrom.setOpacity(0.5f);
        TouchLayout settingsTo = new TouchLayout(true);
        settingsTo.takeSettings(settingsFrom);
        check(!settingsTo.rumbleEnabled && settingsTo.gyroscopeEnabled && settingsTo.floatingStick
              && settingsTo.opacity == 0.5f, "The settings follow from one layout to the other");
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
