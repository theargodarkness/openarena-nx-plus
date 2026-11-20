/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
// cl_cgame.c  -- client system interaction with client game

#include "client.h"

#include "../botlib/botlib.h"

#ifdef USE_MUMBLE
#include "libmumblelink.h"
#endif

extern	botlib_export_t	*botlib_export;

extern qboolean loadCamera(const char *name);
extern void startCamera(int time);
extern qboolean getCameraInfo(int time, vec3_t *origin, vec3_t *angles);

/*
====================
CL_GetGameState
====================
*/
void CL_GetGameState( gameState_t *gs ) {
	*gs = cl.gameState;
}

/*
====================
CL_GetGlconfig
====================
*/
void CL_GetGlconfig( glconfig_t *glconfig ) {
	*glconfig = cls.glconfig;
}


/*
====================
CL_GetUserCmd
====================
*/
qboolean CL_GetUserCmd( int cmdNumber, usercmd_t *ucmd ) {
	// cmds[cmdNumber] is the last properly generated command

	// can't return anything that we haven't created yet
	if ( cmdNumber > cl.cmdNumber ) {
		Com_Error( ERR_DROP, "CL_GetUserCmd: %i >= %i", cmdNumber, cl.cmdNumber );
	}

	// the usercmd has been overwritten in the wrapping
	// buffer because it is too far out of date
	if ( cmdNumber <= cl.cmdNumber - CMD_BACKUP ) {
		return qfalse;
	}

	*ucmd = cl.cmds[ cmdNumber & CMD_MASK ];

	return qtrue;
}

int CL_GetCurrentCmdNumber( void ) {
	return cl.cmdNumber;
}


/*
====================
CL_GetParseEntityState
====================
*/
qboolean	CL_GetParseEntityState( int parseEntityNumber, entityState_t *state ) {
	// can't return anything that hasn't been parsed yet
	if ( parseEntityNumber >= cl.parseEntitiesNum ) {
		Com_Error( ERR_DROP, "CL_GetParseEntityState: %i >= %i",
			parseEntityNumber, cl.parseEntitiesNum );
	}

	// can't return anything that has been overwritten in the circular buffer
	if ( parseEntityNumber <= cl.parseEntitiesNum - MAX_PARSE_ENTITIES ) {
		return qfalse;
	}

	*state = cl.parseEntities[ parseEntityNumber & ( MAX_PARSE_ENTITIES - 1 ) ];
	return qtrue;
}

/*
====================
CL_GetCurrentSnapshotNumber
====================
*/
void	CL_GetCurrentSnapshotNumber( int *snapshotNumber, int *serverTime ) {
	*snapshotNumber = cl.snap.messageNum;
	*serverTime = cl.snap.serverTime;
}

/*
====================
CL_GetSnapshot
====================
*/
qboolean	CL_GetSnapshot( int snapshotNumber, snapshot_t *snapshot ) {
	clSnapshot_t	*clSnap;
	int				i, count;

	if ( snapshotNumber > cl.snap.messageNum ) {
		Com_Error( ERR_DROP, "CL_GetSnapshot: snapshotNumber > cl.snapshot.messageNum" );
	}

	// if the frame has fallen out of the circular buffer, we can't return it
	if ( cl.snap.messageNum - snapshotNumber >= PACKET_BACKUP ) {
		return qfalse;
	}

	// if the frame is not valid, we can't return it
	clSnap = &cl.snapshots[snapshotNumber & PACKET_MASK];
	if ( !clSnap->valid ) {
		return qfalse;
	}

	// if the entities in the frame have fallen out of their
	// circular buffer, we can't return it
	if ( cl.parseEntitiesNum - clSnap->parseEntitiesNum >= MAX_PARSE_ENTITIES ) {
		return qfalse;
	}

	// write the snapshot
	snapshot->snapFlags = clSnap->snapFlags;
	snapshot->serverCommandSequence = clSnap->serverCommandNum;
	snapshot->ping = clSnap->ping;
	snapshot->serverTime = clSnap->serverTime;
	Com_Memcpy( snapshot->areamask, clSnap->areamask, sizeof( snapshot->areamask ) );
	snapshot->ps = clSnap->ps;
	count = clSnap->numEntities;
	if ( count > MAX_ENTITIES_IN_SNAPSHOT ) {
		Com_DPrintf( "CL_GetSnapshot: truncated %i entities to %i\n", count, MAX_ENTITIES_IN_SNAPSHOT );
		count = MAX_ENTITIES_IN_SNAPSHOT;
	}
	snapshot->numEntities = count;
	for ( i = 0 ; i < count ; i++ ) {
		snapshot->entities[i] = 
			cl.parseEntities[ ( clSnap->parseEntitiesNum + i ) & (MAX_PARSE_ENTITIES-1) ];
	}

	// FIXME: configstring changes and server commands!!!

	return qtrue;
}

/*
=====================
CL_SetUserCmdValue
=====================
*/
void CL_SetUserCmdValue( int userCmdValue, float sensitivityScale ) {
	cl.cgameUserCmdValue = userCmdValue;
	cl.cgameSensitivity = sensitivityScale;
}

/*
=====================
CL_AddCgameCommand
=====================
*/
void CL_AddCgameCommand( const char *cmdName ) {
	Cmd_AddCommand( cmdName, NULL );
}


/*
=====================
CL_ConfigstringModified
=====================
*/
void CL_ConfigstringModified( void ) {
	char		*old, *s;
	int			i, index;
	char		*dup;
	gameState_t	oldGs;
	int			len;

	index = atoi( Cmd_Argv(1) );
	if ( index < 0 || index >= MAX_CONFIGSTRINGS ) {
		Com_Error( ERR_DROP, "CL_ConfigstringModified: bad index %i", index );
	}
	// get everything after "cs <num>"
	s = Cmd_ArgsFrom(2);

	old = cl.gameState.stringData + cl.gameState.stringOffsets[ index ];
	if ( !strcmp( old, s ) ) {
		return;		// unchanged
	}

	// build the new gameState_t
	oldGs = cl.gameState;

	Com_Memset( &cl.gameState, 0, sizeof( cl.gameState ) );

	// leave the first 0 for uninitialized strings
	cl.gameState.dataCount = 1;
		
	for ( i = 0 ; i < MAX_CONFIGSTRINGS ; i++ ) {
		if ( i == index ) {
			dup = s;
		} else {
			dup = oldGs.stringData + oldGs.stringOffsets[ i ];
		}
		if ( !dup[0] ) {
			continue;		// leave with the default empty string
		}

		len = strlen( dup );

		if ( len + 1 + cl.gameState.dataCount > MAX_GAMESTATE_CHARS ) {
			Com_Error( ERR_DROP, "MAX_GAMESTATE_CHARS exceeded" );
		}

		// append it to the gameState string buffer
		cl.gameState.stringOffsets[ i ] = cl.gameState.dataCount;
		Com_Memcpy( cl.gameState.stringData + cl.gameState.dataCount, dup, len + 1 );
		cl.gameState.dataCount += len + 1;
	}

	if ( index == CS_SYSTEMINFO ) {
		// parse serverId and other cvars
		CL_SystemInfoChanged();
	}

}


