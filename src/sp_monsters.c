/*  Copyright (C) 1996-1997  Id Software, Inc.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA

    See file, 'COPYING', for details.
*/

#include "g_local.h"

/* ALL MONSTERS SHOULD BE 1 0 0 IN COLOR */

// name =[framenum,	nexttime, nextthink] {code}
// expands to:
// name ()
// {
//		self.frame=framenum;
//		self.nextthink = time + nexttime;
//		self.think = nextthink
//		<code>
// }

//============================================================================

// LIMITS.
static const int k_bloodfest_monsters = 100;	// maximum monsters allowed in bloodfest mode.
static const int k_bloodfest_projectiles = 30;	// maximum projectiles allowed in bloodfest mode.
static const float k_bloodfest_monsters_spawn_period = 20;	// with which perioud we should issue monsters wave.
static const float k_bloodfest_monsters_spawn_factor = 0.2;	// monsters population is bigger by this each wave.
static const int   k_bloodfest_monsters_spawn_initial = 20;	// monsters population in first wave.
static const int   k_bloodfest_regen_hp = 3;
static const int   k_bloodfest_regen_armor = 2;

static const float k_bloodfest_monsters_damage_factor = 90;	// 

// RUNTIME.
float k_bloodfest_monsters_spawn_time;			// is it time to start monsters spawn wave.
int	  k_bloodfest_monsters_to_spawn;			// amount of monsters we want to spawn in this wave.

static char *monsters_names[] =
{
	"monster_fish",	// WARNING: FISH MUST BE FIRST IN ARRAY, I HAVE HACK FOR IT IN bloodfest_spawn_monsters()!!!
	"monster_ogre",
	"monster_demon1",
	"monster_shambler",
	"monster_knight",
	"monster_army",
	"monster_wizard",
	"monster_dog",
	"monster_zombie",
//	"monster_boss",			// e1m7 boss, can't use it here.
	"monster_tarbaby",
	"monster_hell_knight",
	"monster_shalrath",
	"monster_enforcer",
//	"monster_oldone",		// end boss, can't use it here.
};

static const int monsters_names_count = sizeof(monsters_names) / sizeof(monsters_names[0]);

// print bloodfest stats.
void bloodfest_stats(void)
{
	float time = g_globalvars.time - match_start_time;

	G_bprint( 2, "The surviving is over\n");
	G_bprint( 2, "Surviving time is %.1f seconds\n", time);
}

static void safe_ent_remove( gedict_t * t )
{
	if ( !t || t == world /* || NUM_FOR_EDICT( t ) <= MAX_CLIENTS */ )
		return;

	ent_remove( t );
}

// monsters do more damage with times, so its harder to survive.
float bloodfest_monster_damage_factor(void)
{
#if 1
	return 1;
#else
	float factor = match_start_time ? 1 + (g_globalvars.time - match_start_time) / k_bloodfest_monsters_damage_factor : 1;
	return bound(1, factor, 999999);
#endif
}

void SP_info_monster_start()
{
	if ( deathmatch )
	{
		ent_remove( self );
		return;
	}
}

// return monsters count, including corpses.
int monsters_count( qbool alive )
{
	int cnt = 0;
	gedict_t *p;

    for( p = world; ( p = nextent( p ) ); )
    {
		if ( !( (int)p->s.v.flags & FL_MONSTER ) )
			continue; // not a monster

		if ( alive && !ISLIVE(p) )
			continue; // not alive.

		cnt++;
    }

	return cnt;
}

// return projectiles count.
int projectiles_count(void)
{
	int cnt = 0;
	gedict_t *p;

    for( p = world; ( p = nextent( p ) ); )
    {
		if ( !p->isMissile )
			continue; // not a projectile

		cnt++;
    }

	return cnt;
}


