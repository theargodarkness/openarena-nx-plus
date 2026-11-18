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

#ifdef USE_LOCAL_HEADERS
#	include "SDL.h"
#else
#	include <SDL.h>
#endif

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <switch.h>

#include "../client/client.h"
#include "../sys/sys_local.h"

static cvar_t *in_keyboardDebug     = NULL;
static char keyboardShiftMap[K_LAST_KEY];

static qboolean mouseAvailable = qfalse;
static qboolean mouseActive = qfalse;

static cvar_t *in_mouse             = NULL;
static cvar_t *in_nograb;

static cvar_t *in_gyromouse          = NULL;
static cvar_t *in_gyromouse_pitch    = NULL; // Negative values invert
static cvar_t *in_gyromouse_yaw      = NULL; // Negative values invert
static cvar_t *in_gyromouse_pitch_ui = NULL; // Negative values invert (in-menu)
static cvar_t *in_gyromouse_yaw_ui   = NULL; // Negative values invert (in-menu)
static cvar_t *in_gyromouse_yaw_axis     = NULL; // 0 = .y is yaw, 1 = .z is yaw
static cvar_t *in_gyromouse_debug    = NULL;
static cvar_t *in_gyromouse_yaw_auto = NULL; // 1 = pick the yaw axis from how the pad is held
static s32 prev_touchcount=0;

static cvar_t *in_joystick          = NULL;
static cvar_t *in_joystickThreshold = NULL;
static cvar_t *in_joyconStickRotate = NULL; // rotate the stick of a sideways single Joy-Con
static cvar_t *in_gyroSingleJoyCon  = NULL; // a lone Joy-Con always aims with its gyro
static cvar_t *in_padDebug          = NULL; // print what every pad reports and what it is turned into
static cvar_t *in_padJoin           = NULL; // holding L+R / SL+SR on a spare pad adds a split-screen player
static cvar_t *in_joystickUseAnalog = NULL;

static int vidRestartTime = 0;

static int in_eventTime = 0;

static SDL_Window *SDL_window = NULL;

#define CTRL(a) ((a)-'a'+1)

static void IN_ShowOSK_f( void );

/*
===============
IN_PrintKey
===============
*/
static void IN_PrintKey( const SDL_Keysym *keysym, keyNum_t key, qboolean down )
{
	if( down )
		Com_Printf( "+ " );
	else
		Com_Printf( "  " );

	Com_Printf( "Scancode: 0x%02x(%s) Sym: 0x%02x(%s)",
			keysym->scancode, SDL_GetScancodeName( keysym->scancode ),
			keysym->sym, SDL_GetKeyName( keysym->sym ) );

	if( keysym->mod & KMOD_LSHIFT )   Com_Printf( " KMOD_LSHIFT" );
	if( keysym->mod & KMOD_RSHIFT )   Com_Printf( " KMOD_RSHIFT" );
	if( keysym->mod & KMOD_LCTRL )    Com_Printf( " KMOD_LCTRL" );
	if( keysym->mod & KMOD_RCTRL )    Com_Printf( " KMOD_RCTRL" );
	if( keysym->mod & KMOD_LALT )     Com_Printf( " KMOD_LALT" );
	if( keysym->mod & KMOD_RALT )     Com_Printf( " KMOD_RALT" );
	if( keysym->mod & KMOD_LGUI )     Com_Printf( " KMOD_LGUI" );
	if( keysym->mod & KMOD_RGUI )     Com_Printf( " KMOD_RGUI" );
	if( keysym->mod & KMOD_NUM )      Com_Printf( " KMOD_NUM" );
	if( keysym->mod & KMOD_CAPS )     Com_Printf( " KMOD_CAPS" );
	if( keysym->mod & KMOD_MODE )     Com_Printf( " KMOD_MODE" );
	if( keysym->mod & KMOD_RESERVED ) Com_Printf( " KMOD_RESERVED" );

	Com_Printf( " Q:0x%02x(%s)\n", key, Key_KeynumToString( key ) );
}

#define MAX_CONSOLE_KEYS 16

/*
===============
IN_IsConsoleKey

TODO: If the SDL_Scancode situation improves, use it instead of
      both of these methods
===============
*/
static qboolean IN_IsConsoleKey( keyNum_t key, int character )
{
	typedef struct consoleKey_s
	{
		enum
		{
			QUAKE_KEY,
			CHARACTER
		} type;

		union
		{
			keyNum_t key;
			int character;
		} u;
	} consoleKey_t;

	static consoleKey_t consoleKeys[ MAX_CONSOLE_KEYS ];
	static int numConsoleKeys = 0;
	int i;

	// Only parse the variable when it changes
	if( cl_consoleKeys->modified )
	{
		char *text_p, *token;

		cl_consoleKeys->modified = qfalse;
		text_p = cl_consoleKeys->string;
		numConsoleKeys = 0;

		while( numConsoleKeys < MAX_CONSOLE_KEYS )
		{
			consoleKey_t *c = &consoleKeys[ numConsoleKeys ];
			int charCode = 0;

			token = COM_Parse( &text_p );
			if( !token[ 0 ] )
				break;

			charCode = Com_HexStrToInt( token );

			if( charCode > 0 )
			{
				c->type = CHARACTER;
				c->u.character = charCode;
			}
			else
			{
				c->type = QUAKE_KEY;
				c->u.key = Key_StringToKeynum( token );

				// 0 isn't a key
				if( c->u.key <= 0 )
					continue;
			}

			numConsoleKeys++;
		}
	}

	// If the character is the same as the key, prefer the character
	if( key == character )
		key = 0;

	for( i = 0; i < numConsoleKeys; i++ )
	{
		consoleKey_t *c = &consoleKeys[ i ];

		switch( c->type )
		{
			case QUAKE_KEY:
				if( key && c->u.key == key )
					return qtrue;
				break;

			case CHARACTER:
				if( c->u.character == character )
					return qtrue;
				break;
		}
	}

	return qfalse;
}

/*
===============
IN_TranslateSDLToQ3Key
===============
*/
static keyNum_t IN_TranslateSDLToQ3Key( SDL_Keysym *keysym, qboolean down )
{
	keyNum_t key = 0;

	if( keysym->scancode >= SDL_SCANCODE_1 && keysym->scancode <= SDL_SCANCODE_0 )
	{
		// Always map the number keys as such even if they actually map
		// to other characters (eg, "1" is "&" on an AZERTY keyboard).
		// This is required for SDL before 2.0.6, except on Windows
		// which already had this behavior.
		if( keysym->scancode == SDL_SCANCODE_0 )
			key = '0';
		else
			key = '1' + keysym->scancode - SDL_SCANCODE_1;
	}
	else if( keysym->sym >= SDLK_SPACE && keysym->sym < SDLK_DELETE )
	{
		// These happen to match the ASCII chars
		key = (int)keysym->sym;
	}
	else
	{
		switch( keysym->sym )
		{
			case SDLK_PAGEUP:       key = K_PGUP;          break;
			case SDLK_KP_9:         key = K_KP_PGUP;       break;
			case SDLK_PAGEDOWN:     key = K_PGDN;          break;
			case SDLK_KP_3:         key = K_KP_PGDN;       break;
			case SDLK_KP_7:         key = K_KP_HOME;       break;
			case SDLK_HOME:         key = K_HOME;          break;
			case SDLK_KP_1:         key = K_KP_END;        break;
			case SDLK_END:          key = K_END;           break;
			case SDLK_KP_4:         key = K_KP_LEFTARROW;  break;
			case SDLK_LEFT:         key = K_LEFTARROW;     break;
			case SDLK_KP_6:         key = K_KP_RIGHTARROW; break;
			case SDLK_RIGHT:        key = K_RIGHTARROW;    break;
			case SDLK_KP_2:         key = K_KP_DOWNARROW;  break;
			case SDLK_DOWN:         key = K_DOWNARROW;     break;
			case SDLK_KP_8:         key = K_KP_UPARROW;    break;
			case SDLK_UP:           key = K_UPARROW;       break;
			case SDLK_ESCAPE:       key = K_ESCAPE;        break;
			case SDLK_KP_ENTER:     key = K_KP_ENTER;      break;
			case SDLK_RETURN:       key = K_ENTER;         break;
			case SDLK_TAB:          key = K_TAB;           break;
			case SDLK_F1:           key = K_F1;            break;
			case SDLK_F2:           key = K_F2;            break;
			case SDLK_F3:           key = K_F3;            break;
			case SDLK_F4:           key = K_F4;            break;
			case SDLK_F5:           key = K_F5;            break;
			case SDLK_F6:           key = K_F6;            break;
			case SDLK_F7:           key = K_F7;            break;
			case SDLK_F8:           key = K_F8;            break;
			case SDLK_F9:           key = K_F9;            break;
			case SDLK_F10:          key = K_F10;           break;
			case SDLK_F11:          key = K_F11;           break;
			case SDLK_F12:          key = K_F12;           break;
			case SDLK_F13:          key = K_F13;           break;
			case SDLK_F14:          key = K_F14;           break;
			case SDLK_F15:          key = K_F15;           break;

			case SDLK_BACKSPACE:    key = K_BACKSPACE;     break;
			case SDLK_KP_PERIOD:    key = K_KP_DEL;        break;
			case SDLK_DELETE:       key = K_DEL;           break;
			case SDLK_PAUSE:        key = K_PAUSE;         break;

			case SDLK_LSHIFT:
			case SDLK_RSHIFT:       key = K_SHIFT;         break;

			case SDLK_LCTRL:
			case SDLK_RCTRL:        key = K_CTRL;          break;

#ifdef __APPLE__
			case SDLK_RGUI:
			case SDLK_LGUI:         key = K_COMMAND;       break;
#else
			case SDLK_RGUI:
			case SDLK_LGUI:         key = K_SUPER;         break;
#endif

			case SDLK_RALT:
			case SDLK_LALT:         key = K_ALT;           break;

			case SDLK_KP_5:         key = K_KP_5;          break;
			case SDLK_INSERT:       key = K_INS;           break;
			case SDLK_KP_0:         key = K_KP_INS;        break;
			case SDLK_KP_MULTIPLY:  key = K_KP_STAR;       break;
			case SDLK_KP_PLUS:      key = K_KP_PLUS;       break;
			case SDLK_KP_MINUS:     key = K_KP_MINUS;      break;
			case SDLK_KP_DIVIDE:    key = K_KP_SLASH;      break;

			case SDLK_MODE:         key = K_MODE;          break;
			case SDLK_HELP:         key = K_HELP;          break;
			case SDLK_PRINTSCREEN:  key = K_PRINT;         break;
			case SDLK_SYSREQ:       key = K_SYSREQ;        break;
			case SDLK_MENU:         key = K_MENU;          break;
			case SDLK_APPLICATION:	key = K_MENU;          break;
			case SDLK_POWER:        key = K_POWER;         break;
			case SDLK_UNDO:         key = K_UNDO;          break;
			case SDLK_SCROLLLOCK:   key = K_SCROLLOCK;     break;
			case SDLK_NUMLOCKCLEAR: key = K_KP_NUMLOCK;    break;
			case SDLK_CAPSLOCK:     key = K_CAPSLOCK;      break;

			default:
				if( !( keysym->sym & SDLK_SCANCODE_MASK ) && keysym->scancode <= 95 )
				{
					// Map Unicode characters to 95 world keys using the key's scan code.
					// FIXME: There aren't enough world keys to cover all the scancodes.
					// Maybe create a map of scancode to quake key at start up and on
					// key map change; allocate world key numbers as needed similar
					// to SDL 1.2.
					key = K_WORLD_0 + (int)keysym->scancode;
				}
				break;
		}
	}

	if( in_keyboardDebug->integer )
		IN_PrintKey( keysym, key, down );

	if( IN_IsConsoleKey( key, 0 ) )
	{
		// Console keys can't be bound or generate characters
		key = K_CONSOLE;
	}

	return key;
}