/*
===================
CL_GetServerCommand

Set up argc/argv for the given command
===================
*/
qboolean CL_GetServerCommand( int serverCommandNumber ) {
	char	*s;
	char	*cmd;
	static char bigConfigString[BIG_INFO_STRING];
	int argc;

	// if we have irretrievably lost a reliable command, drop the connection
	if ( serverCommandNumber <= clc.serverCommandSequence - MAX_RELIABLE_COMMANDS ) {
		// when a demo record was started after the client got a whole bunch of
		// reliable commands then the client never got those first reliable commands
		if ( clc.demoplaying )
			return qfalse;
		Com_Error( ERR_DROP, "CL_GetServerCommand: a reliable command was cycled out" );
		return qfalse;
	}

	if ( serverCommandNumber > clc.serverCommandSequence ) {
		Com_Error( ERR_DROP, "CL_GetServerCommand: requested a command not received" );
		return qfalse;
	}

	s = clc.serverCommands[ serverCommandNumber & ( MAX_RELIABLE_COMMANDS - 1 ) ];
	clc.lastExecutedServerCommand = serverCommandNumber;

	Com_DPrintf( "serverCommand: %i : %s\n", serverCommandNumber, s );

rescan:
	Cmd_TokenizeString( s );
	cmd = Cmd_Argv(0);
	argc = Cmd_Argc();

	if ( !strcmp( cmd, "disconnect" ) && cl_localClient > 0 ) {
		// An extra split-screen client was dropped. That only takes that
		// player out, and it has to wait until we are out of its cgame.
		Com_Printf( "Player %d was disconnected by the server\n", cl_localClient + 1 );
		cl_dropLocalClient[cl_localClient] = qtrue;
		return qfalse;
	}

	if ( !strcmp( cmd, "disconnect" ) ) {
		// https://zerowing.idsoftware.com/bugzilla/show_bug.cgi?id=552
		// allow server to indicate why they were disconnected
		if ( argc >= 2 )
			Com_Error( ERR_SERVERDISCONNECT, "Server disconnected - %s", Cmd_Argv( 1 ) );
		else
			Com_Error( ERR_SERVERDISCONNECT, "Server disconnected" );
	}

	if ( !strcmp( cmd, "bcs0" ) ) {
		Com_sprintf( bigConfigString, BIG_INFO_STRING, "cs %s \"%s", Cmd_Argv(1), Cmd_Argv(2) );
		return qfalse;
	}

	if ( !strcmp( cmd, "bcs1" ) ) {
		s = Cmd_Argv(2);
		if( strlen(bigConfigString) + strlen(s) >= BIG_INFO_STRING ) {
			Com_Error( ERR_DROP, "bcs exceeded BIG_INFO_STRING" );
		}
		strcat( bigConfigString, s );
		return qfalse;
	}

	if ( !strcmp( cmd, "bcs2" ) ) {
		s = Cmd_Argv(2);
		if( strlen(bigConfigString) + strlen(s) + 1 >= BIG_INFO_STRING ) {
			Com_Error( ERR_DROP, "bcs exceeded BIG_INFO_STRING" );
		}
		strcat( bigConfigString, s );
		strcat( bigConfigString, "\"" );
		s = bigConfigString;
		goto rescan;
	}

	if ( !strcmp( cmd, "cs" ) ) {
		CL_ConfigstringModified();
		// reparse the string, because CL_ConfigstringModified may have done another Cmd_TokenizeString()
		Cmd_TokenizeString( s );
		return qtrue;
	}

	if ( !strcmp( cmd, "map_restart" ) ) {
		// clear notify lines and outgoing commands before passing
		// the restart to the cgame
		Con_ClearNotify();
		// reparse the string, because Con_ClearNotify() may have done another Cmd_TokenizeString()
		Cmd_TokenizeString( s );
		Com_Memset( cl.cmds, 0, sizeof( cl.cmds ) );
		return qtrue;
	}

	// the clientLevelShot command is used during development
	// to generate 128*128 screenshots from the intermission
	// point of levels for the menu system to use
	// we pass it along to the cgame to make appropriate adjustments,
	// but we also clear the console and notify lines here
	if ( !strcmp( cmd, "clientLevelShot" ) ) {
		// don't do it if we aren't running the server locally,
		// otherwise malicious remote servers could overwrite
		// the existing thumbnails
		if ( !com_sv_running->integer ) {
			return qfalse;
		}
		// close the console
		Con_Close();
		// take a special screenshot next frame
		Cbuf_AddText( "wait ; wait ; wait ; wait ; screenshot levelshot\n" );
		return qtrue;
	}

	// we may want to put a "connect to other server" command here

	// cgame can now act on the command
	return qtrue;
}


/*
====================
CL_CM_LoadMap

Just adds default parameters that cgame doesn't need to know about
====================
*/
void CL_CM_LoadMap( const char *mapname ) {
	int		checksum;

	CM_LoadMap( mapname, qtrue, &checksum );
}

/*
====================
CL_ShutdonwCGame

====================
*/
void CL_ShutdownCGame( void ) {
	Key_SetCatcher( Key_GetCatcher( ) & ~KEYCATCH_CGAME );
	cls.cgameStarted = qfalse;
	if ( !cgvm ) {
		return;
	}
	VM_Call( cgvm, CG_SHUTDOWN );
	VM_Free( cgvm );
	cgvm = NULL;
}

/*
====================
Split-screen drawing

Each cgame believes it owns the whole screen. Its drawing is squeezed into
the viewport of its local client on the way to the renderer, so the game
code needs no changes and players can join or leave without restarting it.
====================
*/
qboolean	cl_worldMapLoaded;
static qhandle_t	cl_markerShader;
static char	cl_worldMapName[MAX_QPATH];

static void CL_LoadWorldMap( const char *name ) {
	// every local client asks for the world; the renderer loads it once
	if ( cl_worldMapLoaded && !Q_stricmp( name, cl_worldMapName ) ) {
		return;
	}

	re.LoadWorld( name );
	cl_markerShader = 0;	// registrations were flushed before this load
	Q_strncpyz( cl_worldMapName, name, sizeof( cl_worldMapName ) );
	cl_worldMapLoaded = qtrue;
}