// spawn one monster.
gedict_t * bloodfest_spawn_monster(gedict_t *spot, char * classname)
{
	extern qbool G_CallSpawn( gedict_t * ent );

	gedict_t *	oself;
	gedict_t *	p = spawn();

	p->s.v.classname = classname;
	VectorCopy(spot->s.v.origin, p->s.v.origin);
	VectorCopy(spot->s.v.angles, p->s.v.angles);
	setorigin( p, PASSVEC3(p->s.v.origin) );

	// G_CallSpawn will change 'self', so we have to do trick about it.
	oself = self;	// save!!!

	if (!G_CallSpawn( p ) || !p->s.v.solid)
	{
		// failed to call spawn function, so remove it ASAP.
		ent_remove( p );
		p = NULL;
	}

	self = oself;	// restore!!!

	return p;
}

void bloodfest_wave_calculate(void)
{
	float factor;

	if ( k_bloodfest_monsters_spawn_time > g_globalvars.time )
		return; // not ready to calculate wave.

	// OK. time for next wave!

	// calculate next wave time.
	k_bloodfest_monsters_spawn_time = g_globalvars.time + k_bloodfest_monsters_spawn_period;

	// calculate how much monsters we should spawn in this wave.
	factor = match_start_time ? 1 + k_bloodfest_monsters_spawn_factor * (g_globalvars.time - match_start_time) / k_bloodfest_monsters_spawn_period : 1;
	factor = bound(1, factor, 999999);

	k_bloodfest_monsters_to_spawn = (int)(factor * k_bloodfest_monsters_spawn_initial);
	// if there is still alive monsters, reduce wave by that count.
	k_bloodfest_monsters_to_spawn -= monsters_count( true );

	// apply limits.
	k_bloodfest_monsters_to_spawn = bound( 0, k_bloodfest_monsters_to_spawn, k_bloodfest_monsters );

//	G_cprint("to spawn: %d\n", k_bloodfest_monsters_to_spawn);
}

// attempt to spawn more monsters.
void bloodfest_spawn_monsters(void)
{
	gedict_t *		spot;
	gedict_t *		p;
	int				total_spawns;
	int				i;
	intptr_t		content;

	// precache: spawn all possible monsters and remove them so they precached.
	// we do it once at first frame of the map.
	if ( framecount == 1 )
	{
		for ( i = 0; i < monsters_names_count; i++ )
			safe_ent_remove( bloodfest_spawn_monster( world, monsters_names[i] ) );

		return;
	}

	if ( intermission_running || match_in_progress != 2 || match_over )
			return;

	bloodfest_wave_calculate();

	if ( k_bloodfest_monsters_to_spawn < 1 )
		return; // nothing to spawn.

	// too much monsters, can't spawn more.
	if ( monsters_count( true ) >= k_bloodfest_monsters )
		return;

	// find total amount of spots.
	total_spawns = find_cnt( FOFCLSN, "info_monster_start" );

	// can't find spawn point.
	if ( !total_spawns )
		return;

	// attempt to spawn one monster,
	// we trying to do it few times in row since we can fail because spawn point is busy or something.
	for ( i = 0; i < 10; i++ )
	{
		// find some random spawn point.
		spot = find_idx( i_rnd(0, total_spawns - 1), FOFCLSN, "info_monster_start" );

		// can't find.
		if ( !spot )
			break;

		// get spawn content.
		content = trap_pointcontents( PASSVEC3( spot->s.v.origin ) );
		// spawn monster.
		if ( content == CONTENT_WATER )
			p = bloodfest_spawn_monster( spot, monsters_names[0] ); // HACK: spawn fish.
		else
			p = bloodfest_spawn_monster( spot, monsters_names[i_rnd(1, monsters_names_count - 1)] );

		if ( p )
			break; // spawned something.

//		G_cprint("respawn %d\n", i);
	}

	// reduce amount to spawn next time.
	k_bloodfest_monsters_to_spawn--;
}

// remove monsters corpses, so there free edicts.
void bloodfest_free_monsters(void)
{
	gedict_t *p;

    for( p = world; ( p = nextent( p ) ); )
    {
		if ( !( (int)p->s.v.flags & FL_MONSTER ) )
			continue; // not a monster

		if (match_in_progress == 2)
		{
			if ( ISLIVE( p ) )
				continue; // not dead

			if ( p->dead_time && p->dead_time + 5 > g_globalvars.time )
				continue; // time is not come yet
		}

		ent_remove( p );
    }
}