/*
===============
IN_GobbleMotionEvents
===============
*/
static void IN_GobbleMotionEvents( void )
{
	SDL_Event dummy[ 1 ];
	int val = 0;

	// Gobble any mouse motion events
	SDL_PumpEvents( );
	while( ( val = SDL_PeepEvents( dummy, 1, SDL_GETEVENT,
		SDL_MOUSEMOTION, SDL_MOUSEMOTION ) ) > 0 ) { }

	if ( val < 0 )
		Com_Printf( "IN_GobbleMotionEvents failed: %s\n", SDL_GetError( ) );
}

/*
===============
IN_ActivateMouse
===============
*/
static void IN_ActivateMouse( qboolean isFullscreen )
{
	if (!mouseAvailable || !SDL_WasInit( SDL_INIT_VIDEO ) )
		return;

	if( !mouseActive )
	{
		SDL_SetRelativeMouseMode( SDL_TRUE );
		SDL_SetWindowGrab( SDL_window, SDL_TRUE );

		IN_GobbleMotionEvents( );
	}

	// in_nograb makes no sense in fullscreen mode
	if( !isFullscreen )
	{
		if( in_nograb->modified || !mouseActive )
		{
			if( in_nograb->integer ) {
				SDL_SetRelativeMouseMode( SDL_FALSE );
				SDL_SetWindowGrab( SDL_window, SDL_FALSE );
			} else {
				SDL_SetRelativeMouseMode( SDL_TRUE );
				SDL_SetWindowGrab( SDL_window, SDL_TRUE );
			}

			in_nograb->modified = qfalse;
		}
	}

	mouseActive = qtrue;
}

/*
===============
IN_DeactivateMouse
===============
*/
static void IN_DeactivateMouse( qboolean isFullscreen )
{
	if( !SDL_WasInit( SDL_INIT_VIDEO ) )
		return;

	// Always show the cursor when the mouse is disabled,
	// but not when fullscreen
	if( !isFullscreen )
		SDL_ShowCursor( SDL_TRUE );

	if( !mouseAvailable )
		return;

	if( mouseActive )
	{
		IN_GobbleMotionEvents( );

		SDL_SetWindowGrab( SDL_window, SDL_FALSE );
		SDL_SetRelativeMouseMode( SDL_FALSE );

		// Don't warp the mouse unless the cursor is within the window
		if( SDL_GetWindowFlags( SDL_window ) & SDL_WINDOW_MOUSE_FOCUS )
			SDL_WarpMouseInWindow( SDL_window, cls.glconfig.vidWidth / 2, cls.glconfig.vidHeight / 2 );

		mouseActive = qfalse;
	}
}

/*
==============
IN_InitKeyMaps
==============
*/
static void IN_InitKeys( void )
{
	int i;
	for (i = 0; i < K_LAST_KEY; i++)
		keyboardShiftMap[i] = i;

	for (i='a' ; i<='z' ; i++)
		keyboardShiftMap[i] = i - 'a' + 'A';

	keyboardShiftMap['1'] = '!';
	keyboardShiftMap['2'] = '@';
	keyboardShiftMap['3'] = '#';
	keyboardShiftMap['4'] = '$';
	keyboardShiftMap['5'] = '%';
	keyboardShiftMap['6'] = '^';
	keyboardShiftMap['7'] = '&';
	keyboardShiftMap['8'] = '*';
	keyboardShiftMap['9'] = '(';
	keyboardShiftMap['0'] = ')';
	keyboardShiftMap['-'] = '_';
	keyboardShiftMap['='] = '+';
	keyboardShiftMap[','] = '<';
	keyboardShiftMap['.'] = '>';
	keyboardShiftMap['/'] = '?';
	keyboardShiftMap[';'] = ':';
	keyboardShiftMap['\''] = '"';
	keyboardShiftMap['['] = '{';
	keyboardShiftMap[']'] = '}';
	keyboardShiftMap['`'] = '~';
	keyboardShiftMap['\\'] = '|';
}

/*
===========================================================================
Native (libnx) controller backend

Controllers are read straight from HID instead of through SDL's joystick
layer. switch-sdl2 only ever exposed player 1 to us, left the last button
state latched when a pad dropped off, and popped the system controller
applet by itself whenever a pad changed type. Reading npads directly lets
us release everything cleanly on disconnect, accept whichever controller
the player actually picked up, and give every split-screen player a pad.

Who plays on which pad:
- One player: every connected pad controls that player, so it never
  matters which slot Horizon put a controller in.
- Split-screen: each player owns one pad. A pad nobody owns takes the first
  free seat when a button is pressed on it. Holding L+R (SL+SR on a
  sideways Joy-Con) for a second on a pad without a seat adds a player.
===========================================================================
*/

#define NX_MAX_PLAYERS   MAX_LOCAL_CLIENTS
#define NX_NUM_SLOTS     ( NX_MAX_PLAYERS + 1 )	// No1..No4 + handheld
#define NX_SLOT_HANDHELD NX_MAX_PLAYERS
#define NX_NUM_BUTTONS   15						// SDL_CONTROLLER_BUTTON_A..DPAD_RIGHT
#define NX_NUM_AXES      6						// SDL_CONTROLLER_AXIS_LEFTX..TRIGGERRIGHT
#define NX_JOIN_HOLD_MSEC    1000
#define NX_JOIN_TIMEOUT_MSEC 3000

typedef struct
{
	HidNpadIdType id;
	PadState pad;
	u32 style;								// active HidNpadStyleTag, 0 = not connected
	HidSixAxisSensorHandle sixaxis[2];
	int numSixaxis;

	int player;								// split-screen player who owns this pad, -1 = nobody
	qboolean buttons[NX_NUM_BUTTONS];		// this frame, in SDL controller layout
	int axes[NX_NUM_AXES];
	qboolean active;						// produced input this frame
	qboolean anyButton;
	qboolean joinHeld;						// L+R / SL+SR are down
	int joinHoldStart;
	u64 debugDown;							// last raw buttons printed by in_padDebug
} nxPad_t;

typedef struct
{
	qboolean buttons[NX_NUM_BUTTONS];		// last state sent to the game
	int oldaaxes[MAX_JOYSTICK_AXIS];
	int gyroSlot;							// pad whose motion sensor aims for this player, -1 = none
	float gyroRemX, gyroRemY;
} nxPlayer_t;

static nxPad_t nx_pads[NX_NUM_SLOTS];
static nxPlayer_t nx_players[NX_MAX_PLAYERS];
static qboolean nx_menuButtons[NX_NUM_BUTTONS];	// last merged state used for menu navigation
static qboolean nx_padsInitialized = qfalse;
static qboolean nx_splitWasOn = qfalse;
static int nx_homeSlot = -1;				// pad the first player was last seen using
static int nx_pendingJoinSlot = -1;			// pad that asked for a new player to be added
static int nx_pendingJoinTime = 0;

