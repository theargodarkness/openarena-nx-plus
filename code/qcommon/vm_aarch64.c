/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.
Copyright (C) 2020-2024 Quake3e project

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

// Load time compiler and execution environment for ARM aarch64, with dynamic
// register allocation and various optimizations.
//
// This is the QVM compiler from Quake3e (https://github.com/ec-/Quake3e,
// code/qcommon/vm_aarch64.c plus the instruction loader and checker from its
// vm.c), adapted to ioquake3's VM structures. What was changed for this port:
//  - a compatibility section replaces Quake3e's vm_local.h
//  - VM_Compile / VM_CallCompiled keep ioquake3's signatures
//  - code memory goes through a small allocator that also covers the Switch,
//    where generated code is written through one mapping and run from another

#include <math.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __SWITCH__
#include <switch.h>
#else
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#endif

#include "vm_local.h"

/*
=============================================================================

Compatibility with Quake3e's VM internals

=============================================================================
*/

#define	MAX_OPSTACK_SIZE	512
#define	PROC_OPSTACK_SIZE	30

#define	MAX_VMMAIN_CALL_ARGS	MAX_VMMAIN_ARGS

#define OP_MAX	( OP_CVFI + 1 )

// flags for vm_rtChecks cvar
#define VM_RTCHECK_PSTACK  1
#define VM_RTCHECK_OPSTACK 2
#define VM_RTCHECK_JUMP    4
#define VM_RTCHECK_DATA    8

#define S_COLOR_WARNING	S_COLOR_YELLOW
#define S_COLOR_ERROR	S_COLOR_RED

typedef struct {
	int32_t	value;			// 32
	byte	op;				// 8 -> opcode_t
	union {					// 8
		byte	opStack;	// for OP_ENTER and tracking max.opStack during initial bytecode load
		byte	origOp;		// original opcode for a macro op
	};
	unsigned jused:1;		// this instruction is a jump target
	unsigned swtch:1;		// indirect jump
	unsigned safe:1;		// non-masked OP_STORE* with known/verified address
	unsigned endp:1;		// for last OP_LEAVE instruction
	unsigned fpu:1;			// load into FPU register
	unsigned njump:1;		// near jump
} instruction_t;

#define JUMP	(1<<0)
#define FPU		(1<<1)

typedef struct opcode_info_s
{
	int	size;
	int	stack;
	int	nargs;
	int	flags; // rhs type cast
} opcode_info_t;

static cvar_t *vm_rtChecks;

static const opcode_info_t ops[ OP_MAX ] =
{
	// size, stack, nargs, flags
	{ 0, 0, 0, 0 }, // undef
	{ 0, 0, 0, 0 }, // ignore
	{ 0, 0, 0, 0 }, // break

	{ 4, 0, 0, 0 }, // enter
	{ 4,-4, 0, 0 }, // leave
	{ 0, 0, 1, 0 }, // call
	{ 0, 4, 0, 0 }, // push
	{ 0,-4, 1, 0 }, // pop

	{ 4, 4, 0, 0 }, // const
	{ 4, 4, 0, 0 }, // local
	{ 0,-4, 1, 0 }, // jump

	{ 4,-8, 2, JUMP }, // eq
	{ 4,-8, 2, JUMP }, // ne

	{ 4,-8, 2, JUMP }, // lti
	{ 4,-8, 2, JUMP }, // lei
	{ 4,-8, 2, JUMP }, // gti
	{ 4,-8, 2, JUMP }, // gei

	{ 4,-8, 2, JUMP }, // ltu
	{ 4,-8, 2, JUMP }, // leu
	{ 4,-8, 2, JUMP }, // gtu
	{ 4,-8, 2, JUMP }, // geu

	{ 4,-8, 2, JUMP|FPU }, // eqf
	{ 4,-8, 2, JUMP|FPU }, // nef

	{ 4,-8, 2, JUMP|FPU }, // ltf
	{ 4,-8, 2, JUMP|FPU }, // lef
	{ 4,-8, 2, JUMP|FPU }, // gtf
	{ 4,-8, 2, JUMP|FPU }, // gef

	{ 0, 0, 1, 0 }, // load1
	{ 0, 0, 1, 0 }, // load2
	{ 0, 0, 1, 0 }, // load4
	{ 0,-8, 2, 0 }, // store1
	{ 0,-8, 2, 0 }, // store2
	{ 0,-8, 2, 0 }, // store4
	{ 1,-4, 1, 0 }, // arg
	{ 4,-8, 2, 0 }, // bcopy

	{ 0, 0, 1, 0 }, // sex8
	{ 0, 0, 1, 0 }, // sex16

	{ 0, 0, 1, 0 }, // negi
	{ 0,-4, 3, 0 }, // add
	{ 0,-4, 3, 0 }, // sub
	{ 0,-4, 3, 0 }, // divi
	{ 0,-4, 3, 0 }, // divu
	{ 0,-4, 3, 0 }, // modi
	{ 0,-4, 3, 0 }, // modu
	{ 0,-4, 3, 0 }, // muli
	{ 0,-4, 3, 0 }, // mulu

	{ 0,-4, 3, 0 }, // band
	{ 0,-4, 3, 0 }, // bor
	{ 0,-4, 3, 0 }, // bxor
	{ 0, 0, 1, 0 }, // bcom

	{ 0,-4, 3, 0 }, // lsh
	{ 0,-4, 3, 0 }, // rshi
	{ 0,-4, 3, 0 }, // rshu

	{ 0, 0, 1, FPU }, // negf
	{ 0,-4, 3, FPU }, // addf
	{ 0,-4, 3, FPU }, // subf
	{ 0,-4, 3, FPU }, // divf
	{ 0,-4, 3, FPU }, // mulf

	{ 0, 0, 1, 0 },   // cvif
	{ 0, 0, 1, FPU }  // cvfi
};

static const char *opname[ 256 ] = {
	"OP_UNDEF",

	"OP_IGNORE",

	"OP_BREAK",

	"OP_ENTER",
	"OP_LEAVE",
	"OP_CALL",
	"OP_PUSH",
	"OP_POP",

	"OP_CONST",

	"OP_LOCAL",

	"OP_JUMP",

	//-------------------

	"OP_EQ",
	"OP_NE",

	"OP_LTI",
	"OP_LEI",
	"OP_GTI",
	"OP_GEI",

	"OP_LTU",
	"OP_LEU",
	"OP_GTU",
	"OP_GEU",

	"OP_EQF",
	"OP_NEF",

	"OP_LTF",
	"OP_LEF",
	"OP_GTF",
	"OP_GEF",

	//-------------------

	"OP_LOAD1",
	"OP_LOAD2",
	"OP_LOAD4",
	"OP_STORE1",
	"OP_STORE2",
	"OP_STORE4",
	"OP_ARG",

	"OP_BLOCK_COPY",

	//-------------------

	"OP_SEX8",
	"OP_SEX16",

	"OP_NEGI",
	"OP_ADD",
	"OP_SUB",
	"OP_DIVI",
	"OP_DIVU",
	"OP_MODI",
	"OP_MODU",
	"OP_MULI",
	"OP_MULU",

	"OP_BAND",
	"OP_BOR",
	"OP_BXOR",
	"OP_BCOM",

	"OP_LSH",
	"OP_RSHI",
	"OP_RSHU",

	"OP_NEGF",
	"OP_ADDF",
	"OP_SUBF",
	"OP_DIVF",
	"OP_MULF",

	"OP_CVIF",
	"OP_CVFI"
};

static void VM_IgnoreInstructions( instruction_t *buf, const int count ) {
	int i;

	for ( i = 0; i < count; i++ ) {
		Com_Memset( buf + i, 0, sizeof( *buf ) );
		buf[i].op = OP_IGNORE;
	}

	buf[0].value = count > 0 ? count - 1 : 0;
}


static int InvertCondition( int op )
{
	switch ( op ) {
		case OP_EQ: return OP_NE;   // == -> !=
		case OP_NE: return OP_EQ;   // != -> ==

		case OP_LTI: return OP_GEI;	// <  -> >=
		case OP_LEI: return OP_GTI;	// <= -> >
		case OP_GTI: return OP_LEI; // >  -> <=
		case OP_GEI: return OP_LTI; // >= -> <

		case OP_LTU: return OP_GEU;
		case OP_LEU: return OP_GTU;
		case OP_GTU: return OP_LEU;
		case OP_GEU: return OP_LTU;

		case OP_EQF: return OP_NEF;
		case OP_NEF: return OP_EQF;

		case OP_LTF: return OP_GEF;
		case OP_LEF: return OP_GTF;
		case OP_GTF: return OP_LEF;
		case OP_GEF: return OP_LTF;

		default: 
			Com_Error( ERR_DROP, "incorrect condition opcode %i", op );
			return op;
	}
}


/*
=================
VM_FindLocal

search for specified local variable until end of function
=================
*/
static qboolean VM_FindLocal( int32_t addr, const instruction_t *buf, const instruction_t *end, int32_t *back_addr ) {
	int32_t curr_addr = *back_addr;
	while ( buf < end ) {
		if ( buf->op == OP_LOCAL ) {
			if ( buf->value == addr ) {
				return qtrue;
			}
			++buf; continue;
		}
		if ( ops[ buf->op ].flags & JUMP ) {
			if ( buf->value < curr_addr ) {
				curr_addr = buf->value;
			}
			++buf; continue;
		}
		if ( buf->op == OP_JUMP ) {
			if ( buf->value && buf->value < curr_addr ) {
				curr_addr = buf->value;
			}
			++buf; continue;
		}
		if ( buf->op == OP_PUSH && (buf+1)->op == OP_LEAVE ) {
			break;
		}
		++buf;
	}
	*back_addr = curr_addr;
	return qfalse;
}


/*
=================
VM_Fixup

Do some corrections to fix known Q3LCC flaws
=================
*/
static void VM_Fixup( instruction_t *buf, int instructionCount )
{
	int n;
	instruction_t *i;

	i = buf;
	n = 0;

	while ( n < instructionCount )
	{
		if ( i->op == OP_LOCAL ) {

			// skip useless sequences
			if ( (i+1)->op == OP_LOCAL && (i+0)->value == (i+1)->value && (i+2)->op == OP_LOAD4 && (i+3)->op == OP_STORE4 ) {
				VM_IgnoreInstructions( i, 4 );
				i += 4; n += 4;
				continue;
			}

			// [0]OP_LOCAL + [1]OP_CONST + [2]OP_CALL + [3]OP_STORE4
			if ( (i+1)->op == OP_CONST && (i+2)->op == OP_CALL && (i+3)->op == OP_STORE4 && !(i+4)->jused ) {
				// [4]OP_CONST|OP_LOCAL (dest) + [5]OP_LOCAL(temp) + [6]OP_LOAD4 + [7]OP_STORE4
				if ( (i+4)->op == OP_CONST || (i+4)->op == OP_LOCAL ) {
					if ( (i+5)->op == OP_LOCAL && (i+5)->value == (i+0)->value && (i+6)->op == OP_LOAD4 && (i+7)->op == OP_STORE4 ) {
						int32_t back_addr = n;
						int32_t curr_addr = n;
						qboolean do_break = qfalse;

						// make sure that address of (potentially) temporary variable is not referenced further in this function
						if ( VM_FindLocal( i->value, i + 8, buf + instructionCount, &back_addr ) ) {
							i++; n++;
							continue;
						}

						// we have backward jumps in code then check for references before current position
						while ( back_addr < curr_addr ) {
							curr_addr = back_addr;
							if ( VM_FindLocal( i->value, buf + back_addr, i, &back_addr ) ) {
								do_break = qtrue;
								break;
							}
						}
						if ( do_break ) {
							i++; n++;
							continue;
						}

						(i+0)->op = (i+4)->op;
						(i+0)->value = (i+4)->value;
						VM_IgnoreInstructions( i + 4, 4 );
						i += 8;
						n += 8;
						continue;
					}
				}
			}
		}

		if ( i->op == OP_LEAVE && !i->endp ) {
			if ( !(i+1)->jused && (i+1)->op == OP_CONST && (i+2)->op == OP_JUMP ) {
				int v = (i+1)->value;
				if ( buf[ v ].op == OP_PUSH && buf[ v+1 ].op == OP_LEAVE && buf[ v+1 ].endp ) {
					VM_IgnoreInstructions( i + 1, 2 );
					i += 3;
					n += 3;
					continue;
				}
			}
		}

		//n + 0: if ( cond ) goto label1;
		//n + 2: goto label2;
		//n + 3: label1:
		// ...
		//n + x: label2:
		// NOTE: this transform is not suitable for FP to handle NaNs
		if ( ( ops[i->op].flags & (JUMP | FPU) ) == JUMP && !(i+1)->jused && (i+1)->op == OP_CONST && (i+2)->op == OP_JUMP ) {
			if ( i->value == n + 3 && (i+1)->value >= n + 3 ) {
				i->op = InvertCondition( i->op );
				i->value = ( i + 1 )->value;
				VM_IgnoreInstructions( i + 1, 2 );
				i += 3;
				n += 3;
				continue;
			}
		}

		// OP_LOAD1|OP_LOAD2 + OP_SEX8|OP_SEX16 + OP_CONST(0) + OP_EQ|OP_NE -> ignore OP_SEX8|OP_SEX16
		if ( (i->op == OP_LOAD1 && (i + 1)->op == OP_SEX8) || (i->op == OP_LOAD2 && (i + 1)->op == OP_SEX16) ) {
			if ( (i + 2)->op == OP_CONST && (i + 2)->value == 0 ) {
				if ( (i + 3)->op == OP_EQ || (i + 3)->op == OP_NE )	{
					(i + 1)->op = OP_IGNORE;
					i += 3;
					n += 3;
					continue;
				}
			}
		}

		// local = func() ; return local -> return func(), assume that local is not used/referenced afterwards
		if ( (i + 1)->op == OP_CONST && (i + 2)->op == OP_CALL && (i + 3)->op == OP_STORE4 && (i + 4)->op == OP_LOCAL && (i + 5)->op == OP_LOAD4 && (i + 6)->op == OP_LEAVE ) {
			if ( i->value == (i + 4)->value && !(i + 4)->jused ) {
				(i + 0)->op = OP_IGNORE; (i + 0)->value = 0;
				(i + 3)->op = OP_IGNORE; (i + 3)->value = 2; // jump over 2 next instructions
				(i + 4)->op = OP_IGNORE; (i + 4)->value = 0;
				(i + 5)->op = OP_IGNORE; (i + 5)->value = 0;
				i += 7;
				n += 7;
				continue;
			}
		}

		i++;
		n++;
	}
}


/*
=================
VM_LoadInstructions

loads instructions in structured format
performs basic consistency checks
=================
*/
static const char *VM_LoadInstructions( const byte *code_pos, int codeLength, int instructionCount, instruction_t *buf )
{
	static char errBuf[ 128 ];
	const byte *code_start, *code_end;
	int i, n, op0, op1, opStack;
	instruction_t *ci;

	code_start = code_pos; // for printing
	code_end = code_pos + codeLength;

	ci = buf;
	opStack = 0;
	op1 = OP_UNDEF;

	// load instructions and perform some initial calculations/checks
	for ( i = 0; i < instructionCount; i++, ci++, op1 = op0 ) {
		op0 = *code_pos;
		if ( (unsigned) op0 >= OP_MAX ) {
			sprintf( errBuf, "bad opcode %02X at offset %d", op0, (int)(code_pos - code_start) );
			return errBuf;
		}
		n = ops[ op0 ].size;
		if ( code_pos + 1 + n  > code_end ) {
			sprintf( errBuf, "code_pos > code_end" );
			return errBuf;
		}
		code_pos++;
		ci->op = op0;
		if ( n == 4 ) {
			ci->value = LittleLong( *((int32_t*)code_pos) );
			code_pos += 4;
		} else if ( n == 1 ) {
			ci->value = *((unsigned char*)code_pos);
			code_pos += 1;
		} else {
			ci->value = 0;
		}

		if ( ops[ op0 ].flags & FPU ) {
			ci->fpu = 1;
		}

		// setup jump value from previous const
		if ( op0 == OP_JUMP && op1 == OP_CONST ) {
			ci->value = (ci-1)->value;
		}

		ci->opStack = opStack;
		opStack += ops[ op0 ].stack;

		// opstack checks
		if ( opStack < 0 ) {
			sprintf( errBuf, "opStack underflow at %i", i );
			return errBuf;
		}
		if ( opStack >= PROC_OPSTACK_SIZE * 4 ) {
			sprintf( errBuf, "opStack overflow at %i", i );
			return errBuf;
		}
	}

	return NULL;
}


static qboolean safe_address( instruction_t *ci, instruction_t *proc, int dataLength )
{
	if ( ci->op == OP_LOCAL ) {
		// local address can't exceed programStack frame plus 256 bytes of passed arguments
		if ( ci->value < 8 || ( proc && ci->value >= proc->value + 256 ) )
			return qfalse;
		return qtrue;
	}

	if ( ci->op == OP_CONST ) {
		// constant address can't exceed data segment
		if ( ci->value >= dataLength || ci->value < 0 )
			return qfalse;
		return qtrue;
	}

	return qfalse;
}