// apply content damage for the monsters.
void bloodfest_monsters_content_damage(void)
{
	gedict_t *p;

	if ( match_in_progress != 2 )
		return;

    for( p = world; ( p = nextent( p ) ); )
    {
		if ( !( (int)p->s.v.flags & FL_MONSTER ) )
			continue; // not a monster

		if ( !ISLIVE( p ) )
			continue; // dead

		if ( p->s.v.watertype == CONTENT_LAVA )
		{			// do damage
			if ( p->dmgtime < g_globalvars.time )
			{
				p->dmgtime = g_globalvars.time + 0.2;
				p->deathtype = dtLAVA_DMG;
				T_Damage( p, world, world, 30 * p->s.v.waterlevel );
			}
		}
		else if ( p->s.v.watertype == CONTENT_SLIME )
		{			// do damage
			if ( p->dmgtime < g_globalvars.time )
			{
				p->dmgtime = g_globalvars.time + 0.2;
				p->deathtype = dtSLIME_DMG;
				T_Damage( p, world, world, 20 * p->s.v.waterlevel );
			}
		}
    }
}

// remove projectiles if there too much.
void bloodfest_free_projectiles(void)
{
	int remove;
	gedict_t *p;

	remove = projectiles_count() - k_bloodfest_projectiles;

    for( p = world; remove > 0 && ( p = nextent( p ) ); )
    {
		if ( !p->isMissile )
			continue; // not a projectile

		if ( PROG_TO_EDICT( p->s.v.owner )->ct == ctPlayer )
			continue; // can't remove players projectiles.

		ent_remove( p );
		remove--;
    }
}

void bloodfest_change_pov(void)
{
    gedict_t *p;

	if ( self->trackent > 0 && self->trackent <= MAX_CLIENTS )
		p = &g_edicts[ self->trackent ];
	else
		p = world;

	for( ; (p = find_plr( p )); )
	{
		if ( ISLIVE( p ) )
			break; // we found alive player!
	}

	self->trackent = NUM_FOR_EDICT( p ? p : world );
	if ( p )
		G_sprint( self, 2, "tracking %s\n", getname( p )) ;
}

void bloodfest_dead_jump_button( void )
{
	if ( !self->s.v.button2 )
	{
 		self->s.v.flags = ( ( int ) ( self->s.v.flags ) ) | FL_JUMPRELEASED;
		return;
	}

	if ( !( ( ( int ) ( self->s.v.flags ) ) & FL_JUMPRELEASED ) )
		return;

	self->s.v.flags = (int)self->s.v.flags & ~FL_JUMPRELEASED;
	
	// switch pov.
	bloodfest_change_pov();
}

// main clients bloodfest hook.
void bloodfest_client_think(void)
{
	if ( self->ct != ctPlayer )
		return;

	// if player dead, then allow speccing alive players with jump button.
	if ( !ISLIVE( self ) )
		bloodfest_dead_jump_button();
}

void bloodfest_check_end_match(void)
{
    gedict_t *p;

	if ( match_in_progress != 2 )
		return;

	for( p = world; (p = find_plr( p )); )
	{
		if ( ISLIVE( p ) )
			return; // we found at least one alive player!
	}

	// all players is dead, so issue end match now!
	EndMatch( 0 );
}

// called each time something/someone is killed.
void bloodfest_killed_hook( gedict_t * killed, gedict_t * attacker )
{
	if ( match_in_progress != 2 )
		return;

	// if killed was a player then check for endmatch.
	if ( killed->ct == ctPlayer )
	{
		bloodfest_check_end_match();
		return;
	}

	// ok, bellow killed is a monsters or trigger.

	// check for health regen, players regen health for killing monsters.
	if ( !( (int)killed->s.v.flags & FL_MONSTER ) || attacker->ct != ctPlayer || !ISLIVE( attacker ) )
		return;

	// so regen.

	if ( attacker->s.v.health < 250 )
	{
		attacker->s.v.health += k_bloodfest_regen_hp;	
	}
	
	if ( attacker->s.v.armorvalue < 200 )
	{
		attacker->s.v.armorvalue += k_bloodfest_regen_armor;	
		// remove all armors add red armor.
		attacker->s.v.items += IT_ARMOR3 - ( ( int ) attacker->s.v.items & ( IT_ARMOR1 | IT_ARMOR2 | IT_ARMOR3 ) );
		attacker->s.v.armortype = 0.8;
	}
}