// Indexed by SDL_CONTROLLER_BUTTON_*, which is what K_PAD0_A + i and the
// existing binds expect. This reproduces the switch-sdl2 layout the port has
// always used: the "A" slot is the bottom face button (physical B), and so on.
static const u64 nx_buttonsFull[NX_NUM_BUTTONS] = {
	HidNpadButton_B, HidNpadButton_A, HidNpadButton_Y, HidNpadButton_X,
	HidNpadButton_Minus, 0, HidNpadButton_Plus,
	HidNpadButton_StickL, HidNpadButton_StickR,
	HidNpadButton_L, HidNpadButton_R,
	HidNpadButton_Up, HidNpadButton_Down, HidNpadButton_Left, HidNpadButton_Right
};

// Single Joy-Con held sideways: face buttons keep their positions (bottom
// button is still the "A" slot), stick click is start, SL/SR are the
// triggers, and the two buttons on the end are the shoulders.
static const u64 nx_buttonsJoyLeft[NX_NUM_BUTTONS] = {
	HidNpadButton_Left, HidNpadButton_Down, HidNpadButton_Up, HidNpadButton_Right,
	HidNpadButton_Minus, 0, HidNpadButton_StickL,
	0, 0,
	HidNpadButton_L, HidNpadButton_ZL,
	0, 0, 0, 0
};

static const u64 nx_buttonsJoyRight[NX_NUM_BUTTONS] = {
	HidNpadButton_A, HidNpadButton_X, HidNpadButton_B, HidNpadButton_Y,
	HidNpadButton_Plus, 0, HidNpadButton_StickR,
	0, 0,
	HidNpadButton_R, HidNpadButton_ZR,
	0, 0, 0, 0
};

static const char *IN_NXStyleName( u32 style )
{
	switch( style )
	{
		case HidNpadStyleTag_NpadHandheld: return "handheld";
		case HidNpadStyleTag_NpadFullKey:  return "Pro Controller";
		case HidNpadStyleTag_NpadJoyDual:  return "Joy-Con pair";
		case HidNpadStyleTag_NpadJoyLeft:  return "single Joy-Con (L)";
		case HidNpadStyleTag_NpadJoyRight: return "single Joy-Con (R)";
		default:                           return "unknown";
	}
}

static qboolean IN_NXIsSingleJoyCon( u32 style )
{
	return style == HidNpadStyleTag_NpadJoyLeft || style == HidNpadStyleTag_NpadJoyRight;
}

static u32 IN_NXPrimaryStyle( u32 styleSet )
{
	if( styleSet & HidNpadStyleTag_NpadHandheld ) return HidNpadStyleTag_NpadHandheld;
	if( styleSet & HidNpadStyleTag_NpadFullKey )  return HidNpadStyleTag_NpadFullKey;
	if( styleSet & HidNpadStyleTag_NpadJoyDual )  return HidNpadStyleTag_NpadJoyDual;
	if( styleSet & HidNpadStyleTag_NpadJoyLeft )  return HidNpadStyleTag_NpadJoyLeft;
	if( styleSet & HidNpadStyleTag_NpadJoyRight ) return HidNpadStyleTag_NpadJoyRight;
	return 0;
}

static void IN_NXStopSixAxis( nxPad_t *p )
{
	int i;

	for( i = 0; i < p->numSixaxis; i++ )
		hidStopSixAxisSensor( p->sixaxis[i] );
	p->numSixaxis = 0;
}

/*
===============
IN_NXPadChanged

A pad connected, disconnected or changed type (e.g. Joy-Cons slid off the
console, or a pair split into singles). Motion sensor handles are tied to
the id+style, so they have to be fetched again.
===============
*/
static void IN_NXPadChanged( nxPad_t *p, int slot, u32 style )
{
	int i, count;

	IN_NXStopSixAxis( p );

	if( slot == NX_SLOT_HANDHELD )
		Com_Printf( "Controller (handheld): %s\n", style ? "connected" : "disconnected" );
	else if( style )
		Com_Printf( "Controller %d: %s connected\n", slot + 1, IN_NXStyleName( style ) );
	else
		Com_Printf( "Controller %d: disconnected\n", slot + 1 );

	p->style = style;
	p->joinHoldStart = 0;
	if( !style )
		return;

	count = ( style == HidNpadStyleTag_NpadJoyDual ) ? 2 : 1;
	if( R_SUCCEEDED( hidGetSixAxisSensorHandles( p->sixaxis, count, p->id, (HidNpadStyleTag)style ) ) )
	{
		for( i = 0; i < count; i++ )
			hidStartSixAxisSensor( p->sixaxis[i] );
		p->numSixaxis = count;
	}
}

static int IN_NXClampAxis( int v )
{
	if( v > 32767 ) return 32767;
	if( v < -32767 ) return -32767;
	return v;
}

/*
===============
IN_NXReadPad

Translate one pad into the SDL-style button/axis layout the rest of the
input code works with. Y axes are positive-down, like SDL.
===============
*/
static void IN_NXReadPad( nxPad_t *p )
{
	const u64 *map = nx_buttonsFull;
	u64 down = padGetButtons( &p->pad );
	HidAnalogStickState l = padGetStickPos( &p->pad, 0 );
	HidAnalogStickState r = padGetStickPos( &p->pad, 1 );
	qboolean *buttons = p->buttons;
	int *axes = p->axes;
	int i;

	if( p->style == HidNpadStyleTag_NpadJoyLeft )
	{
		map = nx_buttonsJoyLeft;
		if( in_joyconStickRotate->integer )
		{
			// stick "up" points to the player's left
			HidAnalogStickState t = l;
			l.x = -t.y;
			l.y = t.x;
		}
		r.x = r.y = 0;
		axes[SDL_CONTROLLER_AXIS_TRIGGERLEFT]  = ( down & HidNpadButton_LeftSL ) ? 32767 : 0;
		axes[SDL_CONTROLLER_AXIS_TRIGGERRIGHT] = ( down & HidNpadButton_LeftSR ) ? 32767 : 0;
		p->joinHeld = ( down & HidNpadButton_LeftSL ) && ( down & HidNpadButton_LeftSR );
	}
	else if( p->style == HidNpadStyleTag_NpadJoyRight )
	{
		map = nx_buttonsJoyRight;
		// the only stick on a right Joy-Con is the right one; it drives movement
		l = r;
		if( in_joyconStickRotate->integer )
		{
			// stick "up" points to the player's right
			HidAnalogStickState t = l;
			l.x = t.y;
			l.y = -t.x;
		}
		r.x = r.y = 0;
		axes[SDL_CONTROLLER_AXIS_TRIGGERLEFT]  = ( down & HidNpadButton_RightSL ) ? 32767 : 0;
		axes[SDL_CONTROLLER_AXIS_TRIGGERRIGHT] = ( down & HidNpadButton_RightSR ) ? 32767 : 0;
		p->joinHeld = ( down & HidNpadButton_RightSL ) && ( down & HidNpadButton_RightSR );
	}
	else
	{
		axes[SDL_CONTROLLER_AXIS_TRIGGERLEFT]  = ( down & HidNpadButton_ZL ) ? 32767 : 0;
		axes[SDL_CONTROLLER_AXIS_TRIGGERRIGHT] = ( down & HidNpadButton_ZR ) ? 32767 : 0;
		p->joinHeld = ( down & HidNpadButton_L ) && ( down & HidNpadButton_R );
	}

	for( i = 0; i < NX_NUM_BUTTONS; i++ )
	{
		buttons[i] = ( map[i] && ( down & map[i] ) ) ? qtrue : qfalse;
		if( buttons[i] )
			p->anyButton = qtrue;
	}

	if( in_padDebug->integer )
	{
		// raw HID state whenever the buttons change, to see what a pad really sends
		if( down != p->debugDown )
		{
			HidAnalogStickState rl = padGetStickPos( &p->pad, 0 );
			HidAnalogStickState rr = padGetStickPos( &p->pad, 1 );

			Com_Printf( "pad%d %s p%d raw %08x%08x L(%d,%d) R(%d,%d)\n", (int)( p - nx_pads ), IN_NXStyleName( p->style ),
				p->player, (unsigned int)( down >> 32 ), (unsigned int)down, rl.x, rl.y, rr.x, rr.y );
			p->debugDown = down;
		}
	}

	axes[SDL_CONTROLLER_AXIS_LEFTX]  = IN_NXClampAxis( l.x );
	axes[SDL_CONTROLLER_AXIS_LEFTY]  = IN_NXClampAxis( -l.y );
	axes[SDL_CONTROLLER_AXIS_RIGHTX] = IN_NXClampAxis( r.x );
	axes[SDL_CONTROLLER_AXIS_RIGHTY] = IN_NXClampAxis( -r.y );

	if( axes[SDL_CONTROLLER_AXIS_TRIGGERLEFT] || axes[SDL_CONTROLLER_AXIS_TRIGGERRIGHT] )
		p->anyButton = qtrue;

	p->active = p->anyButton;
	for( i = 0; i < 4; i++ )
	{
		if( abs( axes[i] ) > 16384 )
			p->active = qtrue;
	}
}

