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
// cl_padmenu.c -- controller settings screen
//
// The menus come from the game data (ui.qvm) and know nothing about stick
// or gyro aiming, and they differ between games. This screen is drawn by
// the engine instead, so it is the same everywhere and works over the
// menus and over a running game - where the sticks and gyro stay live, so
// a change can be felt while it is being made.

#include "client.h"

typedef struct {
	const char	*label;
	const char	*cvar;
	const char	*def;		// used if nothing registered the cvar yet
	float		min, max, step;
	qboolean	toggle;
} padMenuItem_t;

static const padMenuItem_t padMenuItems[] = {
	{ "Stick aim speed",          "j_lookSensitivity",  "0.3",  0.05f, 1.5f, 0.05f, qfalse },
	{ "Stick aim precision",      "j_lookCurve",        "1.5",  1.0f,  3.0f, 0.25f, qfalse },
	{ "Invert stick aim",         "j_lookInvert",       "0",    0, 1, 1, qtrue },
	{ "Stick dead zone",          "joy_threshold",      "0.15", 0.05f, 0.5f, 0.05f, qfalse },
	{ "Gyro aiming",              "in_gyromouse",       "0",    0, 1, 1, qtrue },
	{ "Gyro sensitivity (controller)", "in_gyroSensitivity",       "1", 0.25f, 12.0f, 0.25f, qfalse },
	{ "Gyro sensitivity (single Joy-Con)", "in_gyroSensitivitySingle", "1", 0.25f, 12.0f, 0.25f, qfalse },
	{ "Single Joy-Con: stick turns", "in_joyconStickTurn", "1", 0, 1, 1, qtrue },
	{ "Controller screen at game start", "in_controllerPrompt", "1", 0, 1, 1, qtrue },
	{ "Field of view",            "cg_fov",             "90",   60.0f, 130.0f, 5.0f, qfalse },
	{ "HUD size",                 "cl_hudScale",        "1",    0.5f, 1.0f, 0.05f, qfalse },
	{ "2 players: side by side",  "cl_splitscreenVertical", "0", 0, 1, 1, qtrue },
	{ "Players can hurt each other", "sv_splitscreenPvP", "1", 0, 1, 1, qtrue },
	{ "Show controller debug info", "in_padDebug",     "0",    0, 1, 1, qtrue },
};

#define NUM_PADMENU_ITEMS	( (int)ARRAY_LEN( padMenuItems ) )

static qboolean	padMenuActive = qfalse;
static int		padMenuCursor = 0;

/*
===============
CL_PadMenuActive
===============
*/
qboolean CL_PadMenuActive( void ) {
	return padMenuActive;
}

/*
===============
CL_PadMenu_f

padsettings: open or close the controller settings
===============
*/
void CL_PadMenu_f( void ) {
	int i;

	padMenuActive = !padMenuActive;

	if ( padMenuActive ) {
		for ( i = 0; i < NUM_PADMENU_ITEMS; i++ ) {
			// make sure it exists; whoever owns the cvar decides if it is saved
			Cvar_Get( padMenuItems[i].cvar, padMenuItems[i].def, 0 );
		}
	}
}

static void CL_PadMenuAdjust( int dir ) {
	const padMenuItem_t	*item = &padMenuItems[padMenuCursor];
	float				value = Cvar_VariableValue( item->cvar );

	if ( item->toggle ) {
		Cvar_Set( item->cvar, value ? "0" : "1" );
		return;
	}

	// step on a clean grid so values don't drift
	value = item->step * (float)(int)( value / item->step + ( dir > 0 ? 1.5f : -0.5f ) );
	if ( value < item->min ) {
		value = item->min;
	}
	if ( value > item->max ) {
		value = item->max;
	}
	Cvar_Set( item->cvar, va( "%.2f", value ) );
}