/*
====================
CL_HudScale

cl_hudScale shrinks everything the game draws in 2D (status bar, scores,
crosshair, HUD models) towards the middle of the view. The game code lays
its HUD out for the full screen and can't be asked to do otherwise, so
this is done on its drawing as it passes through. Pictures that cover the
whole view - damage flashes, backgrounds - are left alone.
====================
*/
static float CL_HudScale( float w, float h ) {
	float scale = cl_hudScale->value;

	if ( scale >= 1.0f || clc.state != CA_ACTIVE ) {
		return 1.0f;
	}
	if ( scale < 0.5f ) {
		scale = 0.5f;
	}
	if ( w >= cls.glconfig.vidWidth * 0.9f && h >= cls.glconfig.vidHeight * 0.9f ) {
		return 1.0f;
	}
	return scale;
}

static void CL_DrawStretchPic( float x, float y, float w, float h,
		float s1, float t1, float s2, float t2, qhandle_t hShader ) {
	float hud = CL_HudScale( w, h );

	if ( hud != 1.0f ) {
		float cx = cls.glconfig.vidWidth * 0.5f;
		float cy = cls.glconfig.vidHeight * 0.5f;

		x = cx + ( x - cx ) * hud;
		y = cy + ( y - cy ) * hud;
		w *= hud;
		h *= hud;
	}

	if ( CL_SplitScreenActive() ) {
		int		vx, vy, vw, vh;
		float	sx, sy;

		CL_GetViewport( cl_localClient, &vx, &vy, &vw, &vh );
		sx = (float)vw / cls.glconfig.vidWidth;
		sy = (float)vh / cls.glconfig.vidHeight;

		x = vx + x * sx;
		y = vy + y * sy;
		w *= sx;
		h *= sy;
	}

	re.DrawStretchPic( x, y, w, h, s1, t1, s2, t2, hShader );
}

/*
====================
Engine-drawn aids

Enemy markers and hit markers are added by the engine around each player's
world view, because the game code that normally draws the HUD is used as
it comes. Everything here reads the same snapshot the game code is given.
====================
*/
// cl_markerShader (declared above): 0 = not registered since the last flush
static int			cl_lastHits[MAX_LOCAL_CLIENTS];
static int			cl_hitMarkerTime[MAX_LOCAL_CLIENTS];

#define	HITMARKER_MSEC	180

static const char *CL_PlayerInfo( int clientNum ) {
	int offset;

	if ( clientNum < 0 || clientNum >= MAX_CLIENTS ) {
		return "";
	}
	offset = cl.gameState.stringOffsets[CS_PLAYERS + clientNum];
	if ( !offset ) {
		return "";
	}
	return cl.gameState.stringData + offset;
}

/*
====================
CL_IsEnemy

Everyone else in a free-for-all, the other team in team games. With co-op
on (sv_splitscreenPvP 0) the other split-screen players are friends.
====================
*/
static qboolean CL_IsEnemy( int clientNum ) {
	const char	*info = CL_PlayerInfo( clientNum );
	int			team, myTeam, gametype, i;

	if ( !info[0] || clientNum == cl.snap.ps.clientNum ) {
		return qfalse;
	}

	team = atoi( Info_ValueForKey( info, "t" ) );
	if ( team == TEAM_SPECTATOR ) {
		return qfalse;
	}

	if ( !Cvar_VariableIntegerValue( "sv_splitscreenPvP" ) ) {
		for ( i = 0; i < cl_numLocalClients; i++ ) {
			if ( CL_LocalClientState( i ) == CA_ACTIVE && CL_LocalClientNum( i ) == clientNum ) {
				return qfalse;
			}
		}
	}

	gametype = atoi( Info_ValueForKey( cl.gameState.stringData + cl.gameState.stringOffsets[CS_SERVERINFO], "g_gametype" ) );
	if ( gametype >= GT_TEAM ) {
		myTeam = atoi( Info_ValueForKey( CL_PlayerInfo( cl.snap.ps.clientNum ), "t" ) );
		return team != myTeam;
	}
	return qtrue;
}

static void CL_DrawMarkerText( float cx, float y, float size, const char *text, const float *color ) {
	char	clean[MAX_NAME_LENGTH];
	int		len, i;
	float	x;

	Q_strncpyz( clean, text, sizeof( clean ) );
	Q_CleanStr( clean );
	len = strlen( clean );
	if ( !len ) {
		return;
	}

	x = cx - len * size * 0.5f;
	re.SetColor( color );
	for ( i = 0; i < len; i++, x += size ) {
		int		ch = clean[i] & 255;
		float	frow = ( ch >> 4 ) * 0.0625f;
		float	fcol = ( ch & 15 ) * 0.0625f;

		if ( ch == ' ' ) {
			continue;
		}
		re.DrawStretchPic( x, y, size, size, fcol, frow, fcol + 0.0625f, frow + 0.0625f, cls.charSetShader );
	}
	re.SetColor( NULL );
}

/*
====================
CL_AddEnemyHalos

Before the view is rendered: a red glow around every enemy in sight. It is
part of the scene, so walls hide it like they hide the enemy.
====================
*/
static void CL_AddEnemyHalos( void ) {
	int i;

	if ( !( cl_enemyMarker->integer & 1 ) || !com_sv_running->integer || !cl.snap.valid ) {
		return;
	}

	if ( !cl_markerShader ) {
		cl_markerShader = re.RegisterShader( "flareShader" );
		if ( !cl_markerShader ) {
			cl_markerShader = -1;	// this game's data has none; don't keep asking
		}
	}
	if ( cl_markerShader <= 0 ) {
		return;
	}

	for ( i = 0; i < cl.snap.numEntities; i++ ) {
		entityState_t	*es = &cl.parseEntities[( cl.snap.parseEntitiesNum + i ) & ( MAX_PARSE_ENTITIES - 1 )];
		refEntity_t		ent;

		if ( es->eType != ET_PLAYER || ( es->eFlags & ( EF_DEAD | EF_NODRAW ) )
			|| ( es->powerups & ( 1 << PW_INVIS ) ) || !CL_IsEnemy( es->clientNum ) ) {
			continue;
		}

		Com_Memset( &ent, 0, sizeof( ent ) );
		ent.reType = RT_SPRITE;
		VectorCopy( es->pos.trBase, ent.origin );
		ent.origin[2] += 8;
		ent.radius = 38;
		ent.customShader = cl_markerShader;
		ent.shaderRGBA[0] = 200;
		ent.shaderRGBA[1] = 16;
		ent.shaderRGBA[2] = 10;
		ent.shaderRGBA[3] = 255;
		re.AddRefEntityToScene( &ent );
	}
}