/*
===============
IN_NXPollPads

Read the current state of every pad. A pad that is not connected reads as
fully released, which is what makes held buttons let go instead of staying
latched.
===============
*/
static void IN_NXPollPads( void )
{
	int slot;

	for( slot = 0; slot < NX_NUM_SLOTS; slot++ )
	{
		nxPad_t *p = &nx_pads[slot];
		u32 style;

		memset( p->buttons, 0, sizeof( p->buttons ) );
		memset( p->axes, 0, sizeof( p->axes ) );
		p->active = p->anyButton = p->joinHeld = qfalse;

		padUpdate( &p->pad );
		style = padIsConnected( &p->pad ) ? IN_NXPrimaryStyle( padGetStyleSet( &p->pad ) ) : 0;

		if( style != p->style )
			IN_NXPadChanged( p, slot, style );

		// Controllers are the console's main input, so they are always read;
		// in_joystick (a desktop setting some menus write to) is ignored.
		if( style )
			IN_NXReadPad( p );

		if( !p->joinHeld )
			p->joinHoldStart = 0;
	}
}

/*
===============
IN_NXSeatTaken

True if a connected pad already plays as this player
===============
*/
static qboolean IN_NXSeatTaken( int player )
{
	int slot;

	for( slot = 0; slot < NX_NUM_SLOTS; slot++ )
	{
		if( nx_pads[slot].style && nx_pads[slot].player == player )
			return qtrue;
	}
	return qfalse;
}

static void IN_NXClaimSeat( int slot, int player )
{
	int i;

	// a pad that went away may still be holding the seat
	for( i = 0; i < NX_NUM_SLOTS; i++ )
	{
		if( nx_pads[i].player == player )
			nx_pads[i].player = -1;
	}

	nx_pads[slot].player = player;
	nx_players[player].gyroSlot = slot;
	Com_Printf( "Player %d is using %s\n", player + 1,
		slot == NX_SLOT_HANDHELD ? "the attached Joy-Cons" : va( "controller %d", slot + 1 ) );
}

/*
===============
IN_NXJoinHeld

True once the join buttons have been held long enough on this pad
===============
*/
static qboolean IN_NXJoinHeld( nxPad_t *p, int now )
{
	if( !p->joinHeld )
		return qfalse;

	if( !p->joinHoldStart )
	{
		p->joinHoldStart = now ? now : 1;
		return qfalse;
	}

	if( now - p->joinHoldStart < NX_JOIN_HOLD_MSEC )
		return qfalse;

	p->joinHoldStart = 0;
	return qtrue;
}

/*
===============
IN_NXAssignPads

Decide which pad plays as which split-screen player (see the top of this
section for the rules)
===============
*/
static void IN_NXAssignPads( void )
{
	int numPlayers = cl_numLocalClients;
	qboolean split = ( numPlayers > 1 );
	qboolean canJoin;
	int now = Sys_Milliseconds( );
	int slot, player;

	// players can only be added to a game we are hosting and playing right now
	canJoin = in_padJoin->integer && com_sv_running->integer && numPlayers < NX_MAX_PLAYERS
		&& CL_LocalClientState( 0 ) == CA_ACTIVE && Key_GetCatcher( ) == 0
		&& nx_pendingJoinSlot < 0;

	if( nx_pendingJoinSlot >= 0 && now - nx_pendingJoinTime > NX_JOIN_TIMEOUT_MSEC )
		nx_pendingJoinSlot = -1;

	if( !split )
	{
		if( nx_splitWasOn )
		{
			for( slot = 0; slot < NX_NUM_SLOTS; slot++ )
				nx_pads[slot].player = -1;
		}
		nx_splitWasOn = qfalse;

		// remember which pad the player is really using; a pad asking to
		// join doesn't count
		for( slot = 0; slot < NX_NUM_SLOTS; slot++ )
		{
			if( nx_pads[slot].active && !nx_pads[slot].joinHeld )
				nx_homeSlot = slot;
		}
		if( nx_homeSlot >= 0 && !nx_pads[nx_homeSlot].style )
			nx_homeSlot = -1;

		for( slot = 0; slot < NX_NUM_SLOTS; slot++ )
		{
			nxPad_t *p = &nx_pads[slot];

			if( !p->style || slot == nx_homeSlot || nx_homeSlot < 0 )
			{
				p->joinHoldStart = 0;
				continue;
			}

			if( canJoin && IN_NXJoinHeld( p, now ) )
			{
				nx_pendingJoinSlot = slot;
				nx_pendingJoinTime = now;
				Cbuf_AddText( "splitscreen 2\n" );
				break;
			}
		}
		return;
	}

	// split-screen is on
	for( slot = 0; slot < NX_NUM_SLOTS; slot++ )
	{
		if( nx_pads[slot].player >= numPlayers )
			nx_pads[slot].player = -1;
	}

	if( !nx_splitWasOn )
	{
		// the first player keeps the pad they were playing on
		nx_splitWasOn = qtrue;
		for( slot = 0; slot < NX_NUM_SLOTS; slot++ )
			nx_pads[slot].player = -1;
		if( nx_homeSlot >= 0 && nx_pads[nx_homeSlot].style && nx_homeSlot != nx_pendingJoinSlot )
			IN_NXClaimSeat( nx_homeSlot, 0 );
	}

	// the pad that asked for the new player gets the new seat
	if( nx_pendingJoinSlot >= 0 )
	{
		nxPad_t *p = &nx_pads[nx_pendingJoinSlot];

		if( p->style && p->player < 0 )
		{
			for( player = numPlayers - 1; player >= 0; player-- )
			{
				if( !IN_NXSeatTaken( player ) )
				{
					IN_NXClaimSeat( nx_pendingJoinSlot, player );
					nx_pendingJoinSlot = -1;
					break;
				}
			}
		}
		else
		{
			nx_pendingJoinSlot = -1;
		}
	}

	for( slot = 0; slot < NX_NUM_SLOTS; slot++ )
	{
		nxPad_t *p = &nx_pads[slot];

		if( !p->style || p->player >= 0 || slot == nx_pendingJoinSlot )
			continue;

		// any button takes the first free seat
		if( p->anyButton )
		{
			for( player = 0; player < numPlayers; player++ )
			{
				if( !IN_NXSeatTaken( player ) )
				{
					IN_NXClaimSeat( slot, player );
					break;
				}
			}
			if( p->player >= 0 )
				continue;
		}

		// every seat is taken: holding the join buttons adds a player
		if( canJoin && IN_NXJoinHeld( p, now ) )
		{
			nx_pendingJoinSlot = slot;
			nx_pendingJoinTime = now;
			Cbuf_AddText( va( "splitscreen %d\n", numPlayers + 1 ) );
			canJoin = qfalse;
		}
	}
}

/*
===============
IN_NXPlayerState

Merge the pads that belong to one player. With split-screen off every pad
belongs to the only player.
===============
*/
static void IN_NXPlayerState( int player, qboolean *buttons, int *axes )
{
	qboolean split = ( cl_numLocalClients > 1 );
	nxPlayer_t *pl = &nx_players[player];
	int slot, i;

	memset( buttons, 0, sizeof( qboolean ) * NX_NUM_BUTTONS );
	memset( axes, 0, sizeof( int ) * NX_NUM_AXES );

	for( slot = 0; slot < NX_NUM_SLOTS; slot++ )
	{
		nxPad_t *p = &nx_pads[slot];

		if( !p->style )
			continue;
		if( split ? ( p->player != player ) : ( player != 0 ) )
			continue;

		for( i = 0; i < NX_NUM_BUTTONS; i++ )
		{
			if( p->buttons[i] )
				buttons[i] = qtrue;
		}
		for( i = 0; i < NX_NUM_AXES; i++ )
		{
			if( abs( p->axes[i] ) > abs( axes[i] ) )
				axes[i] = p->axes[i];
		}

		// gyro follows the pad in use
		if( p->active || pl->gyroSlot < 0 || !nx_pads[pl->gyroSlot].style
			|| ( split && nx_pads[pl->gyroSlot].player != player ) )
			pl->gyroSlot = slot;
	}

	if( pl->gyroSlot >= 0 )
	{
		nxPad_t *g = &nx_pads[pl->gyroSlot];

		if( !g->style || ( split && g->player != player ) )
			pl->gyroSlot = -1;
	}
}

/*
===============
IN_QueuePlayerEvent

Queue an input event for a split-screen player. Events are for the first
player unless an SE_PLAYER event comes right before them.
===============
*/
static void IN_QueuePlayerEvent( int player, sysEventType_t type, int value, int value2 )
{
	if( in_padDebug->integer && type == SE_KEY )
	{
		const char *bind = Key_GetBinding( value );

		Com_Printf( " -> player %d %s %s = %s\n", player + 1, Key_KeynumToString( value ),
			value2 ? "down" : "up", ( bind && bind[0] ) ? bind : "(not bound)" );
	}

	if( player > 0 )
		Com_QueueEvent( in_eventTime, SE_PLAYER, player, 0, 0, NULL );
	Com_QueueEvent( in_eventTime, type, value, value2, 0, NULL );
}

