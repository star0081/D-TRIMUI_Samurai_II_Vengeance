#ifndef SG_COLLIDER_DEBUG_H
#define SG_COLLIDER_DEBUG_H

/* Menu+Select cycles collider groups: tint one, next press turns it off. */
void collider_debug_next(void);

/* Test noclip: turn off the character collider and slide on the stick.
 * Y is left alone so the body stays at the current height. */
void collider_noclip_arm(void);
void collider_noclip_move(float lx, float ly);

/* Press the first level's lever once the player steps onto that dock. */
void collider_first_lever(void);

#endif