/*
====================
CL_DrawViewOverlays

After the view is rendered: red name tags over enemies that are in sight,
and the hit marker. "rd" is the view as it was given to the renderer.
====================
*/
static void CL_DrawViewOverlays( const refdef_t *rd ) {
	static const float	red[4] = { 1.0f, 0.15f, 0.1f, 1.0f };
	static const float	white[4] = { 1.0f, 1.0f, 1.0f, 0.9f };
	float				tanX, tanY, charSize;
	int					i, hits;

	if ( clc.state != CA_ACTIVE || !cl.snap.valid ) {
		return;
	}

	charSize = rd->height / 40.0f;
	if ( charSize < 8.0f ) {
		charSize = 8.0f;
	}

	if ( ( cl_enemyMarker->integer & 2 ) && com_sv_running->integer ) {
		tanX = tan( DEG2RAD( rd->fov_x ) * 0.5f );
		tanY = tan( DEG2RAD( rd->fov_y ) * 0.5f );

		for ( i = 0; i < cl.snap.numEntities; i++ ) {
			entityState_t	*es = &cl.parseEntities[( cl.snap.parseEntitiesNum + i ) & ( MAX_PARSE_ENTITIES - 1 )];
			vec3_t			head, local;
			float			fwd, right, up, px, py;
			trace_t			tr;

			if ( es->eType != ET_PLAYER || ( es->eFlags & ( EF_DEAD | EF_NODRAW ) )
				|| ( es->powerups & ( 1 << PW_INVIS ) ) || !CL_IsEnemy( es->clientNum ) ) {
				continue;
			}

			VectorCopy( es->pos.trBase, head );
			head[2] += 48;

			VectorSubtract( head, rd->vieworg, local );
			fwd = DotProduct( local, rd->viewaxis[0] );
			if ( fwd < 16.0f || tanX <= 0.0f || tanY <= 0.0f ) {
				continue;	// behind the view
			}
			right = -DotProduct( local, rd->viewaxis[1] );
			up = DotProduct( local, rd->viewaxis[2] );

			px = rd->x + rd->width * 0.5f * ( 1.0f + right / ( fwd * tanX ) );
			py = rd->y + rd->height * 0.5f * ( 1.0f - up / ( fwd * tanY ) );
			if ( px < rd->x || px > rd->x + rd->width || py < rd->y + charSize || py > rd->y + rd->height ) {
				continue;
			}

			// not through walls
			CM_BoxTrace( &tr, rd->vieworg, head, NULL, NULL, 0, CONTENTS_SOLID, qfalse );
			if ( tr.fraction < 1.0f ) {
				continue;
			}

			CL_DrawMarkerText( px, py - charSize, charSize, Info_ValueForKey( CL_PlayerInfo( es->clientNum ), "n" ), red );
		}
	}

	// hit marker: the game counts our hits in the player state
	hits = cl.snap.ps.persistant[PERS_HITS];
	if ( hits > cl_lastHits[cl_localClient] && cl_hitMarker->integer ) {
		cl_hitMarkerTime[cl_localClient] = cls.realtime;
	}
	cl_lastHits[cl_localClient] = hits;

	if ( cl_hitMarker->integer && cl_hitMarkerTime[cl_localClient]
		&& cls.realtime - cl_hitMarkerTime[cl_localClient] < HITMARKER_MSEC ) {
		// four short diagonal strokes around the crosshair
		float cx = rd->x + rd->width * 0.5f;
		float cy = rd->y + rd->height * 0.5f;
		float dot = rd->height / 180.0f;
		int   step, sx, sy;

		if ( dot < 2.0f ) {
			dot = 2.0f;
		}

		re.SetColor( white );
		for ( sx = -1; sx <= 1; sx += 2 ) {
			for ( sy = -1; sy <= 1; sy += 2 ) {
				for ( step = 3; step <= 6; step++ ) {
					re.DrawStretchPic( cx + sx * step * dot - dot * 0.5f, cy + sy * step * dot - dot * 0.5f,
						dot, dot, 0, 0, 0, 0, cls.whiteShader );
				}
			}
		}
		re.SetColor( NULL );
	}
}

static void CL_RenderScene( const refdef_t *fd ) {
	refdef_t	rd;
	refdef_t	hudRd;
	qboolean	worldView;
	int			vx, vy, vw, vh;
	float		sx, sy;

	// models drawn as part of the HUD shrink with it
	if ( ( fd->rdflags & RDF_NOWORLDMODEL ) && CL_HudScale( fd->width, fd->height ) != 1.0f ) {
		float hud = CL_HudScale( fd->width, fd->height );
		float cx = cls.glconfig.vidWidth * 0.5f;
		float cy = cls.glconfig.vidHeight * 0.5f;

		hudRd = *fd;
		hudRd.x = (int)( cx + ( fd->x - cx ) * hud );
		hudRd.y = (int)( cy + ( fd->y - cy ) * hud );
		hudRd.width = (int)( fd->width * hud );
		hudRd.height = (int)( fd->height * hud );
		if ( hudRd.width < 1 || hudRd.height < 1 ) {
			return;
		}
		fd = &hudRd;
	}

	worldView = !( fd->rdflags & RDF_NOWORLDMODEL );
	if ( worldView ) {
		CL_AddEnemyHalos();
	}

	if ( !CL_SplitScreenActive() ) {
		re.RenderScene( fd );
		if ( worldView ) {
			CL_DrawViewOverlays( fd );
		}
		return;
	}

	CL_GetViewport( cl_localClient, &vx, &vy, &vw, &vh );
	sx = (float)vw / cls.glconfig.vidWidth;
	sy = (float)vh / cls.glconfig.vidHeight;

	rd = *fd;
	rd.x = vx + (int)( fd->x * sx );
	rd.y = vy + (int)( fd->y * sy );
	rd.width = (int)( fd->width * sx );
	rd.height = (int)( fd->height * sy );

	if ( rd.width < 1 || rd.height < 1 ) {
		return;
	}

	// The game picked its field of view for the full screen's shape. Keep
	// the world view undistorted in the new shape by opening up whichever
	// axis grew, never by zooming in. HUD models (no world) are left alone
	// so they stretch exactly like the 2D HUD around them.
	if ( !( fd->rdflags & RDF_NOWORLDMODEL ) && fd->width > 0 && fd->height > 0 ) {
		float oldAspect = (float)fd->width / fd->height;
		float newAspect = (float)rd.width / rd.height;

		if ( newAspect > oldAspect ) {
			// wider: keep the vertical view, see more to the sides
			float t = tan( DEG2RAD( fd->fov_y ) * 0.5f ) * newAspect;
			rd.fov_x = RAD2DEG( atan( t ) ) * 2.0f;
		} else if ( newAspect < oldAspect ) {
			// taller: keep the horizontal view, see more up and down
			float t = tan( DEG2RAD( fd->fov_x ) * 0.5f ) / newAspect;
			rd.fov_y = RAD2DEG( atan( t ) ) * 2.0f;
		}
	}

	re.RenderScene( &rd );
	if ( worldView ) {
		CL_DrawViewOverlays( &rd );
	}
}