/*
===============
IN_InitPads
===============
*/
static void IN_InitPads( void )
{
	int i;

	// Up to four wireless controllers plus the attached Joy-Cons. Horizontal
	// hold is what makes a lone Joy-Con usable sideways.
	padConfigureInput( NX_MAX_PLAYERS, HidNpadStyleSet_NpadStandard );
	hidSetNpadJoyHoldType( HidNpadJoyHoldType_Horizontal );

	if( nx_padsInitialized )
		return;

	memset( nx_pads, 0, sizeof( nx_pads ) );
	memset( nx_players, 0, sizeof( nx_players ) );
	memset( nx_menuButtons, 0, sizeof( nx_menuButtons ) );

	for( i = 0; i < NX_MAX_PLAYERS; i++ )
	{
		nx_pads[i].id = (HidNpadIdType)( HidNpadIdType_No1 + i );
		padInitialize( &nx_pads[i].pad, nx_pads[i].id );
		nx_players[i].gyroSlot = -1;
	}
	nx_pads[NX_SLOT_HANDHELD].id = HidNpadIdType_Handheld;
	padInitialize( &nx_pads[NX_SLOT_HANDHELD].pad, HidNpadIdType_Handheld );

	for( i = 0; i < NX_NUM_SLOTS; i++ )
		nx_pads[i].player = -1;

	nx_homeSlot = -1;
	nx_pendingJoinSlot = -1;
	nx_splitWasOn = qfalse;
	nx_padsInitialized = qtrue;
}

/*
===============
IN_ShutdownPads
===============
*/
static void IN_ShutdownPads( void )
{
	int i;

	if( !nx_padsInitialized )
		return;

	for( i = 0; i < NX_NUM_SLOTS; i++ )
	{
		IN_NXStopSixAxis( &nx_pads[i] );
		nx_pads[i].style = 0;	// sensors get restarted if we come back up
	}
}

/*
===============
IN_Controllers_f

Opens the system "Controllers" screen: re-pair pads, change grip/order, or
split a Joy-Con pair into two sideways controllers (press SL+SR on each).
===============
*/
static void IN_Controllers_f( void )
{
	HidLaControllerSupportArg args;
	HidLaControllerSupportResultInfo info;
	int maxPlayers = NX_MAX_PLAYERS;
	Result rc;

	if( Cmd_Argc( ) > 1 )
		maxPlayers = atoi( Cmd_Argv( 1 ) );
	if( maxPlayers < 1 )
		maxPlayers = 1;
	if( maxPlayers > NX_MAX_PLAYERS )
		maxPlayers = NX_MAX_PLAYERS;

	hidLaCreateControllerSupportArg( &args );
	args.hdr.player_count_max = maxPlayers;
	if( maxPlayers == 1 )
		args.hdr.enable_single_mode = 1;

	rc = hidLaShowControllerSupportForSystem( &info, &args, false );
	if( R_FAILED( rc ) )
		Com_Printf( "Controller applet failed (0x%x). It is not available in applet (album) mode.\n", (unsigned int)rc );
}


static qboolean KeyToAxisAndSign(int keynum, int *outAxis, int *outSign)
{
	char *bind;

	if (!keynum)
		return qfalse;

	bind = Key_GetBinding(keynum);

	if (!bind || *bind != '+')
		return qfalse;

	*outSign = 0;

	if (Q_stricmp(bind, "+forward") == 0)
	{
		*outAxis = j_forward_axis->integer;
		*outSign = j_forward->value > 0.0f ? 1 : -1;
	}
	else if (Q_stricmp(bind, "+back") == 0)
	{
		*outAxis = j_forward_axis->integer;
		*outSign = j_forward->value > 0.0f ? -1 : 1;
	}
	else if (Q_stricmp(bind, "+moveleft") == 0)
	{
		*outAxis = j_side_axis->integer;
		*outSign = j_side->value > 0.0f ? -1 : 1;
	}
	else if (Q_stricmp(bind, "+moveright") == 0)
	{
		*outAxis = j_side_axis->integer;
		*outSign = j_side->value > 0.0f ? 1 : -1;
	}
	else if (Q_stricmp(bind, "+lookup") == 0)
	{
		*outAxis = j_pitch_axis->integer;
		*outSign = j_pitch->value > 0.0f ? -1 : 1;
	}
	else if (Q_stricmp(bind, "+lookdown") == 0)
	{
		*outAxis = j_pitch_axis->integer;
		*outSign = j_pitch->value > 0.0f ? 1 : -1;
	}
	else if (Q_stricmp(bind, "+left") == 0)
	{
		*outAxis = j_yaw_axis->integer;
		*outSign = j_yaw->value > 0.0f ? 1 : -1;
	}
	else if (Q_stricmp(bind, "+right") == 0)
	{
		*outAxis = j_yaw_axis->integer;
		*outSign = j_yaw->value > 0.0f ? -1 : 1;
	}
	else if (Q_stricmp(bind, "+moveup") == 0)
	{
		*outAxis = j_up_axis->integer;
		*outSign = j_up->value > 0.0f ? 1 : -1;
	}
	else if (Q_stricmp(bind, "+movedown") == 0)
	{
		*outAxis = j_up_axis->integer;
		*outSign = j_up->value > 0.0f ? -1 : 1;
	}

	return *outSign != 0;
}

/*
===============
IN_GamepadMove

Send one player's buttons and sticks to the game
===============
*/
static void IN_GamepadMove( int player, const qboolean *nxButtons, const int *nxAxes, qboolean *changed )
{
	nxPlayer_t *pl = &nx_players[player];
	int i;
	int translatedAxes[MAX_JOYSTICK_AXIS];
	qboolean translatedAxesSet[MAX_JOYSTICK_AXIS];

	// check buttons
	for (i = 0; i < NX_NUM_BUTTONS; i++)
	{
		qboolean pressed = nxButtons[i];

		changed[i] = qfalse;
		if (pressed != pl->buttons[i])
		{
			IN_QueuePlayerEvent(player, SE_KEY, K_PAD0_A + i, pressed);
			pl->buttons[i] = pressed;
			changed[i] = qtrue;
		}
	}

	// must defer translated axes until all real axes are processed
	// must be done this way to prevent a later mapped axis from zeroing out a previous one
	if (in_joystickUseAnalog->integer)
	{
		for (i = 0; i < MAX_JOYSTICK_AXIS; i++)
		{
			translatedAxes[i] = 0;
			translatedAxesSet[i] = qfalse;
		}
	}

	// check axes
	for (i = 0; i < NX_NUM_AXES; i++)
	{
		int axis = nxAxes[i];
		int oldAxis = pl->oldaaxes[i];

		// Smoothly ramp from dead zone to maximum value
		float f = ((float)abs(axis) / 32767.0f - in_joystickThreshold->value) / (1.0f - in_joystickThreshold->value);

		if (f < 0.0f)
			f = 0.0f;

		axis = (int)(32767 * ((axis < 0) ? -f : f));

		if (axis != oldAxis)
		{
			const int negMap[NX_NUM_AXES] = { K_PAD0_LEFTSTICK_LEFT,  K_PAD0_LEFTSTICK_UP,   K_PAD0_RIGHTSTICK_LEFT,  K_PAD0_RIGHTSTICK_UP, 0, 0 };
			const int posMap[NX_NUM_AXES] = { K_PAD0_LEFTSTICK_RIGHT, K_PAD0_LEFTSTICK_DOWN, K_PAD0_RIGHTSTICK_RIGHT, K_PAD0_RIGHTSTICK_DOWN, K_PAD0_LEFTTRIGGER, K_PAD0_RIGHTTRIGGER };

			qboolean posAnalog = qfalse, negAnalog = qfalse;
			int negKey = negMap[i];
			int posKey = posMap[i];

			if (in_joystickUseAnalog->integer)
			{
				int posAxis = 0, posSign = 0, negAxis = 0, negSign = 0;

				// get axes and axes signs for keys if available
				posAnalog = KeyToAxisAndSign(posKey, &posAxis, &posSign);
				negAnalog = KeyToAxisAndSign(negKey, &negAxis, &negSign);

				// positive to negative/neutral -> keyup if axis hasn't yet been set
				if (posAnalog && !translatedAxesSet[posAxis] && oldAxis > 0 && axis <= 0)
				{
					translatedAxes[posAxis] = 0;
					translatedAxesSet[posAxis] = qtrue;
				}

				// negative to positive/neutral -> keyup if axis hasn't yet been set
				if (negAnalog && !translatedAxesSet[negAxis] && oldAxis < 0 && axis >= 0)
				{
					translatedAxes[negAxis] = 0;
					translatedAxesSet[negAxis] = qtrue;
				}

				// negative/neutral to positive -> keydown
				if (posAnalog && axis > 0)
				{
					translatedAxes[posAxis] = axis * posSign;
					translatedAxesSet[posAxis] = qtrue;
				}

				// positive/neutral to negative -> keydown
				if (negAnalog && axis < 0)
				{
					translatedAxes[negAxis] = -axis * negSign;
					translatedAxesSet[negAxis] = qtrue;
				}
			}

			// keyups first so they get overridden by keydowns later

			// positive to negative/neutral -> keyup
			if (!posAnalog && posKey && oldAxis > 0 && axis <= 0)
				IN_QueuePlayerEvent(player, SE_KEY, posKey, qfalse);

			// negative to positive/neutral -> keyup
			if (!negAnalog && negKey && oldAxis < 0 && axis >= 0)
				IN_QueuePlayerEvent(player, SE_KEY, negKey, qfalse);

			// negative/neutral to positive -> keydown
			if (!posAnalog && posKey && oldAxis <= 0 && axis > 0)
				IN_QueuePlayerEvent(player, SE_KEY, posKey, qtrue);

			// positive/neutral to negative -> keydown
			if (!negAnalog && negKey && oldAxis >= 0 && axis < 0)
				IN_QueuePlayerEvent(player, SE_KEY, negKey, qtrue);

			pl->oldaaxes[i] = axis;
		}
	}

	// set translated axes
	if (in_joystickUseAnalog->integer)
	{
		for (i = 0; i < MAX_JOYSTICK_AXIS; i++)
		{
			if (translatedAxesSet[i])
				IN_QueuePlayerEvent(player, SE_JOYSTICK_AXIS, i, translatedAxes[i]);
		}
	}
}

