/* bots_tactics.h: the bots of a team playing together (port/linux/game/bots_tactics.c)

What a team's bots do for each other, on top of what each does alone
(bots.c): they keep apart and within reach of each other, fire at the enemy
a teammate is hurting and take angles apart on one enemy, go after the
killer of a teammate while that player's shields are down, close in
together after a kill and fall back together when the fight goes against
them, and share out the power items and the escort of a human teammate, the
best placed bot taking each. What the team knows of its enemies is what
its bots saw (bots_knowledge.h).

Everything is decided on the host from the game state at the tick (where
the players are, their shields, the damage the game records on each unit)
and from what the bots did the tick before, in the players' order: no random
numbers, so two runs of the same input play the same. bots.tactics turns
the changes off; the minute line's counts are kept either way, so that games
with and without them compare. Bot numbers are bots.h's (0 up, by machine). */
#ifndef HALO_BOTS_TACTICS_H
#define HALO_BOTS_TACTICS_H

/* what a team asks of one of its bots, besides its own choices */
enum
{
	_bots_tactic_none = 0,
	/* after the killer of a teammate, where the team last knew the killer */
	_bots_tactic_trade,
	/* to where the team just killed an enemy */
	_bots_tactic_collapse,
	/* back to the team, the fight going against it */
	_bots_tactic_fall_back,
	/* with a human teammate (the one bot of the team that escorts that
	player) */
	_bots_tactic_escort,
	/* back within reach of a teammate, too far from all of them */
	_bots_tactic_support,
	NUMBER_OF_BOTS_TACTICS,
};

/* a map's start and end: everything forgotten */
void bots_tactics_reset(void);
/* the start of a tick that runs the bots (bots_update_actions, before the
first bot): the deaths since the last tick and who killed, each team's
state, its claims of items and escorts */
void bots_tactics_tick_begin(void);
/* a bot's tick done: its player (its datum), the enemy it fights (NONE),
and whether it fired */
void bots_tactics_bot_done(long bot_index, long player_index, long target_player_index, boolean firing);

/* how much lower (better) an enemy's score as the bot's target is: the
enemy a teammate hurts now, the killer of a teammate */
real bots_tactics_target_bonus(long bot_index, long target_player_index);
/* the side a fighting bot strafes to (1: its left, -1: its right), away
from a teammate close by or round the enemy away from a teammate who fights
it too; 0: its own choice */
real bots_tactics_strafe_side(long bot_index, long target_player_index, real_point3d const *position,
	real_point3d const *target_position);
/* a walking bot's direction (radians) turned away from a teammate too
close, and its throttle eased behind one ahead of it */
void bots_tactics_steer(long bot_index, real_point3d const *position, real *direction, real *throttle);
/* where the team asks the bot to go (a _bots_tactic_*), and the point;
_bots_tactic_none for nowhere (the bot's own choice) */
short bots_tactics_goal(long bot_index, real_point3d const *position, real_point3d *point);
/* the bot may go for a power item (bots_knowledge_item's index): no
teammate is better placed for it, or the one that is (not in a fight nor
sent elsewhere by the team) has let four seconds pass without claiming it */
boolean bots_tactics_may_claim(long bot_index, long item_index);

/* the minute line's part (empty without teams) */
void bots_tactics_report(char *line, long size);

#endif