static int	FloatAsInt( float f ) {
	floatint_t fi;
	fi.f = f;
	return fi.i;
}

/*
====================
CL_CgameSystemCalls

The cgame module is making a system call
====================
*/
intptr_t CL_CgameSystemCalls( intptr_t *args ) {
	switch( args[0] ) {
	case CG_PRINT:
		// every local client is told the same things; print them once
		if ( cl_localClient == 0 ) {
			Com_Printf( "%s", (const char*)VMA(1) );
		}
		return 0;
	case CG_ERROR:
		Com_Error( ERR_DROP, "%s", (const char*)VMA(1) );
		return 0;
	case CG_MILLISECONDS:
		return Sys_Milliseconds();
	case CG_CVAR_REGISTER:
		Cvar_Register( VMA(1), VMA(2), VMA(3), args[4] ); 
		return 0;
	case CG_CVAR_UPDATE:
		Cvar_Update( VMA(1) );
		return 0;
	case CG_CVAR_SET:
		Cvar_SetSafe( VMA(1), VMA(2) );
		return 0;
	case CG_CVAR_VARIABLESTRINGBUFFER:
		Cvar_VariableStringBuffer( VMA(1), VMA(2), args[3] );
		return 0;
	case CG_ARGC:
		return Cmd_Argc();
	case CG_ARGV:
		Cmd_ArgvBuffer( args[1], VMA(2), args[3] );
		return 0;
	case CG_ARGS:
		Cmd_ArgsBuffer( VMA(1), args[2] );
		return 0;
	case CG_FS_FOPENFILE:
		return FS_FOpenFileByMode( VMA(1), VMA(2), args[3] );
	case CG_FS_READ:
		FS_Read( VMA(1), args[2], args[3] );
		return 0;
	case CG_FS_WRITE:
		FS_Write( VMA(1), args[2], args[3] );
		return 0;
	case CG_FS_FCLOSEFILE:
		FS_FCloseFile( args[1] );
		return 0;
	case CG_FS_SEEK:
		return FS_Seek( args[1], args[2], args[3] );
	case CG_SENDCONSOLECOMMAND:
		CL_AddPlayerCommandText( cl_localClient, VMA(1) );
		return 0;
	case CG_ADDCOMMAND:
		CL_AddCgameCommand( VMA(1) );
		return 0;
	case CG_REMOVECOMMAND:
		Cmd_RemoveCommandSafe( VMA(1) );
		return 0;
	case CG_SENDCLIENTCOMMAND:
		CL_AddReliableCommand(VMA(1), qfalse);
		return 0;
	case CG_UPDATESCREEN:
		// the loading screen belongs to the primary; an extra client
		// starts up behind the game that is already running
		if ( cl_localClient > 0 ) {
			return 0;
		}
		// this is used during lengthy level loading, so pump message loop
//		Com_EventLoop();	// FIXME: if a server restarts here, BAD THINGS HAPPEN!
// We can't call Com_EventLoop here, a restart will crash and this _does_ happen
// if there is a map change while we are downloading at pk3.
// ZOID
		SCR_UpdateScreen();
		return 0;
	case CG_CM_LOADMAP:
		CL_CM_LoadMap( VMA(1) );
		return 0;
	case CG_CM_NUMINLINEMODELS:
		return CM_NumInlineModels();
	case CG_CM_INLINEMODEL:
		return CM_InlineModel( args[1] );
	case CG_CM_TEMPBOXMODEL:
		return CM_TempBoxModel( VMA(1), VMA(2), /*int capsule*/ qfalse );
	case CG_CM_TEMPCAPSULEMODEL:
		return CM_TempBoxModel( VMA(1), VMA(2), /*int capsule*/ qtrue );
	case CG_CM_POINTCONTENTS:
		return CM_PointContents( VMA(1), args[2] );
	case CG_CM_TRANSFORMEDPOINTCONTENTS:
		return CM_TransformedPointContents( VMA(1), args[2], VMA(3), VMA(4) );
	case CG_CM_BOXTRACE:
		CM_BoxTrace( VMA(1), VMA(2), VMA(3), VMA(4), VMA(5), args[6], args[7], /*int capsule*/ qfalse );
		return 0;
	case CG_CM_CAPSULETRACE:
		CM_BoxTrace( VMA(1), VMA(2), VMA(3), VMA(4), VMA(5), args[6], args[7], /*int capsule*/ qtrue );
		return 0;
	case CG_CM_TRANSFORMEDBOXTRACE:
		CM_TransformedBoxTrace( VMA(1), VMA(2), VMA(3), VMA(4), VMA(5), args[6], args[7], VMA(8), VMA(9), /*int capsule*/ qfalse );
		return 0;
	case CG_CM_TRANSFORMEDCAPSULETRACE:
		CM_TransformedBoxTrace( VMA(1), VMA(2), VMA(3), VMA(4), VMA(5), args[6], args[7], VMA(8), VMA(9), /*int capsule*/ qtrue );
		return 0;
	case CG_CM_MARKFRAGMENTS:
		return re.MarkFragments( args[1], VMA(2), VMA(3), args[4], VMA(5), args[6], VMA(7) );
	case CG_S_STARTSOUND:
		if ( cl_localClient > 0 ) {
			// There is one listener, the primary. The extra players still
			// hear their own sounds (weapon, jumps, pain) at full volume;
			// the rest of the world reaches them through the primary's ears.
			if ( args[2] == clc.clientNum ) {
				S_StartLocalSound( args[4], args[3] );
			}
			return 0;
		}
		S_StartSound( VMA(1), args[2], args[3], args[4] );
		return 0;
	case CG_S_STARTLOCALSOUND:
		S_StartLocalSound( args[1], args[2] );
		return 0;
	case CG_S_CLEARLOOPINGSOUNDS:
		if ( cl_localClient == 0 ) {
			S_ClearLoopingSounds(args[1]);
		}
		return 0;
	case CG_S_ADDLOOPINGSOUND:
		if ( cl_localClient == 0 ) {
			S_AddLoopingSound( args[1], VMA(2), VMA(3), args[4] );
		}
		return 0;
	case CG_S_ADDREALLOOPINGSOUND:
		if ( cl_localClient == 0 ) {
			S_AddRealLoopingSound( args[1], VMA(2), VMA(3), args[4] );
		}
		return 0;
	case CG_S_STOPLOOPINGSOUND:
		if ( cl_localClient == 0 ) {
			S_StopLoopingSound( args[1] );
		}
		return 0;
	case CG_S_UPDATEENTITYPOSITION:
		if ( cl_localClient == 0 ) {
			S_UpdateEntityPosition( args[1], VMA(2) );
		}
		return 0;
	case CG_S_RESPATIALIZE:
		if ( cl_localClient == 0 ) {
			S_Respatialize( args[1], VMA(2), VMA(3), args[4] );
		}
		return 0;
	case CG_S_REGISTERSOUND:
		return S_RegisterSound( VMA(1), args[2] );
	case CG_S_STARTBACKGROUNDTRACK:
		if ( cl_localClient == 0 ) {
			S_StartBackgroundTrack( VMA(1), VMA(2) );
		}
		return 0;
	case CG_R_LOADWORLDMAP:
		CL_LoadWorldMap( VMA(1) );
		return 0; 
	case CG_R_REGISTERMODEL:
		return re.RegisterModel( VMA(1) );
	case CG_R_REGISTERSKIN:
		return re.RegisterSkin( VMA(1) );
	case CG_R_REGISTERSHADER:
		return re.RegisterShader( VMA(1) );
	case CG_R_REGISTERSHADERNOMIP:
		return re.RegisterShaderNoMip( VMA(1) );
	case CG_R_REGISTERFONT:
		re.RegisterFont( VMA(1), args[2], VMA(3));
		return 0;
	case CG_R_CLEARSCENE:
		re.ClearScene();
		return 0;
	case CG_R_ADDREFENTITYTOSCENE:
		re.AddRefEntityToScene( VMA(1) );
		return 0;
	case CG_R_ADDPOLYTOSCENE:
		re.AddPolyToScene( args[1], args[2], VMA(3), 1 );
		return 0;
	case CG_R_ADDPOLYSTOSCENE:
		re.AddPolyToScene( args[1], args[2], VMA(3), args[4] );
		return 0;
	case CG_R_LIGHTFORPOINT:
		return re.LightForPoint( VMA(1), VMA(2), VMA(3), VMA(4) );
	case CG_R_ADDLIGHTTOSCENE:
		re.AddLightToScene( VMA(1), VMF(2), VMF(3), VMF(4), VMF(5) );
		return 0;
	case CG_R_ADDADDITIVELIGHTTOSCENE:
		re.AddAdditiveLightToScene( VMA(1), VMF(2), VMF(3), VMF(4), VMF(5) );
		return 0;
	case CG_R_RENDERSCENE:
		CL_RenderScene( VMA(1) );
		return 0;
	case CG_R_SETCOLOR:
		re.SetColor( VMA(1) );
		return 0;
	case CG_R_DRAWSTRETCHPIC:
		CL_DrawStretchPic( VMF(1), VMF(2), VMF(3), VMF(4), VMF(5), VMF(6), VMF(7), VMF(8), args[9] );
		return 0;
	case CG_R_MODELBOUNDS:
		re.ModelBounds( args[1], VMA(2), VMA(3) );
		return 0;
	case CG_R_LERPTAG:
		return re.LerpTag( VMA(1), args[2], args[3], args[4], VMF(5), VMA(6) );
	case CG_GETGLCONFIG:
		CL_GetGlconfig( VMA(1) );
		return 0;
	case CG_GETGAMESTATE:
		CL_GetGameState( VMA(1) );
		return 0;
	case CG_GETCURRENTSNAPSHOTNUMBER:
		CL_GetCurrentSnapshotNumber( VMA(1), VMA(2) );
		return 0;
	case CG_GETSNAPSHOT:
		return CL_GetSnapshot( args[1], VMA(2) );
	case CG_GETSERVERCOMMAND:
		return CL_GetServerCommand( args[1] );
	case CG_GETCURRENTCMDNUMBER:
		return CL_GetCurrentCmdNumber();
	case CG_GETUSERCMD:
		return CL_GetUserCmd( args[1], VMA(2) );
	case CG_SETUSERCMDVALUE:
		CL_SetUserCmdValue( args[1], VMF(2) );
		return 0;
	case CG_MEMORY_REMAINING:
		return Hunk_MemoryRemaining();
  case CG_KEY_ISDOWN:
		return Key_IsDown( args[1] );
  case CG_KEY_GETCATCHER:
		return Key_GetCatcher();
  case CG_KEY_SETCATCHER:
		// Don't allow the cgame module to close the console
		Key_SetCatcher( args[1] | ( Key_GetCatcher( ) & KEYCATCH_CONSOLE ) );
    return 0;
  case CG_KEY_GETKEY:
		return Key_GetKey( VMA(1) );



	case CG_MEMSET:
		Com_Memset( VMA(1), args[2], args[3] );
		return 0;
	case CG_MEMCPY:
		Com_Memcpy( VMA(1), VMA(2), args[3] );
		return 0;
	case CG_STRNCPY:
		strncpy( VMA(1), VMA(2), args[3] );
		return args[1];
	case CG_SIN:
		return FloatAsInt( sin( VMF(1) ) );
	case CG_COS:
		return FloatAsInt( cos( VMF(1) ) );
	case CG_ATAN2:
		return FloatAsInt( atan2( VMF(1), VMF(2) ) );
	case CG_SQRT:
		return FloatAsInt( sqrt( VMF(1) ) );
	case CG_FLOOR:
		return FloatAsInt( floor( VMF(1) ) );
	case CG_CEIL:
		return FloatAsInt( ceil( VMF(1) ) );
	case CG_ACOS:
		return FloatAsInt( Q_acos( VMF(1) ) );

	case CG_PC_ADD_GLOBAL_DEFINE:
		return botlib_export->PC_AddGlobalDefine( VMA(1) );
	case CG_PC_LOAD_SOURCE:
		return botlib_export->PC_LoadSourceHandle( VMA(1) );
	case CG_PC_FREE_SOURCE:
		return botlib_export->PC_FreeSourceHandle( args[1] );
	case CG_PC_READ_TOKEN:
		return botlib_export->PC_ReadTokenHandle( args[1], VMA(2) );
	case CG_PC_SOURCE_FILE_AND_LINE:
		return botlib_export->PC_SourceFileAndLine( args[1], VMA(2), VMA(3) );

	case CG_S_STOPBACKGROUNDTRACK:
		if ( cl_localClient == 0 ) {
			S_StopBackgroundTrack();
		}
		return 0;

	case CG_REAL_TIME:
		return Com_RealTime( VMA(1) );
	case CG_SNAPVECTOR:
		Q_SnapVector(VMA(1));
		return 0;

	case CG_CIN_PLAYCINEMATIC:
	  return CIN_PlayCinematic(VMA(1), args[2], args[3], args[4], args[5], args[6]);

	case CG_CIN_STOPCINEMATIC:
	  return CIN_StopCinematic(args[1]);

	case CG_CIN_RUNCINEMATIC:
	  return CIN_RunCinematic(args[1]);

	case CG_CIN_DRAWCINEMATIC:
	  CIN_DrawCinematic(args[1]);
	  return 0;

	case CG_CIN_SETEXTENTS:
	  CIN_SetExtents(args[1], args[2], args[3], args[4], args[5]);
	  return 0;

	case CG_R_REMAP_SHADER:
		re.RemapShader( VMA(1), VMA(2), VMA(3) );
		return 0;

/*
	case CG_LOADCAMERA:
		return loadCamera(VMA(1));

	case CG_STARTCAMERA:
		startCamera(args[1]);
		return 0;

	case CG_GETCAMERAINFO:
		return getCameraInfo(args[1], VMA(2), VMA(3));
*/
	case CG_GET_ENTITY_TOKEN:
		return re.GetEntityToken( VMA(1), args[2] );
	case CG_R_INPVS:
		return re.inPVS( VMA(1), VMA(2) );

	default:
	        assert(0);
		Com_Error( ERR_DROP, "Bad cgame system trap: %ld", (long int) args[0] );
	}
	return 0;
}