// main bloodfest hook.
void bloodfest_think(void)
{
	bloodfest_check_end_match();

	bloodfest_spawn_monsters();
	bloodfest_free_projectiles();
	bloodfest_free_monsters();
	bloodfest_monsters_content_damage();
}

//============================================================================

void MonsterDropPowerups()
{
	int i;

	if ( skill < 3 )
		return; // skill 3 or more required

	if ( !Get_Powerups() )
		return;

	if ( g_random() > cvar("k_nightmare_pu_droprate") )
		return;

	i = i_rnd( 0, 5 );

	switch ( i )
	{
		case 0:

		if ( /* cvar( "dp" ) && */ cvar("k_pow_p") )
			DropPowerup( 30, IT_INVULNERABILITY );

		break;

		case 1:

		if ( /* cvar( "dr" ) && */ cvar("k_pow_r") )
			DropPowerup( 30, IT_INVISIBILITY );

		break;

		// more chances for quad compared to pent and ring

		default:

		if ( /* cvar( "dq" ) && */ cvar("k_pow_q") )
			DropPowerup( 30, IT_QUAD );

		break;
	}
}

//============================================================================

/*
================
monster_use

Using a monster makes it angry at the current activator
================
*/
void monster_use ()
{
	if ( self->s.v.enemy )
		return;

	if ( ISDEAD( self ) )
		return;

	if ( activator->ct != ctPlayer )
		return;
		
	if ( (int)activator->s.v.items & IT_INVISIBILITY )
		return;

	if ( (int)activator->s.v.flags & FL_NOTARGET )
		return;

	// delay reaction so if the monster is teleported, its sound is still heard
	self->s.v.enemy = EDICT_TO_PROG( activator );
	self->s.v.nextthink = g_globalvars.time + 0.1;
	self->s.v.think = ( func_t ) FoundTarget;
}

/*
================
monster_death_use

When a mosnter dies, it fires all of its targets with the current
enemy as activator.
================
*/
void monster_death_use ()
{
	if ( !( ( int )self->s.v.flags & FL_MONSTER ) )
		return; // not a monster

	if ( cvar("k_nightmare_pu"))
		MonsterDropPowerups();

	// fall to ground
	self->s.v.flags = (int)self->s.v.flags & ~( FL_FLY | FL_SWIM);

	if ( !self->s.v.target )
		return;

	activator = PROG_TO_EDICT( self->s.v.enemy );
	SUB_UseTargets ();
}

//============================================================================

void check_monsters_respawn( void )
{
	gedict_t *p, *oself;

	if ( deathmatch )
		return; // no need in dm

	if ( cvar( "k_bloodfest" ) )
	{
		bloodfest_think();
		return;
	}

	if ( skill < 3 )
		return; // skill 3 or more required

	if ( cvar( "k_monster_spawn_time" ) <= 0 )
		return;

    for( p = world; ( p = nextent( p ) ); )
    {
		if ( !( (int)p->s.v.flags & FL_MONSTER ) )
			continue; // not a monster

		if ( ISLIVE( p ) )
			continue; // not dead

		if ( !p->th_respawn )
			continue; // respawn function is not set

		if ( p->monster_desired_spawn_time > g_globalvars.time )
			continue; // time is not come yet

		oself = self;	// save
		self = p;		// WARNING !!!
		
		self->th_respawn();

		self = oself;	// restore
    }
}

//============================================================================

