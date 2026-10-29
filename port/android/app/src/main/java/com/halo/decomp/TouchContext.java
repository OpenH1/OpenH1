package com.halo.decomp;

/**
 * What the touch controls' buttons show of player 1's state (TouchControls):
 * the game sends it with the scene, as touch_game.c's _touch_context_* bits
 * from bit 8 up (port/linux/src/touch_input.c). The buttons go by the game
 * control each does in the player's profile (input_abstraction.c's
 * _game_control_*), so the state follows the button layout.
 */
final class TouchContext {
    /** the bits: the HUD prompts an action, the magazine wants reloading, no grenades, in a vehicle, alive */
    static final int ACTION = 1;
    static final int RELOAD = 2;
    static final int NO_GRENADES = 4;
    static final int VEHICLE = 8;
    static final int ALIVE = 16;
    private static final int SHIFT = 8;
    private static final int MASK = 31;

    /** the game controls the state changes */
    static final int CONTROL_JUMP = 0;
    static final int CONTROL_ACTION = 2;
    static final int CONTROL_GRENADE = 6;
    static final int CONTROL_CROUCH = 10;

    /** how solid the grenade button is drawn with none of its type */
    static final float DIMMED = 0.35f;
    /** the action button's glow, steady while an action is prompted */
    static final float ACTION_GLOW = 1f;
    /** the reload pulse: its rate, and how much it swells the button */
    static final float PULSE_HZ = 1.6f;
    static final float PULSE_SWELL = 0.06f;

    private TouchContext() {
    }

    static int fromScene(int scene) {
        return (scene >>> SHIFT) & MASK;
    }

    /** jump and crouch hide in a vehicle's seat: they do nothing there */
    static boolean hidden(int control, int context) {
        return (context & VEHICLE) != 0 && (control == CONTROL_JUMP || control == CONTROL_CROUCH);
    }

    /**
     * the kind of control crouched (tapped on, -1 standing) once a button of
     * a kind goes down, doing a game control: with input.touch_crouch
     * "toggle" Crouch's tap crouches or stands up; Jump stands up
     */
    static int crouchPressed(int crouched, int kind, int control, boolean toggle) {
        if (toggle && control == CONTROL_CROUCH)
            return crouched == kind ? -1 : kind;
        return control == CONTROL_JUMP ? -1 : crouched;
    }

    /**
     * the kind crouched as it stays, its button doing a game control: it
     * stands up once Crouch is held instead ("hold"), the button no longer
     * crouches (the profile changed) or hides (a vehicle's seat), or the
     * player dies
     */
    static int crouchKept(int crouched, int control, int context, boolean toggle) {
        return crouched >= 0 && toggle && control == CONTROL_CROUCH && !hidden(control, context)
            && (context & ALIVE) != 0 ? crouched : -1;
    }

    /** the action button's caption in a vehicle: it gets out; the others' as they are */
    static String caption(int control, int context, String caption) {
        return (context & VEHICLE) != 0 && control == CONTROL_ACTION ? "EXIT" : caption;
    }

    /** how solid a button is drawn: the grenade's dimmed with none of the current type */
    static float alpha(int control, int context) {
        return control == CONTROL_GRENADE && (context & NO_GRENADES) != 0 ? DIMMED : 1f;
    }

    /** the reload pulse's phase, 0..1, at a time */
    private static float pulse(long millis) {
        double phase = (millis % 1000000L) / 1000.0 * PULSE_HZ * 2 * Math.PI;
        return (float) (0.5 - 0.5 * Math.cos(phase));
    }

    /**
     * the glow around the action button, 0 (none) to 1: pulsing while the
     * magazine wants reloading, steady while the HUD prompts an action
     */
    static float glow(int control, int context, long millis) {
        if (control != CONTROL_ACTION)
            return 0;
        if ((context & RELOAD) != 0)
            return 0.2f + 0.8f * pulse(millis);
        return (context & ACTION) != 0 ? ACTION_GLOW : 0;
    }

    /** the action button's size while it pulses (1: as laid out) */
    static float swell(int control, int context, long millis) {
        return control == CONTROL_ACTION && (context & RELOAD) != 0 ? 1 + PULSE_SWELL * pulse(millis) : 1;
    }

    /** whether the buttons change from frame to frame (the pulse): drawn every frame */
    static boolean animating(int context) {
        return (context & RELOAD) != 0;
    }
}