/*
====================
CL_InitCGame

Should only be called by CL_StartHunkUsers
====================
*/
void CL_InitCGame( void ) {
	const char			*info;
	const char			*mapname;
	int					t1, t2;
	vmInterpret_t		interpret;

	t1 = Sys_Milliseconds();

	// put away the console
	if ( cl_localClient == 0 ) {
		Con_Close();
	}

	// find the current mapname
	info = cl.gameState.stringData + cl.gameState.stringOffsets[ CS_SERVERINFO ];
	mapname = Info_ValueForKey( info, "mapname" );
	Com_sprintf( cl.mapname, sizeof( cl.mapname ), "maps/%s.bsp", mapname );

	// load the dll or bytecode
	interpret = Cvar_VariableValue("vm_cgame");
	if(cl_connectedToPureServer)
	{
		// if sv_pure is set we only allow qvms to be loaded
		if(interpret != VMI_COMPILED && interpret != VMI_BYTECODE)
			interpret = VMI_COMPILED;
	}

	if ( cl_numLocalClients > 1 || cl_localClient > 0 ) {
		// split-screen: each local client runs a cgame of its own
		cgvm = VM_CreateInstance( "cgame", CL_CgameSystemCalls, interpret );
	} else {
		cgvm = VM_Create( "cgame", CL_CgameSystemCalls, interpret );
	}
	if ( !cgvm ) {
		Com_Error( ERR_DROP, "VM_Create on cgame failed" );
	}
	clc.state = CA_LOADING;

	// init for this gamestate
	// use the lastExecutedServerCommand instead of the serverCommandSequence
	// otherwise server commands sent just before a gamestate are dropped
	VM_Call( cgvm, CG_INIT, clc.serverMessageSequence, clc.lastExecutedServerCommand, clc.clientNum );

	// reset any CVAR_CHEAT cvars registered by cgame
	if ( !clc.demoplaying && !cl_connectedToCheatServer )
		Cvar_SetCheatState();

	// we will send a usercmd this frame, which
	// will cause the server to send us the first snapshot
	clc.state = CA_PRIMED;

	t2 = Sys_Milliseconds();

	Com_Printf( "CL_InitCGame: %5.2f seconds\n", (t2-t1)/1000.0 );

	// have the renderer touch all its images, so they are present
	// on the card even if the driver does deferred loading
	re.EndRegistration();

	// make sure everything is paged in
	if (cl_localClient == 0 && !Sys_LowPhysicalMemory()) {
		Com_TouchMemory();
	}

	// clear anything that got printed
	Con_ClearNotify ();
}