/*
 * Checking is it possibile to put entity at position with such minx/maxs.
 * !!! Pretty expensive for CPU. USE WITH CARE !!!
*/
static qbool is_location_occupied(gedict_t * e, vec3_t pos, vec3_t mins, vec3_t maxs)
{
	qbool occupied = false;
	gedict_t * p;
	vec3_t origin_copy, pos_copy;

	// have to copy origin.
	VectorCopy(self->s.v.origin, origin_copy);
	// have to copy position since it most of the time
	// is e->s.v.origin which is gonna be changed one line below.
	VectorCopy(pos, pos_copy);

	// have to change origin, so damned TraceCapsule() working as expected.
	setorigin( self, 1.0e+5, 1.0e+5, 1.0e+5 );

	TraceCapsule( PASSVEC3( pos_copy ), PASSVEC3( pos_copy ), false, e, PASSVEC3( mins ), PASSVEC3( maxs ) );

	p = PROG_TO_EDICT( g_globalvars.trace_ent );

	if (    g_globalvars.trace_startsolid
		 ||	g_globalvars.trace_fraction != 1
		 || ( p != e && p != world && (p->s.v.solid == SOLID_BSP || p->s.v.solid == SOLID_SLIDEBOX) )
       ) {
		occupied = true; // positon occupied.
	}

	// restore origin.
	setorigin( self, PASSVEC3(origin_copy) );

	return occupied;
}


//============================================================================

typedef enum
{
	mtWalk,
	mtFly,
	mtSwim,
} monsterType_t;

void monster_start_go( monsterType_t mt )
{
	vec3_t tmpv;

	self->s.v.enemy = EDICT_TO_PROG( world );
	self->s.v.goalentity = EDICT_TO_PROG( world );
	self->oldenemy = NULL;
	self->lefty = 0;
	self->search_time = 0;
	self->attack_state = 0;

	if ( mt == mtWalk )
	{
		self->s.v.origin[2] += 1;	// raise off floor a bit
		droptofloor( self );
	}

	VectorSet(tmpv, 0, 1, 0);
	self->s.v.ideal_yaw = DotProduct( self->s.v.angles, tmpv );
	if ( !self->s.v.yaw_speed )
	{
		self->s.v.yaw_speed = ( mt == mtWalk ? 20 : 10 );
	}

	VectorSet( self->s.v.view_ofs, 0, 0, mt == mtSwim ? 10 : 25 );
	self->s.v.use = ( func_t ) monster_use;

	self->s.v.solid = SOLID_SLIDEBOX;
	self->s.v.takedamage = DAMAGE_AIM;

	if ( /*!walkmove( self, 0, 0 )*/
			is_location_occupied( self, self->s.v.origin, self->s.v.mins, self->s.v.maxs )
	)
	{
		if ( k_bloodfest )
		{
//			G_cprint( "monster (%s) in wall at: %.1f %.1f %.1f, removed!\n",
//				self->s.v.classname, self->s.v.origin[0], self->s.v.origin[1], self->s.v.origin[2] );

			ent_remove( self ); // remove it ASAP.
			return;
		}

		G_cprint( "monster in wall at: %.1f %.1f %.1f\n", self->s.v.origin[0], self->s.v.origin[1], self->s.v.origin[2] );

		self->s.v.model = ""; // turn off model
		self->s.v.solid = SOLID_NOT;
		self->s.v.takedamage = DAMAGE_NO;

		self->s.v.nextthink = g_globalvars.time + 5; // reshedule

		setorigin( self, PASSVEC3( self->s.v.oldorigin ) );

		return;
	}

	if ( !strnull( self->mdl ) )
		setmodel( self, self->mdl );	// restore original model

	// seems we respawning monster again, use some funky effects
	if ( self->dead_time )
	{
		play_teleport( self );
		spawn_tfog( self->s.v.origin );
	}

	if ( self->s.v.target )
	{
		self->movetarget = find( world, FOFS( s.v.targetname ), self->s.v.target );
		if ( !self->movetarget ) // NOTE: this is a damn difference with qc
			self->movetarget = world;
		self->s.v.goalentity = EDICT_TO_PROG( self->movetarget );

		VectorSubtract( PROG_TO_EDICT( self->s.v.goalentity )->s.v.origin, self->s.v.origin, tmpv );
		self->s.v.ideal_yaw = vectoyaw( tmpv );

		if ( !self->movetarget || self->movetarget == world )
		{
//			G_bprint( 2, "monster can't find target at: %.1f %.1f %.1f\n", self->s.v.origin[0], self->s.v.origin[1], self->s.v.origin[2] );
			G_cprint( "monster can't find target at: %.1f %.1f %.1f\n", self->s.v.origin[0], self->s.v.origin[1], self->s.v.origin[2] );
		}
		// this used to be an objerror

		if ( self->movetarget && streq( self->movetarget->s.v.classname, "path_corner" ) )
		{
			if ( self->th_walk )
				self->th_walk();
		}
		else
		{
			self->pausetime = 99999999;
			if ( self->th_stand )
				self->th_stand();
		}
	}
	else
	{
		self->pausetime = 99999999;
		if ( self->th_stand )
			self->th_stand();
	}

	// spread think times so they don't all happen at same time
	self->s.v.nextthink = g_globalvars.time + g_random() * 0.5;
}

