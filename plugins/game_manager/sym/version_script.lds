{
    global:
        Plugify_*;

        /* s2sdk binding slots (defined by s2sdk_bindings, filled by the plugify module from the
           s2sdk plugin declared as a dependency) */
        __s2sdk_*;

        /* No exported methods: game_manager exposes nothing to other plugins. */

    local: *;
};