/*
====================
CL_GameCommand

See if the current console command is claimed by the cgame
====================
*/
qboolean CL_GameCommand( void ) {
	if ( !cgvm ) {
		return qfalse;
	}

	return VM_Call( cgvm, CG_CONSOLE_COMMAND );
}



/*
=====================
CL_CGameRendering
=====================
*/
void CL_CGameRendering( stereoFrame_t stereo ) {
	VM_Call( cgvm, CG_DRAW_ACTIVE_FRAME, cl.serverTime, stereo, clc.demoplaying );
	VM_Debug( 0 );
}


/*
=================
CL_AdjustTimeDelta

Adjust the clients view of server time.

We attempt to have cl.serverTime exactly equal the server's view
of time plus the timeNudge, but with variable latencies over
the internet it will often need to drift a bit to match conditions.

Our ideal time would be to have the adjusted time approach, but not pass,
the very latest snapshot.

Adjustments are only made when a new snapshot arrives with a rational
latency, which keeps the adjustment process framerate independent and
prevents massive overadjustment during times of significant packet loss
or bursted delayed packets.
=================
*/

#define	RESET_TIME	500

void CL_AdjustTimeDelta( void ) {
	int		newDelta;
	int		deltaDelta;

	cl.newSnapshots = qfalse;

	// the delta never drifts when replaying a demo
	if ( clc.demoplaying ) {
		return;
	}

	newDelta = cl.snap.serverTime - cls.realtime;
	deltaDelta = abs( newDelta - cl.serverTimeDelta );

	if ( deltaDelta > RESET_TIME ) {
		cl.serverTimeDelta = newDelta;
		cl.oldServerTime = cl.snap.serverTime;	// FIXME: is this a problem for cgame?
		cl.serverTime = cl.snap.serverTime;
		if ( cl_showTimeDelta->integer ) {
			Com_Printf( "<RESET> " );
		}
	} else if ( deltaDelta > 100 ) {
		// fast adjust, cut the difference in half
		if ( cl_showTimeDelta->integer ) {
			Com_Printf( "<FAST> " );
		}
		cl.serverTimeDelta = ( cl.serverTimeDelta + newDelta ) >> 1;
	} else {
		// slow drift adjust, only move 1 or 2 msec

		// if any of the frames between this and the previous snapshot
		// had to be extrapolated, nudge our sense of time back a little
		// the granularity of +1 / -2 is too high for timescale modified frametimes
		if ( com_timescale->value == 0 || com_timescale->value == 1 ) {
			if ( cl.extrapolatedSnapshot ) {
				cl.extrapolatedSnapshot = qfalse;
				cl.serverTimeDelta -= 2;
			} else {
				// otherwise, move our sense of time forward to minimize total latency
				cl.serverTimeDelta++;
			}
		}
	}

	if ( cl_showTimeDelta->integer ) {
		Com_Printf( "%i ", cl.serverTimeDelta );
	}
}