void walkmonster_start_go()
{
	monster_start_go( mtWalk );
}

void flymonster_start_go()
{
	monster_start_go( mtFly );
}

void swimmonster_start_go()
{
	monster_start_go( mtSwim );
}

static void common_monster_start( char *model, int flags )
{
	self->s.v.flags = (int)self->s.v.flags & ~FL_ONGROUND;

	self->s.v.movetype = MOVETYPE_STEP;

	g_globalvars.total_monsters += 1; // bump monsters total count

	if ( (int)self->s.v.flags & FL_MONSTER )
	{
		// FL_MONSTER set, so we spawn monster non first time, it should be nightmare mode...

		self->s.v.solid = SOLID_NOT;
		setorigin( self, PASSVEC3( self->s.v.oldorigin ) ); // move monster back to his first spawn origin
		VectorCopy( self->oldangles, self->s.v.angles ); // restore angles
	}
	else
	{
		// FL_MONSTER not set, should be first monster spawn

		setmodel( self, model ); // set model (so modelindex is initialized)
		self->mdl = self->s.v.model; // save model
		VectorCopy( self->s.v.origin, self->s.v.oldorigin ); // save first spawn origin
		VectorCopy( self->s.v.angles, self->oldangles ); // save angles
	}

	// turn off model since monster spawn complete in monster_start_go().
	self->s.v.model = "";

	// always set FL_MONSTER and possibily add some additional flags
	self->s.v.flags = (int)self->s.v.flags | FL_MONSTER | flags;
}

void bloodfest_speedup_monster_spawn(void)
{
	if ( !k_bloodfest || !self->s.v.think )
		return;

	self->s.v.nextthink = g_globalvars.time;
	( ( void ( * )() ) ( self->s.v.think ) ) ();
}

void walkmonster_start( char *model )
{
	common_monster_start( model, 0 );

	// delay drop to floor to make sure all doors have been spawned
	// spread think times so they don't all happen at same time
	self->s.v.nextthink = g_globalvars.time + 0.1 + g_random() * 0.5;
	self->s.v.think = ( func_t ) walkmonster_start_go;

	bloodfest_speedup_monster_spawn();
}

void flymonster_start( char *model )
{
	// set FL_FLY early so that we're not affected by gravity
	common_monster_start( model, FL_FLY );

	// spread think times so they don't all happen at same time
	self->s.v.nextthink = g_globalvars.time + 0.1 + g_random() * 0.5;
	self->s.v.think = ( func_t ) flymonster_start_go;

	bloodfest_speedup_monster_spawn();
}

void swimmonster_start( char *model )
{
	// set FL_SWIM early so that we're not affected by gravity
	common_monster_start( model, FL_SWIM );

	// spread think times so they don't all happen at same time
	self->s.v.nextthink = g_globalvars.time + 0.1 + g_random() * 0.5;
	self->s.v.think = (func_t) swimmonster_start_go;

	bloodfest_speedup_monster_spawn();
}