/*
===============
IN_MenuMove

Menus and the console are shared, so any pad can drive them: this turns
the merged state of all pads into mouse movement, clicks and arrow keys.
In-game it only watches the first player's d-pad for the console.
===============
*/
static void IN_MenuMove( const qboolean *playerChanged )
{
	qboolean buttons[NX_NUM_BUTTONS] = { qfalse };
	qboolean changed[NX_NUM_BUTTONS];
	int mousedx = 0, mousedy = 0;
	int slot, i;

	for( slot = 0; slot < NX_NUM_SLOTS; slot++ )
	{
		nxPad_t *p = &nx_pads[slot];

		if( !p->style )
			continue;

		for( i = 0; i < NX_NUM_BUTTONS; i++ )
		{
			if( p->buttons[i] )
				buttons[i] = qtrue;
		}
		if( abs( p->axes[0] ) > abs( mousedx ) )
			mousedx = p->axes[0];
		if( abs( p->axes[1] ) > abs( mousedy ) )
			mousedy = p->axes[1];
	}

	for( i = 0; i < NX_NUM_BUTTONS; i++ )
	{
		changed[i] = ( buttons[i] != nx_menuButtons[i] );
		nx_menuButtons[i] = buttons[i];
	}

	// HACK: in menus, also generate mouse events from some of the buttons and left analog
	//       also translate dpad presses into arrow keys because UI progs don't recognize them
	if (clc.state == CA_DISCONNECTED || clc.state == CA_CINEMATIC || ( Key_GetCatcher( ) & KEYCATCH_UI ))
	{
		// same dead zone ramp the game axes get
		float t = in_joystickThreshold->value;
		float fx = ( (float)abs( mousedx ) / 32767.0f - t ) / ( 1.0f - t );
		float fy = ( (float)abs( mousedy ) / 32767.0f - t ) / ( 1.0f - t );

		mousedx = ( fx > 0.0f ) ? (int)( 32767 * ( mousedx < 0 ? -fx : fx ) ) : 0;
		mousedy = ( fy > 0.0f ) ? (int)( 32767 * ( mousedy < 0 ? -fy : fy ) ) : 0;

		if (abs(mousedx) > 32 || abs(mousedy) > 32)
			Com_QueueEvent( in_eventTime, SE_MOUSE, mousedx / 4096, mousedy / 4096, 0, NULL );

		// NOTE: A-B and X-Y are swapped in switch-sdl2, so keep this in mind when reading this code
		// A and B act as mouse clicks, X as enter (in case you want to control the menu using the d-pad)
		if (changed[SDL_CONTROLLER_BUTTON_A])
			Com_QueueEvent( in_eventTime, SE_KEY, K_MOUSE2, buttons[SDL_CONTROLLER_BUTTON_A], 0, NULL );
		if (changed[SDL_CONTROLLER_BUTTON_B])
			Com_QueueEvent( in_eventTime, SE_KEY, K_MOUSE1, buttons[SDL_CONTROLLER_BUTTON_B], 0, NULL );
		if (changed[SDL_CONTROLLER_BUTTON_Y])
			Com_QueueEvent( in_eventTime, SE_KEY, K_ENTER, buttons[SDL_CONTROLLER_BUTTON_Y], 0, NULL );

		// Y to bring up console
		if (changed[SDL_CONTROLLER_BUTTON_X])
			Com_QueueEvent( in_eventTime, SE_KEY, K_CONSOLE, buttons[SDL_CONTROLLER_BUTTON_X], 0, NULL );

		// show OSK if L is pressed in a menu
		if (changed[SDL_CONTROLLER_BUTTON_LEFTSHOULDER] && buttons[SDL_CONTROLLER_BUTTON_LEFTSHOULDER])
			IN_ShowOSK_f( );
		// use R to delete characters in menu fields
		if (changed[SDL_CONTROLLER_BUTTON_RIGHTSHOULDER] && buttons[SDL_CONTROLLER_BUTTON_RIGHTSHOULDER])
			Com_QueueEvent( in_eventTime, SE_CHAR, CTRL('h'), 0, 0, NULL );
		// use dpad to control menu cursor
		if (changed[SDL_CONTROLLER_BUTTON_DPAD_UP])
			Com_QueueEvent( in_eventTime, SE_KEY, K_UPARROW, buttons[SDL_CONTROLLER_BUTTON_DPAD_UP], 0, NULL );
		if (changed[SDL_CONTROLLER_BUTTON_DPAD_DOWN])
			Com_QueueEvent( in_eventTime, SE_KEY, K_DOWNARROW, buttons[SDL_CONTROLLER_BUTTON_DPAD_DOWN], 0, NULL );
		if (changed[SDL_CONTROLLER_BUTTON_DPAD_LEFT])
			Com_QueueEvent( in_eventTime, SE_KEY, K_LEFTARROW, buttons[SDL_CONTROLLER_BUTTON_DPAD_LEFT], 0, NULL );
		if (changed[SDL_CONTROLLER_BUTTON_DPAD_RIGHT])
			Com_QueueEvent( in_eventTime, SE_KEY, K_RIGHTARROW, buttons[SDL_CONTROLLER_BUTTON_DPAD_RIGHT], 0, NULL );
	}
	else
	{
		// In-game, dpad down to open console
		if (playerChanged[SDL_CONTROLLER_BUTTON_DPAD_DOWN])
			Com_QueueEvent( in_eventTime, SE_KEY, K_CONSOLE, nx_players[0].buttons[SDL_CONTROLLER_BUTTON_DPAD_DOWN], 0, NULL );
	}

	if (Key_GetCatcher( ) & KEYCATCH_CONSOLE)
	{
		// in console: L to show OSK, R to delete
		if (changed[SDL_CONTROLLER_BUTTON_LEFTSHOULDER] && buttons[SDL_CONTROLLER_BUTTON_LEFTSHOULDER])
			IN_ShowOSK_f( );
		if (changed[SDL_CONTROLLER_BUTTON_RIGHTSHOULDER])
			Com_QueueEvent( in_eventTime, SE_CHAR, CTRL('h'), 0, 0, NULL );
		// A to enter
		if (changed[SDL_CONTROLLER_BUTTON_B])
			Com_QueueEvent( in_eventTime, SE_KEY, K_ENTER, buttons[SDL_CONTROLLER_BUTTON_B], 0, NULL );
		// dpad up to scroll through commands
		if (changed[SDL_CONTROLLER_BUTTON_DPAD_UP])
			Com_QueueEvent( in_eventTime, SE_KEY, K_UPARROW, buttons[SDL_CONTROLLER_BUTTON_DPAD_UP], 0, NULL );
		// left/right to scroll console log
		if (changed[SDL_CONTROLLER_BUTTON_DPAD_LEFT])
			Com_QueueEvent( in_eventTime, SE_KEY, K_PGUP, buttons[SDL_CONTROLLER_BUTTON_DPAD_LEFT], 0, NULL );
		if (changed[SDL_CONTROLLER_BUTTON_DPAD_RIGHT])
			Com_QueueEvent( in_eventTime, SE_KEY, K_PGDN, buttons[SDL_CONTROLLER_BUTTON_DPAD_RIGHT], 0, NULL );
	}
}

/*
===============
IN_PadFrame

Everything the controllers do in one frame
===============
*/
static void IN_PadFrame( void )
{
	qboolean buttons[NX_NUM_BUTTONS];
	qboolean changed[NX_NUM_BUTTONS];
	qboolean firstChanged[NX_NUM_BUTTONS] = { qfalse };
	int axes[NX_NUM_AXES];
	int player;

	if( !nx_padsInitialized )
		return;

	IN_NXPollPads( );
	IN_NXAssignPads( );

	for( player = 0; player < NX_MAX_PLAYERS; player++ )
	{
		if( player >= cl_numLocalClients )
		{
			// nobody plays here; forget what was held so nothing is
			// replayed if the seat is filled again
			memset( nx_players[player].buttons, 0, sizeof( nx_players[player].buttons ) );
			memset( nx_players[player].oldaaxes, 0, sizeof( nx_players[player].oldaaxes ) );
			nx_players[player].gyroSlot = -1;
			continue;
		}

		IN_NXPlayerState( player, buttons, axes );
		IN_GamepadMove( player, buttons, axes, changed );

		if( player == 0 )
			memcpy( firstChanged, changed, sizeof( firstChanged ) );
	}

	IN_MenuMove( firstChanged );
}