/*
==================
CL_FirstSnapshot
==================
*/
void CL_FirstSnapshot( void ) {
	// ignore snapshots that don't have entities
	if ( cl.snap.snapFlags & SNAPFLAG_NOT_ACTIVE ) {
		return;
	}
	clc.state = CA_ACTIVE;

	// set the timedelta so we are exactly on this first frame
	cl.serverTimeDelta = cl.snap.serverTime - cls.realtime;
	cl.oldServerTime = cl.snap.serverTime;

	clc.timeDemoBaseTime = cl.snap.serverTime;

	// if this is the first frame of active play,
	// execute the contents of activeAction now
	// this is to allow scripting a timedemo to start right
	// after loading
	if ( cl_activeAction->string[0] ) {
		Cbuf_AddText( cl_activeAction->string );
		Cvar_Set( "activeAction", "" );
	}

#ifdef USE_MUMBLE
	if ((cl_useMumble->integer) && !mumble_islinked()) {
		int ret = mumble_link(CLIENT_WINDOW_TITLE);
		Com_Printf("Mumble: Linking to Mumble application %s\n", ret==0?"ok":"failed");
	}
#endif

#ifdef USE_VOIP
	if (!clc.voipCodecInitialized) {
		int i;
		int error;

		clc.opusEncoder = opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &error);

		if ( error ) {
			Com_DPrintf("VoIP: Error opus_encoder_create %d\n", error);
			return;
		}

		for (i = 0; i < MAX_CLIENTS; i++) {
			clc.opusDecoder[i] = opus_decoder_create(48000, 1, &error);
			if ( error ) {
				Com_DPrintf("VoIP: Error opus_decoder_create(%d) %d\n", i, error);
				return;
			}
			clc.voipIgnore[i] = qfalse;
			clc.voipGain[i] = 1.0f;
		}
		clc.voipCodecInitialized = qtrue;
		clc.voipMuteAll = qfalse;
		Cmd_AddCommand ("voip", CL_Voip_f);
		Cvar_Set("cl_voipSendTarget", "spatial");
		Com_Memset(clc.voipTargets, ~0, sizeof(clc.voipTargets));
	}
#endif
}

/*
==================
CL_SetCGameTime
==================
*/
void CL_SetCGameTime( void ) {
	// getting a valid frame message ends the connection process
	if ( clc.state != CA_ACTIVE ) {
		if ( clc.state != CA_PRIMED ) {
			return;
		}
		if ( clc.demoplaying ) {
			// we shouldn't get the first snapshot on the same frame
			// as the gamestate, because it causes a bad time skip
			if ( !clc.firstDemoFrameSkipped ) {
				clc.firstDemoFrameSkipped = qtrue;
				return;
			}
			CL_ReadDemoMessage();
		}
		if ( cl.newSnapshots ) {
			cl.newSnapshots = qfalse;
			CL_FirstSnapshot();
		}
		if ( clc.state != CA_ACTIVE ) {
			return;
		}
	}	

	// if we have gotten to this point, cl.snap is guaranteed to be valid
	if ( !cl.snap.valid ) {
		Com_Error( ERR_DROP, "CL_SetCGameTime: !cl.snap.valid" );
	}

	// allow pause in single player
	if ( sv_paused->integer && CL_CheckPaused() && com_sv_running->integer ) {
		// paused
		return;
	}

	if ( cl.snap.serverTime < cl.oldFrameServerTime ) {
		Com_Error( ERR_DROP, "cl.snap.serverTime < cl.oldFrameServerTime" );
	}
	cl.oldFrameServerTime = cl.snap.serverTime;


	// get our current view of time

	if ( clc.demoplaying && cl_freezeDemo->integer ) {
		// cl_freezeDemo is used to lock a demo in place for single frame advances

	} else {
		// cl_timeNudge is a user adjustable cvar that allows more
		// or less latency to be added in the interest of better 
		// smoothness or better responsiveness.
		int tn;
		
		tn = cl_timeNudge->integer;
		if (tn<-30) {
			tn = -30;
		} else if (tn>30) {
			tn = 30;
		}

		cl.serverTime = cls.realtime + cl.serverTimeDelta - tn;

		// guarantee that time will never flow backwards, even if
		// serverTimeDelta made an adjustment or cl_timeNudge was changed
		if ( cl.serverTime < cl.oldServerTime ) {
			cl.serverTime = cl.oldServerTime;
		}
		cl.oldServerTime = cl.serverTime;

		// note if we are almost past the latest frame (without timeNudge),
		// so we will try and adjust back a bit when the next snapshot arrives
		if ( cls.realtime + cl.serverTimeDelta >= cl.snap.serverTime - 5 ) {
			cl.extrapolatedSnapshot = qtrue;
		}
	}

	// if we have gotten new snapshots, drift serverTimeDelta
	// don't do this every frame, or a period of packet loss would
	// make a huge adjustment
	if ( cl.newSnapshots ) {
		CL_AdjustTimeDelta();
	}

	if ( !clc.demoplaying ) {
		return;
	}

	// if we are playing a demo back, we can just keep reading
	// messages from the demo file until the cgame definitely
	// has valid snapshots to interpolate between

	// a timedemo will always use a deterministic set of time samples
	// no matter what speed machine it is run on,
	// while a normal demo may have different time samples
	// each time it is played back
	if ( cl_timedemo->integer ) {
		int now = Sys_Milliseconds( );
		int frameDuration;

		if (!clc.timeDemoStart) {
			clc.timeDemoStart = clc.timeDemoLastFrame = now;
			clc.timeDemoMinDuration = INT_MAX;
			clc.timeDemoMaxDuration = 0;
		}

		frameDuration = now - clc.timeDemoLastFrame;
		clc.timeDemoLastFrame = now;

		// Ignore the first measurement as it'll always be 0
		if( clc.timeDemoFrames > 0 )
		{
			if( frameDuration > clc.timeDemoMaxDuration )
				clc.timeDemoMaxDuration = frameDuration;

			if( frameDuration < clc.timeDemoMinDuration )
				clc.timeDemoMinDuration = frameDuration;

			// 255 ms = about 4fps
			if( frameDuration > UCHAR_MAX )
				frameDuration = UCHAR_MAX;

			clc.timeDemoDurations[ ( clc.timeDemoFrames - 1 ) %
				MAX_TIMEDEMO_DURATIONS ] = frameDuration;
		}

		clc.timeDemoFrames++;
		cl.serverTime = clc.timeDemoBaseTime + clc.timeDemoFrames * 50;
	}

	while ( cl.serverTime >= cl.snap.serverTime ) {
		// feed another messag, which should change
		// the contents of cl.snap
		CL_ReadDemoMessage();
		if ( clc.state != CA_ACTIVE ) {
			return;		// end of demo
		}
	}

}