/*
===============
CL_PadMenuKey

Returns true if the key press was used by the settings screen. Only presses
are taken; releases go on to the game so nothing is left held.
===============
*/
qboolean CL_PadMenuKey( int key, qboolean down ) {
	if ( !padMenuActive || !down ) {
		return qfalse;
	}

	switch ( key ) {
	case K_UPARROW:
	case K_KP_UPARROW:
		padMenuCursor = ( padMenuCursor + NUM_PADMENU_ITEMS - 1 ) % NUM_PADMENU_ITEMS;
		break;
	case K_DOWNARROW:
	case K_KP_DOWNARROW:
		padMenuCursor = ( padMenuCursor + 1 ) % NUM_PADMENU_ITEMS;
		break;
	case K_LEFTARROW:
	case K_KP_LEFTARROW:
		CL_PadMenuAdjust( -1 );
		break;
	case K_RIGHTARROW:
	case K_KP_RIGHTARROW:
	case K_ENTER:
	case K_KP_ENTER:
		CL_PadMenuAdjust( 1 );
		break;
	case K_ESCAPE:
		padMenuActive = qfalse;
		break;
	default:
		// everything else is ignored while the screen is up
		break;
	}

	return qtrue;
}

/*
===============
CL_PadMenuDraw
===============
*/
void CL_PadMenuDraw( void ) {
	static vec4_t	backColor  = { 0.0f, 0.0f, 0.0f, 0.82f };
	static vec4_t	barColor   = { 0.35f, 0.35f, 0.35f, 1.0f };
	static vec4_t	fillColor  = { 1.0f, 0.65f, 0.1f, 1.0f };
	static vec4_t	textColor  = { 0.85f, 0.85f, 0.85f, 1.0f };
	static vec4_t	selColor   = { 1.0f, 1.0f, 1.0f, 1.0f };
	static vec4_t	titleColor = { 1.0f, 0.65f, 0.1f, 1.0f };
	const int		rowHeight = 19;
	const int		boxW = 520;
	const int		boxH = 76 + NUM_PADMENU_ITEMS * rowHeight;
	const int		boxX = ( SCREEN_WIDTH - boxW ) / 2;
	const int		boxY = ( SCREEN_HEIGHT - boxH ) / 2;
	const int		barX = boxX + 316, barW = 134;
	int				i, y;

	if ( !padMenuActive ) {
		return;
	}

	SCR_FillRect( boxX, boxY, boxW, boxH, backColor );
	SCR_DrawStringExt( boxX + 16, boxY + 12, 12, "CONTROLLER SETTINGS", titleColor, qtrue, qfalse );

	y = boxY + 40;
	for ( i = 0; i < NUM_PADMENU_ITEMS; i++, y += rowHeight ) {
		const padMenuItem_t	*item = &padMenuItems[i];
		float				value = Cvar_VariableValue( item->cvar );
		float				*color = ( i == padMenuCursor ) ? selColor : textColor;

		if ( i == padMenuCursor ) {
			SCR_DrawStringExt( boxX + 12, y, 8, ">", fillColor, qtrue, qfalse );
		}
		SCR_DrawStringExt( boxX + 28, y, 8, item->label, color, qtrue, qfalse );

		if ( item->toggle ) {
			SCR_DrawStringExt( barX, y, 8, value ? "On" : "Off", color, qtrue, qfalse );
		} else {
			float frac = ( value - item->min ) / ( item->max - item->min );

			if ( frac < 0.0f ) {
				frac = 0.0f;
			}
			if ( frac > 1.0f ) {
				frac = 1.0f;
			}
			SCR_FillRect( barX, y + 2, barW, 5, barColor );
			SCR_FillRect( barX, y + 2, barW * frac, 5, fillColor );
			SCR_FillRect( barX + barW * frac - 2, y - 1, 4, 11, color );
			SCR_DrawStringExt( barX + barW + 12, y, 8, va( "%.2f", value ), color, qtrue, qfalse );
		}
	}

	SCR_DrawStringExt( boxX + 16, boxY + boxH - 24, 8, "Up/Down select   Left/Right change   B close", textColor, qtrue, qfalse );
}