/*
===============
IN_ProcessEvents
===============
*/
static void IN_ProcessEvents( void )
{
	SDL_Event e;
	keyNum_t key = 0;
	static keyNum_t lastKeyDown = 0;

	if( !SDL_WasInit( SDL_INIT_VIDEO ) )
			return;

	while( SDL_PollEvent( &e ) )
	{
		switch( e.type )
		{
			case SDL_KEYDOWN:
				if ( e.key.repeat && Key_GetCatcher( ) == 0 )
					break;

				if( ( key = IN_TranslateSDLToQ3Key( &e.key.keysym, qtrue ) ) ) {
					Com_QueueEvent( in_eventTime, SE_KEY, key, qtrue, 0, NULL );
					if(key >= 32 && key <= 127) {
						if(e.key.keysym.mod & KMOD_LSHIFT || e.key.keysym.mod & KMOD_RSHIFT)
							key = keyboardShiftMap[key];
						Com_QueueEvent( in_eventTime, SE_CHAR, key, 0, 0, NULL );
					}
				}

				if( key == K_BACKSPACE )
					Com_QueueEvent( in_eventTime, SE_CHAR, CTRL('h'), 0, 0, NULL );
				else if( keys[K_CTRL].down && key >= 'a' && key <= 'z' )
					Com_QueueEvent( in_eventTime, SE_CHAR, CTRL(key), 0, 0, NULL );

				lastKeyDown = key;
				break;

			case SDL_KEYUP:
				if( ( key = IN_TranslateSDLToQ3Key( &e.key.keysym, qfalse ) ) )
					Com_QueueEvent( in_eventTime, SE_KEY, key, qfalse, 0, NULL );

				lastKeyDown = 0;
				break;

			case SDL_TEXTINPUT:
				if( lastKeyDown != K_CONSOLE )
				{
					char *c = e.text.text;

					// Quick and dirty UTF-8 to UTF-32 conversion
					while( *c )
					{
						int utf32 = 0;

						if( ( *c & 0x80 ) == 0 )
							utf32 = *c++;
						else if( ( *c & 0xE0 ) == 0xC0 ) // 110x xxxx
						{
							utf32 |= ( *c++ & 0x1F ) << 6;
							utf32 |= ( *c++ & 0x3F );
						}
						else if( ( *c & 0xF0 ) == 0xE0 ) // 1110 xxxx
						{
							utf32 |= ( *c++ & 0x0F ) << 12;
							utf32 |= ( *c++ & 0x3F ) << 6;
							utf32 |= ( *c++ & 0x3F );
						}
						else if( ( *c & 0xF8 ) == 0xF0 ) // 1111 0xxx
						{
							utf32 |= ( *c++ & 0x07 ) << 18;
							utf32 |= ( *c++ & 0x3F ) << 12;
							utf32 |= ( *c++ & 0x3F ) << 6;
							utf32 |= ( *c++ & 0x3F );
						}
						else
						{
							Com_DPrintf( "Unrecognised UTF-8 lead byte: 0x%x\n", (unsigned int)*c );
							c++;
						}

						if( utf32 != 0 )
						{
							if( IN_IsConsoleKey( 0, utf32 ) )
							{
								Com_QueueEvent( in_eventTime, SE_KEY, K_CONSOLE, qtrue, 0, NULL );
								Com_QueueEvent( in_eventTime, SE_KEY, K_CONSOLE, qfalse, 0, NULL );
							}
							else if( utf32 == '\n' )
							{
								Com_QueueEvent( in_eventTime, SE_KEY, K_ENTER, qtrue, 0, NULL );
								Com_QueueEvent( in_eventTime, SE_KEY, K_ENTER, qfalse, 0, NULL );
							}
							else
							{
								Com_QueueEvent( in_eventTime, SE_CHAR, utf32, 0, 0, NULL );
							}
						}
					}
				}
				break;

			case SDL_MOUSEMOTION:
				if( mouseActive )
				{
					if( !e.motion.xrel && !e.motion.yrel )
						break;
					Com_QueueEvent( in_eventTime, SE_MOUSE, e.motion.xrel, e.motion.yrel, 0, NULL );
				}
				break;

			case SDL_MOUSEBUTTONDOWN:
			case SDL_MOUSEBUTTONUP:
				{
					int b;
					switch( e.button.button )
					{
						case SDL_BUTTON_LEFT:   b = K_MOUSE1;     break;
						case SDL_BUTTON_MIDDLE: b = K_MOUSE3;     break;
						case SDL_BUTTON_RIGHT:  b = K_MOUSE2;     break;
						case SDL_BUTTON_X1:     b = K_MOUSE4;     break;
						case SDL_BUTTON_X2:     b = K_MOUSE5;     break;
						default:                b = K_AUX1 + ( e.button.button - SDL_BUTTON_X2 + 1 ) % 16; break;
					}
					Com_QueueEvent( in_eventTime, SE_KEY, b,
						( e.type == SDL_MOUSEBUTTONDOWN ? qtrue : qfalse ), 0, NULL );
				}
				break;

			case SDL_MOUSEWHEEL:
				if( e.wheel.y > 0 )
				{
					Com_QueueEvent( in_eventTime, SE_KEY, K_MWHEELUP, qtrue, 0, NULL );
					Com_QueueEvent( in_eventTime, SE_KEY, K_MWHEELUP, qfalse, 0, NULL );
				}
				else if( e.wheel.y < 0 )
				{
					Com_QueueEvent( in_eventTime, SE_KEY, K_MWHEELDOWN, qtrue, 0, NULL );
					Com_QueueEvent( in_eventTime, SE_KEY, K_MWHEELDOWN, qfalse, 0, NULL );
				}
				break;

			case SDL_QUIT:
				Cbuf_ExecuteText(EXEC_NOW, "quit Closed window\n");
				break;

			case SDL_WINDOWEVENT:
				switch( e.window.event )
				{
					case SDL_WINDOWEVENT_RESIZED:
						{
							int width, height;

							width = e.window.data1;
							height = e.window.data2;

							// ignore this event on fullscreen
							if( cls.glconfig.isFullscreen )
							{
								break;
							}

							// check if size actually changed
							if( cls.glconfig.vidWidth == width && cls.glconfig.vidHeight == height )
							{
								break;
							}

							Cvar_SetValue( "r_customwidth", width );
							Cvar_SetValue( "r_customheight", height );
							Cvar_Set( "r_mode", "-1" );


							// Wait until user stops dragging for 1 second, so
							// we aren't constantly recreating the GL context while
							// he tries to drag...
							vidRestartTime = Sys_Milliseconds( ) + 1000;
						}
						break;

					case SDL_WINDOWEVENT_MINIMIZED:    Cvar_SetValue( "com_minimized", 1 ); break;
					case SDL_WINDOWEVENT_RESTORED:
					case SDL_WINDOWEVENT_MAXIMIZED:    Cvar_SetValue( "com_minimized", 0 ); break;
					case SDL_WINDOWEVENT_FOCUS_LOST:   Cvar_SetValue( "com_unfocused", 1 ); break;
					case SDL_WINDOWEVENT_FOCUS_GAINED: Cvar_SetValue( "com_unfocused", 0 ); break;
				}
				break;

			default:
				break;
		}
	}
}

/*
===============
IN_ProcessGyro

Gyro aiming for one player, from the pad that player is using. Angular
velocity is integrated over the real frame time, so sensitivity does not
change with framerate (the scale is 1.0 at 60 fps, which keeps existing
sensitivity settings). A lone Joy-Con has no second stick, so its gyro is
always on unless in_gyroSingleJoyCon is 0.
===============
*/
static void IN_ProcessGyro( int player, int frameMsec )
{
	nxPlayer_t *pl = &nx_players[player];
	HidSixAxisSensorState sixaxis;
	HidSixAxisSensorHandle handle;
	nxPad_t *p;
	qboolean inMenu;
	float pitch, yaw, scale, fx, fy;
	int ix, iy, yawAxis;

	if( pl->gyroSlot < 0 )
	{
		pl->gyroRemX = pl->gyroRemY = 0.0f;
		return;
	}

	p = &nx_pads[pl->gyroSlot];
	if( !p->style || !p->numSixaxis )
		return;

	if( !in_gyromouse->integer && !( IN_NXIsSingleJoyCon( p->style ) && in_gyroSingleJoyCon->integer ) )
	{
		pl->gyroRemX = pl->gyroRemY = 0.0f;
		return;
	}

	inMenu = ( clc.state == CA_DISCONNECTED || clc.state == CA_CINEMATIC || ( Key_GetCatcher( ) & KEYCATCH_UI ) );

	// only the first player moves the menu cursor
	if( player > 0 && ( inMenu || Key_GetCatcher( ) ) )
		return;

	// on a Joy-Con pair, aim with the right one if it is there
	handle = p->sixaxis[0];
	if( p->numSixaxis > 1 && ( padGetAttributes( &p->pad ) & HidNpadAttribute_IsRightConnected ) )
		handle = p->sixaxis[1];

	memset( &sixaxis, 0, sizeof( sixaxis ) );
	if( hidGetSixAxisSensorStates( handle, &sixaxis, 1 ) == 0 )
		return;

	if ( in_gyromouse_debug->integer ) {
		Com_Printf("Player %d, pad slot %d (%s)\n", player + 1, pl->gyroSlot, IN_NXStyleName( p->style ));
		Com_Printf("Acceleration:     x=% .4f, y=% .4f, z=% .4f\n", sixaxis.acceleration.x, sixaxis.acceleration.y, sixaxis.acceleration.z);
		Com_Printf("Angular velocity: x=% .4f, y=% .4f, z=% .4f\n", sixaxis.angular_velocity.x, sixaxis.angular_velocity.y, sixaxis.angular_velocity.z);
		Com_Printf("Angle:            x=% .4f, y=% .4f, z=% .4f\n", sixaxis.angle.x, sixaxis.angle.y, sixaxis.angle.z);
	}

	// Handheld is held upright so yaw is around .y; a controller lies flat so
	// yaw is around .z. in_gyromouse_yaw_auto 0 leaves the choice to
	// in_gyromouse_yaw_axis.
	if( in_gyromouse_yaw_auto->integer )
		yawAxis = ( p->style == HidNpadStyleTag_NpadHandheld ) ? 0 : 1;
	else
		yawAxis = in_gyromouse_yaw_axis->integer;

	if( p->style == HidNpadStyleTag_NpadJoyLeft )
	{
		// sideways: the Joy-Con's long axis is now the pitch axis
		pitch = -sixaxis.angular_velocity.y;
		yaw = sixaxis.angular_velocity.z;
	}
	else if( p->style == HidNpadStyleTag_NpadJoyRight )
	{
		pitch = sixaxis.angular_velocity.y;
		yaw = sixaxis.angular_velocity.z;
	}
	else
	{
		pitch = sixaxis.angular_velocity.x;
		yaw = yawAxis ? sixaxis.angular_velocity.z : sixaxis.angular_velocity.y;
	}

	if( inMenu ) {
		pitch *= in_gyromouse_pitch_ui->value;
		yaw *= in_gyromouse_yaw_ui->value;
	} else {
		pitch *= in_gyromouse_pitch->value;
		yaw *= in_gyromouse_yaw->value;
	}

	if( frameMsec < 1 )
		frameMsec = 1;
	if( frameMsec > 100 )
		frameMsec = 100;
	scale = (float)frameMsec * ( 60.0f / 1000.0f );

	// carry the fractional part so slow, precise movements aren't truncated away
	fx = yaw * scale + pl->gyroRemX;
	fy = pitch * scale + pl->gyroRemY;
	ix = (int)fx;
	iy = (int)fy;
	pl->gyroRemX = fx - (float)ix;
	pl->gyroRemY = fy - (float)iy;

	if( ix || iy )
		IN_QueuePlayerEvent( player, SE_MOUSE, ix, iy );
}


