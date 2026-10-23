package com.halo.decomp;

/** What the touch buttons show of the player's state (TouchContext); no phone required. */
public final class TouchContextTest {
    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    public static void main(String[] args) {
        // The state comes from bit 8 of the scene up, past the other bits: known, menus,
        // on, off, a controller (bit 4), the button style (bits 5 and 6), the captions (bit 7).
        int others = 1 | 2 | 4 | 8 | 16 | (3 << 5) | 128;
        int scene = others | (TouchContext.ACTION | TouchContext.VEHICLE) << 8;
        check(TouchContext.fromScene(scene) == (TouchContext.ACTION | TouchContext.VEHICLE),
              "The state is read from bit 8 up");
        check(TouchContext.fromScene(others) == 0, "The other bits are not state");

        // In a vehicle jump and crouch hide, and the action button exits.
        int vehicle = TouchContext.VEHICLE;
        check(TouchContext.hidden(TouchContext.CONTROL_JUMP, vehicle)
              && TouchContext.hidden(TouchContext.CONTROL_CROUCH, vehicle), "Jump and crouch hide in a vehicle");
        check(!TouchContext.hidden(TouchContext.CONTROL_JUMP, 0), "Jump shows on foot");
        for (int control = 0; control < 12; control++) {
            if (control != TouchContext.CONTROL_JUMP && control != TouchContext.CONTROL_CROUCH)
                check(!TouchContext.hidden(control, vehicle), "Only jump and crouch hide in a vehicle: " + control);
        }
        check(!TouchContext.hidden(7, vehicle), "Fire stays in a vehicle (a gunner's too)");
        check("EXIT".equals(TouchContext.caption(TouchContext.CONTROL_ACTION, vehicle, "RELOAD")),
              "The action button exits a vehicle");
        check("RELOAD".equals(TouchContext.caption(TouchContext.CONTROL_ACTION, 0, "RELOAD")),
              "The action button reloads on foot");
        check("JUMP".equals(TouchContext.caption(TouchContext.CONTROL_JUMP, vehicle, "JUMP")),
              "Other captions are unchanged");

        // No grenades of the current type dims the grenade button only.
        check(TouchContext.alpha(TouchContext.CONTROL_GRENADE, TouchContext.NO_GRENADES) == TouchContext.DIMMED,
              "The grenade button dims with no grenades");
        check(TouchContext.alpha(TouchContext.CONTROL_GRENADE, 0) == 1f, "The grenade button is whole with grenades");
        check(TouchContext.alpha(7, TouchContext.NO_GRENADES) == 1f, "No grenades dims nothing else");

        // A prompted action glows steadily; a magazine to reload pulses (and wins).
        float steady = TouchContext.glow(TouchContext.CONTROL_ACTION, TouchContext.ACTION, 0);
        check(steady > 0 && steady == TouchContext.glow(TouchContext.CONTROL_ACTION, TouchContext.ACTION, 333),
              "A prompted action glows steadily");
        check(TouchContext.glow(TouchContext.CONTROL_ACTION, 0, 0) == 0, "No prompt, no glow");
        check(TouchContext.glow(TouchContext.CONTROL_JUMP, TouchContext.ACTION | TouchContext.RELOAD, 300) == 0,
              "Only the action button glows");
        float low = 1, high = 0;
        for (long t = 0; t < 2000; t += 10) {
            float glow = TouchContext.glow(TouchContext.CONTROL_ACTION, TouchContext.RELOAD, t);
            low = Math.min(low, glow);
            high = Math.max(high, glow);
            float swell = TouchContext.swell(TouchContext.CONTROL_ACTION, TouchContext.RELOAD, t);
            check(swell >= 1 && swell <= 1 + TouchContext.PULSE_SWELL, "The pulse swells the button a little");
        }
        check(low < 0.3f && high > 0.95f, "Reloading pulses between faint and full");
        check(TouchContext.swell(TouchContext.CONTROL_ACTION, TouchContext.ACTION, 300) == 1,
              "A prompted action does not swell the button");
        check(TouchContext.animating(TouchContext.RELOAD) && !TouchContext.animating(TouchContext.ACTION),
              "Only the pulse is drawn every frame");
        System.out.println("Touch context checks passed");
    }
}