/*
===============================
VM_CheckInstructions

performs additional consistency and security checks
===============================
*/
static const char *VM_CheckInstructions( instruction_t *buf,
								int instructionCount,
								const int32_t *jumpTableTargets,
								int numJumpTableTargets,
								int dataLength )
{
	static char errBuf[ 128 ];
	instruction_t *opStackPtr[ PROC_OPSTACK_SIZE + 1 ];
	int i, m, n, v, op0, op1, opStack, pstack;
	instruction_t *ci, *proc;
	int startp, endp;
	int safe_stores;
	int unsafe_stores;

	ci = buf;
	pstack = 0;
	opStack = 0;
	safe_stores = 0;
	unsafe_stores = 0;
	op1 = OP_UNDEF;
	proc = NULL;
	Com_Memset( opStackPtr, 0, sizeof( opStackPtr ) );

	startp = 0;
	endp = instructionCount - 1;

	// Additional security checks

	for ( i = 0; i < instructionCount; i++, ci++, op1 = op0 ) {
		op0 = ci->op;

		m = ops[ ci->op ].stack;
		opStack += m;
		if ( m >= 0 ) {
			// do some FPU type promotion for more efficient loads
			if ( ci->fpu && ci->op != OP_CVIF ) {
				opStackPtr[ opStack / 4 ]->fpu = 1;
			}
			opStackPtr[ opStack >> 2 ] = ci;
		} else {
			if ( ci->fpu ) {
				if ( m <= -8 ) {
					opStackPtr[ opStack / 4 + 1 ]->fpu = 1;
					opStackPtr[ opStack / 4 + 2 ]->fpu = 1;
				} else {
					opStackPtr[ opStack / 4 + 0 ]->fpu = 1;
					opStackPtr[ opStack / 4 + 1 ]->fpu = 1;
				}
			} else {
				if ( m <= -8 ) {
					//
				} else {
					opStackPtr[ opStack / 4 + 0 ] = ci;
				}
			}
		}

		// function entry
		if ( op0 == OP_ENTER ) {
			// missing block end
			if ( proc || ( pstack && op1 != OP_LEAVE ) ) {
				sprintf( errBuf, "missing proc end before %i", i );
				return errBuf;
			}
			if ( ci->opStack != 0 ) {
				v = ci->opStack;
				sprintf( errBuf, "bad entry opstack %i at %i", v, i );
				return errBuf;
			}
			v = ci->value;
			if ( v < 0 || v >= PROGRAM_STACK_SIZE || (v & 3) ) {
				sprintf( errBuf, "bad entry programStack %i at %i", v, i );
				return errBuf;
			}

			pstack = ci->value;

			// mark jump target
			ci->jused = 1;
			proc = ci;
			startp = i + 1;

			// locate endproc
			for ( endp = 0, n = i+1 ; n < instructionCount; n++ ) {
				if ( buf[n].op == OP_PUSH && buf[n+1].op == OP_LEAVE ) {
					buf[n+0].endp = 1; // OP_PUSH
					buf[n+1].endp = 1; // OP_LEAVE
					endp = n;
					break;
				}
			}

			if ( endp == 0 ) {
				sprintf( errBuf, "missing end proc for %i", i );
				return errBuf;
			}

			continue;
		}

		// proc opStack will carry max.used opStack value
		// to be checked against vm->opStackTop on function entry
		if ( proc && ci->opStack > proc->opStack ) {
			proc->opStack = ci->opStack;
		}

		// function return
		if ( op0 == OP_LEAVE ) {
			// bad return programStack
			if ( pstack != ci->value ) {
				v = ci->value;
				sprintf( errBuf, "bad programStack %i at %i", v, i );
				return errBuf;
			}
			// bad opStack before return: either void (OP_PUSH) or real value must be present
			if ( ci->opStack != 4 ) {
				v = ci->opStack;
				sprintf( errBuf, "bad opStack %i at %i", v, i );
				return errBuf;
			}
			v = ci->value;
			if ( v < 0 || v >= PROGRAM_STACK_SIZE || (v & 3) ) {
				sprintf( errBuf, "bad return programStack %i at %i", v, i );
				return errBuf;
			}
			if ( op1 == OP_PUSH ) {
				if ( proc == NULL ) {
					sprintf( errBuf, "unexpected proc end at %i", i );
					return errBuf;
				}
				proc = NULL;
				startp = i + 1; // next instruction
				endp = instructionCount - 1; // end of the image
			}
			continue;
		}

		// conditional jumps
		if ( ops[ ci->op ].flags & JUMP ) {
			v = ci->value;
			// conditional jumps should have opStack >= 8
			if ( ci->opStack < 8 ) {
				sprintf( errBuf, "bad jump opStack %i at %i", ci->opStack, i );
				return errBuf;
			}
			//if ( v >= header->instructionCount ) {
			// allow only local proc jumps
			if ( v < startp || v > endp ) {
				sprintf( errBuf, "jump target %i at %i is out of range (%i,%i)", v, i-1, startp, endp );
				return errBuf;
			}
			if ( buf[v].opStack != ci->opStack - 8 ) {
				n = buf[v].opStack;
				sprintf( errBuf, "jump target %i has bad opStack %i", v, n );
				return errBuf;
			}
			// mark jump target
			buf[v].jused = 1;
#if (id386 || idx64)
			// mark NaN-checks for vm_x86
			if ( ops[ci->op].flags & FPU ) {
				switch ( ci->op ) {
					case OP_EQF:
					case OP_LTF:
					case OP_LEF:
						ci->nanchk = 1;
						break;
					default:
						break;
				}
			}
#endif
			continue;
		}

		// unconditional jumps
		if ( op0 == OP_JUMP ) {
			// jumps should have opStack >= 4
			if ( ci->opStack < 4 ) {
				sprintf( errBuf, "bad jump opStack %i at %i", ci->opStack, i );
				return errBuf;
			}
			if ( op1 == OP_CONST ) {
				v = buf[i-1].value;
				// allow only local jumps
				if ( v < startp || v > endp ) {
					sprintf( errBuf, "jump target %i at %i is out of range (%i,%i)", v, i-1, startp, endp );
					return errBuf;
				}
				if ( buf[v].opStack != ci->opStack - 4 ) {
					n = buf[v].opStack;
					sprintf( errBuf, "jump target %i has bad opStack %i", v, n );
					return errBuf;
				}
				if ( buf[v].op == OP_ENTER ) {
					n = buf[v].op;
					sprintf( errBuf, "jump target %i has bad opcode %s", v, opname[ n ] );
					return errBuf;
				}
				if ( v == (i-1) ) {
					sprintf( errBuf, "self loop at %i", v );
					return errBuf;
				}
				// mark jump target
				buf[v].jused = 1;
			} else {
				if ( proc )
					proc->swtch = 1;
				else
					ci->swtch = 1;
			}
			// mark next instruction as jump target too
			if ( i < instructionCount-1 ) {
				buf[i+1].jused = 1;
			}
			continue;
		}

		if ( op0 == OP_CALL ) {
			if ( ci->opStack < 4 ) {
				sprintf( errBuf, "bad call opStack at %i", i );
				return errBuf;
			}
			if ( op1 == OP_CONST ) {
				v = buf[i-1].value;
				// analyse only local function calls
				if ( v >= 0 ) {
					if ( v >= instructionCount ) {
						sprintf( errBuf, "call target %i is out of range", v );
						return errBuf;
					}
					if ( buf[v].op != OP_ENTER ) {
						n = buf[v].op;
						sprintf( errBuf, "call target %i has bad opcode %s", v, opname[ n ] );
						return errBuf;
					}
					if ( v == 0 ) {
						sprintf( errBuf, "explicit vmMain call inside VM at %i", i );
						return errBuf;
					}
					// mark jump target
					buf[v].jused = 1;
				}
			}
			continue;
		}

		if ( ci->op == OP_ARG ) {
			v = ci->value & 255;
			if ( proc == NULL ) {
				sprintf( errBuf, "missing proc frame for %s %i at %i", opname[ ci->op ], v, i );
				return errBuf;
			}
			// argument can't exceed programStack frame
			if ( v < 8 || v > pstack - 4 || (v & 3) ) {
				sprintf( errBuf, "bad argument address %i at %i", v, i );
				return errBuf;
			}
			continue;
		}

		if ( ci->op == OP_LOCAL ) {
			v = ci->value;
			if ( proc == NULL ) {
				sprintf( errBuf, "missing proc frame for %s %i at %i", opname[ ci->op ], v, i );
				return errBuf;
			}
			if ( (ci+1)->op == OP_LOAD4 || (ci+1)->op == OP_LOAD2 || (ci+1)->op == OP_LOAD1 ) {
				if ( !safe_address( ci, proc, dataLength ) ) {
					sprintf( errBuf, "bad %s address %i at %i", opname[ ci->op ], v, i );
					return errBuf;
				}
			}
			continue;
		}

		if ( ci->op == OP_LOAD4 && op1 == OP_CONST ) {
			v = (ci-1)->value;
			if ( v < 0 || v > dataLength - 4 ) {
				sprintf( errBuf, "bad %s address %i at %i", opname[ ci->op ], v, i - 1 );
				return errBuf;
			}
			continue;
		}

		if ( ci->op == OP_LOAD2 && op1 == OP_CONST ) {
			v = (ci-1)->value;
			if ( v < 0 || v > dataLength - 2 ) {
				sprintf( errBuf, "bad %s address %i at %i", opname[ ci->op ], v, i - 1 );
				return errBuf;
			}
			continue;
		}

		if ( ci->op == OP_LOAD1 && op1 == OP_CONST ) {
			v =  (ci-1)->value;
			if ( v < 0 || v > dataLength - 1 ) {
				sprintf( errBuf, "bad %s address %i at %i", opname[ ci->op ], v, i - 1 );
				return errBuf;
			}
			continue;
		}

		if ( ci->op == OP_STORE4 || ci->op == OP_STORE2 || ci->op == OP_STORE1 ) {
			instruction_t *x = opStackPtr[ opStack / 4 + 1 ];
			if ( x->op == OP_CONST || x->op == OP_LOCAL ) {
				if ( safe_address( x, proc, dataLength ) ) {
					ci->safe = 1;
					safe_stores++;
					continue;
				} else {
					sprintf( errBuf, "bad %s address %i at %i", opname[ ci->op ], x->value, (int)(x - buf) );
					return errBuf;
				}
			}
			unsafe_stores++;
			continue;
		}

		if ( ci->op == OP_BLOCK_COPY ) {
			instruction_t *src = opStackPtr[ opStack / 4 + 2 ];
			instruction_t *dst = opStackPtr[ opStack / 4 + 1 ];
			int safe = 0;
			v = ci->value;
			if ( v >= dataLength ) {
				sprintf( errBuf, "bad count %i for block copy at %i", v, i - 1 );
				return errBuf;
			}
			if ( src->op == OP_LOCAL || src->op == OP_CONST ) {
				if ( !safe_address( src, proc, dataLength ) ) {
					sprintf( errBuf, "bad src for block copy at %i", (int)(dst - buf) );
					return errBuf;
				}
				src->safe = 1;
				safe++;
			}
			if ( dst->op == OP_LOCAL || dst->op == OP_CONST ) {
				if ( !safe_address( dst, proc, dataLength ) ) {
					sprintf( errBuf, "bad dst for block copy at %i", (int)(dst - buf) );
					return errBuf;
				}
				dst->safe = 1;
				safe++;
			}
			if ( safe == 2 ) {
				ci->safe = 1;
			}
		}
//		op1 = op0;
//		ci++;
	}

	if ( ( safe_stores + unsafe_stores ) > 0 ) {
		Com_DPrintf( "%s: safe stores - %i (%i%%)\n", __func__, safe_stores, safe_stores * 100 / ( safe_stores + unsafe_stores ) );
	}

	if ( op1 != OP_UNDEF && op1 != OP_LEAVE ) {
		sprintf( errBuf, "missing return instruction at the end of the image" );
		return errBuf;
	}

	// ensure that the optimization pass knows about all the jump table targets
	if ( jumpTableTargets ) {
		// first pass - validate
		for( i = 0; i < numJumpTableTargets; i++ ) {
			n = jumpTableTargets[ i ];
			if ( n < 0 || n >= instructionCount ) {
				Com_Printf( S_COLOR_YELLOW "jump target %i set on instruction %i that is out of range [0..%i]",
					i, n, instructionCount - 1 );
				break;
			}
			if ( buf[n].opStack != 0 ) {
				Com_Printf( S_COLOR_YELLOW "jump target %i set on instruction %i (%s) with bad opStack %i\n",
					i, n, opname[ buf[n].op ], buf[n].opStack );
				break;
			}
		}
		if ( i != numJumpTableTargets ) {
			// we may trap this on buggy VM_MAGIC_VER2 images
			// but we can safely optimize code even without JTRGSEG
			// so just switch to VM_MAGIC path here
			goto __noJTS;
		}
		// second pass - apply
		for( i = 0; i < numJumpTableTargets; i++ ) {
			n = jumpTableTargets[ i ];
			buf[ n ].jused = 1;
		}
	} else {
__noJTS:
		v = 0;
		// instructions with opStack > 0 can't be jump labels so it is safe to optimize/merge
		for ( i = 0, ci = buf; i < instructionCount; i++, ci++ ) {
			if ( ci->op == OP_ENTER ) {
				v = ci->swtch;
				continue;
			}
			// if there is a switch statement in function -
			// mark all potential jump labels
			if ( ci->swtch ) {
				v = ci->swtch;
			}
			if ( ci->opStack > 0 ) {
				// ci->jused = 0; // do not reset already marked targets
			} else if ( v ) {
				ci->jused = 1;
			}
		}
	}

	// explicitly mark all jump targets with unsafe bit
	for ( i = 0, ci = buf; i < instructionCount; i++, ci++ ) {
		if ( ci->jused ) {
			ci->safe = 0;
		}
	}

	VM_Fixup( buf, instructionCount );

	return NULL;
}


/*
=============================================================================

The compiler

=============================================================================
*/


#define NUM_PASSES	1

#define PASS_INIT	0
#define PASS_FINAL	1

// additional integrity checks
#define DEBUG_VM

// various definitions to enable/disable particular optimization

// use dynamic allocation of integer/scalar registers
#define DYN_ALLOC_RX
#define DYN_ALLOC_SX

// re-use constants previously stored in scratch registers
#define CONST_CACHE_RX
#define CONST_CACHE_SX

#define REGS_OPTIMIZE
#define FPU_OPTIMIZE
#define CONST_OPTIMIZE
#define ADDR_OPTIMIZE
#define LOAD_OPTIMIZE
#define RET_OPTIMIZE
#define USE_LITERAL_POOL

// allow sharing both variables and constants in registers
#define REG_TYPE_MASK
// number of variables/memory mappings per register
#define REG_MAP_COUNT 4

#define FUNC_ALIGN 16

//#define DUMP_CODE

// registers map

// general purpose registers:
// R0..R17 can be used as a scratch registers
// R18 must not be used, especially on windows
// R19..R29 and R31 must be preserved

// FPU scalar registers:
// S0..S7 can be used as a scratch registers
// S8..S15 must be preserved
// S16..S31 can be used as a scratch registers

#define R0	0 // scratch, return value
#define R1	1 // scratch
#define R2	2 // scratch
#define R3	3 // scratch
#define R4	4 // scratch
#define R5	5 // scratch
#define R6	6 // scratch
#define R7	7 // scratch
#define R8	8 // scratch, indirect return value
#define R9	9  // scratch
#define R10	10 // scratch
#define R11	11 // scratch
#define R12	12 // scratch
#define R13	13 // scratch
#define R14	14 // scratch
#define R15	15 // scratch
#define R16	16 // intra-procedure-call scratch
#define R17	17 // intra-procedure-call scratch - opStack shift
#define R18	18 // ! platform-specific, do not use
#define R19	19 // * litBase
#define R20	20 // * vmBase
#define R21	21 // * opStack
#define R22	22 // * opStackTop
#define R23	23 // * instructionPointers
#define R24	24 // * programStack
#define R25	25 // * programStackBottom
#define R26	26 // * dataBase
#define R27	27 // * dataMask
#define R28	28 // * procBase
#define R29	29 // * FP
#define R30	30 // link register
#define R31	31 // stack or zero

#define FP	R29
#define LR	R30
#define SP	R31

#define rLITBASE		R19
#define rVMBASE			R20
#define rOPSTACK		R21
#define rOPSTACKTOP		R22
#define rINSPOINTERS	R23
#define rPSTACK			R24
#define rPSTACKBOTTOM	R25
#define rDATABASE		R26
#define rDATAMASK		R27
#define rPROCBASE		R28

#define S0      0
#define S1      1
#define S2      2

typedef enum
{
	FUNC_ENTR,
	FUNC_BCPY,
	FUNC_CALL,
	FUNC_SYSC,
	FUNC_SYSF,
	FUNC_PSOF,
	FUNC_OSOF,
	FUNC_BADJ,
	FUNC_OUTJ,
	FUNC_BADR,
	FUNC_BADW,
	OFFSET_T_LAST
} offset_t;


static uint32_t *code;
static uint32_t compiledOfs;

static  instruction_t *inst = NULL;

static	uint32_t	ip;
static	uint32_t	pass;
static	uint32_t	funcOffset[ OFFSET_T_LAST ];
static	uint32_t	branchOffset[ OFFSET_T_LAST ];

static	qboolean	forceDataMask;

// literal pool
#ifdef USE_LITERAL_POOL

#define MAX_LITERALS  1024  /* usually ~160 for Q3 mods, ~270 for q3ut4 */
#define LIT_HASH_SIZE 256
#define LIT_HASH_FUNC(v) ((v*157)&(LIT_HASH_SIZE-1))

typedef struct literal_s {
	struct literal_s *next;
	uint32_t value;
} literal_t;

static uint32_t numLiterals;
static literal_t *litHash[ LIT_HASH_SIZE ];
static literal_t litList[ MAX_LITERALS ];

static void VM_InitLiterals( void )
{
	Com_Memset( litHash, 0, sizeof( litHash ) );
	Com_Memset( litList, 0, sizeof( litList ) );
	numLiterals = 0;
}

static int VM_SearchLiteral( const uint32_t value )
{
	uint32_t h = LIT_HASH_FUNC( value );
	literal_t *lt = litHash[ h ];

	while ( lt ) {
		if ( lt->value == value ) {
			return (lt - &litList[0]);
		}
		lt = lt->next;
	}

	if ( numLiterals >= ARRAY_LEN( litList ) ) {
		return -1;
	}

	lt = &litList[ numLiterals ];
	lt->next = litHash[ h ];
	lt->value = value;
	litHash[ h ] = lt;

	return numLiterals++;
}
#endif // USE_LITERAL_POOL