/*
===============
IN_Frame
===============
*/
void IN_Frame( void )
{
	static int lastFrameTime = 0;
	qboolean loading;
	int now, player;

	IN_PadFrame( );

	// If not DISCONNECTED (main menu) or ACTIVE (in game), we're loading
	loading = ( clc.state != CA_DISCONNECTED && clc.state != CA_ACTIVE );

	// update isFullscreen since it might of changed since the last vid_restart
	cls.glconfig.isFullscreen = Cvar_VariableIntegerValue( "r_fullscreen" ) != 0;

	if( !cls.glconfig.isFullscreen && ( Key_GetCatcher( ) & KEYCATCH_CONSOLE ) )
	{
		// Console is down in windowed mode
		IN_DeactivateMouse( cls.glconfig.isFullscreen );
	}
	else if( !cls.glconfig.isFullscreen && loading )
	{
		// Loading in windowed mode
		IN_DeactivateMouse( cls.glconfig.isFullscreen );
	}
	else if( !( SDL_GetWindowFlags( SDL_window ) & SDL_WINDOW_INPUT_FOCUS ) )
	{
		// Window not got focus
		IN_DeactivateMouse( cls.glconfig.isFullscreen );
	}
	else
		IN_ActivateMouse( cls.glconfig.isFullscreen );

	IN_ProcessEvents( );

	now = Sys_Milliseconds( );
	for( player = 0; player < cl_numLocalClients && player < NX_MAX_PLAYERS; player++ )
		IN_ProcessGyro( player, lastFrameTime ? now - lastFrameTime : 16 );
	lastFrameTime = now;

	// Set event time for next frame to earliest possible time an event could happen
	in_eventTime = Sys_Milliseconds( );

	// In case we had to delay actual restart of video system
	if( ( vidRestartTime != 0 ) && ( vidRestartTime < Sys_Milliseconds( ) ) )
	{
		vidRestartTime = 0;
		Cbuf_AddText( "vid_restart\n" );
	}
}

/*
===============
IN_Init
===============
*/
void IN_Init( void *windowData )
{
	int appState;

	if( !SDL_WasInit( SDL_INIT_VIDEO ) )
	{
		Com_Error( ERR_FATAL, "IN_Init called before SDL_Init( SDL_INIT_VIDEO )" );
		return;
	}

	SDL_window = (SDL_Window *)windowData;

	Com_DPrintf( "\n------- Input Initialization -------\n" );

	if (!in_mouse) // HACK: check if this wasn't already called
	{
		Cmd_AddCommand( "showosk", IN_ShowOSK_f );
		Cmd_AddCommand( "controllers", IN_Controllers_f );
	}

	in_keyboardDebug = Cvar_Get( "in_keyboardDebug", "0", CVAR_ARCHIVE );

	// mouse variables
	in_mouse = Cvar_Get( "in_mouse", "1", CVAR_ARCHIVE );
	in_nograb = Cvar_Get( "in_nograb", "0", CVAR_ARCHIVE );

	in_gyromouse = Cvar_Get( "in_gyromouse", "0", CVAR_ARCHIVE );
	in_gyromouse_debug = Cvar_Get( "in_gyromouse_debug", "0", CVAR_ARCHIVE );
	in_gyromouse_yaw_axis = Cvar_Get( "in_gyromouse_yaw_axis", "0", CVAR_ARCHIVE );
	in_gyromouse_yaw_auto = Cvar_Get( "in_gyromouse_yaw_auto", "1", CVAR_ARCHIVE );
	in_gyromouse_pitch = Cvar_Get( "in_gyromouse_pitch", "-10.0", CVAR_ARCHIVE );
	in_gyromouse_yaw = Cvar_Get( "in_gyromouse_yaw", "-20.0", CVAR_ARCHIVE );
	in_gyromouse_pitch_ui = Cvar_Get( "in_gyromouse_pitch_ui", "0.0", CVAR_ARCHIVE );
	in_gyromouse_yaw_ui = Cvar_Get( "in_gyromouse_yaw_ui", "0.0", CVAR_ARCHIVE );

	in_joystick = Cvar_Get( "in_joystick", "1", CVAR_ARCHIVE|CVAR_LATCH );
	in_joystickThreshold = Cvar_Get( "joy_threshold", "0.15", CVAR_ARCHIVE );
	in_joystickUseAnalog = Cvar_Get( "in_joystickUseAnalog", "1", CVAR_ARCHIVE );
	in_joyconStickRotate = Cvar_Get( "in_joyconStickRotate", "1", CVAR_ARCHIVE );
	in_gyroSingleJoyCon = Cvar_Get( "in_gyroSingleJoyCon", "1", CVAR_ARCHIVE );
	in_padJoin = Cvar_Get( "in_padJoin", "1", CVAR_ARCHIVE );
	in_padDebug = Cvar_Get( "in_padDebug", "0", CVAR_TEMP );

	mouseAvailable = ( in_mouse->value != 0 );
	IN_DeactivateMouse( Cvar_VariableIntegerValue( "r_fullscreen" ) != 0 );

	appState = SDL_GetWindowFlags( SDL_window );
	Cvar_SetValue( "com_unfocused",	!( appState & SDL_WINDOW_INPUT_FOCUS ) );
	Cvar_SetValue( "com_minimized", appState & SDL_WINDOW_MINIMIZED );

	IN_InitPads( );

	IN_InitKeys( );

	Com_DPrintf( "------------------------------------\n" );
}

/*
===============
IN_Shutdown
===============
*/
void IN_Shutdown( void )
{
	SDL_StopTextInput( );

	IN_DeactivateMouse( Cvar_VariableIntegerValue( "r_fullscreen" ) != 0 );
	mouseAvailable = qfalse;

	IN_ShutdownPads( );

	SDL_window = NULL;
}

/*
===============
IN_Restart
===============
*/
void IN_Restart( void )
{
	IN_Init( SDL_window );
}

/*
===============
IN_SwitchKeyboard
===============
*/
void IN_SwitchKeyboard( char *out, int out_len )
{
	SwkbdConfig kbd;
	char tmp_out[out_len + 1];
	Result rc;
	tmp_out[0] = 0;
	rc = swkbdCreate( &kbd, 0 );
	if (R_SUCCEEDED( rc )) {
		swkbdConfigMakePresetDefault( &kbd );
		swkbdConfigSetInitialText( &kbd, out );
		rc = swkbdShow( &kbd, tmp_out, out_len );
		if (R_SUCCEEDED( rc ))
			strncpy( out, tmp_out, out_len );
		swkbdClose( &kbd );
	}
}

/*
=============
IN_ShowOSK_f
=============
*/
static void IN_ShowOSK_f( void )
{
	static char buffer[1024];
	int len = 0;
	SDL_Event ev;
	buffer[0] = '\0';

	IN_SwitchKeyboard( buffer, sizeof( buffer )-1 );
	len = strlen( buffer );

	ev.type = SDL_TEXTINPUT;
	ev.text.type = SDL_TEXTINPUT;
	ev.text.timestamp = SDL_GetTicks();
	ev.text.windowID = 0;
	for (int i = 0; i < len; i += 31)
	{
		strncpy( ev.text.text, buffer + i, sizeof( ev.text.text )-1 );
		SDL_PeepEvents( &ev, 1, SDL_ADDEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT );
	}
}