#ifdef _MSC_VER
#define DROP( reason, ... ) \
	do { \
		VM_FreeBuffers(); \
		Com_Error( ERR_DROP, "%s: " reason, __func__, __VA_ARGS__ ); \
	} while(0)
#else
#define DROP( reason, args... ) \
	do { \
		VM_FreeBuffers(); \
		Com_Error( ERR_DROP, "%s: " reason, __func__, ##args ); \
	} while(0)
#endif


static void VM_FreeBuffers( void )
{
	// should be freed in reversed allocation order
	//if ( instructionOffsets ) {
	//	Z_Free( instructionOffsets );
	//	instructionOffsets = NULL;
	//}

	if ( inst ) {
		Z_Free( inst );
		inst = NULL;
	}
}


/*
=================
Code memory

The compiler writes through "writable" and the finished code runs from the
address VM_AllocCode returns. On Linux these are one mapping that is
flipped from writable to executable. On the Switch they are two views of
the same JIT memory, and the executable view only exists once the code is
sealed.
=================
*/
#ifdef __SWITCH__

static byte *VM_AllocCode( vm_t *vm, uint32_t size, byte **writable )
{
	Jit *jit = (Jit *)malloc( sizeof( *jit ) );

	if ( !jit )
		return NULL;

	if ( R_FAILED( jitCreate( jit, size ) ) ) {
		free( jit );
		return NULL;
	}

	if ( R_FAILED( jitTransitionToWritable( jit ) ) ) {
		jitClose( jit );
		free( jit );
		return NULL;
	}

	vm->codeHandle = jit;
	*writable = (byte *)jitGetRwAddr( jit );
	return (byte *)jitGetRxAddr( jit );
}

static qboolean VM_SealCode( vm_t *vm )
{
	// also flushes the instruction cache
	return R_SUCCEEDED( jitTransitionToExecutable( (Jit *)vm->codeHandle ) ) ? qtrue : qfalse;
}

static void VM_FreeCode( vm_t *vm )
{
	Jit *jit = (Jit *)vm->codeHandle;

	if ( jit ) {
		jitClose( jit );
		free( jit );
	}
	vm->codeHandle = NULL;
}

#else

static byte *VM_AllocCode( vm_t *vm, uint32_t size, byte **writable )
{
	void *ptr = mmap( NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0 );

	if ( ptr == MAP_FAILED )
		return NULL;

	*writable = (byte *)ptr;
	return (byte *)ptr;
}

static qboolean VM_SealCode( vm_t *vm )
{
	if ( mprotect( vm->codeBase, vm->codeLength, PROT_READ | PROT_EXEC ) )
		return qfalse;

	// clear icache, http://blogs.arm.com/software-enablement/141-caches-and-self-modifying-code/
	__clear_cache( (char *)vm->codeBase, (char *)vm->codeBase + vm->codeLength );
	return qtrue;
}

static void VM_FreeCode( vm_t *vm )
{
	if ( munmap( vm->codeBase, vm->codeLength ) )
		Com_Printf( S_COLOR_ERROR "%s(): memory unmap failed, possible memory leak!\n", __func__ );
}

#endif


static void VM_Destroy_Compiled( vm_t *vm )
{
	if ( vm->codeBase )
		VM_FreeCode( vm );

	vm->codeBase = NULL;
}


static void __attribute__((__noreturn__)) OutJump( void )
{
	Com_Error( ERR_DROP, "program tried to execute code outside VM" );
}


static void __attribute__((__noreturn__)) BadJump( void )
{
	Com_Error( ERR_DROP, "program tried to execute code at bad location inside VM" );
}

static void __attribute__((__noreturn__)) ErrBadProgramStack( void )
{
	Com_Error( ERR_DROP, "program tried to overflow programStack" );
}


static void __attribute__((__noreturn__)) ErrBadOpStack( void )
{
	Com_Error( ERR_DROP, "program tried to overflow opStack" );
}


static void __attribute__( ( __noreturn__ ) ) ErrBadDataRead( void )
{
	Com_Error( ERR_DROP, "program tried to read out of data segment" );
}


static void __attribute__( ( __noreturn__ ) ) ErrBadDataWrite( void )
{
	Com_Error( ERR_DROP, "program tried to write out of data segment" );
}


static void emit( uint32_t isn )
{
	if ( code )
	{
		code[ compiledOfs >> 2 ] = isn;
	}

	compiledOfs += 4;
}


// conditions
#define EQ (0b0000) // equal/equals zero
#define NE (0b0001) // not equal
#define CS (0b0010) // unsigned higher or same
#define HS CS       // unsigned higher or same
#define CC (0b0011) // unsigned lower
#define LO CC       // unsigned lower
#define MI (0b0100) // minus/negative
#define PL (0b0101) // plus/positive or zero
#define VS (0b0110) // overflow
#define VC (0b0111) // no overflow
#define HI (0b1000) // unsigned higher
#define LS (0b1001) // unsigned lower or same
#define GE (0b1010) // signed greater or equal
#define LT (0b1011) // signed less than
#define GT (0b1100) // signed greater than
#define LE (0b1101) // signed less than or equal
#define AL (0b1110) // always
#define NV (0b1111) // never

#define WZR 0b11111
#define XZR 0b11111

#define NOP                     ( (0b1101010100<<22) | (0b000011<<16) | (0b00100000<<8) | 0b00011111 )
#define BRK(imm16)              ( (0b11010100001<<21) | (imm16<<5) )
#define RET(Rn)                 ( (0b1101011<<25) | (0b0010<<21) | (0b11111<<16) | (0b000000<<10) | (Rn<<5) | 0b00000 /*Rm*/ )

#define MOVZ32(Rd,imm16)        ( (0<<31) /*sf*/ | (0b10100101<<23) | (0b00<<21) | (((imm16)&0xFFFF)<<5) | Rd )
#define MOVZ32_16(Rd,imm16)     ( (0<<31) /*sf*/ | (0b10100101<<23) | (0b01<<21) | (((imm16)&0xFFFF)<<5) | Rd )
#define MOVZ64(Rd,imm16)        ( (1<<31) /*sf*/ | (0b10100101<<23) | (0b00<<21) | (((imm16)&0xFFFF)<<5) | Rd )

#define MOVK32_16(Rd,imm16)     ( (0<<31) /*sf*/ | (0b11100101<<23) | (0b01<<21) | (((imm16)&0xFFFF)<<5) | Rd )
#define MOVK64_16(Rd,imm16)     ( (1<<31) /*sf*/ | (0b11100101<<23) | (0b01<<21) | (((imm16)&0xFFFF)<<5) | Rd )
#define MOVK64_32(Rd,imm16)     ( (1<<31) /*sf*/ | (0b11100101<<23) | (0b10<<21) | (((imm16)&0xFFFF)<<5) | Rd )
#define MOVK64_48(Rd,imm16)     ( (1<<31) /*sf*/ | (0b11100101<<23) | (0b11<<21) | (((imm16)&0xFFFF)<<5) | Rd )

#define MOVN32(Rd,imm16)        ( (0<<31) /*sf*/ | (0b00100101<<23) | (0b00<<21) | ((imm16&0xFFFF)<<5) | Rd )
#define MOVN32_16(Rd,imm16)     ( (0<<31) /*sf*/ | (0b00100101<<23) | (0b01<<21) | ((imm16&0xFFFF)<<5) | Rd )
#define MOVN64(Rd,imm16)        ( (1<<31) /*sf*/ | (0b00100101<<23) | (0b00<<21) | ((imm16&0xFFFF)<<5) | Rd )
#define MOVN64_16(Rd,imm16)     ( (1<<31) /*sf*/ | (0b00100101<<23) | (0b01<<21) | ((imm16&0xFFFF)<<5) | Rd )
#define MOVN64_32(Rd,imm16)     ( (1<<31) /*sf*/ | (0b00100101<<23) | (0b10<<21) | ((imm16&0xFFFF)<<5) | Rd )
#define MOVN64_48(Rd,imm16)     ( (1<<31) /*sf*/ | (0b00100101<<23) | (0b11<<21) | ((imm16&0xFFFF)<<5) | Rd )

#define ORR32(Rd, Rn, Rm)       ( (0<<31) /*sf*/ | 0b0101010 << 24 | 0b00<<22 /*shift*/ | (0<<21) /*N*/ | (Rm<<16) | 0b000000<<10 /*imm6*/ | (Rn<<5) | Rd )
#define ORR64(Rd, Rn, Rm)       ( (1<<31) /*sf*/ | 0b0101010 << 24 | 0b00<<22 /*shift*/ | (0<<21) /*N*/ | (Rm<<16) | 0b000000<<10 /*imm6*/ | (Rn<<5) | Rd )

#define EOR32(Rd, Rn, Rm)       ( (0<<31) /*sf*/ | (0b1001010<<24) | 0b00<<22 /*shift*/ | (0<<21) /*N*/ | (Rm<<16) | 0b000000<<10 /*imm6*/ | (Rn<<5) | Rd )
#define EOR64(Rd, Rn, Rm)       ( (1<<31) /*sf*/ | (0b1001010<<24) | 0b00<<22 /*shift*/ | (0<<21) /*N*/ | (Rm<<16) | 0b000000<<10 /*imm6*/ | (Rn<<5) | Rd )

#define AND32(Rd, Rn, Rm)       ( (0<<31) /*sf*/ | (0b0001010<<24) | 0b00<<22 /*shift*/ | (0<<21) /*N*/ | (Rm<<16) | 0b000000<<10 /*imm6*/ | (Rn<<5) | Rd )
#define AND64(Rd, Rn, Rm)       ( (1<<31) /*sf*/ | (0b0001010<<24) | 0b00<<22 /*shift*/ | (0<<21) /*N*/ | (Rm<<16) | 0b000000<<10 /*imm6*/ | (Rn<<5) | Rd )

#define AND32i(Rd, Rn, immrs)   ( (0<<31) /*sf*/ | (0b00<<29) | (0b100100 << 23) | ((immrs) << 10) | ((Rn)<<5) | (Rd) )
#define ORR32i(Rd, Rn, immrs)   ( (0<<31) /*sf*/ | (0b01<<29) | (0b100100 << 23) | ((immrs) << 10) | ((Rn)<<5) | (Rd) )
#define EOR32i(Rd, Rn, immrs)   ( (0<<31) /*sf*/ | (0b10<<29) | (0b100100 << 23) | ((immrs) << 10) | ((Rn)<<5) | (Rd) )

#define MOV32(Rd, Rm)           ORR32(Rd, WZR, Rm)
#define MOV64(Rd, Rm)           ORR64(Rd, XZR, Rm)

#define MOV32i(Rd, immrs)       ORR32i(Rd, WZR, immrs)

// MUL, alias for MADD
#define MUL32(Rd, Rn, Rm)       ( (0<<31) | (0b00<<29) | (0b11011<<24) | (0b000<<21) | (Rm<<16) | (0<<15) | (WZR<<10) /*Ra*/ | (Rn<<5) | Rd )

// ADD (shifted register)
#define ADD32(Rd, Rn, Rm)       ( (0<<31) | (0b0001011000<<21) | (Rm<<16) | (0b000000<<10) /*imm6*/ | (Rn<<5) | Rd  )
#define ADD64(Rd, Rn, Rm)       ( (1<<31) | (0b0001011000<<21) | (Rm<<16) | (0b000000<<10) /*imm6*/ | (Rn<<5) | Rd  )

// ADD (immediate)
#define ADD32i(Rd, Rn, pimm12)  ( (0<<31) | (0b00100010<<23) | (0<<22) /*sh*/ | ((pimm12)<<10) | (Rn<<5) | Rd )
#define ADD64i(Rd, Rn, pimm12)  ( (1<<31) | (0b00100010<<23) | (0<<22) /*sh*/ | ((pimm12)<<10) | (Rn<<5) | Rd )

// SUB (shifted register)
#define SUB32(Rd, Rn, Rm)       ( (0<<31) | 0b1001011000<<21 | (Rm<<16) | 0b000000<<10 /*imm6*/ | (Rn<<5) | Rd  )
#define SUB64(Rd, Rn, Rm)       ( (1<<31) | 0b1001011000<<21 | (Rm<<16) | 0b000000<<10 /*imm6*/ | (Rn<<5) | Rd  )

// SUB (immediate)
#define SUB32i(Rd, Rn, pimm12)  ( (0<<31) | (0b10100010<<23) | (0<<22) /*sh*/ | ((pimm12)<<10) | (Rn<<5) | Rd )
#define SUB64i(Rd, Rn, pimm12)  ( (1<<31) | (0b10100010<<23) | (0<<22) /*sh*/ | ((pimm12)<<10) | (Rn<<5) | Rd )

#define SDIV32(Rd, Rn, Rm)      ( (0<<31) | (0b00<<29) | (0b11010110<<21) | (Rm<<16) | (0b00001<<11) | (1<<10) | (Rn<<5) | Rd )
#define UDIV32(Rd, Rn, Rm)      ( (0<<31) | (0b00<<29) | (0b11010110<<21) | (Rm<<16) | (0b00001<<11) | (0<<10) | (Rn<<5) | Rd )

#define MSUB32(Rd, Rn,Rm, Ra)   ( (0<<31) | (0b00<<29) | (0b11011<<24) | (0b000<<21) | (Rm<<16) | (1<<15) | (Ra<<10) | (Rn<<5) | Rd )

// MVN, alias for ORN (shifted register)
#define MVN32(Rd, Rm)           ( (0<<31) | (0b01<<29) | (0b01010<<24) | (0b001<<21) | (Rm<<16) | (0b000000<<10) | (0b11111<<5) | Rd  )

// NEG (shifted register), alias for SUB(shifted register)
#define NEG32(Rd, Rm)           SUB32(Rd, WZR, Rm)
//#define NEG64(Rd, Rm)           SUB64(Rd, XZR, Rm)

// LSL (register)
#define LSL32(Rd, Rn, Rm)       ( (0<<31) | (0b00<<29) | (0b11010110<<21) | (Rm<<16) | (0b0010<<12) | (0b00<<10) | (Rn<<5) | Rd )

// LSR (register)
#define LSR32(Rd, Rn, Rm)       ( (0<<31) | (0b00<<29) | (0b11010110<<21) | (Rm<<16) | (0b0010<<12) | (0b01<<10) | (Rn<<5) | Rd )

// ASR (register)
#define ASR32(Rd, Rn, Rm)       ( (0<<31) | (0b00<<29) | (0b11010110<<21) | (Rm<<16) | (0b0010<<12) | (0b10<<10) | (Rn<<5) | Rd )

// LSL (immediate in range 1..31)
#define LSL32i(Rd, Rn, shift)   ( (0<<31) | (0b10<<29) | (0b100110<<23) | (0<<22) | (((-(shift))&31)<<16) | ((31-(shift))<<10) | ((Rn)<<5) | Rd )

// LSR (immediate in range 1..31)
#define LSR32i(Rd, Rn, shift)   ( (0<<31) | (0b10<<29) | (0b100110<<23) | (0<<22) | ((shift)<<16) | (31<<10) | ((Rn)<<5) | Rd )

// ASR (immediate in range 1..31)
#define ASR32i(Rd, Rn, shift)   ( (0<<31) | (0b00<<29) | (0b100110<<23) | (0<<22) | ((shift)<<16) | (31<<10) | ((Rn)<<5) | Rd )


// LDP - load pair of registers with signed offset
#define LDP32(Rt1,Rt2,Rn,simm7)     ( 0b00<<30 | 0b101<<27 | 0<<26 | 0b010<<23 | 1<<22 /*L*/ | ((((simm7)>>2)&0x7F)<<15) | Rt2<<10 | Rn<<5 | Rt1 )
#define LDP64(Rt1,Rt2,Rn,simm7)     ( 0b10<<30 | 0b101<<27 | 0<<26 | 0b010<<23 | 1<<22 /*L*/ | ((((simm7)>>3)&0x7F)<<15) | Rt2<<10 | Rn<<5 | Rt1 )

// LDP - load pair of registers with post-index
#define LDP32post(Rt1,Rt2,Rn,simm7) ( 0b00<<30 | 0b101<<27 | 0<<26 | 0b001<<23 | 1<<22 /*L*/ | ((((simm7)>>2)&0x7F)<<15) | Rt2<<10 | Rn<<5 | Rt1 )
#define LDP64post(Rt1,Rt2,Rn,simm7) ( 0b10<<30 | 0b101<<27 | 0<<26 | 0b001<<23 | 1<<22 /*L*/ | ((((simm7)>>3)&0x7F)<<15) | Rt2<<10 | Rn<<5 | Rt1 )

// LDP - load pair of registers with pre-index
#define LDP32pre(Rt1,Rt2,Rn,simm7)  ( 0b00<<30 | 0b101<<27 | 0<<26 | 0b011<<23 | 1<<22 /*L*/ | ((((simm7)>>2)&0x7F)<<15) | Rt2<<10 | Rn<<5 | Rt1 )
#define LDP64pre(Rt1,Rt2,Rn,simm7)  ( 0b10<<30 | 0b101<<27 | 0<<26 | 0b011<<23 | 1<<22 /*L*/ | ((((simm7)>>3)&0x7F)<<15) | Rt2<<10 | Rn<<5 | Rt1 )

// STP - store pair of registers with signed offset
#define STP32(Rt1,Rt2,Rn,simm7)     ( 0b00<<30 | 0b101<<27 | 0<<26 | 0b010<<23 | 0<<22 /*L*/ | ((((simm7)>>2)&0x7F)<<15) | ((Rt2)<<10) | ((Rn)<<5) | (Rt1) )
#define STP64(Rt1,Rt2,Rn,simm7)     ( 0b10<<30 | 0b101<<27 | 0<<26 | 0b010<<23 | 0<<22 /*L*/ | ((((simm7)>>3)&0x7F)<<15) | ((Rt2)<<10) | ((Rn)<<5) | (Rt1) )

// STP - load pair of registers with post-index
#define STP32post(Rt1,Rt2,Rn,simm7) ( 0b00<<30 | 0b101<<27 | 0<<26 | 0b001<<23 | 0<<22 /*L*/ | ((((simm7)>>2)&0x7F)<<15) | Rt2<<10 | Rn<<5 | Rt1 )
#define STP64post(Rt1,Rt2,Rn,simm7) ( 0b10<<30 | 0b101<<27 | 0<<26 | 0b001<<23 | 0<<22 /*L*/ | ((((simm7)>>3)&0x7F)<<15) | Rt2<<10 | Rn<<5 | Rt1 )

// LDP - load pair of registers with pre-index
#define STP32pre(Rt1,Rt2,Rn,simm7)  ( 0b00<<30 | 0b101<<27 | 0<<26 | 0b011<<23 | 0<<22 /*L*/ | ((((simm7)>>2)&0x7F)<<15) | Rt2<<10 | Rn<<5 | Rt1 )
#define STP64pre(Rt1,Rt2,Rn,simm7)  ( 0b10<<30 | 0b101<<27 | 0<<26 | 0b011<<23 | 0<<22 /*L*/ | ((((simm7)>>3)&0x7F)<<15) | Rt2<<10 | Rn<<5 | Rt1 )


#define LDR32iwpost(Rt, Rn, simm9)  ( (0b10<<30) | 0b111000010<<21 | ((simm9&511) << 12) | (0b01 << 10) | (Rn << 5) | Rt )
#define LDR32iwpre(Rt, Rn, simm9)   ( (0b10<<30) | 0b111000010<<21 | ((simm9&511) << 12) | (0b11 << 10) | (Rn << 5) | Rt )
#define LDR32ipre(Rt, Rn, simm9)    ( (0b10<<30) | 0b111000010<<21 | ((simm9&511) << 12) | (0b00 << 10) | (Rn << 5) | Rt )

#define LDR32(Rt, Rn, Rm)       ( (0b10<<30) | (0b111000011<<21) | (Rm<<16) | (0b010<<13) /*UXTW*/ | (0<<12) /*#0*/ | (0b10<<10) | (Rn << 5) | Rt )
#define LDRH32(Rt, Rn, Rm)      ( (0b01<<30) | (0b111000011<<21) | (Rm<<16) | (0b010<<13) /*UXTW*/ | (0<<12) /*#0*/ | (0b10<<10) | (Rn << 5) | Rt )
#define LDRB32(Rt, Rn, Rm)      ( (0b00<<30) | (0b111000011<<21) | (Rm<<16) | (0b010<<13) /*UXTW*/ | (0<<12) /*#0*/ | (0b10<<10) | (Rn << 5) | Rt )

#define LDR32i(Rt, Rn, imm12)   ( (0b10<<30) | (0b11100101<<22) |  (imm12_scale((imm12),2) << 10) | (Rn << 5) | Rt )
#define LDRH32i(Rt, Rn, imm12)  ( (0b01<<30) | (0b11100101<<22) |  (imm12_scale((imm12),1) << 10) | (Rn << 5) | Rt )
#define LDRB32i(Rt, Rn, imm12)  ( (0b00<<30) | (0b11100101<<22) |  (imm12_scale((imm12),0) << 10) | (Rn << 5) | Rt )
#define LDRB32iwpost(Rt, Rn, simm9) ( (0b00<<30) | (0b111000010<<21) | (((simm9)&511)<<12) | (0b01<<10) | ((Rn)<<5) | (Rt) )

#define LDRSB32(Rt, Rn, Rm)     ( (0b00<<30) | (0b111000<<24) | (0b11<<22) /*opc*/ | (1<<21) | (Rm<<16) | (0b010<<13) /*UXTW*/ | (0<<12) /*S*/ | (0b10<<10)  | (Rn<<5) | Rt )
#define LDRSH32(Rt, Rn, Rm)     ( (0b01<<30) | (0b111000<<24) | (0b11<<22) /*opc*/ | (1<<21) | (Rm<<16) | (0b010<<13) /*UXTW*/ | (0<<12) /*S*/ | (0b10<<10)  | (Rn<<5) | Rt )

#define LDRSB32i(Rt, Rn, imm12) ( (0b00<<30) | (0b111001<<24) | (0b11<<22) | (imm12_scale(imm12,0)<<10) | (Rn<<5) | Rt )
#define LDRSH32i(Rt, Rn, imm12) ( (0b01<<30) | (0b111001<<24) | (0b11<<22) | (imm12_scale(imm12,1)<<10) | (Rn<<5) | Rt )

#define LDRSWi(Rt, Rn, imm12)   ( (0b10<<30) | (0b111001<<24) | (0b10<<22) | (imm12_scale(imm12,2)<<10) | ((Rn)<<5) | (Rt) )

//#define LDR32_4(Rt, Rn, Rm)        ( (0b10<<30) | (0b111000011<<21) | (Rm<<16) | (0b011<<13) /*LSL*/ | (1<<12) /*#2*/ | (0b10<<10) | (Rn << 5) | Rt )
#define LDR64_8(Rt, Rn, Rm)        ( (0b11<<30) | (0b111000011<<21) | (Rm<<16) | (0b011<<13) /*LSL*/ | (1<<12) /*#3*/ | (0b10<<10) | (Rn << 5) | Rt )

#define LDR64iwpost(Rt, Rn, simm9) ( (0b11<<30) | (0b111000010<<21) | ((simm9&511) << 12) | (0b01 << 10) | (Rn << 5) | Rt )
#define LDR64iwpre(Rt, Rn, simm9)  ( (0b11<<30) | (0b111000010<<21) | ((simm9&511) << 12) | (0b11 << 10) | (Rn << 5) | Rt )
#define LDR64i(Rt, Rn, imm12)      ( (0b11<<30) | (0b11100101<<22) |  (imm12_scale(imm12,3) << 10) | (Rn << 5) | Rt )

#define STR32iwpost(Rt, Rn, simm9) ( (0b10<<30) | (0b111000000<<21) | ((simm9&511) << 12) | (0b01<<10) | (Rn<<5) | Rt )
#define STR32iwpre(Rt, Rn, simm9)  ( (0b10<<30) | (0b111000000<<21) | ((simm9&511) << 12) | (0b11<<10) | (Rn<<5) | Rt )
#define STR32ipre(Rt, Rn, simm9)   ( (0b10<<30) | (0b111000000<<21) | ((simm9&511) << 12) | (0b00<<10) | (Rn<<5) | Rt )

#define STRB32i(Rt, Rn, imm12)     ( (0b00<<30) | (0b11100100<<22) |  (imm12_scale((imm12),0) << 10) | (Rn << 5) | Rt )
#define STRH32i(Rt, Rn, imm12)     ( (0b01<<30) | (0b11100100<<22) |  (imm12_scale((imm12),1) << 10) | (Rn << 5) | Rt )
#define STR32i(Rt, Rn, imm12)      ( (0b10<<30) | (0b11100100<<22) |  (imm12_scale((imm12),2) << 10) | (Rn << 5) | Rt )

#define STR64iwpost(Rt, Rn, simm9) ( (0b11<<30) | (0b111000000<<21) | ((simm9&511) << 12) | (0b01<<10) | (Rn<<5) | Rt )
#define STR64iwpre(Rt, Rn, simm9)  ( (0b11<<30) | (0b111000000<<21) | ((simm9&511) << 12) | (0b11<<10) | (Rn<<5) | Rt )
#define STR64i(Rt, Rn, imm12)      ( (0b11<<30) | (0b11100100<<22) |  (imm12_scale(imm12,3) << 10) | (Rn << 5) | Rt )

//#define STR32(Rt, Rn, Rm)          ( (0b10<<30) | (0b111000001<<21) | (Rm<<16) | (0b011<<13) /*LSL*/  | (0<<12) /*#0*/ | (0b10<<10) | (Rn<<5) | Rt )
#define STR32(Rt, Rn, Rm)          ( (0b10<<30) | (0b111000001<<21) | (Rm<<16) | (0b010<<13) /*UXTW*/ | (0<<12) /*#0*/ | (0b10<<10) | (Rn<<5) | Rt )
#define STRH32(Rt, Rn, Rm)         ( (0b01<<30) | (0b111000001<<21) | (Rm<<16) | (0b010<<13) /*UXTW*/ | (0<<12) /*#0*/ | (0b10<<10) | (Rn<<5) | Rt )
#define STRB32(Rt, Rn, Rm)         ( (0b00<<30) | (0b111000001<<21) | (Rm<<16) | (0b010<<13) /*UXTW*/ | (0<<12) /*#0*/ | (0b10<<10) | (Rn<<5) | Rt )
#define STRB32iwpost(Rt, Rn, simm9) ( (0b00<<30) | (0b111000000<<21) | (((simm9)&511)<<12) | (0b01<<10) | ((Rn)<<5) | (Rt) )

// LDR (literal) - PC-related load
#define LDR32lit(Rt,simm19)        ( (0b00<<30) | (0b011<<27) | (0<<26) | (0b00<<24) | (encode_offset19(simm19)<<5) | Rt )
#define LDR64lit(Rt,simm19)        ( (0b01<<30) | (0b011<<27) | (0<<26) | (0b00<<24) | (encode_offset19(simm19)<<5) | Rt )

//#define STR32_4(Rt, Rn, Rm)       ( (0b10<<30) | 0b111000001<<21 | Rm << 16 | 0b011<<13 /*LSL*/ | 1<<12 /*#2*/ | 0b10 << 10 | (Rn << 5) | Rt )
//#define STR64_8(Rt, Rn, Rm)       ( (0b11<<30) | 0b111000001<<21 | Rm << 16 | 0b011<<13 /*LSL*/ | 1<<12 /*#3*/ | 0b10 << 10 | (Rn << 5) | Rt )

#define SXTB(Rd, Rn)               ( (0<<31) | (0b00<<29) | (0b100110<<23) | (0<<22) /*N*/ | (0b000000<<16) /*immr*/ | (0b000111<<10) /*imms*/ | (Rn<<5) | Rd )
#define UXTB(Rd, Rn)               ( (0<<31) | (0b10<<29) | (0b100110<<23) | (0<<22) /*N*/ | (0b000000<<16) /*immr*/ | (0b000111<<10) /*imms*/ | (Rn<<5) | Rd )
#define SXTH(Rd, Rn)               ( (0<<31) | (0b00<<29) | (0b100110<<23) | (0<<22) /*N*/ | (0b000000<<16) /*immr*/ | (0b001111<<10) /*imms*/ | (Rn<<5) | Rd )
#define UXTH(Rd, Rn)               ( (0<<31) | (0b10<<29) | (0b100110<<23) | (0<<22) /*N*/ | (0b000000<<16) /*immr*/ | (0b001111<<10) /*imms*/ | (Rn<<5) | Rd )

// CMP (immediate)
#define CMP32i(Rn, imm12)          ( (0<<31) | (0b11<<29) | (0b100010<<23) | (0<<22) /*sh*/ | (imm12) << 10 | (Rn<<5) | WZR /*Rd*/ )
#define CMP64i(Rn, imm12)          ( (1<<31) | (0b11<<29) | (0b100010<<23) | (0<<22) /*sh*/ | (imm12) << 10 | (Rn<<5) | XZR /*Rd*/ )

// CMP (shifted register)
#define CMP32(Rn, Rm)              ( (0<<31) | (0b11<<29) | (0b01011<<24) | (0b00<<22) /*sh*/ | (0<<21) | (Rm<<16) | (0b000000<<10) /*imm6*/ | (Rn<<5) | WZR /*Rd*/ )
#define CMP64(Rn, Rm)              ( (1<<31) | (0b11<<29) | (0b01011<<24) | (0b00<<22) /*sh*/ | (0<<21) | (Rm<<16) | (0b000000<<10) /*imm6*/ | (Rn<<5) | XZR /*Rd*/ )

// CBZ - Compare and Branch on Zero
#define CBZ32(Rt, simm19)          ( (0<<31) | (0b011010<<25) | (0<<24) /*op*/ | (encode_offset19(simm19)<<5) | Rt )

// CBNZ - Compare and Branch on Nonzero
#define CBNZ32(Rt, simm19)         ( (0<<31) | (0b011010<<25) | (1<<24) /*op*/ | (encode_offset19(simm19)<<5) | Rt )

// conditional branch within +/-1M
#define Bcond(cond, simm19)        ( (0b0101010<<25) | (0<<24) | (encode_offset19(simm19)<<5) | (0<<4) | cond )

// unconditional branch within +/-128M
#define B(simm26)                  ( (0<<31) | (0b00101<<26) | encode_offset26(simm26) )

// branch with link within +/-128M
#define BL(simm26)                 ( (1<<31) | (0b00101<<26) | encode_offset26(simm26) )

// branch to register
#define BR(Rn)                     ( (0b1101011<<25) | (0<<24) | (0<<23) | (0b00<<21) | (0b11111<<16) | (0b0000<<12) | (0<<11) /*A*/ | (0<<10) /*M*/ | (Rn<<5) | 0b00000 /*Rm*/ )

// branch with link to register
#define BLR(Rn)                    ( (0b1101011<<25) | (0<<24) | (0<<23) | (0b01<<21) | (0b11111<<16) | (0b0000<<12) | (0<<11) /*A*/ | (0<<10) /*M*/ | (Rn<<5) | 0b00000 /*Rm*/ )

// Prefetch Memory (immediate)
#define PRFMi(Rt, Rn, imm12)       ( (0b11111<<27) | (0b00110<<22) | (((imm12>>3)&0xFFF)<<10) | ((Rn)<<5) | (Rt) )
// Rt register fields:
// policy
#define KEEP 0
#define STRM 1 // non-temporal hint
// target cache
#define L1 (0b00<<1)
#define L2 (0b01<<1)
#define L3 (0b10<<1)
// type
#define PLD (0b00<<3) // prefetch for load
#define PLI (0b01<<3) // preload instructions
#define PST (0b10<<3) // prefetch for store


#define FABS(Sd, Sn)             ( (0b000<<29) | (0b11110<<24) | (0b00<<22) | (1<<21) | (0b00000110000<<10) | (Sn<<5) | Sd )
#define FSQRT(Sd, Sn)            ( (0b000<<29) | (0b11110<<24) | (0b00<<22) | (1<<21) | (0b00001110000<<10) | (Sn<<5) | Sd )
#define FNEG(Sd, Sn)             ( (0b000<<29) | (0b11110<<24) | (0b00<<22) | (1<<21) | (0b0000<<17) | (0b10<<15) | (0b10000<<10) | (Sn<<5) | Sd )
#define FADD(Sd, Sn, Sm)         ( (0b000<<29) | (0b11110<<24) | (0b00<<22) | (1<<21) | (Sm<<16) | (0b001<<13) | (0<<12) /*op*/ | (0b10<<10) | (Sn<<5) | Sd )
#define FSUB(Sd, Sn, Sm)         ( (0b000<<29) | (0b11110<<24) | (0b00<<22) | (1<<21) | (Sm<<16) | (0b001<<13) | (1<<12) /*op*/ | (0b10<<10) | (Sn<<5) | Sd )
#define FMUL(Sd, Sn, Sm)         ( (0b000<<29) | (0b11110<<24) | (0b00<<22) | (1<<21) | (Sm<<16) | (0<<15) /*op*/ | (0b000<<12) | (0b10<<10) | (Sn<<5) | Sd )
#define FDIV(Sd, Sn, Sm)         ( (0b000<<29) | (0b11110<<24) | (0b00<<22) | (1<<21) | (Sm<<16) | (0b0001<<12) | (0b10<<10) | (Sn<<5) | Sd )

#define FCMP(Sn, Sm)             ( (0b000<<29) | (0b11110<<24) | (0b00<<22) | (1<<21) | (Sm<<16) | (0b00<<14) | (0b1000<<10) | (Sn<<5) | (0b00<<3) /*opc*/ | 0b000 )
#define FCMP0(Sn)                ( (0b000<<29) | (0b11110<<24) | (0b00<<22) | (1<<21) | (0<<16)  | (0b00<<14) | (0b1000<<10) | (Sn<<5) | (0b01<<3) /*opc*/ | 0b000 )

// single precision to signed integer
#define FCVTZS(Rd, Sn)           ( (0<<31) | (0b00<<29) | (0b11110<<24) | (0b00<<22)  | (1<<21) | (0b11<<19) /*rmode*/ | (0b000<<16) /*opcode*/ | (0b000000<<10) | (Sn<<5) | Rd )
// signed integer to single precision
#define SCVTF(Sd, Rn)            ( (0<<31) | (0b00<<29) | (0b11110<<24) | (0b00<<22) | (1<<21) | (0b00<<19) /*rmode*/ | (0b010<<16) /*opcode*/ | (0b000000<<10) | (Rn<<5) | Sd )

// move scalar to scalar
#define FMOV(Sd, Sn)             ( (0<<31) | (0b00<<29) | (0b11110<<24) | (0b00<<22) | (1<<21) | (0b00<<19) /*rmode*/ | (0b000<<16) /*opcode*/ | (0b010000<<10) | (Sn<<5) | Sd )
// move scalar to general
#define FMOVgs(Rd, Sn)           ( (0<<31) | (0b00<<29) | (0b11110<<24) | (0b00<<22) | (1<<21) | (0b00<<19) /*rmode*/ | (0b110<<16) /*opcode*/ | (0b000000<<10) | (Sn<<5) | Rd )
// move general to scalar
#define FMOVsg(Sd, Rn)           ( (0<<31) | (0b00<<29) | (0b11110<<24) | (0b00<<22) | (1<<21) | (0b00<<19) /*rmode*/ | (0b111<<16) /*opcode*/ | (0b000000<<10) | (Rn<<5) | Sd )
// move immediate to scalar
#define FMOVi(Sd, imm8)          ( (0<<31) | (0b00<<29) | (0b11110<<24) | (0b00<<22) | (1<<21) | ((imm8)<<13) | (0b100<<10) | (0b00000<<5) | Sd )

#define VLDR(St, Rn, Rm)         ( (0b10<<30) | (0b111<<27) | (1<<26) | (0b00<<24) | (0b01<<22) /*opc*/ | (1<<21) | (Rm<<16) | (0b010<<13) /*UXTW*/ | (0<<12) /*S*/ | (0b10<<10) | (Rn<<5) | St )
#define VSTR(St, Rn, Rm)         ( (0b10<<30) | (0b111<<27) | (1<<26) | (0b00<<24) | (0b00<<22) /*opc*/ | (1<<21) | (Rm<<16) | (0b010<<13) /*UXTW*/ | (0<<12) /*S*/ | (0b10<<10) | (Rn<<5) | St )

#define VLDRi(St, Rn, imm12)     ( (0b10<<30) | (0b111<<27) | (1<<26) | (0b01<<24) | (0b01<<22) /*opc*/ | (imm12_scale(imm12,2) << 10) | (Rn<<5) | St )
#define VSTRi(St, Rn, imm12)     ( (0b10<<30) | (0b111<<27) | (1<<26) | (0b01<<24) | (0b00<<22) /*opc*/ | (imm12_scale(imm12,2) << 10) | (Rn<<5) | St )


static qboolean can_encode_imm12( const uint32_t imm12, const uint32_t scale )
{
	const uint32_t mask = (1<<scale) - 1;

	if ( imm12 & mask || imm12 >= 4096 * (1 << scale) )
		return qfalse;

	return qtrue;
}


static uint32_t imm12_scale( const uint32_t imm12, const uint32_t scale )
{
#ifdef DEBUG_VM
	const uint32_t mask = (1<<scale) - 1;

	if ( imm12 & mask || imm12 >= 4096 * (1 << scale) )
		DROP( "can't encode offset %i with scale %i", imm12, (1 << scale) );
#endif
	return imm12 >> scale;
}


static qboolean encode_arith_imm( const uint32_t imm, uint32_t *res ) {

	if ( imm <= 0xFFF ) {
		*res = imm;
		return qtrue;
	}

	if ( (imm >> 12) <= 0xFFF && (imm & 0xFFF) == 0 ) {
		*res = (1 << 12) | (imm >> 12);
		return qtrue;
	}

	return qfalse;
}


static int shifted_mask( const uint64_t v ) {
	const uint64_t m = v - 1;
	return ( ( ( m | v ) + 1 ) & m ) == 0;
}


static qboolean encode_logic_imm( const uint64_t v, uint32_t reg_size, uint32_t *res ) {
	uint64_t mask, imm;
	uint32_t size, len;
	uint32_t N, immr, imms;

	// determine element size
	if ( reg_size == 64 ) {
		mask = 0xFFFFFFFF;
		size = 32;
	} else {
		if ( v > 0xFFFFFFFF ) {
			return qfalse;
		}
		mask = 0xFFFF;
		size = 16;
	}
	for ( ;; ) {
		if ( ( v & mask ) != ( (v >> size) & mask ) || size == 1 ) {
			mask |= mask << size;
			size <<= 1;
			break;
		}
		size >>= 1;
		mask >>= size;
	}

	imm = v & mask;

	// early reject
	if ( !shifted_mask( imm ) && !shifted_mask( ~( imm | ~mask ) ) ) {
		return qfalse;
	}

	// rotate right to set leading zero and trailing one
	mask = 1ULL << ( size - 1 ) | 1;
	for ( immr = 0; immr < size; immr++ ) {
		if ( ( imm & mask ) == 1 ) {
			break;
		}
		imm = ( ( imm & 1 ) << ( size - 1 ) ) | ( imm >> 1 );
	}

	if ( immr == size ) {
		// all ones/zeros, unsupported
		return qfalse;
	}

	// count trailing bits set
	for ( len = 0; len < size; len++ ) {
		if ( ( ( imm >> len ) & 1 ) == 0 ) {
			break;
		}
	}

	//if ( len == size || ( imm >> len ) != 0 ) {
	//	return qfalse;
	//}

	N = ( size >> 6 ) & 1;
	imms = (63 & (64 - size*2)) | (len - 1);
	*res = ( N << 12 ) | ( (size - immr) << 6 ) | imms;

	return qtrue;
}


// check if we can encode single-precision scalar immediate
static qboolean can_encode_f32_imm( const uint32_t v )
{
	uint32_t exp3 = (v >> 25) & ((1<<6)-1);

	if ( exp3 != 0x20 && exp3 != 0x1F )
		return qfalse;

	if ( v & ((1<<19)-1) )
		return qfalse;

	return qtrue;
}


static uint32_t encode_f32_imm( const uint32_t v )
{
	return  (((v >> 31) & 0x1) << 7) | (((v >> 23) & 0x7) << 4) | ((v >> 19) & 0xF);
}


static void emit_MOVXi( uint32_t reg, uint64_t imm )
{
	emit( MOVZ64( reg, imm & 0xFFFF ) );

	if ( imm <= 0xFFFF )
		return;

	emit( MOVK64_16( reg, (imm >> 16)&0xFFFF ) );

	if ( imm <= 0xFFFFFFFF )
		return;

	emit( MOVK64_32( reg, (imm >> 32)&0xFFFF ) );

	if ( imm <= 0xFFFFFFFFFFFF )
		return;

	emit( MOVK64_48( reg, (imm >> 48)&0xFFFF ) );
}

// Fixed-size variant: always emits 4 instructions regardless of value.
// Required when the same call site sees different magnitudes across
// VM_Compile's two passes -- notably the prologue's rLITBASE load, where
// pass 0 passes litBase=NULL (1 instr) and pass 1 passes a real 48-bit
// address (3 instr).  Variable-length there shifts every subsequent
// offset, leaving pass-0's funcOffset[FUNC_ENTR] stale and pass-1's
// BL FUNC_ENTR landing in the dispatch trampoline table.
static void emit_MOVXi64( uint32_t reg, uint64_t imm )
{
	emit( MOVZ64( reg, imm & 0xFFFF ) );
	emit( MOVK64_16( reg, (imm >> 16) & 0xFFFF ) );
	emit( MOVK64_32( reg, (imm >> 32) & 0xFFFF ) );
	emit( MOVK64_48( reg, (imm >> 48) & 0xFFFF ) );
}

// array sizes for cached/meta registers
#define NUM_RX_REGS 18 // max[R0..R17] + 1
#define NUM_SX_REGS 32 // max[S0..S31] + 1

// general-purpose register list available for dynamic allocation
static const uint32_t rx_list_alloc[] = {
	R0, R1, R2, R3, // R0-R3 are required minimum
	R4, R5, R6, R7,
	R8, R9, R10, R11,
	R12, R13, R14, R15,
	R16, R17
};

// FPU scalar register list available for dynamic allocation
static const uint32_t sx_list_alloc[] = {
	S0, S1, 2, 3, 4, 5, 6, 7, // S0 and S1 are required minimum
	// S8..S15 must be preserved
	16, 17, 18, 19, 20, 21, 22, 23,
	24, 25, 26, 27, 28, 29, 30, 31
};

#ifdef CONST_CACHE_RX
static const uint32_t rx_list_cache[] = {
	//R0, R1,
	R2, R3,
	R4, R5, R6, R7,
	R8, R9, R10, R11,
	R12, R13, R14, R15,
	R16, R17,
};
#endif

#ifdef CONST_CACHE_SX
static const uint32_t sx_list_cache[] = {
	S0, S1, 2, 3, 4, 5, 6, 7,
	// S8..S15 must be preserved
	16, 17, 18, 19, 20, 21, 22, 23,
	24, 25, 26, 27, 28, 29, 30, 31
};
#endif

// optimization framework:

#include "vm_optimize.h"

// platform functions:

static void mov_rx( uint32_t dst, uint32_t src )
{
	emit( MOV32( dst, src ) );
}


static void mov_sx( uint32_t dst, uint32_t src )
{
	emit( FMOV( dst, src ) );
}


static uint32_t split_rx( uint32_t reg )
{
	const uint32_t rx = alloc_rx( R2 );
	unmask_rx( reg );
	return rx;
}


static uint32_t split_sx( uint32_t reg )
{
	const uint32_t sx = alloc_sx( S2 );
	unmask_sx( reg );
	return sx;
}


static void mov_rx_sx( uint32_t rx, uint32_t sx )
{
	emit( FMOVgs( rx, sx ) );
}


static void mov_sx_rx( uint32_t sx, uint32_t rx )
{
	emit( FMOVsg( sx, rx ) );
}


static void mov_rx_imm32( uint32_t reg, uint32_t imm32 )
{
	uint32_t immrs;

	if ( imm32 <= 0xFFFF ) {
		emit( MOVZ32( reg, imm32 ) );
		return;
	}

	if ( (imm32 & 0xFFFF) == 0 ) {
		emit( MOVZ32_16( reg, (imm32 >> 16) & 0xFFFF ) );
		return;
	}

	if ( ~imm32 <= 0xFFFF ) {
		emit( MOVN32( reg, ~imm32 ) );
		return;
	}

	if ( encode_logic_imm( imm32, 32, &immrs ) ) {
		emit( MOV32i( reg, immrs ) );
		return;
	}

	emit( MOVZ32( reg, imm32 & 0xFFFF ) );
	emit( MOVK32_16( reg, (imm32 >> 16) & 0xFFFF ) );
}


static void mov_sx_imm32( uint32_t reg, uint32_t imm32 )
{
	uint32_t rx;

#ifdef USE_LITERAL_POOL
	int litIndex;
#endif
	if ( imm32 == 0 ) {
		mov_sx_rx( reg, WZR );
		return;
	}

	if ( can_encode_f32_imm( imm32 ) ) {
		emit( FMOVi( reg, encode_f32_imm( imm32 ) ) );
		return;
	}

#ifdef USE_LITERAL_POOL
	litIndex = VM_SearchLiteral( imm32 );
	if ( litIndex >= 0 ) {
		emit( VLDRi( reg, rLITBASE, (litIndex * 4) ) );
		return;
	}
#endif

	rx = alloc_rx_const( R2, imm32 );	// rx = imm32
	mov_sx_rx( reg, rx );				// sx = rx
	unmask_rx( rx );
}


static void mov_rx_local( uint32_t reg, const uint32_t addr )
{
	uint32_t imm;
	if ( encode_arith_imm( addr, &imm ) ) {
		emit( ADD32i( reg, rPSTACK, imm ) );	// reg = pStack + addr
	} else {
		if ( find_rx_const( addr ) ) {
			uint32_t rx = alloc_rx_const( R3, addr );	// rx = const
			emit( ADD32( reg, rPSTACK, rx ) );			// reg = pStack + rx
			unmask_rx( rx );
		} else {
			mov_rx_imm32( reg, addr );			// reg = addr
			emit( ADD32( reg, rPSTACK, reg ) );	// reg = pStack + reg
		}
	}
}


static void mov_sx_local( uint32_t reg, const uint32_t addr )
{
	const uint32_t rx = alloc_rx_local( R2 | RCONST, addr );
	mov_sx_rx( reg, rx );
	unmask_rx( rx );
}


static void load4_rx( uint32_t reg, uint32_t offset )
{
	emit( LDR32i( reg, rOPSTACK, offset ) );
}


static void load4_sx( uint32_t reg, uint32_t offset )
{
	emit( VLDRi( reg, rOPSTACK, offset ) );
}


static void store4_rx( uint32_t rx, uint32_t offset )
{
	emit( STR32i( rx, rOPSTACK, offset ) );
}


static void store4_sx( uint32_t sx, uint32_t offset )
{
	emit( VSTRi( sx, rOPSTACK, offset ) );
}


static void store4_const( uint32_t value, uint32_t offset )
{
	const uint32_t rx = alloc_rx_const( R2, value );
	store4_rx( rx, offset );
	unmask_rx( rx );
}


static void store4_local( uint32_t value, uint32_t offset )
{
	const uint32_t rx = alloc_rx_local( R2 | TEMP, value );
	store4_rx( rx, offset );
	unmask_rx( rx );
}


static uint32_t get_comp( int op )
{
	switch ( op ) {
		case OP_EQ: return EQ;
		case OP_NE: return NE;
		case OP_LTI: return LT;
		case OP_LEI: return LE;
		case OP_GTI: return GT;
		case OP_GEI: return GE;
		case OP_LTU: return LO;
		case OP_LEU: return LS;
		case OP_GTU: return HI;
		case OP_GEU: return HS;
		case OP_EQF: return EQ;
		case OP_NEF: return NE;
		case OP_LTF: return MI;
		case OP_LEF: return LS;
		case OP_GTF: return GT;
		case OP_GEF: return GE;
		default: DROP( "unexpected op %i", op );
	}

	return 0;
}


static uint32_t encode_offset26( uint32_t ofs )
{
	const uint32_t x = ofs >> 2;
	const uint32_t t = x >> 26;

	if ( ( ( t != 0x0F && t != 0x00 ) || ( ofs & 3 ) ) && pass != PASS_INIT )
		DROP( "can't encode %i", ofs );

	return x & 0x03FFFFFF;
}


static qboolean can_encode_offset19( int32_t ofs )
{
	if ( ofs >= -(1 << 20) && ofs <= (1 << 20) - 4 )
		return qtrue;

	return qfalse;
}


static uint32_t encode_offset19( uint32_t ofs )
{
	const uint32_t x = ofs >> 2;
	const uint32_t t = x >> 19;

	if ( ( ( t != 0x7FF && t != 0x00 ) || ( ofs & 3 ) ) && pass != PASS_INIT )
		DROP( "can't encode %i", ofs );

	return x & 0x7FFFF;
}


static void emitAlign( const uint32_t align )
{
	while ( compiledOfs & (align-1) )
		emit(NOP);
}


static void emitFuncOffset( vm_t *vm, offset_t func )
{
	uint32_t offset = funcOffset[ func ] - compiledOfs;

	emit( BL( offset ) );
}


static void emitFuncBranch( vm_t *vm, offset_t func )
{
	branchOffset[ func ] = compiledOfs;

	emitFuncOffset( vm, func );
}


static int32_t getFuncOffset( vm_t *vm, offset_t func )
{
	return branchOffset[ func ];
	//return funcOffset[ func ];
}


static void emit_CheckReg( vm_t *vm, uint32_t reg, offset_t func )
{
	int32_t offset;

	if ( forceDataMask ) {
		emit( AND32( reg, rDATAMASK, reg ) ); // rN = rN & rDATAMASK
		return;
	}

	emit( CMP32( reg, rDATAMASK ) );
	offset = getFuncOffset( vm, func ) - compiledOfs;
	if ( can_encode_offset19( offset ) ) {
		emit( Bcond( HS, offset ) );	// if >= (unsigned)
	} else {
		emit( Bcond( LO, +8 ) );		// if < (unsigned) then jump over
		emitFuncBranch( vm, func );
	}
}


static void emit_CheckJump( vm_t *vm, uint32_t reg, int proc_base, int proc_len )
{
	uint32_t rx[2], imm;
	int32_t offset;

	if ( ( vm_rtChecks->integer & VM_RTCHECK_JUMP ) == 0 ) {
		return;
	}

	if ( proc_base != -1 ) {
		// allow jump within local function scope only
		rx[0] = alloc_rx( R2 | TEMP );
		if ( encode_arith_imm( proc_base, &imm ) )
			emit(SUB32i(rx[0], reg, imm));			// r2 = ip - procBase
		else {
			mov_rx_imm32(rx[0], proc_base);			// r2 = procBase
			emit(SUB32(rx[0], reg, rx[0]));			// r2 = ip - R2
		}
		// (ip > proc_len) ?
		if ( encode_arith_imm( proc_len, &imm ) ) {
			emit(CMP32i(rx[0], imm));				// cmp r2, proclen
		} else {
			rx[1] = alloc_rx_const( R1, proc_len );	// r1 = procLen
			emit(CMP32(rx[0], rx[1]));				// cmp r2, r1
			unmask_rx( rx[1] );
		}
		unmask_rx( rx[0] );
		offset = getFuncOffset( vm, FUNC_BADJ ) - compiledOfs;
		if ( can_encode_offset19( offset ) ) {
			emit( Bcond( HI, offset ) );	// jump if unsigned greater
		} else {
			emit( Bcond( LS, +8 ) );		// jump over if unsigned less or same
			emitFuncBranch( vm, FUNC_BADJ );
		}
	} else {
		// check if reg >= header->instructionCount
		rx[0] = alloc_rx( R2 | TEMP );
		emit(LDR32i(rx[0], rVMBASE, offsetof(vm_t, instructionCount))); // r2 = vm->instructionCount
		emit(CMP32(reg, rx[0]));				// cmp reg, r2
		offset = getFuncOffset( vm, FUNC_OUTJ ) - compiledOfs;
		if ( can_encode_offset19( offset ) ) {
			emit( Bcond( HS, offset ) );		// jump if unsigned greater or equal
		} else {
			emit( Bcond( LO, +8 ) );			// jump over if unsigned less
			emitFuncBranch( vm, FUNC_OUTJ );	// error function
		}
		unmask_rx( rx[0] );
	}
}


static void emit_CheckProc( vm_t *vm, instruction_t *ins )
{
	uint32_t imm;
	int32_t offset;

	// programStack overflow check
	if ( vm_rtChecks->integer & VM_RTCHECK_PSTACK ) {
		emit(CMP32(rPSTACK, rPSTACKBOTTOM));	// check if pStack < vm->stackBottom
		offset = getFuncOffset( vm, FUNC_PSOF ) - compiledOfs;
		if ( can_encode_offset19( offset ) ) {
			emit( Bcond( LT, offset ) );		// jump if signed less
		} else {
			emit( Bcond( GE, +8 ) );			// jump over if signed higher or equal
			emitFuncBranch( vm, FUNC_PSOF );
		}
	}

	// opStack overflow check
	if ( vm_rtChecks->integer & VM_RTCHECK_OPSTACK ) {
		uint32_t n = ins->opStack;        // proc->opStack carries max.used opStack value
		uint32_t rx = alloc_rx( R2 | TEMP );
		if ( encode_arith_imm( n, &imm ) ) {
			emit(ADD64i(rx, rOPSTACK, imm));// r2 = opstack + max.opStack
		} else {
			mov_rx_imm32(rx, n);           // r2 = max.opStack
			emit(ADD64(rx, rOPSTACK, rx)); // r2 = opStack + r2
		}
		emit(CMP64(rx, rOPSTACKTOP));      // check if r2 > vm->opstackTop
		offset = getFuncOffset( vm, FUNC_OSOF ) - compiledOfs;
		if ( can_encode_offset19( offset ) ) {
			emit( Bcond( HI, offset ) );	// jump over if unsigned higher
		} else {
			emit( Bcond( LS, +8 ) );		// jump over if unsigned less or equal
			emitFuncBranch( vm, FUNC_OSOF );
		}
		unmask_rx( rx );
	}
}


static void emitCallFunc( vm_t *vm )
{
	int i;
	init_opstack(); // to avoid any side-effects on emit_CheckJump()

funcOffset[ FUNC_CALL ] = compiledOfs; // to jump from OP_CALL

	emit(CMP32i(R0, 0)); // check if syscall
	emit(Bcond(LT, funcOffset[ FUNC_SYSC ] - compiledOfs));

	// check if R0 >= header->instructionCount
	mask_rx( R0 );
	emit_CheckJump( vm, R0, -1, 0 );
	unmask_rx( R0 );

	// local function call
	emit(LDR64_8(R16, rINSPOINTERS, R0)); // r16 = instructionPointers[ r0 ]
	emit(BR(R16));
	emit(BRK(0));

	// syscall
funcOffset[ FUNC_SYSC ] = compiledOfs; // to jump from OP_CALL

	emit(MVN32(R0, R0));   // r0 = ~r0

funcOffset[ FUNC_SYSF ] = compiledOfs; // to jump from ConstOptimize()

	emit(SUB64i(SP, SP, 128+16)); // SP -= (128 + 16)

	// save LR, (+FP) because it will be clobbered by BLR instruction
	emit(STP64(LR, FP, SP, 128)); // SP[128] = { LR, FP }

	// modify VM stack pointer for recursive VM entry

	//currentVM->programStack = pstack - 8;
	emit(SUB32i(R1, rPSTACK, 8)); // r1 = pstack - 8
	emit(STR32i(R1, rVMBASE, offsetof(vm_t, programStack))); // vm->programStack = r1

	// sign-extend arguments starting from [procBase+8]
	// R0 is already zero-extended
	emit(LDRSWi(R1, rPROCBASE, 8));
	emit(STP64(R0, R1, SP, 0));
	for ( i = 2; i < 16; i += 2 ) {
		emit(LDRSWi(R0, rPROCBASE, 4+(i+0)*4));
		emit(LDRSWi(R1, rPROCBASE, 4+(i+1)*4));
		emit(STP64(R0, R1, SP, (i/2)*16));
	}

	emit(ADD64i(R0, SP, 0)); // r0 = sp

	//ret = currentVM->systemCall( args );
	emit(LDR64i(R16, rVMBASE, offsetof(vm_t,systemCall))); // r16 = vm->systemCall
	emit(BLR(R16)); // call [r16]( r0 )

	// restore LR, FP
	emit(LDP64(LR, FP, SP, 128)); // { LR, FP } = SP[ 128 ]
	emit(ADD64i(SP, SP, 128+16)); // SP += 128 + 16

	// store return value
	emit(STR32i(R0, rOPSTACK, 4)); // *(opstack+4) = r0

	emit(RET(LR));
}


// R0 - src, R1 - dst, R2 - count, R3 - scratch
static void emitBlockCopyFunc( vm_t *vm )
{
	// adjust R2 if needed
	emit(AND32(R0, R0, rDATAMASK)); // r0 &= dataMask
	emit(AND32(R1, R1, rDATAMASK)); // r1 &= dataMask

	emit(ADD32(R3, R0, R2));        // r3 = r0 + r2
	emit(AND32(R3, R3, rDATAMASK)); // r3 &= dataMask
	emit(SUB32(R2, R3, R0));        // r2 = r3 - r0

	emit(ADD32(R3, R1, R2));        // r3 = r1 + r2
	emit(AND32(R3, R3, rDATAMASK)); // r3 &= dataMask
	emit(SUB32(R2, R3, R1));        // r2 = r3 - r1

	emit(ADD64(R0, R0, rDATABASE)); // r0 += dataBase
	emit(ADD64(R1, R1, rDATABASE)); // r1 += dataBase

	emitAlign( 16 );
	emit(CMP32i(R2, 8));
	emit(Bcond(LT, +20));           // jump to next block if R2 is less than 8
	emit(LDR64iwpost(R3, R0, 8));   // r3 = [r0]; r0 += 8
	emit(STR64iwpost(R3, R1, 8));   // r[1] = r3; r1 += 8
	emit(SUB32i(R2, R2, 8));        // r2 -= 8
	emit(B(-20));

	emitAlign( 16 );
	emit(CMP32i(R2, 4));
	emit(Bcond(LT, +20));           // jump to next block if R2 is less than 4
	emit(LDR32iwpost(R3, R0, 4));   // r3 = [r0]; r0 += 4
	emit(STR32iwpost(R3, R1, 4));   // r[1] = r3; r1 += 4
	emit(SUB32i(R2, R2, 4));        // r2 -= 4
	emit(B(-20));

	emitAlign( 16 );
	emit(CMP32i(R2, 1));
	emit(Bcond(LT, +20));           // jump to next block if R2 is less than 1
	emit(LDRB32iwpost(R3, R0, 1));  // r3 = [r0]; r0 += 1
	emit(STRB32iwpost(R3, R1, 1));  // r[1] = r3; r1 += 1
	emit(SUB32i(R2, R2, 1));        // r2 -= 1
	emit(B(-20));

	emit(RET(LR));
}


static void emitBlockCopy( vm_t *vm, const uint32_t count )
{
	if ( count == 12 ) // most common case - 3d vector copy
	{
		uint32_t rx[3];
		int safe_arg[2];

		rx[0] = load_rx_opstack( R0 ); // src: r0 = *opstack;
		safe_arg[0] = is_safe_arg();
		dec_opstack(); // opstack -= 4

		rx[1] = load_rx_opstack( R1 ); // dst: r1 = *opstack
		safe_arg[1] = is_safe_arg();
		dec_opstack(); // opstack -= 4

		if ( !safe_arg[0] )
			emit(AND32(rx[0], rx[0], rDATAMASK)); // r0 &= dataMask

		if ( !safe_arg[1] )
			emit(AND32(rx[1], rx[1], rDATAMASK)); // r1 &= dataMask

		emit(ADD64(rx[0], rx[0], rDATABASE));
		emit(ADD64(rx[1], rx[1], rDATABASE));

		rx[2] = alloc_rx( R2 | TEMP );

		// load/store double word
		emit(LDR64i(rx[2], rx[0], 0));
		emit(STR64i(rx[2], rx[1], 0));

		// load/store word
		emit(LDR32i(rx[2], rx[0], 8));
		emit(STR32i(rx[2], rx[1], 8));

		unmask_rx( rx[2] );

		unmask_rx( rx[1] );
		unmask_rx( rx[0] );
		return;
	}

	load_rx_opstack( R0 | FORCED ); dec_opstack(); // src: r0 = *opstack; opstack -= 4
	load_rx_opstack( R1 | FORCED ); dec_opstack(); // dst: r1 = *opstack; opstack -= 4
	alloc_rx( R2 | FORCED ); // r2 - count
	alloc_rx( R3 | FORCED ); // r3 - scratch
	mov_rx_imm32(R2, count);
	emitFuncOffset( vm, FUNC_BCPY );
	unmask_rx( R3 );
	unmask_rx( R2 );
	unmask_rx( R1 );
	unmask_rx( R0 );
}


#ifdef CONST_OPTIMIZE
static qboolean ConstOptimize( vm_t *vm, instruction_t *ci, instruction_t *ni )
{
	uint32_t immrs;
	uint32_t rx[3];
	uint32_t sx[2];
	uint32_t x, imm;

	if ( ni->jused ) {
		return qfalse;
	}

	switch ( ni->op ) {

	case OP_ADD:
	case OP_SUB:
		if ( encode_arith_imm( ci->value, &imm ) ) {
			//rx[1] = rx[0] = load_rx_opstack( R0 );	// r0 = *opstack
			load_rx_opstack2( &rx[1], R1, &rx[0], R0 ); // r1 = r0 = *opstack
			switch ( ni->op ) {
				case OP_ADD: emit( ADD32i( rx[1], rx[0], imm ) ); break; // r1 = r0 + imm
				case OP_SUB: emit( SUB32i( rx[1], rx[0], imm ) ); break; // r1 = r0 - imm
			}
			if ( rx[0] != rx[1] ) {
				unmask_rx( rx[0] );
			}
			store_rx_opstack( rx[1] );				// *opstack = r1
			ip += 1; // OP_ADD | OP_SUB
			return qtrue;
		}
		break;

	case OP_BAND:
	case OP_BOR:
	case OP_BXOR:
		x = ci->value;
		if ( encode_logic_imm( x, 32, &immrs ) ) {
			//rx[1] = rx[0] = load_rx_opstack( R0 );	// r1 = r0 = *opstack
			load_rx_opstack2( &rx[1], R1, &rx[0], R0 ); // r1 = r0 = *opstack
			switch ( ni->op ) {
				case OP_BAND: emit( AND32i( rx[1], rx[0], immrs ) ); break; // r1 = r0 & const
				case OP_BOR:  emit( ORR32i( rx[1], rx[0], immrs ) ); break; // r1 = r0 | const
				case OP_BXOR: emit( EOR32i( rx[1], rx[0], immrs ) ); break; // r1 = r0 ^ const
			}
			if ( rx[0] != rx[1] ) {
				unmask_rx( rx[0] );
			}
			store_rx_opstack( rx[1] );	// *opstack = r1
			ip += 1; // OP_BAND | OP_BOR | OP_BXOR
			return qtrue;
		}
		break;

	case OP_LSH:
	case OP_RSHI:
	case OP_RSHU:
		if ( ci->value < 0 || ci->value > 31 )
			break; // undefined behavior
		if ( ci->value ) {
			//rx[1] = rx[0] = load_rx_opstack( R0 );	// r1 = r0 = *opstack
			load_rx_opstack2( &rx[1], R1, &rx[0], R0 ); // r1 = r0 = *opstack
			switch ( ni->op ) {
				case OP_LSH: emit( LSL32i( rx[1], rx[0], ci->value ) );	break; // r1 = r0 << x
				case OP_RSHI: emit( ASR32i( rx[1], rx[0], ci->value ) ); break;	// r1 = r0 >> x
				case OP_RSHU: emit( LSR32i( rx[1], rx[0], ci->value ) ); break;	// r1 = (unsigned)r0 >> x
			}
			if ( rx[0] != rx[1] ) {
				unmask_rx( rx[0] );
			}
			store_rx_opstack( rx[1] );		// *opstack = r1
		}
		ip += 1; // OP_LSH | OP_RSHI | OP_RSHU
		return qtrue;

	case OP_JUMP:
		flush_opstack();
		emit(B(vm->instructionPointers[ ci->value ] - compiledOfs));
		ip += 1; // OP_JUMP
		return qtrue;

	case OP_CALL:
		inc_opstack(); // opstack += 4
		if ( ci->value == ~TRAP_SQRT ) {
			sx[0] = alloc_sx( S0 );
			emit(VLDRi(sx[0], rPROCBASE, 8)); // s0 = [procBase + 8]
			emit(FSQRT(sx[0], sx[0]));        // s0 = sqrtf( s0 )
			store_sx_opstack( sx[0] );        // *opstack = s0
			ip += 1; // OP_CALL
			return qtrue;
		}
		flush_volatile();
		if ( ci->value == ~TRAP_SIN || ci->value == ~TRAP_COS ) {
			sx[0] = S0; mask_sx( sx[0] );
			rx[0] = alloc_rx( R16 );
			emit(VLDRi(sx[0], rPROCBASE, 8)); // s0 = [procBase + 8]
			if ( ci->value == ~TRAP_SIN )
				emit_MOVXi(rx[0], (intptr_t)sinf);
			else
				emit_MOVXi(rx[0], (intptr_t)cosf);
			emit(BLR(rx[0]));
			unmask_rx( rx[0] );
			store_sx_opstack( sx[0] );        // *opstack = s0
			ip += 1; // OP_CALL
			return qtrue;
		}
		if ( ci->value < 0 ) // syscall
		{
			alloc_rx( R0 | FORCED );
			mov_rx_imm32(R0, ~ci->value); // r0 = syscall number
			if ( opstack != 1 ) {
				emit( ADD64i( rOPSTACK, rOPSTACK, ( opstack - 1 ) * sizeof( int32_t ) ) );
				emitFuncOffset( vm, FUNC_SYSF );
				emit( SUB64i( rOPSTACK, rOPSTACK, ( opstack - 1 ) * sizeof( int32_t ) ) );
			} else {
				emitFuncOffset( vm, FUNC_SYSF );
			}
			ip += 1; // OP_CALL;
			store_syscall_opstack( R0 );
			return qtrue;
		}
		if ( opstack != 1 ) {
			emit( ADD64i( rOPSTACK, rOPSTACK, ( opstack - 1 ) * sizeof( int32_t ) ) );
			emit( BL( vm->instructionPointers[ci->value] - compiledOfs ) );
			emit( SUB64i( rOPSTACK, rOPSTACK, ( opstack - 1 ) * sizeof( int32_t ) ) );
		} else {
			emit( BL( vm->instructionPointers[ci->value] - compiledOfs ) );
		}
		ip += 1; // OP_CALL;
		return qtrue;

	case OP_EQ:
	case OP_NE:
	case OP_GEI:
	case OP_GTI:
	case OP_GTU:
	case OP_GEU:
	case OP_LTU:
	case OP_LEU:
	case OP_LEI:
	case OP_LTI: {
		uint32_t comp = get_comp( ni->op );
		rx[0] = load_rx_opstack( R0 | RCONST ); dec_opstack(); // r0 = *opstack; opstack -= 4
		flush_nonvolatile();
		x = ci->value;
		if ( x == 0 && ( ni->op == OP_EQ || ni->op == OP_NE ) ) {
			if ( ni->op == OP_EQ )
				emit(CBZ32(rx[0], vm->instructionPointers[ ni->value ] - compiledOfs));
			else
				emit(CBNZ32(rx[0], vm->instructionPointers[ ni->value ] - compiledOfs));
		} else {
			if ( encode_arith_imm( x, &imm ) ) {
				emit(CMP32i(rx[0], imm));
			} else {
				rx[1] = alloc_rx_const( R2, x );
				emit(CMP32(rx[0], rx[1]));
				unmask_rx( rx[1] );
			}
			emit(Bcond(comp, vm->instructionPointers[ ni->value ] - compiledOfs));
		}
		}
		unmask_rx( rx[0] );
		ip += 1; // OP_cond
		return qtrue;

	case OP_EQF:
	case OP_NEF:
	case OP_LTF:
	case OP_LEF:
	case OP_GTF:
	case OP_GEF:
		if ( ci->value == 0 ) {
			uint32_t comp = get_comp( ni->op );
			sx[0] = load_sx_opstack( S0 | RCONST ); dec_opstack(); // s0 = *opstack; opstack -= 4
			emit( FCMP0( sx[0] ) );
			emit( Bcond( comp, vm->instructionPointers[ni->value] - compiledOfs ) );
			unmask_sx( sx[0] );
			ip += 1; // OP_cond
			return qtrue;
		}
		break;

	default:
		break;
	}

	return qfalse;
}
#endif // CONST_OPTIMIZE


#ifdef DUMP_CODE
static void dump_code( const char *vmname, uint32_t *code, int32_t code_len )
{
	const char *filename = va( "vm-%s.hex", vmname );
	fileHandle_t fh = FS_FOpenFileWrite( filename );
	if ( fh != FS_INVALID_HANDLE ) {
		uint32_t i;
		for ( i = 0; i < code_len; i++ ) {
			FS_Printf( fh, "%02x %02x %02x %02x\n", ( code[i] >> 0 ) & 0xFF, ( code[i] >> 8 ) & 0xFF, ( code[i] >> 16 ) & 0xFF, ( code[i] >> 24 ) & 0xFF );
		}
		FS_FCloseFile( fh );
	}
}
#endif


static void emitFuncBranches( vm_t *vm )
{
	if ( vm_rtChecks->integer & VM_RTCHECK_PSTACK ) {
		emitFuncBranch( vm, FUNC_PSOF );
	}
	if ( vm_rtChecks->integer & VM_RTCHECK_OPSTACK ) {
		emitFuncBranch( vm, FUNC_OSOF );
	}
	if ( vm_rtChecks->integer & VM_RTCHECK_JUMP ) {
		emitFuncBranch( vm, FUNC_OUTJ );
		emitFuncBranch( vm, FUNC_BADJ );
	}
	if ( vm_rtChecks->integer & VM_RTCHECK_DATA && !vm->forceDataMask ) {
		emitFuncBranch( vm, FUNC_BADR );
		emitFuncBranch( vm, FUNC_BADW );
	}
}


static qboolean VM_CompileAArch64( vm_t *vm, vmHeader_t *header )
{
	byte *codeWritable = NULL;
	instruction_t *ci;
	const char *errMsg;
	uint32_t *litBase;
	uint32_t rx[4], imm;
	uint32_t sx[3];
	int proc_base;
	int proc_len;
#ifdef RET_OPTIMIZE
	int proc_end;
#endif
	var_addr_t var;
	reg_t *reg;
	int i;

	inst = (instruction_t*)Z_Malloc( (header->instructionCount + 8 ) * sizeof( instruction_t ) );
	//instructionOffsets = (uint32_t*)Z_Malloc( header->instructionCount * sizeof( uint32_t ) );

	errMsg = VM_LoadInstructions( (byte *) header + header->codeOffset, header->codeLength, header->instructionCount, inst );
	if ( !errMsg ) {
		errMsg = VM_CheckInstructions( inst, vm->instructionCount, (const int32_t *)vm->jumpTableTargets, vm->numJumpTableTargets, vm->exactDataLength );
	}

	if ( errMsg ) {
		VM_FreeBuffers();
		Com_Printf( S_COLOR_WARNING "%s(%s) error: %s\n", __func__, vm->name, errMsg );
		return qfalse;
	}

	if ( !vm->instructionPointers ) {
		vm->instructionPointers = Hunk_Alloc( header->instructionCount * sizeof(vm->instructionPointers[0]), h_high );
	}


	litBase = NULL;
#ifdef USE_LITERAL_POOL
	VM_InitLiterals();
#endif

	memset( funcOffset, 0, sizeof( funcOffset ) );

	code = NULL;
	vm->codeBase = NULL;

	if ( vm->forceDataMask || (vm_rtChecks->integer & VM_RTCHECK_DATA) == 0 ) {
		forceDataMask = qtrue;
	} else {
		forceDataMask = qfalse;
	}

	for ( pass = 0; pass < NUM_PASSES; pass++ ) {
__recompile:

	// translate all instructions
	ip = 0;
	compiledOfs = 0;

	proc_base = -1;
	proc_len = 0;
#ifdef RET_OPTIMIZE
	proc_end = 0;
#endif

	memset( branchOffset, 0, sizeof( branchOffset ) );

	init_opstack();

	emit(SUB64i(SP, SP, 96)); // SP -= 96

	emit(STP64(R20, R21, SP, 0));
	emit(STP64(R22, R23, SP, 16));
	emit(STP64(R24, R25, SP, 32));
	emit(STP64(R26, R27, SP, 48));
	emit(STP64(R28, R29, SP, 64));
	emit(STP64(R19, LR,  SP, 80));

	emit_MOVXi64(rLITBASE, (intptr_t)litBase );
	emit_MOVXi(rVMBASE, (intptr_t)vm );
	emit_MOVXi(rINSPOINTERS, (intptr_t)vm->instructionPointers );
	emit_MOVXi(rDATABASE, (intptr_t)vm->dataBase );

	mov_rx_imm32(rDATAMASK, vm->dataMask);
	mov_rx_imm32(rPSTACKBOTTOM, vm->stackBottom);

	// these are volatile variables
	emit(LDR64i(rOPSTACK, rVMBASE, offsetof(vm_t, opStack)));
	emit(LDR64i(rOPSTACKTOP, rVMBASE, offsetof(vm_t, opStackTop)));
	emit(LDR32i(rPSTACK, rVMBASE, offsetof(vm_t, programStack)));

	emitFuncOffset( vm, FUNC_ENTR );  // call vmMain()

#ifdef DEBUG_VM
	emit(STR32i(rPSTACK, rVMBASE, offsetof(vm_t, programStack))); // vm->programStack = rPSTACK;
#endif

	emit(LDP64(R20, R21, SP, 0));
	emit(LDP64(R22, R23, SP, 16));
	emit(LDP64(R24, R25, SP, 32));
	emit(LDP64(R26, R27, SP, 48));
	emit(LDP64(R28, R29, SP, 64));
	emit(LDP64(R19, LR,  SP, 80));

	emit(ADD64i(SP, SP, 96)); // SP += 96

	emit(RET(LR));
	emitAlign(16);

	// emit initial branch offsets
	emitFuncBranches( vm );

	emitAlign(16);
	funcOffset[ FUNC_ENTR ] = compiledOfs; // offset to vmMain() entry point

	while ( ip < header->instructionCount ) {

		ci = &inst[ ip + 0 ];

#ifdef REGS_OPTIMIZE
		if ( ci->jused )
#endif
		{
			// we can safely perform register optimizations only in case if
			// we are 100% sure that current instruction is not a jump label
			flush_opstack();
		}

		vm->instructionPointers[ ip++ ] = compiledOfs;

		switch ( ci->op )
		{
			case OP_UNDEF:
				emit( BRK( 0 ) );
				break;

			case OP_IGNORE:
				ip += ci->value;
				break;

			case OP_BREAK:
				emit( BRK( 3 ) );
				break;

			case OP_ENTER:
#ifdef FUNC_ALIGN
				emitAlign( FUNC_ALIGN );
#endif
				vm->instructionPointers[ ip - 1 ] = compiledOfs;

				proc_base = ip; // this points on next instruction after OP_ENTER
				// locate endproc
				for ( proc_len = -1, i = ip; i < header->instructionCount; i++ ) {
					if ( inst[ i ].op == OP_PUSH && inst[ i + 1 ].op == OP_LEAVE ) {
						proc_len = i - proc_base;
#ifdef RET_OPTIMIZE
						proc_end = i + 1;
#endif
						break;
					}
				}

				if ( proc_len == 0 ) {
					// empty function, just return
					emit( RET( LR ) );
					proc_base = -1;
					ip += 2; // OP_PUSH + OP_LEAVE
					break;
				}

				// save opStack, LR
				emit(STP64pre(LR, rOPSTACK, SP, -16));

				// save programStack, procBase
				emit(STP64pre(rPSTACK, rPROCBASE, SP, -16));

				if ( encode_arith_imm( ci->value, &imm ) ) {
					emit(SUB32i(rPSTACK, rPSTACK, imm));		// pstack -= arg
				} else {
					rx[0] = alloc_rx_const( R2, ci->value );	// r2 = arg
					emit(SUB32(rPSTACK, rPSTACK, rx[0]));		// pstack -= r2
					unmask_rx( rx[0] );
				}

				emit_CheckProc( vm, ci );
				emit(ADD64(rPROCBASE, rPSTACK, rDATABASE)); // procBase = programStack + dataBase
				break;

			case OP_LEAVE:
				flush_opstack();
				dec_opstack(); // opstack -= 4
#ifdef DEBUG_VM
				if ( opstack != 0 )
					DROP( "opStack corrupted on OP_LEAVE" );
#endif

#ifdef RET_OPTIMIZE
				if ( !ci->endp && proc_base >= 0 ) {
					// jump to last OP_LEAVE instruction in this function
					if ( inst[ ip + 0 ].op == OP_PUSH && inst[ ip + 1 ].op == OP_LEAVE ) {
						// next instruction is proc_end
					} else {
						emit( B( vm->instructionPointers[ proc_end ] - compiledOfs ) );
					}
					break;
				}
#endif
				// restore programStack, procBase
				emit( LDP64post( rPSTACK, rPROCBASE, SP, 16 ) );
				// restore LR, opStack
				emit( LDP64post( LR, rOPSTACK, SP, 16 ) );
				// return to caller
				emit( RET( LR ) );
				break;

			case OP_CALL:
				rx[0] = load_rx_opstack( R0 | FORCED ); // r0 = *opstack
				flush_volatile();
				if ( opstack != 1 ) {
					emit( ADD64i( rOPSTACK, rOPSTACK, ( opstack - 1 ) * sizeof( int32_t ) ) );
					emitFuncOffset( vm, FUNC_CALL );
					emit( SUB64i( rOPSTACK, rOPSTACK, ( opstack - 1 ) * sizeof( int32_t ) ) );
				} else {
					emitFuncOffset( vm, FUNC_CALL );
				}
				unmask_rx( rx[0] );
				break;

			case OP_PUSH:
				inc_opstack(); // opstack += 4
				if ( (ci + 1)->op == OP_LEAVE ) {
					proc_base = -1;
				}
				break;

			case OP_POP:
				dec_opstack_discard(); // opstack -= 4
				break;

			case OP_CONST:
#ifdef CONST_OPTIMIZE
				if ( ConstOptimize( vm, ci + 0, ci + 1 ) )
					break;
#endif
				inc_opstack(); // opstack += 4
				store_item_opstack( ci );
				break;

			case OP_LOCAL:
				inc_opstack(); // opstack += 4
				store_item_opstack( ci );
				break;

			case OP_JUMP:
				rx[0] = load_rx_opstack( R0 | RCONST ); dec_opstack(); // r0 = *opstack; opstack -= 4
				flush_opstack();
				emit_CheckJump( vm, rx[0], proc_base, proc_len ); // check if r0 is within current proc
				rx[1] = alloc_rx( R16 );
				emit(LDR64_8(rx[1], rINSPOINTERS, rx[0]));        // r16 = instructionPointers[ r0 ]
				emit(BR(rx[1]));
				unmask_rx( rx[1] );
				unmask_rx( rx[0] );
				wipe_vars();
				break;

			case OP_EQ:
			case OP_NE:
			case OP_LTI:
			case OP_LEI:
			case OP_GTI:
			case OP_GEI:
			case OP_LTU:
			case OP_LEU:
			case OP_GTU:
			case OP_GEU: {
				uint32_t comp = get_comp( ci->op );
				rx[0] = load_rx_opstack( R0 | RCONST ); dec_opstack(); // r0 = *opstack; opstack -= 4
				rx[1] = load_rx_opstack( R1 | RCONST ); dec_opstack(); // r1 = *opstack; opstack -= 4
				flush_nonvolatile();
				unmask_rx( rx[0] );
				unmask_rx( rx[1] );
				emit(CMP32(rx[1], rx[0]));
				emit(Bcond(comp, vm->instructionPointers[ci->value] - compiledOfs));
				}
				break;

			case OP_EQF:
			case OP_NEF:
			case OP_LTF:
			case OP_LEF:
			case OP_GTF:
			case OP_GEF: {
				uint32_t comp = get_comp( ci->op );
				sx[0] = load_sx_opstack( S0 | RCONST ); dec_opstack(); // s0 = *opstack; opstack -= 4
				sx[1] = load_sx_opstack( S1 | RCONST ); dec_opstack(); // s1 = *opstack; opstack -= 4
				flush_nonvolatile();
				unmask_sx( sx[0] );
				unmask_sx( sx[1] );
				emit(FCMP(sx[1], sx[0]));
				emit(Bcond(comp, vm->instructionPointers[ci->value] - compiledOfs));
				}
				break;

			case OP_LOAD1:
			case OP_LOAD2:
			case OP_LOAD4:
#ifdef FPU_OPTIMIZE
				if ( ci->fpu && ci->op == OP_LOAD4 ) {
					// fpu path
					if ( addr_on_top( &var, rDATABASE, rPROCBASE ) ) {
						// address specified by CONST/LOCAL
						discard_top();
						var.size = 4;
						if ( find_sx_var( &sx[0], &var ) ) {
							// already cached in some register
							mask_sx( sx[0] );
						} else {
							// not cached, perform load
							sx[0] = alloc_sx( S0 );
							if ( can_encode_imm12( var.addr, 2 ) ) {
								emit( VLDRi( sx[0], var.base, var.addr ) ); // s0 = var.base[var.addr]
							} else {
								rx[0] = alloc_rx_const( R2, var.addr );		// r2 = var.addr
								emit( VLDR( sx[0], var.base, rx[0] ) );		// s0 = var.base[r2]
								unmask_rx( rx[0] );
							}
							set_sx_var( sx[0], &var );						// update metadata, this may wipe constant
						}
					} else {
						rx[0] = load_rx_opstack( R0 );				// r0 = *opstack
						emit_CheckReg( vm, rx[0], FUNC_BADR );
						sx[0] = alloc_sx( S0 );
						emit( VLDR( sx[0], rDATABASE, rx[0] ) );	// s0 = dataBase[r0]
						unmask_rx( rx[0] );
					}
					store_sx_opstack( sx[0] );						// *opstack = s0
					break;
				}
#endif // FPU_OPTIMIZE
				if ( addr_on_top( &var, rDATABASE, rPROCBASE ) ) {
					// address specified by CONST/LOCAL
					opcode_t sign_extend;
					int scale;
					switch ( ci->op ) {
						case OP_LOAD1: var.size = 1; scale = 0; sign_extend = OP_SEX8; break;
						case OP_LOAD2: var.size = 2; scale = 1; sign_extend = OP_SEX16; break;
						default:       var.size = 4; scale = 2; sign_extend = OP_UNDEF; break;
					}
					discard_top();
					if ( ( reg = find_rx_var( &rx[0], &var ) ) != NULL ) {
						// already cached in some register
						// do zero extension if needed
						switch ( ci->op ) {
							case OP_LOAD1:
								if ( reg->ext != Z_EXT8 ) {
									if ( search_opstack( TYPE_RX, rx[0] ) ) {
										rx[1] = split_rx( rx[0] );		// alloc rx[1], unmask rx[0]
										emit( UXTB( rx[1], rx[0] ) );	// r1 = (unsigned byte) r0
										set_rx_ext( rx[1], Z_EXT8 );
										rx[0] = rx[1];					// remap rx[0] to the copy
									} else {
										emit( UXTB( rx[0], rx[0] ) );	// r0 = (unsigned byte) r0
										reduce_map_size( reg, 1 );
									}
								}
								break;
							case OP_LOAD2:
								if ( reg->ext != Z_EXT16 ) {
									if ( search_opstack( TYPE_RX, rx[0] ) ) {
										rx[1] = split_rx( rx[0] );		// alloc rx[1], unmask rx[0]
										emit( UXTH( rx[1], rx[0] ) );	// r1 = (unsigned short) r0
										set_rx_ext( rx[1], Z_EXT16 );
										rx[0] = rx[1];					// remap rx[0] to the copy
									} else {
										emit( UXTH( rx[0], rx[0] ) );	// r0 = (unsigned short) r0
										reduce_map_size( reg, 2 );
									}
								}
								break;
							case OP_LOAD4:
								reg->ext = Z_NONE;
								break;
						}
						mask_rx( rx[0] );
					} else {
						// not cached, perform load
						rx[0] = alloc_rx( R0 );					// allocate new register
						if ( can_encode_imm12( var.addr, scale ) ) {
							// short encoding
							if ( ( ci + 1 )->op == sign_extend && sign_extend != OP_UNDEF ) {
								// merge with following sign-extension instruction
								switch ( ci->op ) {
									case OP_LOAD1: emit( LDRSB32i( rx[0], var.base, var.addr ) ); set_rx_ext( rx[0], S_EXT8 ); break;
									case OP_LOAD2: emit( LDRSH32i( rx[0], var.base, var.addr ) ); set_rx_ext( rx[0], S_EXT16 ); break;
								}
								ip += 1; // OP_SEX8/OP_SEX16
							} else {
								switch ( ci->op ) {
									case OP_LOAD1: emit( LDRB32i( rx[0], var.base, var.addr ) ); set_rx_ext( rx[0], Z_EXT8 ); break;
									case OP_LOAD2: emit( LDRH32i( rx[0], var.base, var.addr ) ); set_rx_ext( rx[0], Z_EXT16 ); break;
									case OP_LOAD4: emit( LDR32i( rx[0], var.base, var.addr ) ); set_rx_ext( rx[0], Z_NONE ); break;
								}
							}
						} else {
							// long encoding
							rx[1] = alloc_rx_const( R2, var.addr );
							switch ( ci->op ) {
								case OP_LOAD1: var.size = 1; break; // r0 = (unsigned char)var.base[r2]
								case OP_LOAD2: var.size = 2; break; // r0 = (unsigned short)var.base[r2]
								default:       var.size = 4; break; // r0 = (unsigned word)var.base[r2]
							}
							if ( ( ci + 1 )->op == sign_extend && sign_extend != OP_UNDEF ) {
								// merge with following sign-extension instruction
								switch ( ci->op ) {
									case OP_LOAD1: emit( LDRSB32( rx[0], var.base, rx[1] ) ); set_rx_ext( rx[0], S_EXT8 ); break; // r0 = (signed char)var.base[r2]
									case OP_LOAD2: emit( LDRSH32( rx[0], var.base, rx[1] ) ); set_rx_ext( rx[0], S_EXT16 ); break; // r0 = (signed short)var.base[r2]
								}
							} else {
								switch ( ci->op ) {
									case OP_LOAD1: emit( LDRB32( rx[0], var.base, rx[1] ) ); set_rx_ext( rx[0], Z_EXT8 ); break; // r0 = (unsigned char)var.base[r2]
									case OP_LOAD2: emit( LDRH32( rx[0], var.base, rx[1] ) ); set_rx_ext( rx[0], Z_EXT16 ); break; // r0 = (unsigned short)var.base[r2]
									default:       emit( LDR32( rx[0], var.base, rx[1] ) ); set_rx_ext( rx[0], Z_NONE ); break; // r0 = (unsigned word)var.base[r2]
								}
							}
							unmask_rx( rx[1] );
						} // long encoding
						set_rx_var( rx[0], &var ); // update metadata
					}
				} else {
					// address specified by register
					opcode_t sign_extend;
					switch ( ci->op ) {
						case OP_LOAD1: sign_extend = OP_SEX8; break;
						case OP_LOAD2: sign_extend = OP_SEX16; break;
						default:       sign_extend = OP_UNDEF; break;
					}

					if ( forceDataMask ) {
						rx[0] = rx[1] = load_rx_opstack( R0 );			 // target = address = *opStack
					} else {
						load_rx_opstack2( &rx[0], R1, &rx[1], R0 );		 // target, address(const) = *opStack
					}

					emit_CheckReg( vm, rx[1], FUNC_BADR );
					if ( ( ci + 1 )->op == sign_extend && sign_extend != OP_UNDEF ) {
						// merge with following sign-extension instruction
						switch ( ci->op ) {
							case OP_LOAD1: emit( LDRSB32( rx[0], rDATABASE, rx[1] ) ); set_rx_ext( rx[0], S_EXT8 ); break; // target = (signed char)dataBase[address]
							case OP_LOAD2: emit( LDRSH32( rx[0], rDATABASE, rx[1] ) ); set_rx_ext( rx[0], S_EXT16 ); break; // target = (signed short)dataBase[address]
						}
						ip += 1; // OP_SEX8/OP_SEX16
					} else {
						switch ( ci->op ) {
							case OP_LOAD1: emit( LDRB32( rx[0], rDATABASE, rx[1] ) ); set_rx_ext( rx[0], Z_EXT8 ); break; // target = (unsigned char)dataBase[address]
							case OP_LOAD2: emit( LDRH32( rx[0], rDATABASE, rx[1] ) ); set_rx_ext( rx[0], Z_EXT16 ); break; // target = (unsigned short)dataBase[address]
							default:       emit( LDR32( rx[0], rDATABASE, rx[1] ) ); set_rx_ext( rx[0], Z_NONE ); break; // target = (unsigned word)dataBase[address]
						}
					}
					if ( rx[1] != rx[0] ) {
						unmask_rx( rx[1] );
					}
				}
				store_rx_opstack( rx[0] ); // *opstack = target
				break;

			case OP_STORE1:
			case OP_STORE2:
			case OP_STORE4:
				if ( scalar_on_top() && ci->op == OP_STORE4 ) {
					sx[0] = load_sx_opstack( S0 | RCONST ); dec_opstack(); // s0 = *opstack; opstack -= 4
					if ( addr_on_top( &var, rDATABASE, rPROCBASE ) ) {
						// address specified by CONST/LOCAL
						discard_top(); dec_opstack();
						var.size = 4;
						if ( can_encode_imm12( var.addr, 2 ) ) {
							emit( VSTRi( sx[0], var.base, var.addr ) );		// var.base[var.addr] = s0
						} else {
							rx[0] = alloc_rx_const( R1, var.addr );			// r1 = var.addr
							emit( VSTR( sx[0], var.base, rx[0] ) );			// var.base[r1] = s0
							unmask_rx( rx[0] );
						}
						wipe_var_range( &var );
						set_sx_var( sx[0], &var );							// update metadata
					} else {
						rx[1] = load_rx_opstack( forceDataMask ? R1 : R1 | RCONST );
						dec_opstack();								// r1 = *opStack; opStack -= 4
						emit_CheckReg( vm, rx[1], FUNC_BADW );		// check for (r1 < dataMask) or r1 = r1 & dataMask
						emit( VSTR( sx[0], rDATABASE, rx[1] ) );	// database[r1] = s0
						unmask_rx( rx[1] );
						wipe_vars(); // unknown/dynamic address, wipe all register mappings
					}
					unmask_sx( sx[0] );
				} else {
					// integer path
					rx[0] = load_rx_opstack( R0 | RCONST ); dec_opstack(); // r0 = *opstack; opstack -= 4
					if ( addr_on_top( &var, rDATABASE, rPROCBASE ) ) {
						// address specified by CONST/LOCAL
						int scale;
						discard_top(); dec_opstack();
						switch ( ci->op ) {
							case OP_STORE1:	var.size = 1; scale = 0; break;
							case OP_STORE2:	var.size = 2; scale = 1; break;
							default:        var.size = 4; scale = 2; break;
						}
						if ( can_encode_imm12( var.addr, scale ) ) {
							switch ( ci->op ) {
								case OP_STORE1:	emit( STRB32i( rx[0], var.base, var.addr ) ); break; // (byte*)var.base[var.addr] = r0
								case OP_STORE2:	emit( STRH32i( rx[0], var.base, var.addr ) ); break; // (short*)var.base[var.addr] = r0
								default:        emit( STR32i( rx[0], var.base, var.addr ) ); break;  // (word*)var.base[var.addr] = r0
							}
						} else {
							rx[1] = alloc_rx_const( R1, var.addr );
							switch ( ci->op ) {
								case OP_STORE1: emit( STRB32( rx[0], var.base, rx[1] ) ); break; // (byte*)var.base[r1] = r0
								case OP_STORE2: emit( STRH32( rx[0], var.base, rx[1] ) ); break; // (short*)var.base[r1] = r0
								default:        emit( STR32( rx[0], var.base, rx[1] ) ); break; // (word*)var.base[r1] = r0
							}
							unmask_rx( rx[1] );
						}
						wipe_var_range( &var );
						set_rx_var( rx[0], &var ); // update metadata
					} else {
						// address specified by register
						rx[1] = load_rx_opstack( forceDataMask ? R1 : R1 | RCONST );
						dec_opstack(); // r1 = *opStack; opStack -= 4
						emit_CheckReg( vm, rx[1], FUNC_BADW );
						switch ( ci->op ) {
							case OP_STORE1: emit( STRB32( rx[0], rDATABASE, rx[1] ) ); break; // (byte*)database[r1] = r0
							case OP_STORE2: emit( STRH32( rx[0], rDATABASE, rx[1] ) ); break; // (short*)database[r1] = r0
							default:        emit( STR32( rx[0], rDATABASE, rx[1] ) ); break; // database[r1] = r0
						}
						unmask_rx( rx[1] );
						wipe_vars(); // unknown/dynamic address, wipe all register mappings
					}
					unmask_rx( rx[0] );
				}
				break;

			case OP_ARG:
				var.base = rPROCBASE;
				var.addr = ci->value;
				var.size = 4;
				wipe_var_range( &var );
				if ( scalar_on_top() ) {
					sx[0] = load_sx_opstack( S0 | RCONST ); dec_opstack(); // s0 = *opstack; opstack -=4
					emit( VSTRi( sx[0], var.base, var.addr ) ); // [procBase + v] = s0
					unmask_sx( sx[0] );
				} else {
					rx[0] = load_rx_opstack( R0 | RCONST ); dec_opstack(); // r0 = *opstack; opstack -=4
					emit( STR32i( rx[0], var.base, var.addr ) ); // [procBase + v] = r0
					unmask_rx( rx[0] );
				}
				break;

			case OP_BLOCK_COPY:
				emitBlockCopy( vm, ci->value );
				wipe_vars();
				break;

			case OP_SEX8:
			case OP_SEX16:
			case OP_NEGI:
			case OP_BCOM:
				rx[1] = rx[0] = load_rx_opstack( R0 ); // r0 = *opstack
				//load_rx_opstack2( &rx[1], R1, &rx[0], R0 );
				switch ( ci->op ) {
					case OP_SEX8:  emit(SXTB(rx[1], rx[0]));  break; // r1 = sign extend byte r0
					case OP_SEX16: emit(SXTH(rx[1], rx[0]));  break; // r1 = sign extend short r0
					case OP_NEGI:  emit(NEG32(rx[1], rx[0])); break; // r1 = -r0
					case OP_BCOM:  emit(MVN32(rx[1], rx[0])); break; // r1 = ~r0
				}
				if ( rx[0] != rx[1] ) {
					unmask_rx( rx[0] );
				}
				store_rx_opstack( rx[1] ); // *opstack = r1
				break;

			case OP_ADD:
			case OP_SUB:
			case OP_MULI:
			case OP_MULU:
			case OP_DIVI:
			case OP_DIVU:
			case OP_MODI:
			case OP_MODU:
			case OP_BAND:
			case OP_BOR:
			case OP_BXOR:
			case OP_LSH:
			case OP_RSHI:
			case OP_RSHU:

				load_rx_opstack2( &rx[3], R0, &rx[0], R3 ); dec_opstack();
				//rx[3] = rx[0] = load_rx_opstack( R0 ); dec_opstack(); // source, target = *opstack

				rx[1] = load_rx_opstack( R1 | RCONST ); // opstack-=4; r1 = *opstack
				switch ( ci->op ) {
					case OP_ADD:  emit(ADD32(rx[3], rx[1], rx[0])); break;  // r3 = r1 + r0
					case OP_SUB:  emit(SUB32(rx[3], rx[1], rx[0])); break;  // r3 = r1 - r0
					case OP_MULI:
					case OP_MULU: emit(MUL32(rx[3], rx[1], rx[0])); break;  // r3 = r1 * r0
					case OP_DIVI: emit(SDIV32(rx[3], rx[1], rx[0])); break; // r3 = r1 / r0
					case OP_DIVU: emit(UDIV32(rx[3], rx[1], rx[0])); break; // r3 = (unsigned)r1 / r0
					case OP_BAND: emit(AND32(rx[3], rx[1], rx[0])); break;  // r3 = r1 & r0
					case OP_BOR:  emit(ORR32(rx[3], rx[1], rx[0])); break;  // r3 = r1 | r0
					case OP_BXOR: emit(EOR32(rx[3], rx[1], rx[0])); break;  // r3 = r1 ^ r0
					case OP_LSH:  emit(LSL32(rx[3], rx[1], rx[0])); break;  // r3 = r1 << r0
					case OP_RSHI: emit(ASR32(rx[3], rx[1], rx[0])); break;  // r3 = r1 >> r0
					case OP_RSHU: emit(LSR32(rx[3], rx[1], rx[0])); break;  // r3 = (unsigned)r1 >> r0
					case OP_MODI:
					case OP_MODU:
						rx[2] = alloc_rx( R2 | TEMP );
						if ( ci->op == OP_MODI )
							emit(SDIV32(rx[2], rx[1], rx[0]));		// r2 = r1 / r0
						else
							emit(UDIV32(rx[2], rx[1], rx[0]));		// r2 = (unsigned)r1 / r0
						emit(MSUB32(rx[3], rx[0], rx[2], rx[1]));	// r3 = r1 - r0 * r2
						unmask_rx( rx[2] );
						break;
				}
				if ( rx[0] != rx[3] ) {
					unmask_rx( rx[0] );
				}
				unmask_rx( rx[1] );
				store_rx_opstack( rx[3] ); // *opstack = r3
				break;

			case OP_ADDF:
			case OP_SUBF:
			case OP_MULF:
			case OP_DIVF:
				//sx[2] = sx[0] = load_sx_opstack( S0 ); dec_opstack();	// s0 = *opstack
				load_sx_opstack2( &sx[2], S2, &sx[0], S0 ); dec_opstack();
				sx[1] = load_sx_opstack( S1 | RCONST );			// opstack -= 4; s1 = *opstack
				switch ( ci->op ) {
					case OP_ADDF: emit(FADD(sx[2], sx[1], sx[0])); break; // s2 = s1 + s0
					case OP_SUBF: emit(FSUB(sx[2], sx[1], sx[0])); break; // s2 = s1 - s0
					case OP_MULF: emit(FMUL(sx[2], sx[1], sx[0])); break; // s2 = s1 * s0
					case OP_DIVF: emit(FDIV(sx[2], sx[1], sx[0])); break; // s2 = s1 / s0
				}
				if ( sx[0] != sx[2] ) {
					unmask_sx( sx[0] );
				}
				unmask_sx( sx[1] );
				store_sx_opstack( sx[2] ); // *opstack = s2
				break;

			case OP_NEGF:
				load_sx_opstack2( &sx[1], S1, &sx[0], S0 );
				//sx[1] = sx[0] = load_sx_opstack( S0 );	// s0 = *opstack
				emit(FNEG(sx[1], sx[0]));		// s1 = -s0
				if ( sx[0] != sx[1] ) {
					unmask_sx( sx[0] );
				}
				store_sx_opstack( sx[1] );		// *opstack = s1
				break;

			case OP_CVIF:
				sx[0] = alloc_sx( S0 );
				rx[0] = load_rx_opstack( R0 | RCONST ); // r0 = *opstack
				emit(SCVTF(sx[0], rx[0]));		// s0 = (float)r0
				unmask_rx( rx[0] );
				store_sx_opstack( sx[0] );		// *opstack = s0
				break;

			case OP_CVFI:
				rx[0] = alloc_rx( R0 );
				sx[0] = load_sx_opstack( S0 | RCONST ); // s0 = *opstack
				emit(FCVTZS(rx[0], sx[0]));		// r0 = (int)s0
				unmask_sx( sx[0] );
				store_rx_opstack( rx[0] );		// *opstack = r0;
				break;

		} // switch op
	} // ip

		flush_opstack();

#ifdef FUNC_ALIGN
		emitAlign( FUNC_ALIGN );
#endif
		// it will set multiple offsets
		emitCallFunc( vm );
#ifdef FUNC_ALIGN
		emitAlign( FUNC_ALIGN );
#endif
		funcOffset[ FUNC_BCPY ] = compiledOfs;
		emitBlockCopyFunc( vm );

		if ( vm_rtChecks->integer & VM_RTCHECK_JUMP ) {
			funcOffset[ FUNC_BADJ ] = compiledOfs;
			emit_MOVXi( R16, (intptr_t)BadJump );
			emit( BLR( R16 ) );

			funcOffset[ FUNC_OUTJ ] = compiledOfs;
			emit_MOVXi( R16, (intptr_t)OutJump );
			emit( BLR( R16 ) );
		}

		if ( vm_rtChecks->integer & VM_RTCHECK_OPSTACK ) {
			funcOffset[ FUNC_OSOF ] = compiledOfs;
			emit_MOVXi( R16, (intptr_t)ErrBadOpStack );
			emit( BLR( R16 ) );
		}

		if ( vm_rtChecks->integer & VM_RTCHECK_PSTACK ) {
			funcOffset[ FUNC_PSOF ] = compiledOfs;
			emit_MOVXi( R16, (intptr_t)ErrBadProgramStack );
			emit( BLR( R16 ) );
		}

		if ( vm_rtChecks->integer & VM_RTCHECK_DATA ) {
			funcOffset[ FUNC_BADR ] = compiledOfs;
			emit_MOVXi( R16, (intptr_t)ErrBadDataRead );
			emit( BLR( R16 ) );

			funcOffset[ FUNC_BADW ] = compiledOfs;
			emit_MOVXi( R16, (intptr_t)ErrBadDataWrite );
			emit( BLR( R16 ) );
		}

	} // pass

	if ( vm->codeBase == NULL ) {
#ifdef USE_LITERAL_POOL
		uint32_t allocSize = compiledOfs + numLiterals * sizeof( uint32_t );
#else
		uint32_t allocSize = compiledOfs;
#endif

		vm->codeBase = VM_AllocCode( vm, allocSize, &codeWritable );
		if ( !vm->codeBase ) {
			VM_FreeBuffers();
			Com_Printf( S_COLOR_WARNING "%s(%s): could not get executable memory, using the interpreter\n", __func__, vm->name );
			return qfalse;
		}

		vm->codeLength = allocSize; // code + literals
		vm->codeSize = compiledOfs;
		code = (uint32_t*)codeWritable;
		// where the running code will find the literals
		litBase = (uint32_t*)(vm->codeBase + compiledOfs);
		goto __recompile;
	}

#ifdef USE_LITERAL_POOL
	// append literals to the code
	if ( numLiterals ) {
		// written through the writable view, which may not be where litBase points
		uint32_t *lp = (uint32_t*)( codeWritable + vm->codeSize );
		for ( i = 0; i < numLiterals; i++, lp++ ) {
			*lp = litList[ i ].value;
		}
	}
#endif

#ifdef DUMP_CODE
	dump_code( vm->name, code, compiledOfs / 4 );
#endif

	// offset all the instruction pointers for the new location
	for ( i = 0; i < header->instructionCount; i++ ) {
		if ( !inst[i].jused ) {
			vm->instructionPointers[ i ] = (intptr_t)BadJump;
			continue;
		}
		vm->instructionPointers[ i ] += (intptr_t)vm->codeBase;
	}

	VM_FreeBuffers();

	if ( !VM_SealCode( vm ) ) {
		VM_Destroy_Compiled( vm );
		Com_Printf( S_COLOR_WARNING "%s(%s): could not make the code executable, using the interpreter\n", __func__, vm->name );
		return qfalse;
	}

	vm->destroy = VM_Destroy_Compiled;

	Com_Printf( "VM file %s compiled to %i bytes of code\n", vm->name, vm->codeLength );

	return qtrue;
}


int VM_CallCompiled( vm_t *vm, int *args )
{
	const int	nargs = MAX_VMMAIN_ARGS;
	int32_t		opStack[ MAX_OPSTACK_SIZE ];
	int32_t		stackOnEntry;
	int32_t		*image;
	int		i;

	// we might be called recursively, so this might not be the very top
	stackOnEntry = vm->programStack;

	vm->programStack -= ( MAX_VMMAIN_CALL_ARGS + 2 ) * sizeof( int32_t );

	// set up the stack frame
	image = (int32_t*) ( vm->dataBase + vm->programStack );
	for ( i = 0; i < nargs; i++ ) {
		image[i + 2] = args[i];
	}

	// these only needed for interpreter:
	// image[1] =  0; // return stack
	// image[0] = -1; // will terminate loop on return

#ifdef DEBUG_VM
	opStack[0] = 0xDEADC0DE;
#endif
	opStack[1] = 0;

	vm->opStack = opStack;
	vm->opStackTop = opStack + ARRAY_LEN( opStack ) - 1;

	( (void (*)( void ))vm->codeBase )(); // go into generated code

#ifdef DEBUG_VM
	if ( opStack[0] != 0xDEADC0DE ) {
		Com_Error( ERR_DROP, "%s(%s): opStack corrupted in compiled code", __func__, vm->name );
	}

	if ( vm->programStack != (int32_t)( stackOnEntry - ( MAX_VMMAIN_CALL_ARGS + 2 ) * sizeof( int32_t ) ) ) {
		Com_Error( ERR_DROP, "%s(%s): programStack corrupted in compiled code", __func__, vm->name );
	}
#endif

	vm->programStack = stackOnEntry;

	return opStack[1];
}


/*
=================
VM_Compile

ioquake3 entry point. On failure the VM is left uncompiled and the caller
falls back to the interpreter.
=================
*/
void VM_Compile( vm_t *vm, vmHeader_t *header )
{
	if ( !vm_rtChecks ) {
		// bit mask of run-time checks: 1 program stack, 2 opStack, 4 jumps, 8 data range
		vm_rtChecks = Cvar_Get( "vm_rtChecks", "15", CVAR_INIT | CVAR_PROTECTED );
	}

	// the generated code has these baked in, so they are needed up front
	vm->programStack = vm->dataMask + 1;
	vm->stackBottom = vm->programStack - PROGRAM_STACK_SIZE;
	vm->forceDataMask = qfalse;

	if ( !VM_CompileAArch64( vm, header ) ) {
		vm->compiled = qfalse;
		vm->codeBase = NULL;
		vm->destroy = NULL;
		vm->codeLength = header->codeLength;	// the interpreter wants the bytecode length
	}
}
